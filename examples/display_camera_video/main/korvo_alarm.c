#include "korvo_alarm.h"
#include "korvo_audio_bridge.h"
#include "korvo_bluetooth.h"
#include "korvo_storage.h"

#include <stdio.h>

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "korvo_alarm";
static const char *NVS_NS = "korvo_alarm";
static const char *NVS_KEY = "selected";

#define SAMPLE_RATE_HZ 8000U
#define CHUNK_SAMPLES  160U

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static korvo_alarm_status_t s_status = {
    .selected = KORVO_ALARM_TIMBRE,
};

static void lock(void) { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

static bool valid_type(uint8_t type)
{
    return type >= KORVO_ALARM_TIMBRE && type <= KORVO_ALARM_CRITICA;
}

static void set_error(const char *text)
{
    lock();
    if (!text) text = "";
    size_t n = strlen(text);
    if (n >= sizeof(s_status.last_error)) n = sizeof(s_status.last_error) - 1;
    if (n) memcpy(s_status.last_error, text, n);
    s_status.last_error[n] = '\0';
    unlock();
}

static bool has_error(void)
{
    lock();
    bool yes = s_status.last_error[0] != '\0';
    unlock();
    return yes;
}

const char *korvo_alarm_name(uint8_t type)
{
    switch (type) {
        case KORVO_ALARM_TIMBRE: return "TIMBRE";
        case KORVO_ALARM_DOBLE_BEEP: return "DOBLE BEEP";
        case KORVO_ALARM_ASCENDENTE: return "ASCENDENTE";
        case KORVO_ALARM_SIRENA: return "INCENDIO";
        case KORVO_ALARM_CRITICA: return "CRITICA";
        default: return "DESCONOCIDA";
    }
}

static esp_err_t persist_selected(uint8_t type)
{
    nvs_handle_t nvs;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &nvs);
    if (e != ESP_OK) return e;
    e = nvs_set_u8(nvs, NVS_KEY, type);
    if (e == ESP_OK) e = nvs_commit(nvs);
    nvs_close(nvs);
    return e;
}

esp_err_t korvo_alarm_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    uint8_t selected = KORVO_ALARM_TIMBRE;
    nvs_handle_t nvs;
    esp_err_t e = nvs_open(NVS_NS, NVS_READONLY, &nvs);
    if (e == ESP_OK) {
        if (nvs_get_u8(nvs, NVS_KEY, &selected) != ESP_OK || !valid_type(selected)) {
            selected = KORVO_ALARM_TIMBRE;
        }
        nvs_close(nvs);
    } else if (e != ESP_ERR_NVS_NOT_FOUND) {
        return e;
    }

    lock();
    s_status.selected = selected;
    s_status.playing = false;
    s_status.last_error[0] = '\0';
    unlock();
    return korvo_audio_bridge_init();
}

esp_err_t korvo_alarm_set_selected(uint8_t type)
{
    if (!valid_type(type)) return ESP_ERR_INVALID_ARG;
    lock(); s_status.selected = type; unlock();
    return persist_selected(type);
}

uint8_t korvo_alarm_get_selected(void)
{
    lock(); uint8_t type = s_status.selected; unlock();
    return type;
}

void korvo_alarm_get_status(korvo_alarm_status_t *status)
{
    if (!status) return;
    lock(); *status = s_status; unlock();
}

static esp_err_t emit_pcm_chunk(const int16_t *pcm, size_t samples)
{
    size_t sent = korvo_audio_alarm_write(pcm, samples, 250);
    if (sent != samples) return ESP_ERR_TIMEOUT;
    korvo_bluetooth_audio_kick();
    vTaskDelay(pdMS_TO_TICKS(18));
    return ESP_OK;
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static const char *alarm_wav_file(uint8_t type)
{
    switch (type) {
        case KORVO_ALARM_TIMBRE: return "01_timbre.wav";
        case KORVO_ALARM_DOBLE_BEEP: return "02_doble_beep.wav";
        case KORVO_ALARM_ASCENDENTE: return "03_ascendente.wav";
        case KORVO_ALARM_SIRENA: return "04_incendio.wav";
        case KORVO_ALARM_CRITICA: return "05_critica.wav";
        default: return NULL;
    }
}

/* Stream WAV directly from SD.  We deliberately accept only the exact format
 * used by the HFP/CVSD lab path: PCM, mono, 8000 Hz, 16-bit.  No resampler and
 * no decoder are added to KORVO; replacing an alarm is just replacing the WAV
 * on the SD card with another file in the same format. */
static esp_err_t play_wav(uint8_t type)
{
    const char *file = alarm_wav_file(type);
    const char *root = korvo_storage_root();
    if (!file || !root || !korvo_storage_is_ready()) return ESP_ERR_INVALID_STATE;

    char path[192];
    int pn = snprintf(path, sizeof(path), "%s/audio/alarms/%s", root, file);
    if (pn <= 0 || (size_t)pn >= sizeof(path)) return ESP_ERR_INVALID_SIZE;

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "WAV not found: %s", path);
        set_error("WAV alarma no encontrado en SD");
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t hdr[12];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
        memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        fclose(f);
        set_error("WAV invalido: falta RIFF/WAVE");
        return ESP_ERR_INVALID_RESPONSE;
    }

    bool fmt_ok = false;
    uint32_t data_size = 0;
    long data_pos = -1;

    for (;;) {
        uint8_t ch[8];
        if (fread(ch, 1, sizeof(ch), f) != sizeof(ch)) break;
        uint32_t sz = le32(ch + 4);

        if (memcmp(ch, "fmt ", 4) == 0) {
            if (sz < 16 || sz > 1024) {
                fclose(f);
                set_error("WAV fmt invalido");
                return ESP_ERR_INVALID_RESPONSE;
            }
            uint8_t fmt[16];
            if (fread(fmt, 1, sizeof(fmt), f) != sizeof(fmt)) {
                fclose(f);
                return ESP_ERR_INVALID_RESPONSE;
            }
            uint16_t audio_fmt = le16(fmt + 0);
            uint16_t channels = le16(fmt + 2);
            uint32_t rate = le32(fmt + 4);
            uint16_t bits = le16(fmt + 14);
            fmt_ok = audio_fmt == 1 && channels == 1 && rate == SAMPLE_RATE_HZ && bits == 16;

            long remain = (long)sz - 16L;
            if (remain > 0 && fseek(f, remain, SEEK_CUR) != 0) {
                fclose(f);
                return ESP_ERR_INVALID_RESPONSE;
            }
            if (sz & 1U) (void)fseek(f, 1, SEEK_CUR);
        } else if (memcmp(ch, "data", 4) == 0) {
            data_pos = ftell(f);
            data_size = sz;
            break;
        } else {
            if (fseek(f, (long)sz + (long)(sz & 1U), SEEK_CUR) != 0) break;
        }
    }

    if (!fmt_ok || data_pos < 0 || data_size < 2) {
        fclose(f);
        set_error("WAV debe ser PCM mono 8000 Hz 16-bit");
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (fseek(f, data_pos, SEEK_SET) != 0) {
        fclose(f);
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_LOGI(TAG, "playing WAV %s (%lu bytes)", path, (unsigned long)data_size);
    int16_t pcm[CHUNK_SAMPLES];
    uint32_t remaining = data_size & ~1U;
    esp_err_t result = ESP_OK;

    while (remaining) {
        size_t want = remaining > sizeof(pcm) ? sizeof(pcm) : (size_t)remaining;
        size_t got = fread(pcm, 1, want, f);
        if (got == 0) {
            result = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        got &= ~(size_t)1U;
        result = emit_pcm_chunk(pcm, got / sizeof(int16_t));
        if (result != ESP_OK) break;
        remaining -= (uint32_t)got;
    }

    fclose(f);
    return result;
}

static void alarm_task(void *arg)
{
    uint8_t type = (uint8_t)(uintptr_t)arg;
    bool opened_audio = false;
    esp_err_t result = ESP_OK;

    korvo_bluetooth_status_t bt = {0};
    korvo_bluetooth_get_status(&bt);
    if (!bt.slc_connected) {
        result = ESP_ERR_INVALID_STATE;
        set_error("HF Bluetooth no conectado");
        goto done;
    }

    if (!bt.audio_connected) {
        result = korvo_bluetooth_audio_connect();
        if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
            set_error("No se pudo abrir audio HFP");
            goto done;
        }
        opened_audio = result == ESP_OK;

        bool ready = false;
        for (int i = 0; i < 50; ++i) {
            vTaskDelay(pdMS_TO_TICKS(50));
            korvo_bluetooth_get_status(&bt);
            if (bt.audio_connected) { ready = true; break; }
        }
        if (!ready) {
            result = ESP_ERR_TIMEOUT;
            set_error("Timeout abriendo audio HFP");
            goto done;
        }
    }

    korvo_audio_alarm_reset();
    result = play_wav(type);
    if (result != ESP_OK) {
        if (!has_error()) set_error("No se pudo reproducir WAV de alarma");
        goto done;
    }

    vTaskDelay(pdMS_TO_TICKS(250));
    set_error("");

done:
    if (opened_audio) {
        korvo_audio_bridge_status_t bridge = {0};
        korvo_audio_bridge_get_status(&bridge);
        if (!bridge.running) (void)korvo_bluetooth_audio_disconnect();
    }

    lock();
    s_status.playing = false;
    s_task = NULL;
    unlock();
    ESP_LOGI(TAG, "alarm %u (%s) finished: %s", (unsigned)type,
             korvo_alarm_name(type), esp_err_to_name(result));
    vTaskDelete(NULL);
}

esp_err_t korvo_alarm_test(uint8_t type)
{
    if (!valid_type(type)) return ESP_ERR_INVALID_ARG;
    esp_err_t e = korvo_alarm_set_selected(type);
    if (e != ESP_OK) return e;

    korvo_bluetooth_status_t bt = {0};
    korvo_bluetooth_get_status(&bt);
    if (!bt.enabled || !bt.engine_ready || !bt.slc_connected) {
        set_error("Conecte primero un dispositivo HFP HF");
        return ESP_ERR_INVALID_STATE;
    }

    lock();
    if (s_status.playing || s_task) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_status.playing = true;
    s_status.last_error[0] = '\0';
    unlock();

    if (xTaskCreate(alarm_task, "korvo_alarm", 4096, (void *)(uintptr_t)type, 6, &s_task) != pdPASS) {
        lock(); s_status.playing = false; s_task = NULL; unlock();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
