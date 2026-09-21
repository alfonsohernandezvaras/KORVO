#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KORVO_BT_NAME_MAX       64
#define KORVO_BT_MAC_MAX        18
#define KORVO_BT_PIN_MAX        17
#define KORVO_BT_ERROR_MAX      96
#define KORVO_BT_SCAN_MAX       12
#define KORVO_BT_NUMBER_MAX     32

typedef struct {
    bool enabled;
    bool auto_connect;
    char local_name[KORVO_BT_NAME_MAX];
    char pin[KORVO_BT_PIN_MAX];
    char peer_mac[KORVO_BT_MAC_MAX];
    char peer_name[KORVO_BT_NAME_MAX];
} korvo_bluetooth_config_t;

typedef struct {
    char mac[KORVO_BT_MAC_MAX];
    char name[KORVO_BT_NAME_MAX];
    int rssi;
} korvo_bluetooth_device_t;

typedef struct {
    bool enabled;
    bool engine_ready;
    bool scanning;
    bool paired;
    bool connecting;
    bool slc_connected;
    bool audio_connected;
    char local_name[KORVO_BT_NAME_MAX];
    char peer_mac[KORVO_BT_MAC_MAX];
    char peer_name[KORVO_BT_NAME_MAX];
    char codec[16];
    uint32_t pairing_number;
    char last_error[KORVO_BT_ERROR_MAX];
    size_t device_count;
    korvo_bluetooth_device_t devices[KORVO_BT_SCAN_MAX];
} korvo_bluetooth_status_t;

typedef enum {
    KORVO_BT_EVENT_NONE = 0,
    KORVO_BT_EVENT_ANSWER_REQUEST,
    KORVO_BT_EVENT_HANGUP_REQUEST,
    KORVO_BT_EVENT_DIAL_REQUEST,
} korvo_bluetooth_event_type_t;

typedef struct {
    korvo_bluetooth_event_type_t type;
    char number[KORVO_BT_NUMBER_MAX];
} korvo_bluetooth_event_t;

typedef void (*korvo_bluetooth_event_listener_t)(const korvo_bluetooth_event_t *event, void *ctx);

esp_err_t korvo_bluetooth_init(void);
esp_err_t korvo_bluetooth_get_config(korvo_bluetooth_config_t *cfg);
esp_err_t korvo_bluetooth_save_config(const korvo_bluetooth_config_t *cfg);
void korvo_bluetooth_get_status(korvo_bluetooth_status_t *status);
size_t korvo_bluetooth_status_json(char *out, size_t out_len);
esp_err_t korvo_bluetooth_set_event_listener(korvo_bluetooth_event_listener_t listener, void *ctx);

esp_err_t korvo_bluetooth_scan(void);
esp_err_t korvo_bluetooth_connect(const char *mac);
esp_err_t korvo_bluetooth_disconnect(void);
esp_err_t korvo_bluetooth_forget(void);
esp_err_t korvo_bluetooth_audio_connect(void);
esp_err_t korvo_bluetooth_audio_disconnect(void);
void korvo_bluetooth_audio_kick(void);
void korvo_bluetooth_supervise(void);

/* Local output gain applied to PCM sent over HFP/SCO. 0..100 %.
 * persist=true stores the setting in NVS; false is intended for live slider motion. */
uint8_t korvo_bluetooth_get_output_volume(void);
esp_err_t korvo_bluetooth_set_output_volume(uint8_t percent, bool persist);

/* Local microphone gain applied before G.711/RTP encode. 0..100 %. */
uint8_t korvo_bluetooth_get_mic_gain(void);
esp_err_t korvo_bluetooth_set_mic_gain(uint8_t percent, bool persist);

void korvo_bluetooth_notify_outgoing(const char *number, bool alerting);
void korvo_bluetooth_notify_incoming(const char *number);
void korvo_bluetooth_notify_active(const char *number);
void korvo_bluetooth_notify_ended(const char *number);

#ifdef __cplusplus
}
#endif
