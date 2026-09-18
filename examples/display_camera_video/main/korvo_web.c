#include "korvo_web.h"
#include "korvo_network.h"
#include "korvo_storage.h"
#include "korvo_update.h"

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
    "input{width:100%;padding:11px;border:1px solid #445363;"
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
    char b[1600];

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
        sizeof(b),
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

        "<div>Gateway / controlador</div>"
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

        "<label>IP Gateway / controlador KORVO</label>"
        "<input name=gateway_node maxlength=15 value='%s'>"

        "<button class=btn>"
        "GUARDAR Y REINICIAR"
        "</button>"

        "</form>"

        "<p class=small>"
        "Laboratorio: korvo_wifi / "
        "KORVO 192.168.10.50 / "
        "LAN GW 192.168.10.1 / "
        "Gateway KORVO 192.168.10.51"
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


    c.max_uri_handlers = 16;
    c.lru_purge_enable = true;
    c.stack_size = 16384;
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
        "Web admin + firmware manager ready");

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