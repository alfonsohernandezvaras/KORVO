#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KORVO_ALARM_TIMBRE = 1,
    KORVO_ALARM_DOBLE_BEEP = 2,
    KORVO_ALARM_ASCENDENTE = 3,
    KORVO_ALARM_SIRENA = 4,
    KORVO_ALARM_CRITICA = 5,
} korvo_alarm_type_t;

typedef struct {
    uint8_t selected;
    bool playing;
    char last_error[64];
} korvo_alarm_status_t;

esp_err_t korvo_alarm_init(void);
esp_err_t korvo_alarm_set_selected(uint8_t type);
uint8_t korvo_alarm_get_selected(void);
const char *korvo_alarm_name(uint8_t type);
void korvo_alarm_get_status(korvo_alarm_status_t *status);
esp_err_t korvo_alarm_test(uint8_t type);

#ifdef __cplusplus
}
#endif
