#include "korvo_config.h"

#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "korvo_config";
static const char *NVS_NS = "korvo";
static const char *NVS_KEY = "config";
static korvo_config_t s_cfg;

void korvo_config_set_defaults(korvo_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strcpy(cfg->device_id, "KORVO-001");
    strcpy(cfg->room_id, "LAB");
    strcpy(cfg->hostname, "korvo-001");
    strcpy(cfg->ip, "192.168.5.50");
    strcpy(cfg->netmask, "255.255.255.0");
    strcpy(cfg->gateway_ip, "192.168.5.51");
    cfg->web_port = 80;
    strcpy(cfg->gateway_host, "192.168.5.51");
    cfg->gateway_api_port = 8080;
    cfg->sip_port = 5060;
    cfg->rtp_port_start = 10000;
    cfg->rtp_port_end = 10100;
    cfg->face_enabled = 1;
    cfg->rfid_enabled = 1;
    cfg->sip_enabled = 0;
    cfg->gateway_enabled = 1;
}

esp_err_t korvo_config_load(korvo_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t len = sizeof(*cfg);
    err = nvs_get_blob(h, NVS_KEY, cfg, &len);
    nvs_close(h);
    if (err == ESP_OK && len != sizeof(*cfg)) return ESP_ERR_INVALID_SIZE;
    return err;
}

esp_err_t korvo_config_save(const korvo_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, NVS_KEY, cfg, sizeof(*cfg));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) s_cfg = *cfg;
    return err;
}

esp_err_t korvo_config_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    err = korvo_config_load(&s_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Using KORVO recovery defaults");
        korvo_config_set_defaults(&s_cfg);
        return korvo_config_save(&s_cfg);
    }
    return ESP_OK;
}

esp_err_t korvo_config_factory_reset(void)
{
    korvo_config_t defaults;
    korvo_config_set_defaults(&defaults);
    return korvo_config_save(&defaults);
}

const korvo_config_t *korvo_config_get(void)
{
    return &s_cfg;
}
