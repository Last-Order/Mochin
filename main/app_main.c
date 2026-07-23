#include "app_display.h"
#include "board_buttons.h"
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "button_display";

static const app_display_accent_t BUTTON_ACCENTS[BOARD_BUTTON_COUNT] = {
    [BOARD_BUTTON_BOOT] = APP_DISPLAY_ACCENT_YELLOW,
    [BOARD_BUTTON_PWR] = APP_DISPLAY_ACCENT_MAGENTA,
    [BOARD_BUTTON_PLUS] = APP_DISPLAY_ACCENT_GREEN,
};

static void on_button_pressed(board_button_id_t button, void *user_ctx)
{
    (void)user_ctx;

    if (button < 0 || button >= BOARD_BUTTON_COUNT) {
        ESP_LOGW(TAG, "Ignoring unknown button id %d", button);
        return;
    }

    const char *name = board_button_get_name(button);
    esp_err_t ret = app_display_show_button(name, BUTTON_ACCENTS[button]);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to display %s button: %s", name,
                 esp_err_to_name(ret));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Initializing Waveshare ESP32-S3-Touch-LCD-1.54");

    ESP_ERROR_CHECK(app_display_init());
    ESP_ERROR_CHECK(board_buttons_init());
    ESP_ERROR_CHECK(board_buttons_start(on_button_pressed, NULL));

    ESP_LOGI(TAG, "Application is ready");
}
