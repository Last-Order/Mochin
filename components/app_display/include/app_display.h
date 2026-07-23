#pragma once

#include "esp_err.h"

typedef enum {
    APP_DISPLAY_ACCENT_YELLOW = 0,
    APP_DISPLAY_ACCENT_MAGENTA,
    APP_DISPLAY_ACCENT_GREEN,
    APP_DISPLAY_ACCENT_COUNT,
} app_display_accent_t;

/**
 * @brief Initialize the ST7789 display and show the initial button prompt.
 */
esp_err_t app_display_init(void);

/**
 * @brief Restore the initial "PRESS A BUTTON" screen.
 */
esp_err_t app_display_show_prompt(void);

/**
 * @brief Show a pressed button name using the selected accent color.
 */
esp_err_t app_display_show_button(const char *button_name,
                                  app_display_accent_t accent);
