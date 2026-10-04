#include "korvo_web.h"
#include "korvo_network.h"
#include "korvo_gateway.h"
#include "korvo_storage.h"
#include "korvo_update.h"
#include "korvo_sip.h"
#include "korvo_bluetooth.h"
#include "korvo_audio_bridge.h"
#include "korvo_alarm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


static const char *TAG = "korvo_web";

static httpd_handle_t s_server = NULL;

static void web_diag(const char *route)
{
    ESP_LOGI(TAG, "HTTP %s stack_hwm=%u heap=%u",
             route ? route : "?",
             (unsigned)uxTaskGetStackHighWaterMark(NULL),
             (unsigned)esp_get_free_heap_size());
}

#define ADMIN_USER   "admin"
#define ADMIN_PASS   "password"
#define COOKIE       "KORVO_SESSION=korvo_admin"
#define UPLOAD_CHUNK 16384


static const char *STYLE =
    "<style>"
    "*{box-sizing:border-box}"
    "body{margin:0;background:#10151b;color:#e9eef4;font-family:Arial}"
    ".top{background:#171f28;padding:18px 24px}"
    ".wrap{max-width:960px;margin:24px auto;padding:0 16px}"
    ".card{background:#18212b;border:1px solid #34404d;"
    "border-radius:10px;padding:20px;margin-bottom:18px}"
    "a{color:#8ecbff;text-decoration:none;margin-right:18px}"
    "label{display:block;margin:13px 0 5px;color:#aebac6}"
    "input,select{width:100%;padding:11px;border:1px solid #445363;"
    "border-radius:6px;background:#0f151b;color:white}"
    "input[type=checkbox]{width:auto}"
    ".btn{margin-top:18px;padding:11px 18px;border:0;"
    "border-radius:6px;font-weight:bold;cursor:pointer}"
    ".danger{background:#a53a3a;color:white}"
    ".grid{display:grid;grid-template-columns:190px 1fr;gap:9px 14px}"
    ".mono{font-family:monospace}"
    ".ok{color:#74d680}"
    ".bad{color:#ff7777}"
    ".small{font-size:13px;color:#9eabb8}"
    ".bar{height:16px;background:#0f151b;border-radius:8px;overflow:hidden}"
    ".fill{height:100%;background:#74d680;width:0}"
    "</style>";


static bool auth(httpd_req_t *r)
{
    size_t n =
        httpd_req_get_hdr_value_len(
            r,
            "Cookie");

    if (!n || n > 255) {
        return false;
    }

    char b[256];

    return
        httpd_req_get_hdr_value_str(
            r,
            "Cookie",
            b,
            sizeof(b)) == ESP_OK &&
        strstr(b, COOKIE) != NULL;
}


static esp_err_t redir(
    httpd_req_t *r,
    const char *p)
{
    httpd_resp_set_status(
        r,
        "303 See Other");

    httpd_resp_set_hdr(
        r,
        "Location",
        p);

    return httpd_resp_send(
        r,
        NULL,
        0);
}


static bool guard(httpd_req_t *r)
{
    if (auth(r)) {
        return true;
    }

    redir(r, "/login");

    return false;
}


static void decode(
    char *d,
    size_t z,
    const char *s)
{
    size_t i = 0;

    while (*s && i + 1 < z) {

        if (*s == '%' &&
            s[1] &&
            s[2]) {

            char h[3] = {
                s[1],
                s[2],
                0
            };

            d[i++] =
                (char)strtol(
                    h,
                    NULL,
                    16);

            s += 3;

        } else if (*s == '+') {

            d[i++] = ' ';
            s++;

        } else {

            d[i++] = *s++;
        }
    }

    d[i] = 0;
}


static bool val(
    const char *b,
    const char *k,
    char *o,
    size_t z)
{
    size_t kl = strlen(k);
    const char *p = b;

    while (p && *p) {

        if ((p == b || p[-1] == '&') &&
            !strncmp(p, k, kl) &&
            p[kl] == '=') {

            p += kl + 1;

            const char *e =
                strchr(p, '&');

            size_t n =
                e ? (size_t)(e - p) : strlen(p);

            char x[160];

            if (n >= sizeof(x)) {
                n = sizeof(x) - 1;
            }

            memcpy(
                x,
                p,
                n);

            x[n] = 0;

            decode(
                o,
                z,
                x);

            return true;
        }

        p = strchr(
            p,
            '&');

        if (p) {
            p++;
        }
    }

    return false;
}


static esp_err_t body(
    httpd_req_t *r,
    char *b,
    size_t z)
{
    if (r->content_len <= 0 ||
        (size_t)r->content_len >= z) {

        return ESP_ERR_INVALID_SIZE;
    }

    int n = 0;

    while (n < r->content_len) {

        int x =
            httpd_req_recv(
                r,
                b + n,
                r->content_len - n);

        if (x <= 0) {
            return ESP_FAIL;
        }

        n += x;
    }

    b[n] = 0;

    return ESP_OK;
}


static void head(
    httpd_req_t *r,
    const char *t)
{
    char b[160];

    httpd_resp_set_type(
        r,
        "text/html");

    httpd_resp_sendstr_chunk(
        r,
        "<!doctype html><html><head>"
        "<meta name=viewport "
        "content='width=device-width,initial-scale=1'>");

    httpd_resp_sendstr_chunk(
        r,
        STYLE);

    snprintf(
        b,
        sizeof(b),
        "<title>KORVO - %s</title>"
        "</head><body>",
        t);

    httpd_resp_sendstr_chunk(
        r,
        b);

    httpd_resp_sendstr_chunk(
        r,
        "<div class=top>"
        "<b>KORVO</b> &nbsp; "
        "<a href='/'>ESTADO</a>"
        "<a href='/network'>RED</a>"
        "<a href='/gateway'>GATEWAY</a>"
        "<a href='/intercom'>INTERCOM</a>"
        "<a href='/firmware'>FIRMWARE</a>"
        "<a href='/logout'>SALIR</a>"
        "</div>"
        "<div class=wrap>");
}


static void foot(httpd_req_t *r)
{
    httpd_resp_sendstr_chunk(
        r,
        "</div></body></html>");

    httpd_resp_sendstr_chunk(
        r,
        NULL);
}


/* ============================================================
 * LOGIN
 * ============================================================ */

static esp_err_t login_g(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html");

    httpd_resp_sendstr_chunk(
        r,
        "<!doctype html><html><head>"
        "<meta name=viewport "
        "content='width=device-width,initial-scale=1'>");

    httpd_resp_sendstr_chunk(r, STYLE);

    httpd_resp_sendstr_chunk(
        r,
        "</head><body>"
        "<div class=top><b>KORVO</b></div>"
        "<div class=wrap>"
        "<div class=card>"
        "<h2>Administracion</h2>"
        "<form method=post action=/login>"
        "<label>Usuario</label>"
        "<input name=user>"
        "<label>Clave</label>"
        "<input name=pass type=password>"
        "<button class=btn>ENTRAR</button>"
        "</form>"
        "</div>"
        "</div>"
        "</body></html>");

    return httpd_resp_sendstr_chunk(r, NULL);
}


static esp_err_t login_p(httpd_req_t *r)
{
    char b[256];
    char u[64] = {0};
    char p[64] = {0};

    if (body(r, b, sizeof(b)) != ESP_OK ||
        !val(b, "user", u, sizeof(u)) ||
        !val(b, "pass", p, sizeof(p))) {

        return httpd_resp_send_err(
            r,
            HTTPD_400_BAD_REQUEST,
            "Solicitud invalida");
    }

    if (strcmp(u, ADMIN_USER) ||
        strcmp(p, ADMIN_PASS)) {

        httpd_resp_set_status(
            r,
            "401 Unauthorized");

        return httpd_resp_sendstr(
            r,
            "Usuario o clave incorrectos");
    }

    httpd_resp_set_hdr(
        r,
        "Set-Cookie",
        COOKIE "; Path=/; HttpOnly; SameSite=Strict");

    return redir(
        r,
        "/");
}


static esp_err_t logout_g(httpd_req_t *r)
{
    httpd_resp_set_hdr(
        r,
        "Set-Cookie",
        "KORVO_SESSION=; Path=/; Max-Age=0");

    return redir(
        r,
        "/login");
}


/* ============================================================
 * ESTADO
 * ============================================================ */

static esp_err_t status_g(httpd_req_t *r)
{
    if (!guard(r)) {
        return ESP_OK;
    }

    korvo_network_status_t s;
    korvo_network_get_status(&s);

    const esp_app_desc_t *a =
        esp_app_get_description();

    korvo_update_info_t ui = {0};
    korvo_update_get_info(&ui);

    char m[24];
    const size_t bz = 3000;
    char *b = malloc(bz);
    if (!b) {
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Sin memoria web");
    }
    web_diag("STATUS begin");

    snprintf(
        m,
        sizeof(m),
        "%02X:%02X:%02X:%02X:%02X:%02X",
        s.mac[0],
        s.mac[1],
        s.mac[2],
        s.mac[3],
        s.mac[4],
        s.mac[5]);

    head(
        r,
        "Estado");

    snprintf(
        b,
        bz,
        "<div class=card>"
        "<h2>Estado</h2>"
        "<div class=grid>"

        "<div>Equipo</div>"
        "<div><b>KORVO</b></div>"

        "<div>Firmware</div>"
        "<div class=mono>%s</div>"

        "<div>Proyecto</div>"
        "<div class=mono>%s</div>"

        "<div>ESP-IDF</div>"
        "<div class=mono>%s</div>"

        "<div>Uptime</div>"
        "<div>%llu s</div>"

        "<div>SD</div>"
        "<div class=%s>%s</div>"

        "<div>Update pendiente</div>"
        "<div class=%s>%s</div>"

        "<div>Wi-Fi STA</div>"
        "<div class=%s>%s</div>"

        "<div>SSID</div>"
        "<div class=mono>%s</div>"

        "<div>Gateway LAN</div>"
        "<div class=mono>%s</div>"

        "<div>IP KORVO</div>"
        "<div class=mono>%s</div>"

        "<div>Gateway sugerido (ENROLL)</div>"
        "<div class=mono>%s</div>"

        "<div>DNS</div>"
        "<div class=mono>%s</div>"

        "<div>MAC</div>"
        "<div class=mono>%s</div>"

        "</div>"
        "</div>",

        a ? a->version : "--",
        a ? a->project_name : "--",
        a ? a->idf_ver : "--",

        (unsigned long long)
            (esp_timer_get_time() / 1000000ULL),

        korvo_storage_is_ready()
            ? "ok"
            : "bad",

        korvo_storage_is_ready()
            ? "READY"
            : "NO DISPONIBLE",

        ui.pending
            ? "ok"
            : "",

        ui.pending
            ? "SI - se instalara al reiniciar"
            : "NO",

        s.connected
            ? "ok"
            : "bad",

        s.connected
            ? "CONECTADO"
            : "DESCONECTADO",

        s.config.ssid,
        s.config.network_gateway,
        s.config.ip,
        s.config.gateway_node_ip,
        s.config.dns,

        m);

    httpd_resp_sendstr_chunk(
        r,
        b);

    korvo_gateway_status_t gw = {0};
    korvo_gateway_get_status(&gw);
    snprintf(b, bz,
        "<div class=card><h2>Gateway V2.5</h2><div class=grid>"
        "<div>Host / Sala KORVO</div><div class=mono>%s</div>"
        "<div>IP Gateway</div><div class=mono>%s</div>"
        "<div>WebSocket</div><div class=%s>%s</div>"
        "<div>PAIR</div><div class=%s>%s</div>"
        "<div>CONTROL GPIO</div><div class=%s>%s</div>"
        "</div><p><a href='/gateway'>Configurar Gateway IP</a></p></div>",
        gw.local.hostname,
        gw.expected_gateway.ip[0] ? gw.expected_gateway.ip : "NO CONFIGURADO",
        gw.websocket_connected ? "ok" : "bad", gw.websocket_connected ? "CONNECTED" : "DISCONNECTED",
        gw.gateway_ok ? "ok" : "bad", gw.gateway_ok ? "PAIRED" : "NOT PAIRED",
        gw.control_ok ? "ok" : "bad", gw.control_ok ? "ENABLED" : "BLOCKED");
    httpd_resp_sendstr_chunk(r, b);

    korvo_sip_status_t sip = {0};
    korvo_bluetooth_status_t bt = {0};
    korvo_audio_bridge_status_t audio = {0};
    korvo_sip_get_status(&sip);
    korvo_bluetooth_get_status(&bt);
    korvo_audio_bridge_get_status(&audio);
    snprintf(b, bz,
        "<div class=card><h2>Intercom KORVO</h2><div class=grid>"
        "<div>SIP</div><div class=%s>%s / %s</div>"
        "<div>Remoto</div><div class=mono>%s</div>"
        "<div>Codec RTP</div><div>%s</div>"
        "<div>Bluetooth HFP AG</div><div class=%s>%s</div>"
        "<div>Dispositivo HF</div><div class=mono>%s %s</div>"
        "<div>Audio SCO</div><div class=%s>%s / %s</div>"
        "<div>RTP bridge</div><div class=%s>%s</div>"
        "</div><p><a href='/intercom'>Configurar SIP + Bluetooth</a></p></div>",
        sip.registered ? "ok" : "bad", sip.registered ? "REGISTERED" : "NOT REGISTERED", korvo_sip_state_name(sip.state),
        sip.remote_number[0] ? sip.remote_number : "--",
        sip.negotiated_codec[0] ? sip.negotiated_codec : "--",
        bt.slc_connected ? "ok" : "bad", bt.slc_connected ? "CONNECTED" : (bt.enabled ? "READY" : "DISABLED"),
        bt.peer_name[0] ? bt.peer_name : "--", bt.peer_mac[0] ? bt.peer_mac : "",
        bt.audio_connected ? "ok" : "bad", bt.audio_connected ? "CONNECTED" : "DISCONNECTED", bt.codec[0] ? bt.codec : "--",
        audio.running ? "ok" : "bad", audio.running ? "RUNNING" : "STOPPED");
    httpd_resp_sendstr_chunk(r, b);

    free(b);
    web_diag("STATUS end");
    foot(r);

    return ESP_OK;
}


/* ============================================================
 * RED
 * ============================================================ */

static esp_err_t net_g(httpd_req_t *r)
{
    if (!guard(r)) {
        return ESP_OK;
    }

    korvo_network_config_t c;
    korvo_network_get_config(&c);

    char b[2400];

    head(
        r,
        "Red");

    snprintf(
        b,
        sizeof(b),

        "<div class=card>"
        "<h2>Red KORVO</h2>"
        "<p>KORVO = cliente Wi-Fi STA. "
        "El AP/bridge puede ser el Gateway KORVO, "
        "un router, un access point o cualquier equipo compatible.</p>"

        "<form method=post action=/network>"

        "<label>SSID del AP / bridge</label>"
        "<input name=ssid maxlength=32 value='%s'>"

        "<label>Clave Wi-Fi</label>"
        "<input name=password type=password maxlength=63 value='%s'>"

        "<label>IP fija KORVO</label>"
        "<input name=ip maxlength=15 value='%s'>"

        "<label>Mascara</label>"
        "<input name=netmask maxlength=15 value='%s'>"

        "<label>Gateway de la LAN</label>"
        "<input name=network_gateway maxlength=15 value='%s'>"

        "<label>DNS</label>"
        "<input name=dns maxlength=15 value='%s'>"

        "<label>IP Gateway sugerida para ENROLL</label>"
        "<input name=gateway_node maxlength=15 value='%s'>"

        "<button class=btn>"
        "GUARDAR Y REINICIAR"
        "</button>"

        "</form>"

        "<p class=small>"
        "Laboratorio: korvo_wifi / "
        "KORVO 192.168.10.50 / "
        "LAN GW 192.168.10.1 / "
        "Gateway sugerido 192.168.10.51 (no hardcoded para control)"
        "</p>"

        "</div>"

        "<div class=card>"
        "<form method=post action=/factory-network>"
        "<button class=btn>"
        "RESTAURAR RED DE FABRICA"
        "</button>"
        "</form>"
        "</div>",

        c.ssid,
        c.password,
        c.ip,
        c.netmask,
        c.network_gateway,
        c.dns,
        c.gateway_node_ip);

    httpd_resp_sendstr_chunk(
        r,
        b);

    foot(r);

    return ESP_OK;
}


static esp_err_t net_p(httpd_req_t *r)
{
    if (!guard(r)) {
        return ESP_OK;
    }

    char b[768];
    char x[128];

    if (body(
            r,
            b,
            sizeof(b)) != ESP_OK) {

        return httpd_resp_send_err(
            r,
            HTTPD_400_BAD_REQUEST,
            "Solicitud invalida");
    }

    korvo_network_config_t c;
    korvo_network_get_config(&c);

    if (val(
            b,
            "ssid",
            x,
            sizeof(x))) {

        strlcpy(
            c.ssid,
            x,
            sizeof(c.ssid));
    }

    if (val(
            b,
            "password",
            x,
            sizeof(x))) {

        strlcpy(
            c.password,
            x,
            sizeof(c.password));
    }

    if (val(
            b,
            "ip",
            x,
            sizeof(x))) {

        strlcpy(
            c.ip,
            x,
            sizeof(c.ip));
    }

    if (val(
            b,
            "netmask",
            x,
            sizeof(x))) {

        strlcpy(
            c.netmask,
            x,
            sizeof(c.netmask));
    }

    if (val(
            b,
            "network_gateway",
            x,
            sizeof(x))) {

        strlcpy(
            c.network_gateway,
            x,
            sizeof(c.network_gateway));
    }

    if (val(
            b,
            "dns",
            x,
            sizeof(x))) {

        strlcpy(
            c.dns,
            x,
            sizeof(c.dns));
    }

    if (val(
            b,
            "gateway_node",
            x,
            sizeof(x))) {

        strlcpy(
            c.gateway_node_ip,
            x,
            sizeof(c.gateway_node_ip));
    }

    if (korvo_network_save_config(&c) != ESP_OK) {

        return httpd_resp_send_err(
            r,
            HTTPD_400_BAD_REQUEST,
            "Configuracion invalida");
    }

    httpd_resp_sendstr(
        r,
        "<html><body>"
        "<h2>KORVO</h2>"
        "<p>Configuracion guardada. "
        "Reiniciando...</p>"
        "</body></html>");

    vTaskDelay(
        pdMS_TO_TICKS(700));

    esp_restart();

    return ESP_OK;
}


static esp_err_t factory_p(httpd_req_t *r)
{
    if (!guard(r)) {
        return ESP_OK;
    }

    if (korvo_network_factory_defaults() != ESP_OK) {

        return httpd_resp_send_err(
            r,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Error restaurando red");
    }

    httpd_resp_sendstr(
        r,
        "Red restaurada. Reiniciando...");

    vTaskDelay(
        pdMS_TO_TICKS(700));

    esp_restart();

    return ESP_OK;
}


/* ============================================================
 * GATEWAY V2.5 / SIMPLE IP PAIRING
 * ============================================================ */

static esp_err_t identity_api_g(httpd_req_t *r)
{
    korvo_gateway_status_t gw = {0};
    korvo_gateway_get_status(&gw);
    const esp_app_desc_t *a = esp_app_get_description();
    char j[768];
    snprintf(j, sizeof(j),
             "{\"role\":\"korvo\",\"name\":\"%s\",\"hostname\":\"%s\","
             "\"location\":\"%s\",\"ip\":\"%s\",\"mac\":\"%s\","
             "\"gateway_ip\":\"%s\",\"protocol\":1,\"firmware\":\"%s\"}",
             gw.local.name, gw.local.hostname, gw.local.location,
             gw.local.ip, gw.local.mac, gw.expected_gateway.ip, a ? a->version : "unknown");
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_sendstr(r, j);
}

static esp_err_t gateway_status_api_g(httpd_req_t *r)
{
    if (!guard(r)) return ESP_OK;
    const size_t jz = 4096;
    char *j = malloc(jz);
    if (!j) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Sin memoria web");
    korvo_gateway_status_json(j, jz);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_sendstr(r, j);
    free(j);
    return e;
}

static esp_err_t gateway_g(httpd_req_t *r)
{
    if (!guard(r)) return ESP_OK;

    korvo_gateway_status_t gw = {0};
    korvo_gateway_get_status(&gw);
    korvo_network_config_t net = {0};
    korvo_network_get_config(&net);

    const char *target = gw.expected_gateway.ip[0] ? gw.expected_gateway.ip : net.gateway_node_ip;
    const size_t bz = 6400;
    char *b = malloc(bz);
    if (!b) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Sin memoria web");
    web_diag("GATEWAY begin");
    head(r, "Gateway");
    snprintf(b, bz,
        "<div class=card><h2>Identidad KORVO</h2>"
        "<p class=small>Name / Sala / Location son etiquetas de diagnostico. El pairing V1.2 se autoriza por IP.</p>"
        "<form method=post action=/gateway>"
        "<input type=hidden name=action value=local>"
        "<label>Name</label><input name=name maxlength=63 value='%s'>"
        "<label>Hostname / Sala</label><input name=hostname maxlength=63 value='%s'>"
        "<label>Location</label><input name=location maxlength=63 value='%s'>"
        "<button class=btn>GUARDAR IDENTIDAD</button></form></div>"

        "<div class=card><h2>Gateway / Pairing por IP</h2>"
        "<p>Configure aqui la IP del Gateway. En el Gateway configure la IP de esta KORVO. Cuando el WebSocket llegue desde las IP correctas, CONTROL queda habilitado.</p>"
        "<form method=post action=/gateway><input type=hidden name=action value=save>"
        "<label>IP Gateway</label><input name=gateway_ip maxlength=15 value='%s'>"
        "<button class=btn>GUARDAR / CONECTAR</button></form>"
        "<form method=post action=/gateway><input type=hidden name=action value=verify><button class=btn>VERIFY GATEWAY IP</button></form>"
        "<form method=post action=/gateway><input type=hidden name=action value=clear><button class='btn danger'>REMOVE GATEWAY IP</button></form>"
        "</div>"

        "<div class=card><h2>Estado del enlace</h2><div class=grid>"
        "<div>Gateway configurado</div><div class=mono>%s</div>"
        "<div>Gateway detectado</div><div class=mono>%s</div>"
        "<div>Hostname / Sala</div><div class=mono>%s</div>"
        "<div>Location</div><div class=mono>%s</div>"
        "<div>WebSocket</div><div class=%s>%s</div>"
        "<div>GATEWAY</div><div class=%s>%s</div>"
        "<div>CONTROL</div><div class=%s>%s</div>"
        "<div>Estado</div><div>%s</div>"
        "<div>Ultimo error</div><div class=bad>%s</div>"
        "</div></div>",
        gw.local.name, gw.local.hostname, gw.local.location,
        target,
        gw.expected_gateway.ip[0] ? gw.expected_gateway.ip : "--",
        gw.observed_gateway.name[0] ? gw.observed_gateway.name : "--",
        gw.observed_gateway.hostname[0] ? gw.observed_gateway.hostname : "--",
        gw.observed_gateway.location[0] ? gw.observed_gateway.location : "--",
        gw.websocket_connected ? "ok" : "bad", gw.websocket_connected ? "CONNECTED" : "DISCONNECTED",
        gw.gateway_ok ? "ok" : "bad", gw.gateway_ok ? "PAIRED" : "NOT PAIRED",
        gw.control_ok ? "ok" : "bad", gw.control_ok ? "OK" : "BLOCKED",
        korvo_gateway_state_name(gw.state),
        gw.last_error[0] ? gw.last_error : "--");
    httpd_resp_sendstr_chunk(r, b);
    free(b);
    web_diag("GATEWAY end");
    foot(r);
    return ESP_OK;
}

static esp_err_t gateway_p(httpd_req_t *r)
{
    if (!guard(r)) return ESP_OK;
    char b[1024], action[32] = {0}, x[128] = {0};
    if (body(r, b, sizeof(b)) != ESP_OK || !val(b, "action", action, sizeof(action))) {
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Solicitud invalida");
    }

    esp_err_t e = ESP_ERR_INVALID_ARG;
    char msg[256] = {0};
    if (!strcmp(action, "local")) {
        char name[64] = {0}, host[64] = {0}, loc[64] = {0};
        if (val(b, "name", name, sizeof(name)) &&
            val(b, "hostname", host, sizeof(host)) &&
            val(b, "location", loc, sizeof(loc))) {
            e = korvo_gateway_set_local_identity(name, host, loc);
            snprintf(msg, sizeof(msg), "%s", e == ESP_OK ? "Identidad guardada" : "Identidad invalida");
        }
    } else if (!strcmp(action, "save") || !strcmp(action, "enroll")) {
        if (val(b, "gateway_ip", x, sizeof(x))) e = korvo_gateway_enroll(x, msg, sizeof(msg));
    } else if (!strcmp(action, "verify")) {
        e = korvo_gateway_verify(msg, sizeof(msg));
    } else if (!strcmp(action, "clear")) {
        e = korvo_gateway_clear_pairing();
        snprintf(msg, sizeof(msg), "%s", e == ESP_OK ? "Gateway IP removida; CONTROL bloqueado" : "No se pudo borrar pairing");
    }

    if (e == ESP_OK) return redir(r, "/gateway");

    head(r, "Gateway - Error");
    char out[640];
    snprintf(out, sizeof(out),
             "<div class=card><h2>Operacion no completada</h2><p class=bad>%s</p>"
             "<p class=mono>%s</p><p><a href='/gateway'>VOLVER A GATEWAY</a></p></div>",
             msg[0] ? msg : "Error", esp_err_to_name(e));
    httpd_resp_sendstr_chunk(r, out);
    foot(r);
    return ESP_OK;
}


/* ============================================================
 * INTERCOM / SIP + BLUETOOTH HFP AG
 * ============================================================ */

static esp_err_t sip_status_api_g(httpd_req_t *r)
{
    if (!guard(r)) return ESP_OK;
    char j[1024];
    korvo_sip_status_json(j, sizeof(j));
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_sendstr(r, j);
}

static esp_err_t bt_status_api_g(httpd_req_t *r)
{
    if (!guard(r)) return ESP_OK;
    char j[1024];
    korvo_bluetooth_status_json(j, sizeof(j));
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_sendstr(r, j);
}

static esp_err_t intercom_g(httpd_req_t *r)
{
    if (!guard(r)) return ESP_OK;

    /* KORVO_V21_33_INTERCOM_COMPACT */
    korvo_sip_config_t sc = {0};
    korvo_bluetooth_config_t bc = {0};
    korvo_bluetooth_status_t bs = {0};
    korvo_audio_bridge_status_t as = {0};
    korvo_alarm_status_t alarm = {0};
    korvo_sip_get_config(&sc);
    korvo_bluetooth_get_config(&bc);
    korvo_bluetooth_get_status(&bs);

    char bt_link_form[320] = {0};
    if (bs.slc_connected || bs.connecting) {
        snprintf(bt_link_form, sizeof(bt_link_form),
                 "<form method=post action=/bluetooth/action style='display:inline'><input type=hidden name=action value=disconnect><button class=btn>%s</button></form> ",
                 bs.connecting && !bs.slc_connected ? "CANCELAR" : "DISCONNECT");
    } else if (bs.paired && bs.peer_mac[0]) {
        snprintf(bt_link_form, sizeof(bt_link_form),
                 "<form method=post action=/bluetooth/action style='display:inline'><input type=hidden name=action value=connect><input type=hidden name=mac value='%s'><button class=btn>CONNECT</button></form> ",
                 bs.peer_mac);
    }
    korvo_audio_bridge_get_status(&as);
    korvo_alarm_get_status(&alarm);

    const size_t bz = 7600;
    char *b = malloc(bz);
    if (!b) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Sin memoria web");
    web_diag("INTERCOM begin");
    head(r, "Intercom");

    snprintf(b, bz,
        "<style>"
        ".sipcompact{padding:14px 16px;margin-bottom:14px}.sipcompact h2{margin:0 0 10px;font-size:20px}"
        ".sipcompact .hint{margin:0 0 10px}.sipgrid{display:grid;grid-template-columns:1fr 1fr;gap:8px 12px}"
        ".sipfield label{margin:0 0 4px;font-size:12px}.sipfield input{padding:8px 9px;height:36px}"
        ".sipactions{display:flex;gap:10px;margin-top:12px;flex-wrap:wrap}.sipactions .btn{margin-top:0;padding:9px 14px}"
        "@media(max-width:650px){.sipgrid{grid-template-columns:1fr}}"
        "</style>"
        "<div class='card sipcompact'><h2>SIP / INTERCOM</h2>"
        "<p class='small hint'>Configuracion SIP KORVO</p>"
        "<form method=post action=/sip/save><input type=hidden name=enabled value=1><div class=sipgrid>"
        "<div class=sipfield><label>Servidor Asterisk / PBX</label><input id=sip_server name=server maxlength=63 value='%s'></div>"
        "<div class=sipfield><label>Puerto SIP</label><input id=sip_server_port name=server_port type=number min=1 max=65535 value='%u'></div>"
        "<div class=sipfield><label>Puerto SIP local</label><input id=sip_local_port name=local_port type=number min=1 max=65535 value='%u'></div>"
        "<div class=sipfield><label>Puerto RTP local</label><input id=sip_rtp_port name=rtp_port type=number min=1 max=65535 value='%u'></div>"
        "<div class=sipfield><label>Extension KORVO</label><input id=sip_extension name=extension maxlength=31 value='%s'></div>"
        "<div class=sipfield><label>Usuario SIP</label><input id=sip_username name=username maxlength=31 value='%s'></div>"
        "<div class=sipfield><label>Password SIP</label><input id=sip_password name=password type=password maxlength=63 placeholder='conservar actual'></div>"
        "<div class=sipfield><label>Nombre / Sala</label><input id=sip_display name=display_name maxlength=47 value='%s'></div>"
        "<div class=sipfield><label>Extension operador / central</label><input id=sip_operator name=operator_extension maxlength=31 value='%s'></div>"
        "<div class=sipfield><label>REGISTER expires (s)</label><input id=sip_expires name=expires type=number min=60 max=3600 value='%u'></div>"
        "</div><div class=sipactions><button class=btn type=submit>GUARDAR SIP</button>"
        "<button class=btn type=button onclick=sipDefaults()>DEFAULT SETTINGS</button></div></form>"
        "<script>function sipDefaults(){"
        "sip_server.value='192.168.10.10';sip_server_port.value='5060';sip_local_port.value='5060';"
        "sip_rtp_port.value='4000';sip_extension.value='200';sip_username.value='200';"
        "sip_password.value='korvo200';sip_display.value='Sala 01';sip_operator.value='300';sip_expires.value='300';}"
        "</script></div>",
        sc.server, (unsigned)sc.server_port, (unsigned)sc.local_port, (unsigned)sc.rtp_port,
        sc.extension, sc.username, sc.display_name, sc.operator_extension, (unsigned)sc.register_expires);
    httpd_resp_sendstr_chunk(r, b);

    snprintf(b, bz,
        "<div class=card id=bluetooth><h2>BLUETOOTH / HFP AUDIO GATEWAY</h2>"
        "<p class=small>KORVO actua como HFP AG (telefono). El parlante/manos libres debe ser HFP HF.</p>"
        "<form method=post action=/bluetooth/save>"
        "<label><input type=checkbox name=enabled value=1 %s> Bluetooth HFP habilitado</label>"
        "<label><input type=checkbox name=auto_connect value=1 %s> Mantener HFP conectado al ultimo HF (supervisor)</label>"
        "<label>Nombre Bluetooth KORVO</label><input name=local_name maxlength=63 value='%s'>"
        "<label>PIN legacy</label><input name=pin maxlength=16 value='%s'>"
        "<button class=btn>GUARDAR BLUETOOTH</button></form>"
        "<hr><h3>NIVELES DE AUDIO</h3>"
        "<form method=post action=/audio/levels>"
        "<label>Salida auricular / interfaz: %u%%</label><input name=out type=range min=0 max=100 value='%u'>"
        "<label>Microfono TX: %u%%</label><input name=mic type=range min=0 max=100 value='%u'>"
        "<button class=btn>GUARDAR NIVELES</button></form>"
        "<hr><div class=grid>"
        "<div>HFP AG engine</div><div class=%s>%s</div>"
        "<div>PAIR</div><div class=%s>%s</div>"
        "<div>HF conectado</div><div class=%s>%s</div>"
        "<div>Peer</div><div class=mono>%s %s</div>"
        "<div>Audio SCO/eSCO</div><div class=%s>%s</div>"
        "<div>Codec HFP</div><div>%s</div>"
        "<div>RTP bridge</div><div class=%s>%s</div>"
        "<div>RTP RX / TX</div><div>%lu / %lu</div>"
        "<div>RTP RX dropped</div><div>%lu</div>"
        "<div>MIC dropped</div><div>%lu</div>"
        "<div>SCO underruns</div><div>%lu</div>"
        "<div>Error</div><div class=bad>%s</div>"
        "</div>"
        "<form method=post action=/bluetooth/action style='display:inline'><input type=hidden name=action value=power_toggle><button class=btn>%s BLUETOOTH</button></form> "
        "<form method=post action=/bluetooth/action style='display:inline'><input type=hidden name=action value=scan><button class=btn>SCAN</button></form> "
        "%s"
        "<form method=post action=/bluetooth/action style='display:inline'><input type=hidden name=action value=forget><button class='btn danger'>FORGET</button></form>"
        "</div>",
        bc.enabled ? "checked" : "", bc.auto_connect ? "checked" : "",
        bc.local_name, bc.pin,
        (unsigned)korvo_bluetooth_get_output_volume(), (unsigned)korvo_bluetooth_get_output_volume(),
        (unsigned)korvo_bluetooth_get_mic_gain(), (unsigned)korvo_bluetooth_get_mic_gain(),
        (bc.enabled && bs.engine_ready) ? "ok" : "bad",
        !bc.enabled ? "DISABLED" : (bs.engine_ready ? "READY" : "STARTING"),
        bs.paired ? "ok" : "bad", bs.paired ? "PAIRED" : "NOT PAIRED",
        bs.slc_connected ? "ok" : (bs.connecting ? "" : "bad"),
        bs.slc_connected ? "CONNECTED" : (bs.connecting ? "CONNECTING" : "DISCONNECTED"),
        bs.peer_name[0] ? bs.peer_name : "--", bs.peer_mac[0] ? bs.peer_mac : "",
        bs.audio_connected ? "ok" : "bad", bs.audio_connected ? "CONNECTED" : "DISCONNECTED",
        bs.codec[0] ? bs.codec : "--",
        as.running ? "ok" : "bad", as.running ? "RUNNING" : "STOPPED",
        (unsigned long)as.rx_packets, (unsigned long)as.tx_packets,
        (unsigned long)as.rx_dropped, (unsigned long)as.mic_dropped,
        (unsigned long)as.sco_underruns,
        bs.last_error[0] ? bs.last_error : "--",
        bc.enabled ? "APAGAR" : "ENCENDER",
        bt_link_form);
    httpd_resp_sendstr_chunk(r, b);
    if (!bc.enabled) {
        httpd_resp_sendstr_chunk(r,
            "<div class=card><p class=bad><b>Bluetooth HFP esta DESHABILITADO.</b> "
            "Marque Bluetooth HFP habilitado, pulse GUARDAR BLUETOOTH y luego SCAN.</p></div>");
    }

    snprintf(b, bz,
        "<div class=card><h2>ALARMAS BLUETOOTH</h2>"
        "<p class=small>Seleccione una alarma WAV y pulse PROBAR. La KORVO abre el audio HFP y reproduce el archivo desde la SD. WAV requerido: PCM mono, 8000 Hz, 16 bits.</p>"
        "<form method=post action=/alarm/test>"
        "<label>Tipo de alarma</label><select name=type>"
        "<option value=1 %s>1 - TIMBRE</option>"
        "<option value=2 %s>2 - DOBLE BEEP</option>"
        "<option value=3 %s>3 - ASCENDENTE</option>"
        "<option value=4 %s>4 - SIRENA</option>"
        "<option value=5 %s>5 - CRITICA</option>"
        "</select>"
        "<button class=btn>PROBAR</button></form>"
        "<hr><div class=grid>"
        "<div>Seleccionada</div><div>%s</div>"
        "<div>Estado</div><div class=%s>%s</div>"
        "<div>Error</div><div class=bad>%s</div>"
        "</div></div>",
        alarm.selected == 1 ? "selected" : "",
        alarm.selected == 2 ? "selected" : "",
        alarm.selected == 3 ? "selected" : "",
        alarm.selected == 4 ? "selected" : "",
        alarm.selected == 5 ? "selected" : "",
        korvo_alarm_name(alarm.selected),
        alarm.playing ? "ok" : "", alarm.playing ? "REPRODUCIENDO" : "IDLE",
        alarm.last_error[0] ? alarm.last_error : "--");
    httpd_resp_sendstr_chunk(r, b);

    httpd_resp_sendstr_chunk(r, "<div class=card id=bt-devices><h2>Dispositivos Bluetooth encontrados</h2>");
    if (bs.scanning) {
        httpd_resp_sendstr_chunk(r, "<p class=ok>SCAN ACTIVO... espere unos 10 segundos.</p><script>setTimeout(function(){location.href='/intercom#bt-devices'},2500)</script>");
    }
    if (!bs.device_count) {
        httpd_resp_sendstr_chunk(r, "<p class=small>No hay dispositivos en la lista. Ponga el manos libres en pairing y pulse SCAN.</p>");
    } else {
        httpd_resp_sendstr_chunk(r, "<div class=grid><div><b>Dispositivo</b></div><div><b>Accion</b></div>");
        for (size_t i = 0; i < bs.device_count; ++i) {
            char row[640];
            bool same_peer = bs.peer_mac[0] && !strcmp(bs.peer_mac, bs.devices[i].mac);
            const char *action_label = (bs.slc_connected && same_peer) ? "CONNECTED" :
                                       ((bs.connecting && same_peer) ? "CONNECTING..." :
                                        ((bs.paired && same_peer) ? "CONNECT" : "PAIR / CONNECT"));
            const char *disabled = ((bs.slc_connected || bs.connecting) && same_peer) ? " disabled" : "";
            snprintf(row, sizeof(row),
                "<div><span class=mono>%s</span><br><span class=small>%s &nbsp; RSSI %d dBm</span></div>"
                "<div><form method=post action=/bluetooth/action>"
                "<input type=hidden name=action value=connect><input type=hidden name=mac value='%s'>"
                "<button class=btn%s>%s</button></form></div>",
                bs.devices[i].name[0] ? bs.devices[i].name : "--",
                bs.devices[i].mac, bs.devices[i].rssi, bs.devices[i].mac,
                disabled, action_label);
            httpd_resp_sendstr_chunk(r, row);
        }
        httpd_resp_sendstr_chunk(r, "</div>");
    }
    httpd_resp_sendstr_chunk(r, "</div>");

    free(b);
    web_diag("INTERCOM end");
    foot(r);
    return ESP_OK;
}

static esp_err_t sip_save_p(httpd_req_t *r)
{
    if (!guard(r)) return ESP_OK;
    char b[1800];
    if (body(r, b, sizeof(b)) != ESP_OK) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Solicitud invalida");

    korvo_sip_config_t c = {0};
    korvo_sip_get_config(&c);
    char x[128] = {0};
    c.enabled = val(b, "enabled", x, sizeof(x));
    if (val(b, "server", x, sizeof(x))) snprintf(c.server, sizeof(c.server), "%s", x);
    if (val(b, "extension", x, sizeof(x))) snprintf(c.extension, sizeof(c.extension), "%s", x);
    if (val(b, "username", x, sizeof(x))) snprintf(c.username, sizeof(c.username), "%s", x);
    if (val(b, "display_name", x, sizeof(x))) snprintf(c.display_name, sizeof(c.display_name), "%s", x);
    if (val(b, "operator_extension", x, sizeof(x))) snprintf(c.operator_extension, sizeof(c.operator_extension), "%s", x);
    if (val(b, "password", x, sizeof(x)) && x[0]) snprintf(c.password, sizeof(c.password), "%s", x);
    if (val(b, "server_port", x, sizeof(x))) c.server_port = (uint16_t)strtoul(x, NULL, 10);
    if (val(b, "local_port", x, sizeof(x))) c.local_port = (uint16_t)strtoul(x, NULL, 10);
    if (val(b, "rtp_port", x, sizeof(x))) c.rtp_port = (uint16_t)strtoul(x, NULL, 10);
    if (val(b, "expires", x, sizeof(x))) c.register_expires = (uint16_t)strtoul(x, NULL, 10);

    esp_err_t e = korvo_sip_save_config(&c);
    if (e != ESP_OK) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Configuracion SIP invalida");
    return redir(r, "/intercom");
}

static esp_err_t bt_save_p(httpd_req_t *r)
{
    if (!guard(r)) return ESP_OK;
    char b[1024];
    if (body(r, b, sizeof(b)) != ESP_OK) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Solicitud invalida");

    korvo_bluetooth_config_t c = {0};
    korvo_bluetooth_get_config(&c);
    char x[128] = {0};
    c.enabled = val(b, "enabled", x, sizeof(x));
    c.auto_connect = val(b, "auto_connect", x, sizeof(x));
    if (val(b, "local_name", x, sizeof(x))) snprintf(c.local_name, sizeof(c.local_name), "%s", x);
    if (val(b, "pin", x, sizeof(x))) snprintf(c.pin, sizeof(c.pin), "%s", x);

    esp_err_t e = korvo_bluetooth_save_config(&c);
    if (e != ESP_OK) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Configuracion Bluetooth invalida");
    return redir(r, "/intercom#bluetooth");
}

static esp_err_t audio_levels_p(httpd_req_t *r)
{
    if (!guard(r)) return ESP_OK;
    char b[256], x[32] = {0};
    if (body(r, b, sizeof(b)) != ESP_OK)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Solicitud invalida");
    if (val(b, "out", x, sizeof(x))) {
        unsigned long v = strtoul(x, NULL, 10); if (v > 100) v = 100;
        (void)korvo_bluetooth_set_output_volume((uint8_t)v, true);
    }
    if (val(b, "mic", x, sizeof(x))) {
        unsigned long v = strtoul(x, NULL, 10); if (v > 100) v = 100;
        (void)korvo_bluetooth_set_mic_gain((uint8_t)v, true);
    }
    return redir(r, "/intercom#bluetooth");
}

static esp_err_t bt_action_p(httpd_req_t *r)
{
    if (!guard(r)) return ESP_OK;
    char b[512], action[32] = {0}, mac[32] = {0};
    if (body(r, b, sizeof(b)) != ESP_OK || !val(b, "action", action, sizeof(action)))
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Solicitud invalida");

    esp_err_t e = ESP_ERR_INVALID_ARG;
    if (!strcmp(action, "power_toggle")) {
        korvo_bluetooth_config_t c = {0};
        e = korvo_bluetooth_get_config(&c);
        if (e == ESP_OK) {
            c.enabled = !c.enabled;
            e = korvo_bluetooth_save_config(&c);
        }
    } else if (!strcmp(action, "scan")) e = korvo_bluetooth_scan();
    else if (!strcmp(action, "connect")) {
        if (val(b, "mac", mac, sizeof(mac))) e = korvo_bluetooth_connect(mac);
    } else if (!strcmp(action, "disconnect")) e = korvo_bluetooth_disconnect();
    else if (!strcmp(action, "forget")) e = korvo_bluetooth_forget();
    else if (!strcmp(action, "audio_connect")) e = korvo_bluetooth_audio_connect();
    else if (!strcmp(action, "audio_disconnect")) e = korvo_bluetooth_audio_disconnect();

    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        char msg[128]; snprintf(msg, sizeof(msg), "Bluetooth action failed: %s", esp_err_to_name(e));
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, msg);
    }
    /* Keep the browser at the Bluetooth area instead of jumping to page top.
       ESP_ERR_INVALID_STATE is shown through Bluetooth last_error (e.g. disabled engine). */
    return redir(r, !strcmp(action, "scan") ? "/intercom#bt-devices" : "/intercom#bluetooth");
}

static esp_err_t alarm_test_p(httpd_req_t *r)
{
    if (!guard(r)) return ESP_OK;
    char b[256], x[32] = {0};
    if (body(r, b, sizeof(b)) != ESP_OK || !val(b, "type", x, sizeof(x)))
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Solicitud invalida");

    unsigned long type = strtoul(x, NULL, 10);
    if (type < KORVO_ALARM_TIMBRE || type > KORVO_ALARM_CRITICA)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Tipo de alarma invalido");

    esp_err_t e = korvo_alarm_test((uint8_t)type);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Alarm test failed: %s", esp_err_to_name(e));
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, msg);
    }
    return redir(r, "/intercom");
}



/* ============================================================
 * FIRMWARE
 * ============================================================ */

static esp_err_t firmware_g(httpd_req_t *r)
{
    if (!guard(r)) {
        return ESP_OK;
    }

    const esp_app_desc_t *cur =
        esp_app_get_description();

    korvo_update_info_t u = {0};

    esp_err_t ue =
        korvo_update_get_info(&u);

    char b[3000];

    head(
        r,
        "Firmware");

    snprintf(
        b,
        sizeof(b),

        "<div class=card>"
        "<h2>Firmware actual</h2>"

        "<div class=grid>"

        "<div>Producto</div>"
        "<div><b>KORVO</b></div>"

        "<div>Proyecto</div>"
        "<div class=mono>%s</div>"

        "<div>Version</div>"
        "<div class=mono>%s</div>"

        "<div>Compilado</div>"
        "<div>%s %s</div>"

        "<div>ESP-IDF</div>"
        "<div class=mono>%s</div>"

        "</div>"
        "</div>",

        cur ? cur->project_name : "--",
        cur ? cur->version : "--",
        cur ? cur->date : "--",
        cur ? cur->time : "--",
        cur ? cur->idf_ver : "--");

    httpd_resp_sendstr_chunk(
        r,
        b);


    if (ue == ESP_OK &&
        u.valid) {

        snprintf(
            b,
            sizeof(b),

            "<div class=card>"
            "<h2>Update en SD</h2>"

            "<div class=grid>"

            "<div>Estado</div>"
            "<div class=%s>%s</div>"

            "<div>Version</div>"
            "<div class=mono>%s</div>"

            "<div>Proyecto</div>"
            "<div class=mono>%s</div>"

            "<div>Tamano</div>"
            "<div>%u bytes</div>"

            "<div>SHA-256</div>"
            "<div class=mono "
            "style='word-break:break-all'>%s</div>"

            "</div>"

            "<form method=post "
            "action=/firmware/cancel>"

            "<button class='btn danger'>"
            "ELIMINAR UPDATE"
            "</button>"

            "</form>"
            "</div>",

            u.pending
                ? "ok"
                : "",

            u.pending
                ? "VALIDADO / PENDIENTE"
                : "ARCHIVO SIN MARCA",

            u.app.version,
            u.app.project_name,

            (unsigned)u.size,

            u.sha256);

    } else {

        snprintf(
            b,
            sizeof(b),

            "<div class=card>"
            "<h2>Update en SD</h2>"
            "<p>No hay firmware pendiente.</p>"
            "</div>");
    }

    httpd_resp_sendstr_chunk(
        r,
        b);


    httpd_resp_sendstr_chunk(
        r,

        "<div class=card>"

        "<h2>Subir firmware</h2>"

        "<p>"
        "El nombre del archivo no importa. "
        "KORVO lee la cabecera ESP-IDF, "
        "valida proyecto/version, "
        "calcula SHA-256 y lo guarda internamente como "
        "<span class=mono>korvo.bin</span>."
        "</p>"

        "<input id=f type=file "
        "accept='.bin,application/octet-stream'>"

        "<label>"
        "<input id=d type=checkbox> "
        "Permitir downgrade/reinstalacion "
        "(mantenimiento)"
        "</label>"

        "<button class=btn onclick=up()>"
        "SUBIR Y VALIDAR"
        "</button>"

        "<div class=bar>"
        "<div id=pb class=fill></div>"
        "</div>"

        "<p id=msg class=small></p>"

        "</div>"


        "<div class=card>"

        "<h2>Mantenimiento</h2>"

        "<p>"
        "REBOOT reinicia KORVO. "
        "Si existe un update VALIDADO/PENDIENTE "
        "en la SD, KORVO lo valida e instala "
        "durante la secuencia de arranque."
        "</p>"

        "<form method=post action=/reboot>"

        "<button class='btn danger'>"
        "REBOOT KORVO"
        "</button>"

        "</form>"

        "</div>"


        "<script>"

        "function up(){"

        "let f=document.getElementById('f').files[0];"

        "if(!f){"
        "msg.textContent='Seleccione un .bin';"
        "return"
        "}"

        "let x=new XMLHttpRequest();"

        "x.open("
        "'POST',"
        "'/firmware/upload?downgrade='+(d.checked?'1':'0')"
        ");"

        "x.setRequestHeader("
        "'Content-Type',"
        "'application/octet-stream'"
        ");"

        "x.upload.onprogress=e=>{"
        "if(e.lengthComputable)"
        "pb.style.width=(e.loaded*100/e.total)+'%'"
        "};"

        "x.onload=()=>{"
        "msg.textContent=x.responseText;"
        "if(x.status==200)"
        "setTimeout(()=>location.reload(),1200)"
        "};"

        "x.onerror=()=>"
        "msg.textContent='Error de comunicacion';"

        "x.send(f)"

        "}"

        "</script>");

    foot(r);

    return ESP_OK;
}


/* ============================================================
 * UPLOAD FIRMWARE
 * ============================================================ */

static esp_err_t upload_p(httpd_req_t *r)
{
    if (!guard(r)) {
        return ESP_OK;
    }

    if (!korvo_storage_is_ready()) {

        return httpd_resp_send_err(
            r,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "SD no disponible");
    }

    if (r->content_len <= 0) {

        return httpd_resp_send_err(
            r,
            HTTPD_400_BAD_REQUEST,
            "BIN vacio");
    }


    const esp_partition_t *next =
        esp_ota_get_next_update_partition(NULL);


    if (!next ||
        (size_t)r->content_len > next->size) {

        return httpd_resp_send_err(
            r,
            HTTPD_400_BAD_REQUEST,
            "BIN excede particion OTA");
    }


    unlink(KORVO_UPDATE_TEMP);


    FILE *f =
        fopen(
            KORVO_UPDATE_TEMP,
            "wb");

    if (!f) {

        return httpd_resp_send_err(
            r,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "No se puede escribir SD");
    }


    char *buf =
        malloc(UPLOAD_CHUNK);

    if (!buf) {

        fclose(f);
        unlink(KORVO_UPDATE_TEMP);

        return httpd_resp_send_err(
            r,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Sin memoria");
    }


    int remain = r->content_len;
    bool ok = true;


    while (remain > 0) {

        int want =
            remain > UPLOAD_CHUNK
                ? UPLOAD_CHUNK
                : remain;

        int n =
            httpd_req_recv(
                r,
                buf,
                want);

        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }

        if (n <= 0 ||
            fwrite(
                buf,
                1,
                n,
                f) != (size_t)n) {

            ok = false;
            break;
        }

        remain -= n;
    }


    free(buf);

    fflush(f);
    fclose(f);


    if (!ok ||
        remain != 0) {

        unlink(KORVO_UPDATE_TEMP);

        return httpd_resp_send_err(
            r,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Upload incompleto");
    }


    /*
     * Validar primero upload.tmp.
     */

    korvo_update_info_t info = {0};

    esp_err_t e =
        korvo_update_validate_file(
            KORVO_UPDATE_TEMP,
            &info);


    if (e != ESP_OK) {

        unlink(KORVO_UPDATE_TEMP);

        char msg[180];

        snprintf(
            msg,
            sizeof(msg),
            "BIN rechazado: %s",
            info.error[0]
                ? info.error
                : esp_err_to_name(e));

        return httpd_resp_send_err(
            r,
            HTTPD_400_BAD_REQUEST,
            msg);
    }


    /*
     * El BIN ya fue validado.
     * Activarlo como nombre interno fijo.
     */

    unlink(KORVO_UPDATE_FILE);


    if (rename(
            KORVO_UPDATE_TEMP,
            KORVO_UPDATE_FILE) != 0) {

        unlink(KORVO_UPDATE_TEMP);

        return httpd_resp_send_err(
            r,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "No se pudo activar BIN en SD");
    }


    /*
     * Revisar opción explícita de downgrade/reinstalación.
     */

    char q[32] = {0};

    bool downgrade =
        httpd_req_get_url_query_str(
            r,
            q,
            sizeof(q)) == ESP_OK &&
        strstr(
            q,
            "downgrade=1") != NULL;


    e = korvo_update_mark_pending(
        downgrade);


    if (e != ESP_OK) {

        /*
         * IMPORTANTE:
         * si el nuevo BIN no puede quedar pendiente,
         * eliminamos también cualquier marcador anterior
         * para no dejar pending.txt apuntando a un BIN
         * inexistente o diferente.
         */

        unlink(KORVO_UPDATE_PENDING);
        unlink(KORVO_UPDATE_FILE);

        return httpd_resp_send_err(
            r,
            HTTPD_400_BAD_REQUEST,

            e == ESP_ERR_INVALID_STATE

                ? "Version no es superior. "
                  "Use downgrade/reinstalacion "
                  "solo si corresponde."

                : "No se pudo marcar update");
    }


    char msg[220];

    snprintf(
        msg,
        sizeof(msg),

        "OK - KORVO %s validado, "
        "%u bytes, "
        "SHA-256 %.16s... "
        "Queda pendiente para el proximo reboot.",

        info.app.version,
        (unsigned)info.size,
        info.sha256);


    return httpd_resp_sendstr(
        r,
        msg);
}


/* ============================================================
 * CANCELAR / REBOOT
 * ============================================================ */

static esp_err_t cancel_p(httpd_req_t *r)
{
    if (!guard(r)) {
        return ESP_OK;
    }

    korvo_update_cancel();

    return redir(
        r,
        "/firmware");
}


static esp_err_t reboot_p(httpd_req_t *r)
{
    if (!guard(r)) {
        return ESP_OK;
    }

    httpd_resp_sendstr(
        r,
        "KORVO reiniciando...");

    vTaskDelay(
        pdMS_TO_TICKS(500));

    esp_restart();

    return ESP_OK;
}


/* ============================================================
 * INICIAR WEBSERVER
 * ============================================================ */

esp_err_t korvo_web_start(void)
{
    if (s_server) {
        return ESP_OK;
    }


    httpd_config_t c =
        HTTPD_DEFAULT_CONFIG();


    c.max_uri_handlers = 32;
    c.lru_purge_enable = true;
    c.stack_size = 12288;
    c.recv_wait_timeout = 10;
    c.send_wait_timeout = 10;


    esp_err_t e =
        httpd_start(
            &s_server,
            &c);


    if (e != ESP_OK) {
        return e;
    }


    const httpd_uri_t u[] = {

        {
            .uri = "/",
            .method = HTTP_GET,
            .handler = status_g
        },

        {
            .uri = "/login",
            .method = HTTP_GET,
            .handler = login_g
        },

        {
            .uri = "/login",
            .method = HTTP_POST,
            .handler = login_p
        },

        {
            .uri = "/logout",
            .method = HTTP_GET,
            .handler = logout_g
        },

        {
            .uri = "/network",
            .method = HTTP_GET,
            .handler = net_g
        },

        {
            .uri = "/network",
            .method = HTTP_POST,
            .handler = net_p
        },

        {
            .uri = "/factory-network",
            .method = HTTP_POST,
            .handler = factory_p
        },

        {
            .uri = "/gateway",
            .method = HTTP_GET,
            .handler = gateway_g
        },

        {
            .uri = "/gateway",
            .method = HTTP_POST,
            .handler = gateway_p
        },

        {
            .uri = "/intercom",
            .method = HTTP_GET,
            .handler = intercom_g
        },

        {
            .uri = "/sip/save",
            .method = HTTP_POST,
            .handler = sip_save_p
        },

        {
            .uri = "/bluetooth/save",
            .method = HTTP_POST,
            .handler = bt_save_p
        },

        {
            .uri = "/bluetooth/action",
            .method = HTTP_POST,
            .handler = bt_action_p
        },

        {
            .uri = "/audio/levels",
            .method = HTTP_POST,
            .handler = audio_levels_p
        },

        {
            .uri = "/alarm/test",
            .method = HTTP_POST,
            .handler = alarm_test_p
        },

        {
            .uri = "/api/korvo/sip/status",
            .method = HTTP_GET,
            .handler = sip_status_api_g
        },

        {
            .uri = "/api/korvo/bluetooth/status",
            .method = HTTP_GET,
            .handler = bt_status_api_g
        },

        {
            .uri = "/api/device/identity",
            .method = HTTP_GET,
            .handler = identity_api_g
        },

        {
            .uri = "/api/korvo/gateway/status",
            .method = HTTP_GET,
            .handler = gateway_status_api_g
        },

        {
            .uri = "/firmware",
            .method = HTTP_GET,
            .handler = firmware_g
        },

        {
            .uri = "/firmware/upload",
            .method = HTTP_POST,
            .handler = upload_p
        },

        {
            .uri = "/firmware/cancel",
            .method = HTTP_POST,
            .handler = cancel_p
        },

        {
            .uri = "/reboot",
            .method = HTTP_POST,
            .handler = reboot_p
        }
    };


    for (size_t i = 0;
         i < sizeof(u) / sizeof(u[0]);
         i++) {

        e =
            httpd_register_uri_handler(
                s_server,
                &u[i]);

        if (e != ESP_OK) {

            httpd_stop(
                s_server);

            s_server = NULL;

            return e;
        }
    }


    ESP_LOGI(
        TAG,
        "Web admin + SIP/HFP intercom + firmware manager ready");

    return ESP_OK;
}


/* ============================================================
 * DETENER WEBSERVER
 * ============================================================ */

esp_err_t korvo_web_stop(void)
{
    if (!s_server) {
        return ESP_OK;
    }

    esp_err_t e =
        httpd_stop(
            s_server);

    s_server = NULL;

    return e;
}