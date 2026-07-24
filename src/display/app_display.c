/*
 * 显示组件的内部实现。
 *
 * 推荐按以下顺序阅读：
 * 1. app_display_init()：初始化背光、SPI、ST7789 和 LVGL；
 * 2. display_create_ui()：用 LVGL 对象搭出原有的边框、标题和主消息；
 * 3. display_update_ui()：在按键回调所在任务中安全更新 LVGL 对象。
 *
 * 本文件仍然负责已经在实物上验证的板级显示初始化，LVGL 不会替代
 * esp_lcd，也不会修改 Waveshare 面板所需的寄存器设置。迁移后的边界是：
 *
 * - esp_lcd：负责 ST7789、SPI 命令和 DMA 像素传输；
 * - esp_lvgl_port：负责 LVGL tick、任务、刷新回调和 DMA 缓冲生命周期；
 * - LVGL：负责文字、边框、颜色、布局和脏区域重绘；
 * - app_display 公共 API：继续向应用层提供与迁移前相同的业务操作。
 */

#include "display/app_display.h"

#include <stdbool.h>
#include <stddef.h>
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
 * LVGL 使用“部分刷新”时，绘制缓冲不必覆盖整屏。官方建议缓冲至少能容纳
 * 约 1/10 屏幕，因此这里使用 24 行，并启用双缓冲：
 *
 * 240 × 24 × 2 字节 × 2 个缓冲 = 23040 字节。
 *
 * 两个缓冲都放在 DMA 可访问的内部内存中。LVGL 可以在一个缓冲传输时绘制
 * 另一个缓冲，esp_lvgl_port 会在 DMA 完成前阻止 LVGL 复用正在发送的数据。
 */
#define LVGL_DRAW_BUFFER_LINES (BOARD_LCD_HEIGHT / 10)
#define LVGL_DRAW_BUFFER_PIXELS \
    (BOARD_LCD_WIDTH * LVGL_DRAW_BUFFER_LINES)
#define LVGL_DRAW_BUFFER_BYTES \
    (LVGL_DRAW_BUFFER_PIXELS * sizeof(uint16_t))

// 下列 RGB888 数值量化到 RGB565 后，与迁移前的界面颜色一致。
#define UI_COLOR_NAVY 0x00007B
#define UI_COLOR_CYAN 0x00FFFF
#define UI_COLOR_YELLOW 0xFFFF00
#define UI_COLOR_GREEN 0x00FF00
#define UI_COLOR_MAGENTA 0xFF00FF

// 原界面的边框距离四边 8 像素、宽 3 像素。
#define UI_FRAME_INSET 8
#define UI_FRAME_WIDTH 3

static const char *TAG = "app_display";

/*
 * 当前硬件只有一块 LCD，因此显示组件保持单例设计。LVGL 对象只能在取得
 * esp_lvgl_port 的递归互斥锁后访问，避免按键任务与 LVGL 刷新任务并发操作。
 */
typedef struct {
    esp_lcd_panel_io_handle_t io; // SPI 面板 IO，交给 esp_lvgl_port 注册完成回调
    esp_lcd_panel_handle_t panel; // ST7789 面板句柄
    lv_display_t *display;        // LVGL 显示设备
    lv_obj_t *frame;              // 原界面的彩色矩形边框
    lv_obj_t *title;              // 顶部标题
    lv_obj_t *message;            // 屏幕中央的提示或按键名称
    bool initialized;             // 公共 API 是否已经可以使用
} display_context_t;

static display_context_t s_display;

/**
 * @brief 写入 Waveshare 这块 LCD 需要的附加寄存器设置。
 *
 * ESP-IDF 的 ST7789 驱动负责通用的复位、像素格式、地址窗口和传输。本函数
 * 只保留已经在本板实物上验证过的供电、porch 与帧率参数。引入 LVGL 不应
 * 改变这一层硬件配置。
 */
static esp_err_t display_apply_waveshare_settings(
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

/**
 * @brief 将业务层强调色转换成 LVGL 颜色。
 *
 * 业务层仍然不接触 RGB565 或 LVGL 类型，因此以后调整主题不会影响 app。
 */
static lv_color_t display_get_accent_color(app_display_accent_t accent)
{
    switch (accent) {
    case APP_DISPLAY_ACCENT_MAGENTA:
        return lv_color_hex(UI_COLOR_MAGENTA);
    case APP_DISPLAY_ACCENT_GREEN:
        return lv_color_hex(UI_COLOR_GREEN);
    case APP_DISPLAY_ACCENT_YELLOW:
    default:
        return lv_color_hex(UI_COLOR_YELLOW);
    }
}

/**
 * @brief 在已经取得 LVGL 锁的情况下更新界面内容。
 *
 * @param title        顶部标题。
 * @param message      中央文字。
 * @param message_font 中央文字使用的 LVGL 字体。
 * @param accent       边框和中央文字的强调色。
 *
 * 此函数不自行加锁，供初始化和运行期更新共同复用。所有对象先完成属性更新，
 * 最后重新对齐；这样字体高度改变时，中央文字仍会保持在原来的视觉中心。
 */
static void display_set_ui_content_locked(const char *title,
                                          const char *message,
                                          const lv_font_t *message_font,
                                          lv_color_t accent)
{
    lv_obj_set_style_border_color(s_display.frame, accent, LV_PART_MAIN);

    lv_label_set_text(s_display.title, title);
    lv_obj_set_style_text_color(
        s_display.title, lv_color_hex(UI_COLOR_CYAN), LV_PART_MAIN);
    lv_obj_set_style_text_font(
        s_display.title, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(s_display.title, LV_ALIGN_TOP_MID, 0, 45);

    lv_label_set_text(s_display.message, message);
    lv_obj_set_style_text_color(s_display.message, accent, LV_PART_MAIN);
    lv_obj_set_style_text_font(
        s_display.message, message_font, LV_PART_MAIN);
    lv_obj_align(s_display.message, LV_ALIGN_CENTER, 0, 12);
}

/**
 * @brief 创建与迁移前外观和信息层级相同的 LVGL 对象树。
 *
 * UI 只创建一次；后续按键事件只修改已有对象，避免频繁分配和销毁对象造成
 * 堆碎片。首次刷新发生在面板输出和背光开启之前，用户不会看到初始化噪点。
 */
static esp_err_t display_create_ui(void)
{
    ESP_RETURN_ON_FALSE(lvgl_port_lock(0), ESP_FAIL, TAG,
                        "Failed to lock LVGL while creating UI");

    lv_obj_t *screen = lv_display_get_screen_active(s_display.display);
    lv_obj_set_style_bg_color(
        screen, lv_color_hex(UI_COLOR_NAVY), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    s_display.frame = lv_obj_create(screen);
    s_display.title = lv_label_create(screen);
    s_display.message = lv_label_create(screen);

    if (s_display.frame == NULL ||
        s_display.title == NULL ||
        s_display.message == NULL) {
        lvgl_port_unlock();
        return ESP_ERR_NO_MEM;
    }

    /*
     * 基础 lv_obj 默认带背景、圆角、内边距和可滚动行为。这里全部显式关闭，
     * 让它只承担迁移前“四条直线组成的边框”这一项视觉职责。
     */
    lv_obj_set_size(
        s_display.frame,
        BOARD_LCD_WIDTH - 2 * UI_FRAME_INSET,
        BOARD_LCD_HEIGHT - 2 * UI_FRAME_INSET);
    lv_obj_align(s_display.frame, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(
        s_display.frame, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(
        s_display.frame, UI_FRAME_WIDTH, LV_PART_MAIN);
    lv_obj_set_style_radius(s_display.frame, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_display.frame, 0, LV_PART_MAIN);
    lv_obj_remove_flag(s_display.frame, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_display.frame, LV_OBJ_FLAG_CLICKABLE);

    display_set_ui_content_locked(
        "BUTTON TEST", "PRESS A BUTTON",
        &lv_font_montserrat_20, lv_color_hex(UI_COLOR_YELLOW));

    /*
     * 正常运行时 esp_lvgl_port 的任务会自动处理脏区域。初始化阶段主动刷新，
     * 是为了在点亮背光前先把第一帧完整写入 LCD GRAM。
     */
    lv_refr_now(s_display.display);
    lvgl_port_unlock();
    return ESP_OK;
}

/**
 * @brief 以线程安全方式更新并立即刷新当前界面。
 *
 * 按键监听运行在独立 FreeRTOS 任务中，而 LVGL 也有自己的刷新任务。所有
 * LVGL API 都必须位于 lvgl_port_lock()/unlock() 之间。这里保留“函数返回
 * 时画面已经完成本次刷新”的同步语义，与迁移前等待 DMA 完成的行为一致。
 */
static esp_err_t display_update_ui(const char *title,
                                   const char *message,
                                   const lv_font_t *message_font,
                                   lv_color_t accent)
{
    ESP_RETURN_ON_FALSE(s_display.initialized, ESP_ERR_INVALID_STATE, TAG,
                        "Display is not initialized");
    ESP_RETURN_ON_FALSE(
        title != NULL && message != NULL && message_font != NULL,
        ESP_ERR_INVALID_ARG, TAG, "UI text and font must not be NULL");
    ESP_RETURN_ON_FALSE(lvgl_port_lock(0), ESP_FAIL, TAG,
                        "Failed to lock LVGL while updating UI");

    display_set_ui_content_locked(title, message, message_font, accent);
    lv_refr_now(s_display.display);

    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t app_display_init(void)
{
    // 本组件是单例；重复初始化会重复占用 SPI 总线并创建第二个 LVGL 任务。
    ESP_RETURN_ON_FALSE(!s_display.initialized, ESP_ERR_INVALID_STATE, TAG,
                        "Display is already initialized");

    /*
     * 初始化过程中先关闭背光。LCD 复位、写寄存器以及创建 LVGL 缓冲时都可能
     * 暂时没有有效画面，先关闭背光可避免用户看到随机像素或白屏。
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

    /*
     * LCD 只接收数据，所以 MISO 为 -1。max_transfer_sz 与一个 LVGL 部分刷新
     * 缓冲相同；任意一次 flush 的像素数都不会超过该缓冲容量。
     */
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
     * 不在此处安装 DMA 完成回调。lvgl_port_add_disp() 会向同一个 IO 注册
     * 回调，并在传输完成时通知 LVGL 哪个绘制缓冲可以安全复用。
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
        display_apply_waveshare_settings(s_display.io), TAG,
        "Failed to apply Waveshare LCD settings");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_invert_color(s_display.panel, true), TAG,
        "Failed to enable LCD color inversion");

    /*
     * esp_lvgl_port 为本项目创建 LVGL tick 定时器和刷新任务。暂时沿用适配层
     * 的默认优先级、栈和 5 ms tick；后续真正加入桌宠动画后再用测量结果调优。
     */
    const lvgl_port_cfg_t lvgl_config = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(
        lvgl_port_init(&lvgl_config), TAG,
        "Failed to initialize LVGL port");

    /*
     * ST7789 通过 SPI 接收 RGB565 时需要高字节在前，ESP32-S3 内存中则是
     * 小端序，因此启用 swap_bytes。迁移前由手写 set_pixel() 逐像素完成
     * 同一转换，现在统一交给 esp_lvgl_port 在 flush 前处理。
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

    ESP_RETURN_ON_ERROR(
        display_create_ui(), TAG,
        "Failed to create initial LVGL UI");

    /*
     * 只有硬件、LVGL、对象树和第一帧都准备好后才开放公共 API 并点亮屏幕。
     * 这也维持了原先“显示初始化完成后再启动按键任务”的调用契约。
     */
    s_display.initialized = true;
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_disp_on_off(s_display.panel, true), TAG,
        "Failed to enable LCD panel");
    ESP_RETURN_ON_ERROR(
        gpio_set_level(BOARD_LCD_PIN_BACKLIGHT, 1), TAG,
        "Failed to turn LCD backlight on");

    ESP_LOGI(TAG, "LCD and LVGL are ready");
    return ESP_OK;
}

esp_err_t app_display_show_prompt(void)
{
    return display_update_ui(
        "BUTTON TEST", "PRESS A BUTTON",
        &lv_font_montserrat_20, lv_color_hex(UI_COLOR_YELLOW));
}

esp_err_t app_display_show_button(const char *button_name,
                                  app_display_accent_t accent)
{
    ESP_RETURN_ON_FALSE(
        accent >= 0 && accent < APP_DISPLAY_ACCENT_COUNT,
        ESP_ERR_INVALID_ARG, TAG, "Invalid display accent");

    return display_update_ui(
        "BUTTON", button_name,
        &lv_font_montserrat_42, display_get_accent_color(accent));
}
