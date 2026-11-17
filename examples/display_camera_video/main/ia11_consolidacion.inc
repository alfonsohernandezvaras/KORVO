/* IA11_CONSOLIDACION01: correlacion de indicios; NO valida vida ni autoriza acceso.
 * Un voto de crominancia IA10 NO es suficiente para clasificar persona.
 * Enfoque: gradiente RGB565 en ROI (solo diagnostico hasta calibracion).
 * Codigos HMI: 0 sin rostro, 1 persona compatible, 2 foto sospechosa, 3 analizando.
 */
static uint8_t ia11_person_streak = 0, ia11_card_streak = 0;
static uint32_t ia11_frame_count = 0;
static int64_t ia11_last_log_us = 0;
static int ia11_last_logged = -1;

static void ia11_reset(void)
{
    ia11_result = 0;
    ia11_person_streak = ia11_card_streak = 0;
}

static void ia11_sample(const uint8_t *buf, uint32_t w, uint32_t h, size_t len)
{
    if (!buf || w < 64 || h < 64 || len < (size_t)w*h*2U) return;
    if ((++ia11_frame_count % 12U) != 0U) return;
    face_detection_t f = {0};
    TickType_t tick = 0;
    if (!face_ai_mutex || xSemaphoreTake(face_ai_mutex, 0) != pdTRUE) return;
    f = ia04_face_roi;
    tick = ia04_roi_tick;
    xSemaphoreGive(face_ai_mutex);
    if (!esp_dl_enabled || !f.detected || !tick ||
        (xTaskGetTickCount()-tick) > pdMS_TO_TICKS(900)) {
        ia11_reset(); return;
    }
    int x1=f.x1, y1=f.y1, x2=f.x2, y2=f.y2;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > (int)w) x2 = (int)w;
    if (y2 > (int)h) y2 = (int)h;
    if(x2-x1<48 || y2-y1<48){ia11_result=3;return;}
    /* Gradiente medio y Laplaciano: diagnostico de nitidez, no umbral de identidad.
       Malla 32x32, 1 KB stack; no buffers nuevos en PSRAM. */
    const uint16_t *rgb=(const uint16_t*)buf;
    uint8_t luma[32*32];
    for(int y=0;y<32;y++)for(int x=0;x<32;x++){
        int sx=x1+(x*2+1)*(x2-x1)/64;
        int sy=y1+(y*2+1)*(y2-y1)/64;
        luma[y*32+x]=ia07_luma565(rgb[(size_t)sy*w+sx]);
    }
    uint32_t grad=0,lap=0;
    for(int y=1;y<31;y++)for(int x=1;x<31;x++){
        int i=y*32+x, c=luma[i];
        grad+=(unsigned)abs((int)luma[i+1]-(int)luma[i-1]);
        grad+=(unsigned)abs((int)luma[i+32]-(int)luma[i-32]);
        lap+=(unsigned)abs(4*c-(int)luma[i-1]-(int)luma[i+1]-(int)luma[i-32]-(int)luma[i+32]);
    }
    unsigned sharp_grad=grad/900U, sharp_lap=lap/900U;
    /* IA07 = evidencia de bordes, IA08 = geometria. IA09 y IA06 no votan:
       sus implementaciones actuales solo emiten diagnosticos no calibrados. */
    int card_votes=(ia07_suspect?2:0)+(ia08_shape_vote<0?2:0);
    int human_votes=(ia08_shape_vote>0?2:0)+(ia10_result==1?1:0);
    bool contradictory=(card_votes>0 && human_votes>0);
    bool card_candidate=card_votes>=2;
    bool human_candidate=human_votes>=3 && card_votes==0;
    if(card_candidate){if(ia11_card_streak<8)ia11_card_streak++;ia11_person_streak=0;}
    else if(human_candidate){if(ia11_person_streak<8)ia11_person_streak++;ia11_card_streak=0;}
    else {if(ia11_card_streak)ia11_card_streak--;if(ia11_person_streak)ia11_person_streak--;}
    int result=3;
    if(ia11_card_streak>=3 && !contradictory)result=2;
    if(ia11_person_streak>=4 && !contradictory)result=1;
    ia11_result=result;
    int64_t us=esp_timer_get_time();
    if(result!=ia11_last_logged || us-ia11_last_log_us>=1000000LL){
        ia11_last_log_us=us;ia11_last_logged=result;
        ESP_LOGI("IA11_FUSION","ia07=%u ia08=%d ia10=%d votes_person=%d votes_card=%d contradiction=%u streak_person=%u streak_card=%u focus_grad=%u focus_lap=%u roi=%dx%d result=%d (EXPERIMENTAL, NO AUTORIZA)",
            (unsigned)ia07_suspect,(int)ia08_shape_vote,(int)ia10_result,
            human_votes,card_votes,(unsigned)contradictory,
            (unsigned)ia11_person_streak,(unsigned)ia11_card_streak,
            sharp_grad,sharp_lap,x2-x1,y2-y1,result);
    }
}
