/* IA12_CIERRE01: decision experimental in five captures; no access authorization.
 * 0 no face, 1 human-compatible, 2 credential-suspected, 3 analysing,
 * 4..8 capture progress 1..5. Final decision persists until face leaves.
 */
static uint32_t ia11_frame_count = 0;
static int64_t ia11_last_log_us = 0;
static void ia11_reset(void)
{
    int64_t now = esp_timer_get_time();
    if (ia12_face_active && now - ia12_last_face_us < 2000000LL) return;
    if (ia12_queue && uxQueueMessagesWaiting(ia12_queue) != 0) return;
    if (ia12_captured > 0 && ia12_saved + ia12_failed < ia12_captured) return;
    ia12_face_active = false;
    ia12_captured = ia12_saved = ia12_failed = 0;
    ia12_person_evidence = ia12_card_evidence = 0;
    ia12_finalized = false;
    ia12_last_capture_us = 0;
    ia11_result = 0;
}
static void ia11_sample(const uint8_t *buf, uint32_t w, uint32_t h, size_t len)
{
    if (!buf || w < 64 || h < 64 || len < (size_t)w*h*2U) return;
    if ((++ia11_frame_count % 4U) != 0U) return;
    face_detection_t f = {0};
    TickType_t tick = 0;
    if (!face_ai_mutex || xSemaphoreTake(face_ai_mutex, 0) != pdTRUE) return;
    f = ia04_face_roi; tick = ia04_roi_tick;
    xSemaphoreGive(face_ai_mutex);
    int64_t now = esp_timer_get_time();
    if (!esp_dl_enabled || !f.detected || !tick ||
        (xTaskGetTickCount()-tick) > pdMS_TO_TICKS(900)) {
        ia11_reset(); return;
    }
    int x1=f.x1, y1=f.y1, x2=f.x2, y2=f.y2;
    if (x1 < 0) x1=0;
    if (y1 < 0) y1=0;
    if (x2 > (int)w) x2=(int)w;
    if (y2 > (int)h) y2=(int)h;
    if (x2-x1<48 || y2-y1<48) { ia11_result=3; return; }
    ia12_face_active = true;
    ia12_last_face_us = now;
    if (ia12_finalized) return;
    if (ia12_captured < 5) {
        bool fresh = ia10_evidence_us && now - ia10_evidence_us <= 1200000LL;
        if (!fresh || ia10_result != 1) {
            ia11_result = fresh && ia10_result == 2 ? 2 : 3;
            return;
        }
        uint8_t before = ia12_captured;
        ia12_capture(buf, w, h, len, x1, y1, x2, y2);
        if (ia12_captured != before) {
            ia12_person_evidence++;
            ia11_result = 3 + ia12_captured;
            ESP_LOGI("IA12", "CAPTURE %u/5 IA10=%d PERSON=%u (VALIDACION TEMPORAL EXPERIMENTAL)",
                (unsigned)ia12_captured, (int)ia10_result, (unsigned)ia12_person_evidence);
        }
        return;
    }
    if (ia12_saved + ia12_failed < 5) return;
    /* Forced experimental decision. Tie goes to credential for conservative display. */
    ia11_result = (ia12_saved == 5 && ia12_failed == 0) ? 1 : 3;
    ia12_finalized = true;
    ESP_LOGW("IA12", "FINAL %s person=%u card=%u sd_saved=%u sd_failed=%u (EXPERIMENTAL, SIN ACCESO)",
        ia11_result==1 ? "ROSTRO_HUMANO_DETECTADO" : "CAPTURA_INCOMPLETA",
        (unsigned)ia12_person_evidence, (unsigned)ia12_card_evidence,
        (unsigned)ia12_saved, (unsigned)ia12_failed);
    ia11_last_log_us = now;
}
