#include "korvo_storage.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "bsp/esp-bsp.h"
#include "esp_log.h"

static const char *TAG = "korvo_storage";
static bool s_ready = false;

#define KORVO_STORAGE_LAYOUT_VERSION "1"
#define KORVO_STORAGE_VERSION_FILE BSP_SD_MOUNT_POINT "/korvo/system/storage.version"

static esp_err_t ensure_dir(const char *path)
{
    if (mkdir(path, 0775) == 0 || errno == EEXIST) {
        return ESP_OK;
    }

    ESP_LOGE(TAG, "mkdir failed: %s errno=%d", path, errno);
    return ESP_FAIL;
}

static esp_err_t write_layout_version(void)
{
    FILE *f = fopen(KORVO_STORAGE_VERSION_FILE, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "Cannot create %s", KORVO_STORAGE_VERSION_FILE);
        return ESP_FAIL;
    }

    fprintf(f, "%s\n", KORVO_STORAGE_LAYOUT_VERSION);
    fclose(f);
    return ESP_OK;
}

esp_err_t korvo_storage_create_layout(void)
{
    static const char *dirs[] = {
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

        BSP_SD_MOUNT_POINT "/korvo/sip",
        BSP_SD_MOUNT_POINT "/korvo/events",
        BSP_SD_MOUNT_POINT "/korvo/logs",

        BSP_SD_MOUNT_POINT "/korvo/firmware",
        BSP_SD_MOUNT_POINT "/korvo/firmware/current",
        BSP_SD_MOUNT_POINT "/korvo/firmware/releases",
        BSP_SD_MOUNT_POINT "/korvo/firmware/staging",
        BSP_SD_MOUNT_POINT "/korvo/firmware/recovery",

        BSP_SD_MOUNT_POINT "/korvo/sync",
        BSP_SD_MOUNT_POINT "/korvo/sync/inbox",
        BSP_SD_MOUNT_POINT "/korvo/sync/processed",

        BSP_SD_MOUNT_POINT "/korvo/backup",
        BSP_SD_MOUNT_POINT "/korvo/update",
        BSP_SD_MOUNT_POINT "/korvo/temp"
    };

    for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i) {
        esp_err_t err = ensure_dir(dirs[i]);
        if (err != ESP_OK) {
            return err;
        }
    }

    return write_layout_version();
}

esp_err_t korvo_storage_init(void)
{
    s_ready = false;

    /*
     * IMPORTANT:
     * Mount only. Never auto-format here.
     * A mount failure must not destroy identities, firmware or configuration.
     * Formatting will be an explicit protected maintenance operation later.
     */
    bsp_sdcard_cfg_t cfg = {0};
    esp_err_t err = bsp_sdcard_sdmmc_mount(&cfg);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "SD mounted at %s", BSP_SD_MOUNT_POINT);

    err = korvo_storage_create_layout();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create/verify KORVO directory layout");
        return err;
    }

    s_ready = true;
    ESP_LOGI(TAG, "KORVO SD ready: %s/korvo (layout v%s)",
             BSP_SD_MOUNT_POINT, KORVO_STORAGE_LAYOUT_VERSION);

    return ESP_OK;
}

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
