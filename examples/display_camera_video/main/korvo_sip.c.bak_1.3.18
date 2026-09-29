#include "korvo_sip.h"
#include "korvo_network.h"
#include "korvo_audio_bridge.h"
#include "korvo_bluetooth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "esp_rom_md5.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"

static const char *TAG = "korvo_sip";
static const char *NVS_NS = "korvo_sip";
static const char *NVS_KEY = "cfg";

#define SIP_RX_MAX          4096
#define SIP_TX_MAX          3072
#define SIP_SDP_MAX         768
#define SIP_HEADER_MAX      512
#define SIP_AUTH_MAX        640
#define SIP_LISTENER_MAX    4
#define SIP_CMD_Q_LEN       8
#define SIP_TASK_STACK      9216
#define SIP_TASK_PRIO       6
#define SIP_SELECT_MS       100
#define SIP_REG_RETRY_MS    5000
#define SIP_INVITE_TO_MS    30000

typedef enum {
    CMD_RECONFIGURE = 1,
    CMD_REGISTER,
    CMD_CALL,
    CMD_ANSWER,
    CMD_REJECT,
    CMD_HANGUP,
} sip_cmd_type_t;

typedef struct {
    sip_cmd_type_t type;
    char arg[KORVO_SIP_URI_MAX];
} sip_cmd_t;

typedef struct {
    korvo_sip_listener_t fn;
    void *ctx;
} listener_slot_t;

typedef struct {
    int sock;
    struct sockaddr_in server_addr;
    bool server_valid;
    char local_ip[16];

    uint32_t cseq;
    char reg_call_id[64];

    char call_id[128];
    char from_tag[32];
    char to_tag[64];
    char remote_uri[KORVO_SIP_URI_MAX];
    char remote_number[KORVO_SIP_NUMBER_MAX];

    struct sockaddr_in last_request_addr;
    char last_via[SIP_HEADER_MAX];
    char last_from[SIP_HEADER_MAX];
    char last_to[SIP_HEADER_MAX];
    char last_call_id[128];
    char last_cseq[64];

    char auth_realm[96];
    char auth_nonce[192];
    char auth_qop[32];
    char auth_opaque[96];
    char auth_cnonce[24];
    bool auth_valid;
    bool auth_proxy;
    bool register_auth_sent;
    bool invite_auth_sent;

    char rtp_remote_ip[64];
    uint16_t rtp_remote_port;
    int rtp_payload_type;

    uint64_t next_register_ms;
    uint64_t register_retry_ms;
    uint64_t invite_deadline_ms;
} sip_runtime_t;

static SemaphoreHandle_t s_lock;
static QueueHandle_t s_cmd_q;
static TaskHandle_t s_task;
static korvo_sip_config_t s_cfg;
static korvo_sip_status_t s_status;
static listener_slot_t s_listeners[SIP_LISTENER_MAX];

/* SIP working memory: keep large buffers out of the scarce internal task stack. */
static sip_runtime_t *s_rt;
static char *s_rx_buf;
static char *s_tx_buf;
static char *s_sdp_buf;
static char *s_auth_buf;

static void *sip_work_alloc(size_t bytes, const char *name)
{
    void *p = heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) {
        ESP_LOGI(TAG, "SIP work %-7s -> PSRAM %u bytes", name, (unsigned)bytes);
        return p;
    }
    p = heap_caps_calloc(1, bytes, MALLOC_CAP_8BIT);
    if (p) ESP_LOGW(TAG, "SIP work %-7s -> heap fallback %u bytes", name, (unsigned)bytes);
    return p;
}

static void sip_work_free_all(void)
{
    free(s_auth_buf); s_auth_buf = NULL;
    free(s_sdp_buf);  s_sdp_buf = NULL;
    free(s_tx_buf);   s_tx_buf = NULL;
    free(s_rx_buf);   s_rx_buf = NULL;
    free(s_rt);       s_rt = NULL;
}

static esp_err_t sip_work_alloc_all(void)
{
    if (s_rt && s_rx_buf && s_tx_buf && s_sdp_buf && s_auth_buf) return ESP_OK;
    sip_work_free_all();
    s_rt       = sip_work_alloc(sizeof(*s_rt), "runtime");
    s_rx_buf   = sip_work_alloc(SIP_RX_MAX + 1, "rx");
    s_tx_buf   = sip_work_alloc(SIP_TX_MAX, "tx");
    s_sdp_buf  = sip_work_alloc(SIP_SDP_MAX, "sdp");
    s_auth_buf = sip_work_alloc(SIP_AUTH_MAX, "auth");
    if (!s_rt || !s_rx_buf || !s_tx_buf || !s_sdp_buf || !s_auth_buf) {
        sip_work_free_all();
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG,
             "SIP work ready: internal_free=%u largest=%u psram_free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return ESP_OK;
}

static void lock(void) { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

static uint64_t now_ms(void)
{
    return (uint64_t)(xTaskGetTickCount()) * 1000ULL / configTICK_RATE_HZ;
}

static void copy_text(char *dst, size_t len, const char *src)
{
    if (!dst || !len) return;
    if (!src) { dst[0] = 0; return; }
    size_t n = strlen(src);
    if (n >= len) n = len - 1;
    if (n) memcpy(dst, src, n);
    dst[n] = '\0';
}

static bool clean_token(const char *s)
{
    if (!s || !s[0]) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if (*p < 0x20 || *p == 0x7f || *p == '\r' || *p == '\n') return false;
    }
    return true;
}

static void defaults(korvo_sip_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->enabled = false;
    copy_text(cfg->server, sizeof(cfg->server), "192.168.10.2");
    cfg->server_port = 5060;
    cfg->local_port = 5060;
    cfg->rtp_port = 4000;
    cfg->register_expires = 300;
    copy_text(cfg->extension, sizeof(cfg->extension), "200");
    copy_text(cfg->username, sizeof(cfg->username), "200");
    copy_text(cfg->display_name, sizeof(cfg->display_name), "KORVO-001");
    copy_text(cfg->operator_extension, sizeof(cfg->operator_extension), "300");
}

static bool valid_cfg(const korvo_sip_config_t *cfg)
{
    if (!cfg) return false;
    if (!cfg->server_port || !cfg->local_port || !cfg->rtp_port) return false;
    if (cfg->register_expires < 60 || cfg->register_expires > 3600) return false;
    if (!clean_token(cfg->server) || !clean_token(cfg->extension) ||
        !clean_token(cfg->username) || !clean_token(cfg->display_name) ||
        !clean_token(cfg->operator_extension)) return false;
    if (strchr(cfg->server, ' ') || strchr(cfg->extension, ' ') ||
        strchr(cfg->username, ' ') || strchr(cfg->operator_extension, ' ')) return false;
    return true;
}

static esp_err_t load_cfg(void)
{
    defaults(&s_cfg);
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (e == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (e != ESP_OK) return e;
    size_t n = sizeof(s_cfg);
    e = nvs_get_blob(h, NVS_KEY, &s_cfg, &n);
    nvs_close(h);
    if (e == ESP_ERR_NVS_NOT_FOUND || n != sizeof(s_cfg) || !valid_cfg(&s_cfg)) {
        defaults(&s_cfg);
        return ESP_OK;
    }
    return e;
}

static esp_err_t persist_cfg(void)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_blob(h, NVS_KEY, &s_cfg, sizeof(s_cfg));
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

const char *korvo_sip_state_name(korvo_sip_state_t state)
{
    switch (state) {
        case KORVO_SIP_DISABLED: return "disabled";
        case KORVO_SIP_OFFLINE: return "offline";
        case KORVO_SIP_REGISTERING: return "registering";
        case KORVO_SIP_IDLE: return "idle";
        case KORVO_SIP_CALLING: return "calling";
        case KORVO_SIP_RINGING: return "ringing";
        case KORVO_SIP_INCOMING: return "incoming";
        case KORVO_SIP_CONNECTING: return "connecting";
        case KORVO_SIP_CONNECTED: return "connected";
        case KORVO_SIP_BUSY: return "busy";
        case KORVO_SIP_NO_ANSWER: return "no_answer";
        case KORVO_SIP_REJECTED: return "rejected";
        case KORVO_SIP_FAILED: return "failed";
        case KORVO_SIP_ENDED: return "ended";
        default: return "unknown";
    }
}

static void notify_status(void)
{
    korvo_sip_status_t st;
    listener_slot_t slots[SIP_LISTENER_MAX];
    lock();
    st = s_status;
    memcpy(slots, s_listeners, sizeof(slots));
    unlock();
    for (size_t i = 0; i < SIP_LISTENER_MAX; ++i) {
        if (slots[i].fn) slots[i].fn(&st, slots[i].ctx);
    }
}

static void set_state(korvo_sip_state_t state, int code, const char *error)
{
    bool changed;
    lock();
    changed = s_status.state != state || s_status.last_code != code;
    s_status.state = state;
    s_status.last_code = code;
    if (error) copy_text(s_status.last_error, sizeof(s_status.last_error), error);
    else if (state != KORVO_SIP_FAILED) s_status.last_error[0] = 0;
    unlock();
    if (changed || error) notify_status();
}

static void set_remote(const char *uri, const char *number)
{
    lock();
    copy_text(s_status.remote_uri, sizeof(s_status.remote_uri), uri);
    copy_text(s_status.remote_number, sizeof(s_status.remote_number), number);
    unlock();
}

static void set_codec(int pt)
{
    lock();
    copy_text(s_status.negotiated_codec, sizeof(s_status.negotiated_codec),
              pt == 8 ? "PCMA/8000" : pt == 0 ? "PCMU/8000" : "--");
    unlock();
}

static void set_registered(bool registered)
{
    bool changed;
    lock();
    changed = s_status.registered != registered;
    s_status.registered = registered;
    unlock();
    if (changed) notify_status();
}

static bool network_ready(char ip[16])
{
    korvo_network_status_t ns = {0};
    if (korvo_network_get_status(&ns) != ESP_OK || !ns.connected) return false;
    if (ip) copy_text(ip, 16, ns.config.ip);
    return true;
}

static void md5_hex(const char *text, char out[33])
{
    uint8_t digest[ESP_ROM_MD5_DIGEST_LEN] = {0};
    md5_context_t ctx;
    esp_rom_md5_init(&ctx);
    esp_rom_md5_update(&ctx, text, (uint32_t)strlen(text));
    esp_rom_md5_final(digest, &ctx);
    for (int i = 0; i < ESP_ROM_MD5_DIGEST_LEN; ++i) {
        static const char hex[] = "0123456789abcdef";
        out[i * 2] = hex[(digest[i] >> 4) & 0x0f];
        out[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out[32] = '\0';
}

static const char *ci_strstr(const char *haystack, const char *needle)
{
    if (!haystack || !needle) return NULL;
    size_t n = strlen(needle);
    if (!n) return haystack;
    for (const char *p = haystack; *p; ++p) {
        if (!strncasecmp(p, needle, n)) return p;
    }
    return NULL;
}

static bool parse_header(const char *msg, const char *name, char *out, size_t out_len)
{
    if (!msg || !name || !out || !out_len) return false;
    size_t name_len = strlen(name);
    const char *p = msg;
    while (p && *p) {
        const char *e = strstr(p, "\r\n");
        if (!e) break;
        if ((size_t)(e - p) > name_len && !strncasecmp(p, name, name_len) && p[name_len] == ':') {
            const char *v = p + name_len + 1;
            while (v < e && (*v == ' ' || *v == '\t')) ++v;
            size_t n = (size_t)(e - v);
            if (n >= out_len) n = out_len - 1;
            memcpy(out, v, n);
            out[n] = 0;
            return true;
        }
        p = e + 2;
    }
    out[0] = 0;
    return false;
}

static void auth_param(const char *challenge, const char *key, char *out, size_t out_len)
{
    if (!out || !out_len) return;
    out[0] = 0;
    if (!challenge || !key) return;
    const char *p = ci_strstr(challenge, key);
    if (!p) return;
    p += strlen(key);
    while (*p == ' ' || *p == '=') ++p;
    bool quoted = *p == '"';
    if (quoted) ++p;
    const char *e = p;
    while (*e && ((quoted && *e != '"') || (!quoted && *e != ',' && !isspace((unsigned char)*e)))) ++e;
    size_t n = (size_t)(e - p);
    if (n >= out_len) n = out_len - 1;
    memcpy(out, p, n); out[n] = 0;
}

static void parse_auth_challenge(sip_runtime_t *rt, const char *header, bool proxy)
{
    auth_param(header, "realm", rt->auth_realm, sizeof(rt->auth_realm));
    auth_param(header, "nonce", rt->auth_nonce, sizeof(rt->auth_nonce));
    auth_param(header, "qop", rt->auth_qop, sizeof(rt->auth_qop));
    auth_param(header, "opaque", rt->auth_opaque, sizeof(rt->auth_opaque));
    if (rt->auth_qop[0] && !ci_strstr(rt->auth_qop, "auth")) rt->auth_qop[0] = 0;
    else if (rt->auth_qop[0]) copy_text(rt->auth_qop, sizeof(rt->auth_qop), "auth");
    snprintf(rt->auth_cnonce, sizeof(rt->auth_cnonce), "%08" PRIx32 "%08" PRIx32,
             esp_random(), esp_random());
    rt->auth_proxy = proxy;
    rt->auth_valid = rt->auth_realm[0] && rt->auth_nonce[0];
}

static void build_auth_header(sip_runtime_t *rt, const char *method, const char *uri,
                              char *out, size_t out_len)
{
    out[0] = 0;
    if (!rt->auth_valid) return;
    korvo_sip_config_t cfg;
    lock(); cfg = s_cfg; unlock();

    char a1[256], a2[256], ha1[33], ha2[33], response[33], raw[640];
    snprintf(a1, sizeof(a1), "%s:%s:%s", cfg.username, rt->auth_realm, cfg.password);
    snprintf(a2, sizeof(a2), "%s:%s", method, uri);
    md5_hex(a1, ha1); md5_hex(a2, ha2);
    if (rt->auth_qop[0]) {
        snprintf(raw, sizeof(raw), "%s:%s:00000001:%s:%s:%s",
                 ha1, rt->auth_nonce, rt->auth_cnonce, rt->auth_qop, ha2);
    } else {
        snprintf(raw, sizeof(raw), "%s:%s:%s", ha1, rt->auth_nonce, ha2);
    }
    md5_hex(raw, response);

    const char *hdr = rt->auth_proxy ? "Proxy-Authorization" : "Authorization";
    int n = snprintf(out, out_len,
        "%s: Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", response=\"%s\", algorithm=MD5",
        hdr, cfg.username, rt->auth_realm, rt->auth_nonce, uri, response);
    if (n > 0 && (size_t)n < out_len && rt->auth_qop[0]) {
        n += snprintf(out + n, out_len - (size_t)n,
                      ", qop=%s, nc=00000001, cnonce=\"%s\"", rt->auth_qop, rt->auth_cnonce);
    }
    if (n > 0 && (size_t)n < out_len && rt->auth_opaque[0]) {
        n += snprintf(out + n, out_len - (size_t)n, ", opaque=\"%s\"", rt->auth_opaque);
    }
    if (n > 0 && (size_t)n < out_len) snprintf(out + n, out_len - (size_t)n, "\r\n");
}

static esp_err_t resolve_server(sip_runtime_t *rt)
{
    korvo_sip_config_t cfg;
    lock(); cfg = s_cfg; unlock();
    char port[8]; snprintf(port, sizeof(port), "%u", (unsigned)cfg.server_port);
    struct addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo *res = NULL;
    int rc = getaddrinfo(cfg.server, port, &hints, &res);
    if (rc != 0 || !res) {
        if (res) freeaddrinfo(res);
        return ESP_ERR_NOT_FOUND;
    }
    memcpy(&rt->server_addr, res->ai_addr, sizeof(rt->server_addr));
    freeaddrinfo(res);
    rt->server_valid = true;
    return ESP_OK;
}

static void close_socket(sip_runtime_t *rt)
{
    if (rt->sock >= 0) {
        shutdown(rt->sock, SHUT_RDWR);
        close(rt->sock);
    }
    rt->sock = -1;
    rt->server_valid = false;
}

static esp_err_t open_socket(sip_runtime_t *rt)
{
    korvo_sip_config_t cfg;
    lock(); cfg = s_cfg; unlock();
    close_socket(rt);
    if (!network_ready(rt->local_ip)) return ESP_ERR_INVALID_STATE;
    ESP_RETURN_ON_ERROR(resolve_server(rt), TAG, "resolve SIP server");

    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return ESP_FAIL;
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    local.sin_port = htons(cfg.local_port);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&local, sizeof(local)) < 0) {
        close(s);
        return ESP_FAIL;
    }
    rt->sock = s;
    return ESP_OK;
}

static void extract_uri_and_number(const char *header, char *uri, size_t uri_len,
                                   char *number, size_t number_len)
{
    if (uri && uri_len) uri[0] = 0;
    if (number && number_len) number[0] = 0;
    if (!header) return;
    const char *s = strchr(header, '<');
    const char *e = s ? strchr(s, '>') : NULL;
    if (s && e && e > s + 1) {
        ++s;
        size_t n = (size_t)(e - s);
        if (uri && uri_len) { if (n >= uri_len) n = uri_len - 1; memcpy(uri, s, n); uri[n] = 0; }
    } else {
        s = ci_strstr(header, "sip:");
        if (s && uri && uri_len) {
            e = s;
            while (*e && *e != ';' && *e != ' ' && *e != '\r' && *e != '\n') ++e;
            size_t n = (size_t)(e - s); if (n >= uri_len) n = uri_len - 1;
            memcpy(uri, s, n); uri[n] = 0;
        }
    }
    const char *u = uri && uri[0] ? ci_strstr(uri, "sip:") : NULL;
    if (u) {
        u += 4; const char *at = strchr(u, '@');
        size_t n = at ? (size_t)(at - u) : strlen(u);
        if (number && number_len) { if (n >= number_len) n = number_len - 1; memcpy(number, u, n); number[n] = 0; }
    }
}

static void append_text(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0 || !src) return;
    size_t used = strlen(dst);
    if (used >= dst_len - 1) return;
    size_t n = strlen(src);
    size_t room = dst_len - 1 - used;
    if (n > room) n = room;
    if (n) memcpy(dst + used, src, n);
    dst[used + n] = '\0';
}

static void build_target_uri(const char *number_or_uri, char *out, size_t out_len)
{
    korvo_sip_config_t cfg;
    lock(); cfg = s_cfg; unlock();
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!number_or_uri) return;
    if (ci_strstr(number_or_uri, "sip:") == number_or_uri) {
        copy_text(out, out_len, number_or_uri);
        return;
    }
    append_text(out, out_len, "sip:");
    append_text(out, out_len, number_or_uri);
    append_text(out, out_len, "@");
    append_text(out, out_len, cfg.server);
}

static int make_sdp(sip_runtime_t *rt, char *out, size_t out_len, int answer_pt)
{
    korvo_sip_config_t cfg;
    lock(); cfg = s_cfg; unlock();
    if (answer_pt == 0 || answer_pt == 8) {
        return snprintf(out, out_len,
            "v=0\r\n"
            "o=KORVO 1 1 IN IP4 %s\r\n"
            "s=KORVO Intercom\r\n"
            "c=IN IP4 %s\r\n"
            "t=0 0\r\n"
            "m=audio %u RTP/AVP %d\r\n"
            "a=rtpmap:%d %s/8000\r\n"
            "a=sendrecv\r\n"
            "a=ptime:20\r\n",
            rt->local_ip, rt->local_ip, (unsigned)cfg.rtp_port,
            answer_pt, answer_pt, answer_pt == 8 ? "PCMA" : "PCMU");
    }
    return snprintf(out, out_len,
        "v=0\r\n"
        "o=KORVO 1 1 IN IP4 %s\r\n"
        "s=KORVO Intercom\r\n"
        "c=IN IP4 %s\r\n"
        "t=0 0\r\n"
        "m=audio %u RTP/AVP 8 0\r\n"
        "a=rtpmap:8 PCMA/8000\r\n"
        "a=rtpmap:0 PCMU/8000\r\n"
        "a=sendrecv\r\n"
        "a=ptime:20\r\n",
        rt->local_ip, rt->local_ip, (unsigned)cfg.rtp_port);
}

static bool parse_sdp(const char *body, char remote_ip[64], uint16_t *port, int *pt)
{
    if (!body || !remote_ip || !port || !pt) return false;
    const char *c = strstr(body, "c=IN IP4 ");
    const char *m = strstr(body, "m=audio ");
    if (!c || !m) return false;
    if (sscanf(c, "c=IN IP4 %63s", remote_ip) != 1) return false;
    unsigned p = 0;
    int consumed = 0;
    if (sscanf(m, "m=audio %u RTP/AVP %n", &p, &consumed) < 1 || p == 0 || p > 65535) return false;
    bool has8 = false, has0 = false;
    const char *q = m + consumed;
    while (*q && *q != '\r' && *q != '\n') {
        int x = -1, n = 0;
        if (sscanf(q, " %d%n", &x, &n) != 1 || n <= 0) break;
        if (x == 8) has8 = true;
        if (x == 0) has0 = true;
        q += n;
    }
    *port = (uint16_t)p;
    *pt = has8 ? 8 : has0 ? 0 : -1;
    return *pt >= 0;
}

static esp_err_t udp_send(sip_runtime_t *rt, const struct sockaddr_in *dst, const char *msg, size_t len)
{
    if (rt->sock < 0 || !dst || !msg || !len) return ESP_ERR_INVALID_STATE;
    int n = sendto(rt->sock, msg, len, 0, (const struct sockaddr *)dst, sizeof(*dst));
    return n == (int)len ? ESP_OK : ESP_FAIL;
}

static void send_register(sip_runtime_t *rt, bool with_auth)
{
    korvo_sip_config_t cfg;
    lock(); cfg = s_cfg; unlock();
    if (rt->sock < 0 || !rt->server_valid) return;
    char uri[160], branch[48];
    char *auth = s_auth_buf;
    char *msg = s_tx_buf;
    if (!auth || !msg) return;
    auth[0] = 0; msg[0] = 0;
    snprintf(uri, sizeof(uri), "sip:%s", cfg.server);
    if (with_auth) build_auth_header(rt, "REGISTER", uri, auth, SIP_AUTH_MAX);
    snprintf(branch, sizeof(branch), "z9hG4bK%08" PRIx32, esp_random());
    uint32_t cseq = rt->cseq++;
    int n = snprintf(msg, SIP_TX_MAX,
        "REGISTER %s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%u;branch=%s;rport\r\n"
        "Max-Forwards: 70\r\n"
        "From: \"%s\" <sip:%s@%s>;tag=korvo-reg\r\n"
        "To: <sip:%s@%s>\r\n"
        "Call-ID: %s\r\n"
        "CSeq: %" PRIu32 " REGISTER\r\n"
        "Contact: <sip:%s@%s:%u>\r\n"
        "Expires: %u\r\n"
        "User-Agent: KORVO/1.3.18\r\n"
        "%s"
        "Content-Length: 0\r\n\r\n",
        uri, rt->local_ip, (unsigned)cfg.local_port, branch,
        cfg.display_name, cfg.extension, cfg.server,
        cfg.extension, cfg.server, rt->reg_call_id, cseq,
        cfg.extension, rt->local_ip, (unsigned)cfg.local_port,
        (unsigned)cfg.register_expires, auth);
    if (n > 0 && n < SIP_TX_MAX) {
        (void)udp_send(rt, &rt->server_addr, msg, (size_t)n);
        rt->register_auth_sent = with_auth;
        korvo_sip_status_t st = {0};
        korvo_sip_get_status(&st);
        if (!st.registered) set_state(KORVO_SIP_REGISTERING, 0, NULL);
        rt->register_retry_ms = now_ms() + SIP_REG_RETRY_MS;
        ESP_LOGI(TAG, "REGISTER -> %s:%u", cfg.server, (unsigned)cfg.server_port);
    }
}

static void send_invite(sip_runtime_t *rt, bool with_auth)
{
    korvo_sip_config_t cfg;
    lock(); cfg = s_cfg; unlock();
    char branch[48];
    char *sdp = s_sdp_buf;
    char *auth = s_auth_buf;
    char *msg = s_tx_buf;
    if (!sdp || !auth || !msg) return;
    sdp[0] = 0; auth[0] = 0; msg[0] = 0;
    int sdp_len = make_sdp(rt, sdp, SIP_SDP_MAX, -1);
    if (with_auth) build_auth_header(rt, "INVITE", rt->remote_uri, auth, SIP_AUTH_MAX);
    snprintf(branch, sizeof(branch), "z9hG4bK%08" PRIx32, esp_random());
    uint32_t cseq = rt->cseq++;
    int n = snprintf(msg, SIP_TX_MAX,
        "INVITE %s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%u;branch=%s;rport\r\n"
        "Max-Forwards: 70\r\n"
        "From: \"%s\" <sip:%s@%s>;tag=%s\r\n"
        "To: <%s>\r\n"
        "Call-ID: %s\r\n"
        "CSeq: %" PRIu32 " INVITE\r\n"
        "Contact: <sip:%s@%s:%u>\r\n"
        "Allow: INVITE, ACK, CANCEL, BYE, OPTIONS\r\n"
        "User-Agent: KORVO/1.3.18\r\n"
        "%s"
        "Content-Type: application/sdp\r\n"
        "Content-Length: %d\r\n\r\n%s",
        rt->remote_uri, rt->local_ip, (unsigned)cfg.local_port, branch,
        cfg.display_name, cfg.extension, cfg.server, rt->from_tag,
        rt->remote_uri, rt->call_id, cseq,
        cfg.extension, rt->local_ip, (unsigned)cfg.local_port,
        auth, sdp_len, sdp);
    if (n > 0 && n < SIP_TX_MAX) {
        (void)udp_send(rt, &rt->server_addr, msg, (size_t)n);
        rt->invite_auth_sent = with_auth;
        set_state(KORVO_SIP_CALLING, 0, NULL);
        korvo_bluetooth_notify_outgoing(rt->remote_number, false);
        rt->invite_deadline_ms = now_ms() + SIP_INVITE_TO_MS;
        ESP_LOGI(TAG, "INVITE -> %s", rt->remote_uri);
    }
}

static void send_ack(sip_runtime_t *rt, const struct sockaddr_in *dst, uint32_t invite_cseq)
{
    korvo_sip_config_t cfg;
    lock(); cfg = s_cfg; unlock();
    char branch[48];
    char *msg = s_tx_buf;
    if (!msg) return;
    msg[0] = 0;
    snprintf(branch, sizeof(branch), "z9hG4bK%08" PRIx32, esp_random());
    int n = snprintf(msg, SIP_TX_MAX,
        "ACK %s SIP/2.0\r\nVia: SIP/2.0/UDP %s:%u;branch=%s;rport\r\n"
        "Max-Forwards: 70\r\nFrom: \"%s\" <sip:%s@%s>;tag=%s\r\n"
        "To: <%s>%s%s\r\nCall-ID: %s\r\nCSeq: %" PRIu32 " ACK\r\n"
        "Content-Length: 0\r\n\r\n",
        rt->remote_uri, rt->local_ip, (unsigned)cfg.local_port, branch,
        cfg.display_name, cfg.extension, cfg.server, rt->from_tag,
        rt->remote_uri, rt->to_tag[0] ? ";tag=" : "", rt->to_tag,
        rt->call_id, invite_cseq);
    if (n > 0 && n < SIP_TX_MAX) (void)udp_send(rt, dst, msg, (size_t)n);
}

static void send_cancel(sip_runtime_t *rt)
{
    korvo_sip_config_t cfg;
    lock(); cfg = s_cfg; unlock();
    char branch[48];
    char *msg = s_tx_buf;
    if (!msg) return;
    msg[0] = 0;
    snprintf(branch, sizeof(branch), "z9hG4bK%08" PRIx32, esp_random());
    uint32_t cseq = rt->cseq++;
    int n = snprintf(msg, SIP_TX_MAX,
        "CANCEL %s SIP/2.0\r\nVia: SIP/2.0/UDP %s:%u;branch=%s;rport\r\n"
        "Max-Forwards: 70\r\nFrom: \"%s\" <sip:%s@%s>;tag=%s\r\n"
        "To: <%s>%s%s\r\nCall-ID: %s\r\nCSeq: %" PRIu32 " CANCEL\r\n"
        "Content-Length: 0\r\n\r\n",
        rt->remote_uri, rt->local_ip, (unsigned)cfg.local_port, branch,
        cfg.display_name, cfg.extension, cfg.server, rt->from_tag,
        rt->remote_uri, rt->to_tag[0] ? ";tag=" : "", rt->to_tag,
        rt->call_id, cseq);
    if (n > 0 && n < SIP_TX_MAX) (void)udp_send(rt, &rt->server_addr, msg, (size_t)n);
}

static void send_bye(sip_runtime_t *rt)
{
    korvo_sip_config_t cfg;
    lock(); cfg = s_cfg; unlock();
    char branch[48];
    char *msg = s_tx_buf;
    char *auth = s_auth_buf;
    if (!msg || !auth) return;
    msg[0] = 0; auth[0] = 0;
    snprintf(branch, sizeof(branch), "z9hG4bK%08" PRIx32, esp_random());
    build_auth_header(rt, "BYE", rt->remote_uri, auth, SIP_AUTH_MAX);
    uint32_t cseq = rt->cseq++;
    int n = snprintf(msg, SIP_TX_MAX,
        "BYE %s SIP/2.0\r\nVia: SIP/2.0/UDP %s:%u;branch=%s;rport\r\n"
        "Max-Forwards: 70\r\nFrom: \"%s\" <sip:%s@%s>;tag=%s\r\n"
        "To: <%s>%s%s\r\nCall-ID: %s\r\nCSeq: %" PRIu32 " BYE\r\n"
        "User-Agent: KORVO/1.3.18\r\n%sContent-Length: 0\r\n\r\n",
        rt->remote_uri, rt->local_ip, (unsigned)cfg.local_port, branch,
        cfg.display_name, cfg.extension, cfg.server, rt->from_tag,
        rt->remote_uri, rt->to_tag[0] ? ";tag=" : "", rt->to_tag,
        rt->call_id, cseq, auth);
    if (n > 0 && n < SIP_TX_MAX) (void)udp_send(rt, &rt->server_addr, msg, (size_t)n);
}

static void send_response(sip_runtime_t *rt, int code, const char *reason, bool with_sdp)
{
    korvo_sip_config_t cfg;
    lock(); cfg = s_cfg; unlock();
    char tag_part[96] = {0};
    char *msg = s_tx_buf;
    char *sdp = s_sdp_buf;
    if (!msg || !sdp) return;
    msg[0] = 0; sdp[0] = 0;
    int sdp_len = 0;
    if (with_sdp && (rt->rtp_payload_type == 0 || rt->rtp_payload_type == 8))
        sdp_len = make_sdp(rt, sdp, SIP_SDP_MAX, rt->rtp_payload_type);
    if (rt->to_tag[0] && !ci_strstr(rt->last_to, ";tag="))
        snprintf(tag_part, sizeof(tag_part), ";tag=%s", rt->to_tag);
    int n = snprintf(msg, SIP_TX_MAX,
        "SIP/2.0 %d %s\r\nVia: %s\r\nFrom: %s\r\nTo: %s%s\r\n"
        "Call-ID: %s\r\nCSeq: %s\r\nContact: <sip:%s@%s:%u>\r\n"
        "Allow: INVITE, ACK, CANCEL, BYE, OPTIONS\r\nUser-Agent: KORVO/1.3.18\r\n"
        "%sContent-Length: %d\r\n\r\n%s",
        code, reason, rt->last_via, rt->last_from, rt->last_to, tag_part,
        rt->last_call_id, rt->last_cseq,
        cfg.extension, rt->local_ip, (unsigned)cfg.local_port,
        sdp_len ? "Content-Type: application/sdp\r\n" : "",
        sdp_len, sdp_len ? sdp : "");
    if (n > 0 && n < SIP_TX_MAX) (void)udp_send(rt, &rt->last_request_addr, msg, (size_t)n);
}

static void stop_audio(void)
{
    korvo_audio_bridge_stop();
    (void)korvo_bluetooth_audio_disconnect();
}

static void start_audio(sip_runtime_t *rt)
{
    korvo_sip_config_t cfg;
    lock(); cfg = s_cfg; unlock();
    if (!rt->rtp_remote_ip[0] || !rt->rtp_remote_port || (rt->rtp_payload_type != 0 && rt->rtp_payload_type != 8)) return;
    esp_err_t e = korvo_audio_bridge_start(rt->rtp_remote_ip, rt->rtp_remote_port, cfg.rtp_port, rt->rtp_payload_type);
    if (e != ESP_OK) {
        set_state(KORVO_SIP_FAILED, 0, "RTP bridge start failed");
        return;
    }
    /* Publish active-call state to the HF before requesting SCO. Some HF devices
     * close/reject SCO when the AG has not yet announced an active call. */
    korvo_bluetooth_notify_active(rt->remote_number);
    esp_err_t sco = korvo_bluetooth_audio_connect();
    if (sco != ESP_OK && sco != ESP_ERR_INVALID_STATE)
        ESP_LOGW(TAG, "SCO connect request failed: %s", esp_err_to_name(sco));
    set_codec(rt->rtp_payload_type);
}

static void clear_call(sip_runtime_t *rt, korvo_sip_state_t terminal_state, int code)
{
    char ended[KORVO_SIP_NUMBER_MAX]; copy_text(ended, sizeof(ended), rt->remote_number);
    stop_audio();
    korvo_bluetooth_notify_ended(ended);
    rt->call_id[0] = rt->from_tag[0] = rt->to_tag[0] = 0;
    rt->remote_uri[0] = rt->remote_number[0] = 0;
    rt->rtp_remote_ip[0] = 0; rt->rtp_remote_port = 0; rt->rtp_payload_type = -1;
    rt->invite_deadline_ms = 0;
    set_remote("", "");
    set_codec(-1);
    set_state(terminal_state, code, NULL);
}

static void parse_to_tag(const char *to, char *tag, size_t len)
{
    if (!to || !tag || !len) return;
    const char *p = ci_strstr(to, ";tag=");
    if (!p) return;
    p += 5; const char *e = p;
    while (*e && *e != ';' && *e != ' ' && *e != '\r' && *e != '\n') ++e;
    size_t n = (size_t)(e - p); if (n >= len) n = len - 1;
    memcpy(tag, p, n); tag[n] = 0;
}

static void process_response(sip_runtime_t *rt, char *msg, const struct sockaddr_in *src)
{
    int code = 0;
    if (sscanf(msg, "SIP/2.0 %d", &code) != 1) return;
    char cseq[64] = {0}, method[32] = {0}, callid[128] = {0}, to[512] = {0};
    unsigned cseq_num = 0;
    parse_header(msg, "CSeq", cseq, sizeof(cseq));
    parse_header(msg, "Call-ID", callid, sizeof(callid));
    parse_header(msg, "To", to, sizeof(to));
    (void)sscanf(cseq, "%u %31s", &cseq_num, method);

    if (!strcasecmp(method, "REGISTER")) {
        if (code == 200) {
            korvo_sip_config_t cfg; lock(); cfg = s_cfg; unlock();
            set_registered(true);
            rt->auth_valid = false;
            rt->register_auth_sent = false;
            set_state(KORVO_SIP_IDLE, code, NULL);
            rt->next_register_ms = now_ms() + (uint64_t)cfg.register_expires * 800ULL;
            rt->register_retry_ms = 0;
        } else if (code == 401 || code == 407) {
            char challenge[512] = {0};
            const char *hn = code == 407 ? "Proxy-Authenticate" : "WWW-Authenticate";
            if (rt->register_auth_sent) {
                set_registered(false);
                set_state(KORVO_SIP_FAILED, code, "SIP authentication rejected");
                rt->auth_valid = false;
                rt->register_auth_sent = false;
                rt->register_retry_ms = now_ms() + SIP_REG_RETRY_MS;
            } else if (parse_header(msg, hn, challenge, sizeof(challenge))) {
                parse_auth_challenge(rt, challenge, code == 407);
                send_register(rt, true);
            } else {
                set_registered(false); set_state(KORVO_SIP_FAILED, code, "SIP auth challenge missing");
            }
        } else if (code >= 300) {
            set_registered(false); set_state(KORVO_SIP_FAILED, code, "SIP registration failed");
        }
        return;
    }

    if (!strcasecmp(method, "INVITE") && rt->call_id[0] && !strcmp(callid, rt->call_id)) {
        if (code >= 100 && code < 200) {
            if (code == 180 || code == 183) {
                set_state(KORVO_SIP_RINGING, code, NULL);
                korvo_bluetooth_notify_outgoing(rt->remote_number, true);
            }
        } else if (code == 200) {
            parse_to_tag(to, rt->to_tag, sizeof(rt->to_tag));
            char *body = strstr(msg, "\r\n\r\n");
            char rip[64] = {0}; uint16_t rport = 0; int pt = -1;
            if (body && parse_sdp(body + 4, rip, &rport, &pt)) {
                copy_text(rt->rtp_remote_ip, sizeof(rt->rtp_remote_ip), rip);
                rt->rtp_remote_port = rport; rt->rtp_payload_type = pt;
                send_ack(rt, src, cseq_num);
                rt->invite_auth_sent = false;
                set_state(KORVO_SIP_CONNECTED, code, NULL);
                start_audio(rt);
            } else {
                send_ack(rt, src, cseq_num);
                send_bye(rt);
                clear_call(rt, KORVO_SIP_FAILED, 488);
            }
        } else if (code == 401 || code == 407) {
            send_ack(rt, src, cseq_num);
            char challenge[512] = {0};
            const char *hn = code == 407 ? "Proxy-Authenticate" : "WWW-Authenticate";
            if (rt->invite_auth_sent) {
                rt->auth_valid = false;
                rt->invite_auth_sent = false;
                clear_call(rt, KORVO_SIP_FAILED, code);
            } else if (parse_header(msg, hn, challenge, sizeof(challenge))) {
                parse_auth_challenge(rt, challenge, code == 407);
                send_invite(rt, true);
            } else clear_call(rt, KORVO_SIP_FAILED, code);
        } else if (code == 486) clear_call(rt, KORVO_SIP_BUSY, code);
        else if (code == 408 || code == 480) clear_call(rt, KORVO_SIP_NO_ANSWER, code);
        else if (code == 603) clear_call(rt, KORVO_SIP_REJECTED, code);
        else if (code >= 300) clear_call(rt, KORVO_SIP_FAILED, code);
        return;
    }

    if (!strcasecmp(method, "BYE") && code >= 200 && code < 300) {
        clear_call(rt, KORVO_SIP_ENDED, code);
    }
}

static void remember_request(sip_runtime_t *rt, const char *msg, const struct sockaddr_in *src)
{
    rt->last_request_addr = *src;
    parse_header(msg, "Via", rt->last_via, sizeof(rt->last_via));
    parse_header(msg, "From", rt->last_from, sizeof(rt->last_from));
    parse_header(msg, "To", rt->last_to, sizeof(rt->last_to));
    parse_header(msg, "Call-ID", rt->last_call_id, sizeof(rt->last_call_id));
    parse_header(msg, "CSeq", rt->last_cseq, sizeof(rt->last_cseq));
}

static void process_request(sip_runtime_t *rt, char *msg, const struct sockaddr_in *src)
{
    char method[32] = {0};
    if (sscanf(msg, "%31s", method) != 1) return;
    remember_request(rt, msg, src);

    if (!strcmp(method, "OPTIONS")) {
        send_response(rt, 200, "OK", false);
        return;
    }

    if (!strcmp(method, "INVITE")) {
        korvo_sip_state_t state; lock(); state = s_status.state; unlock();
        if (state != KORVO_SIP_IDLE && state != KORVO_SIP_ENDED && state != KORVO_SIP_FAILED && state != KORVO_SIP_BUSY && state != KORVO_SIP_REJECTED && state != KORVO_SIP_NO_ANSWER) {
            send_response(rt, 486, "Busy Here", false); return;
        }
        char from[512] = {0}; parse_header(msg, "From", from, sizeof(from));
        copy_text(rt->call_id, sizeof(rt->call_id), rt->last_call_id);
        extract_uri_and_number(from, rt->remote_uri, sizeof(rt->remote_uri), rt->remote_number, sizeof(rt->remote_number));
        snprintf(rt->to_tag, sizeof(rt->to_tag), "%08" PRIx32, esp_random());
        char *body = strstr(msg, "\r\n\r\n");
        char rip[64] = {0}; uint16_t rport = 0; int pt = -1;
        if (!body || !parse_sdp(body + 4, rip, &rport, &pt)) {
            send_response(rt, 488, "Not Acceptable Here", false); return;
        }
        copy_text(rt->rtp_remote_ip, sizeof(rt->rtp_remote_ip), rip);
        rt->rtp_remote_port = rport; rt->rtp_payload_type = pt;
        set_remote(rt->remote_uri, rt->remote_number);
        set_codec(pt);
        send_response(rt, 100, "Trying", false);
        send_response(rt, 180, "Ringing", false);
        set_state(KORVO_SIP_INCOMING, 180, NULL);
        korvo_bluetooth_notify_incoming(rt->remote_number);
        return;
    }

    if (!strcmp(method, "ACK")) {
        korvo_sip_state_t state; lock(); state = s_status.state; unlock();
        if (state == KORVO_SIP_CONNECTING) {
            set_state(KORVO_SIP_CONNECTED, 200, NULL);
            start_audio(rt);
        }
        return;
    }

    if (!strcmp(method, "BYE")) {
        send_response(rt, 200, "OK", false);
        clear_call(rt, KORVO_SIP_ENDED, 200);
        return;
    }

    if (!strcmp(method, "CANCEL")) {
        korvo_sip_state_t state; lock(); state = s_status.state; unlock();
        if (state == KORVO_SIP_INCOMING) {
            send_response(rt, 200, "OK", false);
            send_response(rt, 487, "Request Terminated", false);
            clear_call(rt, KORVO_SIP_ENDED, 487);
        } else send_response(rt, 481, "Call/Transaction Does Not Exist", false);
        return;
    }

    send_response(rt, 501, "Not Implemented", false);
}

static void process_packet(sip_runtime_t *rt, char *msg, int len, const struct sockaddr_in *src)
{
    if (!msg || len <= 0) return;
    msg[len] = 0;
    if (!strncmp(msg, "SIP/2.0", 7)) process_response(rt, msg, src);
    else process_request(rt, msg, src);
}

static void do_call(sip_runtime_t *rt, const char *number_or_uri)
{
    korvo_sip_status_t st; korvo_sip_get_status(&st);
    if (!st.registered || (st.state != KORVO_SIP_IDLE && st.state != KORVO_SIP_ENDED && st.state != KORVO_SIP_FAILED && st.state != KORVO_SIP_BUSY && st.state != KORVO_SIP_REJECTED && st.state != KORVO_SIP_NO_ANSWER)) {
        set_state(st.state, 0, "SIP not ready to call"); return;
    }
    build_target_uri(number_or_uri, rt->remote_uri, sizeof(rt->remote_uri));
    extract_uri_and_number(rt->remote_uri, NULL, 0, rt->remote_number, sizeof(rt->remote_number));
    set_remote(rt->remote_uri, rt->remote_number);
    snprintf(rt->call_id, sizeof(rt->call_id), "%08" PRIx32 "%08" PRIx32 "@%s", esp_random(), esp_random(), rt->local_ip);
    snprintf(rt->from_tag, sizeof(rt->from_tag), "%08" PRIx32, esp_random());
    rt->to_tag[0] = 0; rt->rtp_remote_ip[0] = 0; rt->rtp_remote_port = 0; rt->rtp_payload_type = -1;
    rt->auth_valid = false;
    rt->invite_auth_sent = false;
    send_invite(rt, false);
}

static void handle_cmd(sip_runtime_t *rt, const sip_cmd_t *cmd)
{
    if (!cmd) return;
    switch (cmd->type) {
        case CMD_RECONFIGURE:
            set_registered(false); stop_audio(); close_socket(rt);
            memset(rt, 0, sizeof(*rt)); rt->sock = -1; rt->rtp_payload_type = -1; rt->cseq = 1;
            snprintf(rt->reg_call_id, sizeof(rt->reg_call_id), "%08" PRIx32 "%08" PRIx32, esp_random(), esp_random());
            break;
        case CMD_REGISTER:
            rt->next_register_ms = 0; rt->register_retry_ms = 0;
            if (rt->sock >= 0) send_register(rt, false);
            break;
        case CMD_CALL: do_call(rt, cmd->arg); break;
        case CMD_ANSWER: {
            korvo_sip_state_t st; lock(); st = s_status.state; unlock();
            if (st == KORVO_SIP_INCOMING) {
                send_response(rt, 200, "OK", true);
                set_state(KORVO_SIP_CONNECTING, 200, NULL);
            }
            break;
        }
        case CMD_REJECT: {
            korvo_sip_state_t st; lock(); st = s_status.state; unlock();
            if (st == KORVO_SIP_INCOMING) {
                send_response(rt, 486, "Busy Here", false);
                clear_call(rt, KORVO_SIP_REJECTED, 486);
            }
            break;
        }
        case CMD_HANGUP: {
            korvo_sip_state_t st; lock(); st = s_status.state; unlock();
            if (st == KORVO_SIP_INCOMING) {
                send_response(rt, 486, "Busy Here", false);
                clear_call(rt, KORVO_SIP_REJECTED, 486);
            } else if (st == KORVO_SIP_CALLING || st == KORVO_SIP_RINGING) {
                send_cancel(rt); clear_call(rt, KORVO_SIP_ENDED, 0);
            } else if (st == KORVO_SIP_CONNECTING || st == KORVO_SIP_CONNECTED) {
                send_bye(rt); clear_call(rt, KORVO_SIP_ENDED, 0);
            }
            break;
        }
        default: break;
    }
}

static void sip_task(void *arg)
{
    (void)arg;
    sip_runtime_t *rt = s_rt;
    char *rx = s_rx_buf;
    if (!rt || !rx) {
        ESP_LOGE(TAG, "SIP work buffers unavailable");
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    memset(rt, 0, sizeof(*rt));
    rt->sock = -1; rt->rtp_payload_type = -1; rt->cseq = 1;
    snprintf(rt->reg_call_id, sizeof(rt->reg_call_id), "%08" PRIx32 "%08" PRIx32, esp_random(), esp_random());

    for (;;) {
        sip_cmd_t cmd;
        while (xQueueReceive(s_cmd_q, &cmd, 0) == pdTRUE) handle_cmd(rt, &cmd);

        korvo_sip_config_t cfg; lock(); cfg = s_cfg; unlock();
        char ip[16] = {0}; bool net = network_ready(ip);
        lock(); s_status.enabled = cfg.enabled; s_status.network_ready = net; unlock();

        if (!cfg.enabled) {
            if (rt->sock >= 0) { close_socket(rt); stop_audio(); }
            set_registered(false); set_state(KORVO_SIP_DISABLED, 0, NULL);
            vTaskDelay(pdMS_TO_TICKS(250)); continue;
        }
        if (!net) {
            if (rt->sock >= 0) { close_socket(rt); stop_audio(); }
            set_registered(false); set_state(KORVO_SIP_OFFLINE, 0, NULL);
            vTaskDelay(pdMS_TO_TICKS(500)); continue;
        }
        if (rt->sock < 0) {
            esp_err_t e = open_socket(rt);
            if (e != ESP_OK) {
                set_registered(false); set_state(KORVO_SIP_OFFLINE, 0, esp_err_to_name(e));
                vTaskDelay(pdMS_TO_TICKS(1000)); continue;
            }
            rt->auth_valid = false;
            send_register(rt, false);
        }

        uint64_t now = now_ms();
        korvo_sip_status_t st; korvo_sip_get_status(&st);
        if (st.registered && rt->next_register_ms && now >= rt->next_register_ms) send_register(rt, false);
        else if (!st.registered && rt->register_retry_ms && now >= rt->register_retry_ms) send_register(rt, rt->auth_valid);
        if ((st.state == KORVO_SIP_CALLING || st.state == KORVO_SIP_RINGING) && rt->invite_deadline_ms && now >= rt->invite_deadline_ms) {
            send_cancel(rt); clear_call(rt, KORVO_SIP_NO_ANSWER, 408);
        }

        /* During an established RTP call SCO is mandatory. Recover it without
         * disturbing the SLC or RTP bridge if the HF drops/rejects the first open. */
        static uint64_t next_sco_retry_ms = 0;
        if (st.state == KORVO_SIP_CONNECTED) {
            korvo_audio_bridge_status_t ab = {0};
            korvo_bluetooth_status_t bt = {0};
            korvo_audio_bridge_get_status(&ab);
            korvo_bluetooth_get_status(&bt);
            if (ab.running && bt.slc_connected && !bt.audio_connected && now >= next_sco_retry_ms) {
                esp_err_t ae = korvo_bluetooth_audio_connect();
                if (ae != ESP_OK && ae != ESP_ERR_INVALID_STATE)
                    ESP_LOGW(TAG, "SCO recovery request failed: %s", esp_err_to_name(ae));
                next_sco_retry_ms = now + 1500;
            } else if (bt.audio_connected) {
                next_sco_retry_ms = 0;
            }
        } else {
            next_sco_retry_ms = 0;
        }

        fd_set rfds; FD_ZERO(&rfds); FD_SET(rt->sock, &rfds);
        struct timeval tv = {.tv_sec = 0, .tv_usec = SIP_SELECT_MS * 1000};
        int rc = select(rt->sock + 1, &rfds, NULL, NULL, &tv);
        if (rc > 0 && FD_ISSET(rt->sock, &rfds)) {
            struct sockaddr_in src = {0}; socklen_t sl = sizeof(src);
            int n = recvfrom(rt->sock, rx, SIP_RX_MAX, 0, (struct sockaddr *)&src, &sl);
            if (n > 0) process_packet(rt, rx, n, &src);
        } else if (rc < 0 && errno != EINTR) {
            ESP_LOGW(TAG, "SIP socket error %d", errno);
            close_socket(rt); set_registered(false); set_state(KORVO_SIP_OFFLINE, 0, "SIP socket error");
        }
    }
}

static esp_err_t ensure_sip_task_started(void)
{
    if (s_task) return ESP_OK;
    if (!s_cmd_q) s_cmd_q = xQueueCreate(SIP_CMD_Q_LEN, sizeof(sip_cmd_t));
    if (!s_cmd_q) return ESP_ERR_NO_MEM;
    esp_err_t mem_err = sip_work_alloc_all();
    if (mem_err != ESP_OK) {
        vQueueDelete(s_cmd_q);
        s_cmd_q = NULL;
        return mem_err;
    }
    if (xTaskCreate(sip_task, "korvo_sip", SIP_TASK_STACK, NULL, SIP_TASK_PRIO, &s_task) != pdPASS) {
        vQueueDelete(s_cmd_q);
        s_cmd_q = NULL;
        sip_work_free_all();
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "SIP task started on demand (%u byte stack)", (unsigned)SIP_TASK_STACK);
    return ESP_OK;
}

static esp_err_t queue_cmd(sip_cmd_type_t type, const char *arg)
{
    if (!s_cmd_q) return ESP_ERR_INVALID_STATE;
    sip_cmd_t cmd = {.type = type};
    copy_text(cmd.arg, sizeof(cmd.arg), arg);
    return xQueueSend(s_cmd_q, &cmd, pdMS_TO_TICKS(50)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t korvo_sip_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    ESP_RETURN_ON_ERROR(load_cfg(), TAG, "load sip config");
    memset(&s_status, 0, sizeof(s_status));
    s_status.engine_ready = true; s_status.enabled = s_cfg.enabled;
    s_status.state = s_cfg.enabled ? KORVO_SIP_OFFLINE : KORVO_SIP_DISABLED;
    copy_text(s_status.negotiated_codec, sizeof(s_status.negotiated_codec), "--");

    /* Keep the 9 KB SIP stack out of scarce internal RAM until SIP is
     * actually enabled. The web UI can enable it later; save_config()
     * starts the task on demand. */
    if (!s_cfg.enabled) {
        ESP_LOGI(TAG, "SIP disabled - task deferred to preserve internal RAM");
        return ESP_OK;
    }
    return ensure_sip_task_started();
}

esp_err_t korvo_sip_get_config(korvo_sip_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    lock(); *cfg = s_cfg; unlock();
    return ESP_OK;
}

esp_err_t korvo_sip_save_config(const korvo_sip_config_t *cfg)
{
    if (!valid_cfg(cfg)) return ESP_ERR_INVALID_ARG;
    korvo_sip_config_t c = *cfg;
    c.server[sizeof(c.server)-1] = 0; c.extension[sizeof(c.extension)-1] = 0;
    c.username[sizeof(c.username)-1] = 0; c.password[sizeof(c.password)-1] = 0;
    c.display_name[sizeof(c.display_name)-1] = 0; c.operator_extension[sizeof(c.operator_extension)-1] = 0;
    lock(); s_cfg = c; s_status.enabled = c.enabled; unlock();
    ESP_RETURN_ON_ERROR(persist_cfg(), TAG, "save sip config");

    if (c.enabled) {
        ESP_RETURN_ON_ERROR(ensure_sip_task_started(), TAG, "start sip task");
        return queue_cmd(CMD_RECONFIGURE, NULL);
    }

    /* If SIP was never enabled there is no task/queue to notify. If it was
     * running, let it process the disable and close its socket/audio path. */
    return s_task ? queue_cmd(CMD_RECONFIGURE, NULL) : ESP_OK;
}

void korvo_sip_get_status(korvo_sip_status_t *status)
{
    if (!status) return;
    lock(); *status = s_status; unlock();
}

size_t korvo_sip_status_json(char *out, size_t out_len)
{
    if (!out || !out_len) return 0;
    korvo_sip_status_t s; korvo_sip_get_status(&s);
    int n = snprintf(out, out_len,
        "{\"engine_ready\":%s,\"enabled\":%s,\"network_ready\":%s,\"registered\":%s,"
        "\"state\":\"%s\",\"last_code\":%d,\"remote_uri\":\"%s\",\"remote_number\":\"%s\","
        "\"codec\":\"%s\",\"last_error\":\"%s\"}",
        s.engine_ready?"true":"false", s.enabled?"true":"false", s.network_ready?"true":"false",
        s.registered?"true":"false", korvo_sip_state_name(s.state), s.last_code,
        s.remote_uri, s.remote_number, s.negotiated_codec, s.last_error);
    if (n < 0) return 0;
    return (size_t)n < out_len ? (size_t)n : out_len - 1;
}

esp_err_t korvo_sip_add_listener(korvo_sip_listener_t listener, void *ctx)
{
    if (!listener || !s_lock) return ESP_ERR_INVALID_ARG;
    lock();
    for (size_t i = 0; i < SIP_LISTENER_MAX; ++i) {
        if (s_listeners[i].fn == listener && s_listeners[i].ctx == ctx) { unlock(); return ESP_OK; }
        if (!s_listeners[i].fn) { s_listeners[i].fn = listener; s_listeners[i].ctx = ctx; unlock(); return ESP_OK; }
    }
    unlock(); return ESP_ERR_NO_MEM;
}

esp_err_t korvo_sip_call_default(void)
{
    korvo_sip_config_t cfg; korvo_sip_get_config(&cfg);
    return korvo_sip_call(cfg.operator_extension);
}

esp_err_t korvo_sip_call(const char *number_or_uri)
{
    if (!number_or_uri || !clean_token(number_or_uri)) return ESP_ERR_INVALID_ARG;
    korvo_sip_status_t s; korvo_sip_get_status(&s);
    if (!s.enabled || !s.registered) return ESP_ERR_INVALID_STATE;
    return queue_cmd(CMD_CALL, number_or_uri);
}

esp_err_t korvo_sip_answer(void) { return queue_cmd(CMD_ANSWER, NULL); }
esp_err_t korvo_sip_reject(void) { return queue_cmd(CMD_REJECT, NULL); }
esp_err_t korvo_sip_hangup(void) { return queue_cmd(CMD_HANGUP, NULL); }
esp_err_t korvo_sip_force_register(void) { return queue_cmd(CMD_REGISTER, NULL); }
