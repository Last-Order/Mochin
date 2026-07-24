# 编译显示实现，并把 include/display/ 中的公开接口暴露给 app 组件。
# esp_lvgl_port 和 LVGL 只在 display 内部使用，不会泄漏到 app 的公开接口。
# 第三方组件的精确版本统一锁定在同目录的 idf_component.yml 中。
idf_component_register(
    SRCS "app_display.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        board
        esp_lcd
        esp_driver_gpio
        esp_driver_spi
        esp_lvgl_port
)
