#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Mount SD and create/verify the standard KORVO directory tree. */
esp_err_t korvo_storage_init(void);

/* Create missing directories without deleting existing data. */
esp_err_t korvo_storage_create_layout(void);

bool korvo_storage_is_ready(void);
const char *korvo_storage_root(void);
const char *korvo_storage_layout_version(void);

#ifdef __cplusplus
}
#endif
