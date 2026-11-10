#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KORVO_SIP_HOST_MAX       64
#define KORVO_SIP_USER_MAX       32
#define KORVO_SIP_PASS_MAX       64
#define KORVO_SIP_NAME_MAX       48
#define KORVO_SIP_NUMBER_MAX     32
#define KORVO_SIP_URI_MAX        128
#define KORVO_SIP_ERROR_MAX      96

typedef enum {
    KORVO_SIP_DISABLED = 0,
    KORVO_SIP_OFFLINE,
    KORVO_SIP_REGISTERING,
    KORVO_SIP_IDLE,
    KORVO_SIP_CALLING,
    KORVO_SIP_RINGING,
    KORVO_SIP_INCOMING,
    KORVO_SIP_CONNECTING,
    KORVO_SIP_CONNECTED,
    KORVO_SIP_BUSY,
    KORVO_SIP_NO_ANSWER,
    KORVO_SIP_REJECTED,
    KORVO_SIP_FAILED,
    KORVO_SIP_ENDED,
} korvo_sip_state_t;

typedef struct {
    bool enabled;
    char server[KORVO_SIP_HOST_MAX];
    uint16_t server_port;
    uint16_t local_port;
    uint16_t rtp_port;
    uint16_t register_expires;
    char extension[KORVO_SIP_NUMBER_MAX];
    char username[KORVO_SIP_USER_MAX];
    char password[KORVO_SIP_PASS_MAX];
    char display_name[KORVO_SIP_NAME_MAX];
    char operator_extension[KORVO_SIP_NUMBER_MAX];
} korvo_sip_config_t;

typedef struct {
    bool engine_ready;
    bool enabled;
    bool network_ready;
    bool registered;
    korvo_sip_state_t state;
    int last_code;
    char remote_uri[KORVO_SIP_URI_MAX];
    char remote_number[KORVO_SIP_NUMBER_MAX];
    char negotiated_codec[16];
    char last_error[KORVO_SIP_ERROR_MAX];
} korvo_sip_status_t;

typedef void (*korvo_sip_listener_t)(const korvo_sip_status_t *status, void *ctx);

esp_err_t korvo_sip_init(void);
esp_err_t korvo_sip_get_config(korvo_sip_config_t *cfg);
esp_err_t korvo_sip_save_config(const korvo_sip_config_t *cfg);
void korvo_sip_get_status(korvo_sip_status_t *status);
const char *korvo_sip_state_name(korvo_sip_state_t state);
size_t korvo_sip_status_json(char *out, size_t out_len);
esp_err_t korvo_sip_add_listener(korvo_sip_listener_t listener, void *ctx);

esp_err_t korvo_sip_call_default(void);
esp_err_t korvo_sip_call(const char *number_or_uri);
esp_err_t korvo_sip_answer(void);
esp_err_t korvo_sip_reject(void);
esp_err_t korvo_sip_hangup(void);
esp_err_t korvo_sip_force_register(void);

#ifdef __cplusplus
}
#endif
