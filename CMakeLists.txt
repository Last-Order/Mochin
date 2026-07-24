cmake_minimum_required(VERSION 3.16)

# 本工程只支持 ESP32-S3。把目标写在根 CMake 中，可以避免误编译到其他芯片。
set(IDF_TARGET esp32s3 CACHE STRING "ESP-IDF target")
set(SUPPORTED_TARGETS esp32s3)

# ESP-IDF 默认会扫描根目录的 main/ 和 components/。
# 本项目把自有代码统一放在 src/，因此在加载 project.cmake 之前把它加入
# 额外组件搜索目录。src/ 下每个带 CMakeLists.txt 的一级目录都是独立组件。
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/src")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_s3_touch_lcd_154_button_display)
