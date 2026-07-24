/*
 * SNTP 校时与北京时间服务。
 *
 * SNTP 在 IP_EVENT_STA_GOT_IP 到达后才启动，避免设备尚未联网时立即请求
 * 导致无意义的退避。同步回调只写入原子有效标志；页面读取时间时使用
 * time()/localtime_r()，不与 lwIP 或事件任务共享可变 tm 缓冲区。
 */

#include "time_service/time_service.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"

#define TIME_SERVICE_TIMEZONE "CST-8"

static const char *TAG = "time_service";

typedef struct {
    esp_event_handler_instance_t ip_event_instance;
    bool initialized;
    bool sntp_enabled;
} time_service_context_t;

static time_service_context_t s_time_service;
static atomic_bool s_time_synchronized;

/**
 * @brief SNTP 完成系统时间更新后的通知。
 *
 * 回调运行在 lwIP/TCPIP 相关上下文，只执行无阻塞的时间格式化、日志和
 * 原子写入，不操作 LVGL 对象。
 */
static void time_service_sync_cb(struct timeval *time_value)
{
    const time_t synchronized_at =
        time_value != NULL ? (time_t)time_value->tv_sec : time(NULL);
    struct tm local_time = {0};
    char time_text[32] = {0};

    atomic_store(&s_time_synchronized, true);
    if (localtime_r(&synchronized_at, &local_time) != NULL &&
        strftime(time_text, sizeof(time_text), "%Y-%m-%d %H:%M:%S",
                 &local_time) > 0U) {
        ESP_LOGI(TAG, "System time synchronized: %s", time_text);
    } else {
        ESP_LOGI(TAG, "System time synchronized");
    }
}

/**
 * @brief 取得 IPv4 地址后启动或重启 SNTP。
 */
static void time_service_ip_event_handler(
    void *arg, esp_event_base_t event_base, int32_t event_id,
    void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;
    (void)event_data;

    if (!s_time_service.sntp_enabled) {
        return;
    }

    ESP_LOGI(TAG, "Starting SNTP with server '%s'",
             CONFIG_PET_SNTP_SERVER);
    const esp_err_t err = esp_netif_sntp_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start SNTP: %s",
                 esp_err_to_name(err));
    }
}

esp_err_t time_service_init(void)
{
    ESP_RETURN_ON_FALSE(!s_time_service.initialized,
                        ESP_ERR_INVALID_STATE, TAG,
                        "Time service is already initialized");
    ESP_RETURN_ON_FALSE(
        setenv("TZ", TIME_SERVICE_TIMEZONE, 1) == 0, ESP_FAIL, TAG,
        "Failed to configure China Standard Time timezone");
    tzset();

    atomic_store(&s_time_synchronized, false);
    s_time_service.initialized = true;

    if (CONFIG_PET_SNTP_SERVER[0] == '\0') {
        ESP_LOGE(TAG,
                 "NTP server is empty; network time synchronization "
                 "is disabled");
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP,
            time_service_ip_event_handler, NULL,
            &s_time_service.ip_event_instance),
        TAG, "Failed to register time-service IP event handler");

    esp_sntp_config_t config =
        ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_PET_SNTP_SERVER);
    config.start = false;
    config.wait_for_sync = false;
    config.smooth_sync = false;
    config.sync_cb = time_service_sync_cb;

    const esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        esp_event_handler_instance_unregister(
            IP_EVENT, IP_EVENT_STA_GOT_IP,
            s_time_service.ip_event_instance);
        ESP_LOGE(TAG, "Failed to initialize SNTP: %s",
                 esp_err_to_name(err));
        return err;
    }

    s_time_service.sntp_enabled = true;
    ESP_LOGI(TAG, "Time service initialized: timezone=%s, server=%s",
             TIME_SERVICE_TIMEZONE, CONFIG_PET_SNTP_SERVER);
    return ESP_OK;
}

bool time_service_get_local_time(struct tm *out_time)
{
    if (out_time == NULL || !atomic_load(&s_time_synchronized)) {
        return false;
    }

    const time_t now = time(NULL);
    return localtime_r(&now, out_time) != NULL;
}
