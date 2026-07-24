# 编译显示实现，并把 include/display/ 中的公开接口暴露给 app 组件。
# PRIV_REQUIRES 表示这些依赖只用于 display 内部，不构成公开 API。
idf_component_register(
    SRCS "app_display.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES board esp_lcd esp_driver_gpio esp_driver_spi
)
