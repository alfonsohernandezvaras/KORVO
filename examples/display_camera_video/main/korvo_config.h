#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KORVO_CONFIG_VERSION 1

typedef struct {
    char device_id[32];
    char room_id[32];
    char hostname[32];

    char wifi_ssid[33];
    char wifi_password[65];

    char ip[16];
    char netmask[16];
    char gateway_ip[16];
    uint16_t web_port;

    char gateway_host[16];
    uint16_t gateway_api_port;

    char sip_server[64];
    uint16_t sip_port;
    char sip_extension[32];
    char sip_user[32];
    char sip_password[64];
    char sip_operator[32];
    uint16_t rtp_port_start;
    uint16_t rtp_port_end;

    uint8_t face_enabled;
    uint8_t rfid_enabled;
    uint8_t sip_enabled;
    uint8_t gateway_enabled;
} korvo_config_t;

void korvo_config_set_defaults(korvo_config_t *cfg);
esp_err_t korvo_config_init(void);
esp_err_t korvo_config_load(korvo_config_t *cfg);
esp_err_t korvo_config_save(const korvo_config_t *cfg);
esp_err_t korvo_config_factory_reset(void);
const korvo_config_t *korvo_config_get(void);

#ifdef __cplusplus
}
#endif
