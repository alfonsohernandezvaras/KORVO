#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool detected;
    float score;
    int x1;
    int y1;
    int x2;
    int y2;
    /* IA03_EYE_ROI: estimaciones geometricas; NO landmarks reales. */
    bool eye_roi_valid;
    bool fr01_points_valid;
    int fr01_points[10];
    int eye_left_x1, eye_left_y1, eye_left_x2, eye_left_y2;
    int eye_right_x1, eye_right_y1, eye_right_x2, eye_right_y2;
} face_detection_t;

bool face_detect_init(void);

bool face_detect_run(const uint8_t *rgb565be,
                     uint16_t width,
                     uint16_t height,
                     face_detection_t *result);

#ifdef __cplusplus
}
#endif
