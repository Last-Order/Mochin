#pragma once

/*
 * display 组件通过 app_display API 向应用层提供显示屏功能。
 *
 * 调用者不需要知道 SPI、ST7789、LVGL 或 DMA 的细节，只需：
 * 1. 启动时调用一次 app_display_init()；
 * 2. 按需调用 app_display_show_prompt() 或 app_display_show_button()。
 *
 * 当前板上只有一块 LCD，所以实现采用单例（全局唯一显示设备）设计。
 */

#include "esp_err.h"

/**
 * @brief 界面的强调色。
 *
 * 这里使用枚举而不是让业务层直接传 RGB565 数值，可避免业务代码依赖
 * LCD 的具体像素格式，也便于以后统一更换配色。
 */
typedef enum {
    APP_DISPLAY_ACCENT_YELLOW = 0,  ///< 黄色：BOOT 按键
    APP_DISPLAY_ACCENT_MAGENTA,     ///< 品红色：PWR 按键
    APP_DISPLAY_ACCENT_GREEN,       ///< 绿色：PLUS 按键
    APP_DISPLAY_ACCENT_COUNT,       ///< 颜色数量，仅用于范围检查
} app_display_accent_t;

/**
 * @brief 初始化 ST7789 LCD，并显示初始的按键提示画面。
 *
 * 此函数会配置背光 GPIO、SPI 总线、esp_lcd、LVGL 任务和双 DMA 绘制缓冲，
 * 并在点亮背光前显示初始画面。必须在其他 app_display_* 函数之前调用，
 * 且整个程序只调用一次。
 *
 * @return ESP_OK 成功；其他值表示具体的 ESP-IDF 初始化错误。
 */
esp_err_t app_display_init(void);

/**
 * @brief 恢复显示“PRESS A BUTTON”的初始提示画面。
 *
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 表示显示屏尚未初始化。
 */
esp_err_t app_display_show_prompt(void);

/**
 * @brief 使用指定强调色显示刚刚按下的按键名称。
 *
 * @param button_name 要显示的按键名称，例如 "BOOT"。
 * @param accent      从 app_display_accent_t 中选择的强调色。
 *
 * @return ESP_OK 成功；参数或显示状态不正确时返回对应错误。
 */
esp_err_t app_display_show_button(const char *button_name,
                                  app_display_accent_t accent);
