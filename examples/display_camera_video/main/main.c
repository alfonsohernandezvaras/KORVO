/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

/**
 * @file
 * @brief BSP Camera Example
 * @details Stream camera output to display (LVGL)
 * @example https://espressif.github.io/esp-launchpad/?flashConfigURL=https://espressif.github.io/esp-bsp/config.toml&app=display_camera_video
 */

#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "bsp/esp-bsp.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_attr.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#if SOC_PPA_SUPPORTED
#include "driver/ppa.h"
#endif
#include "esp_private/esp_cache_private.h"
#include "app_video.h"
#include "face_detect.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define NUM_BUFS 2
#define ALIGN_UP(num, align)    (((num) + ((align) - 1)) & ~((align) - 1))

typedef struct {
    void  *start;
    size_t length;
} mmap_buf_t;

static const char *TAG = "example";

/*
 * One-shot software restart after a real power-on.
 * RTC_NOINIT_ATTR survives esp_restart(), so the second boot continues
 * normally instead of entering a restart loop.
 */
#define COLD_BOOT_RESTART_MAGIC 0x4B4F5256U
RTC_NOINIT_ATTR static uint32_t cold_boot_restart_magic;

static void cold_boot_restart_once(void)
{
    esp_reset_reason_t reason = esp_reset_reason();

    if (reason == ESP_RST_POWERON && cold_boot_restart_magic != COLD_BOOT_RESTART_MAGIC) {
        cold_boot_restart_magic = COLD_BOOT_RESTART_MAGIC;
        ESP_LOGW(TAG, "Cold power-on detected: automatic RESET in 2000 ms");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }

    /* Arm the next genuine power cycle. */
    if (reason != ESP_RST_SW) {
        cold_boot_restart_magic = 0;
    }
}
#if SOC_PPA_SUPPORTED
static ppa_client_handle_t ppa_srm_handle = NULL;
#endif
static size_t data_cache_line_size = 0;
static lv_obj_t *camera_canvas = NULL;
static uint8_t *cam_buff[NUM_BUFS];
static uint32_t cam_buff_size = 0;

static lv_color_format_t lvgl_cam_rgb565_fmt;

/* ===== FACE DETECTION ===== */
#define FACE_DETECT_EVERY_N_FRAMES 5
#define CAMERA_START_RETRIES        5
#define VIDEO_OPEN_RETRIES          10

static uint8_t *face_ai_buffer = NULL;
static size_t face_ai_buffer_size = 0;
static SemaphoreHandle_t face_ai_mutex = NULL;
static volatile bool face_ai_pending = false;
static uint32_t face_frame_counter = 0;
static uint16_t face_ai_width = 0;
static uint16_t face_ai_height = 0;

static lv_color_format_t lvgl_rgb565_fmt_from_v4l2(uint32_t pixelformat)
{
    if (pixelformat == V4L2_PIX_FMT_RGB565X) {
        return LV_COLOR_FORMAT_RGB565_SWAPPED;
    }
    if (pixelformat == V4L2_PIX_FMT_RGB565) {
        return LV_COLOR_FORMAT_RGB565;
    }
    ESP_LOGW(TAG, "Unexpected RGB565 pixel format, using LV_COLOR_FORMAT_RGB565");
    return LV_COLOR_FORMAT_RGB565;
}

#if SOC_PPA_SUPPORTED
static void app_ppa_init(void)
{
    /* Initialize PPA */
    ppa_client_config_t ppa_srm_config = {
        .oper_type = PPA_OPERATION_SRM,
    };

    esp_err_t ret = ppa_register_client(&ppa_srm_config, &ppa_srm_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register PPA client: 0x%x", ret);
    }
}

static esp_err_t app_image_process_scale_crop(
    uint8_t *in_buf, uint32_t in_width, uint32_t in_height,
    uint8_t *out_buf, uint32_t out_width, uint32_t out_height, size_t out_buf_size,
    ppa_srm_rotation_angle_t rotation_angle)
{
    float scale_x = (float)out_width / in_width;
    float scale_y = (float)out_height / in_height;

    if (rotation_angle == PPA_SRM_ROTATION_ANGLE_90 || rotation_angle == PPA_SRM_ROTATION_ANGLE_270) {
        scale_x = (float)out_height / in_width;
        scale_y = (float)out_width / in_height;
    }

    ppa_srm_oper_config_t srm_config = {
        .in.buffer = in_buf,
        .in.pic_w = in_width,
        .in.pic_h = in_height,
        .in.block_w = in_width,
        .in.block_h = in_height,
        .in.block_offset_x = 0,
        .in.block_offset_y = 0,
        .in.srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        .out.buffer = out_buf,
        .out.buffer_size = out_buf_size,
        .out.pic_w = out_width,
        .out.pic_h = out_height,
        .out.block_offset_x = 0,
        .out.block_offset_y = 0,
        .out.srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        .rotation_angle = rotation_angle,
        .scale_x = scale_x,
        .scale_y = scale_y,
        .rgb_swap = 0,
        .byte_swap = 0,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    return ppa_do_scale_rotate_mirror(ppa_srm_handle, &srm_config);
}

static void calc_aspect_fit(
    uint32_t src_w, uint32_t src_h,
    uint32_t dst_w, uint32_t dst_h,
    uint32_t *out_w, uint32_t *out_h)
{
    float src_aspect = (float)src_w / src_h;
    float dst_aspect = (float)dst_w / dst_h;

    if (src_aspect > dst_aspect) {
        *out_w = dst_w;
        *out_h = dst_w / src_aspect;
    } else {
        *out_h = dst_h;
        *out_w = dst_h * src_aspect;
    }
}
#endif

static void face_detection_task(void *arg)
{
    ESP_LOGI(TAG, "Starting ESP-DL face detector");

    if (!face_detect_init()) {
        ESP_LOGE(TAG, "Failed to initialize face detector");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "ESP-DL face detector ready");

    while (1) {
        if (!face_ai_pending) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        face_detection_t result = {0};

        if (xSemaphoreTake(face_ai_mutex, portMAX_DELAY) == pdTRUE) {
            bool ok = face_detect_run(face_ai_buffer,
                                      face_ai_width,
                                      face_ai_height,
                                      &result);
            face_ai_pending = false;
            xSemaphoreGive(face_ai_mutex);

            if (!ok) {
                ESP_LOGW(TAG, "Face detector run failed");
                continue;
            }
        }

        if (result.detected) {
            ESP_LOGI(TAG, "FACE score=%.3f box=[%d,%d,%d,%d]",
                     result.score, result.x1, result.y1,
                     result.x2, result.y2);
        }
    }
}

static void camera_video_frame_operation(uint8_t *camera_buf, uint8_t camera_buf_index, uint32_t camera_buf_hes,
        uint32_t camera_buf_ves, size_t camera_buf_len)
{
    uint32_t out_w = camera_buf_hes;
    uint32_t out_h = camera_buf_ves ;
    uint8_t *out_buf = camera_buf;

    /*
     * Copy a periodic camera frame into a dedicated PSRAM buffer.
     * ESP-DL runs in its own task, never inside this video callback.
     */
    face_frame_counter++;
    if (face_ai_buffer != NULL &&
        face_ai_mutex != NULL &&
        !face_ai_pending &&
        (face_frame_counter % FACE_DETECT_EVERY_N_FRAMES) == 0) {

        size_t required = (size_t)camera_buf_hes * camera_buf_ves * 2;

        if (required <= face_ai_buffer_size &&
            required <= camera_buf_len &&
            xSemaphoreTake(face_ai_mutex, 0) == pdTRUE) {

            memcpy(face_ai_buffer, camera_buf, required);
            face_ai_width = (uint16_t)camera_buf_hes;
            face_ai_height = (uint16_t)camera_buf_ves;
            face_ai_pending = true;

            xSemaphoreGive(face_ai_mutex);
        }
    }
#if SOC_PPA_SUPPORTED
    ppa_srm_rotation_angle_t rotation = PPA_SRM_ROTATION_ANGLE_0;

    switch (BSP_CAMERA_ROTATION) {
    case 0:
        rotation = PPA_SRM_ROTATION_ANGLE_0;
        break;
    case 90:
        rotation = PPA_SRM_ROTATION_ANGLE_90;
        break;
    case 180:
        rotation = PPA_SRM_ROTATION_ANGLE_180;
        break;
    case 270:
        rotation = PPA_SRM_ROTATION_ANGLE_270;
        break;
    }

    /* Get size of camera for screen (by aspect ratio)  */
    if (BSP_CAMERA_ROTATION == 90 || BSP_CAMERA_ROTATION == 270) {
        calc_aspect_fit(camera_buf_ves, camera_buf_hes, BSP_LCD_H_RES, BSP_LCD_V_RES, &out_w, &out_h);
    } else {
        calc_aspect_fit(camera_buf_hes, camera_buf_ves, BSP_LCD_H_RES, BSP_LCD_V_RES, &out_w, &out_h);
    }

    /* Scale camera picture for the screen + rotation */
    app_image_process_scale_crop(
        camera_buf, camera_buf_hes, camera_buf_ves,
        cam_buff[camera_buf_index], out_w, out_h, cam_buff_size,
        rotation
    );
    out_buf = cam_buff[camera_buf_index];
#endif

    bsp_display_lock(0);
    lv_canvas_set_buffer(camera_canvas, out_buf, out_w, out_h, lvgl_cam_rgb565_fmt);
    lv_obj_center(camera_canvas);
    lv_obj_invalidate(camera_canvas);
    bsp_display_unlock();
}

void app_main(void)
{
    esp_err_t ret = ESP_OK;

    /* Reproduce the known-good physical RESET automatically after power-on. */
    cold_boot_restart_once();

    bsp_display_start();
    bsp_display_backlight_on(); // Set display brightness to 100%

    /*
     * ESP32-S31-Korvo-1 cold power-on stabilization.
     * Delay OV3660/DVP initialization until board rails/clocks are stable.
     */
    ESP_LOGI(TAG, "Cold boot camera stabilization (1000 ms)...");
    vTaskDelay(pdMS_TO_TICKS(1000));

    /* Initialize Camera - robust cold boot startup */
    ESP_LOGI(TAG, "Starting camera...");

    esp_err_t camera_ret = ESP_FAIL;
    for (int attempt = 1; attempt <= CAMERA_START_RETRIES; attempt++) {
        camera_ret = bsp_camera_start(NULL);
        if (camera_ret == ESP_OK) {
            ESP_LOGI(TAG, "Camera initialized on attempt %d", attempt);
            break;
        }

        ESP_LOGW(TAG, "Camera init attempt %d failed: %s",
                 attempt, esp_err_to_name(camera_ret));
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    if (camera_ret != ESP_OK) {
        ESP_LOGE(TAG, "Camera initialization failed after %d attempts",
                 CAMERA_START_RETRIES);
        return;
    }

#if SOC_PPA_SUPPORTED
    /* Initialize PPA for scaling */
    app_ppa_init();
#endif

    /* Get cache alignment */
    ret = esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &data_cache_line_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Get cache alignment failed: 0x%x", ret);
        return;
    }

    /* Allocate canvas buffers */
    cam_buff_size = ALIGN_UP(BSP_LCD_H_RES * BSP_LCD_V_RES * 2, data_cache_line_size);
    for (int i = 0; i < NUM_BUFS; i++) {
        cam_buff[i] = heap_caps_aligned_calloc(data_cache_line_size, 1, cam_buff_size, MALLOC_CAP_SPIRAM);
        if (cam_buff[i] == NULL) {
            ESP_LOGE(TAG, "Failed to allocate camera buffer %d", i);
            return;
        }
    }

    /* Dedicated full VGA RGB565 buffer for ESP-DL */
    face_ai_buffer_size = 640U * 480U * 2U;
    face_ai_buffer = heap_caps_malloc(face_ai_buffer_size, MALLOC_CAP_SPIRAM);
    if (face_ai_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate face AI buffer (%u bytes)",
                 (unsigned)face_ai_buffer_size);
        return;
    }

    face_ai_mutex = xSemaphoreCreateMutex();
    if (face_ai_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create face AI mutex");
        return;
    }

    /* Open video first: pixel format comes from menuconfig / driver (VIDIOC_G_FMT). */
    int fd = -1;
    for (int attempt = 1; attempt <= VIDEO_OPEN_RETRIES; attempt++) {
        fd = app_video_open(BSP_CAMERA_DEVICE, APP_VIDEO_FMT_DRIVER_DEFAULT);
        if (fd >= 0) {
            ESP_LOGI(TAG, "Video device opened on attempt %d", attempt);
            break;
        }

        ESP_LOGW(TAG, "Video device not ready, attempt %d/%d",
                 attempt, VIDEO_OPEN_RETRIES);
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (fd < 0) {
        ESP_LOGE(TAG, "Failed to open video device after %d attempts",
                 VIDEO_OPEN_RETRIES);
        return;
    }
    lvgl_cam_rgb565_fmt = lvgl_rgb565_fmt_from_v4l2(app_video_get_pixelformat());

    /* Create LVGL canvas for camera image */
    bsp_display_lock(0);
    camera_canvas = lv_canvas_create(lv_scr_act());
    lv_canvas_set_buffer(camera_canvas, cam_buff[0], BSP_LCD_H_RES, BSP_LCD_V_RES, lvgl_cam_rgb565_fmt);
    assert(camera_canvas);
    lv_obj_center(camera_canvas);
/* ===== KORVO ACCESS HMI ===== */

lv_obj_t *title = lv_label_create(lv_scr_act());
lv_label_set_text(title, "CONTROL DE ACCESO");
lv_obj_set_style_text_color(title, lv_color_white(), 0);
lv_obj_set_style_bg_color(title, lv_color_black(), 0);
lv_obj_set_style_bg_opa(title, LV_OPA_70, 0);
lv_obj_set_style_pad_all(title, 8, 0);
lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

lv_obj_t *status = lv_label_create(lv_scr_act());
lv_label_set_text(status, "SISTEMA ACTIVO");
lv_obj_set_style_text_color(status, lv_color_white(), 0);
lv_obj_set_style_bg_color(status, lv_color_black(), 0);
lv_obj_set_style_bg_opa(status, LV_OPA_70, 0);
lv_obj_set_style_pad_all(status, 6, 0);
lv_obj_align(status, LV_ALIGN_BOTTOM_MID, 0, -8);
    bsp_display_unlock();

    /* Initialize video capture device */
    ret = app_video_set_bufs(fd, NUM_BUFS, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set video buffers: 0x%x", ret);
        return;
    }

    /* Register frame process callback */
    ret = app_video_register_frame_operation_cb(camera_video_frame_operation);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register frame operation callback: 0x%x", ret);
        return;
    }

    /* Start video stream task */
    ret = app_video_stream_task_start(fd, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start video stream task: 0x%x", ret);
        return;
    }

    BaseType_t ai_task_ret = xTaskCreate(
        face_detection_task,
        "face_ai",
        8192,
        NULL,
        5,
        NULL
    );

    if (ai_task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create face detection task");
    }

    ESP_LOGI(TAG, "Camera + ESP-DL face detection running.");
}
