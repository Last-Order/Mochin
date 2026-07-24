/*
 * Waveshare ESP32-S3-Touch-LCD-1.54 电池供电管理。
 *
 * PWR 按键只能临时建立电池到 VSYS 的供电路径，ESP32-S3 启动后必须尽快
 * 把 BAT_EN(GPIO2) 拉高完成自保持。本组件还轮询 KEY_PWR(GPIO5)，在确认
 * 开机按键已经释放后，允许用户再次长按来关闭背光并切断电池供电。
 */

#include "power/power_manager.h"

#include <stdbool.h>
#include <stdint.h>

#include "board/board_config.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define POWER_BUTTON_POLL_INTERVAL_MS 20
#define POWER_BUTTON_DEBOUNCE_MS 40
#define POWER_BUTTON_SHUTDOWN_HOLD_MS 2000
#define POWER_SHUTDOWN_SETTLE_MS 100
#define POWER_MONITOR_TASK_STACK_SIZE 3072
#define POWER_MONITOR_TASK_PRIORITY (tskIDLE_PRIORITY + 1)

static const char *TAG = "power_manager";

typedef struct {
    TaskHandle_t monitor_task;
    power_manager_shutdown_callback_t shutdown_callback;
    void *shutdown_context;
    bool initialized;
    bool monitoring;
} power_manager_context_t;

static power_manager_context_t s_power;

/**
 * @brief 执行应用关机准备，并撤销电池供电保持。
 *
 * 拉低 BAT_EN 后，PWR 仍按住时硬件会暂时继续供电；用户松开 PWR 后才会
 * 真正断电。若 USB 仍连接，开发板会继续由 USB 供电，监控任务在这里
 * 挂起，避免重复触发关机流程。
 */
static void power_manager_power_off(void)
{
    ESP_LOGI(TAG, "Long press detected; preparing to power off");

    if (s_power.shutdown_callback != NULL) {
        const esp_err_t callback_result =
            s_power.shutdown_callback(s_power.shutdown_context);
        if (callback_result != ESP_OK) {
            ESP_LOGW(TAG, "Shutdown callback failed: %s",
                     esp_err_to_name(callback_result));
        }
    }

    /*
     * 给日志和外设关闭动作留出很短的收尾时间。BAT_EN 此时仍为高，哪怕
     * 用户已经松开按键也不会在回调尚未返回时突然掉电。
     */
    vTaskDelay(pdMS_TO_TICKS(POWER_SHUTDOWN_SETTLE_MS));
    ESP_LOGI(TAG, "Releasing battery power hold");

    const esp_err_t result =
        gpio_set_level(BOARD_POWER_HOLD_PIN, 0);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to release battery power hold: %s",
                 esp_err_to_name(result));
        return;
    }

    /*
     * 电池模式下执行到这里后通常会随 PWR 松开而掉电。USB 模式下仍有
     * 外部供电，因此主动挂起任务，保持 BAT_EN 为低且不重复处理按键。
     */
    vTaskSuspend(NULL);
}

/**
 * @brief PWR 按键去抖和长按状态机。
 *
 * 初始检测到低电平时先禁止关机，直到稳定检测到一次释放。这样用户为
 * 开机而持续按住 PWR 时，不会在启动两秒后又被本任务关闭。
 */
static void power_manager_monitor_task(void *argument)
{
    (void)argument;

    bool raw_pressed =
        gpio_get_level(BOARD_BUTTON_PIN_PWR) == 0;
    bool stable_pressed = raw_pressed;
    bool shutdown_armed = !stable_pressed;
    bool tracking_press = false;
    TickType_t raw_changed_at = xTaskGetTickCount();
    TickType_t pressed_at = 0;

    if (!shutdown_armed) {
        ESP_LOGI(TAG, "Waiting for the startup PWR press to be released");
    }

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POWER_BUTTON_POLL_INTERVAL_MS));

        const TickType_t now = xTaskGetTickCount();
        const bool current_pressed =
            gpio_get_level(BOARD_BUTTON_PIN_PWR) == 0;

        if (current_pressed != raw_pressed) {
            raw_pressed = current_pressed;
            raw_changed_at = now;
        }

        if (raw_pressed != stable_pressed &&
            now - raw_changed_at >=
                pdMS_TO_TICKS(POWER_BUTTON_DEBOUNCE_MS)) {
            stable_pressed = raw_pressed;

            if (stable_pressed) {
                if (shutdown_armed) {
                    pressed_at = now;
                    tracking_press = true;
                }
            } else {
                if (!shutdown_armed) {
                    ESP_LOGI(TAG, "PWR released; shutdown is now armed");
                }
                shutdown_armed = true;
                tracking_press = false;
            }
        }

        if (shutdown_armed && stable_pressed && tracking_press &&
            now - pressed_at >=
                pdMS_TO_TICKS(POWER_BUTTON_SHUTDOWN_HOLD_MS)) {
            power_manager_power_off();
            tracking_press = false;
        }
    }
}

esp_err_t power_manager_init(void)
{
    ESP_RETURN_ON_FALSE(!s_power.initialized, ESP_ERR_INVALID_STATE, TAG,
                        "Power manager is already initialized");

    /*
     * 先预装高电平，再把引脚切换为输出，可避免 GPIO 模式切换过程中出现
     * 一个短暂低脉冲而提前释放电池供电路径。
     */
    ESP_RETURN_ON_ERROR(
        gpio_set_level(BOARD_POWER_HOLD_PIN, 1), TAG,
        "Failed to preload battery power hold level");

    const gpio_config_t hold_config = {
        .pin_bit_mask = 1ULL << BOARD_POWER_HOLD_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(
        gpio_config(&hold_config), TAG,
        "Failed to configure battery power hold GPIO");
    ESP_RETURN_ON_ERROR(
        gpio_set_level(BOARD_POWER_HOLD_PIN, 1), TAG,
        "Failed to enable battery power hold");

    const gpio_config_t button_config = {
        .pin_bit_mask = 1ULL << BOARD_BUTTON_PIN_PWR,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(
        gpio_config(&button_config), TAG,
        "Failed to configure PWR button GPIO");

    s_power.initialized = true;
    ESP_LOGI(TAG, "Battery power hold enabled on GPIO%d",
             BOARD_POWER_HOLD_PIN);
    return ESP_OK;
}

esp_err_t power_manager_start(
    power_manager_shutdown_callback_t callback, void *context)
{
    ESP_RETURN_ON_FALSE(s_power.initialized, ESP_ERR_INVALID_STATE, TAG,
                        "Power manager is not initialized");
    ESP_RETURN_ON_FALSE(!s_power.monitoring, ESP_ERR_INVALID_STATE, TAG,
                        "Power monitoring is already started");

    s_power.shutdown_callback = callback;
    s_power.shutdown_context = context;

    const BaseType_t task_result = xTaskCreate(
        power_manager_monitor_task, "power_monitor",
        POWER_MONITOR_TASK_STACK_SIZE, NULL,
        POWER_MONITOR_TASK_PRIORITY, &s_power.monitor_task);
    if (task_result != pdPASS) {
        s_power.shutdown_callback = NULL;
        s_power.shutdown_context = NULL;
        s_power.monitor_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_power.monitoring = true;
    ESP_LOGI(TAG, "PWR long-press shutdown monitor started (%d ms)",
             POWER_BUTTON_SHUTDOWN_HOLD_MS);
    return ESP_OK;
}
