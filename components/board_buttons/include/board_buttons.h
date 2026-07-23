#pragma once

#include "esp_err.h"

typedef enum {
    BOARD_BUTTON_BOOT = 0,
    BOARD_BUTTON_PWR,
    BOARD_BUTTON_PLUS,
    BOARD_BUTTON_COUNT,
} board_button_id_t;

typedef void (*board_button_pressed_cb_t)(board_button_id_t button,
                                          void *user_ctx);

/**
 * @brief Configure the three active-low onboard buttons.
 */
esp_err_t board_buttons_init(void);

/**
 * @brief Start the debounced button polling task.
 */
esp_err_t board_buttons_start(board_button_pressed_cb_t callback,
                              void *user_ctx);

/**
 * @brief Return the silkscreen name for a board button.
 */
const char *board_button_get_name(board_button_id_t button);
