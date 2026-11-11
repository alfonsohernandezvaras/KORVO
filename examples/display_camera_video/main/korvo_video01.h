/* KORVO VIDEO-02: API compartida, sin SIP. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_http_server.h"
#ifdef __cplusplus
extern "C" {
#endif
bool korvo_esp_dl_enabled(void);
void korvo_esp_dl_set_enabled(bool enabled);
bool korvo_video01_ready(void);
void korvo_video01_capture(const uint8_t *p, uint32_t w, uint32_t h, size_t len);
void korvo_video01_register(httpd_handle_t server);
/* Administrative reset from paired Gateway2 or local WebServer. */
bool korvo_video01_admin_reset(void);
#ifdef __cplusplus
}
#endif
