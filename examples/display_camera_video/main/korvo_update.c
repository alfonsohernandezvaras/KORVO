#include "korvo_update.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"

static const char *TAG = "korvo_update";

#define EXPECTED_PROJECT "display_camera_new"
#define COPY_BUF 4096


static bool exists(const char *p)
{
    struct stat st;

    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}


static void hex32(const unsigned char *d, char out[65])
{
    static const char h[] = "0123456789abcdef";

    for (int i = 0; i < 32; i++) {
        out[i * 2] = h[d[i] >> 4];
        out[i * 2 + 1] = h[d[i] & 15];
    }

    out[64] = 0;
}


esp_err_t korvo_update_validate_file(
    const char *path,
    korvo_update_info_t *info)
{
    if (!info) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(info, 0, sizeof(*info));

    FILE *f = fopen(path, "rb");

    if (!f) {
        snprintf(
            info->error,
            sizeof(info->error),
            "archivo no disponible");

        return ESP_ERR_NOT_FOUND;
    }

    struct stat st;

    if (stat(path, &st) != 0 ||
        st.st_size <
            sizeof(esp_image_header_t) +
            sizeof(esp_image_segment_header_t) +
            sizeof(esp_app_desc_t)) {

        fclose(f);

        snprintf(
            info->error,
            sizeof(info->error),
            "imagen demasiado pequena");

        return ESP_ERR_INVALID_SIZE;
    }

    info->file_present = true;
    info->size = (size_t)st.st_size;

    esp_image_header_t ih;
    esp_image_segment_header_t sh;

    if (fread(&ih, 1, sizeof(ih), f) != sizeof(ih) ||
        fread(&sh, 1, sizeof(sh), f) != sizeof(sh) ||
        fread(&info->app, 1, sizeof(info->app), f) != sizeof(info->app)) {

        fclose(f);

        snprintf(
            info->error,
            sizeof(info->error),
            "cabecera incompleta");

        return ESP_FAIL;
    }

    if (ih.magic != ESP_IMAGE_HEADER_MAGIC ||
        info->app.magic_word != ESP_APP_DESC_MAGIC_WORD) {

        fclose(f);

        snprintf(
            info->error,
            sizeof(info->error),
            "no es una app ESP-IDF valida");

        return ESP_ERR_INVALID_ARG;
    }

    if (strncmp(
            info->app.project_name,
            EXPECTED_PROJECT,
            sizeof(info->app.project_name)) != 0) {

        fclose(f);

        snprintf(
            info->error,
            sizeof(info->error),
            "proyecto incompatible: %.31s",
            info->app.project_name);

        return ESP_ERR_INVALID_STATE;
    }


    /*
     * Calcular SHA-256 del BIN completo.
     */

    rewind(f);

    unsigned char *buf = malloc(COPY_BUF);
    unsigned char digest[32];

    if (!buf) {
        fclose(f);
        snprintf(info->error, sizeof(info->error), "sin memoria para SHA-256");
        return ESP_ERR_NO_MEM;
    }

    psa_status_t ps = psa_crypto_init();

    if (ps != PSA_SUCCESS) {

        free(buf);
        fclose(f);

        snprintf(
            info->error,
            sizeof(info->error),
            "SHA-256 init fallo: %ld",
            (long)ps);

        return ESP_FAIL;
    }

    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;

    ps = psa_hash_setup(
        &op,
        PSA_ALG_SHA_256);

    if (ps != PSA_SUCCESS) {

        free(buf);
        fclose(f);

        snprintf(
            info->error,
            sizeof(info->error),
            "SHA-256 setup fallo: %ld",
            (long)ps);

        return ESP_FAIL;
    }

    size_t n;

    while ((n = fread(buf, 1, COPY_BUF, f)) > 0) {

        ps = psa_hash_update(
            &op,
            buf,
            n);

        if (ps != PSA_SUCCESS) {
            break;
        }
    }

    size_t digest_len = 0;

    if (ps == PSA_SUCCESS) {

        ps = psa_hash_finish(
            &op,
            digest,
            sizeof(digest),
            &digest_len);

    } else {

        psa_hash_abort(&op);
    }

    free(buf);
    fclose(f);

    if (ps != PSA_SUCCESS ||
        digest_len != sizeof(digest)) {

        snprintf(
            info->error,
            sizeof(info->error),
            "SHA-256 fallo: %ld",
            (long)ps);

        return ESP_FAIL;
    }

    hex32(
        digest,
        info->sha256);

    info->valid = true;
    info->pending = exists(KORVO_UPDATE_PENDING);

    return ESP_OK;
}


esp_err_t korvo_update_get_info(
    korvo_update_info_t *info)
{
    /*
     * LIGHTWEIGHT STATUS PATH.
     *
     * This function is called by HTTP GET handlers.  Do NOT calculate the
     * SHA-256 of the complete BIN here: doing so makes every page navigation
     * read several MB from SD inside the httpd task and can starve the rest of
     * the controller.  Full validation remains mandatory in upload/mark-pending
     * and again during boot before OTA installation.
     */
    if (!info) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(info, 0, sizeof(*info));
    info->pending = exists(KORVO_UPDATE_PENDING);

    FILE *f = fopen(KORVO_UPDATE_FILE, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }

    struct stat st;
    if (stat(KORVO_UPDATE_FILE, &st) != 0 ||
        st.st_size < (off_t)(sizeof(esp_image_header_t) +
                             sizeof(esp_image_segment_header_t) +
                             sizeof(esp_app_desc_t))) {
        fclose(f);
        snprintf(info->error, sizeof(info->error), "imagen demasiado pequena");
        return ESP_ERR_INVALID_SIZE;
    }

    info->file_present = true;
    info->size = (size_t)st.st_size;

    esp_image_header_t ih;
    esp_image_segment_header_t sh;
    if (fread(&ih, 1, sizeof(ih), f) != sizeof(ih) ||
        fread(&sh, 1, sizeof(sh), f) != sizeof(sh) ||
        fread(&info->app, 1, sizeof(info->app), f) != sizeof(info->app)) {
        fclose(f);
        snprintf(info->error, sizeof(info->error), "cabecera incompleta");
        return ESP_FAIL;
    }
    fclose(f);

    if (ih.magic != ESP_IMAGE_HEADER_MAGIC ||
        info->app.magic_word != ESP_APP_DESC_MAGIC_WORD) {
        snprintf(info->error, sizeof(info->error), "no es una app ESP-IDF valida");
        return ESP_ERR_INVALID_ARG;
    }

    if (strncmp(info->app.project_name, EXPECTED_PROJECT,
                sizeof(info->app.project_name)) != 0) {
        snprintf(info->error, sizeof(info->error),
                 "proyecto incompatible: %.31s", info->app.project_name);
        return ESP_ERR_INVALID_STATE;
    }

    /* Header/project are sane. Full cryptographic validation is deferred. */
    info->valid = true;
    return ESP_OK;
}


/* ============================================================
 * COMPARACIÓN DE VERSIONES
 * ============================================================ */

static int vercmp(
    const char *a,
    const char *b)
{
    int am = 0;
    int an = 0;
    int ap = 0;

    int bm = 0;
    int bn = 0;
    int bp = 0;

    if (sscanf(
            a,
            "%d.%d.%d",
            &am,
            &an,
            &ap) == 3 &&
        sscanf(
            b,
            "%d.%d.%d",
            &bm,
            &bn,
            &bp) == 3) {

        if (am != bm) {
            return am > bm ? 1 : -1;
        }

        if (an != bn) {
            return an > bn ? 1 : -1;
        }

        if (ap != bp) {
            return ap > bp ? 1 : -1;
        }

        return 0;
    }

    return strcmp(a, b);
}


/* ============================================================
 * MARCAR UPDATE COMO PENDIENTE
 * ============================================================ */

esp_err_t korvo_update_mark_pending(
    bool allow_downgrade)
{
    korvo_update_info_t i;

    esp_err_t e =
        korvo_update_validate_file(
            KORVO_UPDATE_FILE,
            &i);

    if (e != ESP_OK) {
        return e;
    }

    const esp_app_desc_t *cur =
        esp_app_get_description();

    if (cur &&
        !allow_downgrade &&
        vercmp(i.app.version, cur->version) <= 0) {

        ESP_LOGE(
            TAG,
            "Firmware %s is not newer than running %s",
            i.app.version,
            cur->version);

        return ESP_ERR_INVALID_STATE;
    }

    FILE *f =
        fopen(
            KORVO_UPDATE_PENDING,
            "w");

    if (!f) {
        return ESP_FAIL;
    }

    fprintf(
        f,
        "version=%s\n"
        "sha256=%s\n"
        "size=%u\n"
        "allow_downgrade=%u\n",
        i.app.version,
        i.sha256,
        (unsigned)i.size,
        allow_downgrade ? 1 : 0);

    fclose(f);

    return ESP_OK;
}


/* ============================================================
 * CANCELAR UPDATE
 * ============================================================ */

esp_err_t korvo_update_cancel(void)
{
    unlink(KORVO_UPDATE_PENDING);
    unlink(KORVO_UPDATE_TEMP);
    unlink(KORVO_UPDATE_FILE);

    return ESP_OK;
}


/* ============================================================
 * INSTALAR BIN EN PARTICIÓN OTA INACTIVA
 * ============================================================ */

static esp_err_t install(
    const char *path)
{
    korvo_update_info_t i;

    esp_err_t e =
        korvo_update_validate_file(
            path,
            &i);

    if (e != ESP_OK) {
        return e;
    }


    const esp_partition_t *target =
        esp_ota_get_next_update_partition(NULL);

    if (!target) {
        return ESP_ERR_NOT_FOUND;
    }


    if (i.size > target->size) {

        ESP_LOGE(
            TAG,
            "BIN %u exceeds OTA partition %u",
            (unsigned)i.size,
            (unsigned)target->size);

        return ESP_ERR_INVALID_SIZE;
    }


    FILE *f =
        fopen(
            path,
            "rb");

    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }


    esp_ota_handle_t h = 0;

    e = esp_ota_begin(
        target,
        i.size,
        &h);

    if (e != ESP_OK) {

        fclose(f);

        return e;
    }


    /* Keep the OTA copy buffer off the caller stack (boot runs in main_task). */
    unsigned char *buf = malloc(COPY_BUF);
    if (!buf) {
        esp_ota_abort(h);
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    size_t n;
    size_t total = 0;


    while ((n = fread(
                buf,
                1,
                COPY_BUF,
                f)) > 0) {

        e = esp_ota_write(
            h,
            buf,
            n);

        if (e != ESP_OK) {
            break;
        }

        total += n;
    }


    free(buf);
    fclose(f);


    /*
     * Verificar que se escribió el BIN completo.
     */

    if (e == ESP_OK &&
        total != i.size) {

        e = ESP_FAIL;
    }


    /*
     * Finalizar OTA solamente si todas las escrituras
     * fueron correctas.
     */

    if (e == ESP_OK) {

        e = esp_ota_end(h);

    } else {

        esp_ota_abort(h);
    }


    if (e != ESP_OK) {
        return e;
    }


    /*
     * Seleccionar nueva partición para el próximo boot.
     */

    e = esp_ota_set_boot_partition(
        target);

    if (e != ESP_OK) {
        return e;
    }


    ESP_LOGI(
        TAG,
        "Installed %s (%u bytes) to %s",
        i.app.version,
        (unsigned)i.size,
        target->label);


    /*
     * La actualización ya fue instalada correctamente.
     * Eliminamos el marcador pendiente.
     */

    unlink(KORVO_UPDATE_PENDING);


    /*
     * Archivar BIN instalado.
     */

    char archive[160];

    snprintf(
        archive,
        sizeof(archive),
        KORVO_ARCHIVE_DIR "/korvo-%s.bin",
        i.app.version);

    unlink(archive);

    rename(
        path,
        archive);


    return ESP_OK;
}


/* ============================================================
 * COMPROBACIÓN DE UPDATE DURANTE BOOT
 * ============================================================ */

esp_err_t korvo_update_boot_check(void)
{
    if (!exists(KORVO_UPDATE_PENDING)) {

        ESP_LOGI(
            TAG,
            "No pending SD firmware update");

        return ESP_OK;
    }


    ESP_LOGW(
        TAG,
        "Pending SD firmware update found");


    if (!exists(KORVO_UPDATE_FILE)) {

        ESP_LOGE(
            TAG,
            "pending.txt exists but korvo.bin is missing; "
            "canceling marker");

        unlink(KORVO_UPDATE_PENDING);

        return ESP_ERR_NOT_FOUND;
    }


    /*
     * Vincular pending.txt al BIN exacto previamente
     * validado durante la subida web.
     */

    char expected_ver[40] = {0};
    char expected_sha[65] = {0};
    char line[160];

    FILE *mf =
        fopen(
            KORVO_UPDATE_PENDING,
            "r");

    if (!mf) {
        return ESP_FAIL;
    }


    while (fgets(
               line,
               sizeof(line),
               mf)) {

        if (!strncmp(
                line,
                "version=",
                8)) {

            strlcpy(
                expected_ver,
                line + 8,
                sizeof(expected_ver));

            expected_ver[
                strcspn(
                    expected_ver,
                    "\r\n")
            ] = 0;

        } else if (!strncmp(
                       line,
                       "sha256=",
                       7)) {

            strlcpy(
                expected_sha,
                line + 7,
                sizeof(expected_sha));

            expected_sha[
                strcspn(
                    expected_sha,
                    "\r\n")
            ] = 0;
        }
    }


    fclose(mf);


    /*
     * Revalidar el BIN antes de tocar la partición OTA.
     */

    korvo_update_info_t check = {0};

    esp_err_t e =
        korvo_update_validate_file(
            KORVO_UPDATE_FILE,
            &check);


    if (e != ESP_OK ||
        !expected_ver[0] ||
        !expected_sha[0] ||
        strcmp(
            expected_ver,
            check.app.version) != 0 ||
        strcasecmp(
            expected_sha,
            check.sha256) != 0) {

        ESP_LOGE(
            TAG,
            "Pending marker does not match korvo.bin - "
            "update blocked");

        return ESP_ERR_INVALID_CRC;
    }


    /*
     * BIN y marcador coinciden.
     */

    e = install(
        KORVO_UPDATE_FILE);


    if (e != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Update rejected/failed: %s; "
            "current firmware preserved",
            esp_err_to_name(e));

        return e;
    }


    ESP_LOGW(
        TAG,
        "Update complete. Rebooting into new firmware...");


    fflush(NULL);

    vTaskDelay(
        pdMS_TO_TICKS(500));


    esp_restart();


    /*
     * esp_restart() no debería retornar.
     */

    return ESP_OK;
}