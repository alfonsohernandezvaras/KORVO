#include "korvo_network.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/timers.h"

static const char *TAG = "korvo_network";
static const char *NVS_NS = "korvo_net";

static bool s_started;
static bool s_connected;
static esp_netif_t *s_sta_netif;
static korvo_network_config_t s_cfg;
static uint8_t s_reconnect_attempts;
static uint16_t s_last_disconnect_reason;
static uint32_t s_disconnect_count;

/* V21.22 WIFI AUTO RECOVERY QUIET
 * One-shot timer exists only to wake a reconnect attempt while disconnected.
 * No ping, no active scan API, no gateway traffic.
 */
static TimerHandle_t s_wifi_recovery_timer;
#define KORVO_WIFI_RECOVERY_MS 10000

static void wifi_recovery_timer_cb(TimerHandle_t timer)
{
    (void)timer;

    if (!s_started || s_connected) {
        return;
    }

    ESP_LOGI(TAG,
             "Wi-Fi RECOVERY QUIET: buscando SSID configurado mediante reconnect");

    esp_err_t e = esp_wifi_connect();
    if (e != ESP_OK) {
        ESP_LOGW(TAG,
                 "Wi-Fi RECOVERY connect fallo: %s",
                 esp_err_to_name(e));

        if (s_wifi_recovery_timer) {
            (void)xTimerStart(s_wifi_recovery_timer, 0);
        }
    }
}

static void wifi_recovery_arm(void)
{
    if (!s_wifi_recovery_timer) {
        s_wifi_recovery_timer =
            xTimerCreate("wifi_recovery",
                         pdMS_TO_TICKS(KORVO_WIFI_RECOVERY_MS),
                         pdFALSE,
                         NULL,
                         wifi_recovery_timer_cb);
    }

    if (s_wifi_recovery_timer) {
        (void)xTimerStop(s_wifi_recovery_timer, 0);
        (void)xTimerStart(s_wifi_recovery_timer, 0);
    }
}

static void wifi_recovery_cancel(void)
{
    if (s_wifi_recovery_timer) {
        (void)xTimerStop(s_wifi_recovery_timer, 0);
    }
}
#define KORVO_WIFI_QUIET_RECONNECT_MAX 3

static void defaults(korvo_network_config_t *c)
{
    memset(c, 0, sizeof(*c));

    strlcpy(
        c->ssid,
        KORVO_WIFI_DEFAULT_SSID,
        sizeof(c->ssid));

    strlcpy(
        c->password,
        KORVO_WIFI_DEFAULT_PASSWORD,
        sizeof(c->password));

    strlcpy(
        c->ip,
        KORVO_WIFI_DEFAULT_IP,
        sizeof(c->ip));

    strlcpy(
        c->netmask,
        KORVO_WIFI_DEFAULT_NETMASK,
        sizeof(c->netmask));

    strlcpy(
        c->network_gateway,
        KORVO_WIFI_DEFAULT_GATEWAY,
        sizeof(c->network_gateway));

    strlcpy(
        c->dns,
        KORVO_WIFI_DEFAULT_DNS,
        sizeof(c->dns));

    strlcpy(
        c->gateway_node_ip,
        KORVO_GATEWAY_DEFAULT_IP,
        sizeof(c->gateway_node_ip));

    c->ssid_visible = true;
}

static bool valid_ip(const char *s)
{
    esp_ip4_addr_t a;

    return
        s &&
        esp_netif_str_to_ip4(
            s,
            &a) == ESP_OK;
}

static bool valid_cfg(const korvo_network_config_t *c)
{
    size_t n =
        c
            ? strlen(c->password)
            : 0;

    return
        c &&
        c->ssid[0] &&
        (n == 0 || (n >= 8 && n <= 63)) &&
        valid_ip(c->ip) &&
        valid_ip(c->netmask) &&
        valid_ip(c->network_gateway) &&
        (!c->dns[0] || valid_ip(c->dns)) &&
        valid_ip(c->gateway_node_ip);
}


static esp_err_t load_cfg(korvo_network_config_t *c)
{
    /* KORVO_V21_23_RESTORE_NVS_WIFI
     * Los valores compilados son solo defaults. La fuente persistente
     * es NVS 'korvo_net', las mismas claves de save_config().
     */
    if (!c) return ESP_ERR_INVALID_ARG;
    defaults(c);

    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (e == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "Wi-Fi NVS sin configuracion: usando defaults");
        return ESP_OK;
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi NVS no disponible: %s", esp_err_to_name(e));
        return e;
    }

    korvo_network_config_t saved = *c;
    size_t n;
    bool any_saved = false;
    bool bad_saved = false;

#define READ_NVS_STR(key, field) do {                                   \
        n = sizeof(saved.field);                                        \
        esp_err_t r = nvs_get_str(h, (key), saved.field, &n);           \
        if (r == ESP_OK) any_saved = true;                              \
        else if (r != ESP_ERR_NVS_NOT_FOUND) {                          \
            ESP_LOGW(TAG, "NVS Wi-Fi clave %s: %s", (key),               \
                     esp_err_to_name(r));                               \
            bad_saved = true;                                           \
        }                                                               \
    } while (0)

    READ_NVS_STR("ssid", ssid);
    READ_NVS_STR("pass", password);
    READ_NVS_STR("ip", ip);
    READ_NVS_STR("mask", netmask);
    READ_NVS_STR("netgw", network_gateway);
    READ_NVS_STR("dns", dns);
    READ_NVS_STR("node", gateway_node_ip);
#undef READ_NVS_STR

    nvs_close(h);

    if (bad_saved || !valid_cfg(&saved)) {
        ESP_LOGE(TAG, "Wi-Fi NVS invalida: no se reemplaza por una configuracion parcial");
        return ESP_ERR_INVALID_ARG;
    }

    *c = saved;
    ESP_LOGI(TAG, "Wi-Fi CFG origen=%s ssid=%s ip=%s gw=%s gateway-io=%s",
             any_saved ? "NVS" : "DEFAULT",
             c->ssid, c->ip, c->network_gateway, c->gateway_node_ip);
    return ESP_OK;
}

static void wifi_event(
    void *arg,
    esp_event_base_t base,
    int32_t id,
    void *data)
{
    (void)arg;
    if (base != WIFI_EVENT) {
        return;
    }

    if (id == WIFI_EVENT_STA_START) {

        ESP_LOGI(
            TAG,
            "Wi-Fi STA iniciado; conectando a SSID=%s",
            s_cfg.ssid);

        esp_err_t e =
            esp_wifi_connect();

        if (e != ESP_OK) {
            ESP_LOGE(
                TAG,
                "esp_wifi_connect fallo: %s",
                esp_err_to_name(e));
        }

    } else if (id == WIFI_EVENT_STA_CONNECTED) {

        s_connected = true;
        wifi_recovery_cancel();
        s_reconnect_attempts = 0;

        ESP_LOGI(
            TAG,
            "Wi-Fi asociado a SSID=%s",
            s_cfg.ssid);

    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev =
            (const wifi_event_sta_disconnected_t *)data;
        s_last_disconnect_reason = ev ? (uint16_t)ev->reason : 0;
        s_disconnect_count++;

        ESP_LOGW(TAG,
                 "Wi-Fi LINK DOWN reason=%u disconnects=%lu",
                 (unsigned)s_last_disconnect_reason,
                 (unsigned long)s_disconnect_count);

        s_connected = false;

        if (s_reconnect_attempts < KORVO_WIFI_QUIET_RECONNECT_MAX) {
            s_reconnect_attempts++;
            ESP_LOGW(TAG, "Wi-Fi desconectado; retry %u/%u",
                     (unsigned)s_reconnect_attempts,
                     (unsigned)KORVO_WIFI_QUIET_RECONNECT_MAX);

            esp_err_t e = esp_wifi_connect();
            if (e != ESP_OK) {
                ESP_LOGE(TAG, "Reconexion Wi-Fi fallo: %s", esp_err_to_name(e));
            }
        } else {
            ESP_LOGW(TAG,
                     "Wi-Fi QUIET: retries rapidos agotados; recovery cada %u s",
                     (unsigned)(KORVO_WIFI_RECOVERY_MS / 1000U));
            wifi_recovery_arm();
        }
    }
}

static esp_err_t configure_static_ip(void)
{
    if (!s_sta_netif) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_netif_ip_info_t ip = {0};

    if (esp_netif_str_to_ip4(
            s_cfg.ip,
            &ip.ip) != ESP_OK ||
        esp_netif_str_to_ip4(
            s_cfg.netmask,
            &ip.netmask) != ESP_OK ||
        esp_netif_str_to_ip4(
            s_cfg.network_gateway,
            &ip.gw) != ESP_OK) {

        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t e =
        esp_netif_dhcpc_stop(
            s_sta_netif);

    if (e != ESP_OK &&
        e != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {

        return e;
    }

    e =
        esp_netif_set_ip_info(
            s_sta_netif,
            &ip);

    if (e != ESP_OK) {
        return e;
    }

    if (s_cfg.dns[0]) {
        esp_netif_dns_info_t dns = {0};
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        if (esp_netif_str_to_ip4(s_cfg.dns, &dns.ip.u_addr.ip4) != ESP_OK) return ESP_ERR_INVALID_ARG;
        e = esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns);
        if (e != ESP_OK) return e;
    }

    ESP_LOGI(
        TAG,
        "IP fija KORVO: %s/%s gateway-lan=%s dns=%s",
        s_cfg.ip,
        s_cfg.netmask,
        s_cfg.network_gateway,
        s_cfg.dns[0] ? s_cfg.dns : "DISABLED");

    return ESP_OK;
}

esp_err_t korvo_network_init(void)
{
    if (s_started) {
        return ESP_OK;
    }

    esp_err_t e =
        nvs_flash_init();

    if (e == ESP_ERR_NVS_NO_FREE_PAGES ||
        e == ESP_ERR_NVS_NEW_VERSION_FOUND) {

        ESP_ERROR_CHECK(
            nvs_flash_erase());

        e =
            nvs_flash_init();
    }

    if (e != ESP_OK) {
        return e;
    }

    if (load_cfg(&s_cfg) != ESP_OK) {
        defaults(&s_cfg);
    }

    e =
        esp_netif_init();

    if (e != ESP_OK &&
        e != ESP_ERR_INVALID_STATE) {

        return e;
    }

    e =
        esp_event_loop_create_default();

    if (e != ESP_OK &&
        e != ESP_ERR_INVALID_STATE) {

        return e;
    }

    s_sta_netif =
        esp_netif_create_default_wifi_sta();

    if (!s_sta_netif) {
        return ESP_FAIL;
    }

    e =
        configure_static_ip();

    if (e != ESP_OK) {
        return e;
    }

    wifi_init_config_t wi =
        WIFI_INIT_CONFIG_DEFAULT();

    e =
        esp_wifi_init(
            &wi);

    if (e != ESP_OK) {
        return e;
    }

    e =
        esp_event_handler_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            wifi_event,
            NULL);

    if (e != ESP_OK) {
        return e;
    }

    wifi_config_t sta = {0};

    strlcpy(
        (char *)sta.sta.ssid,
        s_cfg.ssid,
        sizeof(sta.sta.ssid));

    strlcpy(
        (char *)sta.sta.password,
        s_cfg.password,
        sizeof(sta.sta.password));

    sta.sta.scan_method =
        WIFI_FAST_SCAN;

    sta.sta.sort_method =
        WIFI_CONNECT_AP_BY_SIGNAL;

    sta.sta.threshold.authmode =
        strlen(s_cfg.password)
            ? WIFI_AUTH_WPA2_PSK
            : WIFI_AUTH_OPEN;

    e = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (e != ESP_OK) {
        return e;
    }

    e = esp_wifi_set_mode(
            WIFI_MODE_STA);

    if (e != ESP_OK) {
        return e;
    }

    e =
        esp_wifi_set_config(
            WIFI_IF_STA,
            &sta);

    if (e != ESP_OK) {
        return e;
    }

    /* V21.10.1: quiet STA power save */

    esp_err_t ps_e = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

    if (ps_e != ESP_OK) {

        ESP_LOGW(TAG, "WIFI_PS_MIN_MODEM fallo: %s", esp_err_to_name(ps_e));

    }


    e =
        esp_wifi_start();

    if (e != ESP_OK) {
        return e;
    }

    s_started = true;

    ESP_LOGI(
        TAG,
        "STA READY ssid=%s ip=%s gateway-lan=%s gateway-io=%s",
        s_cfg.ssid,
        s_cfg.ip,
        s_cfg.network_gateway,
        s_cfg.gateway_node_ip);

    return ESP_OK;
}

esp_err_t korvo_network_get_status(
    korvo_network_status_t *s)
{
    if (!s) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(
        s,
        0,
        sizeof(*s));

    s->started =
        s_started;

    s->connected =
        s_connected;

    s->last_disconnect_reason = s_last_disconnect_reason;
    s->disconnect_count = s_disconnect_count;

    /*
     * Se conserva el campo por compatibilidad de API.
     * KORVO ahora es STA, por lo que no aloja clientes Wi-Fi.
     */
    s->connected_clients =
        0;

    s->config =
        s_cfg;

    if (s_started) {
        (void)esp_wifi_get_mac(
            WIFI_IF_STA,
            s->mac);
    }

    return ESP_OK;
}

esp_err_t korvo_network_get_config(
    korvo_network_config_t *c)
{
    if (!c) {
        return ESP_ERR_INVALID_ARG;
    }

    *c =
        s_cfg;

    return ESP_OK;
}

esp_err_t korvo_network_save_config(
    const korvo_network_config_t *c)
{
    if (!valid_cfg(c)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;

    esp_err_t e =
        nvs_open(
            NVS_NS,
            NVS_READWRITE,
            &h);

    if (e != ESP_OK) {
        return e;
    }

    if ((e = nvs_set_str(
            h,
            "ssid",
            c->ssid)) == ESP_OK &&
        (e = nvs_set_str(
            h,
            "pass",
            c->password)) == ESP_OK &&
        (e = nvs_set_str(
            h,
            "ip",
            c->ip)) == ESP_OK &&
        (e = nvs_set_str(
            h,
            "mask",
            c->netmask)) == ESP_OK &&
        (e = nvs_set_str(
            h,
            "netgw",
            c->network_gateway)) == ESP_OK &&
        (e = nvs_set_str(
            h,
            "dns",
            c->dns)) == ESP_OK &&
        (e = nvs_set_str(
            h,
            "node",
            c->gateway_node_ip)) == ESP_OK) {

        e =
            nvs_commit(
                h);
    }

    nvs_close(h);

    if (e == ESP_OK) {
        s_cfg =
            *c;
    }

    return e;
}

esp_err_t korvo_network_factory_defaults(void)
{
    korvo_network_config_t c;

    defaults(
        &c);

    return korvo_network_save_config(
        &c);
}
