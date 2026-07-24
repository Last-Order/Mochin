/*
 * ST7789 显示硬件与 LVGL 端口初始化。
 *
 * 本组件只管理背光、SPI、面板和 LVGL 绘制通道，不创建任何业务页面。
 * 页面组件可以在 lcd_display_init() 之后创建对象；首帧准备好后再由应用
 * 调用 lcd_display_enable() 点亮屏幕。
 */

#include "display/lcd_display.h"

#include <stdbool.h>
#include <stdint.h>

#include "board/board_config.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

/*
 * LVGL 使用部分刷新。24 行双 DMA 缓冲总计 23040 字节，允许一个缓冲发送
 * 时在另一个缓冲继续绘制；esp_lvgl_port 负责保证 DMA 完成前不会复用它。
 */
#define LVGL_DRAW_BUFFER_LINES (BOARD_LCD_HEIGHT / 10)
#define LVGL_DRAW_BUFFER_PIXELS \
    (BOARD_LCD_WIDTH * LVGL_DRAW_BUFFER_LINES)
#define LVGL_DRAW_BUFFER_BYTES \
    (LVGL_DRAW_BUFFER_PIXELS * sizeof(uint16_t))

static const char *TAG = "lcd_display";

typedef struct {
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t panel;
    lv_display_t *display;
    bool initialized;
    bool enabled;
} lcd_display_context_t;

static lcd_display_context_t s_display;

/**
 * @brief 写入本板 ST7789 已通过实物验证的附加寄存器参数。
 *
 * ESP-IDF 驱动负责通用初始化；这里只保留 Waveshare 面板所需的供电、
 * porch 和帧率设置。调用时面板和背光都仍处于关闭状态。
 */
static esp_err_t lcd_display_apply_waveshare_settings(
    esp_lcd_panel_io_handle_t io)
{
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

esp_err_t lcd_display_init(void)
{
    ESP_RETURN_ON_FALSE(!s_display.initialized, ESP_ERR_INVALID_STATE, TAG,
                        "Display is already initialized");

    /*
     * 初始化全程关闭背光。LCD 复位和 LVGL 建立刷新缓冲期间都可能没有有效
     * 图像，延后点亮可避免用户看到随机像素或中间状态。
     */
    const gpio_config_t backlight_config = {
        .pin_bit_mask = 1ULL << BOARD_LCD_PIN_BACKLIGHT,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&backlight_config), TAG,
                        "Failed to configure LCD backlight");
    ESP_RETURN_ON_ERROR(
        gpio_set_level(BOARD_LCD_PIN_BACKLIGHT, 0), TAG,
        "Failed to turn LCD backlight off");

    const spi_bus_config_t bus_config = {
        .mosi_io_num = BOARD_LCD_PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = BOARD_LCD_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LVGL_DRAW_BUFFER_BYTES,
    };
    ESP_RETURN_ON_ERROR(
        spi_bus_initialize(BOARD_LCD_HOST, &bus_config, SPI_DMA_CH_AUTO),
        TAG, "Failed to initialize LCD SPI bus");

    /*
     * DMA 完成回调由 lvgl_port_add_disp() 注册。trans_queue_depth 允许刷新
     * 管线排队，但正在发送的双缓冲生命周期仍由适配层统一管理。
     */
    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = BOARD_LCD_PIN_DC,
        .cs_gpio_num = BOARD_LCD_PIN_CS,
        .pclk_hz = BOARD_LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 3,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi(
            (esp_lcd_spi_bus_handle_t)BOARD_LCD_HOST,
            &io_config, &s_display.io),
        TAG, "Failed to create LCD panel IO");

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BOARD_LCD_PIN_RESET,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_st7789(
            s_display.io, &panel_config, &s_display.panel),
        TAG, "Failed to create ST7789 panel");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_reset(s_display.panel), TAG,
        "Failed to reset LCD panel");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_init(s_display.panel), TAG,
        "Failed to initialize LCD panel");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_disp_on_off(s_display.panel, false), TAG,
        "Failed to disable LCD panel");
    ESP_RETURN_ON_ERROR(
        lcd_display_apply_waveshare_settings(s_display.io), TAG,
        "Failed to apply Waveshare LCD settings");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_invert_color(s_display.panel, true), TAG,
        "Failed to enable LCD color inversion");

    const lvgl_port_cfg_t lvgl_config = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(
        lvgl_port_init(&lvgl_config), TAG,
        "Failed to initialize LVGL port");

    /*
     * ST7789 的 SPI 字节序与 ESP32-S3 内存中的 RGB565 字节序相反，刷新时
     * 由 esp_lvgl_port 统一交换。图片资源仍使用 LVGL 原生 RGB565 排列。
     */
    const lvgl_port_display_cfg_t display_config = {
        .io_handle = s_display.io,
        .panel_handle = s_display.panel,
        .buffer_size = LVGL_DRAW_BUFFER_PIXELS,
        .double_buffer = true,
        .hres = BOARD_LCD_WIDTH,
        .vres = BOARD_LCD_HEIGHT,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = false,
            .swap_bytes = true,
            .full_refresh = false,
            .direct_mode = false,
        },
    };
    s_display.display = lvgl_port_add_disp(&display_config);
    ESP_RETURN_ON_FALSE(
        s_display.display != NULL, ESP_ERR_NO_MEM, TAG,
        "Failed to add LCD to LVGL");

    s_display.initialized = true;
    ESP_LOGI(TAG, "LCD and LVGL initialized; backlight remains off");
    return ESP_OK;
}

esp_err_t lcd_display_enable(void)
{
    ESP_RETURN_ON_FALSE(s_display.initialized, ESP_ERR_INVALID_STATE, TAG,
                        "Display is not initialized");
    ESP_RETURN_ON_FALSE(!s_display.enabled, ESP_ERR_INVALID_STATE, TAG,
                        "Display is already enabled");
    ESP_RETURN_ON_FALSE(lvgl_port_lock(0), ESP_FAIL, TAG,
                        "Failed to lock LVGL for first refresh");

    /*
     * 页面组件已经在背光关闭时建立对象树。这里强制完成首帧刷新，再点亮
     * 面板和背光，用户第一眼看到的就是完整待机页。
     */
    lv_refr_now(s_display.display);
    lvgl_port_unlock();

    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_disp_on_off(s_display.panel, true), TAG,
        "Failed to enable LCD panel");
    ESP_RETURN_ON_ERROR(
        gpio_set_level(BOARD_LCD_PIN_BACKLIGHT, 1), TAG,
        "Failed to turn LCD backlight on");

    s_display.enabled = true;
    ESP_LOGI(TAG, "LCD panel and backlight enabled");
    return ESP_OK;
}

esp_err_t lcd_display_disable(void)
{
    ESP_RETURN_ON_FALSE(s_display.initialized, ESP_ERR_INVALID_STATE, TAG,
                        "Display is not initialized");
    ESP_RETURN_ON_FALSE(s_display.enabled, ESP_ERR_INVALID_STATE, TAG,
                        "Display is not enabled");

    /*
     * 先关闭背光，用户立即得到关机反馈。取得 LVGL 锁可避免面板关闭命令
     * 与 LVGL 刷新任务同时访问 SPI 面板；面板关闭后仍保留对象和缓冲，
     * 因为电池供电马上会由电源组件切断。
     */
    ESP_RETURN_ON_ERROR(
        gpio_set_level(BOARD_LCD_PIN_BACKLIGHT, 0), TAG,
        "Failed to turn LCD backlight off");
    ESP_RETURN_ON_FALSE(lvgl_port_lock(0), ESP_FAIL, TAG,
                        "Failed to lock LVGL while disabling display");

    const esp_err_t panel_result =
        esp_lcd_panel_disp_on_off(s_display.panel, false);
    lvgl_port_unlock();
    ESP_RETURN_ON_ERROR(
        panel_result, TAG, "Failed to disable LCD panel");

    s_display.enabled = false;
    ESP_LOGI(TAG, "LCD panel and backlight disabled");
    return ESP_OK;
}
