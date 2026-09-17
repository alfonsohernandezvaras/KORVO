#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KORVO_STORAGE_STATE_ERROR = 0,
    KORVO_STORAGE_STATE_READY,
    KORVO_STORAGE_STATE_INITIALIZING,
    KORVO_STORAGE_STATE_REPAIRING,
    KORVO_STORAGE_STATE_FOREIGN
} korvo_storage_state_t;

/*
 * Mount SD and safely auto-provision the KORVO directory tree.
 * Existing foreign data is never formatted or modified automatically.
 */
esp_err_t korvo_storage_init(void);

/* Create missing directories without deleting existing data. */
esp_err_t korvo_storage_create_layout(void);

typedef struct {
    bool ok;              /* Overall: SD + FAT filesystem + KORVO structure */
    bool sd_ok;           /* SD was mounted by KORVO */
    bool fs_ok;           /* FAT filesystem is accessible and capacity query works */
    bool structure_ok;    /* Required KORVO directories + storage.version are valid */
    korvo_storage_state_t state;
    uint64_t total_bytes;
    uint64_t free_bytes;
    uint64_t used_bytes;
} korvo_storage_status_t;

/*
 * Runtime SD watchdog probe.
 * Read-only supervisor: never creates folders, formats the card or reboots KORVO.
 * Checks SD mount state, FAT filesystem accessibility/capacity and the complete
 * KORVO directory structure.
 */
esp_err_t korvo_storage_get_status(korvo_storage_status_t *status);

bool korvo_storage_is_ready(void);
const char *korvo_storage_root(void);
const char *korvo_storage_layout_version(void);
korvo_storage_state_t korvo_storage_state(void);

#ifdef __cplusplus
}
#endif
