#pragma once

/*
 * power 组件管理 Waveshare 板卡的电池供电保持和 PWR 长按关机。
 *
 * 调用顺序：
 * 1. app_main 进入后首先调用 power_manager_init()，立即接管电池供电；
 * 2. 显示和页面初始化完成后调用 power_manager_start() 监听关机按键；
 * 3. PWR 释放过一次后，再次持续按下约 2 秒会执行关机回调并切断电池。
 */

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 关机前准备回调。
 *
 * 回调运行在电源监控任务上下文中，不能在其中永久阻塞。适合关闭背光、
 * 停止外设或保存少量状态；返回错误会被记录，但不会阻止最终断电。
 *
 * @param context 注册回调时传入的用户上下文。
 * @return ESP_OK 表示准备完成，其他错误码会记录为警告。
 */
typedef esp_err_t (*power_manager_shutdown_callback_t)(void *context);

/**
 * @brief 接管电池供电并初始化 PWR 按键。
 *
 * 必须作为 app_main 的第一个硬件初始化调用。函数先把 BAT_EN(GPIO2)
 * 置为高电平，再配置低电平有效的 PWR(GPIO5) 输入。只能调用一次。
 *
 * @return ESP_OK 成功；重复调用或 GPIO 配置失败时返回对应错误。
 */
esp_err_t power_manager_init(void);

/**
 * @brief 启动 PWR 长按关机监控。
 *
 * 必须在 power_manager_init() 之后、应用硬件和页面准备完成后调用。
 * 若开机时 PWR 仍被按住，监控会先等待按键释放，不会把开机动作误判成
 * 长按关机。此函数只能调用一次。
 *
 * @param callback 断电前执行的可选回调，允许为 NULL。
 * @param context 传给 callback 的用户上下文。
 * @return ESP_OK 成功；调用顺序错误或任务创建失败时返回对应错误。
 */
esp_err_t power_manager_start(
    power_manager_shutdown_callback_t callback, void *context);

#ifdef __cplusplus
}
#endif
