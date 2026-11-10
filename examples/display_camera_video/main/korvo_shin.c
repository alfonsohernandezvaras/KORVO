#include "korvo_shin.h"
#include "korvo_storage.h"
#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <sys/stat.h>
#include <errno.h>

#define RATE 16000
#define SAMPLES 160
#define WAV_PATH BSP_SD_MOUNT_POINT "/audio/shin.wav"
static const char *TAG="korvo_shin";
static volatile bool s_busy=false;
static esp_codec_dev_handle_t s_speaker=NULL; /* Reuse one ES8389 device */
static void put16(uint8_t *p, uint16_t n){p[0]=n&255;p[1]=n>>8;}
static void put32(uint8_t *p, uint32_t n){for(int i=0;i<4;i++)p[i]=(uint8_t)(n>>(i*8));}

static esp_err_t ensure_wav(void){
    if(!korvo_storage_is_ready()) return ESP_ERR_INVALID_STATE;
    FILE *f=fopen(WAV_PATH,"rb"); if(f){fclose(f);return ESP_OK;}
    if(mkdir(BSP_SD_MOUNT_POINT "/audio",0775)!=0 && errno!=EEXIST) return ESP_FAIL;
    f=fopen(WAV_PATH,"wb"); if(!f)return ESP_FAIL;
    /* 1.4 s dual-tone notification, PCM mono 16 kHz 16-bit. */
    const uint32_t count=RATE*14/10, bytes=count*2;
    uint8_t h[44]={0}; memcpy(h,"RIFF",4);put32(h+4,36+bytes);memcpy(h+8,"WAVEfmt ",8);
    put32(h+16,16);put16(h+20,1);put16(h+22,1);put32(h+24,RATE);put32(h+28,RATE*2);
    put16(h+32,2);put16(h+34,16);memcpy(h+36,"data",4);put32(h+40,bytes);
    if(fwrite(h,1,44,f)!=44){fclose(f);remove(WAV_PATH);return ESP_FAIL;}
    int16_t pcm[SAMPLES];
    for(uint32_t off=0;off<count;off+=SAMPLES){
        uint32_t n=count-off<SAMPLES?count-off:SAMPLES;
        for(uint32_t i=0;i<n;i++){
            uint32_t t=off+i;
            double sec=(double)t/RATE;
            double hz=sec<0.7?660.0:880.0;
            double envelope=sec<0.04?sec/0.04:(sec>1.35?(1.4-sec)/0.05:1.0);
            if(envelope<0)envelope=0;
            pcm[i]=(int16_t)(sin(6.283185307179586*hz*sec)*9000.0*envelope);
        }
        if(fwrite(pcm,2,n,f)!=n){fclose(f);remove(WAV_PATH);return ESP_FAIL;}
    }
    fclose(f);ESP_LOGI(TAG,"Created %s",WAV_PATH);return ESP_OK;
}

static void task(void *unused){
    (void)unused;
    esp_err_t err=ensure_wav();
    if(err!=ESP_OK){ESP_LOGE(TAG,"SD/WAV unavailable: %s",esp_err_to_name(err));goto done;}
    FILE *f=fopen(WAV_PATH,"rb");if(!f){ESP_LOGE(TAG,"Cannot open WAV");goto done;}
    uint8_t hdr[44];
    if(fread(hdr,1,44,f)!=44||memcmp(hdr,"RIFF",4)||memcmp(hdr+8,"WAVE",4)||
       memcmp(hdr+12,"fmt ",4)||memcmp(hdr+36,"data",4)||
       hdr[20]!=1||hdr[21]!=0||hdr[22]!=1||hdr[23]!=0||
       (uint32_t)(hdr[24]|(hdr[25]<<8)|(hdr[26]<<16)|(hdr[27]<<24))!=RATE||
       hdr[34]!=16||hdr[35]!=0){
       ESP_LOGE(TAG,"shin.wav must be standard PCM mono 16 kHz 16 bit with 44-byte header");fclose(f);goto done;
    }
    ESP_LOGI(TAG,"Opening native ES8389 playback");
    if (!s_speaker) {
        ESP_LOGI(TAG,"Creating ES8389 speaker once");
        s_speaker=bsp_audio_codec_speaker_init();
    }
    esp_codec_dev_handle_t spk=s_speaker;
    if(!spk){ESP_LOGE(TAG,"ES8389 speaker init failed");fclose(f);goto done;}
    esp_codec_dev_sample_info_t info={.sample_rate=RATE,.channel=1,.bits_per_sample=16};
    ESP_LOGI(TAG,"Opening ES8389 at %d Hz mono",RATE);
    err=esp_codec_dev_open(spk,&info);
    if(err!=ESP_OK){ESP_LOGE(TAG,"ES8389 open: %s",esp_err_to_name(err));fclose(f);goto done;}
    ESP_LOGI(TAG,"ES8389 open OK");
    (void)esp_codec_dev_set_out_vol(spk,25);
    int16_t pcm[SAMPLES];size_t n;
    while((n=fread(pcm,1,sizeof(pcm),f))>0){
        err=esp_codec_dev_write(spk,pcm,(int)n);
        if(err!=ESP_OK){ESP_LOGE(TAG,"ES8389 write: %s",esp_err_to_name(err));break;}
    }
    (void)esp_codec_dev_close(spk);fclose(f);
    ESP_LOGI(TAG,"shin playback finished: %s",esp_err_to_name(err));
done:
    s_busy=false;vTaskDelete(NULL);
}

esp_err_t korvo_shin_play_async(void){
    if(s_busy)return ESP_ERR_INVALID_STATE;
    s_busy=true;
    if(xTaskCreate(task,"korvo_shin",8192,NULL,5,NULL)!=pdPASS){s_busy=false;return ESP_ERR_NO_MEM;}
    return ESP_OK;
}
