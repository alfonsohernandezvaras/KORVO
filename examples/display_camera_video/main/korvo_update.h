#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_app_desc.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif


/* ============================================================
 * RUTAS DEL SISTEMA DE ACTUALIZACIÓN KORVO
 * ============================================================ */

#define KORVO_UPDATE_DIR \
    "/sdcard/korvo/firmware/update"

#define KORVO_UPDATE_FILE \
    KORVO_UPDATE_DIR "/korvo.bin"

#define KORVO_UPDATE_TEMP \
    KORVO_UPDATE_DIR "/upload.tmp"

#define KORVO_UPDATE_PENDING \
    KORVO_UPDATE_DIR "/pending.txt"

#define KORVO_ARCHIVE_DIR \
    "/sdcard/korvo/firmware/archive"


/* ============================================================
 * INFORMACIÓN DE FIRMWARE
 * ============================================================ */

typedef struct {
    bool file_present;
    bool pending;
    bool valid;

    size_t size;

    esp_app_desc_t app;

    char sha256[65];
    char error[96];

} korvo_update_info_t;


/* ============================================================
 * API
 * ============================================================ */

/*
 * Ejecutar inmediatamente después de montar la SD.
 *
 * Si existe una actualización marcada como pendiente,
 * valida el BIN y ejecuta el proceso OTA.
 */
esp_err_t korvo_update_boot_check(void);


/*
 * Obtiene información del firmware almacenado en
 * /korvo/firmware/update/korvo.bin.
 */
esp_err_t korvo_update_get_info(
    korvo_update_info_t *info);


/*
 * Valida una imagen ESP-IDF:
 * - cabecera
 * - descriptor de aplicación
 * - proyecto
 * - tamaño
 * - SHA-256
 */
esp_err_t korvo_update_validate_file(
    const char *path,
    korvo_update_info_t *info);


/*
 * Marca el firmware validado como pendiente de instalación.
 *
 * allow_downgrade = false:
 *      solamente acepta una versión superior.
 *
 * allow_downgrade = true:
 *      permite reinstalar o instalar una versión anterior.
 */
esp_err_t korvo_update_mark_pending(
    bool allow_downgrade);


/*
 * Cancela la actualización y elimina:
 * - pending.txt
 * - upload.tmp
 * - korvo.bin
 */
esp_err_t korvo_update_cancel(void);


#ifdef __cplusplus
}
#endif