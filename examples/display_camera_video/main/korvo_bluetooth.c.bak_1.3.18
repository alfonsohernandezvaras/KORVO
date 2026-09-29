#include "korvo_bluetooth.h"
#include "korvo_audio_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <inttypes.h>

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_hf_ag_api.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "korvo_bt";
static const char *NVS_NS = "korvo_bt";
static const char *NVS_KEY = "cfg";
static const char *NVS_VOL_KEY = "volume";
static const char *NVS_MIC_KEY = "mic_gain";

static SemaphoreHandle_t s_lock;
static korvo_bluetooth_config_t s_cfg;
static korvo_bluetooth_status_t s_status;
static bool s_stack_started;
static bool s_profile_ready;
static int64_t s_next_reconnect_us;
static int64_t s_connecting_since_us;
static bool s_manual_disconnect;
static esp_bd_addr_t s_peer_bda;
static bool s_have_peer_bda;
static korvo_bluetooth_event_listener_t s_listener;
static void *s_listener_ctx;
static esp_hf_call_status_t s_call_state = ESP_HF_CALL_STATUS_NO_CALLS;
static esp_hf_call_setup_status_t s_call_setup = ESP_HF_CALL_SETUP_STATUS_IDLE;
static char s_call_number[KORVO_BT_NUMBER_MAX];
static bool s_call_incoming;
static uint8_t s_output_volume = 75;
static uint8_t s_mic_gain = 75;

#define BT_RECONNECT_INTERVAL_US  (5LL * 1000LL * 1000LL)
#define BT_RECONNECT_AFTER_AUTH_US (1000LL * 1000LL)
#define BT_CONNECT_STALE_US       (15LL * 1000LL * 1000LL)

static void lock(void) { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

static void copy_text(char *dst, size_t len, const char *src)
{
    if (!dst || !len) return;
    if (!src) { dst[0] = 0; return; }
    size_t n = strlen(src);
    if (n >= len) n = len - 1;
    if (n) memcpy(dst, src, n);
    dst[n] = '\0';
}

static void set_error(const char *text)
{
    lock(); copy_text(s_status.last_error, sizeof(s_status.last_error), text); unlock();
}

static void mac_to_text(const uint8_t bda[6], char out[KORVO_BT_MAC_MAX])
{
    snprintf(out, KORVO_BT_MAC_MAX, "%02X:%02X:%02X:%02X:%02X:%02X",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

static bool text_to_mac(const char *s, esp_bd_addr_t out)
{
    if (!s || strlen(s) != 17) return false;
    unsigned v[6];
    if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
    for (int i = 0; i < 6; ++i) out[i] = (uint8_t)v[i];
    return true;
}

static bool valid_pin(const char *pin)
{
    if (!pin || !pin[0] || strlen(pin) > 16) return false;
    for (const char *p = pin; *p; ++p) if (!isdigit((unsigned char)*p)) return false;
    return true;
}

static void defaults(korvo_bluetooth_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->enabled = false;
    cfg->auto_connect = true;
    copy_text(cfg->local_name, sizeof(cfg->local_name), "KORVO-001");
    copy_text(cfg->pin, sizeof(cfg->pin), "0000");
}

static esp_err_t load_cfg(void)
{
    defaults(&s_cfg);
    nvs_handle_t nvs;
    esp_err_t e = nvs_open(NVS_NS, NVS_READONLY, &nvs);
    if (e == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (e != ESP_OK) return e;
    size_t n = sizeof(s_cfg);
    e = nvs_get_blob(nvs, NVS_KEY, &s_cfg, &n);
    nvs_close(nvs);
    if (e == ESP_ERR_NVS_NOT_FOUND || n != sizeof(s_cfg)) {
        defaults(&s_cfg);
        return ESP_OK;
    }
    return e;
}

static esp_err_t persist_cfg(void)
{
    nvs_handle_t nvs;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &nvs);
    if (e != ESP_OK) return e;
    e = nvs_set_blob(nvs, NVS_KEY, &s_cfg, sizeof(s_cfg));
    if (e == ESP_OK) e = nvs_commit(nvs);
    nvs_close(nvs);
    return e;
}

static void load_output_volume(void)
{
    uint8_t value = 75;
    nvs_handle_t nvs;
    esp_err_t e = nvs_open(NVS_NS, NVS_READONLY, &nvs);
    if (e == ESP_OK) {
        uint8_t stored = 75;
        if (nvs_get_u8(nvs, NVS_VOL_KEY, &stored) == ESP_OK && stored <= 100) {
            value = stored;
        }
        nvs_close(nvs);
    }
    lock();
    s_output_volume = value;
    unlock();
}

static esp_err_t persist_output_volume(void)
{
    uint8_t value;
    lock();
    value = s_output_volume;
    unlock();

    nvs_handle_t nvs;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &nvs);
    if (e != ESP_OK) return e;
    e = nvs_set_u8(nvs, NVS_VOL_KEY, value);
    if (e == ESP_OK) e = nvs_commit(nvs);
    nvs_close(nvs);
    return e;
}

static void load_mic_gain(void)
{
    uint8_t value = 75;
    nvs_handle_t nvs;
    esp_err_t e = nvs_open(NVS_NS, NVS_READONLY, &nvs);
    if (e == ESP_OK) {
        uint8_t stored = 75;
        if (nvs_get_u8(nvs, NVS_MIC_KEY, &stored) == ESP_OK && stored <= 100) value = stored;
        nvs_close(nvs);
    }
    lock(); s_mic_gain = value; unlock();
}

static esp_err_t persist_mic_gain(void)
{
    uint8_t value;
    lock(); value = s_mic_gain; unlock();
    nvs_handle_t nvs;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &nvs);
    if (e != ESP_OK) return e;
    e = nvs_set_u8(nvs, NVS_MIC_KEY, value);
    if (e == ESP_OK) e = nvs_commit(nvs);
    nvs_close(nvs);
    return e;
}

static void emit_event(korvo_bluetooth_event_type_t type, const char *number)
{
    korvo_bluetooth_event_listener_t fn;
    void *ctx;
    lock(); fn = s_listener; ctx = s_listener_ctx; unlock();
    if (!fn) return;
    korvo_bluetooth_event_t ev = {.type = type};
    copy_text(ev.number, sizeof(ev.number), number);
    fn(&ev, ctx);
}

static void update_scan_device(const esp_bd_addr_t bda, const char *name, int rssi)
{
    char mac[KORVO_BT_MAC_MAX];
    mac_to_text(bda, mac);
    lock();
    size_t idx = s_status.device_count;
    for (size_t i = 0; i < s_status.device_count; ++i) {
        if (!strcasecmp(s_status.devices[i].mac, mac)) { idx = i; break; }
    }
    if (idx == s_status.device_count) {
        if (s_status.device_count >= KORVO_BT_SCAN_MAX) { unlock(); return; }
        s_status.device_count++;
    }
    copy_text(s_status.devices[idx].mac, sizeof(s_status.devices[idx].mac), mac);
    if (name && name[0]) copy_text(s_status.devices[idx].name, sizeof(s_status.devices[idx].name), name);
    s_status.devices[idx].rssi = rssi;
    unlock();
}

static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
        case ESP_BT_GAP_DISC_RES_EVT: {
            char name[KORVO_BT_NAME_MAX] = {0};
            int rssi = -127;
            for (int i = 0; i < param->disc_res.num_prop; ++i) {
                esp_bt_gap_dev_prop_t *p = &param->disc_res.prop[i];
                if (p->type == ESP_BT_GAP_DEV_PROP_BDNAME && p->val && p->len > 0) {
                    size_t n = (size_t)p->len < sizeof(name)-1 ? (size_t)p->len : sizeof(name)-1;
                    memcpy(name, p->val, n); name[n] = 0;
                } else if (p->type == ESP_BT_GAP_DEV_PROP_RSSI && p->val) {
                    rssi = *(int8_t *)p->val;
                } else if (p->type == ESP_BT_GAP_DEV_PROP_EIR && p->val && !name[0]) {
                    uint8_t n = 0;
                    uint8_t *q = esp_bt_gap_resolve_eir_data((uint8_t *)p->val, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &n);
                    if (!q) q = esp_bt_gap_resolve_eir_data((uint8_t *)p->val, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &n);
                    if (q && n) {
                        size_t z = n < sizeof(name)-1 ? n : sizeof(name)-1;
                        memcpy(name, q, z); name[z] = 0;
                    }
                }
            }
            update_scan_device(param->disc_res.bda, name[0] ? name : "--", rssi);
            break;
        }
        case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
            lock(); s_status.scanning = param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED; unlock();
            break;
        case ESP_BT_GAP_PIN_REQ_EVT: {
            esp_bt_pin_code_t pin_code = {0};
            char pin[KORVO_BT_PIN_MAX];
            lock(); copy_text(pin, sizeof(pin), s_cfg.pin); unlock();
            uint8_t n = (uint8_t)strlen(pin);
            if (!n) { copy_text(pin, sizeof(pin), "0000"); n = 4; }
            memcpy(pin_code, pin, n);
            esp_bt_gap_pin_reply(param->pin_req.bda, true, n, pin_code);
            break;
        }
        case ESP_BT_GAP_CFM_REQ_EVT:
            lock(); s_status.pairing_number = param->cfm_req.num_val; unlock();
            ESP_LOGI(TAG, "SSP numeric confirm %06lu -> accepted for lab",
                     (unsigned long)param->cfm_req.num_val);
            esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
            break;
        case ESP_BT_GAP_AUTH_CMPL_EVT:
            if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
                char mac[KORVO_BT_MAC_MAX];
                mac_to_text(param->auth_cmpl.bda, mac);
                lock();
                copy_text(s_status.peer_mac, sizeof(s_status.peer_mac), mac);
                copy_text(s_cfg.peer_mac, sizeof(s_cfg.peer_mac), mac);
                s_status.paired = true;
                if (param->auth_cmpl.device_name[0]) {
                    copy_text(s_status.peer_name, sizeof(s_status.peer_name), (const char *)param->auth_cmpl.device_name);
                    copy_text(s_cfg.peer_name, sizeof(s_cfg.peer_name), (const char *)param->auth_cmpl.device_name);
                }
                s_status.last_error[0] = 0;
                s_next_reconnect_us = esp_timer_get_time() + BT_RECONNECT_AFTER_AUTH_US;
                s_manual_disconnect = false;
                unlock();
                persist_cfg();
                ESP_LOGI(TAG, "Bluetooth authenticated/bonded with %s; HFP supervisor will establish SLC", mac);
            } else {
                lock();
                s_status.paired = s_cfg.peer_mac[0] != 0;
                copy_text(s_status.peer_mac, sizeof(s_status.peer_mac), s_cfg.peer_mac);
                copy_text(s_status.peer_name, sizeof(s_status.peer_name), s_cfg.peer_name);
                unlock();
                set_error("Bluetooth authentication failed");
            }
            break;
        default:
            break;
    }
}

static void hfp_cb(esp_hf_cb_event_t event, esp_hf_cb_param_t *param)
{
    switch (event) {
        case ESP_HF_PROF_STATE_EVT:
            if (param->prof_stat.state == ESP_HF_INIT_SUCCESS || param->prof_stat.state == ESP_HF_INIT_ALREADY) {
                lock(); s_profile_ready = true; s_status.engine_ready = true; s_status.last_error[0] = 0; unlock();
                ESP_LOGI(TAG, "HFP AG ready");
            } else if (param->prof_stat.state == ESP_HF_INIT_FAIL) {
                set_error("HFP AG init failed");
            }
            break;

        case ESP_HF_CONNECTION_STATE_EVT: {
            char mac[KORVO_BT_MAC_MAX];
            mac_to_text(param->conn_stat.remote_bda, mac);
            lock();
            memcpy(s_peer_bda, param->conn_stat.remote_bda, ESP_BD_ADDR_LEN);
            s_have_peer_bda = true;
            copy_text(s_status.peer_mac, sizeof(s_status.peer_mac), mac);
            s_status.slc_connected = param->conn_stat.state == ESP_HF_CONNECTION_STATE_SLC_CONNECTED;
            s_status.connecting = param->conn_stat.state == ESP_HF_CONNECTION_STATE_CONNECTING ||
                                  param->conn_stat.state == ESP_HF_CONNECTION_STATE_CONNECTED;
            if (s_status.connecting && s_connecting_since_us == 0) {
                s_connecting_since_us = esp_timer_get_time();
            }
            if (param->conn_stat.state == ESP_HF_CONNECTION_STATE_DISCONNECTED) {
                s_status.slc_connected = false;
                s_status.connecting = false;
                s_status.audio_connected = false;
                s_connecting_since_us = 0;
                if (!s_manual_disconnect && s_cfg.auto_connect && s_cfg.peer_mac[0]) {
                    /* A failed/closed RFCOMM attempt must not be hammered every
                       few hundred ms. Keep monitoring continuously, but retry
                       the real HFP SLC at a controlled 5 s cadence. */
                    s_next_reconnect_us = esp_timer_get_time() + BT_RECONNECT_INTERVAL_US;
                }
                if (!s_status.paired) {
                    s_status.paired = s_cfg.peer_mac[0] != 0;
                    copy_text(s_status.peer_mac, sizeof(s_status.peer_mac), s_cfg.peer_mac);
                    copy_text(s_status.peer_name, sizeof(s_status.peer_name), s_cfg.peer_name);
                }
            } else if (param->conn_stat.state == ESP_HF_CONNECTION_STATE_SLC_CONNECTED) {
                s_status.connecting = false;
                s_connecting_since_us = 0;
                s_next_reconnect_us = 0;
                s_status.last_error[0] = 0;
            }
            unlock();
            ESP_LOGI(TAG, "HFP link state=%d peer=%s peer_feat=0x%08" PRIx32 " chld_feat=0x%08" PRIx32,
                     (int)param->conn_stat.state, mac,
                     (uint32_t)param->conn_stat.peer_feat,
                     (uint32_t)param->conn_stat.chld_feat);
            if (param->conn_stat.state == ESP_HF_CONNECTION_STATE_SLC_CONNECTED) {
                lock();
                s_status.paired = true;
                copy_text(s_cfg.peer_mac, sizeof(s_cfg.peer_mac), mac);
                if (s_status.peer_name[0]) copy_text(s_cfg.peer_name, sizeof(s_cfg.peer_name), s_status.peer_name);
                unlock();
                persist_cfg();
                esp_hf_ag_bsir(param->conn_stat.remote_bda, ESP_HF_IN_BAND_RINGTONE_NOT_PROVIDED);
            }
            break;
        }

        case ESP_HF_AUDIO_STATE_EVT:
            lock();
            s_status.audio_connected = param->audio_stat.state == ESP_HF_AUDIO_STATE_CONNECTED ||
                                       param->audio_stat.state == ESP_HF_AUDIO_STATE_CONNECTED_MSBC;
            copy_text(s_status.codec, sizeof(s_status.codec),
                      param->audio_stat.state == ESP_HF_AUDIO_STATE_CONNECTED_MSBC ? "mSBC" :
                      (param->audio_stat.state == ESP_HF_AUDIO_STATE_CONNECTED ? "CVSD" : "--"));
            unlock();
#if CONFIG_BT_HFP_AUDIO_DATA_PATH_HCI
            if (param->audio_stat.state == ESP_HF_AUDIO_STATE_CONNECTED ||
                param->audio_stat.state == ESP_HF_AUDIO_STATE_CONNECTED_MSBC) {
                esp_hf_ag_register_data_callback(korvo_audio_hfp_mic_rx, korvo_audio_hfp_speaker_tx);
            }
#endif
            break;

        case ESP_HF_VOLUME_CONTROL_EVT:
            ESP_LOGI(TAG, "HF volume control type=%d volume=%d",
                     (int)param->volume_control.type, param->volume_control.volume);
            break;

        case ESP_HF_CIND_RESPONSE_EVT:
            esp_hf_ag_cind_response(param->cind_rep.remote_addr,
                                    s_call_state, s_call_setup,
                                    ESP_HF_NETWORK_STATE_AVAILABLE, 5,
                                    ESP_HF_ROAMING_STATUS_INACTIVE, 5,
                                    ESP_HF_CALL_HELD_STATUS_NONE);
            break;
        case ESP_HF_UNAT_RESPONSE_EVT: {
            const char *cmd = param->unat_rep.unat ? param->unat_rep.unat : "(null)";
            ESP_LOGW(TAG, "HF unknown AT -> ERROR response: %s", cmd);
            /* The HF may send vendor-specific AT commands even while idle.
               The official ESP-IDF HFP AG example always terminates an unknown
               command with a response; leaving it unanswered can make some HF
               devices time out and tear down the SLC. */
            esp_err_t e = esp_hf_ag_unknown_at_send(param->unat_rep.remote_addr, NULL);
            if (e != ESP_OK) {
                ESP_LOGW(TAG, "HF unknown AT response failed: %s", esp_err_to_name(e));
            }
            break;
        }

        case ESP_HF_IND_UPDATE_EVT:
            esp_hf_ag_ciev_report(param->ind_upd.remote_addr, ESP_HF_IND_TYPE_CALL, s_call_state);
            esp_hf_ag_ciev_report(param->ind_upd.remote_addr, ESP_HF_IND_TYPE_CALLSETUP, s_call_setup);
            esp_hf_ag_ciev_report(param->ind_upd.remote_addr, ESP_HF_IND_TYPE_SERVICE, ESP_HF_NETWORK_STATE_AVAILABLE);
            esp_hf_ag_ciev_report(param->ind_upd.remote_addr, ESP_HF_IND_TYPE_SIGNAL, 5);
            esp_hf_ag_ciev_report(param->ind_upd.remote_addr, ESP_HF_IND_TYPE_BATTCHG, 5);
            break;
        case ESP_HF_COPS_RESPONSE_EVT:
            esp_hf_ag_cops_response(param->cops_rep.remote_addr, "KORVO");
            break;
        case ESP_HF_CNUM_RESPONSE_EVT:
            esp_hf_ag_cnum_response(param->cnum_rep.remote_addr, "KORVO", 129, ESP_HF_SUBSCRIBER_SERVICE_TYPE_VOICE);
            break;
        case ESP_HF_CLCC_RESPONSE_EVT: {
            esp_hf_current_call_status_t cs = ESP_HF_CURRENT_CALL_STATUS_ACTIVE;
            if (s_call_setup == ESP_HF_CALL_SETUP_STATUS_INCOMING) cs = ESP_HF_CURRENT_CALL_STATUS_INCOMING;
            else if (s_call_setup == ESP_HF_CALL_SETUP_STATUS_OUTGOING_DIALING) cs = ESP_HF_CURRENT_CALL_STATUS_DIALING;
            else if (s_call_setup == ESP_HF_CALL_SETUP_STATUS_OUTGOING_ALERTING) cs = ESP_HF_CURRENT_CALL_STATUS_ALERTING;
            if (s_call_state == ESP_HF_CALL_STATUS_CALL_IN_PROGRESS || s_call_setup != ESP_HF_CALL_SETUP_STATUS_IDLE) {
                esp_hf_ag_clcc_response(param->clcc_rep.remote_addr, 1,
                                        s_call_setup == ESP_HF_CALL_SETUP_STATUS_INCOMING ? ESP_HF_CURRENT_CALL_DIRECTION_INCOMING : ESP_HF_CURRENT_CALL_DIRECTION_OUTGOING,
                                        cs, ESP_HF_CURRENT_CALL_MODE_VOICE,
                                        ESP_HF_CURRENT_CALL_MPTY_TYPE_SINGLE,
                                        s_call_number[0] ? s_call_number : "--",
                                        ESP_HF_CALL_ADDR_TYPE_UNKNOWN);
            }
            esp_hf_ag_clcc_response(param->clcc_rep.remote_addr, 0,
                                    ESP_HF_CURRENT_CALL_DIRECTION_OUTGOING,
                                    ESP_HF_CURRENT_CALL_STATUS_ACTIVE,
                                    ESP_HF_CURRENT_CALL_MODE_VOICE,
                                    ESP_HF_CURRENT_CALL_MPTY_TYPE_SINGLE,
                                    NULL, ESP_HF_CALL_ADDR_TYPE_UNKNOWN);
            break;
        }
        case ESP_HF_NREC_RESPONSE_EVT:
            ESP_LOGI(TAG, "HF NREC request: %d", (int)param->nrec.state);
            break;
        case ESP_HF_ATA_RESPONSE_EVT:
            emit_event(KORVO_BT_EVENT_ANSWER_REQUEST, NULL);
            break;
        case ESP_HF_CHUP_RESPONSE_EVT:
            emit_event(KORVO_BT_EVENT_HANGUP_REQUEST, NULL);
            break;
        case ESP_HF_DIAL_EVT:
            if (param->out_call.num_or_loc) emit_event(KORVO_BT_EVENT_DIAL_REQUEST, param->out_call.num_or_loc);
            break;
        case ESP_HF_BCS_RESPONSE_EVT:
            lock(); copy_text(s_status.codec, sizeof(s_status.codec), param->bcs_rep.mode == ESP_HF_WBS_YES ? "mSBC" : "CVSD"); unlock();
            break;
        default:
            break;
    }
}

static esp_err_t start_stack(void)
{
    lock(); bool already = s_stack_started; unlock();
    if (already) return ESP_OK;

    esp_err_t e = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) ESP_LOGW(TAG, "BLE memory release: %s", esp_err_to_name(e));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    e = esp_bt_controller_init(&bt_cfg);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
    e = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;

    esp_bluedroid_config_t bcfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    e = esp_bluedroid_init_with_cfg(&bcfg);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
    e = esp_bluedroid_enable();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_bt_gap_register_callback(gap_cb));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_hf_ag_register_callback(hfp_cb));

    lock();
    char local_name[KORVO_BT_NAME_MAX];
    char pin[KORVO_BT_PIN_MAX];
    copy_text(local_name, sizeof(local_name), s_cfg.local_name);
    copy_text(pin, sizeof(pin), s_cfg.pin);
    unlock();

    esp_bt_gap_set_device_name(local_name);
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap));
    esp_bt_pin_code_t pin_code = {0};
    uint8_t pin_len = (uint8_t)strlen(pin);
    if (!pin_len) { memcpy(pin_code, "0000", 4); pin_len = 4; }
    else memcpy(pin_code, pin, pin_len);
    esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_VARIABLE, pin_len, pin_code);

    e = esp_hf_ag_init();
    if (e != ESP_OK) return e;
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

    lock();
    s_stack_started = true;
    s_status.enabled = true;
    copy_text(s_status.local_name, sizeof(s_status.local_name), local_name);
    unlock();
    return ESP_OK;
}

void korvo_bluetooth_supervise(void)
{
    int64_t now = esp_timer_get_time();
    bool enabled, ready, autoconnect, paired, connected, connecting, scanning, manual_disconnect;
    int64_t next_retry, connecting_since;
    char mac[KORVO_BT_MAC_MAX];

    lock();
    enabled = s_cfg.enabled;
    ready = s_profile_ready;
    autoconnect = s_cfg.auto_connect;
    paired = s_status.paired && s_cfg.peer_mac[0];
    connected = s_status.slc_connected;
    connecting = s_status.connecting;
    scanning = s_status.scanning;
    manual_disconnect = s_manual_disconnect;
    next_retry = s_next_reconnect_us;
    connecting_since = s_connecting_since_us;
    copy_text(mac, sizeof(mac), s_cfg.peer_mac);
    unlock();

    if (!enabled || !ready || !autoconnect || !paired || connected || scanning || manual_disconnect || !mac[0]) return;

    if (connecting) {
        if (connecting_since > 0 && (now - connecting_since) < BT_CONNECT_STALE_US) return;
        ESP_LOGW(TAG, "HFP connect attempt stale; supervisor will retry %s", mac);
        lock();
        s_status.connecting = false;
        s_connecting_since_us = 0;
        unlock();
    }

    if (next_retry > 0 && now < next_retry) return;

    lock();
    s_next_reconnect_us = now + BT_RECONNECT_INTERVAL_US;
    unlock();

    ESP_LOGI(TAG, "HFP supervisor -> SLC connect %s", mac);
    esp_err_t e = korvo_bluetooth_connect(mac);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        char msg[KORVO_BT_ERROR_MAX];
        snprintf(msg, sizeof(msg), "HFP reconnect: %s", esp_err_to_name(e));
        set_error(msg);
    }
}

esp_err_t korvo_bluetooth_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    ESP_RETURN_ON_ERROR(korvo_audio_bridge_init(), TAG, "audio bridge init");
    ESP_RETURN_ON_ERROR(load_cfg(), TAG, "load bt config");
    load_output_volume();
    load_mic_gain();

    lock();
    memset(&s_status, 0, sizeof(s_status));
    s_status.enabled = s_cfg.enabled;
    s_status.paired = s_cfg.peer_mac[0] != 0;
    copy_text(s_status.local_name, sizeof(s_status.local_name), s_cfg.local_name);
    copy_text(s_status.peer_mac, sizeof(s_status.peer_mac), s_cfg.peer_mac);
    copy_text(s_status.peer_name, sizeof(s_status.peer_name), s_cfg.peer_name);
    copy_text(s_status.codec, sizeof(s_status.codec), "--");
    unlock();

    if (!s_cfg.enabled) return ESP_OK;
    esp_err_t e = start_stack();
    if (e != ESP_OK) { set_error(esp_err_to_name(e)); return e; }
    lock();
    s_next_reconnect_us = 0;
    s_manual_disconnect = false;
    unlock();
    return ESP_OK;
}

esp_err_t korvo_bluetooth_get_config(korvo_bluetooth_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    lock(); *cfg = s_cfg; unlock();
    return ESP_OK;
}

esp_err_t korvo_bluetooth_save_config(const korvo_bluetooth_config_t *cfg)
{
    if (!cfg || !cfg->local_name[0] || !valid_pin(cfg->pin)) return ESP_ERR_INVALID_ARG;
    korvo_bluetooth_config_t c = *cfg;
    c.local_name[sizeof(c.local_name)-1] = 0;
    c.pin[sizeof(c.pin)-1] = 0;
    c.peer_mac[sizeof(c.peer_mac)-1] = 0;
    c.peer_name[sizeof(c.peer_name)-1] = 0;
    if (c.peer_mac[0]) { esp_bd_addr_t tmp; if (!text_to_mac(c.peer_mac, tmp)) return ESP_ERR_INVALID_ARG; }

    lock(); s_cfg = c; s_status.enabled = c.enabled; copy_text(s_status.local_name, sizeof(s_status.local_name), c.local_name); unlock();
    esp_err_t e = persist_cfg();
    if (e != ESP_OK) return e;

    if (c.enabled) {
        e = start_stack();
        if (e != ESP_OK) return e;
        esp_bt_gap_set_device_name(c.local_name);
        esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
        lock();
        s_manual_disconnect = false;
        s_next_reconnect_us = 0;
        unlock();
    } else if (s_stack_started) {
        if (s_status.scanning) esp_bt_gap_cancel_discovery();
        esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
        lock();
        s_status.scanning = false;
        s_status.connecting = false;
        s_manual_disconnect = true;
        s_connecting_since_us = 0;
        unlock();
        korvo_bluetooth_audio_disconnect();
        korvo_bluetooth_disconnect();
    }
    return ESP_OK;
}

void korvo_bluetooth_get_status(korvo_bluetooth_status_t *status)
{
    if (!status) return;
    lock(); *status = s_status; unlock();
}

uint8_t korvo_bluetooth_get_output_volume(void)
{
    uint8_t value;
    lock();
    value = s_output_volume;
    unlock();
    return value;
}

esp_err_t korvo_bluetooth_set_output_volume(uint8_t percent, bool persist)
{
    if (percent > 100) return ESP_ERR_INVALID_ARG;
    lock();
    s_output_volume = percent;
    unlock();
    return persist ? persist_output_volume() : ESP_OK;
}

uint8_t korvo_bluetooth_get_mic_gain(void)
{
    uint8_t value;
    lock(); value = s_mic_gain; unlock();
    return value;
}

esp_err_t korvo_bluetooth_set_mic_gain(uint8_t percent, bool persist)
{
    if (percent > 100) percent = 100;
    lock(); s_mic_gain = percent; unlock();
    return persist ? persist_mic_gain() : ESP_OK;
}

size_t korvo_bluetooth_status_json(char *out, size_t out_len)
{
    if (!out || !out_len) return 0;
    korvo_bluetooth_status_t s; korvo_bluetooth_get_status(&s);
    int n = snprintf(out, out_len,
        "{\"enabled\":%s,\"engine_ready\":%s,\"scanning\":%s,\"paired\":%s,"
        "\"connecting\":%s,\"slc_connected\":%s,\"audio_connected\":%s,"
        "\"local_name\":\"%s\",\"peer_mac\":\"%s\",\"peer_name\":\"%s\","
        "\"codec\":\"%s\",\"last_error\":\"%s\",\"device_count\":%u}",
        s.enabled?"true":"false", s.engine_ready?"true":"false", s.scanning?"true":"false", s.paired?"true":"false",
        s.connecting?"true":"false", s.slc_connected?"true":"false", s.audio_connected?"true":"false",
        s.local_name, s.peer_mac, s.peer_name, s.codec, s.last_error, (unsigned)s.device_count);
    if (n < 0) return 0;
    return (size_t)n < out_len ? (size_t)n : out_len - 1;
}

esp_err_t korvo_bluetooth_set_event_listener(korvo_bluetooth_event_listener_t listener, void *ctx)
{
    lock(); s_listener = listener; s_listener_ctx = ctx; unlock();
    return ESP_OK;
}

esp_err_t korvo_bluetooth_scan(void)
{
    if (!s_cfg.enabled) {
        set_error("Bluetooth HFP deshabilitado: habilite y guarde antes de SCAN");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t e = start_stack();
    if (e != ESP_OK) {
        set_error(esp_err_to_name(e));
        return e;
    }
    lock();
    s_status.device_count = 0;
    s_status.scanning = true;
    s_status.last_error[0] = 0;
    unlock();
    e = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 8, 0);
    if (e != ESP_OK) {
        lock(); s_status.scanning = false; unlock();
        set_error(esp_err_to_name(e));
    }
    return e;
}

esp_err_t korvo_bluetooth_connect(const char *mac)
{
    if (!s_cfg.enabled) return ESP_ERR_INVALID_STATE;
    ESP_RETURN_ON_ERROR(start_stack(), TAG, "start bt");
    esp_bd_addr_t bda;
    if (!text_to_mac(mac, bda)) return ESP_ERR_INVALID_ARG;
    if (s_status.scanning) esp_bt_gap_cancel_discovery();

    lock();
    bool same_saved_peer = s_status.paired && s_cfg.peer_mac[0] && !strcmp(s_cfg.peer_mac, mac);
    if (s_status.slc_connected && s_status.peer_mac[0] && !strcmp(s_status.peer_mac, mac)) {
        s_manual_disconnect = false;
        unlock();
        return ESP_OK;
    }
    if (s_status.connecting && s_status.peer_mac[0] && !strcmp(s_status.peer_mac, mac)) {
        s_manual_disconnect = false;
        unlock();
        return ESP_OK;
    }
    memcpy(s_peer_bda, bda, ESP_BD_ADDR_LEN); s_have_peer_bda = true;
    s_status.paired = same_saved_peer;
    s_status.connecting = true;
    s_connecting_since_us = esp_timer_get_time();
    s_manual_disconnect = false;
    copy_text(s_status.peer_mac, sizeof(s_status.peer_mac), mac);
    for (size_t i = 0; i < s_status.device_count; ++i) {
        if (!strcmp(s_status.devices[i].mac, mac) && s_status.devices[i].name[0]) {
            copy_text(s_status.peer_name, sizeof(s_status.peer_name), s_status.devices[i].name);
            break;
        }
    }
    s_status.last_error[0] = 0;
    unlock();
    /* Persist the peer only after authentication/SLC succeeds. A failed pair
       must not become the saved/auto-connect device. */
    esp_err_t e = esp_hf_ag_slc_connect(bda);
    if (e != ESP_OK) {
        lock();
        s_status.paired = s_cfg.peer_mac[0] != 0;
        s_status.connecting = false;
        s_connecting_since_us = 0;
        s_next_reconnect_us = esp_timer_get_time() + BT_RECONNECT_INTERVAL_US;
        copy_text(s_status.peer_mac, sizeof(s_status.peer_mac), s_cfg.peer_mac);
        copy_text(s_status.peer_name, sizeof(s_status.peer_name), s_cfg.peer_name);
        unlock();
    }
    return e;
}

esp_err_t korvo_bluetooth_disconnect(void)
{
    lock();
    bool have = s_have_peer_bda;
    bool linked = s_status.slc_connected || s_status.connecting;
    esp_bd_addr_t bda;
    if (have) memcpy(bda, s_peer_bda, ESP_BD_ADDR_LEN);
    s_manual_disconnect = true;
    s_next_reconnect_us = 0;
    unlock();
    if (!have || !linked) return ESP_OK;
    return esp_hf_ag_slc_disconnect(bda);
}

esp_err_t korvo_bluetooth_forget(void)
{
    esp_bd_addr_t bda;
    bool have = false;
    lock();
    if (s_cfg.peer_mac[0]) have = text_to_mac(s_cfg.peer_mac, bda);
    s_cfg.peer_mac[0] = 0; s_cfg.peer_name[0] = 0;
    s_status.peer_mac[0] = 0; s_status.peer_name[0] = 0;
    s_status.paired = false;
    s_status.connecting = false;
    s_have_peer_bda = false;
    s_manual_disconnect = true;
    s_connecting_since_us = 0;
    s_next_reconnect_us = 0;
    unlock();
    esp_err_t pe = persist_cfg();
    if (have && s_stack_started) esp_bt_gap_remove_bond_device(bda);
    return pe;
}

esp_err_t korvo_bluetooth_audio_connect(void)
{
    lock(); bool ok = s_status.slc_connected && s_have_peer_bda; esp_bd_addr_t bda; if (ok) memcpy(bda, s_peer_bda, ESP_BD_ADDR_LEN); unlock();
    if (!ok) return ESP_ERR_INVALID_STATE;
    return esp_hf_ag_audio_connect(bda);
}

esp_err_t korvo_bluetooth_audio_disconnect(void)
{
    lock(); bool ok = s_status.audio_connected && s_have_peer_bda; esp_bd_addr_t bda; if (ok) memcpy(bda, s_peer_bda, ESP_BD_ADDR_LEN); unlock();
    if (!ok) return ESP_OK;
    return esp_hf_ag_audio_disconnect(bda);
}

void korvo_bluetooth_audio_kick(void)
{
#if CONFIG_BT_HFP_AUDIO_DATA_PATH_HCI
    lock(); bool ok = s_status.audio_connected; unlock();
    if (ok) esp_hf_ag_outgoing_data_ready();
#endif
}

static bool peer_for_call(esp_bd_addr_t bda)
{
    lock(); bool ok = s_status.slc_connected && s_have_peer_bda; if (ok) memcpy(bda, s_peer_bda, ESP_BD_ADDR_LEN); unlock();
    return ok;
}

void korvo_bluetooth_notify_outgoing(const char *number, bool alerting)
{
    korvo_bluetooth_supervise();
    copy_text(s_call_number, sizeof(s_call_number), number);
    s_call_incoming = false;
    s_call_state = ESP_HF_CALL_STATUS_NO_CALLS;
    s_call_setup = alerting ? ESP_HF_CALL_SETUP_STATUS_OUTGOING_ALERTING : ESP_HF_CALL_SETUP_STATUS_OUTGOING_DIALING;
    esp_bd_addr_t bda;
    if (peer_for_call(bda)) esp_hf_ag_out_call(bda, 0, 0, s_call_state, s_call_setup,
                                               s_call_number, ESP_HF_CALL_ADDR_TYPE_UNKNOWN);
}

void korvo_bluetooth_notify_incoming(const char *number)
{
    korvo_bluetooth_supervise();
    copy_text(s_call_number, sizeof(s_call_number), number);
    s_call_incoming = true;
    s_call_state = ESP_HF_CALL_STATUS_NO_CALLS;
    s_call_setup = ESP_HF_CALL_SETUP_STATUS_INCOMING;
    esp_bd_addr_t bda;
    if (peer_for_call(bda)) {
        esp_hf_ag_ciev_report(bda, ESP_HF_IND_TYPE_CALLSETUP, s_call_setup);
        esp_hf_ag_bsir(bda, ESP_HF_IN_BAND_RINGTONE_NOT_PROVIDED);
    }
}

void korvo_bluetooth_notify_active(const char *number)
{
    korvo_bluetooth_supervise();
    copy_text(s_call_number, sizeof(s_call_number), number);
    s_call_state = ESP_HF_CALL_STATUS_CALL_IN_PROGRESS;
    s_call_setup = ESP_HF_CALL_SETUP_STATUS_IDLE;
    esp_bd_addr_t bda;
    if (peer_for_call(bda)) {
        if (s_call_incoming) {
            esp_hf_ag_answer_call(bda, 1, 0, s_call_state, s_call_setup,
                                  s_call_number, ESP_HF_CALL_ADDR_TYPE_UNKNOWN);
        } else {
            esp_hf_ag_out_call(bda, 1, 0, s_call_state, s_call_setup,
                               s_call_number, ESP_HF_CALL_ADDR_TYPE_UNKNOWN);
        }
    }
}

void korvo_bluetooth_notify_ended(const char *number)
{
    copy_text(s_call_number, sizeof(s_call_number), number);
    s_call_state = ESP_HF_CALL_STATUS_NO_CALLS;
    s_call_setup = ESP_HF_CALL_SETUP_STATUS_IDLE;
    s_call_incoming = false;
    esp_bd_addr_t bda;
    if (peer_for_call(bda)) {
        esp_hf_ag_end_call(bda, 0, 0, s_call_state, s_call_setup,
                           s_call_number, ESP_HF_CALL_ADDR_TYPE_UNKNOWN);
    }
}
