#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool running;
    uint16_t local_port;
    uint16_t remote_port;
    int payload_type;
    uint32_t rx_packets;
    uint32_t tx_packets;
    uint32_t rx_dropped;
    uint32_t mic_dropped;
    uint32_t sco_underruns;
} korvo_audio_bridge_status_t;

esp_err_t korvo_audio_bridge_init(void);
esp_err_t korvo_audio_bridge_start(const char *remote_ip,
                                   uint16_t remote_port,
                                   uint16_t local_port,
                                   int payload_type);
void korvo_audio_bridge_stop(void);
void korvo_audio_bridge_get_status(korvo_audio_bridge_status_t *status);

/* Alarm/test PCM path. Alarm audio has priority over RTP and can be used
 * even when no SIP call is active. Samples are signed 16-bit LE at 8 kHz. */
void korvo_audio_alarm_reset(void);
size_t korvo_audio_alarm_write(const int16_t *pcm, size_t samples, uint32_t timeout_ms);

/* ESP-IDF HFP AG HCI PCM callbacks. Narrow-band CVSD is used in V1.3 lab,
 * therefore both sides are 8 kHz / signed 16-bit little-endian PCM. */
void korvo_audio_hfp_mic_rx(const uint8_t *buf, uint32_t len);
uint32_t korvo_audio_hfp_speaker_tx(uint8_t *buf, uint32_t len);

#ifdef __cplusplus
}
#endif
