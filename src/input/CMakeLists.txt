# board_buttons.c 实现板载按键驱动，include/input/ 保存供应用层调用的接口。
# 组件只依赖统一板级配置和 ESP-IDF GPIO 驱动。
idf_component_register(
    SRCS "board_buttons.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES board esp_driver_gpio
)
