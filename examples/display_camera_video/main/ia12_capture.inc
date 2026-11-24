/* IA12_CIERRE01: five fixed temporary RGB565 PPM captures, diagnostic only.
 * Camera callback copies a 128x128 face crop; SD writes run in a worker task.
 * No access command is emitted. */
#define IA12_SIDE 128
#define IA12_BYTES (IA12_SIDE * IA12_SIDE * 2)
#define IA12_PATH BSP_SD_MOUNT_POINT "/korvo/temp"
typedef struct { uint8_t *pixels; uint8_t index; } ia12_job_t;
static QueueHandle_t ia12_queue = NULL;
static volatile uint8_t ia12_captured = 0;
static volatile uint8_t ia12_saved = 0;
static volatile uint8_t ia12_failed = 0;
static int64_t ia12_last_capture_us = 0;
static uint8_t ia12_person_evidence = 0, ia12_card_evidence = 0;
static bool ia12_finalized = false;
static bool ia12_face_active = false;
static int64_t ia12_last_face_us = 0;
static void ia12_writer(void *unused)
{
    (void)unused;
    ia12_job_t job;
    for (;;) {
        if (xQueueReceive(ia12_queue, &job, portMAX_DELAY) != pdTRUE) continue;
        char filename[128];
        snprintf(filename, sizeof(filename), IA12_PATH "/rostro_%u.ppm", (unsigned)job.index);
        bool ok = false;
        if (korvo_storage_is_ready()) {
            FILE *fp = fopen(filename, "wb");
            if (fp) {
                char hdr[40];
                int n = snprintf(hdr, sizeof(hdr), "P6\n%d %d\n255\n", IA12_SIDE, IA12_SIDE);
                ok = fwrite(hdr, 1, (size_t)n, fp) == (size_t)n;
                uint8_t row[IA12_SIDE * 3];
                for (int y=0; y<IA12_SIDE && ok; y++) {
                    for (int x=0; x<IA12_SIDE; x++) {
                        size_t i = ((size_t)y*IA12_SIDE+x)*2;
                        uint16_t p = (uint16_t)job.pixels[i] | ((uint16_t)job.pixels[i+1]<<8);
                        row[x*3] = (uint8_t)(((p>>11)&31)*255/31);
                        row[x*3+1] = (uint8_t)(((p>>5)&63)*255/63);
                        row[x*3+2] = (uint8_t)((p&31)*255/31);
                    }
                    ok = fwrite(row, 1, sizeof(row), fp) == sizeof(row);
                }
                if (fclose(fp) != 0) ok = false;
                if (!ok) remove(filename);
            }
        }
        if (ok) { ia12_saved++; ESP_LOGI("IA12", "SD OK %s", filename); }
        else { ia12_failed++; ESP_LOGW("IA12", "SD ERROR %s", filename); }
        heap_caps_free(job.pixels);
    }
}
static void ia12_init(void)
{
    ia12_queue = xQueueCreate(5, sizeof(ia12_job_t));
    if (!ia12_queue) { ESP_LOGE("IA12", "queue allocation failed"); return; }
    if (xTaskCreate(ia12_writer, "ia12_sd", 4096, NULL, 3, NULL) != pdPASS) {
        vQueueDelete(ia12_queue); ia12_queue = NULL;
        ESP_LOGE("IA12", "writer task creation failed");
    }
}
static void ia12_capture(const uint8_t *buf, uint32_t w, uint32_t h, size_t len,
                         int x1, int y1, int x2, int y2)
{
    if (!ia12_queue || !buf || len < (size_t)w*h*2 || ia12_captured >= 5) return;
    int64_t now = esp_timer_get_time();
    if (now - ia12_last_capture_us < 350000LL) return;
    if (uxQueueSpacesAvailable(ia12_queue) == 0) return;
    uint8_t *pixels = heap_caps_malloc(IA12_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pixels) { ESP_LOGW("IA12", "no PSRAM for capture"); return; }
    const uint8_t *src = buf;
    for (int y=0; y<IA12_SIDE; y++) {
        int sy = y1 + (y*2+1)*(y2-y1)/(IA12_SIDE*2);
        for (int x=0; x<IA12_SIDE; x++) {
            int sx = x1 + (x*2+1)*(x2-x1)/(IA12_SIDE*2);
            size_t from = ((size_t)sy*w+(size_t)sx)*2;
            size_t to = ((size_t)y*IA12_SIDE+(size_t)x)*2;
            pixels[to]=src[from]; pixels[to+1]=src[from+1];
        }
    }
    ia12_job_t job = {.pixels=pixels, .index=(uint8_t)(ia12_captured+1)};
    if (xQueueSend(ia12_queue, &job, 0) != pdTRUE) { heap_caps_free(pixels); return; }
    ia12_last_capture_us = now;
    ia12_captured++;
    ESP_LOGI("IA12", "FLASH CAPTURA %u/5", (unsigned)ia12_captured);
}
