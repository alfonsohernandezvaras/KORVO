#include "korvo_intercom.h"
#include "korvo_gateway.h"
#include "korvo_sip.h"
#include "korvo_bluetooth.h"
#include "korvo_alarm.h"

#include <string.h>
#include "esp_log.h"

static const char *TAG = "korvo_intercom";

static void gateway_event_cb(const korvo_gateway_event_t *event, void *ctx)
{
    (void)ctx;
    if (!event || !event->name[0]) return;

    korvo_sip_status_t sip = {0};
    korvo_sip_get_status(&sip);

    if (!strcmp(event->name, "call.short_press")) {
        if (sip.state == KORVO_SIP_INCOMING) {
            ESP_LOGI(TAG, "Gateway CALL short -> answer incoming SIP call");
            (void)korvo_sip_answer();
        } else if (sip.state == KORVO_SIP_IDLE ||
                   sip.state == KORVO_SIP_ENDED ||
                   sip.state == KORVO_SIP_BUSY ||
                   sip.state == KORVO_SIP_NO_ANSWER ||
                   sip.state == KORVO_SIP_REJECTED ||
                   sip.state == KORVO_SIP_FAILED) {
            ESP_LOGI(TAG, "Gateway CALL short -> call configured operator");
            (void)korvo_sip_call_default();
        }
        return;
    }

    if (!strcmp(event->name, "call.long_press")) {
        if (sip.state == KORVO_SIP_CALLING ||
            sip.state == KORVO_SIP_RINGING ||
            sip.state == KORVO_SIP_INCOMING ||
            sip.state == KORVO_SIP_CONNECTING ||
            sip.state == KORVO_SIP_CONNECTED) {
            ESP_LOGI(TAG, "Gateway CALL long -> hangup SIP call");
            (void)korvo_sip_hangup();
        }
    }
}

static void bt_event_cb(const korvo_bluetooth_event_t *event, void *ctx)
{
    (void)ctx;
    if (!event) return;

    switch (event->type) {
        case KORVO_BT_EVENT_ANSWER_REQUEST:
            ESP_LOGI(TAG, "Bluetooth HF -> answer SIP call");
            (void)korvo_sip_answer();
            break;
        case KORVO_BT_EVENT_HANGUP_REQUEST:
            ESP_LOGI(TAG, "Bluetooth HF -> hangup SIP call");
            (void)korvo_sip_hangup();
            break;
        case KORVO_BT_EVENT_DIAL_REQUEST:
            ESP_LOGI(TAG, "Bluetooth HF -> dial %s", event->number[0] ? event->number : "operator");
            if (event->number[0]) (void)korvo_sip_call(event->number);
            else (void)korvo_sip_call_default();
            break;
        default:
            break;
    }
}

esp_err_t korvo_intercom_init(void)
{
    esp_err_t bt = korvo_bluetooth_init();
    if (bt != ESP_OK) {
        ESP_LOGW(TAG, "Bluetooth/HFP init: %s", esp_err_to_name(bt));
    }

    esp_err_t alarm = korvo_alarm_init();
    if (alarm != ESP_OK) {
        ESP_LOGW(TAG, "Alarm audio init: %s", esp_err_to_name(alarm));
    }

    esp_err_t sip = korvo_sip_init();
    if (sip != ESP_OK) {
        ESP_LOGW(TAG, "SIP init: %s", esp_err_to_name(sip));
    }

    esp_err_t ge = korvo_gateway_add_listener(gateway_event_cb, NULL);
    if (ge != ESP_OK) {
        ESP_LOGW(TAG, "Gateway listener: %s", esp_err_to_name(ge));
    }

    (void)korvo_bluetooth_set_event_listener(bt_event_cb, NULL);

    if (bt != ESP_OK && sip != ESP_OK) return ESP_FAIL;
    return ESP_OK;
}
