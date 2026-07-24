#pragma once

/*
 * network 组件负责 Wi-Fi STA 的一次性初始化、异步启动和断线重连。
 *
 * 组件使用 ESP-IDF 默认事件循环发布的 WIFI_EVENT/IP_EVENT；调用者不需要
 * 等待联网完成。SSID 和密码来自 menuconfig，不通过公开接口传递。
 */

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 NVS、TCP/IP、默认事件循环和 Wi-Fi STA。
 *
 * 必须在 network_manager_start() 和 time_service_init() 之前调用，整个
 * 程序只能调用一次。函数只建立驱动、事件处理器和重连定时器，不启动射频
 * 或等待网络连接。
 *
 * @return ESP_OK 成功；重复调用、内存不足或底层初始化失败时返回对应错误。
 */
esp_err_t network_manager_init(void);

/**
 * @brief 使用 menuconfig 中的凭据异步启动 Wi-Fi STA。
 *
 * 必须先调用 network_manager_init()。SSID 必须为 1 到 32 字节，密码必须
 * 为 8 到 63 字节；本阶段仅连接 WPA2/WPA3 Personal 网络。启动成功仅表示
 * Wi-Fi 驱动已经运行，是否取得 IP 应通过 network_manager_is_connected()
 * 或 IP_EVENT 判断。
 *
 * @return ESP_OK 已启动；凭据无效、重复调用或驱动启动失败时返回对应错误。
 */
esp_err_t network_manager_start(void);

/**
 * @brief 查询 Wi-Fi STA 当前是否已经取得 IPv4 地址。
 *
 * 此函数可从普通任务、LVGL 任务或 ESP-IDF 事件回调之外的上下文调用。
 *
 * @return true 已取得地址；false 未连接、正在重连或尚未启动。
 */
bool network_manager_is_connected(void);

#ifdef __cplusplus
}
#endif
