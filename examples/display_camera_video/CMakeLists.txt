# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
#
# KORVO - Root CMakeLists.txt

cmake_minimum_required(VERSION 3.16)

# Trim build: main + dependencias requeridas por main.
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Version embebida en esp_app_desc_t.
# El sistema OTA KORVO utiliza esta versión para validar
# actualizaciones, reinstalaciones y downgrades.
set(PROJECT_VER "1.0.0")

project(display_camera_new)