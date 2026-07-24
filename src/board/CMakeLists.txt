# board 是一个“仅头文件组件”，没有需要编译的 .c 文件。
# INCLUDE_DIRS 让其他组件可以通过 #include "board/board_config.h"
# 使用板级配置，并通过 board/ 前缀避免与其他组件的头文件重名。
# REQUIRES 会把 GPIO 和 SPI 驱动的头文件传递给依赖 board 的组件。
idf_component_register(
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_driver_spi
)
