#include "korvo_web.h"
#include "korvo_video01.h"
#include "korvo_network.h"
#include "korvo_gateway.h"
#include "korvo_storage.h"
#include "korvo_update.h"
#include "korvo_sip.h"

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
    ".top{background:#171f28;padding:10px 14px;display:flex;align-items:center;gap:7px;flex-wrap:wrap}"
    ".wrap{max-width:960px;margin:12px auto;padding:0 12px}"
    ".card{background:#18212b;border:1px solid #34404d;"
    "border-radius:10px;padding:14px 16px;margin-bottom:12px}"
    "a{color:#8ecbff;text-decoration:none;margin-right:8px}"
    "label{display:block;margin:0 0 4px;color:#aebac6;font-size:12px}"
    "input,select{width:100%;min-width:0;padding:8px 9px;border:1px solid #445363;"
    "border-radius:6px;background:#0f151b;color:white}"
    "input[type=checkbox]{width:auto}"
    ".btn{margin-top:10px;padding:9px 14px;border:0;"
    "border-radius:6px;font-weight:bold;cursor:pointer}"
    ".danger{background:#a53a3a;color:white}"
    ".grid{display:grid;grid-template-columns:minmax(110px,165px) minmax(0,1fr);gap:6px 12px;font-size:13px}"
    ".grid>div{min-width:0;overflow-wrap:anywhere}"
    ".formgrid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:9px 12px}"
    ".field{min-width:0}.actions{display:flex;gap:8px;flex-wrap:wrap}"
    ".actions .btn{margin-top:8px}.card h2{font-size:18px;margin:0 0 10px}"
    ".card h3{font-size:15px;margin:8px 0}.card p{margin:8px 0}"
    "details summary{cursor:pointer;color:#8ecbff;padding:7px 0;font-size:13px}"
    ".top a{padding:7px 8px;background:#202c37;border-radius:6px;font-size:12px;white-space:nowrap}"
    "@media(max-width:650px){.formgrid{grid-template-columns:1fr}.wrap{padding:0 8px}"
    ".card{padding:12px}.top{gap:5px;padding:9px}.top a{padding:8px 7px}"
    ".grid{grid-template-columns:minmax(90px,40%) minmax(0,1fr)}.btn{min-height:40px}}"
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
        /* V25-P05 SIP GATEWAY DIAGNOSTICO: fuente sys.status, no SIP local. */
        "<div>SIP Gateway</div><div class=%s>%s</div>"
        "<div>SIP Registered</div><div class=%s>%s</div>"
        "<div>SIP State</div><div class=mono>%s</div>"
        "<div>Diccionario V2</div><div class=%s>%s</div>"
        "</div><p><a href='/gateway'>Configurar Gateway IP</a></p></div>",
        gw.local.hostname,
        gw.expected_gateway.ip[0] ? gw.expected_gateway.ip : "NO CONFIGURADO",
        gw.websocket_connected ? "ok" : "bad", gw.websocket_connected ? "CONNECTED" : "DISCONNECTED",
        gw.gateway_ok ? "ok" : "bad", gw.gateway_ok ? "PAIRED" : "NOT PAIRED",
        gw.control_ok ? "ok" : "bad", gw.control_ok ? "ENABLED" : "BLOCKED",
        gw.websocket_connected ? "ok" : "bad", gw.websocket_connected ? "ONLINE" : "OFFLINE",
        (gw.websocket_connected && gw.dictionary_state == KORVO_DICT_OK && gw.sip_registered) ? "ok" : "bad",
        !gw.websocket_connected ? "SIN ENLACE" :
            gw.dictionary_state != KORVO_DICT_OK ? "DICT INVALIDO" :
            gw.sip_registered ? "YES" : "NO",
        (gw.websocket_connected && gw.dictionary_state == KORVO_DICT_OK) ?
            (gw.sip_state[0] ? gw.sip_state : "--") : "--",
        gw.dictionary_state == KORVO_DICT_OK ? "ok" : "bad",
        gw.dictionary_state == KORVO_DICT_OK ? "OK" :
            gw.dictionary_state == KORVO_DICT_MISMATCH ? "MISMATCH" : "UNKNOWN");
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
        "<p class=small>Cliente Wi-Fi STA / bridge ETH01 o AP compatible.</p>"

        "<form method=post action=/network><div class=formgrid>"

        "<div class=field><label>SSID del AP / bridge</label>"
        "<input name=ssid maxlength=32 value='%s'></div>"

        "<div class=field><label>Clave Wi-Fi</label>"
        "<input name=password type=password maxlength=63 value='%s'></div>"

        "<div class=field><label>IP fija KORVO</label>"
        "<input name=ip maxlength=15 value='%s'></div>"

        "<div class=field><label>Mascara</label>"
        "<input name=netmask maxlength=15 value='%s'></div>"

        "<div class=field><label>Gateway de la LAN</label>"
        "<input name=network_gateway maxlength=15 value='%s'></div>"

        "<div class=field><label>DNS</label>"
        "<input name=dns maxlength=15 value='%s'></div>"

        "<div class=field><label>IP Gateway sugerida para ENROLL</label>"
        "<input name=gateway_node maxlength=15 value='%s'></div>"

        "</div><div class=actions><button class=btn>"
        "GUARDAR Y REINICIAR"
        "</button></div>"

        "</form>"

        "<details><summary>Referencia de laboratorio</summary><p class=small>"
        "Laboratorio: korvo_wifi / "
        "KORVO 192.168.10.50 / "
        "LAN GW 192.168.10.1 / "
        "Gateway sugerido 192.168.10.51"
        "</p></details>"

        "</div>"

        "<details class=card><summary>Mantenimiento de red</summary>"
        "<form method=post action=/factory-network onsubmit='return confirm(&quot;Restaurar red de fabrica?&quot;)'>"
        "<button class=btn>"
        "RESTAURAR RED DE FABRICA"
        "</button>"
        "</form>"
        "</details>",

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
        /* V25-P04 TAG HOST: etiquetas web, NVS intacta. */
        "<div class=card><h2>Identidad KORVO</h2>"
        "<p class=small>Identidad de sala / pairing por IP.</p>"
        "<form method=post action=/gateway>"
        "<input type=hidden name=action value=local><div class=formgrid>"
        "<div class=field><label>TAG</label><input name=name maxlength=63 value='%s'></div>"
        "<div class=field><label>HOSTNAME</label><input name=hostname maxlength=63 value='%s'></div>"
        "<div class=field><label>UBICACION</label><input name=location maxlength=63 value='%s'></div>"
        "</div>"
        "<button class=btn>GUARDAR IDENTIDAD</button></form></div>"

        "<div class=card><h2>Gateway / Pairing por IP</h2>"
        "<p class=small>Configure la IP del Gateway; el control se autoriza por IP.</p>"
        "<form method=post action=/gateway><input type=hidden name=action value=save>"
        "<label>IP Gateway</label><input name=gateway_ip maxlength=15 value='%s'>"
        "<button class=btn>GUARDAR / CONECTAR</button></form>"
        "<form method=post action=/gateway><input type=hidden name=action value=verify><button class=btn>VERIFY GATEWAY IP</button></form>"
        "<details><summary>Opciones avanzadas</summary>"
        "<form method=post action=/gateway onsubmit='return confirm(&quot;Eliminar pairing del Gateway?&quot;)'><input type=hidden name=action value=clear><button class='btn danger'>REMOVE GATEWAY IP</button></form></details>"
        "</div>"

        "<div class=card><h2>Estado del enlace</h2><div class=grid>"
        "<div>Gateway configurado</div><div class=mono>%s</div>"
        "<div>Gateway detectado</div><div class=mono>%s</div>"
        "<div>Hostname / Sala</div><div class=mono>%s</div>"
        "<div>Location</div><div class=mono>%s</div>"
        "<div>Configuracion NVS</div><div class=%s>%s</div>"
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
        /* KORVO_V6_5_2_NVS_ESTADO */
        gw.expected_gateway.ip[0] ? "ok" : "bad", gw.expected_gateway.ip[0] ? "GUARDADA" : "NO CONFIGURADA",
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


/* V25-P03: INTERCOM WEB RETIRADO. API SIP de estado conservada. */

static esp_err_t sip_status_api_g(httpd_req_t *r)
{
    if (!guard(r)) return ESP_OK;
    char j[1024];
    korvo_sip_status_json(j, sizeof(j));
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_sendstr(r, j);
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
            "action=/firmware/cancel onsubmit='return confirm(&quot;Eliminar update pendiente?&quot;)'>"

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

        "<p class=small>Archivo .bin validado por version y SHA-256 antes de instalar.</p>"

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

        "<form method=post action=/reboot onsubmit='return confirm(&quot;Reiniciar KORVO ahora?&quot;)'>"

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


    korvo_video01_register(s_server);
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
            .uri = "/api/korvo/sip/status",
            .method = HTTP_GET,
            .handler = sip_status_api_g
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
        "Web admin + SIP intercom + firmware manager ready");

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