#include "korvo_hmi.h"

#include <stdlib.h>
#include <stdio.h>
#include "bsp/esp-bsp.h"
#include "esp_log.h"

static const char *TAG = "korvo_hmi";

static lv_obj_t *s_guide = NULL;
static lv_obj_t *s_message = NULL;
static lv_obj_t *s_face_box = NULL;
static lv_obj_t *s_face_center = NULL;
static lv_obj_t *s_motion_diag = NULL;
static lv_obj_t *s_storage_diag = NULL;
static lv_obj_t *s_screensaver = NULL;
static bool s_awake = false;
static korvo_hmi_face_state_t s_face_state = KORVO_HMI_FACE_NONE;

/* Tall portrait guide agreed for the KORVO access terminal. */
#define GUIDE_W_PERCENT  46
#define GUIDE_H_PERCENT  78

/*
 * Face quality matrix.
 *
 * A single ESP-DL detection is NEVER enough for READY.
 * We validate geometry, size, aspect and temporal stability.
 */
#define FACE_MIN_W_PERCENT          28
#define FACE_MIN_H_PERCENT          34
#define FACE_CENTER_X_MIN_PERCENT   27
#define FACE_CENTER_X_MAX_PERCENT   73
#define FACE_CENTER_Y_MIN_PERCENT   18
#define FACE_CENTER_Y_MAX_PERCENT   82

/* Reject extremely thin/wide detector boxes as unusable face candidates. */
#define FACE_ASPECT_MIN_PERCENT     55
#define FACE_ASPECT_MAX_PERCENT     125

/*
 * Temporal stability thresholds between consecutive detector samples.
 * Percentages are relative to camera dimensions / previous face size.
 */
#define STABLE_CENTER_DELTA_X_PERCENT   4
#define STABLE_CENTER_DELTA_Y_PERCENT   4
#define STABLE_SIZE_DELTA_PERCENT       10

/* Require this many consecutive good/stable samples before READY. */
#define READY_STABLE_SAMPLES            5

typedef struct {
    bool valid;
    int cx;
    int cy;
    int w;
    int h;
    uint32_t stable_samples;
} face_track_t;

static face_track_t s_track = {0};

static void set_message(const char *text)
{
    if (s_message == NULL) {
        return;
    }

    bsp_display_lock(0);
    lv_label_set_text(s_message, text);
    bsp_display_unlock();
}

static void reset_face_track(void)
{
    s_track.valid = false;
    s_track.cx = 0;
    s_track.cy = 0;
    s_track.w = 0;
    s_track.h = 0;
    s_track.stable_samples = 0;
}

static void set_face_state(korvo_hmi_face_state_t state)
{
    if (s_face_state == state) {
        return;
    }

    s_face_state = state;

    switch (state) {
    case KORVO_HMI_FACE_NONE:
        set_message("POSICIONE SU ROSTRO\nEN EL RECUADRO");
        break;

    case KORVO_HMI_FACE_POSITION:
        set_message("POSICIONE SU ROSTRO\nEN EL RECUADRO");
        break;

    case KORVO_HMI_FACE_HOLD_STILL:
        set_message("MANTENGASE QUIETO");
        break;

    case KORVO_HMI_FACE_READY:
        set_message("ROSTRO LISTO");
        break;

    default:
        break;
    }
}

void korvo_hmi_init(void)
{
    lv_obj_t *screen = lv_scr_act();

    s_guide = lv_obj_create(screen);
    lv_obj_remove_style_all(s_guide);
    lv_obj_set_size(s_guide,
                    (BSP_LCD_H_RES * GUIDE_W_PERCENT) / 100,
                    (BSP_LCD_V_RES * GUIDE_H_PERCENT) / 100);
    lv_obj_center(s_guide);
    lv_obj_set_style_border_width(s_guide, 4, 0);
    lv_obj_set_style_border_color(s_guide, lv_color_white(), 0);
    lv_obj_set_style_border_opa(s_guide, LV_OPA_90, 0);
    lv_obj_set_style_radius(s_guide, 18, 0);
    lv_obj_set_style_bg_opa(s_guide, LV_OPA_TRANSP, 0);

    /*
     * Dynamic ESP-DL face overlay.
     * It is intentionally separate from the fixed portrait guide.
     */
    s_face_box = lv_obj_create(screen);
    lv_obj_remove_style_all(s_face_box);
    lv_obj_set_style_border_width(s_face_box, 2, 0);
    lv_obj_set_style_border_color(s_face_box, lv_color_white(), 0);
    lv_obj_set_style_border_opa(s_face_box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(s_face_box, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_face_box, LV_OBJ_FLAG_HIDDEN);

    s_face_center = lv_obj_create(screen);
    lv_obj_remove_style_all(s_face_center);
    lv_obj_set_size(s_face_center, 8, 8);
    lv_obj_set_style_radius(s_face_center, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_face_center, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_face_center, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_face_center, LV_OBJ_FLAG_HIDDEN);

    s_message = lv_label_create(screen);
    lv_label_set_text(s_message, "POSICIONE SU ROSTRO\nEN EL RECUADRO");
    lv_obj_set_style_text_color(s_message, lv_color_white(), 0);
    lv_obj_set_style_text_align(s_message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(s_message, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_message, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(s_message, 8, 0);
    lv_obj_align(s_message, LV_ALIGN_BOTTOM_MID, 0, -12);

    /* Temporary engineering diagnostic for motion / standby calibration. */
    s_motion_diag = lv_label_create(screen);
    lv_label_set_text(s_motion_diag,
                      "MOVIMIENTO: --.-%\n"
                      "ESTADO: INICIANDO\n"
                      "INACTIVO: 0.0 / 10.0 s\n"
                      "APAGADO EN: 10.0 s");
    lv_obj_set_style_text_color(s_motion_diag, lv_color_white(), 0);
    lv_obj_set_style_bg_color(s_motion_diag, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_motion_diag, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(s_motion_diag, 5, 0);
    lv_obj_align(s_motion_diag, LV_ALIGN_TOP_LEFT, 6, 6);

    /* Compact SD status card, symmetric with the motion diagnostic. */
    s_storage_diag = lv_label_create(screen);
    lv_label_set_text(s_storage_diag,
                      "SD CARD: --\n"
                      "FS: --\n"
                      "ESTRUCTURA: --\n"
                      "USO: --\n"
                      "LIBRE: --");
    lv_obj_set_style_text_color(s_storage_diag, lv_color_white(), 0);
    lv_obj_set_style_text_align(s_storage_diag, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_bg_color(s_storage_diag, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_storage_diag, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(s_storage_diag, 5, 0);
    lv_obj_align(s_storage_diag, LV_ALIGN_TOP_RIGHT, -6, 6);

    /*
     * Full-screen software screensaver.
     * Created LAST so it can cover camera + HMI + diagnostics completely.
     * It starts hidden; standby shows it as an opaque black layer.
     */
    s_screensaver = lv_obj_create(screen);
    lv_obj_remove_style_all(s_screensaver);
    lv_obj_set_pos(s_screensaver, 0, 0);
    lv_obj_set_size(s_screensaver, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(s_screensaver, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_screensaver, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screensaver, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_screensaver, LV_OBJ_FLAG_HIDDEN);

    reset_face_track();
    s_face_state = KORVO_HMI_FACE_NONE;

    ESP_LOGI(TAG, "Access HMI initialized");
}

void korvo_hmi_sleep(void)
{
    s_awake = false;
    reset_face_track();
    s_face_state = KORVO_HMI_FACE_NONE;
    korvo_hmi_hide_face_track();

    /*
     * Software standby: cover the complete LCD with an opaque black LVGL
     * object. Camera, AI, video, Wi-Fi and the LCD controller keep running.
     */
    if (s_screensaver != NULL) {
        bsp_display_lock(0);
        lv_obj_clear_flag(s_screensaver, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_screensaver);
        bsp_display_unlock();
    }

    ESP_LOGI(TAG, "Standby: software screensaver BLACK");
}

void korvo_hmi_wake(void)
{
    s_awake = true;
    reset_face_track();

    /* Remove the black cover immediately on confirmed motion. */
    if (s_screensaver != NULL) {
        bsp_display_lock(0);
        lv_obj_add_flag(s_screensaver, LV_OBJ_FLAG_HIDDEN);
        bsp_display_unlock();
    }

    s_face_state = KORVO_HMI_FACE_POSITION;
    set_message("POSICIONE SU ROSTRO\\nEN EL RECUADRO");

    ESP_LOGI(TAG, "Confirmed motion: software screensaver HIDDEN");
}

void korvo_hmi_no_face(void)
{
    /*
     * Losing the face invalidates READY immediately. This is deliberately
     * separate from the 10 s LCD timeout.
     */
    reset_face_track();
    korvo_hmi_hide_face_track();

    if (s_awake) {
        set_face_state(KORVO_HMI_FACE_POSITION);
    } else {
        s_face_state = KORVO_HMI_FACE_NONE;
    }
}

void korvo_hmi_hide_face_track(void)
{
    if (s_face_box == NULL || s_face_center == NULL) {
        return;
    }

    bsp_display_lock(0);
    lv_obj_add_flag(s_face_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_face_center, LV_OBJ_FLAG_HIDDEN);
    bsp_display_unlock();
}

void korvo_hmi_track_face(const face_detection_t *face,
                          uint16_t camera_width,
                          uint16_t camera_height)
{
    if (!s_awake || face == NULL || !face->detected ||
        camera_width == 0 || camera_height == 0 ||
        s_face_box == NULL || s_face_center == NULL) {
        korvo_hmi_hide_face_track();
        return;
    }

    int x1 = face->x1;
    int y1 = face->y1;
    int x2 = face->x2;
    int y2 = face->y2;

    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= camera_width) x2 = camera_width - 1;
    if (y2 >= camera_height) y2 = camera_height - 1;

    if (x2 <= x1 || y2 <= y1) {
        korvo_hmi_hide_face_track();
        return;
    }

    /*
     * ESP-DL coordinates are in camera-frame space.
     * Map them to the LCD canvas. The current project uses rotation=0 in
     * normal operation; this mapping follows the aspect-fit dimensions.
     */
    int display_w = BSP_LCD_H_RES;
    int display_h = BSP_LCD_V_RES;

    float camera_aspect = (float)camera_width / (float)camera_height;
    float display_aspect = (float)display_w / (float)display_h;

    int video_w;
    int video_h;
    int video_x;
    int video_y;

    if (camera_aspect > display_aspect) {
        video_w = display_w;
        video_h = (int)((float)display_w / camera_aspect);
        video_x = 0;
        video_y = (display_h - video_h) / 2;
    } else {
        video_h = display_h;
        video_w = (int)((float)display_h * camera_aspect);
        video_x = (display_w - video_w) / 2;
        video_y = 0;
    }

    int sx1 = video_x + (x1 * video_w) / camera_width;
    int sy1 = video_y + (y1 * video_h) / camera_height;
    int sx2 = video_x + (x2 * video_w) / camera_width;
    int sy2 = video_y + (y2 * video_h) / camera_height;

    int box_w = sx2 - sx1;
    int box_h = sy2 - sy1;
    int center_x = sx1 + box_w / 2;
    int center_y = sy1 + box_h / 2;

    bsp_display_lock(0);

    lv_obj_set_pos(s_face_box, sx1, sy1);
    lv_obj_set_size(s_face_box, box_w, box_h);
    lv_obj_clear_flag(s_face_box, LV_OBJ_FLAG_HIDDEN);

    lv_obj_set_pos(s_face_center, center_x - 4, center_y - 4);
    lv_obj_clear_flag(s_face_center, LV_OBJ_FLAG_HIDDEN);

    /* Keep diagnostics and instructions above the video canvas. */
    lv_obj_move_foreground(s_guide);
    lv_obj_move_foreground(s_face_box);
    lv_obj_move_foreground(s_face_center);
    lv_obj_move_foreground(s_message);
    if (s_motion_diag != NULL) {
        lv_obj_move_foreground(s_motion_diag);
    }
    if (s_storage_diag != NULL) {
        lv_obj_move_foreground(s_storage_diag);
    }

    bsp_display_unlock();
}

void korvo_hmi_update_face(const face_detection_t *face,
                           uint16_t camera_width,
                           uint16_t camera_height)
{
    if (!s_awake || face == NULL || !face->detected ||
        camera_width == 0 || camera_height == 0) {
        return;
    }

    int face_w = face->x2 - face->x1;
    int face_h = face->y2 - face->y1;

    if (face_w <= 0 || face_h <= 0) {
        reset_face_track();
        set_face_state(KORVO_HMI_FACE_POSITION);
        return;
    }

    int face_cx = face->x1 + face_w / 2;
    int face_cy = face->y1 + face_h / 2;

    bool centered_x =
        face_cx > (int)(camera_width * FACE_CENTER_X_MIN_PERCENT / 100) &&
        face_cx < (int)(camera_width * FACE_CENTER_X_MAX_PERCENT / 100);

    bool centered_y =
        face_cy > (int)(camera_height * FACE_CENTER_Y_MIN_PERCENT / 100) &&
        face_cy < (int)(camera_height * FACE_CENTER_Y_MAX_PERCENT / 100);

    bool close_enough =
        face_w > (int)(camera_width * FACE_MIN_W_PERCENT / 100) &&
        face_h > (int)(camera_height * FACE_MIN_H_PERCENT / 100);

    int aspect_percent = (face_w * 100) / face_h;
    bool plausible_aspect =
        aspect_percent >= FACE_ASPECT_MIN_PERCENT &&
        aspect_percent <= FACE_ASPECT_MAX_PERCENT;

    /*
     * Geometry gate: an ESP-DL candidate may wake the screen, but it cannot
     * accumulate READY stability unless it looks like a usable face crop.
     */
    bool geometry_ok = centered_x && centered_y &&
                       close_enough && plausible_aspect;

    if (!geometry_ok) {
        reset_face_track();
        set_face_state(KORVO_HMI_FACE_POSITION);
        return;
    }

    bool stable = false;

    if (s_track.valid) {
        int dx = abs(face_cx - s_track.cx);
        int dy = abs(face_cy - s_track.cy);
        int dw = abs(face_w - s_track.w);
        int dh = abs(face_h - s_track.h);

        int max_dx = (int)(camera_width * STABLE_CENTER_DELTA_X_PERCENT / 100);
        int max_dy = (int)(camera_height * STABLE_CENTER_DELTA_Y_PERCENT / 100);
        int max_dw = (s_track.w * STABLE_SIZE_DELTA_PERCENT) / 100;
        int max_dh = (s_track.h * STABLE_SIZE_DELTA_PERCENT) / 100;

        if (max_dw < 1) max_dw = 1;
        if (max_dh < 1) max_dh = 1;

        stable = dx <= max_dx &&
                 dy <= max_dy &&
                 dw <= max_dw &&
                 dh <= max_dh;
    }

    if (!s_track.valid) {
        s_track.stable_samples = 1;
    } else if (stable) {
        if (s_track.stable_samples < READY_STABLE_SAMPLES) {
            s_track.stable_samples++;
        }
    } else {
        /*
         * The face is valid but moving. Start stability accumulation again.
         */
        s_track.stable_samples = 1;
    }

    s_track.valid = true;
    s_track.cx = face_cx;
    s_track.cy = face_cy;
    s_track.w = face_w;
    s_track.h = face_h;

    if (s_track.stable_samples >= READY_STABLE_SAMPLES) {
        set_face_state(KORVO_HMI_FACE_READY);
    } else {
        set_face_state(KORVO_HMI_FACE_HOLD_STILL);
    }

    ESP_LOGI(TAG,
             "FACE MATRIX geom=%d stable=%d samples=%lu/%d center=%d,%d size=%dx%d aspect=%d%%",
             geometry_ok,
             stable,
             (unsigned long)s_track.stable_samples,
             READY_STABLE_SAMPLES,
             face_cx, face_cy, face_w, face_h, aspect_percent);
}

void korvo_hmi_update_motion(uint32_t motion_percent_x10,
                             bool moving,
                             uint32_t idle_ms,
                             uint32_t sleep_ms)
{
    if (s_motion_diag == NULL) {
        return;
    }

    uint32_t pct_whole = motion_percent_x10 / 10U;
    uint32_t pct_frac = motion_percent_x10 % 10U;
    uint32_t idle_tenths = idle_ms / 100U;
    uint32_t sleep_tenths = sleep_ms / 100U;
    uint32_t remain_ms = (idle_ms >= sleep_ms) ? 0U : sleep_ms - idle_ms;
    uint32_t remain_tenths = remain_ms / 100U;

    char text[160];
    snprintf(text, sizeof(text),
             "MOVIMIENTO: %lu.%lu%%\n"
             "ESTADO: %s\n"
             "INACTIVO: %lu.%lu / %lu.%lu s\n"
             "APAGADO EN: %lu.%lu s",
             (unsigned long)pct_whole,
             (unsigned long)pct_frac,
             moving ? "MOVIMIENTO" : "SIN MOVIMIENTO",
             (unsigned long)(idle_tenths / 10U),
             (unsigned long)(idle_tenths % 10U),
             (unsigned long)(sleep_tenths / 10U),
             (unsigned long)(sleep_tenths % 10U),
             (unsigned long)(remain_tenths / 10U),
             (unsigned long)(remain_tenths % 10U));

    bsp_display_lock(0);
    lv_label_set_text(s_motion_diag, text);
    lv_obj_move_foreground(s_motion_diag);
    bsp_display_unlock();

}


void korvo_hmi_update_storage(bool sd_ok,
                              bool fs_ok,
                              bool structure_ok,
                              uint64_t used_bytes,
                              uint64_t free_bytes)
{
    if (s_storage_diag == NULL) {
        return;
    }

    char text[192];

    if (sd_ok && fs_ok) {
        const double gib = 1024.0 * 1024.0 * 1024.0;

        snprintf(text, sizeof(text),
                 "SD CARD: OK\n"
                 "FS: FAT32 OK\n"
                 "ESTRUCTURA: %s\n"
                 "USO: %.1f GB\n"
                 "LIBRE: %.1f GB",
                 structure_ok ? "OK" : "ERROR",
                 (double)used_bytes / gib,
                 (double)free_bytes / gib);
    } else if (sd_ok) {
        snprintf(text, sizeof(text),
                 "SD CARD: OK\n"
                 "FS: ERROR\n"
                 "ESTRUCTURA: --\n"
                 "USO: --\n"
                 "LIBRE: --");
    } else {
        snprintf(text, sizeof(text),
                 "SD CARD: ERROR\n"
                 "FS: --\n"
                 "ESTRUCTURA: --\n"
                 "USO: --\n"
                 "LIBRE: --");
    }

    bsp_display_lock(0);
    lv_label_set_text(s_storage_diag, text);
    lv_obj_move_foreground(s_storage_diag);
    bsp_display_unlock();
}

korvo_hmi_face_state_t korvo_hmi_face_state(void)
{
    return s_face_state;
}
