#pragma once

/*
 * time_service 组件负责 SNTP 校时和北京时间转换。
 *
 * 服务依赖 network_manager_init() 已经建立 esp-netif 和默认事件循环；
 * 初始化后监听 IP_EVENT_STA_GOT_IP，在取得网络地址时异步启动 SNTP。
 */

#include <stdbool.h>
#include <time.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 配置北京时间与 SNTP 服务。
 *
 * 必须在 network_manager_init() 之后、network_manager_start() 之前调用，
 * 且整个程序只能调用一次。NTP 服务器来自 CONFIG_PET_SNTP_SERVER；配置
 * 为空时函数仍成功完成初始化，但禁用网络校时并记录错误。
 *
 * @return ESP_OK 成功或按配置禁用；重复调用、事件注册或 SNTP 初始化失败
 *         时返回对应错误。
 */
esp_err_t time_service_init(void);

/**
 * @brief 获取最近一次 SNTP 校准后的北京时间。
 *
 * 函数不会等待网络或 SNTP，可从普通任务和 LVGL 任务调用。首次同步之前
 * 返回 false；同步成功后即使 Wi-Fi 暂时断开，系统时钟仍继续运行并返回
 * 有效时间。
 *
 * @param out_time 接收北京时间的 tm 结构，不能为空。
 *
 * @return true 已写入有效时间；false 参数为空或设备尚未成功校时。
 */
bool time_service_get_local_time(struct tm *out_time);

#ifdef __cplusplus
}
#endif
