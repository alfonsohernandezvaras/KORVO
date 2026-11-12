/* KORVO_VIDEO12_240x180_10FPS: KORVO_VIDEO08_ONDEMAND: VGA camera/HMI untouched, QVGA HTTP on demand.
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
/* KORVO_PERF02_CPU1: tareas HTTP y JPEG en CPU1; HMI intacta. */
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "lwip/sockets.h"
/* SOCKET_DIAG01: diagnostico pasivo de descriptores lwIP. */
#include "sdkconfig.h"
#include "lwip/opt.h"

#include "esp_err.h"
#include "korvo_protocol.h"
#include <inttypes.h>
#include <stdlib.h>

#define SRC_W 640U
#define SRC_H 480U
#define DST_W 240U
#define DST_H 180U
#define RAW_BYTES (DST_W*DST_H*2U)
#define RGB_BYTES (DST_W*DST_H*3U)
#define JPEG_CAP (256U*1024U)
#define INTERVAL_US 100000LL
static const char *TAG="KORVO_VIDEO12";
static uint8_t *raw_frame,*jpeg_frame,*rgb888_frame;
static SemaphoreHandle_t raw_mutex,jpg_mutex;
static TaskHandle_t encoder_task;
static httpd_handle_t stream_server;
static volatile uint32_t callbacks,rejected,published,encode_errors;
static volatile uint32_t input_w,input_h,stream_clients;
static volatile size_t input_bytes,jpeg_len;
static volatile int64_t camera_timestamp,jpeg_timestamp;
static volatile bool pending,startup_done,startup_ok,snapshot_pending;
/* Diagnostico: 1=STREAM2 habilitado, 0=sin copia ni JPEG de STREAM2. */
static volatile bool stream2_enabled = true;
/* KORVO_DIAG01_TASKMANAGER: diagnostico y recuperacion STREAM2. */
static volatile uint32_t stream_epoch=1;
static volatile bool stream_admin_disabled=false;
static volatile uint32_t stream_reset_count=0;
static volatile bool stream_restart_busy=false;
static volatile int stream_restart_result=0; /* 0 idle,1 ejecutando,2 OK,-1 error */
static portMUX_TYPE stream_lock=portMUX_INITIALIZER_UNLOCKED;
static int stream_fds[2]={-1,-1};
static bool stream_client_add(int fd){
 bool ok=false;
 portENTER_CRITICAL(&stream_lock);
 for(int i=0;i<2;i++)if(stream_fds[i]==-1){stream_fds[i]=fd;stream_clients++;ok=true;break;}
 portEXIT_CRITICAL(&stream_lock);return ok;
}
static void stream_client_remove(int fd){
 portENTER_CRITICAL(&stream_lock);
 for(int i=0;i<2;i++)if(stream_fds[i]==fd){stream_fds[i]=-1;if(stream_clients)stream_clients--;break;}
 portEXIT_CRITICAL(&stream_lock);
}
/* DICCIONARIO-STREAM01: comandos locales, NO alteran V3 Gateway. */
typedef enum { STREAM_CMD_ENABLE=0xE0, STREAM_CMD_DISABLE=0xE1,
               STREAM_CMD_RESET=KORVO_MSG_VIDEO_STREAM_RESET } stream_cmd_t;
static bool stream_dispatch(stream_cmd_t cmd);
static bool korvo_admin(httpd_req_t *r){
 char cookie[256]={0};return httpd_req_get_hdr_value_str(r,"Cookie",cookie,sizeof(cookie))==ESP_OK && strstr(cookie,"KORVO_SESSION=korvo_admin")!=NULL;
}
static void stream_close_clients(void){
 int fds[2];portENTER_CRITICAL(&stream_lock);
 for(int i=0;i<2;i++)fds[i]=stream_fds[i];
 portEXIT_CRITICAL(&stream_lock);
 for(int i=0;i<2;i++)if(fds[i]>=0){
   shutdown(fds[i],SHUT_RDWR);
   ESP_LOGW(TAG,"CTRL01 cierre cliente fd=%d",fds[i]);
 }
}
static esp_err_t video_stream(httpd_req_t *r);
static esp_err_t video_snapshot(httpd_req_t *r);
static bool stream_reset_begin(void);
/* KORVO_SOCKET01: reset de sesiones HTTP sin destruir/recrear el listener.
 * httpd_stop/httpd_start necesita sockets libres para su canal interno y
 * puede fallar con errno=23 (ENFILE) bajo F5 repetido.
 * El HTTPD conserva el listener y cierra las sesiones al salir el handler.
 */
static void stream_restart_task(void *arg){
 (void)arg;
 /* SOCKET02: logical reset, keep HTTPD and its control socket alive. */
 stream2_enabled=false;
 stream_epoch++;
 stream_reset_count++;
 snapshot_pending=false;
 stream_close_clients();
 ESP_LOGW(TAG,"SOCKET02 RESET epoch=%lu clients=%lu",(unsigned long)stream_epoch,(unsigned long)stream_clients);
 for(int i=0;i<50 && stream_clients;i++)vTaskDelay(pdMS_TO_TICKS(100));
 if(stream_clients){
   ESP_LOGE(TAG,"SOCKET02 reset: clientes retenidos=%lu",(unsigned long)stream_clients);
   stream_restart_result=-1;
 }else{
   stream_admin_disabled=false;
   stream2_enabled=true;
   stream_restart_result=2;
   ESP_LOGW(TAG,"SOCKET02 STREAM2 READY epoch=%lu",(unsigned long)stream_epoch);
 }
 stream_restart_busy=false;
 vTaskDelete(NULL);
}
static bool stream_reset_begin(void){
 bool accepted=false;
 portENTER_CRITICAL(&stream_lock);
 if(!stream_restart_busy){stream_restart_busy=true;stream_restart_result=1;accepted=true;}
 portEXIT_CRITICAL(&stream_lock);
 if(!accepted)return false;
 if(xTaskCreatePinnedToCore(stream_restart_task,"stream2_reset",6144,NULL,5,NULL,1)!=pdPASS){
   stream_restart_busy=false;stream_restart_result=-1;return false;
 }
 return true;
}
static bool stream_dispatch(stream_cmd_t cmd){
 switch(cmd){
 case STREAM_CMD_RESET: return stream_reset_begin();
 case STREAM_CMD_ENABLE:
   if(stream_restart_busy)return false;
   stream_admin_disabled=false;stream2_enabled=true;return true;
 case STREAM_CMD_DISABLE:
   if(stream_restart_busy)return false;
   stream_admin_disabled=true;stream2_enabled=false;stream_epoch++;
   snapshot_pending=false;stream_close_clients();return true;
 default: return false;
 }
}
bool korvo_video01_admin_reset(void){return stream_dispatch(STREAM_CMD_RESET);}
static esp_err_t video_restart(httpd_req_t *r){
 if(!korvo_admin(r)){httpd_resp_send_err(r,HTTPD_401_UNAUTHORIZED,"Inicie sesion");return ESP_FAIL;}
 if(stream_restart_busy){httpd_resp_set_status(r,"409 Conflict");return httpd_resp_sendstr(r,"{\"error\":\"reinicio en curso\"}");}
 if(!stream_dispatch(STREAM_CMD_RESET)){httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"No se pudo iniciar reset");return ESP_FAIL;}
 httpd_resp_set_type(r,"application/json");return httpd_resp_sendstr(r,"{\"started\":true}");
}
static esp_err_t video_restart_status(httpd_req_t *r){
 char b[128];snprintf(b,sizeof(b),"{\"busy\":%s,\"result\":%d,\"clients\":%lu}",stream_restart_busy?"true":"false",stream_restart_result,(unsigned long)stream_clients);
 httpd_resp_set_type(r,"application/json");httpd_resp_set_hdr(r,"Cache-Control","no-store");return httpd_resp_sendstr(r,b);
}

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
        if(!stream2_enabled){pending=false;continue;}
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
        if(!stream2_enabled){pending=false;continue;}
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
    if(!stream2_enabled)return; /* Corte ANTES de cualquier copia/contador STREAM2 */
    callbacks++;input_w=w;input_h=h;input_bytes=len;
    if(!p||!w||!h||len<(size_t)w*h*2U){rejected++;return;}
    int64_t now=esp_timer_get_time();camera_timestamp=now;
    /* Exactly one startup JPEG self-test, then zero JPEG work when idle. */
    if(startup_done && stream_clients==0 && !snapshot_pending)return;
    if(now-last_sample<INTERVAL_US || pending)return;
    if(!buffers_init()){ESP_LOGE(TAG,"PSRAM buffers unavailable");return;}
    if(!encoder_task){
        if(xTaskCreatePinnedToCore(jpeg_worker,"korvo_jpeg",8192,NULL,4,&encoder_task,1)!=pdPASS){ESP_LOGE(TAG,"JPEG worker task failed");return;}
    }
    if(!stream2_enabled || xSemaphoreTake(raw_mutex,0)!=pdTRUE)return;
    if(!stream2_enabled){xSemaphoreGive(raw_mutex);return;}
    /* WEB only: nearest-neighbor 640x480 -> 240x180 directly into RGB565
     * destination buffer. No full-VGA copy; HMI buffer remains untouched. */
    for (uint32_t y=0; y<DST_H; ++y) {
        const uint32_t sy = h - 1U - ((y * h) / DST_H);
        const uint8_t *src = p + (size_t)sy * w * 2U;
        uint8_t *dst = raw_frame + (size_t)y * DST_W * 2U;
        for (uint32_t x=0; x<DST_W; ++x) {
            const uint32_t sx = w - 1U - ((x * w) / DST_W);
            dst[2U*x] = src[2U*sx];
            dst[2U*x+1] = src[2U*sx+1];
        }
    }
    if(!stream2_enabled){xSemaphoreGive(raw_mutex);return;}
    pending=true;last_sample=now;
    snapshot_pending=false;
    xSemaphoreGive(raw_mutex);
    xTaskNotifyGive(encoder_task);
}
bool korvo_video01_ready(void){
    if(!stream2_enabled)return false;
    return startup_ok && camera_timestamp>0 && (esp_timer_get_time()-camera_timestamp)<3000000LL;
}
static esp_err_t video_status(httpd_req_t *r){
    char b[700];
    int64_t age=jpeg_timestamp?(esp_timer_get_time()-jpeg_timestamp)/1000:-1;
    int64_t cam_age=camera_timestamp?(esp_timer_get_time()-camera_timestamp)/1000:-1;
    snprintf(b,sizeof(b),"{\"stream2_enabled\":%s,\"ok\":%s,\"camera\":\"%s\",\"startup_jpeg_test\":\"%s\",\"stream_state\":\"%s\",\"clients\":%lu,\"jpeg_active\":%s,\"format\":\"JPEG\",\"width\":240,\"height\":180,\"fps_target\":10,\"frames\":%lu,\"age_ms\":%lld,\"camera_age_ms\":%lld,\"jpeg_bytes\":%u,\"input_w\":%lu,\"input_h\":%lu,\"input_bytes\":%u,\"callbacks\":%lu,\"rejected\":%lu,\"encode_errors\":%lu,\"stream_port\":8081,\"session_epoch\":%lu,\"reset_count\":%lu,\"reset_busy\":%s}",
      stream2_enabled?"true":"false",korvo_video01_ready()?"true":"false",cam_age>=0&&cam_age<3000?"OK":"DOWN",
      !startup_done?"PENDING":(startup_ok?"OK":"ERROR"),stream_clients?"STREAMING":"READY",
      (unsigned long)stream_clients,stream_clients||snapshot_pending?"true":"false",
      (unsigned long)published,(long long)age,(long long)cam_age,(unsigned)jpeg_len,
      (unsigned long)input_w,(unsigned long)input_h,(unsigned)input_bytes,
      (unsigned long)callbacks,(unsigned long)rejected,(unsigned long)encode_errors,
      (unsigned long)stream_epoch,(unsigned long)stream_reset_count,stream_restart_busy?"true":"false");
    httpd_resp_set_type(r,"application/json");httpd_resp_set_hdr(r,"Cache-Control","no-store");return httpd_resp_sendstr(r,b);
}
/* Control protegido por la misma cookie de sesion del WebServer principal. */
static esp_err_t video_service(httpd_req_t *r){
    char cookie[256]={0};
    if(httpd_req_get_hdr_value_str(r,"Cookie",cookie,sizeof(cookie))!=ESP_OK ||
       strstr(cookie,"KORVO_SESSION=korvo_admin")==NULL){
        httpd_resp_send_err(r,HTTPD_401_UNAUTHORIZED,"Inicie sesion en KORVO");return ESP_FAIL;
    }
    char q[64]={0},v[8]={0};
    if(httpd_req_get_url_query_str(r,q,sizeof(q))!=ESP_OK ||
       httpd_query_key_value(q,"enabled",v,sizeof(v))!=ESP_OK ||
       !((v[0]=='0'||v[0]=='1')&&v[1]=='\0')){
        httpd_resp_send_err(r,HTTPD_400_BAD_REQUEST,"enabled=0 o 1");return ESP_FAIL;
    }
    if(stream_restart_busy){httpd_resp_set_status(r,"409 Conflict");return httpd_resp_sendstr(r,"{\"error\":\"reinicio en curso\"}");}
    if(!stream_dispatch(v[0]=='1'?STREAM_CMD_ENABLE:STREAM_CMD_DISABLE)){
        httpd_resp_set_status(r,"409 Conflict");return httpd_resp_sendstr(r,"STREAM2 ocupado");
    }
    ESP_LOGW(TAG,"STREAM2 diagnostico: %s",stream2_enabled?"ON":"OFF");
    httpd_resp_set_type(r,"application/json");
    httpd_resp_set_hdr(r,"Cache-Control","no-store");
    return httpd_resp_sendstr(r,stream2_enabled?"{\"stream2_enabled\":true}":"{\"stream2_enabled\":false}");
}
static esp_err_t video_view(httpd_req_t *r){
    httpd_resp_set_status(r,"302 Found");
    httpd_resp_set_hdr(r,"Location","/gateway");
    return httpd_resp_sendstr(r,"");
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
    if(!stream2_enabled){httpd_resp_set_status(r,"503 Service Unavailable");httpd_resp_set_type(r,"text/plain");httpd_resp_sendstr(r,"STREAM2 OFF");return ESP_FAIL;}
    /* One-shot JPEG request when stream is idle. */
    uint32_t before=published;
    snapshot_pending=true;
    for(int i=0;i<25 && stream2_enabled && published==before;i++)vTaskDelay(pdMS_TO_TICKS(80));
    if(!stream2_enabled){snapshot_pending=false;httpd_resp_set_status(r,"503 Service Unavailable");httpd_resp_set_type(r,"text/plain");httpd_resp_sendstr(r,"STREAM2 OFF");return ESP_FAIL;}
    if(!jpg_mutex||xSemaphoreTake(jpg_mutex,pdMS_TO_TICKS(200))!=pdTRUE){httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"VIDEO DOWN");return ESP_FAIL;}
    if(!jpeg_len){xSemaphoreGive(jpg_mutex);httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"NO JPEG");return ESP_FAIL;}
    httpd_resp_set_type(r,"image/jpeg");httpd_resp_set_hdr(r,"Cache-Control","no-store");
    esp_err_t e=httpd_resp_send(r,(const char*)jpeg_frame,jpeg_len);xSemaphoreGive(jpg_mutex);return e;
}
/* SOCKET02_ASYNC: each MJPEG response runs outside HTTPD accept loop. */
typedef struct {httpd_req_t *req; int fd; uint32_t epoch;} stream_session_t;
static void stream_async_worker(void *arg){
 stream_session_t *session=(stream_session_t*)arg;
 httpd_req_t *r=session->req;
 const int fd=session->fd;
 const uint32_t my_epoch=session->epoch;
 uint32_t last=published;
 esp_err_t e=ESP_OK;
 int64_t last_frame=esp_timer_get_time();
 while(stream2_enabled && my_epoch==stream_epoch){
   if(published==last){
     vTaskDelay(pdMS_TO_TICKS(80));
     if(esp_timer_get_time()-last_frame>10000000LL){e=ESP_ERR_TIMEOUT;break;}
     continue;
   }
   last_frame=esp_timer_get_time();
   if(!jpg_mutex||xSemaphoreTake(jpg_mutex,pdMS_TO_TICKS(100))!=pdTRUE)continue;
   size_t n=jpeg_len;last=published;
   char header[110];
   int z=snprintf(header,sizeof(header),"\r\n--korvoframe\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",(unsigned)n);
   e=httpd_resp_send_chunk(r,header,z);
   if(e==ESP_OK)e=httpd_resp_send_chunk(r,(const char*)jpeg_frame,n);
   xSemaphoreGive(jpg_mutex);
   if(e!=ESP_OK)break;
 }
 /* Complete async request to release HTTPD-owned socket/session. */
 stream_client_remove(fd);
 httpd_req_async_handler_complete(r);
 ESP_LOGI(TAG,"SOCKET02 MJPEG close fd=%d epoch=%lu err=%s",fd,(unsigned long)my_epoch,esp_err_to_name(e));
 free(session);
 vTaskDelete(NULL);
}
static esp_err_t video_stream(httpd_req_t *r){
 if(!stream2_enabled || stream_restart_busy){
   httpd_resp_set_status(r,"503 Service Unavailable");
   return httpd_resp_sendstr(r,"STREAM2 OFF");
 }
 /* Reserve a slot before transferring ownership to async worker. */
 int fd=httpd_req_to_sockfd(r);
 if(!stream_client_add(fd)){
   httpd_resp_set_status(r,"503 Service Unavailable");
   return httpd_resp_sendstr(r,"Limite de clientes MJPEG");
 }
 httpd_req_t *async_req=NULL;
 esp_err_t e=httpd_req_async_handler_begin(r,&async_req);
 if(e!=ESP_OK){stream_client_remove(fd);return e;}
 stream_session_t *session=calloc(1,sizeof(*session));
 if(!session){stream_client_remove(fd);httpd_req_async_handler_complete(async_req);return ESP_OK;}
 session->req=async_req;session->fd=fd;session->epoch=stream_epoch;
 httpd_resp_set_type(async_req,"multipart/x-mixed-replace;boundary=korvoframe");
 httpd_resp_set_hdr(async_req,"Cache-Control","no-store");
 if(xTaskCreatePinnedToCore(stream_async_worker,"mjpeg_client",6144,session,4,NULL,1)!=pdPASS){
   free(session);stream_client_remove(fd);httpd_req_async_handler_complete(async_req);return ESP_OK;
 }
 ESP_LOGI(TAG,"SOCKET02 MJPEG open fd=%d clients=%lu",fd,(unsigned long)stream_clients);
 return ESP_OK;
}
static esp_err_t video_ai_control(httpd_req_t *r) {
    char q[64]={0}, value[8]={0};
    if (httpd_req_get_url_query_str(r,q,sizeof(q))==ESP_OK &&
        httpd_query_key_value(q,"enabled",value,sizeof(value))==ESP_OK) {
        if (strcmp(value,"0")==0 || strcmp(value,"1")==0) {
            char cookie[256]={0};
            if (httpd_req_get_hdr_value_str(r,"Cookie",cookie,sizeof(cookie))!=ESP_OK ||
                strstr(cookie,"KORVO_SESSION=korvo_admin")==NULL) {
                httpd_resp_send_err(r,HTTPD_401_UNAUTHORIZED,"Inicie sesion");return ESP_FAIL;
            }
            korvo_esp_dl_set_enabled(value[0]=='1');
        } else {httpd_resp_send_err(r,HTTPD_400_BAD_REQUEST,"enabled=0 o 1");return ESP_FAIL;}
    }
    char reply[96];
    snprintf(reply,sizeof(reply),"{\"esp_dl_enabled\":%s}",korvo_esp_dl_enabled()?"true":"false");
    httpd_resp_set_type(r,"application/json");
    httpd_resp_set_hdr(r,"Cache-Control","no-store");
    return httpd_resp_sendstr(r,reply);
}
/* SOCKET_DIAG01: recuento de sockets lwIP mediante SO_TYPE, sin abrir conexiones
 * adicionales para medir. CONFIG_LWIP_MAX_SOCKETS es el limite configurado,
 * no un diagnostico de todos los recursos internos TCP/IP. */
static esp_err_t socket_diag01(httpd_req_t *r){
    unsigned used=0;
#ifdef CONFIG_LWIP_MAX_SOCKETS
    const unsigned total=(unsigned)CONFIG_LWIP_MAX_SOCKETS;
#else
    const unsigned total=0;
#endif
#if defined(LWIP_SOCKET_OFFSET)
    const int offset=LWIP_SOCKET_OFFSET;
#else
    const int offset=0;
#endif
    for(unsigned i=0;i<total;i++){
        int fd=offset+(int)i;
        int type=0;
        socklen_t len=sizeof(type);
        if(getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&len)==0)used++;
    }
    char response[200];
    if(total){
        snprintf(response,sizeof(response),
          "{\"used\":%u,\"free\":%u,\"total\":%u,\"mjpeg_clients\":%lu}",
          used,used<=total?total-used:0,total,(unsigned long)stream_clients);
    }else{
        snprintf(response,sizeof(response),
          "{\"used\":null,\"free\":null,\"total\":null,\"mjpeg_clients\":%lu}",
          (unsigned long)stream_clients);
    }
    httpd_resp_set_type(r,"application/json");
    httpd_resp_set_hdr(r,"Cache-Control","no-store");
    return httpd_resp_sendstr(r,response);
}

/* SOCKET_PROCESS_VIEW01 */
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include <errno.h>

static void spv_addr(const struct sockaddr_storage *ss, char *dst, size_t cap)
{
    dst[0] = 0;
    if (ss->ss_family == AF_INET) {
        const struct sockaddr_in *p = (const struct sockaddr_in *)ss;
        char ip[48] = {0};
        if (inet_ntop(AF_INET, &p->sin_addr, ip, sizeof(ip)))
            snprintf(dst, cap, "%s:%u", ip, (unsigned)ntohs(p->sin_port));
    }
#if CONFIG_LWIP_IPV6
    else if (ss->ss_family == AF_INET6) {
        const struct sockaddr_in6 *p = (const struct sockaddr_in6 *)ss;
        char ip[64] = {0};
        if (inet_ntop(AF_INET6, &p->sin6_addr, ip, sizeof(ip)))
            snprintf(dst, cap, "[%s]:%u", ip, (unsigned)ntohs(p->sin6_port));
    }
#endif
    if (!dst[0]) snprintf(dst, cap, "-");
}
static esp_err_t spv_data(httpd_req_t *r)
{
    /* Enumeracion de descriptores mediante SO_TYPE. No crea ni cierra sockets.
       No confundir fd validos con limite de sockets lwIP. */
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_send_chunk(r, "{\"method\":\"getsockopt(SO_TYPE)\",\"range\":128,\"connections\":[", HTTPD_RESP_USE_STRLEN);
    if (e != ESP_OK) return e;
    int count = 0;
    for (int fd = 0; fd < 128; ++fd) {
        int type = 0;
        socklen_t n = sizeof(type);
        if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &n) != 0) continue;
        struct sockaddr_storage loc = {0}, peer = {0};
        socklen_t ln = sizeof(loc), pn = sizeof(peer);
        char local[96] = "-", remote[96] = "-";
        if (getsockname(fd, (struct sockaddr *)&loc, &ln) == 0) spv_addr(&loc, local, sizeof(local));
        if (getpeername(fd, (struct sockaddr *)&peer, &pn) == 0) spv_addr(&peer, remote, sizeof(remote));
        char row[350];
        snprintf(row, sizeof(row), "%s{\"fd\":%d,\"type\":\"%s\",\"local\":\"%s\",\"remote\":\"%s\"}",
                 count ? "," : "", fd, type == SOCK_STREAM ? "TCP" : type == SOCK_DGRAM ? "UDP" : "OTRO", local, remote);
        e = httpd_resp_send_chunk(r, row, HTTPD_RESP_USE_STRLEN);
        if (e != ESP_OK) return e;
        count++;
    }
    char tail[110];
    snprintf(tail, sizeof(tail), "],\"enumerated\":%d,\"mjpeg_clients\":%lu}", count, (unsigned long)stream_clients);
    e = httpd_resp_send_chunk(r, tail, HTTPD_RESP_USE_STRLEN);
    if (e != ESP_OK) return e;
    return httpd_resp_send_chunk(r, NULL, 0);
}
static esp_err_t spv_page(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_sendstr(r,
        "<!doctype html><html lang='es'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>KORVO SOCKET PROCESS VIEW</title><style>body{font:14px Arial;background:#111820;color:#dce7f2;margin:22px}"
        "table{border-collapse:collapse;width:100%;margin:10px 0}td,th{border-bottom:1px solid #35414d;padding:8px;text-align:left}"
        "th{color:#8fc7ee}code,pre{white-space:pre-wrap;overflow-wrap:anywhere}h2{color:#9cd0fa}"
        "button{padding:8px;margin:4px;background:#25384b;color:white;border:1px solid #6386a0;cursor:pointer}</style></head><body>"
        "<h2>SOCKET PROCESS VIEW - KORVO</h2><p>Enumeracion real: getsockopt(SO_TYPE), lectura pasiva."
        " No identifica el proceso propietario ni todos los estados TCP. La muestra incluye el socket HTTP de esta consulta.</p>"
        "<button onclick='update()'>Actualizar</button><button onclick='window.close()'>Cerrar</button>"
        "<div id='sum'>Consultando...</div><table><thead><tr><th>FD</th><th>Tipo</th><th>Local</th><th>Remoto</th></tr></thead>"
        "<tbody id='rows'></tbody></table><h3>Contador anterior SOCKET-DIAG01 (calculado)</h3><pre id='old'>--</pre>"
        "<p id='err'></p><script>let busy=false;async function update(){if(busy)return;busy=true;try{"
        "let [a,b]=await Promise.all([fetch('/video/socket-process-data',{cache:'no-store'}),fetch('/video/sockets',{cache:'no-store'})]);"
        "if(!a.ok)throw Error('HTTP '+a.status);let d=await a.json();"
        "document.getElementById('sum').textContent='FD verificables: '+d.enumerated+' | Clientes MJPEG: '+d.mjpeg_clients+' | Rango probado: 0..127';"
        "let t=document.getElementById('rows');t.replaceChildren();for(let x of d.connections){let tr=document.createElement('tr');"
        "for(let v of [x.fd,x.type,x.local,x.remote]){let td=document.createElement('td');td.textContent=String(v);tr.appendChild(td)}t.appendChild(tr)}"
        "document.getElementById('old').textContent=b.ok?JSON.stringify(await b.json(),null,2):'HTTP '+b.status;"
        "document.getElementById('err').textContent='';}catch(e){document.getElementById('err').textContent='ERROR: '+e.message}finally{busy=false}}"
        "update();setInterval(update,2000)</script></body></html>");
}

/* KORVO_TELEMETRY02: no tasks, timers or sampling until main completes boot. */
static volatile bool kt02_enabled = false;
void korvo_video01_telemetry_enable(void)
{
    kt02_enabled = true;
    ESP_LOGI(TAG, "TELEMETRY READY - passive HTTP sampling enabled");
}
static esp_err_t kt02_data(httpd_req_t *r)
{
    if (!kt02_enabled) {
        httpd_resp_set_status(r, "503 Service Unavailable");
        httpd_resp_set_type(r, "application/json");
        return httpd_resp_sendstr(r, "{\"status\":\"waiting_for_korvo_boot\"}");
    }
    static uint32_t last_frames=0, last_callbacks=0;
    static int64_t last_us=0;
    const int64_t now=esp_timer_get_time();
    const uint32_t f=published, c=callbacks;
    float fps_jpeg=0, fps_camera=0;
    if (last_us>0 && now>last_us) {
        float dt=(float)(now-last_us)/1000000.0f;
        fps_jpeg=(float)(f-last_frames)/dt;
        fps_camera=(float)(c-last_callbacks)/dt;
    }
    last_frames=f; last_callbacks=c; last_us=now;
    multi_heap_info_t ir={0},pr={0};
    heap_caps_get_info(&ir,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    heap_caps_get_info(&pr,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    char b[1100];
    snprintf(b,sizeof(b),
      "{\"version\":\"TELEMETRY02\",\"status\":\"ready\",\"uptime_ms\":%lld,"
      "\"cpu\":{\"core0_pct\":null,\"core1_pct\":null},"
      "\"memory\":{\"internal\":{\"total\":%u,\"used\":%u,\"free\":%u,\"minimum_free\":%u,\"largest_free\":%u},"
      "\"psram\":{\"total\":%u,\"used\":%u,\"free\":%u,\"minimum_free\":%u,\"largest_free\":%u}},"
      "\"video\":{\"camera_ok\":%s,\"fps_camera_callbacks\":%.2f,\"fps_mjpeg_encoded\":%.2f,"
      "\"fps_hmi\":null,\"mjpeg_clients\":%lu,\"frames\":%lu,\"callbacks\":%lu,"
      "\"rejected\":%lu,\"encode_errors\":%lu},"
      "\"network\":{\"sockets_configured\":%d}}",
      (long long)(now/1000),
      (unsigned)(ir.total_free_bytes+ir.total_allocated_bytes),(unsigned)ir.total_allocated_bytes,
      (unsigned)ir.total_free_bytes,(unsigned)ir.minimum_free_bytes,(unsigned)ir.largest_free_block,
      (unsigned)(pr.total_free_bytes+pr.total_allocated_bytes),(unsigned)pr.total_allocated_bytes,
      (unsigned)pr.total_free_bytes,(unsigned)pr.minimum_free_bytes,(unsigned)pr.largest_free_block,
      korvo_video01_ready()?"true":"false",(double)fps_camera,(double)fps_jpeg,
      (unsigned long)stream_clients,(unsigned long)f,(unsigned long)c,
      (unsigned long)rejected,(unsigned long)encode_errors,CONFIG_LWIP_MAX_SOCKETS);
    httpd_resp_set_type(r,"application/json");
    httpd_resp_set_hdr(r,"Cache-Control","no-store");
    return httpd_resp_sendstr(r,b);
}

void korvo_video01_register(httpd_handle_t server){
    /* Only registration here: handler returns 503 until main signals boot complete. */
    httpd_uri_t kt02={.uri="/api/telemetry",.method=HTTP_GET,.handler=kt02_data};
    esp_err_t kt02_err=httpd_register_uri_handler(server,&kt02);
    ESP_LOGI(TAG,"TELEMETRY INIT - URI registration: %s",esp_err_to_name(kt02_err));
    httpd_uri_t spv={.uri="/video/socket-process",.method=HTTP_GET,.handler=spv_page};
    httpd_uri_t spvd={.uri="/video/socket-process-data",.method=HTTP_GET,.handler=spv_data};
    httpd_register_uri_handler(server,&spv);
    httpd_register_uri_handler(server,&spvd);
    httpd_uri_t ai={.uri="/video/ai",.method=HTTP_GET,.handler=video_ai_control};
    httpd_register_uri_handler(server,&ai);
    httpd_uri_t st={.uri="/video/status",.method=HTTP_GET,.handler=video_status};
    httpd_uri_t vw={.uri="/video/view",.method=HTTP_GET,.handler=video_view};
    httpd_uri_t co={.uri="/video/color",.method=HTTP_GET,.handler=video_color};
    httpd_uri_t svc={.uri="/video/service",.method=HTTP_POST,.handler=video_service};
    httpd_register_uri_handler(server,&st);
    httpd_uri_t sd01={.uri="/video/sockets",.method=HTTP_GET,.handler=socket_diag01};
    httpd_register_uri_handler(server,&sd01);httpd_register_uri_handler(server,&vw);httpd_register_uri_handler(server,&co);
    httpd_register_uri_handler(server,&svc);
    httpd_uri_t reset={.uri="/video/restart",.method=HTTP_POST,.handler=video_restart};
    httpd_uri_t reset_status={.uri="/video/restart/status",.method=HTTP_GET,.handler=video_restart_status};
    httpd_register_uri_handler(server,&reset);httpd_register_uri_handler(server,&reset_status);
    httpd_config_t cfg=HTTPD_DEFAULT_CONFIG();
    cfg.server_port=8081;cfg.ctrl_port=32769;cfg.stack_size=8192;cfg.max_open_sockets=5;cfg.core_id=1;cfg.send_wait_timeout=2;cfg.lru_purge_enable=false;
    if(httpd_start(&stream_server,&cfg)==ESP_OK){
        httpd_uri_t ss={.uri="/video/stream",.method=HTTP_GET,.handler=video_stream};
        httpd_uri_t sn={.uri="/video/snapshot.jpg",.method=HTTP_GET,.handler=video_snapshot};
        httpd_register_uri_handler(stream_server,&ss);httpd_register_uri_handler(stream_server,&sn);
        ESP_LOGI(TAG,"MJPEG ON-DEMAND 240x180 server port 8081");
    }else ESP_LOGE(TAG,"MJPEG server failed on port 8081");
}
