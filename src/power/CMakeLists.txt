# 电源组件管理电池供电保持和 PWR 长按关机，不依赖显示或页面组件。
idf_component_register(
    SRCS "power_manager.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        board
        esp_driver_gpio
        freertos
)
