/* IA09_PANORAMA01: una referencia RAM fija; diagnostico solamente. */
#define IA09_W 80
#define IA09_H 60
#define IA09_N (IA09_W * IA09_H)
static uint8_t ia09_reference[IA09_N];
static uint8_t ia09_mask[IA09_N];
static bool ia09_ready = false;
static bool ia09_frozen = false;
static uint32_t ia09_frames = 0;
static int64_t ia09_last_bg_us = 0;
static int64_t ia09_last_face_us = 0;
static int64_t ia09_last_log_us = 0;
static uint32_t ia09_snapshots = 0;

static void ia09_panorama_sample(const uint8_t *buf, uint32_t w, uint32_t h, size_t len)
{
    if (!buf || !w || !h || len < (size_t)w * h * 2U) return;
    if ((++ia09_frames % 12U) != 0U) return;
    const int64_t us = esp_timer_get_time();
    const uint16_t *rgb = (const uint16_t *)buf;
    face_detection_t f = {0};
    TickType_t tick = 0;
    if (face_ai_mutex && xSemaphoreTake(face_ai_mutex, 0) == pdTRUE) {
        f = ia04_face_roi;
        tick = ia04_roi_tick;
        xSemaphoreGive(face_ai_mutex);
    }
    const bool face = esp_dl_enabled && f.detected && tick &&
                      (xTaskGetTickCount() - tick) <= pdMS_TO_TICKS(900);
    if (face) ia09_last_face_us = us;

    /* Detectar cambios ANTES de reemplazar el fondo; sin buffers de imagen adicionales. */
    uint32_t changed = 0;
    for (int y = 0; y < IA09_H; ++y) {
        const uint32_t sy = (uint32_t)y * h / IA09_H;
        for (int x = 0; x < IA09_W; ++x) {
            const uint32_t sx = (uint32_t)x * w / IA09_W;
            const size_t i = (size_t)y * IA09_W + x;
            const uint8_t v = ia07_luma565(rgb[(size_t)sy * w + sx]);
            const uint8_t active = ia09_ready && abs((int)v - (int)ia09_reference[i]) >= 28;
            ia09_mask[i] = active;
            changed += active;
        }
    }
    const uint32_t scene = changed * 100U / IA09_N;
    const bool changed_scene = ia09_ready && scene >= 8U;
    const bool recently_face = ia09_last_face_us && us - ia09_last_face_us < 5000000LL;
    const bool busy = face || recently_face || motion_active || changed_scene;
    if (busy) ia09_frozen = true;

    /* Solo rearmar referencia si no hay rostro ni cambios relevantes. No hay
       escritura SD; una sola imagen se sobrescribe cada diez segundos. */
    if (!busy && (!ia09_ready || us - ia09_last_bg_us >= 10000000LL)) {
        for (int y = 0; y < IA09_H; ++y) {
            const uint32_t sy = (uint32_t)y * h / IA09_H;
            for (int x = 0; x < IA09_W; ++x) {
                const uint32_t sx = (uint32_t)x * w / IA09_W;
                ia09_reference[y * IA09_W + x] = ia07_luma565(rgb[(size_t)sy * w + sx]);
            }
        }
        ia09_ready = true;
        ia09_frozen = false;
        ia09_last_bg_us = us;
        ++ia09_snapshots;
        ESP_LOGI("IA09_BG", "CAPTURA_OK samples=%lu interval=10s bytes=%u (RAM, SIN SD)",
                 (unsigned long)ia09_snapshots, (unsigned)sizeof(ia09_reference));
    }
    if (us - ia09_last_log_us < 1000000LL) return;
    ia09_last_log_us = us;
    if (!ia09_ready) {
        ESP_LOGI("IA09_BG", "SIN_REFERENCIA face=%u motion=%u (NO AUTORIZA)",
                 (unsigned)face, (unsigned)motion_active);
        return;
    }
    if (!face) {
        ESP_LOGI("IA09_BG", "state=%s scene=%lu%% age=%llds face=0 (NO AUTORIZA)",
                 ia09_frozen ? "CONGELADA" : "LISTA", (unsigned long)scene,
                 (long long)((us - ia09_last_bg_us) / 1000000LL));
        return;
    }
    int x1 = f.x1 * IA09_W / (int)w, x2 = f.x2 * IA09_W / (int)w;
    int y1 = f.y1 * IA09_H / (int)h, y2 = f.y2 * IA09_H / (int)h;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= IA09_W) x2 = IA09_W - 1;
    if (y2 >= IA09_H) y2 = IA09_H - 1;
    if (x2 <= x1 || y2 <= y1) return;
    uint32_t outside = 0, outside_changed = 0;
    uint32_t head = 0, lower = 0;
    const int head_y = y1 + (y2-y1)/3;
    int lower_y = y2 + (y2-y1)/3;
    if (lower_y >= IA09_H) lower_y = IA09_H - 1;
    for (int y = 0; y < IA09_H; ++y) for (int x = 0; x < IA09_W; ++x) {
        const uint8_t a = ia09_mask[y*IA09_W+x];
        if (x < x1 || x > x2 || y < y1 || y > y2) {
            ++outside; outside_changed += a;
        }
    }
    for (int x = 0; x < IA09_W; ++x) {
        head += ia09_mask[head_y*IA09_W+x];
        lower += ia09_mask[lower_y*IA09_W+x];
    }
    const uint32_t outside_pct = outside ? outside_changed*100U/outside : 0;
    const uint32_t ratio = head ? lower*100U/head : 0;
    const bool clipped = x1<=1 || x2>=IA09_W-2 || y1<=1 || y2>=IA09_H-2 || lower_y>=IA09_H-2;
    /* IA08 dio falsos positivos: no transformar heuristicas no calibradas en
       una opinion de credencial/persona. Exponer metricas para pruebas reales. */
    ESP_LOGI("IA09_FUSION", "face_detected=1 scene=%lu%% outside=%lu%% head=%lu lower=%lu ratio=%lu%% clipped=%u bg_age=%llds result=NO_CONCLUYENTE (EXPERIMENTAL, NO AUTORIZA)",
             (unsigned long)scene, (unsigned long)outside_pct,
             (unsigned long)head, (unsigned long)lower, (unsigned long)ratio,
             (unsigned)clipped, (long long)((us-ia09_last_bg_us)/1000000LL));
}
