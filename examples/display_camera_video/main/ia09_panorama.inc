/* IA09_PANORAMA01: una referencia RAM fija; diagnostico solamente. */
#define IA09_W 80
#define IA09_H 60
#define IA09_N (IA09_W * IA09_H)
static uint8_t ia09_reference[IA09_N];
static uint8_t ia09_mask[IA09_N];
static int ia09_silhouette_match = -1; /* -1 = no disponible */
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
    bool snapshot_ok = false;
    if (face_ai_mutex && xSemaphoreTake(face_ai_mutex, 0) == pdTRUE) {
        snapshot_ok = true;
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
    /* IA09_FIX01: movimiento y cambios de escena no bloquean la referencia. */
    const bool recently_face = ia09_last_face_us && us - ia09_last_face_us < 5000000LL;
    const bool busy = face || recently_face || !snapshot_ok;
    if (busy) ia09_frozen = true;

    /* Rearmar referencia sin rostro reciente y con snapshot disponible. No hay
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
    ia09_silhouette_match = -1;
    const bool log_now = us - ia09_last_log_us >= 1000000LL;
    if (log_now) ia09_last_log_us = us;
    if (!ia09_ready) {
        if (log_now) ESP_LOGI("IA09_BG", "SIN_REFERENCIA face=%u motion=%u (NO AUTORIZA)",
                 (unsigned)face, (unsigned)motion_active);
        return;
    }
    if (!face) {
        if (log_now) ESP_LOGI("IA09_BG", "state=%s scene=%lu%% age=%llds face=0 (NO AUTORIZA)",
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
    /* Patron geometrico normalizado al ROI facial: cabeza, cuello, hombros.
       IoU sobre region visible; no es una decision de autenticidad. */
    const int fw = x2-x1+1, fh = y2-y1+1;
    const int cx2 = x1+x2;
    uint32_t intersection = 0, total = 0, expected = 0;
    for (int y = 0; y < IA09_H; ++y) {
        const int v = (y-y1)*100/fh;
        if (v < -20 || v > 155) continue;
        int half = 0;
        if (v < 0) half = 35 + (v+20)*20/20;
        else if (v < 75) half = 60;
        else if (v < 100) half = 60-(v-75)*25/25;
        else if (v < 115) half = 30;
        else half = 45+(v-115)*65/40;
        for (int x = 0; x < IA09_W; ++x) {
            const int u = abs(2*x-cx2)*50/fw;
            if (u > 125) continue;
            const bool pattern = u <= half;
            const bool observed = ia09_mask[y*IA09_W+x] != 0;
            expected += pattern;
            intersection += pattern && observed;
            total += pattern || observed;
        }
    }
    if (expected && total) ia09_silhouette_match = (int)(100U*intersection/total);
    if (log_now) ESP_LOGI("IA09", "SILUETA_MATCH=%d%% scene=%lu%% bg_age=%llds (SOLO DATO)",
        ia09_silhouette_match, (unsigned long)scene,
        (long long)((us-ia09_last_bg_us)/1000000LL));
}
