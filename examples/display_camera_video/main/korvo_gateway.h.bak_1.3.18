#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KORVO_GW_NAME_MAX      64
#define KORVO_GW_HOST_MAX      64
#define KORVO_GW_LOCATION_MAX  64
#define KORVO_GW_IP_MAX        16
#define KORVO_GW_MAC_MAX       18
#define KORVO_GW_PAIR_ID_MAX   64
#define KORVO_GW_PAIR_KEY_MAX  96
#define KORVO_GW_STATE_MAX     32
#define KORVO_GW_EVENT_MAX     64
#define KORVO_GW_EVENT_DATA_MAX 384

typedef enum {
    KORVO_GW_UNCONFIGURED = 0,
    KORVO_GW_OFFLINE,
    KORVO_GW_CONNECTING,
    KORVO_GW_AUTHENTICATING,
    KORVO_GW_OK,
    KORVO_GW_MISMATCH,
    KORVO_GW_AUTH_FAILED,
} korvo_gateway_state_t;

typedef struct {
    char name[KORVO_GW_NAME_MAX];
    char hostname[KORVO_GW_HOST_MAX];
    char location[KORVO_GW_LOCATION_MAX];
    char ip[KORVO_GW_IP_MAX];
    char mac[KORVO_GW_MAC_MAX];
} korvo_gateway_identity_t;


typedef struct {
    uint32_t seq;
    char name[KORVO_GW_EVENT_MAX];
    bool simulated;
    uint64_t received_ms;
    char data[KORVO_GW_EVENT_DATA_MAX];
} korvo_gateway_event_t;

typedef void (*korvo_gateway_event_listener_t)(const korvo_gateway_event_t *event, void *ctx);

typedef struct {
    korvo_gateway_identity_t local;
    korvo_gateway_identity_t expected_gateway;
    korvo_gateway_identity_t observed_gateway;

    char pair_id[KORVO_GW_PAIR_ID_MAX]; /* legacy V1.1 field; ignored by V1.2 */
    korvo_gateway_state_t state;

    bool paired;
    bool wifi_connected;
    char wifi_bssid[KORVO_GW_MAC_MAX];
    int wifi_rssi;

    bool identity_match;
    bool wifi_mac_match; /* diagnostic only */
    bool websocket_connected;
    bool pair_auth_ok; /* legacy name: true when IP-authorized WS is live */
    bool gateway_ok;
    bool control_ok;

    bool sip_registered;
    char sip_state[KORVO_GW_STATE_MAX];
    char last_event[KORVO_GW_EVENT_MAX];
    uint64_t last_rx_ms;
    char last_error[96];
} korvo_gateway_status_t;

/* Initializes NVS state and the Gateway V2.5 simple-IP WebSocket manager. */
esp_err_t korvo_gateway_init(void);

/* Local identity is diagnostic metadata. It does not authorize control in
 * V1.2; reciprocal IP configuration + the live WebSocket does. */
esp_err_t korvo_gateway_set_local_identity(const char *name,
                                           const char *hostname,
                                           const char *location);

/* Save/verify the explicit Gateway IPv4 address. No scan, Pair ID, Pair Key,
 * MAC binding or enrollment window is used in V1.2. */
esp_err_t korvo_gateway_enroll(const char *target_ip, char *message, size_t message_len);

/* Verifies only the currently configured Gateway IP. Identity metadata is
 * diagnostic and never gates GPIO/control. */
esp_err_t korvo_gateway_verify(char *message, size_t message_len);

/* Clears the local pair and immediately disables field-control commands. */
esp_err_t korvo_gateway_clear_pairing(void);

void korvo_gateway_get_status(korvo_gateway_status_t *status);
const char *korvo_gateway_state_name(korvo_gateway_state_t state);
size_t korvo_gateway_status_json(char *out, size_t out_len);

/* Subscribe to Gateway events (reader.card, rex.*, door.*, call.*, sip.*,
 * bt.*, audio.*, etc.). Callbacks run in the WebSocket event task and must
 * return quickly. Up to four listeners are supported. */
esp_err_t korvo_gateway_add_listener(korvo_gateway_event_listener_t listener, void *ctx);

/* Command surface used later by the multimethod authentication engine. */
esp_err_t korvo_gateway_request_status(void);
esp_err_t korvo_gateway_door_unlock(uint32_t ms);
esp_err_t korvo_gateway_door_lock(void);
esp_err_t korvo_gateway_beacon_on(void);
esp_err_t korvo_gateway_beacon_off(void);
esp_err_t korvo_gateway_beacon_flash(uint32_t on_ms, uint32_t off_ms, uint32_t cycles);
esp_err_t korvo_gateway_wiegand_tx(uint8_t format, uint32_t facility, uint32_t card);
esp_err_t korvo_gateway_sip_call(void);
esp_err_t korvo_gateway_sip_answer(void);
esp_err_t korvo_gateway_sip_reject(void);
esp_err_t korvo_gateway_sip_hangup(void);

#ifdef __cplusplus
}
#endif
