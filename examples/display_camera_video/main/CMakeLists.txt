idf_component_register(SRCS "main.c" "app_video.c" "face_detect.cpp" "korvo_config.c" "korvo_storage.c" "korvo_hmi.c" "korvo_network.c" "korvo_web.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_ppa nvs_flash esp_wifi esp_netif esp_event esp_http_server esp_timer esp_app_format)
