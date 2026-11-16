cmake_minimum_required(VERSION 3.16)

# The ESP32 firmware is a standalone ESP-IDF project; do not include the
# repository-root Linux Gateway CMake project here.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(dockstation_esp32_firmware)
