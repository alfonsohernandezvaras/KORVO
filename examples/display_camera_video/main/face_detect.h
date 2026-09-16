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
} face_detection_t;

bool face_detect_init(void);

bool face_detect_run(const uint8_t *rgb565be,
                     uint16_t width,
                     uint16_t height,
                     face_detection_t *result);

#ifdef __cplusplus
}
#endif
