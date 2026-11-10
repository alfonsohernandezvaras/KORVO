/* KORVO_VIDEO19_CAMERA_MANAGER: NVS separado; no modifica korvo/config blob. */
#ifndef KORVO_CAMERA_SETTINGS_H
#define KORVO_CAMERA_SETTINGS_H
#include <stdint.h>
#include "nvs.h"
#include "esp_err.h"
typedef struct { uint16_t width, height; uint8_t fps; } korvo_camera_setting_t;
static inline korvo_camera_setting_t korvo_camera_setting_read(void)
{
    korvo_camera_setting_t v = {640, 480, 10};
    nvs_handle_t h;
    if (nvs_open("cam_video19", NVS_READONLY, &h) != ESP_OK) return v;
    uint16_t w=0, ht=0; uint8_t fps=0;
    esp_err_t a=nvs_get_u16(h,"width",&w);
    esp_err_t b=nvs_get_u16(h,"height",&ht);
    esp_err_t c=nvs_get_u8(h,"fps",&fps);
    nvs_close(h);
    if (a==ESP_OK && b==ESP_OK && c==ESP_OK &&
        ((w==240 && ht==240) || (w==320 && ht==240) ||
         (w==400 && ht==300) || (w==640 && ht==480)) &&
        (fps==5 || fps==8 || fps==10 || fps==12 || fps==15 || fps==20 || fps==24)) {
        v.width=w; v.height=ht; v.fps=fps;
    }
    return v;
}
static inline esp_err_t korvo_camera_setting_write(korvo_camera_setting_t v)
{
    nvs_handle_t h;
    esp_err_t e=nvs_open("cam_video19", NVS_READWRITE, &h);
    if (e!=ESP_OK) return e;
    if (e==ESP_OK) e=nvs_set_u16(h,"width",v.width);
    if (e==ESP_OK) e=nvs_set_u16(h,"height",v.height);
    if (e==ESP_OK) e=nvs_set_u8(h,"fps",v.fps);
    if (e==ESP_OK) e=nvs_commit(h);
    nvs_close(h);
    return e;
}
#endif
