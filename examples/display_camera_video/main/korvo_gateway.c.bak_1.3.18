#include "korvo_gateway.h"
#include "korvo_network.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "psa/crypto.h"
#include "nvs.h"

static const char *TAG = "korvo_gateway";
static const char *NVS_NS = "korvo_gw";

#define WS_RX_MAX                4096
#define WS_HEARTBEAT_MS          15000
#define WS_DEAD_MS               45000
#define WS_MANAGER_POLL_MS       500
#define WS_START_RETRY_MS        5000
#define WS_CLIENT_TASK_STACK     4096
#define WS_MANAGER_TASK_STACK    4096

typedef struct {
    korvo_gateway_identity_t local;
    korvo_gateway_identity_t expected;
    char pair_id[KORVO_GW_PAIR_ID_MAX];
    char pair_key[KORVO_GW_PAIR_KEY_MAX];
} persistent_cfg_t;

static persistent_cfg_t s_cfg;
static korvo_gateway_status_t s_status;
static SemaphoreHandle_t s_lock;
static esp_websocket_client_handle_t s_ws;
static TaskHandle_t s_manager_task;
static bool s_started;
static bool s_gateway_reported_control;
static bool s_auth_failed;
static bool s_force_restart;
static uint64_t s_last_tx_ms;
static uint64_t s_ws_retry_after_ms;
EXT_RAM_BSS_ATTR static char s_rx[WS_RX_MAX];
static size_t s_rx_expected;

typedef struct {
    korvo_gateway_event_listener_t fn;
    void *ctx;
} listener_slot_t;

#define GW_LISTENER_MAX 4
static listener_slot_t s_listeners[GW_LISTENER_MAX];

static void lock(void)
{
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    if (s_lock) xSemaphoreGive(s_lock);
}

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000LL);
}

static void copy_text(char *dst, size_t len, const char *src)
{
    if (!dst || len == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }

    size_t i = 0;
    while (i + 1 < len && src[i] != '\0') {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

static bool valid_label(const char *s, bool allow_space)
{
    if (!s || !s[0]) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.') continue;
        if (allow_space && *p == ' ') continue;
        return false;
    }
    return true;
}

static bool valid_ipv4(const char *ip)
{
    esp_ip4_addr_t tmp;
    return ip && ip[0] && esp_netif_str_to_ip4(ip, &tmp) == ESP_OK;
}

static void mac_text(const uint8_t mac[6], char out[KORVO_GW_MAC_MAX])
{
    snprintf(out, KORVO_GW_MAC_MAX, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void refresh_local_network_locked(void)
{
    korvo_network_status_t n = {0};
    if (korvo_network_get_status(&n) == ESP_OK) {
        copy_text(s_status.local.ip, sizeof(s_status.local.ip), n.config.ip);
        mac_text(n.mac, s_status.local.mac);
        s_status.wifi_connected = n.connected;
    }

    s_status.wifi_bssid[0] = '\0';
    s_status.wifi_rssi = 0;
    if (s_status.wifi_connected) {
        wifi_ap_record_t ap = {0};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            mac_text(ap.bssid, s_status.wifi_bssid);
            s_status.wifi_rssi = ap.rssi;
        }
    }
}

static bool identity_complete(const korvo_gateway_identity_t *i)
{
    /* V1.2 IP-pair mode: remote metadata may be blank; the peer IP is the
     * operational identity. role validation still happens at HTTP parse time. */
    return i && valid_ipv4(i->ip);
}

static bool pair_complete_locked(void)
{
    return valid_ipv4(s_cfg.expected.ip);
}

static bool identity_equal(const korvo_gateway_identity_t *a,
                           const korvo_gateway_identity_t *b)
{
    return a && b && valid_ipv4(a->ip) && valid_ipv4(b->ip) && strcmp(a->ip, b->ip) == 0;
}

static void recompute_locked(void)
{
    refresh_local_network_locked();
    s_status.paired = pair_complete_locked();
    s_status.local = s_cfg.local;
    korvo_network_status_t n = {0};
    if (korvo_network_get_status(&n) == ESP_OK) {
        copy_text(s_status.local.ip, sizeof(s_status.local.ip), n.config.ip);
        mac_text(n.mac, s_status.local.mac);
    }
    s_status.expected_gateway = s_cfg.expected;
    s_status.pair_id[0] = '\0';

    s_status.identity_match = s_status.paired && identity_equal(&s_status.expected_gateway, &s_status.observed_gateway);
    /* BSSID/MAC remain visible diagnostics only. They never authorize access. */
    s_status.wifi_mac_match = s_status.observed_gateway.mac[0] && s_status.wifi_bssid[0] &&
                              strcasecmp(s_status.observed_gateway.mac, s_status.wifi_bssid) == 0;
    /* PAIRED is reciprocal in practice: this KORVO connected to its configured
     * Gateway IP and that Gateway accepted the WebSocket from this KORVO IP. */
    s_status.gateway_ok = s_status.identity_match && s_status.websocket_connected;
    s_status.pair_auth_ok = s_status.websocket_connected;
    s_status.control_ok = s_status.gateway_ok && s_gateway_reported_control;

    if (!s_status.paired) {
        s_status.state = KORVO_GW_UNCONFIGURED;
    } else if (identity_complete(&s_status.observed_gateway) && !s_status.identity_match) {
        s_status.state = KORVO_GW_MISMATCH;
    } else if (s_status.control_ok) {
        s_status.state = KORVO_GW_OK;
    } else if (s_status.websocket_connected || s_status.gateway_ok) {
        s_status.state = KORVO_GW_CONNECTING;
    } else {
        s_status.state = KORVO_GW_OFFLINE;
    }
}

static esp_err_t nvs_get_text(nvs_handle_t h, const char *key, char *out, size_t len)
{
    size_t n = len;
    esp_err_t e = nvs_get_str(h, key, out, &n);
    if (e == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    return e;
}

static esp_err_t load_cfg(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    copy_text(s_cfg.local.name, sizeof(s_cfg.local.name), "KORVO-001");
    copy_text(s_cfg.local.hostname, sizeof(s_cfg.local.hostname), "LAB");
    copy_text(s_cfg.local.location, sizeof(s_cfg.local.location), "LAB");

    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (e == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (e != ESP_OK) return e;

    (void)nvs_get_text(h, "lname", s_cfg.local.name, sizeof(s_cfg.local.name));
    (void)nvs_get_text(h, "lhost", s_cfg.local.hostname, sizeof(s_cfg.local.hostname));
    (void)nvs_get_text(h, "lloc", s_cfg.local.location, sizeof(s_cfg.local.location));
    (void)nvs_get_text(h, "pname", s_cfg.expected.name, sizeof(s_cfg.expected.name));
    (void)nvs_get_text(h, "phost", s_cfg.expected.hostname, sizeof(s_cfg.expected.hostname));
    (void)nvs_get_text(h, "ploc", s_cfg.expected.location, sizeof(s_cfg.expected.location));
    (void)nvs_get_text(h, "pip", s_cfg.expected.ip, sizeof(s_cfg.expected.ip));
    (void)nvs_get_text(h, "pmac", s_cfg.expected.mac, sizeof(s_cfg.expected.mac));
    (void)nvs_get_text(h, "pid", s_cfg.pair_id, sizeof(s_cfg.pair_id));
    (void)nvs_get_text(h, "pkey", s_cfg.pair_key, sizeof(s_cfg.pair_key));
    nvs_close(h);
    return ESP_OK;
}

static esp_err_t save_cfg(void)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
#define PUT(k,v) do { if (e == ESP_OK) e = nvs_set_str(h, (k), (v)); } while (0)
    PUT("lname", s_cfg.local.name);
    PUT("lhost", s_cfg.local.hostname);
    PUT("lloc", s_cfg.local.location);
    PUT("pname", s_cfg.expected.name);
    PUT("phost", s_cfg.expected.hostname);
    PUT("ploc", s_cfg.expected.location);
    PUT("pip", s_cfg.expected.ip);
    PUT("pmac", s_cfg.expected.mac);
    PUT("pid", s_cfg.pair_id);
    PUT("pkey", s_cfg.pair_key);
#undef PUT
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
} http_buf_t;

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    if (!evt || !evt->user_data) return ESP_OK;
    http_buf_t *b = (http_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data && evt->data_len > 0 && b->buf && b->cap > 1) {
        size_t room = b->cap - b->len - 1;
        size_t take = (size_t)evt->data_len < room ? (size_t)evt->data_len : room;
        if (take) {
            memcpy(b->buf + b->len, evt->data, take);
            b->len += take;
            b->buf[b->len] = '\0';
        }
    }
    return ESP_OK;
}

static esp_err_t http_request(const char *url, esp_http_client_method_t method,
                              const char *post, char *response, size_t response_len,
                              int *status_code)
{
    http_buf_t b = {.buf = response, .cap = response_len, .len = 0};
    if (response && response_len) response[0] = '\0';
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 3000,
        .buffer_size = 768,
        .event_handler = http_event,
        .user_data = &b,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return ESP_ERR_NO_MEM;
    esp_http_client_set_method(c, method);
    esp_http_client_set_header(c, "Connection", "close");
    if (post) {
        esp_http_client_set_header(c, "Content-Type", "application/x-www-form-urlencoded");
        esp_http_client_set_post_field(c, post, (int)strlen(post));
    }
    esp_err_t e = esp_http_client_perform(c);
    if (status_code) *status_code = e == ESP_OK ? esp_http_client_get_status_code(c) : 0;
    esp_http_client_cleanup(c);
    return e;
}

static void json_string(cJSON *obj, const char *key, char *dst, size_t len)
{
    if (!dst || !len) return;
    dst[0] = '\0';
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(v) && v->valuestring) copy_text(dst, len, v->valuestring);
}

static void notify_gateway_event(cJSON *root, const char *name, cJSON *data)
{
    if (!root || !name || !name[0]) return;

    korvo_gateway_event_t event = {0};
    event.received_ms = now_ms();
    copy_text(event.name, sizeof(event.name), name);
    cJSON *seq = cJSON_GetObjectItemCaseSensitive(root, "seq");
    if (cJSON_IsNumber(seq) && seq->valuedouble >= 0) event.seq = (uint32_t)seq->valuedouble;
    event.simulated = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "simulated"));

    char *payload = cJSON_IsObject(data) || cJSON_IsArray(data) ? cJSON_PrintUnformatted(data) : NULL;
    copy_text(event.data, sizeof(event.data), payload ? payload : "{}");
    if (payload) free(payload);

    listener_slot_t listeners[GW_LISTENER_MAX] = {0};
    lock();
    memcpy(listeners, s_listeners, sizeof(listeners));
    unlock();
    for (size_t i = 0; i < GW_LISTENER_MAX; ++i) {
        if (listeners[i].fn) listeners[i].fn(&event, listeners[i].ctx);
    }
}

static bool parse_identity(cJSON *obj, korvo_gateway_identity_t *id, const char *required_role)
{
    if (!cJSON_IsObject(obj) || !id) return false;
    char role[24] = {0};
    memset(id, 0, sizeof(*id));
    json_string(obj, "role", role, sizeof(role));
    json_string(obj, "name", id->name, sizeof(id->name));
    if (!id->name[0]) json_string(obj, "device_name", id->name, sizeof(id->name));
    json_string(obj, "hostname", id->hostname, sizeof(id->hostname));
    json_string(obj, "location", id->location, sizeof(id->location));
    json_string(obj, "ip", id->ip, sizeof(id->ip));
    json_string(obj, "mac", id->mac, sizeof(id->mac));
    return (!required_role || !strcmp(role, required_role)) && identity_complete(id);
}

static esp_err_t get_gateway_identity(const char *ip, korvo_gateway_identity_t *id,
                                      char *message, size_t message_len)
{
    if (!valid_ipv4(ip) || !id) return ESP_ERR_INVALID_ARG;
    char url[96], response[1536];
    int status = 0;
    snprintf(url, sizeof(url), "http://%s/api/device/identity", ip);
    esp_err_t e = http_request(url, HTTP_METHOD_GET, NULL, response, sizeof(response), &status);
    if (e != ESP_OK || status != 200) {
        if (message && message_len) snprintf(message, message_len, "Gateway identity HTTP failed (%d/%s)", status, esp_err_to_name(e));
        return e == ESP_OK ? ESP_ERR_NOT_FOUND : e;
    }
    cJSON *root = cJSON_Parse(response);
    bool ok = root && parse_identity(root, id, "gateway");
    if (root) cJSON_Delete(root);
    if (!ok) {
        if (message && message_len) snprintf(message, message_len, "Target answered but is not a valid Gateway identity");
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (strcmp(id->ip, ip) != 0) {
        if (message && message_len) snprintf(message, message_len, "Gateway advertised IP %s but target is %s", id->ip, ip);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static bool current_ap_matches(const char *mac, char observed[KORVO_GW_MAC_MAX], int *rssi)
{
    if (observed) observed[0] = '\0';
    if (rssi) *rssi = 0;
    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return false;
    char bssid[KORVO_GW_MAC_MAX];
    mac_text(ap.bssid, bssid);
    if (observed) copy_text(observed, KORVO_GW_MAC_MAX, bssid);
    if (rssi) *rssi = ap.rssi;
    return mac && mac[0] && strcasecmp(mac, bssid) == 0;
}

static void url_encode(const char *src, char *dst, size_t dst_len)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (size_t i = 0; src && src[i] && o + 1 < dst_len; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.') {
            dst[o++] = (char)c;
        } else if (c == ' ') {
            dst[o++] = '+';
        } else if (o + 3 < dst_len) {
            dst[o++] = '%'; dst[o++] = hex[c >> 4]; dst[o++] = hex[c & 15];
        } else break;
    }
    dst[o] = '\0';
}

static esp_err_t hmac_hex(const char *nonce, const char *pair_id, const char *pair_key,
                          char out[65])
{
    if (!nonce || !pair_id || !pair_key || !pair_key[0] || !out) return ESP_ERR_INVALID_ARG;

    char msg[160];
    int n = snprintf(msg, sizeof(msg), "%s|%s", nonce, pair_id);
    if (n <= 0 || (size_t)n >= sizeof(msg)) return ESP_ERR_INVALID_SIZE;

    /*
     * ESP-IDF 6.x uses Mbed TLS 4.x, where the legacy mbedtls_md_hmac()
     * interface is no longer the supported application API. Use PSA Crypto
     * with a transient RAM-backed HMAC key. The pair key remains the exact
     * NVS string exchanged during enrollment; no eFuse key is involved here.
     */
    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init failed: %d", (int)status);
        return ESP_FAIL;
    }

    const size_t key_len = strlen(pair_key);
    const psa_algorithm_t alg = PSA_ALG_HMAC(PSA_ALG_SHA_256);
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attr, alg);
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attr, key_len * 8U);

    psa_key_id_t key_id = 0;
    status = psa_import_key(&attr, (const uint8_t *)pair_key, key_len, &key_id);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_import_key failed: %d", (int)status);
        psa_reset_key_attributes(&attr);
        return ESP_FAIL;
    }

    uint8_t digest[32];
    size_t digest_len = 0;
    status = psa_mac_compute(key_id, alg,
                             (const uint8_t *)msg, (size_t)n,
                             digest, sizeof(digest), &digest_len);

    psa_status_t destroy_status = psa_destroy_key(key_id);
    psa_reset_key_attributes(&attr);

    if (status != PSA_SUCCESS || digest_len != sizeof(digest)) {
        ESP_LOGE(TAG, "psa_mac_compute failed: status=%d len=%u",
                 (int)status, (unsigned)digest_len);
        return ESP_FAIL;
    }
    if (destroy_status != PSA_SUCCESS) {
        ESP_LOGW(TAG, "psa_destroy_key failed: %d", (int)destroy_status);
    }

    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < sizeof(digest); ++i) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 15];
    }
    out[64] = '\0';
    return ESP_OK;
}

static int ws_send_raw(const char *text)
{
    esp_websocket_client_handle_t ws;
    bool connected;
    lock(); ws = s_ws; connected = s_status.websocket_connected; unlock();
    if (!ws || !connected || !text) return -1;
    return esp_websocket_client_send_text(ws, text, (int)strlen(text), pdMS_TO_TICKS(1500));
}

static esp_err_t send_auth(const char *nonce, const char *challenge_pair_id)
{
    persistent_cfg_t cfg;
    korvo_gateway_status_t st;
    lock(); cfg = s_cfg; st = s_status; recompute_locked(); st = s_status; unlock();
    if (strcmp(challenge_pair_id, cfg.pair_id) != 0) return ESP_ERR_INVALID_STATE;
    char digest[65];
    esp_err_t he = hmac_hex(nonce, cfg.pair_id, cfg.pair_key, digest);
    if (he != ESP_OK) {
        ESP_LOGE(TAG, "HMAC failed: %s", esp_err_to_name(he));
        return he;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    if (!root || !data) { cJSON_Delete(root); cJSON_Delete(data); return ESP_ERR_NO_MEM; }
    cJSON_AddNumberToObject(root, "v", 1);
    cJSON_AddStringToObject(root, "type", "auth");
    cJSON_AddStringToObject(root, "name", "sys.auth");
    cJSON_AddItemToObject(root, "data", data);
    cJSON_AddStringToObject(data, "pair_id", cfg.pair_id);
    cJSON_AddStringToObject(data, "hmac", digest);
    cJSON_AddStringToObject(data, "name", st.local.name);
    cJSON_AddStringToObject(data, "hostname", st.local.hostname);
    cJSON_AddStringToObject(data, "location", st.local.location);
    cJSON_AddStringToObject(data, "ip", st.local.ip);
    cJSON_AddStringToObject(data, "mac", st.local.mac);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_ERR_NO_MEM;
    int sent = ws_send_raw(json);
    free(json);
    return sent > 0 ? ESP_OK : ESP_FAIL;
}

static void parse_gateway_status(cJSON *data)
{
    if (!cJSON_IsObject(data)) return;
    korvo_gateway_identity_t observed = {0};
    bool have_observed = false;
    bool reported_control = false;
    bool sip_registered = false;
    char sip_state[KORVO_GW_STATE_MAX] = {0};

    cJSON *enroll = cJSON_GetObjectItemCaseSensitive(data, "enrollment");
    if (cJSON_IsObject(enroll)) {
        cJSON *local = cJSON_GetObjectItemCaseSensitive(enroll, "local");
        if (cJSON_IsObject(local)) {
            /* status.enrollment.local has no role member. */
            json_string(local, "name", observed.name, sizeof(observed.name));
            json_string(local, "hostname", observed.hostname, sizeof(observed.hostname));
            json_string(local, "location", observed.location, sizeof(observed.location));
            json_string(local, "ip", observed.ip, sizeof(observed.ip));
            json_string(local, "mac", observed.mac, sizeof(observed.mac));
            have_observed = identity_complete(&observed);
        }
        cJSON *ctrl = cJSON_GetObjectItemCaseSensitive(enroll, "control_ok");
        reported_control = cJSON_IsTrue(ctrl);
    }

    cJSON *sip = cJSON_GetObjectItemCaseSensitive(data, "sip");
    if (cJSON_IsObject(sip)) {
        sip_registered = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(sip, "registered"));
        json_string(sip, "state", sip_state, sizeof(sip_state));
    }

    lock();
    if (have_observed) s_status.observed_gateway = observed;
    s_gateway_reported_control = reported_control;
    s_status.sip_registered = sip_registered;
    if (sip_state[0]) copy_text(s_status.sip_state, sizeof(s_status.sip_state), sip_state);
    s_status.last_rx_ms = now_ms();
    recompute_locked();
    unlock();
}

static void handle_ws_json(const char *text)
{
    cJSON *root = cJSON_Parse(text);
    if (!root) return;
    char type[24] = {0}, name[64] = {0};
    json_string(root, "type", type, sizeof(type));
    json_string(root, "name", name, sizeof(name));
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");

    lock();
    s_status.last_rx_ms = now_ms();
    if (name[0]) copy_text(s_status.last_event, sizeof(s_status.last_event), name);
    unlock();

    if (!strcmp(type, "challenge") && !strcmp(name, "sys.auth")) {
        /* A V2.4 Gateway is still using Pair Key/HMAC. Do not silently fall
         * back; this KORVO expects Gateway V2.5 simple IP pairing. */
        lock();
        copy_text(s_status.last_error, sizeof(s_status.last_error), "Gateway is still in V2.4 Pair-Key mode; update Gateway to V2.5");
        recompute_locked();
        unlock();
    } else if (!strcmp(type, "event") && !strcmp(name, "sys.hello") && cJSON_IsObject(data)) {
        korvo_gateway_identity_t id = {0};
        char role[24] = {0};
        json_string(data, "role", role, sizeof(role));
        json_string(data, "device_name", id.name, sizeof(id.name));
        json_string(data, "hostname", id.hostname, sizeof(id.hostname));
        json_string(data, "location", id.location, sizeof(id.location));
        json_string(data, "ip", id.ip, sizeof(id.ip));
        json_string(data, "mac", id.mac, sizeof(id.mac));
        bool reported = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "control_ok"));
        lock();
        if (!strcmp(role, "gateway") && identity_complete(&id)) s_status.observed_gateway = id;
        s_gateway_reported_control = reported;
        recompute_locked();
        unlock();
    } else if (!strcmp(type, "status") && !strcmp(name, "sys.status")) {
        parse_gateway_status(data);
    } else if (!strncmp(name, "sip.", 4)) {
        lock();
        if (!strcmp(name, "sip.registered")) s_status.sip_registered = true;
        else if (!strcmp(name, "sip.unregistered") || !strcmp(name, "sip.engine_unavailable")) s_status.sip_registered = false;
        copy_text(s_status.sip_state, sizeof(s_status.sip_state), name + 4);
        recompute_locked();
        unlock();
    }

    /* Preserve the complete Gateway event stream for the future KORVO auth
     * engine. The integration layer does not interpret reader.card/door/REX
     * beyond its own status needs; consumers subscribe through the listener. */
    if (!strcmp(type, "event")) notify_gateway_event(root, name, data);

    cJSON_Delete(root);
}

static esp_err_t send_cmd(const char *name, const char *data);

static void ws_event(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg; (void)base;
    esp_websocket_event_data_t *e = (esp_websocket_event_data_t *)event_data;
    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        lock();
        s_status.websocket_connected = true;
        s_status.pair_auth_ok = true;
        s_gateway_reported_control = false;
        s_auth_failed = false;
        s_status.last_rx_ms = now_ms();
        s_status.last_error[0] = '\0';
        recompute_locked();
        unlock();
        ESP_LOGI(TAG, "Gateway WebSocket connected (simple IP pair mode)");
        /* One initial snapshot; afterwards the persistent WS + ping/events are enough. */
        (void)send_cmd("sys.status", "{}");
    } else if (event_id == WEBSOCKET_EVENT_DISCONNECTED || event_id == WEBSOCKET_EVENT_CLOSED) {
        lock();
        s_status.websocket_connected = false;
        s_status.pair_auth_ok = false;
        s_gateway_reported_control = false;
        recompute_locked();
        unlock();
        ESP_LOGW(TAG, "Gateway WebSocket disconnected");
    } else if (event_id == WEBSOCKET_EVENT_ERROR) {
        lock();
        copy_text(s_status.last_error, sizeof(s_status.last_error), "WebSocket transport error");
        recompute_locked();
        unlock();
    } else if (event_id == WEBSOCKET_EVENT_DATA && e && e->data_ptr && e->data_len > 0) {
        if (e->payload_len <= 0 || e->payload_len >= WS_RX_MAX || e->payload_offset < 0 ||
            e->payload_offset + e->data_len >= WS_RX_MAX) {
            return;
        }
        if (e->payload_offset == 0) {
            memset(s_rx, 0, sizeof(s_rx));
            s_rx_expected = (size_t)e->payload_len;
        }
        memcpy(s_rx + e->payload_offset, e->data_ptr, (size_t)e->data_len);
        size_t end = (size_t)e->payload_offset + (size_t)e->data_len;
        if ((e->fin || end >= s_rx_expected) && end <= s_rx_expected) {
            s_rx[s_rx_expected] = '\0';
            handle_ws_json(s_rx);
            s_rx_expected = 0;
        }
    }
}

static void stop_ws(void)
{
    esp_websocket_client_handle_t ws = NULL;
    lock(); ws = s_ws; s_ws = NULL; unlock();
    if (ws) {
        esp_websocket_client_stop(ws);
        esp_websocket_client_destroy(ws);
    }
    lock();
    s_status.websocket_connected = false;
    s_status.pair_auth_ok = false;
    s_gateway_reported_control = false;
    recompute_locked();
    unlock();
}

static esp_err_t start_ws(void)
{
    char ip[KORVO_GW_IP_MAX];
    lock(); copy_text(ip, sizeof(ip), s_cfg.expected.ip); unlock();
    if (!valid_ipv4(ip)) return ESP_ERR_INVALID_ARG;
    char uri[96];
    snprintf(uri, sizeof(uri), "ws://%s/ws/korvo", ip);
    esp_websocket_client_config_t cfg = {
        .uri = uri,
        .task_prio = 5,
        .task_stack = WS_CLIENT_TASK_STACK,
        .buffer_size = WS_RX_MAX,
        .reconnect_timeout_ms = 3000,
        .network_timeout_ms = 5000,
        .pingpong_timeout_sec = 10,
        .ping_interval_sec = 5,
        .keep_alive_enable = true,
        .keep_alive_idle = 5,
        .keep_alive_interval = 5,
        .keep_alive_count = 3,
    };
    esp_websocket_client_handle_t ws = esp_websocket_client_init(&cfg);
    if (!ws) return ESP_ERR_NO_MEM;
    esp_err_t e = esp_websocket_register_events(ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
    if (e != ESP_OK) { esp_websocket_client_destroy(ws); return e; }
    lock(); s_ws = ws; s_status.state = KORVO_GW_CONNECTING; unlock();
    e = esp_websocket_client_start(ws);
    if (e != ESP_OK) {
        lock(); s_ws = NULL; unlock();
        esp_websocket_client_destroy(ws);
        return e;
    }
    ESP_LOGI(TAG, "Gateway WebSocket target %s", uri);
    return ESP_OK;
}

static esp_err_t send_cmd(const char *name, const char *data)
{
    bool control_required = strncmp(name, "sys.", 4) != 0 && strcmp(name, "sip.status") != 0;
    lock();
    bool allowed = !control_required || s_status.control_ok;
    bool connected = s_status.websocket_connected;
    unlock();
    if (!connected) return ESP_ERR_INVALID_STATE;
    if (!allowed) return ESP_ERR_INVALID_STATE;
    char json[320];
    snprintf(json, sizeof(json), "{\"v\":1,\"type\":\"cmd\",\"name\":\"%s\",\"data\":%s}",
             name, data ? data : "{}");
    return ws_send_raw(json) > 0 ? ESP_OK : ESP_FAIL;
}

static void manager_task(void *arg)
{
    (void)arg;
    for (;;) {
        korvo_network_status_t net = {0};
        korvo_network_get_status(&net);
        bool paired, need_start, force;
        esp_websocket_client_handle_t ws;
        uint64_t last_rx;
        bool connected;
        uint64_t now = now_ms();
        lock();
        recompute_locked();
        paired = pair_complete_locked();
        ws = s_ws;
        force = s_force_restart;
        s_force_restart = false;
        last_rx = s_status.last_rx_ms;
        connected = s_status.websocket_connected;
        if (force) s_ws_retry_after_ms = 0;
        need_start = paired && net.connected && !ws && now >= s_ws_retry_after_ms;
        unlock();

        if ((!paired || !net.connected || force) && ws) {
            stop_ws();
            ws = NULL;
        }
        if (need_start || (force && paired && net.connected)) {
            esp_err_t e = start_ws();
            if (e != ESP_OK) {
                lock();
                s_ws_retry_after_ms = now_ms() + WS_START_RETRY_MS;
                snprintf(s_status.last_error, sizeof(s_status.last_error), "WS start: %s; retry 5s", esp_err_to_name(e));
                recompute_locked();
                unlock();
                ESP_LOGW(TAG, "Gateway WebSocket start failed (%s); retry in %u ms",
                         esp_err_to_name(e), (unsigned)WS_START_RETRY_MS);
            } else {
                lock(); s_ws_retry_after_ms = 0; unlock();
            }
        }

        now = now_ms();
        if (connected && now - s_last_tx_ms >= WS_HEARTBEAT_MS) {
            (void)send_cmd("sys.ping", "{}");
            s_last_tx_ms = now;
        }
        if (connected && last_rx && now - last_rx > WS_DEAD_MS) {
            ESP_LOGW(TAG, "Gateway heartbeat timeout; restarting WebSocket");
            lock(); s_force_restart = true; s_gateway_reported_control = false; recompute_locked(); unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(WS_MANAGER_POLL_MS));
    }
}

esp_err_t korvo_gateway_init(void)
{
    if (s_started) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    esp_err_t e = load_cfg();
    if (e != ESP_OK) { vSemaphoreDelete(s_lock); s_lock = NULL; return e; }
    memset(&s_status, 0, sizeof(s_status));
    copy_text(s_status.sip_state, sizeof(s_status.sip_state), "idle");
    lock(); recompute_locked(); unlock();
    if (xTaskCreate(manager_task, "gw_link", WS_MANAGER_TASK_STACK, NULL, 4, &s_manager_task) != pdPASS) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    ESP_LOGI(TAG, "Gateway integration initialized (IP pair): %s", pair_complete_locked() ? "configured" : "not configured");
    return ESP_OK;
}

esp_err_t korvo_gateway_set_local_identity(const char *name, const char *hostname, const char *location)
{
    if (!valid_label(name, true) || !valid_label(hostname, false) || !valid_label(location, true)) return ESP_ERR_INVALID_ARG;
    lock();
    copy_text(s_cfg.local.name, sizeof(s_cfg.local.name), name);
    copy_text(s_cfg.local.hostname, sizeof(s_cfg.local.hostname), hostname);
    copy_text(s_cfg.local.location, sizeof(s_cfg.local.location), location);
    esp_err_t e = save_cfg();
    s_force_restart = true;
    recompute_locked();
    unlock();
    return e;
}

esp_err_t korvo_gateway_enroll(const char *target_ip, char *message, size_t message_len)
{
    if (message && message_len) message[0] = '\0';
    if (!valid_ipv4(target_ip)) return ESP_ERR_INVALID_ARG;

    /* V1.2: SAVE really means SAVE. Do not make configuration depend on
     * discovery, MAC, identity labels or the target being online right now.
     * The live WebSocket is the reciprocal proof: the Gateway will only admit
     * this KORVO if its TCP source IP matches the KORVO IP configured there. */
    lock();
    memset(&s_cfg.expected, 0, sizeof(s_cfg.expected));
    copy_text(s_cfg.expected.ip, sizeof(s_cfg.expected.ip), target_ip);
    s_cfg.pair_id[0] = '\0';
    s_cfg.pair_key[0] = '\0';
    memset(&s_status.observed_gateway, 0, sizeof(s_status.observed_gateway));
    s_auth_failed = false;
    s_gateway_reported_control = false;
    esp_err_t e = save_cfg();
    s_force_restart = true;
    recompute_locked();
    unlock();

    if (e == ESP_OK) {
        korvo_network_config_t hint = {0};
        if (korvo_network_get_config(&hint) == ESP_OK) {
            copy_text(hint.gateway_node_ip, sizeof(hint.gateway_node_ip), target_ip);
            (void)korvo_network_save_config(&hint);
        }

        /* Optional diagnostic probe only. Failure does not undo the saved IP. */
        korvo_network_status_t net = {0};
        korvo_network_get_status(&net);
        if (net.connected) {
            korvo_gateway_identity_t observed = {0};
            char probe_msg[160] = {0};
            if (get_gateway_identity(target_ip, &observed, probe_msg, sizeof(probe_msg)) == ESP_OK) {
                lock();
                s_status.observed_gateway = observed;
                recompute_locked();
                unlock();
            }
        }
    }

    if (message && message_len) snprintf(message, message_len, "Gateway IP saved: %s", target_ip);
    return e;
}

esp_err_t korvo_gateway_verify(char *message, size_t message_len)
{
    if (message && message_len) message[0] = '\0';
    korvo_gateway_identity_t expected;
    lock();
    bool paired = pair_complete_locked();
    expected = s_cfg.expected;
    unlock();
    if (!paired) return ESP_ERR_INVALID_STATE;

    korvo_gateway_identity_t observed = {0};
    esp_err_t e = get_gateway_identity(expected.ip, &observed, message, message_len);
    if (e != ESP_OK) {
        lock(); memset(&s_status.observed_gateway, 0, sizeof(s_status.observed_gateway)); recompute_locked(); unlock();
        return e;
    }
    lock();
    s_status.observed_gateway = observed;
    recompute_locked();
    bool ok = s_status.identity_match;
    unlock();
    if (message && message_len) snprintf(message, message_len, "Gateway %s at %s", ok ? "MATCH" : "MISMATCH", expected.ip);
    return ok ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t korvo_gateway_clear_pairing(void)
{
    lock();
    memset(&s_cfg.expected, 0, sizeof(s_cfg.expected));
    s_cfg.pair_id[0] = '\0';
    s_cfg.pair_key[0] = '\0';
    memset(&s_status.observed_gateway, 0, sizeof(s_status.observed_gateway));
    s_status.pair_auth_ok = false;
    s_gateway_reported_control = false;
    s_auth_failed = false;
    esp_err_t e = save_cfg();
    s_force_restart = true;
    recompute_locked();
    unlock();
    return e;
}

void korvo_gateway_get_status(korvo_gateway_status_t *status)
{
    if (!status) return;
    lock(); recompute_locked(); *status = s_status; unlock();
}

const char *korvo_gateway_state_name(korvo_gateway_state_t state)
{
    switch (state) {
        case KORVO_GW_UNCONFIGURED: return "unconfigured";
        case KORVO_GW_OFFLINE: return "offline";
        case KORVO_GW_CONNECTING: return "connecting";
        case KORVO_GW_AUTHENTICATING: return "connecting";
        case KORVO_GW_OK: return "paired";
        case KORVO_GW_MISMATCH: return "mismatch";
        case KORVO_GW_AUTH_FAILED: return "blocked";
        default: return "unknown";
    }
}

size_t korvo_gateway_status_json(char *out, size_t out_len)
{
    if (!out || !out_len) return 0;
    korvo_gateway_status_t s;
    korvo_gateway_get_status(&s);
    int n = snprintf(out, out_len,
        "{\"state\":\"%s\",\"configured\":%s,\"paired\":%s,\"gateway_ok\":%s,\"control_ok\":%s,"
        "\"websocket\":%s,\"session_ip_ok\":%s,"
        "\"local\":{\"name\":\"%s\",\"hostname\":\"%s\",\"location\":\"%s\",\"ip\":\"%s\",\"mac\":\"%s\"},"
        "\"expected_gateway\":{\"name\":\"%s\",\"hostname\":\"%s\",\"location\":\"%s\",\"ip\":\"%s\",\"mac\":\"%s\"},"
        "\"observed_gateway\":{\"name\":\"%s\",\"hostname\":\"%s\",\"location\":\"%s\",\"ip\":\"%s\",\"mac\":\"%s\"},"
        "\"wifi\":{\"connected\":%s,\"bssid\":\"%s\",\"rssi\":%d,\"mac_match_info\":%s},"
        "\"sip\":{\"registered\":%s,\"state\":\"%s\"},\"last_event\":\"%s\",\"last_error\":\"%s\"}",
        korvo_gateway_state_name(s.state), s.paired ? "true" : "false",
        s.gateway_ok ? "true" : "false",
        s.gateway_ok ? "true" : "false", s.control_ok ? "true" : "false",
        s.websocket_connected ? "true" : "false", s.websocket_connected ? "true" : "false",
        s.local.name, s.local.hostname, s.local.location, s.local.ip, s.local.mac,
        s.expected_gateway.name, s.expected_gateway.hostname, s.expected_gateway.location, s.expected_gateway.ip, s.expected_gateway.mac,
        s.observed_gateway.name, s.observed_gateway.hostname, s.observed_gateway.location, s.observed_gateway.ip, s.observed_gateway.mac,
        s.wifi_connected ? "true" : "false", s.wifi_bssid, s.wifi_rssi, s.wifi_mac_match ? "true" : "false",
        s.sip_registered ? "true" : "false", s.sip_state, s.last_event, s.last_error);
    if (n < 0) return 0;
    return (size_t)n < out_len ? (size_t)n : out_len - 1;
}

esp_err_t korvo_gateway_add_listener(korvo_gateway_event_listener_t listener, void *ctx)
{
    if (!listener || !s_lock) return ESP_ERR_INVALID_ARG;
    esp_err_t result = ESP_ERR_NO_MEM;
    lock();
    for (size_t i = 0; i < GW_LISTENER_MAX; ++i) {
        if (s_listeners[i].fn == listener && s_listeners[i].ctx == ctx) {
            result = ESP_OK;
            break;
        }
        if (!s_listeners[i].fn) {
            s_listeners[i].fn = listener;
            s_listeners[i].ctx = ctx;
            result = ESP_OK;
            break;
        }
    }
    unlock();
    return result;
}

esp_err_t korvo_gateway_request_status(void) { return send_cmd("sys.status", "{}"); }
esp_err_t korvo_gateway_door_unlock(uint32_t ms) { char d[64]; snprintf(d, sizeof(d), "{\"ms\":%lu}", (unsigned long)(ms ? ms : 5000)); return send_cmd("door.unlock", d); }
esp_err_t korvo_gateway_door_lock(void) { return send_cmd("door.lock", "{}"); }
esp_err_t korvo_gateway_beacon_on(void) { return send_cmd("beacon.on", "{}"); }
esp_err_t korvo_gateway_beacon_off(void) { return send_cmd("beacon.off", "{}"); }
esp_err_t korvo_gateway_beacon_flash(uint32_t on_ms, uint32_t off_ms, uint32_t cycles)
{
    char d[112];
    snprintf(d, sizeof(d), "{\"on_ms\":%lu,\"off_ms\":%lu,\"cycles\":%lu}",
             (unsigned long)on_ms, (unsigned long)off_ms, (unsigned long)cycles);
    return send_cmd("beacon.flash", d);
}
esp_err_t korvo_gateway_wiegand_tx(uint8_t format, uint32_t facility, uint32_t card)
{
    char d[112];
    snprintf(d, sizeof(d), "{\"format\":%u,\"facility\":%lu,\"card\":%lu}",
             (unsigned)format, (unsigned long)facility, (unsigned long)card);
    return send_cmd("wiegand.tx", d);
}
esp_err_t korvo_gateway_sip_call(void) { return send_cmd("sip.call", "{}"); }
esp_err_t korvo_gateway_sip_answer(void) { return send_cmd("sip.answer", "{}"); }
esp_err_t korvo_gateway_sip_reject(void) { return send_cmd("sip.reject", "{}"); }
esp_err_t korvo_gateway_sip_hangup(void) { return send_cmd("sip.hangup", "{}"); }
