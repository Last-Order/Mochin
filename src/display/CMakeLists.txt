# 显示组件只初始化硬件和 LVGL 端口，不持有具体页面对象。
# 第三方组件的精确版本统一锁定在同目录的 idf_component.yml 中。
idf_component_register(
    SRCS "lcd_display.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        board
        esp_lcd
        esp_driver_gpio
        esp_driver_spi
        esp_lvgl_port
)
