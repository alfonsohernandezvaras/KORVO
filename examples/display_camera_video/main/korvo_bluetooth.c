#include "korvo_bluetooth.h"
#include <string.h>
/* LITE05: compatibility only; no radio, NVS, tasks or allocations. */
esp_err_t korvo_bluetooth_init(void){return ESP_OK;}
esp_err_t korvo_bluetooth_get_config(korvo_bluetooth_config_t *cfg){if(!cfg)return ESP_ERR_INVALID_ARG; memset(cfg,0,sizeof(*cfg));return ESP_OK;}
esp_err_t korvo_bluetooth_save_config(const korvo_bluetooth_config_t *cfg){(void)cfg;return ESP_ERR_NOT_SUPPORTED;}
void korvo_bluetooth_get_status(korvo_bluetooth_status_t *status){if(status)memset(status,0,sizeof(*status));}
size_t korvo_bluetooth_status_json(char *out,size_t out_len){const char *s="{\"enabled\":false,\"engine_ready\":false}";size_t n=strlen(s);if(!out||out_len<=n)return 0;memcpy(out,s,n+1);return n;}
esp_err_t korvo_bluetooth_set_event_listener(korvo_bluetooth_event_listener_t listener,void *ctx){(void)listener;(void)ctx;return ESP_ERR_NOT_SUPPORTED;}
esp_err_t korvo_bluetooth_connect(const char *mac){(void)mac;return ESP_ERR_NOT_SUPPORTED;}
esp_err_t korvo_bluetooth_scan(void){return ESP_ERR_NOT_SUPPORTED;}
esp_err_t korvo_bluetooth_disconnect(void){return ESP_ERR_NOT_SUPPORTED;}
esp_err_t korvo_bluetooth_forget(void){return ESP_ERR_NOT_SUPPORTED;}
esp_err_t korvo_bluetooth_audio_connect(void){return ESP_ERR_NOT_SUPPORTED;}
esp_err_t korvo_bluetooth_audio_disconnect(void){return ESP_ERR_NOT_SUPPORTED;}
void korvo_bluetooth_audio_kick(void){}
void korvo_bluetooth_supervise(void){}
uint8_t korvo_bluetooth_get_output_volume(void){return 75;}
esp_err_t korvo_bluetooth_set_output_volume(uint8_t percent,bool persist){(void)percent;(void)persist;return ESP_ERR_NOT_SUPPORTED;}
uint8_t korvo_bluetooth_get_mic_gain(void){return 75;}
esp_err_t korvo_bluetooth_set_mic_gain(uint8_t percent,bool persist){(void)percent;(void)persist;return ESP_ERR_NOT_SUPPORTED;}
void korvo_bluetooth_notify_incoming(const char *number){(void)number;}
void korvo_bluetooth_notify_active(const char *number){(void)number;}
void korvo_bluetooth_notify_ended(const char *number){(void)number;}
void korvo_bluetooth_notify_outgoing(const char *number,bool alerting){(void)number;(void)alerting;}
