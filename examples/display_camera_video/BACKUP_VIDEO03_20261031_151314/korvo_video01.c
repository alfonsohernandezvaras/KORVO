/* KORVO VIDEO-02: estado unico para productor y servidor HTTP. */
#include "korvo_video01.h"
/* KORVO VIDEO01 - RGB565 publisher, not SIP codec */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#define KVIDEO_BYTES (320U*240U*2U)
static uint8_t *kv_frame;
static SemaphoreHandle_t kv_lock;
static uint32_t kv_seq;
static int64_t kv_time;
void korvo_video01_capture(const uint8_t *p,uint32_t w,uint32_t h,size_t len){
    if(!p||w!=320||h!=240||len<KVIDEO_BYTES)return;
    if(!kv_lock){kv_lock=xSemaphoreCreateMutex();if(!kv_lock)return;}
    if(!kv_frame){
        uint8_t *tmp=heap_caps_malloc(KVIDEO_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(!tmp)return;
        if(xSemaphoreTake(kv_lock,0)==pdTRUE){
            if(!kv_frame)kv_frame=tmp;else heap_caps_free(tmp);
            xSemaphoreGive(kv_lock);
        }else heap_caps_free(tmp);
    }
    if(!kv_frame)return;
    int64_t t=esp_timer_get_time();
    if(t-kv_time<100000)return;
    if(xSemaphoreTake(kv_lock,0)!=pdTRUE)return;
    memcpy(kv_frame,p,KVIDEO_BYTES);
    kv_seq++;kv_time=t;
    xSemaphoreGive(kv_lock);
}
static esp_err_t kv_status(httpd_req_t *r){
    char buf[180];
    int64_t age=kv_time?(esp_timer_get_time()-kv_time)/1000:-1;
    snprintf(buf,sizeof(buf),
      "{\"ok\":%s,\"format\":\"RGB565_LE\",\"width\":320,\"height\":240,\"bytes\":%u,\"frames\":%lu,\"age_ms\":%lld}",
      kv_frame&&age>=0&&age<3000?"true":"false",(unsigned)KVIDEO_BYTES,
      (unsigned long)kv_seq,(long long)age);
    httpd_resp_set_type(r,"application/json");
    return httpd_resp_sendstr(r,buf);
}
static esp_err_t kv_frame_get(httpd_req_t *r){
    uint8_t *b=heap_caps_malloc(KVIDEO_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!b){httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"No PSRAM");return ESP_FAIL;}
    if(!kv_lock||xSemaphoreTake(kv_lock,pdMS_TO_TICKS(50))!=pdTRUE){
        heap_caps_free(b);httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"VIDEO DOWN");return ESP_FAIL;
    }
    if(!kv_frame){xSemaphoreGive(kv_lock);heap_caps_free(b);
        httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"VIDEO DOWN");return ESP_FAIL;}
    memcpy(b,kv_frame,KVIDEO_BYTES);
    xSemaphoreGive(kv_lock);
    httpd_resp_set_type(r,"application/octet-stream");
    httpd_resp_set_hdr(r,"X-Pixel-Format","RGB565_LE");
    httpd_resp_set_hdr(r,"X-Width","320");
    httpd_resp_set_hdr(r,"X-Height","240");
    httpd_resp_set_hdr(r,"Cache-Control","no-store");
    esp_err_t e=httpd_resp_send(r,(const char *)b,KVIDEO_BYTES);
    heap_caps_free(b);
    return e;
}
void korvo_video01_register(httpd_handle_t server){
    httpd_uri_t status={.uri="/video/status",.method=HTTP_GET,.handler=kv_status};
    httpd_uri_t frame={.uri="/video/frame",.method=HTTP_GET,.handler=kv_frame_get};
    httpd_register_uri_handler(server,&status);
    httpd_register_uri_handler(server,&frame);
}
