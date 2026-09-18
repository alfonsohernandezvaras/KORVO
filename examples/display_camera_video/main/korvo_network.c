#include "korvo_network.h"
#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "korvo_network";
static const char *NVS_NS = "korvo_net";
static bool s_started;
static uint16_t s_clients;
static esp_netif_t *s_ap_netif;
static korvo_network_config_t s_cfg;

static void defaults(korvo_network_config_t *c) {
    memset(c, 0, sizeof(*c));
    strlcpy(c->ssid, KORVO_WIFI_DEFAULT_SSID, sizeof(c->ssid));
    strlcpy(c->password, KORVO_WIFI_DEFAULT_PASSWORD, sizeof(c->password));
    strlcpy(c->ip, KORVO_WIFI_DEFAULT_IP, sizeof(c->ip));
    strlcpy(c->netmask, KORVO_WIFI_DEFAULT_NETMASK, sizeof(c->netmask));
    strlcpy(c->gateway_node_ip, KORVO_GATEWAY_DEFAULT_IP, sizeof(c->gateway_node_ip));
    c->ssid_visible = true;
}
static bool valid_ip(const char *s) { esp_ip4_addr_t a; return s && esp_netif_str_to_ip4(s, &a) == ESP_OK; }
static bool valid_cfg(const korvo_network_config_t *c) {
    size_t n = c ? strlen(c->password) : 0;
    return c && c->ssid[0] && (n == 0 || (n >= 8 && n <= 63)) && valid_ip(c->ip) && valid_ip(c->netmask) && valid_ip(c->gateway_node_ip);
}
static esp_err_t load_cfg(korvo_network_config_t *c) {
    defaults(c); nvs_handle_t h; esp_err_t e = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (e == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (e != ESP_OK) {
        return e;
    }
    size_t n; uint8_t v = 1;
    n=sizeof(c->ssid); (void)nvs_get_str(h,"ssid",c->ssid,&n);
    n=sizeof(c->password); (void)nvs_get_str(h,"pass",c->password,&n);
    n=sizeof(c->ip); (void)nvs_get_str(h,"ip",c->ip,&n);
    n=sizeof(c->netmask); (void)nvs_get_str(h,"mask",c->netmask,&n);
    n=sizeof(c->gateway_node_ip); (void)nvs_get_str(h,"gw",c->gateway_node_ip,&n);
    (void)nvs_get_u8(h,"visible",&v); c->ssid_visible = v != 0; nvs_close(h);
    if (!valid_cfg(c)) {
        defaults(c);
    }
    return ESP_OK;
}
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base != WIFI_EVENT) return;
    if (id == WIFI_EVENT_AP_STACONNECTED) { if (s_clients < UINT16_MAX) s_clients++; wifi_event_ap_staconnected_t *e=data; ESP_LOGI(TAG,"STA " MACSTR " connected, clients=%u",MAC2STR(e->mac),(unsigned)s_clients); }
    else if (id == WIFI_EVENT_AP_STADISCONNECTED) { if (s_clients) s_clients--; wifi_event_ap_stadisconnected_t *e=data; ESP_LOGI(TAG,"STA " MACSTR " disconnected, clients=%u",MAC2STR(e->mac),(unsigned)s_clients); }
}
esp_err_t korvo_network_init(void) {
    if (s_started) return ESP_OK;
    esp_err_t e=nvs_flash_init(); if(e==ESP_ERR_NVS_NO_FREE_PAGES||e==ESP_ERR_NVS_NEW_VERSION_FOUND){ ESP_ERROR_CHECK(nvs_flash_erase()); e=nvs_flash_init(); } if(e!=ESP_OK)return e;
    if(load_cfg(&s_cfg)!=ESP_OK) defaults(&s_cfg);
    e=esp_netif_init(); if(e!=ESP_OK&&e!=ESP_ERR_INVALID_STATE)return e;
    e=esp_event_loop_create_default(); if(e!=ESP_OK&&e!=ESP_ERR_INVALID_STATE)return e;
    s_ap_netif=esp_netif_create_default_wifi_ap(); if(!s_ap_netif)return ESP_FAIL;
    esp_netif_ip_info_t ip={0}; if(esp_netif_str_to_ip4(s_cfg.ip,&ip.ip)!=ESP_OK||esp_netif_str_to_ip4(s_cfg.netmask,&ip.netmask)!=ESP_OK)return ESP_ERR_INVALID_ARG; ip.gw=ip.ip;
    e=esp_netif_dhcps_stop(s_ap_netif); if(e!=ESP_OK&&e!=ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED)return e;
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_ap_netif,&ip)); ESP_ERROR_CHECK(esp_netif_dhcps_start(s_ap_netif));
    wifi_init_config_t wi=WIFI_INIT_CONFIG_DEFAULT(); ESP_ERROR_CHECK(esp_wifi_init(&wi)); ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,wifi_event,NULL));
    wifi_config_t ap={0}; strlcpy((char*)ap.ap.ssid,s_cfg.ssid,sizeof(ap.ap.ssid)); strlcpy((char*)ap.ap.password,s_cfg.password,sizeof(ap.ap.password)); ap.ap.ssid_len=strlen(s_cfg.ssid); ap.ap.channel=1; ap.ap.max_connection=8; ap.ap.ssid_hidden=s_cfg.ssid_visible?0:1; ap.ap.authmode=strlen(s_cfg.password)?WIFI_AUTH_WPA2_PSK:WIFI_AUTH_OPEN; ap.ap.pmf_cfg.required=false;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP)); ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP,&ap)); ESP_ERROR_CHECK(esp_wifi_start()); s_started=true;
    ESP_LOGI(TAG,"AP READY ssid=%s visible=%s ip=%s gateway-node=%s",s_cfg.ssid,s_cfg.ssid_visible?"yes":"no",s_cfg.ip,s_cfg.gateway_node_ip); return ESP_OK;
}
esp_err_t korvo_network_get_status(korvo_network_status_t *s) { if(!s)return ESP_ERR_INVALID_ARG; memset(s,0,sizeof(*s)); s->started=s_started; s->connected_clients=s_clients; s->config=s_cfg; if(s_started)(void)esp_wifi_get_mac(WIFI_IF_AP,s->mac); return ESP_OK; }
esp_err_t korvo_network_get_config(korvo_network_config_t *c) { if(!c)return ESP_ERR_INVALID_ARG; *c=s_cfg; return ESP_OK; }
esp_err_t korvo_network_save_config(const korvo_network_config_t *c) {
    if (!valid_cfg(c)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) {
        return e;
    }
    if((e=nvs_set_str(h,"ssid",c->ssid))==ESP_OK&&(e=nvs_set_str(h,"pass",c->password))==ESP_OK&&(e=nvs_set_str(h,"ip",c->ip))==ESP_OK&&(e=nvs_set_str(h,"mask",c->netmask))==ESP_OK&&(e=nvs_set_str(h,"gw",c->gateway_node_ip))==ESP_OK&&(e=nvs_set_u8(h,"visible",c->ssid_visible?1:0))==ESP_OK)e=nvs_commit(h);
    nvs_close(h); if(e==ESP_OK)s_cfg=*c; return e;
}
esp_err_t korvo_network_factory_defaults(void) { korvo_network_config_t c; defaults(&c); return korvo_network_save_config(&c); }
