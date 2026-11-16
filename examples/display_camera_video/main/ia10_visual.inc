/* IA10_VISUAL01: diagnostico RGB565 2D, sin autorizacion de acceso.
 * Consumidor: callback de camara. ESP-DL aporta solamente el ROI.
 * 0 sin evidencia, 1 persona compatible, 2 foto sospechosa, 3 analizando.
 */
/* ia10_result se declara antes de face task en main.c */
static uint8_t ia10_prev[12 * 12];
static bool ia10_prev_ok = false;
static int ia10_prev_cx = 0, ia10_prev_cy = 0;
static uint32_t ia10_frame_count = 0;
static int64_t ia10_last_us = 0;
static uint8_t ia10_person_hits = 0, ia10_photo_hits = 0;
static int ia10_last_log_state = -1;

static void ia10_visual_sample(const uint8_t *buf, uint32_t w, uint32_t h, size_t len)
{
    if (!buf || w < 64 || h < 64 || len < (size_t)w * h * 2U) return;
    if ((++ia10_frame_count % 5U) != 0U) return;
    int64_t us = esp_timer_get_time();
    if (us - ia10_last_us < 180000LL) return;
    ia10_last_us = us;
    face_detection_t f = {0};
    TickType_t tick = 0;
    if (!face_ai_mutex || xSemaphoreTake(face_ai_mutex, 0) != pdTRUE) return;
    f = ia04_face_roi;
    tick = ia04_roi_tick;
    xSemaphoreGive(face_ai_mutex);
    if (!esp_dl_enabled || !f.detected || !tick ||
        (xTaskGetTickCount() - tick) > pdMS_TO_TICKS(900)) {
        ia10_prev_ok = false;
        ia10_person_hits = ia10_photo_hits = 0;
        ia10_result = 0;
        return;
    }
    int x1 = f.x1, y1 = f.y1, x2 = f.x2, y2 = f.y2;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > (int)w) x2 = (int)w;
    if (y2 > (int)h) y2 = (int)h;
    if (x2 - x1 < 32 || y2 - y1 < 32) { ia10_result = 3; return; }
    const uint16_t *p = (const uint16_t *)buf;
    uint8_t current[144];
    uint32_t chroma = 0, brightness = 0, temporal = 0;
    unsigned temporal_hits = 0;
    for (int gy = 0; gy < 12; gy++) for (int gx = 0; gx < 12; gx++) {
        int x = x1 + (gx * 2 + 1) * (x2 - x1) / 24;
        int y = y1 + (gy * 2 + 1) * (y2 - y1) / 24;
        uint16_t v = p[(size_t)y * w + x];
        int r = ((v >> 11) & 31) * 255 / 31;
        int g = ((v >> 5) & 63) * 255 / 63;
        int b = (v & 31) * 255 / 31;
        int mx = r > g ? r : g; if (b > mx) mx = b;
        int mn = r < g ? r : g; if (b < mn) mn = b;
        chroma += (unsigned)(mx - mn);
        int lum = (r * 30 + g * 59 + b * 11) / 100;
        brightness += (unsigned)lum;
        int i = gy * 12 + gx;
        current[i] = (uint8_t)lum;
        if (ia10_prev_ok) {
            int d = abs(lum - (int)ia10_prev[i]);
            temporal += (unsigned)d;
            if (d >= 16) temporal_hits++;
        }
    }
    int cx = (x1 + x2) / 2, cy = (y1 + y2) / 2;
    bool aligned = ia10_prev_ok && abs(cx - ia10_prev_cx) < 10 &&
                   abs(cy - ia10_prev_cy) < 10;
    memcpy(ia10_prev, current, sizeof(current));
    ia10_prev_ok = true;
    ia10_prev_cx = cx; ia10_prev_cy = cy;
    unsigned c = chroma / 144U, l = brightness / 144U;
    unsigned d = temporal / 144U;
    /* La cromaticidad solo es fiable con iluminacion razonable. El movimiento
       aislado NO se considera prueba de vida (autoexposicion/ruido). */
    bool good_light = l >= 45U && l <= 230U;
    bool color_face = good_light && c >= 20U;
    bool gray_face = good_light && c <= 9U;
    bool static_roi = aligned && d <= 5U && temporal_hits <= 8U;
    if (color_face) {
        if (ia10_person_hits < 12) ia10_person_hits++;
        ia10_photo_hits = 0;
    } else if (gray_face && static_roi) {
        if (ia10_photo_hits < 12) ia10_photo_hits++;
        ia10_person_hits = 0;
    } else {
        if (ia10_person_hits) ia10_person_hits--;
        if (ia10_photo_hits) ia10_photo_hits--;
    }
    int state = ia10_person_hits >= 5 ? 1 : ia10_photo_hits >= 7 ? 2 : 3;
    ia10_result = state;
    static int64_t last_log = 0;
    if (state != ia10_last_log_state || us - last_log >= 1000000LL) {
        last_log = us; ia10_last_log_state = state;
        ESP_LOGI("IA10_VISUAL", "chroma=%u luma=%u delta=%u changed=%u aligned=%d person=%u photo=%u state=%d (EXPERIMENTAL)",
                 c, l, d, temporal_hits, aligned, ia10_person_hits, ia10_photo_hits, state);
    }
}
