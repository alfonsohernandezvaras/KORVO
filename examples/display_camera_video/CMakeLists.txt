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
set(PROJECT_VER "1.3.18")

project(display_camera_new)

# ESP-IDF 6.1 + GCC 15.x: Bluedroid in this IDF release emits
# -Wstringop-truncation from framework source (bta_dm_act.c).
# The generic CONFIG_COMPILER_DISABLE_DEFAULT_ERRORS switch changes -Werror
# to -Werror=all but GCC 15 still promotes this diagnostic to an error.
# Append the warning-specific override AFTER project(), as required by the
# ESP-IDF build system for global compile option overrides.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=stringop-truncation" APPEND)

# ESP-IDF 6.1 / Bluedroid HFP AG on ESP32-S31:
# the computed security service-record table is only 3 entries in this
# configuration.  An outgoing AG -> HF RFCOMM/SLC connection needs an
# additional security record and otherwise fails with:
#   BTM_SEC_REG: Out of Service Records (3)
#   Security Manager: MX service not found PSM:3 Proto:3 SCN:x
# Keep a small explicit margin for HFP/HSP/SDP security records.
idf_build_set_property(COMPILE_OPTIONS "-DBTM_SEC_MAX_SERVICE_RECORDS=8" APPEND)
