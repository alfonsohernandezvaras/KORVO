/* KORVO_VIDEO09_240x180: KORVO_VIDEO08_ONDEMAND: VGA camera/HMI untouched, QVGA HTTP on demand.
 * VIDEO PAIR / SIP SDP Gateway2 control: future integration, not implemented here. */
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
#define SRC_W 640U
#define SRC_H 480U
#define DST_W 640U
#define DST_H 480U
#define RAW_BYTES (DST_W*DST_H*2U)
#define RGB_BYTES (DST_W*DST_H*3U)
#define JPEG_CAP (256U*1024U)
#define INTERVAL_US 100000LL
static const char *TAG="KORVO_VIDEO08";
static uint8_t *raw_frame,*jpeg_frame,*rgb888_frame;
static SemaphoreHandle_t raw_mutex,jpg_mutex;
static TaskHandle_t encoder_task;
static httpd_handle_t stream_server;
static volatile uint32_t callbacks,rejected,published,encode_errors;
static volatile uint32_t input_w,input_h,stream_clients;
static volatile size_t input_bytes,jpeg_len;
static volatile int64_t camera_timestamp,jpeg_timestamp;
static volatile bool pending,startup_done,startup_ok,snapshot_pending;
static volatile int v_stream_bgr=1; /* WEB only, 1=BGR test, 0=original RGB */
static volatile int v07_byte_swap=1; /* working VIDEO07 default */
static int64_t last_sample;

static bool buffers_init(void) {
    if(raw_frame && jpeg_frame && rgb888_frame && raw_mutex && jpg_mutex)return true;
    if(!raw_mutex)raw_mutex=xSemaphoreCreateMutex();
    if(!jpg_mutex)jpg_mutex=xSemaphoreCreateMutex();
    if(!raw_frame)raw_frame=heap_caps_aligned_alloc(64,RAW_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_DMA|MALLOC_CAP_8BIT);
    if(!jpeg_frame)jpeg_frame=heap_caps_aligned_alloc(64,JPEG_CAP,MALLOC_CAP_SPIRAM|MALLOC_CAP_DMA|MALLOC_CAP_8BIT);
    if(!rgb888_frame)rgb888_frame=heap_caps_aligned_alloc(64,RGB_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_DMA|MALLOC_CAP_8BIT);
    return raw_mutex && jpg_mutex && raw_frame && jpeg_frame && rgb888_frame;
}
static void jpeg_worker(void *arg) {
    (void)arg;
    jpeg_encoder_handle_t engine=NULL;
    jpeg_encode_engine_cfg_t ecfg={.intr_priority=0,.timeout_ms=1000};
    if(jpeg_new_encoder_engine(&ecfg,&engine)!=ESP_OK){ESP_LOGE(TAG,"JPEG engine init failed");vTaskDelete(NULL);return;}
    jpeg_encode_cfg_t cfg={.height=DST_H,.width=DST_W,.src_type=JPEG_ENCODE_IN_FORMAT_RGB888,
        .sub_sample=JPEG_DOWN_SAMPLING_YUV422,.image_quality=85,.pixel_reverse=false};
    while(1){
        ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
        if(xSemaphoreTake(raw_mutex,pdMS_TO_TICKS(100))!=pdTRUE)continue;
        const bool swap=v07_byte_swap!=0;
        for(size_t i=0,j=0;i<RAW_BYTES;i+=2,j+=3){
            uint16_t px=swap?(((uint16_t)raw_frame[i]<<8)|raw_frame[i+1]):(((uint16_t)raw_frame[i+1]<<8)|raw_frame[i]);
            uint8_t red=(px>>11)&31,green=(px>>5)&63,blue=px&31;
            /* STREAM 2 only: test RGB/BGR channel order, default BGR. */
            rgb888_frame[j]=v_stream_bgr ? ((blue<<3)|(blue>>2)) : ((red<<3)|(red>>2));
            rgb888_frame[j+1]=(green<<2)|(green>>4);
            rgb888_frame[j+2]=v_stream_bgr ? ((red<<3)|(red>>2)) : ((blue<<3)|(blue>>2));
        }
        xSemaphoreGive(raw_mutex);
        if(xSemaphoreTake(jpg_mutex,pdMS_TO_TICKS(100))==pdTRUE){
            uint32_t out=0;
            esp_err_t e=jpeg_encoder_process(engine,&cfg,rgb888_frame,RGB_BYTES,jpeg_frame,JPEG_CAP,&out);
            if(e==ESP_OK && out>0 && out<=JPEG_CAP){
                jpeg_len=out;jpeg_timestamp=esp_timer_get_time();published++;startup_ok=true;
            }else{encode_errors++;ESP_LOGW(TAG,"JPEG encode error=%s bytes=%lu",esp_err_to_name(e),(unsigned long)out);}
            xSemaphoreGive(jpg_mutex);
        }
        startup_done=true;
        pending=false;
    }
}
/* Called with the same flipped RGB565 buffer as LVGL, no changes to HMI. */
void korvo_video01_capture(const uint8_t *p,uint32_t w,uint32_t h,size_t len){
    callbacks++;input_w=w;input_h=h;input_bytes=len;
    if(!p||w!=SRC_W||h!=SRC_H||len<SRC_W*SRC_H*2U){rejected++;return;}
    int64_t now=esp_timer_get_time();camera_timestamp=now;
    /* Exactly one startup JPEG self-test, then zero JPEG work when idle. */
    if(startup_done && stream_clients==0 && !snapshot_pending)return;
    if(now-last_sample<INTERVAL_US || pending)return;
    if(!buffers_init()){ESP_LOGE(TAG,"PSRAM buffers unavailable");return;}
    if(!encoder_task){
        if(xTaskCreate(jpeg_worker,"korvo_jpeg",8192,NULL,4,&encoder_task)!=pdPASS){ESP_LOGE(TAG,"JPEG worker task failed");return;}
    }
    if(xSemaphoreTake(raw_mutex,0)!=pdTRUE)return;
    /* WEB copy only: full VGA, without resampling or touching LVGL buffer. */
    memcpy(raw_frame,p,RAW_BYTES);
    pending=true;last_sample=now;
    snapshot_pending=false;
    xSemaphoreGive(raw_mutex);
    xTaskNotifyGive(encoder_task);
}
bool korvo_video01_ready(void){
    return startup_ok && camera_timestamp>0 && (esp_timer_get_time()-camera_timestamp)<3000000LL;
}
static esp_err_t video_status(httpd_req_t *r){
    char b[560];
    int64_t age=jpeg_timestamp?(esp_timer_get_time()-jpeg_timestamp)/1000:-1;
    int64_t cam_age=camera_timestamp?(esp_timer_get_time()-camera_timestamp)/1000:-1;
    snprintf(b,sizeof(b),"{\"ok\":%s,\"camera\":\"%s\",\"startup_jpeg_test\":\"%s\",\"stream_state\":\"%s\",\"clients\":%lu,\"jpeg_active\":%s,\"format\":\"JPEG\",\"width\":640,\"height\":480,\"fps_target\":10,\"frames\":%lu,\"age_ms\":%lld,\"camera_age_ms\":%lld,\"jpeg_bytes\":%u,\"input_w\":%lu,\"input_h\":%lu,\"input_bytes\":%u,\"callbacks\":%lu,\"rejected\":%lu,\"encode_errors\":%lu,\"stream_port\":8081}",
      korvo_video01_ready()?"true":"false",cam_age>=0&&cam_age<3000?"OK":"DOWN",
      !startup_done?"PENDING":(startup_ok?"OK":"ERROR"),stream_clients?"STREAMING":"READY",
      (unsigned long)stream_clients,stream_clients||snapshot_pending?"true":"false",
      (unsigned long)published,(long long)age,(long long)cam_age,(unsigned)jpeg_len,
      (unsigned long)input_w,(unsigned long)input_h,(unsigned)input_bytes,
      (unsigned long)callbacks,(unsigned long)rejected,(unsigned long)encode_errors);
    httpd_resp_set_type(r,"application/json");httpd_resp_set_hdr(r,"Cache-Control","no-store");return httpd_resp_sendstr(r,b);
}
static esp_err_t video_view(httpd_req_t *r){
    static const char page[]=
      "<!doctype html><html lang='es'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>KORVO VIDEO-08</title><style>body{background:#101820;color:#eee;font:16px Arial;text-align:center}"
      "img{display:block;width:640px;max-width:95vw;height:auto;margin:14px auto;border:2px solid #268ad1}"
      "pre{display:inline-block;text-align:left;color:#8ce0ad;font-size:13px}#msg{color:#ffd28b}</style></head>"
      "<body><h2>KORVO - VIDEO IP</h2><p>Streaming 640x480 / JPEG / objetivo 10 FPS / ON-DEMAND</p>"
      "<p id='msg'>Conectando MJPEG...</p><img id='v' alt='VIDEO DOWN'>"
      "<p><button onclick='snapshot()'>Ver fotografia</button> <button onclick='stream()'>Ver streaming</button>"
      " <button onclick='stop()'>STOP streaming</button></p><pre id='s'>Conectando...</pre>"
      "<script>const u=location.protocol+'//'+location.hostname+':8081';"
      "const im=document.getElementById('v'),msg=document.getElementById('msg');"
      "function stop(){im.removeAttribute('src');msg.textContent='Streaming detenido';}"
      "function snapshot(){stop();im.src=u+'/video/snapshot.jpg?t='+Date.now();msg.textContent='Fotografia JPEG';}"
      "function stream(){stop();im.src=u+'/video/stream?t='+Date.now();msg.textContent='Conectando MJPEG...';}"
      "im.onload=()=>{msg.textContent='Imagen recibida';};"
      "im.onerror=()=>{msg.textContent='No se pudo cargar video desde puerto 8081';};"
      "stream();setInterval(()=>fetch('/video/status',{cache:'no-store'}).then(x=>x.json())"
      ".then(x=>{document.getElementById('s').textContent=JSON.stringify(x,null,2)})"
      ".catch(()=>{}),2000);</script></body></html>";
    httpd_resp_set_type(r,"text/html; charset=utf-8");return httpd_resp_sendstr(r,page);
}
static esp_err_t video_color(httpd_req_t *r){
    char q[64]={0},val[8]={0};
    if(httpd_req_get_url_query_str(r,q,sizeof(q))==ESP_OK&&httpd_query_key_value(q,"swap",val,sizeof(val))==ESP_OK){
        if(val[0]=='0'&&!val[1])v07_byte_swap=0;
        else if(val[0]=='1'&&!val[1])v07_byte_swap=1;
    }
    if(httpd_req_get_url_query_str(r,q,sizeof(q))==ESP_OK&&httpd_query_key_value(q,"channels",val,sizeof(val))==ESP_OK){
        if(val[0]=='0'&&!val[1])v_stream_bgr=0;
        else if(val[0]=='1'&&!val[1])v_stream_bgr=1;
    }
    char resp[128];snprintf(resp,sizeof(resp),"{\"swap\":%d,\"channels_bgr\":%d,\"description\":\"STREAM2 color test only\"}",v07_byte_swap,v_stream_bgr);
    httpd_resp_set_type(r,"application/json");return httpd_resp_sendstr(r,resp);
}
static esp_err_t video_snapshot(httpd_req_t *r){
    /* One-shot JPEG request when stream is idle. */
    uint32_t before=published;
    snapshot_pending=true;
    for(int i=0;i<25 && published==before;i++)vTaskDelay(pdMS_TO_TICKS(80));
    if(!jpg_mutex||xSemaphoreTake(jpg_mutex,pdMS_TO_TICKS(200))!=pdTRUE){httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"VIDEO DOWN");return ESP_FAIL;}
    if(!jpeg_len){xSemaphoreGive(jpg_mutex);httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"NO JPEG");return ESP_FAIL;}
    httpd_resp_set_type(r,"image/jpeg");httpd_resp_set_hdr(r,"Cache-Control","no-store");
    esp_err_t e=httpd_resp_send(r,(const char*)jpeg_frame,jpeg_len);xSemaphoreGive(jpg_mutex);return e;
}
static esp_err_t video_stream(httpd_req_t *r){
    stream_clients++;
    httpd_resp_set_type(r,"multipart/x-mixed-replace;boundary=korvoframe");
    httpd_resp_set_hdr(r,"Cache-Control","no-store");
    uint32_t last=published;
    esp_err_t e=ESP_OK;
    int64_t start=esp_timer_get_time();
    while(1){
        if(published==last){
            vTaskDelay(pdMS_TO_TICKS(80));
            if(esp_timer_get_time()-start>10000000LL){e=ESP_ERR_TIMEOUT;break;}
            continue;
        }
        start=esp_timer_get_time();
        if(!jpg_mutex||xSemaphoreTake(jpg_mutex,pdMS_TO_TICKS(100))!=pdTRUE)continue;
        size_t n=jpeg_len;last=published;
        char header[110];
        int z=snprintf(header,sizeof(header),"\r\n--korvoframe\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",(unsigned)n);
        e=httpd_resp_send_chunk(r,header,z);
        if(e==ESP_OK)e=httpd_resp_send_chunk(r,(const char*)jpeg_frame,n);
        xSemaphoreGive(jpg_mutex);
        if(e!=ESP_OK)break;
    }
    if(stream_clients>0)stream_clients--;
    return e;
}
void korvo_video01_register(httpd_handle_t server){
    httpd_uri_t st={.uri="/video/status",.method=HTTP_GET,.handler=video_status};
    httpd_uri_t vw={.uri="/video/view",.method=HTTP_GET,.handler=video_view};
    httpd_uri_t co={.uri="/video/color",.method=HTTP_GET,.handler=video_color};
    httpd_register_uri_handler(server,&st);httpd_register_uri_handler(server,&vw);httpd_register_uri_handler(server,&co);
    httpd_config_t cfg=HTTPD_DEFAULT_CONFIG();
    cfg.server_port=8081;cfg.ctrl_port=32769;cfg.stack_size=8192;cfg.max_open_sockets=3;
    if(httpd_start(&stream_server,&cfg)==ESP_OK){
        httpd_uri_t ss={.uri="/video/stream",.method=HTTP_GET,.handler=video_stream};
        httpd_uri_t sn={.uri="/video/snapshot.jpg",.method=HTTP_GET,.handler=video_snapshot};
        httpd_register_uri_handler(stream_server,&ss);httpd_register_uri_handler(stream_server,&sn);
        ESP_LOGI(TAG,"MJPEG ON-DEMAND 640x480 server port 8081");
    }else ESP_LOGE(TAG,"MJPEG server failed on port 8081");
}
