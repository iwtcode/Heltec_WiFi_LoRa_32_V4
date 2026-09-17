cmake_minimum_required(VERSION 3.16.0)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(Heltec_WiFi_LoRa_32_V4)

target_add_binary_data(${CMAKE_PROJECT_NAME}.elf "src/web/index.html" TEXT)