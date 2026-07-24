#pragma once

/*
 * input 组件通过 board_buttons API 封装三个板载按键的 GPIO 读取和软件消抖。
 *
 * 模块启动后会创建一个独立 FreeRTOS 任务轮询按键。确认按下后，通过
 * 回调通知应用层。回调运行在普通任务上下文中，不是中断上下文，因此
 * 可以调用 app_display_show_button() 这类会等待 DMA 完成的函数。
 */

#include "esp_err.h"

// 使用枚举给按键分配稳定的逻辑 ID，应用层无需依赖 GPIO 数字。
typedef enum {
    BOARD_BUTTON_BOOT = 0,  ///< 左侧 BOOT 按键
    BOARD_BUTTON_PWR,       ///< 中间 PWR 按键
    BOARD_BUTTON_PLUS,      ///< 右侧 PLUS 按键
    BOARD_BUTTON_COUNT,     ///< 按键总数，仅用于数组大小和范围检查
} board_button_id_t;

/**
 * @brief 按键按下事件回调函数类型。
 *
 * @param button  发生事件的按键 ID。
 * @param user_ctx 注册回调时原样保存的用户上下文指针，可以为 NULL。
 */
typedef void (*board_button_pressed_cb_t)(board_button_id_t button,
                                          void *user_ctx);

/**
 * @brief 配置三个低电平有效的板载按键，并读取它们的初始状态。
 *
 * 此函数只完成 GPIO 初始化，不会创建任务。整个程序只调用一次。
 *
 * @return ESP_OK 成功；重复初始化或 GPIO 配置失败时返回对应错误。
 */
esp_err_t board_buttons_init(void);

/**
 * @brief 创建并启动带软件消抖的按键轮询任务。
 *
 * 必须先调用 board_buttons_init()。检测到稳定的按下事件后，模块会调用
 * callback(button, user_ctx)。
 *
 * @param callback 检测到按下事件时调用的函数，不能为 NULL。
 * @param user_ctx 传递给回调的自定义上下文，可以为 NULL。
 *
 * @return ESP_OK 成功；状态、参数或任务内存不足时返回对应错误。
 */
esp_err_t board_buttons_start(board_button_pressed_cb_t callback,
                              void *user_ctx);

/**
 * @brief 获取按键在 PCB 丝印上的名称。
 *
 * @param button 按键 ID。
 * @return "BOOT"、"PWR"、"PLUS"；未知 ID 返回 "UNKNOWN"。
 */
const char *board_button_get_name(board_button_id_t button);
