#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
#define KORVO_WIFI_DEFAULT_SSID       "korvo_wifi"
#define KORVO_WIFI_DEFAULT_PASSWORD   "korvo_wifi"
#define KORVO_WIFI_DEFAULT_IP         "192.168.5.50"
#define KORVO_WIFI_DEFAULT_NETMASK    "255.255.255.0"
#define KORVO_GATEWAY_DEFAULT_IP      "192.168.5.51"
typedef struct { char ssid[33]; char password[65]; char ip[16]; char netmask[16]; char gateway_node_ip[16]; bool ssid_visible; } korvo_network_config_t;
typedef struct { bool started; uint16_t connected_clients; uint8_t mac[6]; korvo_network_config_t config; } korvo_network_status_t;
esp_err_t korvo_network_init(void);
esp_err_t korvo_network_get_status(korvo_network_status_t *status);
esp_err_t korvo_network_get_config(korvo_network_config_t *config);
esp_err_t korvo_network_save_config(const korvo_network_config_t *config);
esp_err_t korvo_network_factory_defaults(void);
#ifdef __cplusplus
}
#endif
