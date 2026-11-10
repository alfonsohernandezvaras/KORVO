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
#include "esp_err.h"
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
static volatile bool stream_restart_busy=false;
static volatile int stream_restart_result=0; /* 0 idle,1 ejecutando,2 OK,-1 error */
static portMUX_TYPE stream_lock=portMUX_INITIALIZER_UNLOCKED;
static int stream_fds[4]={-1,-1,-1,-1};
static bool stream_client_add(int fd){
 bool ok=false;
 portENTER_CRITICAL(&stream_lock);
 for(int i=0;i<4;i++)if(stream_fds[i]==-1){stream_fds[i]=fd;stream_clients++;ok=true;break;}
 portEXIT_CRITICAL(&stream_lock);return ok;
}
static void stream_client_remove(int fd){
 portENTER_CRITICAL(&stream_lock);
 for(int i=0;i<4;i++)if(stream_fds[i]==fd){stream_fds[i]=-1;if(stream_clients)stream_clients--;break;}
 portEXIT_CRITICAL(&stream_lock);
}
static bool korvo_admin(httpd_req_t *r){
 char cookie[256]={0};return httpd_req_get_hdr_value_str(r,"Cookie",cookie,sizeof(cookie))==ESP_OK && strstr(cookie,"KORVO_SESSION=korvo_admin")!=NULL;
}
static void stream_close_clients(void){
 int fds[4];portENTER_CRITICAL(&stream_lock);
 for(int i=0;i<4;i++)fds[i]=stream_fds[i];
 portEXIT_CRITICAL(&stream_lock);
 for(int i=0;i<4;i++)if(fds[i]>=0)shutdown(fds[i],SHUT_RDWR);
}
static esp_err_t video_stream(httpd_req_t *r);
static esp_err_t video_snapshot(httpd_req_t *r);
static void stream_restart_task(void *arg){
 (void)arg;
 stream2_enabled=false;
 stream_epoch++;
 snapshot_pending=false;pending=false;
 stream_close_clients();
 /* Los handlers deben salir antes de parar HTTPD. */
 for(int i=0;i<30 && stream_clients;i++)vTaskDelay(pdMS_TO_TICKS(100));
 if(stream_clients){
   ESP_LOGE(TAG,"DIAG01 restart: clientes no liberados (%lu)",(unsigned long)stream_clients);
   stream_restart_result=-1;stream_restart_busy=false;vTaskDelete(NULL);return;
 }
 if(stream_server){httpd_stop(stream_server);stream_server=NULL;}
 vTaskDelay(pdMS_TO_TICKS(250));
 httpd_config_t cfg=HTTPD_DEFAULT_CONFIG();
 cfg.server_port=8081;cfg.ctrl_port=32769;cfg.stack_size=8192;cfg.max_open_sockets=3;cfg.core_id=1;cfg.send_wait_timeout=2;
 esp_err_t e=httpd_start(&stream_server,&cfg);
 if(e==ESP_OK){
   httpd_uri_t ss={.uri="/video/stream",.method=HTTP_GET,.handler=video_stream};
   httpd_uri_t sn={.uri="/video/snapshot.jpg",.method=HTTP_GET,.handler=video_snapshot};
   if(httpd_register_uri_handler(stream_server,&ss)!=ESP_OK || httpd_register_uri_handler(stream_server,&sn)!=ESP_OK){
     httpd_stop(stream_server);stream_server=NULL;e=ESP_FAIL;
   }
 }
 if(e==ESP_OK){stream2_enabled=true;stream_restart_result=2;ESP_LOGW(TAG,"DIAG01 STREAM2 reiniciado");}
 else {stream_restart_result=-1;ESP_LOGE(TAG,"DIAG01 STREAM2 restart error: %s",esp_err_to_name(e));}
 stream_restart_busy=false;vTaskDelete(NULL);
}
static esp_err_t video_restart(httpd_req_t *r){
 if(!korvo_admin(r)){httpd_resp_send_err(r,HTTPD_401_UNAUTHORIZED,"Inicie sesion");return ESP_FAIL;}
 if(stream_restart_busy){httpd_resp_set_status(r,"409 Conflict");return httpd_resp_sendstr(r,"{\"error\":\"reinicio en curso\"}");}
 stream_restart_busy=true;stream_restart_result=1;
 if(xTaskCreatePinnedToCore(stream_restart_task,"stream2_reset",6144,NULL,5,NULL,1)!=pdPASS){stream_restart_busy=false;stream_restart_result=-1;httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"No se pudo crear tarea");return ESP_FAIL;}
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
    char b[560];
    int64_t age=jpeg_timestamp?(esp_timer_get_time()-jpeg_timestamp)/1000:-1;
    int64_t cam_age=camera_timestamp?(esp_timer_get_time()-camera_timestamp)/1000:-1;
    snprintf(b,sizeof(b),"{\"stream2_enabled\":%s,\"ok\":%s,\"camera\":\"%s\",\"startup_jpeg_test\":\"%s\",\"stream_state\":\"%s\",\"clients\":%lu,\"jpeg_active\":%s,\"format\":\"JPEG\",\"width\":240,\"height\":180,\"fps_target\":10,\"frames\":%lu,\"age_ms\":%lld,\"camera_age_ms\":%lld,\"jpeg_bytes\":%u,\"input_w\":%lu,\"input_h\":%lu,\"input_bytes\":%u,\"callbacks\":%lu,\"rejected\":%lu,\"encode_errors\":%lu,\"stream_port\":8081}",
      stream2_enabled?"true":"false",korvo_video01_ready()?"true":"false",cam_age>=0&&cam_age<3000?"OK":"DOWN",
      !startup_done?"PENDING":(startup_ok?"OK":"ERROR"),stream_clients?"STREAMING":"READY",
      (unsigned long)stream_clients,stream_clients||snapshot_pending?"true":"false",
      (unsigned long)published,(long long)age,(long long)cam_age,(unsigned)jpeg_len,
      (unsigned long)input_w,(unsigned long)input_h,(unsigned)input_bytes,
      (unsigned long)callbacks,(unsigned long)rejected,(unsigned long)encode_errors);
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
    stream2_enabled=(v[0]=='1');
    if(!stream2_enabled){snapshot_pending=false;pending=false;}
    ESP_LOGW(TAG,"STREAM2 diagnostico: %s",stream2_enabled?"ON":"OFF");
    httpd_resp_set_type(r,"application/json");
    httpd_resp_set_hdr(r,"Cache-Control","no-store");
    return httpd_resp_sendstr(r,stream2_enabled?"{\"stream2_enabled\":true}":"{\"stream2_enabled\":false}");
}
static esp_err_t video_view(httpd_req_t *r){
    static const char page[]=
      "<!doctype html><html lang='es'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>KORVO VIDEO-08</title><style>body{background:#101820;color:#eee;font:16px Arial;text-align:center}"
      "img{display:block;width:240px;max-width:95vw;height:auto;margin:14px auto;border:2px solid #268ad1}"
      "pre{display:inline-block;text-align:left;color:#8ce0ad;font-size:13px}#msg{color:#ffd28b}</style></head>"
      "<body><h2>KORVO - VIDEO IP</h2><p>Streaming 240x180 / JPEG / objetivo 10 FPS / ON-DEMAND</p>"
      "<p id='msg'>Conectando MJPEG...</p><img id='v' alt='VIDEO DOWN'>"
      "<p><button onclick='snapshot()'>Ver fotografia</button> <button onclick='stream()'>Ver streaming</button>"
      " <button onclick='stop()'>STOP streaming</button></p>"
      "<p>Servicio STREAM2: <button onclick='service(1)'>HABILITAR</button> "
      "<button onclick='service(0)'>DESHABILITAR</button> <button onclick='restartStream()'>REINICIAR STREAM2</button></p><p><button onclick='aiSet(1)'>ACTIVAR IA ESP-DL</button> <button onclick='aiSet(0)'>DESACTIVAR IA ESP-DL</button></p><p id='ai_state'>IA ESP-DL: consultando...</p><pre id='s'>Conectando...</pre>"
      "<script>function aiRefresh(){fetch('/video/ai',{cache:'no-store'})"
      ".then(r=>r.json()).then(d=>{document.getElementById('ai_state').textContent='IA ESP-DL: '+(d.esp_dl_enabled?'ON':'OFF')}).catch(()=>{});}"
      "function aiSet(v){fetch('/video/ai?enabled='+v,{cache:'no-store',credentials:'same-origin'})"
      ".then(r=>{if(!r.ok)throw Error(r.status);return r.json()}).then(()=>aiRefresh()).catch(e=>{document.getElementById('ai_state').textContent='ERROR IA: '+e.message});}"
      "setInterval(aiRefresh,2000);aiRefresh();"
      "const u=location.protocol+'//'+location.hostname+':8081';"
      "const im=document.getElementById('v'),msg=document.getElementById('msg');"
      "function stop(){im.removeAttribute('src');msg.textContent='Streaming detenido';}"
      "function service(n){if(!n)stop();fetch('/video/service?enabled='+n,{method:'POST',credentials:'same-origin'})"
      ".then(r=>{if(!r.ok)throw Error('HTTP '+r.status);return r.json()})"
      ".then(d=>{msg.textContent='STREAM2 '+(d.stream2_enabled?'HABILITADO':'DESHABILITADO')})"
      ".catch(e=>{msg.textContent='ERROR control STREAM2: '+e.message})}"
      "function restartStream(){stop();if(!confirm('Reiniciar STREAM2 y desconectar clientes?'))return;msg.textContent='Reiniciando STREAM2...';"
      "fetch('/video/restart',{method:'POST',credentials:'same-origin'}).then(r=>{if(!r.ok)throw Error(r.status);return r.json()})"
      ".then(()=>{let n=0;let timer=setInterval(()=>fetch('/video/restart/status',{cache:'no-store'}).then(r=>r.json()).then(d=>{if(d.busy&&n++<30)return;clearInterval(timer);if(d.result===2){msg.textContent='STREAM2 reiniciado';stream()}else msg.textContent='ERROR reinicio STREAM2 (ver consola)';}).catch(()=>{clearInterval(timer);msg.textContent='ERROR consultando reinicio'}),500)})"
      ".catch(e=>msg.textContent='ERROR: '+e.message)}"
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
static esp_err_t video_stream(httpd_req_t *r){
    if(!stream2_enabled){httpd_resp_set_status(r,"503 Service Unavailable");httpd_resp_set_type(r,"text/plain");httpd_resp_sendstr(r,"STREAM2 OFF");return ESP_FAIL;}
    int fd=httpd_req_to_sockfd(r);
    if(!stream_client_add(fd)){httpd_resp_set_status(r,"503 Service Unavailable");httpd_resp_set_type(r,"text/plain");httpd_resp_sendstr(r,"Limite de clientes");return ESP_FAIL;}
    const uint32_t my_epoch=stream_epoch;
    httpd_resp_set_type(r,"multipart/x-mixed-replace;boundary=korvoframe");
    httpd_resp_set_hdr(r,"Cache-Control","no-store");
    uint32_t last=published;
    esp_err_t e=ESP_OK;
    int64_t start=esp_timer_get_time();
    while(stream2_enabled && my_epoch==stream_epoch){
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
    stream_client_remove(fd);
    return e;
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
void korvo_video01_register(httpd_handle_t server){
    httpd_uri_t ai={.uri="/video/ai",.method=HTTP_GET,.handler=video_ai_control};
    httpd_register_uri_handler(server,&ai);
    httpd_uri_t st={.uri="/video/status",.method=HTTP_GET,.handler=video_status};
    httpd_uri_t vw={.uri="/video/view",.method=HTTP_GET,.handler=video_view};
    httpd_uri_t co={.uri="/video/color",.method=HTTP_GET,.handler=video_color};
    httpd_uri_t svc={.uri="/video/service",.method=HTTP_POST,.handler=video_service};
    httpd_register_uri_handler(server,&st);httpd_register_uri_handler(server,&vw);httpd_register_uri_handler(server,&co);
    httpd_register_uri_handler(server,&svc);
    httpd_uri_t reset={.uri="/video/restart",.method=HTTP_POST,.handler=video_restart};
    httpd_uri_t reset_status={.uri="/video/restart/status",.method=HTTP_GET,.handler=video_restart_status};
    httpd_register_uri_handler(server,&reset);httpd_register_uri_handler(server,&reset_status);
    httpd_config_t cfg=HTTPD_DEFAULT_CONFIG();
    cfg.server_port=8081;cfg.ctrl_port=32769;cfg.stack_size=8192;cfg.max_open_sockets=3;cfg.core_id=1;cfg.send_wait_timeout=2;
    if(httpd_start(&stream_server,&cfg)==ESP_OK){
        httpd_uri_t ss={.uri="/video/stream",.method=HTTP_GET,.handler=video_stream};
        httpd_uri_t sn={.uri="/video/snapshot.jpg",.method=HTTP_GET,.handler=video_snapshot};
        httpd_register_uri_handler(stream_server,&ss);httpd_register_uri_handler(stream_server,&sn);
        ESP_LOGI(TAG,"MJPEG ON-DEMAND 240x180 server port 8081");
    }else ESP_LOGE(TAG,"MJPEG server failed on port 8081");
}
