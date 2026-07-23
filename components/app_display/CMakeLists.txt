idf_component_register(
    SRCS "app_display.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES board esp_lcd esp_driver_gpio esp_driver_spi
)
