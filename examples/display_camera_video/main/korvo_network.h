#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KORVO_WIFI_DEFAULT_SSID          "korvo_wifi"
#define KORVO_WIFI_DEFAULT_PASSWORD      "korvo_gateway"

#define KORVO_WIFI_DEFAULT_IP            "192.168.10.50"
#define KORVO_WIFI_DEFAULT_NETMASK       "255.255.255.0"
#define KORVO_WIFI_DEFAULT_GATEWAY       "192.168.10.1"
#define KORVO_WIFI_DEFAULT_DNS           "192.168.10.1"

/* Solo sugerencia inicial para el enrolamiento de laboratorio.
 * El control real usa exclusivamente expected_gateway.ip guardado por
 * korvo_gateway tras un ENROLL exitoso. */
#define KORVO_GATEWAY_DEFAULT_IP         "192.168.10.51"

typedef struct
{
    char ssid[33];
    char password[65];

    char ip[16];
    char netmask[16];
    char network_gateway[16];
    char dns[16];

    /* Legacy/UI hint only. Never used as trusted peer after enrollment. */
    char gateway_node_ip[16];

    /*
     * Se conserva por compatibilidad con configuraciones anteriores.
     * En modo STA no controla la visibilidad del SSID.
     */
    bool ssid_visible;
} korvo_network_config_t;

typedef struct
{
    bool started;
    bool connected;
    uint16_t connected_clients;
    uint8_t mac[6];
    korvo_network_config_t config;
} korvo_network_status_t;

esp_err_t korvo_network_init(void);
esp_err_t korvo_network_get_status(korvo_network_status_t *status);
esp_err_t korvo_network_get_config(korvo_network_config_t *config);
esp_err_t korvo_network_save_config(const korvo_network_config_t *config);
esp_err_t korvo_network_factory_defaults(void);

#ifdef __cplusplus
}
#endif
