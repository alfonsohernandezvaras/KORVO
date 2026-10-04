#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * KORVO <-> GATEWAY - DICCIONARIO MAESTRO V1
 *
 * Regla:
 *   COMANDO != ESTADO.
 * La KORVO puede ordenar una accion, pero la HMI debe reflejar
 * el estado real publicado por el Gateway.
 *
 * El audio continuo NO viaja como palabras de este diccionario.
 */

#define KORVO_PROTOCOL_DICTIONARY_VERSION  1u

/*
 * FUENTES DE ESTADO:
 * GATEWAY -> GATEWAY / LECTOR / PUERTA
 * ASTERISK -> SIP
 * LOCAL KORVO -> SD / WEBSERVER
 * AUDIO_* corresponde al futuro puente KORVO-Gateway y no al registro SIP.
 */

typedef enum {
    KORVO_MSG_GATEWAY_READY     = 0x01,
    KORVO_MSG_DICTIONARY_ID     = 0x02,
    KORVO_MSG_HEARTBEAT         = 0x03,
    KORVO_MSG_ACCESS_DB_VERSION = 0x04,

    KORVO_MSG_DOOR_LOCKED       = 0x10,
    KORVO_MSG_DOOR_OPENED       = 0x11,
    KORVO_MSG_REX               = 0x12,
    KORVO_MSG_CALL_BUTTON       = 0x13,
    KORVO_MSG_ACCESS_GRANTED    = 0x14,
    KORVO_MSG_ACCESS_DENIED     = 0x15,

    /* Unico comando normal de puerta enviado por KORVO.
       La decision proviene de la autorizacion facial IA. */
    KORVO_MSG_DOOR_OPEN         = 0x20,

    KORVO_MSG_AUDIO_READY       = 0x30,
    KORVO_MSG_AUDIO_START       = 0x31,
    KORVO_MSG_AUDIO_STOP        = 0x32,
    KORVO_MSG_AUDIO_STATUS      = 0x33
} korvo_message_id_t;

typedef enum {
    KORVO_DIR_GATEWAY_TO_KORVO = 0,
    KORVO_DIR_KORVO_TO_GATEWAY,
    KORVO_DIR_BIDIRECTIONAL
} korvo_message_direction_t;

const char *korvo_protocol_name(korvo_message_id_t id);
korvo_message_direction_t korvo_protocol_direction(korvo_message_id_t id);
int korvo_protocol_is_valid(uint8_t id);

#ifdef __cplusplus
}
#endif
