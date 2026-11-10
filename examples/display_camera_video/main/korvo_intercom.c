#include "korvo_intercom.h"
#include "korvo_alarm.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

/* KORVO_V25_P01_BT_HARD_OFF
 * Gateway2 owns SIP/RTP/Bluetooth. No KORVO Bluetooth startup.
 * Preserve alarm initialization; camera/RGB/watchdogs untouched.
 */
static const char *TAG = "korvo_intercom";

static void log_resources(const char *stage)
{
    ESP_LOGI(TAG, "P01 MEM %s: internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u",
             stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

esp_err_t korvo_intercom_init(void)
{
    log_resources("before alarm");
    esp_err_t alarm = korvo_alarm_init();
    log_resources("after alarm");
    if (alarm != ESP_OK) ESP_LOGW(TAG, "Alarm audio init: %s", esp_err_to_name(alarm));
    ESP_LOGI(TAG, "P01: KORVO SIP and local Bluetooth OFF; Gateway2 owns communications");
    return alarm;
}
