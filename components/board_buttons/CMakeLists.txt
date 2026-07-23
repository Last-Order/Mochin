idf_component_register(
    SRCS "board_buttons.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES board esp_driver_gpio
)
