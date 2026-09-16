#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t korvo_storage_init(void);
bool korvo_storage_is_ready(void);
const char *korvo_storage_root(void);

#ifdef __cplusplus
}
#endif
