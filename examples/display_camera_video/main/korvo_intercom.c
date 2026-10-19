#include "korvo_intercom.h"
#include "korvo_bluetooth.h"
#include "korvo_alarm.h"
#include "esp_log.h"

/* KORVO_VIDEO_V1_R1_SIP_DISABLED
 * SIP/RTP belongs to Gateway2. Preserve Bluetooth and alarm initialization.
 * Legacy SIP implementation, web settings and HMI status remain compiled for
 * compatibility; no KORVO SIP socket/task is started here.
 * No camera, watchdog, LVGL, or network code is changed.
 */
static const char *TAG = "korvo_intercom";

esp_err_t korvo_intercom_init(void)
{
    esp_err_t bt = korvo_bluetooth_init();
    if (bt != ESP_OK) {
        ESP_LOGW(TAG, "Bluetooth init: %s", esp_err_to_name(bt));
    }
    esp_err_t alarm = korvo_alarm_init();
    if (alarm != ESP_OK) {
        ESP_LOGW(TAG, "Alarm audio init: %s", esp_err_to_name(alarm));
    }
    ESP_LOGI(TAG, "VIDEO V1 R1: local SIP disabled; Gateway2 owns SIP/RTP");
    return (bt != ESP_OK && alarm != ESP_OK) ? ESP_FAIL : ESP_OK;
}
