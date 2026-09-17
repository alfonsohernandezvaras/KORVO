idf_component_register(SRCS "main.c" "app_video.c" "face_detect.cpp" "korvo_config.c" "korvo_storage.c" "korvo_hmi.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_ppa nvs_flash)
