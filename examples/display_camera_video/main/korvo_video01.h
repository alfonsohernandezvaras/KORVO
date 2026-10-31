/* KORVO VIDEO-02: API compartida, sin SIP. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_http_server.h"
#ifdef __cplusplus
extern "C" {
#endif
void korvo_video01_capture(const uint8_t *p, uint32_t w, uint32_t h, size_t len);
void korvo_video01_register(httpd_handle_t server);
#ifdef __cplusplus
}
#endif
