/* KORVO VIDEO-05: ESP32-P4 JPEG hardware, VGA display-copy publisher.
 * No changes to camera, LVGL, face AI, VSYNC or SIP. */
#include "korvo_video01.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/jpeg_encode.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define V04_W 640U
#define V04_H 480U
#define V04_RAW (V04_W*V04_H*2U)
#define V04_JPEG_CAP (256U*1024U)
#define V07_RGB888 (V04_W*V04_H*3U)
/* KORVO_VIDEO07_HMI_COPY_COLOR */
#define V04_INTERVAL_US 200000LL
static const char *TAG="KORVO_VIDEO04";
static uint8_t *raw_frame, *jpeg_frame, *rgb888_frame;
static volatile int v07_byte_swap=1;
static size_t jpeg_len;
static SemaphoreHandle_t raw_mutex, jpg_mutex;
static TaskHandle_t encoder_task;
static volatile uint32_t callbacks, rejected, published, encode_errors, input_w, input_h;
static volatile size_t input_bytes;
static volatile int64_t jpeg_timestamp;
static volatile bool pending;
static int64_t last_sample;
static httpd_handle_t stream_server;

static void jpeg_worker(void *arg) {
    (void)arg;
    jpeg_encoder_handle_t engine=NULL;
    jpeg_encode_engine_cfg_t ecfg={.intr_priority=0,.timeout_ms=1000};
    if (jpeg_new_encoder_engine(&ecfg,&engine)!=ESP_OK) {
        ESP_LOGE(TAG,"JPEG engine init failed"); vTaskDelete(NULL); return;
    }
    jpeg_encode_cfg_t cfg={.height=V04_H,.width=V04_W,
        .src_type=JPEG_ENCODE_IN_FORMAT_RGB888,
        .sub_sample=JPEG_DOWN_SAMPLING_YUV422,.image_quality=85,.pixel_reverse=false};
    while(1) {
        ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
        if(!raw_frame || !jpeg_frame || !rgb888_frame || !raw_mutex || !jpg_mutex) continue;
        if(xSemaphoreTake(raw_mutex,pdMS_TO_TICKS(100))!=pdTRUE) continue;
        pending=false;
        if(xSemaphoreTake(jpg_mutex,pdMS_TO_TICKS(100))==pdTRUE) {
            uint32_t out=0;
            /* Web-only RGB565 -> RGB888. HMI RGB565 and camera never touched.
             * LVGL display buffer may store RGB565 bytes in swapped order.
             * User can compare modes with /video/color?swap=0 or swap=1. */
            const bool swap = v07_byte_swap != 0;
            for (size_t i=0,j=0; i<V04_RAW; i+=2,j+=3) {
                uint16_t px=swap ? (((uint16_t)raw_frame[i]<<8)|raw_frame[i+1])
                                 : (((uint16_t)raw_frame[i+1]<<8)|raw_frame[i]);
                uint8_t red=(px>>11)&31, green=(px>>5)&63, blue=px&31;
                rgb888_frame[j]=(red<<3)|(red>>2);
                rgb888_frame[j+1]=(green<<2)|(green>>4);
                rgb888_frame[j+2]=(blue<<3)|(blue>>2);
            }
            esp_err_t e=jpeg_encoder_process(engine,&cfg,rgb888_frame,V07_RGB888,jpeg_frame,V04_JPEG_CAP,&out);
            if(e==ESP_OK && out>0 && out<=V04_JPEG_CAP) {
                jpeg_len=out; jpeg_timestamp=esp_timer_get_time(); published++;
            } else { encode_errors++; ESP_LOGW(TAG,"JPEG encode error=%s size=%lu",esp_err_to_name(e),(unsigned long)out); }
            xSemaphoreGive(jpg_mutex);
        }
        xSemaphoreGive(raw_mutex);
    }
}

/* Called with the SAME oriented RGB565 buffer passed to LVGL. Never retains pointer. */
void korvo_video01_capture(const uint8_t *p,uint32_t w,uint32_t h,size_t len) {
    callbacks++; input_w=w;input_h=h;input_bytes=len;
    if(!p || w!=V04_W || h!=V04_H || len<V04_RAW) { rejected++; return; }
    int64_t now=esp_timer_get_time();
    if(now-last_sample<V04_INTERVAL_US || pending) return;
    if(!raw_frame) {
        raw_mutex=xSemaphoreCreateMutex();jpg_mutex=xSemaphoreCreateMutex();
        raw_frame=heap_caps_aligned_alloc(64,V04_RAW,MALLOC_CAP_SPIRAM|MALLOC_CAP_DMA|MALLOC_CAP_8BIT);
        jpeg_frame=heap_caps_aligned_alloc(64,V04_JPEG_CAP,MALLOC_CAP_SPIRAM|MALLOC_CAP_DMA|MALLOC_CAP_8BIT);
        rgb888_frame=heap_caps_aligned_alloc(64,V07_RGB888,MALLOC_CAP_SPIRAM|MALLOC_CAP_DMA|MALLOC_CAP_8BIT);
        if(!raw_mutex||!jpg_mutex||!raw_frame||!jpeg_frame||!rgb888_frame){
            ESP_LOGE(TAG,"Insufficient PSRAM for VGA JPEG publisher");return;
        }
        if(xTaskCreate(jpeg_worker,"korvo_jpeg",8192,NULL,4,&encoder_task)!=pdPASS){
            ESP_LOGE(TAG,"JPEG task failed");return;
        }
    }
    if(!encoder_task || xSemaphoreTake(raw_mutex,0)!=pdTRUE)return;
    memcpy(raw_frame,p,V04_RAW);
    pending=true;last_sample=now;
    xSemaphoreGive(raw_mutex);
    xTaskNotifyGive(encoder_task);
}

bool korvo_video01_ready(void) {
    int64_t t=jpeg_timestamp;
    return t>0 && (esp_timer_get_time()-t)<3000000LL;
}

static esp_err_t video_status(httpd_req_t *r) {
    char buf[420];
    int64_t age=jpeg_timestamp?(esp_timer_get_time()-jpeg_timestamp)/1000:-1;
    snprintf(buf,sizeof(buf),"{\"ok\":%s,\"format\":\"JPEG\",\"width\":640,\"height\":480,\"fps_target\":5,\"frames\":%lu,\"age_ms\":%lld,\"jpeg_bytes\":%u,\"input_w\":%lu,\"input_h\":%lu,\"input_bytes\":%u,\"callbacks\":%lu,\"rejected\":%lu,\"encode_errors\":%lu,\"stream_port\":8081}",
      korvo_video01_ready()?"true":"false",(unsigned long)published,(long long)age,(unsigned)jpeg_len,
      (unsigned long)input_w,(unsigned long)input_h,(unsigned)input_bytes,
      (unsigned long)callbacks,(unsigned long)rejected,(unsigned long)encode_errors);
    httpd_resp_set_type(r,"application/json");httpd_resp_set_hdr(r,"Cache-Control","no-store");return httpd_resp_sendstr(r,buf);
}
static esp_err_t video_view(httpd_req_t *r) {
    static const char page[] =
      "<!doctype html><html lang='es'><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>KORVO VIDEO-05</title><style>body{background:#101820;color:#eee;font:16px Arial;text-align:center}"
      "img{display:block;width:640px;max-width:95vw;height:auto;margin:14px auto;border:2px solid #268ad1}"
      "pre{display:inline-block;text-align:left;color:#8ce0ad;font-size:13px}#msg{color:#ffd28b}</style></head>"
      "<body><h2>KORVO - VIDEO IP</h2><p>VGA 640x480 / JPEG / objetivo 5 FPS</p>"
      "<p id='msg'>Conectando MJPEG...</p><img id='v' alt='VIDEO DOWN'>"
      "<p><button onclick='snapshot()'>Ver fotografia</button> "
      "<button onclick='stream()'>Ver streaming</button></p><pre id='s'>Conectando...</pre>"
      "<script>const u=location.protocol+'//'+location.hostname+':8081';"
      "const im=document.getElementById('v'),msg=document.getElementById('msg');"
      "function snapshot(){im.src=u+'/video/snapshot.jpg?t='+Date.now();msg.textContent='Fotografia JPEG';}"
      "function stream(){im.src=u+'/video/stream?t='+Date.now();msg.textContent='Conectando MJPEG...';}"
      "im.onload=()=>{msg.textContent='Imagen recibida';};"
      "im.onerror=()=>{msg.textContent='No se pudo cargar video desde puerto 8081';};"
      "stream();setInterval(()=>fetch('/video/status',{cache:'no-store'}).then(x=>x.json())"
      ".then(x=>{document.getElementById('s').textContent=JSON.stringify(x,null,2)})"
      ".catch(()=>{}),2000);</script></body></html>";
    httpd_resp_set_type(r,"text/html; charset=utf-8");
    return httpd_resp_sendstr(r,page);
}
static esp_err_t video_color(httpd_req_t *r) {
    char q[64]={0},val[8]={0};
    if(httpd_req_get_url_query_str(r,q,sizeof(q))==ESP_OK &&
       httpd_query_key_value(q,"swap",val,sizeof(val))==ESP_OK) {
        if(val[0]=='0' && val[1]==0) v07_byte_swap=0;
        else if(val[0]=='1' && val[1]==0) v07_byte_swap=1;
    }
    char resp[96];
    snprintf(resp,sizeof(resp),"{\"swap\":%d,\"description\":\"RGB565 byte order for WEB ONLY\"}",v07_byte_swap);
    httpd_resp_set_type(r,"application/json");
    return httpd_resp_sendstr(r,resp);
}
static esp_err_t video_snapshot(httpd_req_t *r) {
    if(!jpg_mutex || xSemaphoreTake(jpg_mutex,pdMS_TO_TICKS(200))!=pdTRUE) {httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"VIDEO DOWN");return ESP_FAIL;}
    if(!jpeg_len) {xSemaphoreGive(jpg_mutex);httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"NO JPEG");return ESP_FAIL;}
    httpd_resp_set_type(r,"image/jpeg");httpd_resp_set_hdr(r,"Cache-Control","no-store");
    esp_err_t e=httpd_resp_send(r,(const char*)jpeg_frame,jpeg_len);xSemaphoreGive(jpg_mutex);return e;
}
static esp_err_t video_stream(httpd_req_t *r) {
    httpd_resp_set_type(r,"multipart/x-mixed-replace;boundary=korvoframe");
    httpd_resp_set_hdr(r,"Cache-Control","no-store");
    uint32_t last=0;
    while(1) {
        if(published==last || !korvo_video01_ready()) {vTaskDelay(pdMS_TO_TICKS(80));continue;}
        if(!jpg_mutex || xSemaphoreTake(jpg_mutex,pdMS_TO_TICKS(100))!=pdTRUE)continue;
        size_t n=jpeg_len;last=published;
        char header[110];
        int z=snprintf(header,sizeof(header),"\r\n--korvoframe\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",(unsigned)n);
        esp_err_t e=httpd_resp_send_chunk(r,header,z);
        if(e==ESP_OK)e=httpd_resp_send_chunk(r,(const char*)jpeg_frame,n);
        xSemaphoreGive(jpg_mutex);
        if(e!=ESP_OK)break;
    }
    return ESP_OK;
}
void korvo_video01_register(httpd_handle_t server) {
    httpd_uri_t st={.uri="/video/status",.method=HTTP_GET,.handler=video_status};
    httpd_uri_t vw={.uri="/video/view",.method=HTTP_GET,.handler=video_view};
    httpd_register_uri_handler(server,&st);httpd_register_uri_handler(server,&vw);
    httpd_uri_t co={.uri="/video/color",.method=HTTP_GET,.handler=video_color};
    httpd_register_uri_handler(server,&co);
    /* Dedicated HTTP server: a long MJPEG request cannot block the existing UI. */
    httpd_config_t cfg=HTTPD_DEFAULT_CONFIG();
    cfg.server_port=8081;cfg.ctrl_port=32769;cfg.stack_size=8192;cfg.max_open_sockets=3;
    if(httpd_start(&stream_server,&cfg)==ESP_OK) {
        httpd_uri_t ss={.uri="/video/stream",.method=HTTP_GET,.handler=video_stream};
        httpd_uri_t sn={.uri="/video/snapshot.jpg",.method=HTTP_GET,.handler=video_snapshot};
        httpd_register_uri_handler(stream_server,&ss);httpd_register_uri_handler(stream_server,&sn);
        ESP_LOGI(TAG,"MJPEG server started on port 8081");
    } else ESP_LOGE(TAG,"MJPEG server failed on port 8081");
}
