#include "korvo_audio_bridge.h"
#include "korvo_bluetooth.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

static const char *TAG = "korvo_audio";

#define PCM_STREAM_BYTES 16384
#define RTP_PCM_SAMPLES  160
#define RTP_PCM_BYTES    (RTP_PCM_SAMPLES * 2)
#define RTP_PACKET_MAX   512

typedef struct __attribute__((packed)) {
    uint8_t vpxcc;
    uint8_t mpt;
    uint16_t seq;
    uint32_t timestamp;
    uint32_t ssrc;
} rtp_header_t;

static SemaphoreHandle_t s_lock;
static StreamBufferHandle_t s_mic_pcm;
static StreamBufferHandle_t s_speaker_pcm;
static StreamBufferHandle_t s_alarm_pcm;
static TaskHandle_t s_task;
static volatile bool s_running;
static int s_socket = -1;
static struct sockaddr_in s_remote;
static korvo_audio_bridge_status_t s_status;
static uint16_t s_seq;
static uint32_t s_timestamp;
static uint32_t s_ssrc;

static void lock(void) { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

static int search_seg(int val, const int *table, int size)
{
    for (int i = 0; i < size; ++i) if (val <= table[i]) return i;
    return size;
}

static uint8_t linear_to_ulaw(int16_t pcm_val)
{
    static const int seg_uend[8] = {0x3F,0x7F,0xFF,0x1FF,0x3FF,0x7FF,0xFFF,0x1FFF};
    int mask;
    int seg;
    uint8_t uval;
    int pcm = pcm_val;
    pcm >>= 2;
    if (pcm < 0) { pcm = -pcm; mask = 0x7F; }
    else { mask = 0xFF; }
    if (pcm > 8159) pcm = 8159;
    pcm += 33;
    seg = search_seg(pcm, seg_uend, 8);
    if (seg >= 8) return (uint8_t)(0x7F ^ mask);
    uval = (uint8_t)((seg << 4) | ((pcm >> (seg + 1)) & 0xF));
    return (uint8_t)(uval ^ mask);
}

static int16_t ulaw_to_linear(uint8_t u_val)
{
    u_val = (uint8_t)~u_val;
    int t = ((u_val & 0x0F) << 3) + 0x84;
    t <<= ((unsigned)u_val & 0x70) >> 4;
    return (int16_t)((u_val & 0x80) ? (0x84 - t) : (t - 0x84));
}

static uint8_t linear_to_alaw(int16_t pcm_val)
{
    static const int seg_aend[8] = {0x1F,0x3F,0x7F,0xFF,0x1FF,0x3FF,0x7FF,0xFFF};
    int pcm = pcm_val;
    int mask;
    int seg;
    uint8_t aval;

    pcm >>= 3;
    if (pcm >= 0) mask = 0xD5;
    else { mask = 0x55; pcm = -pcm - 1; }
    seg = search_seg(pcm, seg_aend, 8);
    if (seg >= 8) return (uint8_t)(0x7F ^ mask);
    aval = (uint8_t)(seg << 4);
    if (seg < 2) aval |= (pcm >> 1) & 0x0F;
    else aval |= (pcm >> seg) & 0x0F;
    return (uint8_t)(aval ^ mask);
}

static int16_t alaw_to_linear(uint8_t a_val)
{
    a_val ^= 0x55;
    int t = (a_val & 0x0F) << 4;
    int seg = ((unsigned)a_val & 0x70) >> 4;
    switch (seg) {
        case 0: t += 8; break;
        case 1: t += 0x108; break;
        default: t += 0x108; t <<= seg - 1; break;
    }
    return (int16_t)((a_val & 0x80) ? t : -t);
}

static size_t rtp_payload_offset(const uint8_t *pkt, size_t len)
{
    if (!pkt || len < 12 || (pkt[0] >> 6) != 2) return 0;
    size_t off = 12U + (size_t)(pkt[0] & 0x0F) * 4U;
    if (off > len) return 0;
    if (pkt[0] & 0x10) {
        if (off + 4 > len) return 0;
        uint16_t words = (uint16_t)((pkt[off + 2] << 8) | pkt[off + 3]);
        off += 4U + (size_t)words * 4U;
        if (off > len) return 0;
    }
    return off;
}

static void process_rtp_packet(const uint8_t *pkt, size_t len)
{
    size_t off = rtp_payload_offset(pkt, len);
    if (!off || off >= len) return;
    int pt = pkt[1] & 0x7F;

    lock();
    int expected_pt = s_status.payload_type;
    unlock();
    if (pt != expected_pt || (pt != 0 && pt != 8)) return;

    size_t payload_len = len - off;
    if (pkt[0] & 0x20) {
        uint8_t pad = pkt[len - 1];
        if (pad > payload_len) return;
        payload_len -= pad;
    }
    if (payload_len > 320) payload_len = 320;

    int16_t pcm[320];
    for (size_t i = 0; i < payload_len; ++i) {
        pcm[i] = (pt == 8) ? alaw_to_linear(pkt[off + i]) : ulaw_to_linear(pkt[off + i]);
    }

    size_t bytes = payload_len * sizeof(int16_t);
    size_t sent = xStreamBufferSend(s_speaker_pcm, pcm, bytes, 0);
    lock();
    s_status.rx_packets++;
    if (sent != bytes) s_status.rx_dropped++;
    unlock();
    if (sent) korvo_bluetooth_audio_kick();
}

static void send_mic_rtp(void)
{
    if (xStreamBufferBytesAvailable(s_mic_pcm) < RTP_PCM_BYTES) return;

    int16_t pcm[RTP_PCM_SAMPLES];
    if (xStreamBufferReceive(s_mic_pcm, pcm, sizeof(pcm), 0) != sizeof(pcm)) return;

    /* Installation microphone gain: apply once, before G.711 encoding. */
    uint8_t mic_gain = korvo_bluetooth_get_mic_gain();
    if (mic_gain < 100) {
        for (size_t i = 0; i < RTP_PCM_SAMPLES; ++i) {
            int32_t scaled = ((int32_t)pcm[i] * (int32_t)mic_gain) / 100;
            pcm[i] = (int16_t)scaled;
        }
    }

    uint8_t packet[12 + RTP_PCM_SAMPLES];
    rtp_header_t *h = (rtp_header_t *)packet;
    h->vpxcc = 0x80;
    lock();
    int pt = s_status.payload_type;
    struct sockaddr_in remote = s_remote;
    int sock = s_socket;
    unlock();
    h->mpt = (uint8_t)(pt & 0x7F);
    h->seq = htons(s_seq++);
    h->timestamp = htonl(s_timestamp);
    h->ssrc = htonl(s_ssrc);
    s_timestamp += RTP_PCM_SAMPLES;

    for (size_t i = 0; i < RTP_PCM_SAMPLES; ++i) {
        packet[12 + i] = (pt == 8) ? linear_to_alaw(pcm[i]) : linear_to_ulaw(pcm[i]);
    }

    if (sock >= 0) {
        int n = sendto(sock, packet, sizeof(packet), 0,
                       (struct sockaddr *)&remote, sizeof(remote));
        if (n == (int)sizeof(packet)) {
            lock(); s_status.tx_packets++; unlock();
        }
    }
}

static void rtp_task(void *arg)
{
    (void)arg;
    uint8_t packet[RTP_PACKET_MAX];

    while (s_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        int sock;
        lock(); sock = s_socket; unlock();
        if (sock < 0) break;
        FD_SET(sock, &rfds);
        struct timeval tv = {.tv_sec = 0, .tv_usec = 10000};
        int rc = select(sock + 1, &rfds, NULL, NULL, &tv);
        if (rc > 0 && FD_ISSET(sock, &rfds)) {
            struct sockaddr_in src = {0};
            socklen_t sl = sizeof(src);
            int n = recvfrom(sock, packet, sizeof(packet), 0,
                             (struct sockaddr *)&src, &sl);
            if (n > 0) process_rtp_packet(packet, (size_t)n);
        }
        send_mic_rtp();
    }

    lock();
    if (s_socket >= 0) {
        close(s_socket);
        s_socket = -1;
    }
    s_status.running = false;
    s_task = NULL;
    unlock();
    vTaskDelete(NULL);
}

esp_err_t korvo_audio_bridge_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_mic_pcm) s_mic_pcm = xStreamBufferCreate(PCM_STREAM_BYTES, 1);
    if (!s_speaker_pcm) s_speaker_pcm = xStreamBufferCreate(PCM_STREAM_BYTES, 1);
    if (!s_alarm_pcm) s_alarm_pcm = xStreamBufferCreate(PCM_STREAM_BYTES, 1);
    if (!s_lock || !s_mic_pcm || !s_speaker_pcm || !s_alarm_pcm) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

esp_err_t korvo_audio_bridge_start(const char *remote_ip,
                                   uint16_t remote_port,
                                   uint16_t local_port,
                                   int payload_type)
{
    if (!remote_ip || !remote_ip[0] || !remote_port || !local_port ||
        (payload_type != 0 && payload_type != 8)) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(korvo_audio_bridge_init(), TAG, "init audio bridge");

    korvo_audio_bridge_stop();
    for (int i = 0; i < 20 && s_task; ++i) vTaskDelay(pdMS_TO_TICKS(10));
    if (s_task) return ESP_ERR_INVALID_STATE;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return ESP_FAIL;
    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    local.sin_port = htons(local_port);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
        close(sock);
        return ESP_FAIL;
    }

    struct sockaddr_in remote = {0};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(remote_port);
    if (inet_pton(AF_INET, remote_ip, &remote.sin_addr) != 1) {
        close(sock);
        return ESP_ERR_INVALID_ARG;
    }

    xStreamBufferReset(s_mic_pcm);
    xStreamBufferReset(s_speaker_pcm);

    lock();
    memset(&s_status, 0, sizeof(s_status));
    s_socket = sock;
    s_remote = remote;
    s_status.running = true;
    s_status.local_port = local_port;
    s_status.remote_port = remote_port;
    s_status.payload_type = payload_type;
    s_seq = (uint16_t)esp_random();
    s_timestamp = esp_random();
    s_ssrc = esp_random();
    s_running = true;
    unlock();

    if (xTaskCreate(rtp_task, "korvo_rtp", 6144, NULL, 7, &s_task) != pdPASS) {
        lock();
        s_running = false;
        s_status.running = false;
        close(s_socket);
        s_socket = -1;
        unlock();
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "RTP bridge %s:%u PT=%d local=%u",
             remote_ip, (unsigned)remote_port, payload_type, (unsigned)local_port);
    return ESP_OK;
}

void korvo_audio_bridge_stop(void)
{
    s_running = false;
}

void korvo_audio_bridge_get_status(korvo_audio_bridge_status_t *status)
{
    if (!status) return;
    lock(); *status = s_status; unlock();
}

void korvo_audio_alarm_reset(void)
{
    if (s_alarm_pcm) xStreamBufferReset(s_alarm_pcm);
}

size_t korvo_audio_alarm_write(const int16_t *pcm, size_t samples, uint32_t timeout_ms)
{
    if (!pcm || !samples || !s_alarm_pcm) return 0;
    size_t bytes = samples * sizeof(int16_t);
    size_t sent = xStreamBufferSend(s_alarm_pcm, pcm, bytes, pdMS_TO_TICKS(timeout_ms));
    return sent / sizeof(int16_t);
}

void korvo_audio_hfp_mic_rx(const uint8_t *buf, uint32_t len)
{
    if (!buf || !len || !s_running || !s_mic_pcm) return;
    size_t n = xStreamBufferSend(s_mic_pcm, buf, len, 0);
    if (n != len) {
        lock(); s_status.mic_dropped += (uint32_t)(len - n); unlock();
    }
}

static void apply_output_gain(uint8_t *buf, size_t bytes)
{
    if (!buf || bytes < 2) return;
    uint8_t volume = korvo_bluetooth_get_output_volume();
    if (volume >= 100) return;
    if (volume == 0) {
        memset(buf, 0, bytes);
        return;
    }

    size_t samples = bytes / sizeof(int16_t);
    int16_t *pcm = (int16_t *)buf;
    for (size_t i = 0; i < samples; ++i) {
        int32_t scaled = ((int32_t)pcm[i] * (int32_t)volume) / 100;
        pcm[i] = (int16_t)scaled;
    }
}

uint32_t korvo_audio_hfp_speaker_tx(uint8_t *buf, uint32_t len)
{
    if (!buf || !len) return 0;

    /* Web alarm/test audio has priority and works with SIP idle. */
    if (s_alarm_pcm && xStreamBufferBytesAvailable(s_alarm_pcm) > 0) {
        size_t n = xStreamBufferReceive(s_alarm_pcm, buf, len, 0);
        if (n) {
            apply_output_gain(buf, n);
            return (uint32_t)n;
        }
    }

    if (!s_running || !s_speaker_pcm) return 0;
    if (xStreamBufferBytesAvailable(s_speaker_pcm) < len) {
        lock(); s_status.sco_underruns++; unlock();
        return 0;
    }
    size_t n = xStreamBufferReceive(s_speaker_pcm, buf, len, 0);
    if (n) apply_output_gain(buf, n);
    return (uint32_t)n;
}
