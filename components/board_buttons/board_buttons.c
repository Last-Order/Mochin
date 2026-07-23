#include "board_buttons.h"

#include <stdbool.h>
#include <stddef.h>

#include "board_config.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BUTTON_POLL_INTERVAL_MS 10
#define BUTTON_DEBOUNCE_MS 30
#define BUTTON_TASK_STACK_SIZE 4096
#define BUTTON_TASK_PRIORITY 5

static const char *TAG = "board_buttons";

typedef struct {
    board_button_id_t id;
    gpio_num_t gpio_num;
    const char *name;
    int sampled_level;
    int stable_level;
    TickType_t sample_changed_at;
} button_state_t;

static button_state_t s_buttons[] = {
    {
        .id = BOARD_BUTTON_BOOT,
        .gpio_num = BOARD_BUTTON_PIN_BOOT,
        .name = "BOOT",
    },
    {
        .id = BOARD_BUTTON_PWR,
        .gpio_num = BOARD_BUTTON_PIN_PWR,
        .name = "PWR",
    },
    {
        .id = BOARD_BUTTON_PLUS,
        .gpio_num = BOARD_BUTTON_PIN_PLUS,
        .name = "PLUS",
    },
};

static bool s_initialized;
static TaskHandle_t s_task;
static board_button_pressed_cb_t s_callback;
static void *s_callback_context;

static void board_buttons_task(void *arg)
{
    (void)arg;
    const TickType_t debounce_ticks = pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS);

    while (true) {
        const TickType_t now = xTaskGetTickCount();

        for (size_t i = 0; i < sizeof(s_buttons) / sizeof(s_buttons[0]);
             ++i) {
            button_state_t *button = &s_buttons[i];
            const int level = gpio_get_level(button->gpio_num);

            if (level != button->sampled_level) {
                button->sampled_level = level;
                button->sample_changed_at = now;
                continue;
            }

            if (level == button->stable_level ||
                now - button->sample_changed_at < debounce_ticks) {
                continue;
            }

            button->stable_level = level;
            if (level == 0) {
                ESP_LOGI(TAG, "%s button pressed (GPIO%d)", button->name,
                         button->gpio_num);
                s_callback(button->id, s_callback_context);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_INTERVAL_MS));
    }
}

esp_err_t board_buttons_init(void)
{
    ESP_RETURN_ON_FALSE(!s_initialized, ESP_ERR_INVALID_STATE, TAG,
                        "Buttons are already initialized");

    const gpio_config_t button_config = {
        .pin_bit_mask = (1ULL << BOARD_BUTTON_PIN_BOOT) |
                        (1ULL << BOARD_BUTTON_PIN_PWR) |
                        (1ULL << BOARD_BUTTON_PIN_PLUS),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&button_config), TAG,
                        "Failed to configure button GPIOs");

    const TickType_t now = xTaskGetTickCount();
    for (size_t i = 0; i < sizeof(s_buttons) / sizeof(s_buttons[0]); ++i) {
        const int level = gpio_get_level(s_buttons[i].gpio_num);
        s_buttons[i].sampled_level = level;
        s_buttons[i].stable_level = level;
        s_buttons[i].sample_changed_at = now;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Buttons initialized: BOOT=GPIO%d, PWR=GPIO%d, "
                  "PLUS=GPIO%d",
             BOARD_BUTTON_PIN_BOOT, BOARD_BUTTON_PIN_PWR,
             BOARD_BUTTON_PIN_PLUS);
    return ESP_OK;
}

esp_err_t board_buttons_start(board_button_pressed_cb_t callback,
                              void *user_ctx)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG,
                        "Buttons are not initialized");
    ESP_RETURN_ON_FALSE(s_task == NULL, ESP_ERR_INVALID_STATE, TAG,
                        "Button task is already running");
    ESP_RETURN_ON_FALSE(callback != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "Button callback must not be NULL");

    s_callback = callback;
    s_callback_context = user_ctx;

    BaseType_t task_created = xTaskCreate(
        board_buttons_task, "board_buttons", BUTTON_TASK_STACK_SIZE, NULL,
        BUTTON_TASK_PRIORITY, &s_task);
    if (task_created != pdPASS) {
        s_callback = NULL;
        s_callback_context = NULL;
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Button monitoring task started");
    return ESP_OK;
}

const char *board_button_get_name(board_button_id_t button)
{
    for (size_t i = 0; i < sizeof(s_buttons) / sizeof(s_buttons[0]); ++i) {
        if (s_buttons[i].id == button) {
            return s_buttons[i].name;
        }
    }
    return "UNKNOWN";
}
