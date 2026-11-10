#include "korvo_storage.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"

static const char *TAG = "korvo_storage";

static bool s_ready = false;
static bool s_mounted = false;
static korvo_storage_state_t s_state = KORVO_STORAGE_STATE_ERROR;

#define KORVO_STORAGE_LAYOUT_VERSION "1"
#define KORVO_STORAGE_VERSION_FILE \
    BSP_SD_MOUNT_POINT "/korvo/system/storage.version"


/* ============================================================
 * ESTRUCTURA OFICIAL DE LA SD KORVO
 * ============================================================ */

static const char *s_required_dirs[] = {

    BSP_SD_MOUNT_POINT "/korvo",

    BSP_SD_MOUNT_POINT "/korvo/system",
    BSP_SD_MOUNT_POINT "/korvo/system/www",
    BSP_SD_MOUNT_POINT "/korvo/system/assets",
    BSP_SD_MOUNT_POINT "/korvo/system/recovery",

    BSP_SD_MOUNT_POINT "/korvo/config",
    BSP_SD_MOUNT_POINT "/korvo/config/active",
    BSP_SD_MOUNT_POINT "/korvo/config/staging",
    BSP_SD_MOUNT_POINT "/korvo/config/backup",

    BSP_SD_MOUNT_POINT "/korvo/identity",
    BSP_SD_MOUNT_POINT "/korvo/identity/users",
    BSP_SD_MOUNT_POINT "/korvo/identity/face",
    BSP_SD_MOUNT_POINT "/korvo/identity/face/templates",
    BSP_SD_MOUNT_POINT "/korvo/identity/face/images",
    BSP_SD_MOUNT_POINT "/korvo/identity/rfid",

    BSP_SD_MOUNT_POINT "/korvo/access",
    BSP_SD_MOUNT_POINT "/korvo/access/permissions",
    BSP_SD_MOUNT_POINT "/korvo/access/io-maps",

    BSP_SD_MOUNT_POINT "/korvo/audio",
    BSP_SD_MOUNT_POINT "/korvo/audio/alarms",

    BSP_SD_MOUNT_POINT "/korvo/sip",
    BSP_SD_MOUNT_POINT "/korvo/events",
    BSP_SD_MOUNT_POINT "/korvo/logs",

    BSP_SD_MOUNT_POINT "/korvo/firmware",
    BSP_SD_MOUNT_POINT "/korvo/firmware/update",
    BSP_SD_MOUNT_POINT "/korvo/firmware/recovery",
    BSP_SD_MOUNT_POINT "/korvo/firmware/archive",

    BSP_SD_MOUNT_POINT "/korvo/sync",
    BSP_SD_MOUNT_POINT "/korvo/sync/inbox",
    BSP_SD_MOUNT_POINT "/korvo/sync/processed",

    BSP_SD_MOUNT_POINT "/korvo/backup",
    BSP_SD_MOUNT_POINT "/korvo/temp"
};


/*
 * Firmas fuertes KORVO.
 *
 * Un simple directorio /korvo no es suficiente para autorizar
 * modificaciones automáticas sobre una tarjeta que pudiera
 * contener datos ajenos al sistema.
 */

static const char *s_korvo_signature_dirs[] = {

    BSP_SD_MOUNT_POINT "/korvo/system",
    BSP_SD_MOUNT_POINT "/korvo/config",
    BSP_SD_MOUNT_POINT "/korvo/identity",
    BSP_SD_MOUNT_POINT "/korvo/access",
    BSP_SD_MOUNT_POINT "/korvo/firmware",
    BSP_SD_MOUNT_POINT "/korvo/sync"
};


/* ============================================================
 * UTILIDADES
 * ============================================================ */

static esp_err_t ensure_dir(const char *path)
{
    if (mkdir(path, 0775) == 0 || errno == EEXIST) {
        return ESP_OK;
    }

    ESP_LOGE(
        TAG,
        "mkdir failed: %s errno=%d",
        path,
        errno
    );

    return ESP_FAIL;
}


static esp_err_t write_layout_version(void)
{
    FILE *f = fopen(KORVO_STORAGE_VERSION_FILE, "w");

    if (f == NULL) {

        ESP_LOGE(
            TAG,
            "Cannot create %s",
            KORVO_STORAGE_VERSION_FILE
        );

        return ESP_FAIL;
    }

    fprintf(
        f,
        "%s\n",
        KORVO_STORAGE_LAYOUT_VERSION
    );

    fclose(f);

    return ESP_OK;
}


static bool path_exists(const char *path)
{
    struct stat st = {0};

    return stat(path, &st) == 0;
}


static bool path_is_directory(const char *path)
{
    struct stat st = {0};

    if (stat(path, &st) != 0) {
        return false;
    }

    return S_ISDIR(st.st_mode);
}


/* ============================================================
 * DETECCIÓN DE SD VACÍA
 * ============================================================ */

static bool mount_root_is_empty(void)
{
    DIR *dir = opendir(BSP_SD_MOUNT_POINT);

    if (dir == NULL) {
        return false;
    }

    bool empty = true;

    struct dirent *entry = NULL;

    while ((entry = readdir(dir)) != NULL) {

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0 ||
            strcmp(entry->d_name, "System Volume Information") == 0) {

            continue;
        }

        empty = false;
        break;
    }

    closedir(dir);

    return empty;
}


/* ============================================================
 * DETECCIÓN DE FIRMA KORVO
 * ============================================================ */

static unsigned korvo_signature_count(void)
{
    unsigned count = 0;

    /*
     * storage.version constituye evidencia fuerte de que
     * esta tarjeta ya fue utilizada por KORVO.
     */

    if (path_exists(KORVO_STORAGE_VERSION_FILE)) {
        count += 3;
    }

    for (unsigned i = 0;
         i < sizeof(s_korvo_signature_dirs) /
             sizeof(s_korvo_signature_dirs[0]);
         ++i) {

        struct stat st = {0};

        if (stat(s_korvo_signature_dirs[i], &st) == 0 &&
            S_ISDIR(st.st_mode)) {

            count++;
        }
    }

    return count;
}


/* ============================================================
 * CREACIÓN / REPARACIÓN DE ESTRUCTURA
 * ============================================================ */

esp_err_t korvo_storage_create_layout(void)
{
    for (unsigned i = 0;
         i < sizeof(s_required_dirs) /
             sizeof(s_required_dirs[0]);
         ++i) {

        esp_err_t err = ensure_dir(s_required_dirs[i]);

        if (err != ESP_OK) {
            return err;
        }
    }

    return write_layout_version();
}


/* ============================================================
 * INICIALIZACIÓN
 * ============================================================ */

esp_err_t korvo_storage_init(void)
{
    s_ready = false;
    s_mounted = false;
    s_state = KORVO_STORAGE_STATE_ERROR;

    /*
     * Montamos normalmente.
     *
     * No se realiza formateo automático ante un fallo ambiguo:
     * la tarjeta podría contener NTFS/exFAT, corrupción recuperable
     * o información ajena al sistema KORVO.
     */

    bsp_sdcard_cfg_t cfg = {0};

    esp_err_t err = bsp_sdcard_sdmmc_mount(&cfg);

    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "SD mount failed: %s - card left untouched",
            esp_err_to_name(err)
        );

        return err;
    }

    s_mounted = true;

    ESP_LOGI(
        TAG,
        "SD mounted at %s",
        BSP_SD_MOUNT_POINT
    );


    /* --------------------------------------------------------
     * Verificar filesystem FAT
     * -------------------------------------------------------- */

    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;

    err = esp_vfs_fat_info(
        BSP_SD_MOUNT_POINT,
        &total_bytes,
        &free_bytes
    );

    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "FAT filesystem status failed: %s",
            esp_err_to_name(err)
        );

        return err;
    }


    /*
     * Clasificación:
     *
     * 1. FAT vacío
     *      -> inicializar estructura completa.
     *
     * 2. KORVO parcial / antiguo
     *      -> crear solamente rutas faltantes.
     *
     * 3. Datos externos
     *      -> rechazar sin modificar.
     */

    bool empty = mount_root_is_empty();

    unsigned signature = korvo_signature_count();


    if (empty) {

        s_state = KORVO_STORAGE_STATE_INITIALIZING;

        ESP_LOGI(
            TAG,
            "Blank FAT SD detected - initializing KORVO structure"
        );

    } else if (signature >= 2) {

        s_state = KORVO_STORAGE_STATE_REPAIRING;

        ESP_LOGW(
            TAG,
            "Partial/legacy KORVO SD detected "
            "(signature=%u) - correcting structure",
            signature
        );

    } else {

        s_state = KORVO_STORAGE_STATE_FOREIGN;

        ESP_LOGW(
            TAG,
            "Foreign/non-KORVO data detected - "
            "card is not suitable for automatic initialization"
        );

        return ESP_ERR_INVALID_STATE;
    }


    err = korvo_storage_create_layout();

    if (err != ESP_OK) {

        s_state = KORVO_STORAGE_STATE_ERROR;

        ESP_LOGE(
            TAG,
            "Failed to create/correct KORVO directory layout"
        );

        return err;
    }


    s_ready = true;
    s_state = KORVO_STORAGE_STATE_READY;

    ESP_LOGI(
        TAG,
        "KORVO SD ready: %s/korvo (layout v%s)",
        BSP_SD_MOUNT_POINT,
        KORVO_STORAGE_LAYOUT_VERSION
    );

    return ESP_OK;
}


/* ============================================================
 * VALIDACIÓN DE ESTRUCTURA
 * ============================================================ */

static bool layout_is_valid(void)
{
    for (unsigned i = 0;
         i < sizeof(s_required_dirs) /
             sizeof(s_required_dirs[0]);
         ++i) {

        if (!path_is_directory(s_required_dirs[i])) {

            ESP_LOGE(
                TAG,
                "Required KORVO directory "
                "missing/inaccessible: %s",
                s_required_dirs[i]
            );

            return false;
        }
    }


    FILE *f = fopen(
        KORVO_STORAGE_VERSION_FILE,
        "r"
    );

    if (f == NULL) {

        ESP_LOGE(
            TAG,
            "Cannot read %s",
            KORVO_STORAGE_VERSION_FILE
        );

        return false;
    }


    char version[16] = {0};

    bool read_ok =
        (fgets(
            version,
            sizeof(version),
            f
        ) != NULL);

    fclose(f);


    if (!read_ok) {

        ESP_LOGE(
            TAG,
            "Cannot read KORVO storage layout version"
        );

        return false;
    }


    version[
        strcspn(version, "\r\n")
    ] = '\0';


    if (strcmp(
            version,
            KORVO_STORAGE_LAYOUT_VERSION
        ) != 0) {

        ESP_LOGE(
            TAG,
            "KORVO storage layout version mismatch: "
            "found='%s' expected='%s'",
            version,
            KORVO_STORAGE_LAYOUT_VERSION
        );

        return false;
    }


    return true;
}


/* ============================================================
 * ESTADO / WATCHDOG DE STORAGE
 * ============================================================ */

esp_err_t korvo_storage_get_status(
    korvo_storage_status_t *status
)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }


    memset(
        status,
        0,
        sizeof(*status)
    );

    status->state = s_state;


    if (!s_mounted) {

        s_ready = false;

        return ESP_ERR_INVALID_STATE;
    }


    status->sd_ok = true;


    /*
     * Se utiliza la API FATFS de ESP-IDF en lugar de statvfs().
     *
     * Esto consulta directamente el volumen FAT registrado
     * en BSP_SD_MOUNT_POINT.
     */

    esp_err_t fs_err = esp_vfs_fat_info(
        BSP_SD_MOUNT_POINT,
        &status->total_bytes,
        &status->free_bytes
    );


    if (fs_err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "FAT filesystem status failed: %s",
            esp_err_to_name(fs_err)
        );

        s_ready = false;

        return fs_err;
    }


    status->fs_ok = true;


    if (status->free_bytes >
        status->total_bytes) {

        status->free_bytes =
            status->total_bytes;
    }


    status->used_bytes =
        status->total_bytes -
        status->free_bytes;


    /*
     * El watchdog actúa solamente como supervisor.
     *
     * Verifica todos los directorios requeridos y
     * storage.version.
     *
     * NO repara automáticamente durante operación normal.
     */

    if (s_state ==
        KORVO_STORAGE_STATE_FOREIGN) {

        status->structure_ok = false;
        status->ok = false;

        s_ready = false;

        return ESP_ERR_INVALID_STATE;
    }


    status->structure_ok =
        layout_is_valid();


    status->ok =
        status->sd_ok &&
        status->fs_ok &&
        status->structure_ok;


    s_ready = status->ok;


    if (status->ok) {

        s_state =
            KORVO_STORAGE_STATE_READY;

        status->state =
            KORVO_STORAGE_STATE_READY;

        return ESP_OK;
    }


    return ESP_FAIL;
}


/* ============================================================
 * CONSULTAS
 * ============================================================ */

bool korvo_storage_is_ready(void)
{
    return s_ready;
}


const char *korvo_storage_root(void)
{
    return BSP_SD_MOUNT_POINT "/korvo";
}


const char *korvo_storage_layout_version(void)
{
    return KORVO_STORAGE_LAYOUT_VERSION;
}


korvo_storage_state_t korvo_storage_state(void)
{
    return s_state;
}