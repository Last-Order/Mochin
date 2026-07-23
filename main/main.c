#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"

#define LCD_HOST SPI2_HOST

#define LCD_PIN_DC 45
#define LCD_PIN_CS 21
#define LCD_PIN_SCLK 38
#define LCD_PIN_MOSI 39
#define LCD_PIN_RST 40
#define LCD_PIN_BACKLIGHT 46

#define LCD_WIDTH 240
#define LCD_HEIGHT 240
#define LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)
#define LCD_FRAME_BUFFER_SIZE (LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t))

#define RGB565_NAVY 0x000F
#define RGB565_CYAN 0x07FF
#define RGB565_YELLOW 0xFFE0

static const char *TAG = "lcd_hello_world";

typedef struct {
    char character;
    uint8_t columns[5];
} glyph_t;

// Compact 5x7 font containing the characters used by "HELLO WORLD!".
static const glyph_t FONT[] = {
    {'!', {0x00, 0x00, 0x5F, 0x00, 0x00}},
    {'D', {0x7F, 0x41, 0x41, 0x22, 0x1C}},
    {'E', {0x7F, 0x49, 0x49, 0x49, 0x41}},
    {'H', {0x7F, 0x08, 0x08, 0x08, 0x7F}},
    {'L', {0x7F, 0x40, 0x40, 0x40, 0x40}},
    {'O', {0x3E, 0x41, 0x41, 0x41, 0x3E}},
    {'R', {0x7F, 0x09, 0x19, 0x29, 0x46}},
    {'W', {0x3F, 0x40, 0x38, 0x40, 0x3F}},
};

static void frame_set_pixel(uint8_t *frame, int x, int y, uint16_t color)
{
    if (x < 0 || x >= LCD_WIDTH || y < 0 || y >= LCD_HEIGHT) {
        return;
    }

    // SPI LCD pixel data is big-endian: send the high byte first.
    const size_t offset = ((size_t)y * LCD_WIDTH + x) * sizeof(uint16_t);
    frame[offset] = (uint8_t)(color >> 8);
    frame[offset + 1] = (uint8_t)(color & 0xFF);
}

static void frame_fill_rect(uint8_t *frame, int x, int y, int width, int height,
                            uint16_t color)
{
    if (x < 0) {
        width += x;
        x = 0;
    }
    if (y < 0) {
        height += y;
        y = 0;
    }
    if (x + width > LCD_WIDTH) {
        width = LCD_WIDTH - x;
    }
    if (y + height > LCD_HEIGHT) {
        height = LCD_HEIGHT - y;
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
                frame_fill_rect(frame, x + column * scale, y + row * scale,
                                scale, scale, color);
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

static void draw_hello_world(uint8_t *frame)
{
    static const char message[] = "HELLO WORLD!";
    const int scale = 3;
    const int x = (LCD_WIDTH - frame_text_width(message, scale)) / 2;
    const int y = (LCD_HEIGHT - 7 * scale) / 2;

    frame_fill_rect(frame, 0, 0, LCD_WIDTH, LCD_HEIGHT, RGB565_NAVY);

    frame_fill_rect(frame, 8, 8, LCD_WIDTH - 16, 3, RGB565_CYAN);
    frame_fill_rect(frame, 8, LCD_HEIGHT - 11, LCD_WIDTH - 16, 3, RGB565_CYAN);
    frame_fill_rect(frame, 8, 8, 3, LCD_HEIGHT - 16, RGB565_CYAN);
    frame_fill_rect(frame, LCD_WIDTH - 11, 8, 3, LCD_HEIGHT - 16, RGB565_CYAN);

    frame_draw_text(frame, x, y, message, scale, RGB565_YELLOW);
}

static void lcd_apply_waveshare_settings(esp_lcd_panel_io_handle_t io)
{
    // Panel-specific power and timing values used by Waveshare's reference
    // example. The built-in ST7789 driver handles reset, sleep-out, RGB565,
    // address windows, and pixel transfers.
    static const uint8_t ram_control[] = {0x00, 0xF0};
    static const uint8_t porch_control[] = {0x0C, 0x0C, 0x00, 0x33, 0x33};
    static const uint8_t gate_control[] = {0x35};
    static const uint8_t vcom[] = {0x19};
    static const uint8_t lcm_control[] = {0x2C};
    static const uint8_t vdv_vrh_enable[] = {0x01};
    static const uint8_t vrh_set[] = {0x12};
    static const uint8_t vdv_set[] = {0x20};
    static const uint8_t frame_rate[] = {0x0F};
    static const uint8_t power_control[] = {0xA4, 0xA1};

    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xB0, ram_control,
                                              sizeof(ram_control)));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xB2, porch_control,
                                              sizeof(porch_control)));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xB7, gate_control,
                                              sizeof(gate_control)));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xBB, vcom, sizeof(vcom)));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xC0, lcm_control,
                                              sizeof(lcm_control)));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xC2, vdv_vrh_enable,
                                              sizeof(vdv_vrh_enable)));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xC3, vrh_set,
                                              sizeof(vrh_set)));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xC4, vdv_set,
                                              sizeof(vdv_set)));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xC6, frame_rate,
                                              sizeof(frame_rate)));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xD0, power_control,
                                              sizeof(power_control)));
}

void app_main(void)
{
    ESP_LOGI(TAG, "Initializing Waveshare ESP32-S3-Touch-LCD-1.54");

    const gpio_config_t backlight_config = {
        .pin_bit_mask = 1ULL << LCD_PIN_BACKLIGHT,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&backlight_config));
    ESP_ERROR_CHECK(gpio_set_level(LCD_PIN_BACKLIGHT, 0));

    const spi_bus_config_t bus_config = {
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = LCD_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_FRAME_BUFFER_SIZE,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus_config, SPI_DMA_CH_AUTO));

    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = LCD_PIN_DC,
        .cs_gpio_num = LCD_PIN_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 3,
        .trans_queue_depth = 10,
    };

    esp_lcd_panel_io_handle_t io = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io));

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };

    esp_lcd_panel_handle_t panel = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_config, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, false));

    lcd_apply_waveshare_settings(io);
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));

    uint8_t *frame = spi_bus_dma_memory_alloc(
        LCD_HOST, LCD_FRAME_BUFFER_SIZE, 0);
    if (frame == NULL) {
        ESP_LOGE(TAG, "Unable to allocate %u-byte LCD frame buffer",
                 (unsigned)LCD_FRAME_BUFFER_SIZE);
        return;
    }

    draw_hello_world(frame);
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(
        panel, 0, 0, LCD_WIDTH, LCD_HEIGHT, frame));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
    ESP_ERROR_CHECK(gpio_set_level(LCD_PIN_BACKLIGHT, 1));

    ESP_LOGI(TAG, "Hello World is now displayed");
}
