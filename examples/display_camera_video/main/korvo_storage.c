#include "korvo_storage.h"

#include <errno.h>
#include <sys/stat.h>
#include "bsp/esp-bsp.h"
#include "esp_log.h"

static const char *TAG = "korvo_storage";
static bool s_ready = false;

static esp_err_t ensure_dir(const char *path)
{
    if (mkdir(path, 0775) == 0 || errno == EEXIST) return ESP_OK;
    ESP_LOGE(TAG, "mkdir failed: %s errno=%d", path, errno);
    return ESP_FAIL;
}

esp_err_t korvo_storage_init(void)
{
    bsp_sdcard_cfg_t cfg = {0};
    esp_err_t err = bsp_sdcard_sdmmc_mount(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(err));
        s_ready = false;
        return err;
    }

    static const char *dirs[] = {
        BSP_SD_MOUNT_POINT "/korvo",
        BSP_SD_MOUNT_POINT "/korvo/system",
        BSP_SD_MOUNT_POINT "/korvo/system/www",
        BSP_SD_MOUNT_POINT "/korvo/config",
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
        BSP_SD_MOUNT_POINT "/korvo/firmware",
        BSP_SD_MOUNT_POINT "/korvo/firmware/releases",
        BSP_SD_MOUNT_POINT "/korvo/firmware/staging",
        BSP_SD_MOUNT_POINT "/korvo/firmware/recovery",
        BSP_SD_MOUNT_POINT "/korvo/sync",
        BSP_SD_MOUNT_POINT "/korvo/events",
        BSP_SD_MOUNT_POINT "/korvo/logs",
        BSP_SD_MOUNT_POINT "/korvo/backup",
        BSP_SD_MOUNT_POINT "/korvo/update",
        BSP_SD_MOUNT_POINT "/korvo/temp"
    };

    for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i) {
        err = ensure_dir(dirs[i]);
        if (err != ESP_OK) {
            s_ready = false;
            return err;
        }
    }

    s_ready = true;
    ESP_LOGI(TAG, "KORVO SD ready at %s/korvo", BSP_SD_MOUNT_POINT);
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
