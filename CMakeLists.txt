cmake_minimum_required(VERSION 3.16)

set(COMPONENTS main bt nvs_flash)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp_ble)