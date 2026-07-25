/*
 * Wi-Fi STA 初始化与断线重连。
 *
 * 组件把耗时的扫描、认证和 DHCP 全部交给 ESP-IDF Wi-Fi/esp-netif 任务，
 * app_main 只负责启动。断线回调不直接等待，而是用一次性 esp_timer 按
 * 1、2、4、8、16、30 秒退避重连，避免路由器离线时形成紧密重试循环。
 */

#include "network/network_manager.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "nvs_flash.h"

#define NETWORK_WIFI_SSID_MAX_BYTES 32U
#define NETWORK_WIFI_PASSWORD_MIN_BYTES 8U
#define NETWORK_WIFI_PASSWORD_MAX_BYTES 63U

static const char *TAG = "network_manager";

static const uint32_t s_reconnect_delays_ms[] = {
    1000,
    2000,
    4000,
    8000,
    16000,
    30000,
};

typedef struct {
    esp_timer_handle_t reconnect_timer;
    esp_event_handler_instance_t wifi_event_instance;
    esp_event_handler_instance_t ip_event_instance;
    bool initialized;
} network_manager_context_t;

static network_manager_context_t s_network;
static atomic_bool s_started;
static atomic_bool s_connected;
static atomic_uint s_retry_index;

/**
 * @brief 初始化 Wi-Fi 驱动依赖的 NVS。
 *
 * NVS 布局升级或空间耗尽时，旧内容无法再由当前 NVS 实现可靠读取，只在
 * 这两个明确错误下擦除 NVS 分区并重建。当前阶段 Wi-Fi 凭据来自固件配置，
 * 因而此恢复动作不会丢失联网参数。
 */
static esp_err_t network_storage_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err != ESP_ERR_NVS_NO_FREE_PAGES &&
        err != ESP_ERR_NVS_NEW_VERSION_FOUND) {
        return err;
    }

    ESP_LOGW(TAG, "NVS requires recovery (%s); erasing NVS partition",
             esp_err_to_name(err));
    ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG,
                        "Failed to erase incompatible NVS partition");
    return nvs_flash_init();
}

/**
 * @brief 安排下一次异步重连。
 *
 * 此函数可能从默认事件循环任务或 esp_timer 任务调用。重试下标使用原子
 * 变量；定时器停止/重启操作保证同一时刻最多保留一次待执行连接。
 */
static void network_schedule_reconnect(void)
{
    if (!atomic_load(&s_started) || atomic_load(&s_connected)) {
        return;
    }

    unsigned int retry_index = atomic_load(&s_retry_index);
    const unsigned int last_index =
        (unsigned int)(sizeof(s_reconnect_delays_ms) /
                       sizeof(s_reconnect_delays_ms[0]) - 1U);
    if (retry_index > last_index) {
        retry_index = last_index;
    }

    const uint32_t delay_ms = s_reconnect_delays_ms[retry_index];
    if (retry_index < last_index) {
        atomic_store(&s_retry_index, retry_index + 1U);
    }

    if (esp_timer_is_active(s_network.reconnect_timer)) {
        const esp_err_t stop_err =
            esp_timer_stop(s_network.reconnect_timer);
        if (stop_err != ESP_OK && stop_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "Failed to stop reconnect timer: %s",
                     esp_err_to_name(stop_err));
        }
    }

    const esp_err_t start_err = esp_timer_start_once(
        s_network.reconnect_timer, (uint64_t)delay_ms * 1000ULL);
    if (start_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to schedule Wi-Fi reconnect: %s",
                 esp_err_to_name(start_err));
        return;
    }

    ESP_LOGW(TAG, "Wi-Fi reconnect scheduled in %lu ms",
             (unsigned long)delay_ms);
}

/**
 * @brief 重连定时器回调，只触发一次非阻塞连接尝试。
 */
static void network_reconnect_timer_cb(void *arg)
{
    (void)arg;

    if (!atomic_load(&s_started) || atomic_load(&s_connected)) {
        return;
    }

    const esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi reconnect request failed: %s",
                 esp_err_to_name(err));
        network_schedule_reconnect();
    }
}

/**
 * @brief 处理 STA 启动和断线事件。
 */
static void network_wifi_event_handler(
    void *arg, esp_event_base_t event_base, int32_t event_id,
    void *event_data)
{
    (void)arg;
    (void)event_base;

    if (event_id == WIFI_EVENT_STA_START) {
        const esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Initial Wi-Fi connect request failed: %s",
                     esp_err_to_name(err));
            network_schedule_reconnect();
        }
        return;
    }

    if (event_id != WIFI_EVENT_STA_DISCONNECTED) {
        return;
    }

    atomic_store(&s_connected, false);
    const wifi_event_sta_disconnected_t *event =
        (const wifi_event_sta_disconnected_t *)event_data;
    ESP_LOGW(TAG, "Wi-Fi disconnected, reason=%u",
             event != NULL ? (unsigned int)event->reason : 0U);
    network_schedule_reconnect();
}

/**
 * @brief 取得 IPv4 地址后结束退避并发布已连接状态。
 */
static void network_ip_event_handler(
    void *arg, esp_event_base_t event_base, int32_t event_id,
    void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;

    const ip_event_got_ip_t *event =
        (const ip_event_got_ip_t *)event_data;
    atomic_store(&s_connected, true);
    atomic_store(&s_retry_index, 0U);

    if (esp_timer_is_active(s_network.reconnect_timer)) {
        const esp_err_t err = esp_timer_stop(s_network.reconnect_timer);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "Failed to stop reconnect timer: %s",
                     esp_err_to_name(err));
        }
    }

    if (event != NULL) {
        ESP_LOGI(TAG, "Wi-Fi connected, IPv4=" IPSTR,
                 IP2STR(&event->ip_info.ip));
    } else {
        ESP_LOGI(TAG, "Wi-Fi connected and obtained IPv4 address");
    }
}

esp_err_t network_manager_init(void)
{
    ESP_RETURN_ON_FALSE(!s_network.initialized, ESP_ERR_INVALID_STATE,
                        TAG, "Network manager is already initialized");

    ESP_RETURN_ON_ERROR(network_storage_init(), TAG,
                        "Failed to initialize NVS");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG,
                        "Failed to initialize esp-netif");

    esp_err_t err = esp_event_loop_create_default();
    ESP_RETURN_ON_FALSE(
        err == ESP_OK || err == ESP_ERR_INVALID_STATE, err, TAG,
        "Failed to create default event loop");

    ESP_RETURN_ON_FALSE(
        esp_netif_create_default_wifi_sta() != NULL, ESP_ERR_NO_MEM,
        TAG, "Failed to create default Wi-Fi STA interface");

    const wifi_init_config_t wifi_init_config =
        WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_init_config), TAG,
                        "Failed to initialize Wi-Fi driver");

    const esp_timer_create_args_t reconnect_timer_config = {
        .callback = network_reconnect_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_reconnect",
    };
    ESP_RETURN_ON_ERROR(
        esp_timer_create(
            &reconnect_timer_config, &s_network.reconnect_timer),
        TAG, "Failed to create Wi-Fi reconnect timer");

    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, network_wifi_event_handler,
            NULL, &s_network.wifi_event_instance),
        TAG, "Failed to register Wi-Fi event handler");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, network_ip_event_handler,
            NULL, &s_network.ip_event_instance),
        TAG, "Failed to register IP event handler");

    atomic_store(&s_started, false);
    atomic_store(&s_connected, false);
    atomic_store(&s_retry_index, 0U);
    s_network.initialized = true;

    ESP_LOGI(TAG, "Wi-Fi STA infrastructure initialized");
    return ESP_OK;
}

esp_err_t network_manager_start(void)
{
    ESP_RETURN_ON_FALSE(s_network.initialized, ESP_ERR_INVALID_STATE,
                        TAG, "Network manager is not initialized");
    ESP_RETURN_ON_FALSE(!atomic_load(&s_started),
                        ESP_ERR_INVALID_STATE, TAG,
                        "Wi-Fi STA is already started");

    /*
     * Kconfig 字符串始终以 NUL 结尾，先取实际长度再按 Wi-Fi 协议字段上限
     * 校验。不能给 strnlen() 一个大于编译期字面量对象的上界，否则 GCC
     * 的 stringop-overread 检查会把合法的短配置视为构建错误。
     */
    const size_t ssid_length = strlen(CONFIG_PET_WIFI_SSID);
    const size_t password_length = strlen(CONFIG_PET_WIFI_PASSWORD);
    ESP_RETURN_ON_FALSE(
        ssid_length > 0U && ssid_length <= NETWORK_WIFI_SSID_MAX_BYTES,
        ESP_ERR_INVALID_ARG, TAG,
        "Wi-Fi SSID must contain 1 to 32 bytes");
    ESP_RETURN_ON_FALSE(
        password_length >= NETWORK_WIFI_PASSWORD_MIN_BYTES &&
            password_length <= NETWORK_WIFI_PASSWORD_MAX_BYTES,
        ESP_ERR_INVALID_ARG, TAG,
        "Wi-Fi password must contain 8 to 63 bytes");

    wifi_config_t wifi_config = {0};
    memcpy(wifi_config.sta.ssid, CONFIG_PET_WIFI_SSID, ssid_length);
    memcpy(wifi_config.sta.password, CONFIG_PET_WIFI_PASSWORD,
           password_length);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG,
                        "Failed to set Wi-Fi STA mode");
    ESP_RETURN_ON_ERROR(
        esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG,
        "Failed to apply Wi-Fi credentials");

    atomic_store(&s_started, true);
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) {
        atomic_store(&s_started, false);
        ESP_LOGE(TAG, "Failed to start Wi-Fi driver: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to enable Wi-Fi Modem-sleep: %s",
                 esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "Wi-Fi STA started for SSID '%s'",
             CONFIG_PET_WIFI_SSID);
    return ESP_OK;
}

bool network_manager_is_connected(void)
{
    return atomic_load(&s_connected);
}
