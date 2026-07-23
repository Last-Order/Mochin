#include "app_display.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "board_config.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define LCD_FRAME_BUFFER_SIZE \
    (BOARD_LCD_WIDTH * BOARD_LCD_HEIGHT * sizeof(uint16_t))

#define RGB565_NAVY 0x000F
#define RGB565_CYAN 0x07FF
#define RGB565_YELLOW 0xFFE0
#define RGB565_GREEN 0x07E0
#define RGB565_MAGENTA 0xF81F

static const char *TAG = "app_display";

typedef struct {
    char character;
    uint8_t columns[5];
} glyph_t;

typedef struct {
    esp_lcd_panel_handle_t panel;
    uint8_t *frame;
    SemaphoreHandle_t transfer_done;
    SemaphoreHandle_t lock;
    bool initialized;
} display_context_t;

static display_context_t s_display;

// Compact 5x7 font containing the characters used by the button UI.
static const glyph_t FONT[] = {
    {'!', {0x00, 0x00, 0x5F, 0x00, 0x00}},
    {'A', {0x7E, 0x09, 0x09, 0x09, 0x7E}},
    {'B', {0x7F, 0x49, 0x49, 0x49, 0x36}},
    {'D', {0x7F, 0x41, 0x41, 0x22, 0x1C}},
    {'E', {0x7F, 0x49, 0x49, 0x49, 0x41}},
    {'H', {0x7F, 0x08, 0x08, 0x08, 0x7F}},
    {'L', {0x7F, 0x40, 0x40, 0x40, 0x40}},
    {'N', {0x7F, 0x02, 0x04, 0x08, 0x7F}},
    {'O', {0x3E, 0x41, 0x41, 0x41, 0x3E}},
    {'P', {0x7F, 0x09, 0x09, 0x09, 0x06}},
    {'R', {0x7F, 0x09, 0x19, 0x29, 0x46}},
    {'S', {0x46, 0x49, 0x49, 0x49, 0x31}},
    {'T', {0x01, 0x01, 0x7F, 0x01, 0x01}},
    {'U', {0x3F, 0x40, 0x40, 0x40, 0x3F}},
    {'W', {0x3F, 0x40, 0x38, 0x40, 0x3F}},
};

static void frame_set_pixel(uint8_t *frame, int x, int y, uint16_t color)
{
    if (x < 0 || x >= BOARD_LCD_WIDTH ||
        y < 0 || y >= BOARD_LCD_HEIGHT) {
        return;
    }

    // SPI LCD pixel data is big-endian: send the high byte first.
    const size_t offset =
        ((size_t)y * BOARD_LCD_WIDTH + x) * sizeof(uint16_t);
    frame[offset] = (uint8_t)(color >> 8);
    frame[offset + 1] = (uint8_t)(color & 0xFF);
}

static void frame_fill_rect(uint8_t *frame, int x, int y, int width,
                            int height, uint16_t color)
{
    if (x < 0) {
        width += x;
        x = 0;
    }
    if (y < 0) {
        height += y;
        y = 0;
    }
    if (x + width > BOARD_LCD_WIDTH) {
        width = BOARD_LCD_WIDTH - x;
    }
    if (y + height > BOARD_LCD_HEIGHT) {
        height = BOARD_LCD_HEIGHT - y;
    }
    if (width <= 0 || height <= 0) {
        return;
    }

    for (int row = y; row < y + height; ++row) {
        for (int column = x; column < x + width; ++column) {
            frame_set_pixel(frame, column, row, color);
        }
    }
}

static const uint8_t *font_find_glyph(char character)
{
    for (size_t i = 0; i < sizeof(FONT) / sizeof(FONT[0]); ++i) {
        if (FONT[i].character == character) {
            return FONT[i].columns;
        }
    }
    return NULL;
}

static void frame_draw_char(uint8_t *frame, int x, int y, char character,
                            int scale, uint16_t color)
{
    const uint8_t *columns = font_find_glyph(character);
    if (columns == NULL) {
        return; // Spaces and unsupported characters remain blank.
    }

    for (int column = 0; column < 5; ++column) {
        for (int row = 0; row < 7; ++row) {
            if ((columns[column] & (1U << row)) != 0) {
                frame_fill_rect(frame, x + column * scale,
                                y + row * scale, scale, scale, color);
            }
        }
    }
}

static int frame_text_width(const char *text, int scale)
{
    const size_t length = strlen(text);
    return length == 0 ? 0 : (int)(length * 6 - 1) * scale;
}

static void frame_draw_text(uint8_t *frame, int x, int y, const char *text,
                            int scale, uint16_t color)
{
    while (*text != '\0') {
        frame_draw_char(frame, x, y, *text, scale, color);
        x += 6 * scale;
        ++text;
    }
}

static void frame_draw_screen(uint8_t *frame, const char *title,
                              const char *message, int message_scale,
                              uint16_t accent_color)
{
    const int title_scale = 2;
    const int title_x =
        (BOARD_LCD_WIDTH - frame_text_width(title, title_scale)) / 2;
    const int message_x =
        (BOARD_LCD_WIDTH - frame_text_width(message, message_scale)) / 2;
    const int message_y =
        (BOARD_LCD_HEIGHT - 7 * message_scale) / 2 + 12;

    frame_fill_rect(frame, 0, 0, BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT,
                    RGB565_NAVY);

    frame_fill_rect(frame, 8, 8, BOARD_LCD_WIDTH - 16, 3, accent_color);
    frame_fill_rect(frame, 8, BOARD_LCD_HEIGHT - 11,
                    BOARD_LCD_WIDTH - 16, 3, accent_color);
    frame_fill_rect(frame, 8, 8, 3, BOARD_LCD_HEIGHT - 16, accent_color);
    frame_fill_rect(frame, BOARD_LCD_WIDTH - 11, 8, 3,
                    BOARD_LCD_HEIGHT - 16, accent_color);

    frame_draw_text(frame, title_x, 45, title, title_scale, RGB565_CYAN);
    frame_draw_text(frame, message_x, message_y, message, message_scale,
                    accent_color);
}

static bool lcd_color_transfer_done(
    esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_io_event_data_t *event_data,
    void *user_ctx)
{
    (void)panel_io;
    (void)event_data;

    BaseType_t high_priority_task_woken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)user_ctx,
                          &high_priority_task_woken);
    return high_priority_task_woken == pdTRUE;
}

static esp_err_t display_apply_waveshare_settings(
    esp_lcd_panel_io_handle_t io)
{
    // Panel-specific power and timing values from Waveshare's reference
    // example. The ST7789 driver handles reset, sleep-out, RGB565, address
    // windows, and pixel transfers.
    static const uint8_t ram_control[] = {0x00, 0xF0};
    static const uint8_t porch_control[] =
        {0x0C, 0x0C, 0x00, 0x33, 0x33};
    static const uint8_t gate_control[] = {0x35};
    static const uint8_t vcom[] = {0x19};
    static const uint8_t lcm_control[] = {0x2C};
    static const uint8_t vdv_vrh_enable[] = {0x01};
    static const uint8_t vrh_set[] = {0x12};
    static const uint8_t vdv_set[] = {0x20};
    static const uint8_t frame_rate[] = {0x0F};
    static const uint8_t power_control[] = {0xA4, 0xA1};

    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_tx_param(io, 0xB0, ram_control,
                                  sizeof(ram_control)),
        TAG, "Failed to set RAM control");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_tx_param(io, 0xB2, porch_control,
                                  sizeof(porch_control)),
        TAG, "Failed to set porch control");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_tx_param(io, 0xB7, gate_control,
                                  sizeof(gate_control)),
        TAG, "Failed to set gate control");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_tx_param(io, 0xBB, vcom, sizeof(vcom)),
        TAG, "Failed to set VCOM");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_tx_param(io, 0xC0, lcm_control,
                                  sizeof(lcm_control)),
        TAG, "Failed to set LCM control");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_tx_param(io, 0xC2, vdv_vrh_enable,
                                  sizeof(vdv_vrh_enable)),
        TAG, "Failed to enable VDV/VRH");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_tx_param(io, 0xC3, vrh_set, sizeof(vrh_set)),
        TAG, "Failed to set VRH");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_tx_param(io, 0xC4, vdv_set, sizeof(vdv_set)),
        TAG, "Failed to set VDV");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_tx_param(io, 0xC6, frame_rate,
                                  sizeof(frame_rate)),
        TAG, "Failed to set frame rate");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_tx_param(io, 0xD0, power_control,
                                  sizeof(power_control)),
        TAG, "Failed to set power control");

    return ESP_OK;
}

static uint16_t display_get_accent_color(app_display_accent_t accent)
{
    switch (accent) {
    case APP_DISPLAY_ACCENT_MAGENTA:
        return RGB565_MAGENTA;
    case APP_DISPLAY_ACCENT_GREEN:
        return RGB565_GREEN;
    case APP_DISPLAY_ACCENT_YELLOW:
    default:
        return RGB565_YELLOW;
    }
}

static esp_err_t display_render_and_show(const char *title,
                                         const char *message,
                                         int message_scale,
                                         uint16_t accent_color)
{
    ESP_RETURN_ON_FALSE(s_display.initialized, ESP_ERR_INVALID_STATE, TAG,
                        "Display is not initialized");
    ESP_RETURN_ON_FALSE(title != NULL && message != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "Text must not be NULL");

    if (xSemaphoreTake(s_display.lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    frame_draw_screen(s_display.frame, title, message, message_scale,
                      accent_color);

    esp_err_t ret = esp_lcd_panel_draw_bitmap(
        s_display.panel, 0, 0, BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT,
        s_display.frame);
    if (ret == ESP_OK &&
        xSemaphoreTake(s_display.transfer_done, portMAX_DELAY) != pdTRUE) {
        ret = ESP_FAIL;
    }

    xSemaphoreGive(s_display.lock);
    return ret;
}

esp_err_t app_display_init(void)
{
    ESP_RETURN_ON_FALSE(!s_display.initialized, ESP_ERR_INVALID_STATE, TAG,
                        "Display is already initialized");

    s_display.transfer_done = xSemaphoreCreateBinary();
    s_display.lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(
        s_display.transfer_done != NULL && s_display.lock != NULL,
        ESP_ERR_NO_MEM, TAG, "Unable to create display semaphores");

    const gpio_config_t backlight_config = {
        .pin_bit_mask = 1ULL << BOARD_LCD_PIN_BACKLIGHT,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&backlight_config), TAG,
                        "Failed to configure LCD backlight");
    ESP_RETURN_ON_ERROR(gpio_set_level(BOARD_LCD_PIN_BACKLIGHT, 0), TAG,
                        "Failed to turn LCD backlight off");

    const spi_bus_config_t bus_config = {
        .mosi_io_num = BOARD_LCD_PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = BOARD_LCD_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_FRAME_BUFFER_SIZE,
    };
    ESP_RETURN_ON_ERROR(
        spi_bus_initialize(BOARD_LCD_HOST, &bus_config, SPI_DMA_CH_AUTO),
        TAG, "Failed to initialize LCD SPI bus");

    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = BOARD_LCD_PIN_DC,
        .cs_gpio_num = BOARD_LCD_PIN_CS,
        .pclk_hz = BOARD_LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 3,
        .trans_queue_depth = 10,
        .on_color_trans_done = lcd_color_transfer_done,
        .user_ctx = s_display.transfer_done,
    };

    esp_lcd_panel_io_handle_t io = NULL;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi(
            (esp_lcd_spi_bus_handle_t)BOARD_LCD_HOST, &io_config, &io),
        TAG, "Failed to create LCD panel IO");

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BOARD_LCD_PIN_RESET,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_st7789(io, &panel_config, &s_display.panel),
        TAG, "Failed to create ST7789 panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_display.panel), TAG,
                        "Failed to reset LCD panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_display.panel), TAG,
                        "Failed to initialize LCD panel");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_disp_on_off(s_display.panel, false), TAG,
        "Failed to disable LCD panel");

    ESP_RETURN_ON_ERROR(display_apply_waveshare_settings(io), TAG,
                        "Failed to apply Waveshare LCD settings");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_invert_color(s_display.panel, true), TAG,
        "Failed to enable LCD color inversion");

    s_display.frame = spi_bus_dma_memory_alloc(
        BOARD_LCD_HOST, LCD_FRAME_BUFFER_SIZE, 0);
    ESP_RETURN_ON_FALSE(s_display.frame != NULL, ESP_ERR_NO_MEM, TAG,
                        "Unable to allocate LCD frame buffer");

    s_display.initialized = true;
    ESP_RETURN_ON_ERROR(app_display_show_prompt(), TAG,
                        "Failed to draw initial screen");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_disp_on_off(s_display.panel, true), TAG,
        "Failed to enable LCD panel");
    ESP_RETURN_ON_ERROR(gpio_set_level(BOARD_LCD_PIN_BACKLIGHT, 1), TAG,
                        "Failed to turn LCD backlight on");

    ESP_LOGI(TAG, "LCD is ready");
    return ESP_OK;
}

esp_err_t app_display_show_prompt(void)
{
    return display_render_and_show("BUTTON TEST", "PRESS A BUTTON", 2,
                                   RGB565_YELLOW);
}

esp_err_t app_display_show_button(const char *button_name,
                                  app_display_accent_t accent)
{
    ESP_RETURN_ON_FALSE(accent >= 0 && accent < APP_DISPLAY_ACCENT_COUNT,
                        ESP_ERR_INVALID_ARG, TAG,
                        "Invalid display accent");

    return display_render_and_show(
        "BUTTON", button_name, 6, display_get_accent_color(accent));
}
