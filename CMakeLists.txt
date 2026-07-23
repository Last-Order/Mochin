cmake_minimum_required(VERSION 3.16)

set(IDF_TARGET esp32s3 CACHE STRING "ESP-IDF target")
set(SUPPORTED_TARGETS esp32s3)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_s3_touch_lcd_154_button_display)
