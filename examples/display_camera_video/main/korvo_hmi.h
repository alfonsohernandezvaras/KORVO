#pragma once

#include <stdint.h>
#include "face_detect.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KORVO_HMI_FACE_NONE = 0,
    KORVO_HMI_FACE_POSITION,
    KORVO_HMI_FACE_HOLD_STILL,
    KORVO_HMI_FACE_READY
} korvo_hmi_face_state_t;

void korvo_hmi_init(void);
void korvo_hmi_sleep(void);
void korvo_hmi_wake(void);

/* Clear temporal face stability immediately when ESP-DL loses the face. */
void korvo_hmi_no_face(void);

/*
 * Feed one ESP-DL face sample into the geometric + temporal validation
 * matrix. READY is reached only after several stable consecutive samples.
 */
void korvo_hmi_update_face(const face_detection_t *face,
                           uint16_t camera_width,
                           uint16_t camera_height);

/* Dynamic ESP-DL overlay: bounding box + center marker. */
void korvo_hmi_track_face(const face_detection_t *face,
                          uint16_t camera_width,
                          uint16_t camera_height);
void korvo_hmi_hide_face_track(void);

/* Temporary motion/standby engineering diagnostic. */
void korvo_hmi_update_motion(uint32_t motion_percent_x10,
                             bool moving,
                             uint32_t idle_ms,
                             uint32_t sleep_ms);

/* Compact SD status card shown at the upper-right corner. */
void korvo_hmi_update_storage(bool sd_ok,
                              bool fs_ok,
                              bool structure_ok,
                              uint64_t used_bytes,
                              uint64_t free_bytes);

korvo_hmi_face_state_t korvo_hmi_face_state(void);

#ifdef __cplusplus
}
#endif
