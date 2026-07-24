/*
 * 显示组件的内部实现。
 *
 * 这个文件可以按三层来阅读：
 * 1. frame_* 函数：只在内存中的 RGB565 帧缓冲上画点、矩形和文字；
 * 2. display_render_and_show()：把画好的帧通过 DMA 发送给 LCD；
 * 3. app_display_* 公共函数：为应用层提供简单、稳定的界面操作。
 *
 * 将“画什么”和“怎样驱动 LCD”放在同一组件内部，可以保证应用入口不需要
 * 了解 ST7789 命令、SPI 时序和 DMA 缓冲生命周期。
 */

#include "display/app_display.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "board/board_config.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/*
 * 每个像素使用 RGB565，占 16 bit（2 字节）。
 * 240 × 240 × 2 = 115200 字节，因此必须确认 SPI 总线允许这么大的传输。
 */
#define LCD_FRAME_BUFFER_SIZE \
    (BOARD_LCD_WIDTH * BOARD_LCD_HEIGHT * sizeof(uint16_t))

/*
 * RGB565 将红、绿、蓝分别压缩为 5、6、5 bit。
 * 这些常量是已经编码好的 16 bit 颜色值，不是普通的 0xRRGGBB。
 */
#define RGB565_NAVY 0x000F
#define RGB565_CYAN 0x07FF
#define RGB565_YELLOW 0xFFE0
#define RGB565_GREEN 0x07E0
#define RGB565_MAGENTA 0xF81F

static const char *TAG = "app_display";

/*
 * 一个 glyph_t 表示一个 5×7 点阵字符。
 * columns[0..4] 是从左到右的五列，每个字节的低 7 bit 对应从上到下七行。
 */
typedef struct {
    char character;
    uint8_t columns[5];
} glyph_t;

/*
 * 显示模块的全部运行状态集中保存在这里。
 * 当前硬件只有一块 LCD，因此不要求应用层创建或管理对象。
 */
typedef struct {
    esp_lcd_panel_handle_t panel;   // esp_lcd 返回的 ST7789 面板句柄
    uint8_t *frame;                 // 一整屏、支持 DMA 的 RGB565 帧缓冲
    SemaphoreHandle_t transfer_done; // DMA 传输完成后由中断回调释放
    SemaphoreHandle_t lock;         // 防止多个任务同时改写同一帧缓冲
    bool initialized;               // 公共显示 API 是否已经可以使用
} display_context_t;

// static 限制变量只在本文件可见，避免其他模块绕过公开 API 直接修改状态。
static display_context_t s_display;

// 精简的 5×7 英文字库，仅收录当前界面实际需要的字符，以节省 Flash。
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

/**
 * @brief 在帧缓冲中设置一个像素。
 *
 * 坐标超出屏幕时直接忽略，避免调用者写出 frame 数组边界。
 */
static void frame_set_pixel(uint8_t *frame, int x, int y, uint16_t color)
{
    if (x < 0 || x >= BOARD_LCD_WIDTH ||
        y < 0 || y >= BOARD_LCD_HEIGHT) {
        return;
    }

    /*
     * ST7789 通过 SPI 接收 RGB565 时要求高字节先发送。
     * ESP32-S3 本身是小端 CPU，所以这里不能直接用 uint16_t* 赋值；
     * 必须明确把高 8 bit 写在前、低 8 bit 写在后。
     */
    const size_t offset =
        ((size_t)y * BOARD_LCD_WIDTH + x) * sizeof(uint16_t);
    frame[offset] = (uint8_t)(color >> 8);
    frame[offset + 1] = (uint8_t)(color & 0xFF);
}

/**
 * @brief 在帧缓冲中填充一个实心矩形。
 *
 * 前半部分会把超出屏幕的矩形裁剪到有效区域。这样绘制边框或放大字符时，
 * 即使坐标恰好接近边缘，也不会发生越界写内存。
 */
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

/**
 * @brief 从精简字库中查找字符点阵。
 *
 * @return 找到时返回五列点阵；未收录时返回 NULL。
 */
static const uint8_t *font_find_glyph(char character)
{
    for (size_t i = 0; i < sizeof(FONT) / sizeof(FONT[0]); ++i) {
        if (FONT[i].character == character) {
            return FONT[i].columns;
        }
    }
    return NULL;
}

/**
 * @brief 按指定倍数绘制一个 5×7 字符。
 *
 * 字库中的一个“点”会被扩展为 scale × scale 的实心矩形，从而在不准备
 * 多套字体资源的情况下显示更大的文字。
 */
static void frame_draw_char(uint8_t *frame, int x, int y, char character,
                            int scale, uint16_t color)
{
    const uint8_t *columns = font_find_glyph(character);
    if (columns == NULL) {
        // 空格和未收录字符保持为空白，不把它们当作致命错误。
        return;
    }

    // 逐列、逐行检查点阵中的每一个 bit。
    for (int column = 0; column < 5; ++column) {
        for (int row = 0; row < 7; ++row) {
            if ((columns[column] & (1U << row)) != 0) {
                frame_fill_rect(frame, x + column * scale,
                                y + row * scale, scale, scale, color);
            }
        }
    }
}

/**
 * @brief 计算一行点阵文字绘制后的像素宽度。
 *
 * 每个字符占 5 列，字符之间留 1 列空白，因此步进是 6 列；
 * 最后一个字符后面不需要空白，所以总宽度再减 1 列。
 */
static int frame_text_width(const char *text, int scale)
{
    const size_t length = strlen(text);
    return length == 0 ? 0 : (int)(length * 6 - 1) * scale;
}

/**
 * @brief 从左到右绘制一行文字。
 */
static void frame_draw_text(uint8_t *frame, int x, int y, const char *text,
                            int scale, uint16_t color)
{
    while (*text != '\0') {
        frame_draw_char(frame, x, y, *text, scale, color);
        x += 6 * scale;
        ++text;
    }
}

/**
 * @brief 绘制本应用统一使用的“标题 + 主消息 + 彩色边框”画面。
 *
 * title 和 message 会根据实际像素宽度水平居中。这里只修改内存中的 frame，
 * 并不会立即操作 LCD；真正发送发生在 display_render_and_show()。
 */
static void frame_draw_screen(uint8_t *frame, const char *title,
                              const char *message, int message_scale,
                              uint16_t accent_color)
{
    const int title_scale = 2;

    // 先计算文字宽度，再用“屏幕剩余宽度的一半”得到居中的起点。
    const int title_x =
        (BOARD_LCD_WIDTH - frame_text_width(title, title_scale)) / 2;
    const int message_x =
        (BOARD_LCD_WIDTH - frame_text_width(message, message_scale)) / 2;
    const int message_y =
        (BOARD_LCD_HEIGHT - 7 * message_scale) / 2 + 12;

    // 每次都重画整屏，避免上一帧较长的文字残留在新画面上。
    frame_fill_rect(frame, 0, 0, BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT,
                    RGB565_NAVY);

    // 四个矩形共同组成距离屏幕边缘 8 像素的边框。
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

/**
 * @brief LCD 颜色数据发送完成回调。
 *
 * esp_lcd 会在 SPI DMA 中断上下文中调用此函数。中断中不能阻塞，也不能调用
 * 普通版本 xSemaphoreGive()，所以必须使用 xSemaphoreGiveFromISR()。
 *
 * 返回 true 表示本次释放信号量唤醒了更高优先级任务，FreeRTOS 可以在退出
 * 中断时立即切换到该任务。
 */
static bool lcd_color_transfer_done(
    esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_io_event_data_t *event_data,
    void *user_ctx)
{
    (void)panel_io;
    (void)event_data;

    // user_ctx 是创建 panel IO 时传入的 transfer_done 信号量。
    BaseType_t high_priority_task_woken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)user_ctx,
                          &high_priority_task_woken);
    return high_priority_task_woken == pdTRUE;
}

static esp_err_t display_apply_waveshare_settings(
    esp_lcd_panel_io_handle_t io)
{
    /*
     * 这些寄存器值来自 Waveshare 官方示例，用来配置供电和时序。
     * ESP-IDF 自带的 ST7789 驱动已经负责复位、退出休眠、RGB565、地址窗口
     * 和像素传输，所以这里只补充本块 LCD 特有的设置。
     *
     * 不要仅凭其他 ST7789 屏幕的代码修改这些值；同一控制器配合不同面板时，
     * 电压、门控和 porch 参数仍可能不同。
     */
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
 * @brief 将业务层颜色枚举转换为 LCD 使用的 RGB565 数值。
 *
 * 这层转换把像素格式细节留在显示组件内部。
 */
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

/**
 * @brief 以线程安全方式绘制并发送一整帧。
 *
 * 完整流程：
 * 1. 获取 lock，独占帧缓冲；
 * 2. 在内存中画好整屏；
 * 3. 将 DMA 传输加入 esp_lcd 队列；
 * 4. 等待 transfer_done，确认 DMA 已经不再读取 frame；
 * 5. 释放 lock，允许下一个任务更新画面。
 *
 * 第 4 步非常重要：esp_lcd_panel_draw_bitmap() 只负责排队，DMA 在后台继续
 * 读取 frame。如果立即覆盖缓冲，屏幕可能出现撕裂、乱码或随机颜色。
 */
static esp_err_t display_render_and_show(const char *title,
                                         const char *message,
                                         int message_scale,
                                         uint16_t accent_color)
{
    ESP_RETURN_ON_FALSE(s_display.initialized, ESP_ERR_INVALID_STATE, TAG,
                        "Display is not initialized");
    ESP_RETURN_ON_FALSE(title != NULL && message != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "Text must not be NULL");

    // portMAX_DELAY 表示一直等待，直到上一位使用者释放显示锁。
    if (xSemaphoreTake(s_display.lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    // 此时本任务独占 frame，可以安全生成新画面。
    frame_draw_screen(s_display.frame, title, message, message_scale,
                      accent_color);

    // draw_bitmap 返回 ESP_OK 时，数据通常只是进入异步传输队列。
    esp_err_t ret = esp_lcd_panel_draw_bitmap(
        s_display.panel, 0, 0, BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT,
        s_display.frame);

    // 等待中断回调发出完成信号后，frame 才能被下一帧复用。
    if (ret == ESP_OK &&
        xSemaphoreTake(s_display.transfer_done, portMAX_DELAY) != pdTRUE) {
        ret = ESP_FAIL;
    }

    // 无论传输成功还是失败，都必须释放互斥锁，避免后续调用永久卡住。
    xSemaphoreGive(s_display.lock);
    return ret;
}

esp_err_t app_display_init(void)
{
    // 本模块是单例，重复初始化 SPI 总线或面板会造成资源冲突。
    ESP_RETURN_ON_FALSE(!s_display.initialized, ESP_ERR_INVALID_STATE, TAG,
                        "Display is already initialized");

    /*
     * transfer_done 用于“中断通知任务”，二值信号量足够；
     * lock 用于“任务之间互斥”，因此使用带所有权语义的互斥量。
     */
    s_display.transfer_done = xSemaphoreCreateBinary();
    s_display.lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(
        s_display.transfer_done != NULL && s_display.lock != NULL,
        ESP_ERR_NO_MEM, TAG, "Unable to create display semaphores");

    /*
     * 初始化过程中先关闭背光。这样用户不会看到 LCD 复位和写寄存器时的
     * 随机内容；第一帧准备完成后再统一点亮。
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
    ESP_RETURN_ON_ERROR(gpio_set_level(BOARD_LCD_PIN_BACKLIGHT, 0), TAG,
                        "Failed to turn LCD backlight off");

    /*
     * SPI 总线只发送数据到 LCD，因此 miso_io_num 设置为 -1。
     * max_transfer_sz 必须至少容纳一整帧，否则整屏 draw_bitmap 会失败。
     */
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

    /*
     * panel IO 描述“如何通过 SPI 与 LCD 交换命令和数据”：
     * - spi_mode=3 和 40 MHz 已经在实物上验证；
     * - lcd_cmd_bits/lcd_param_bits=8 表示命令和参数都是 8 bit；
     * - 回调与 user_ctx 共同实现 DMA 完成通知。
     */
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

    // panel 对象描述具体控制器型号和像素格式，这里选择 ST7789 + RGB565。
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

    // 在第一帧准备好之前暂时关闭面板输出，与背光关闭配合避免闪屏。
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_disp_on_off(s_display.panel, false), TAG,
        "Failed to disable LCD panel");

    ESP_RETURN_ON_ERROR(display_apply_waveshare_settings(io), TAG,
                        "Failed to apply Waveshare LCD settings");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_invert_color(s_display.panel, true), TAG,
        "Failed to enable LCD color inversion");

    /*
     * 普通 malloc 得到的内存不一定能被 SPI DMA 访问。
     * spi_bus_dma_memory_alloc() 会返回符合当前 SPI 总线 DMA 要求的缓冲区。
     * 缓冲区会在程序整个运行期间反复使用，因此这里不释放它。
     */
    s_display.frame = spi_bus_dma_memory_alloc(
        BOARD_LCD_HOST, LCD_FRAME_BUFFER_SIZE, 0);
    ESP_RETURN_ON_FALSE(s_display.frame != NULL, ESP_ERR_NO_MEM, TAG,
                        "Unable to allocate LCD frame buffer");

    /*
     * 只有完成上述所有关键步骤后才标记 initialized，公共绘图函数会检查
     * 这个标志。先画好提示画面，再开启面板和背光。
     */
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
    // 提示页使用较小字号，确保完整句子可以在 240 像素宽度内居中显示。
    return display_render_and_show("BUTTON TEST", "PRESS A BUTTON", 2,
                                   RGB565_YELLOW);
}

esp_err_t app_display_show_button(const char *button_name,
                                  app_display_accent_t accent)
{
    // APP_DISPLAY_ACCENT_COUNT 是哨兵值，不代表一种可显示颜色。
    ESP_RETURN_ON_FALSE(accent >= 0 && accent < APP_DISPLAY_ACCENT_COUNT,
                        ESP_ERR_INVALID_ARG, TAG,
                        "Invalid display accent");

    // 按键名称较短，使用 6 倍点阵字号突出显示。
    return display_render_and_show(
        "BUTTON", button_name, 6, display_get_accent_color(accent));
}
