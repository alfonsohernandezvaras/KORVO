#include "korvo_protocol.h"

const char *korvo_protocol_name(korvo_message_id_t id)
{
    switch (id) {
    case KORVO_MSG_GATEWAY_READY:     return "GATEWAY_READY";
    case KORVO_MSG_DICTIONARY_ID:     return "DICTIONARY_ID";
    case KORVO_MSG_HEARTBEAT:         return "HEARTBEAT";
    case KORVO_MSG_ACCESS_DB_VERSION: return "ACCESS_DB_VERSION";
    case KORVO_MSG_DOOR_LOCKED:       return "DOOR_LOCKED";
    case KORVO_MSG_DOOR_OPENED:       return "DOOR_OPENED";
    case KORVO_MSG_REX:               return "REX";
    case KORVO_MSG_CALL_BUTTON:       return "CALL_BUTTON";
    case KORVO_MSG_ACCESS_GRANTED:    return "ACCESS_GRANTED";
    case KORVO_MSG_ACCESS_DENIED:     return "ACCESS_DENIED";
    case KORVO_MSG_DOOR_OPEN:         return "DOOR_OPEN";
    case KORVO_MSG_AUDIO_READY:       return "AUDIO_READY";
    case KORVO_MSG_AUDIO_START:       return "AUDIO_START";
    case KORVO_MSG_AUDIO_STOP:        return "AUDIO_STOP";
    case KORVO_MSG_AUDIO_STATUS:      return "AUDIO_STATUS";
    default:                           return "UNKNOWN";
    }
}

korvo_message_direction_t korvo_protocol_direction(korvo_message_id_t id)
{
    switch (id) {
    case KORVO_MSG_DICTIONARY_ID:
    case KORVO_MSG_HEARTBEAT:
        return KORVO_DIR_BIDIRECTIONAL;

    case KORVO_MSG_DOOR_OPEN:
    case KORVO_MSG_AUDIO_START:
    case KORVO_MSG_AUDIO_STOP:
        return KORVO_DIR_KORVO_TO_GATEWAY;

    default:
        return KORVO_DIR_GATEWAY_TO_KORVO;
    }
}

int korvo_protocol_is_valid(uint8_t id)
{
    switch ((korvo_message_id_t)id) {
    case KORVO_MSG_GATEWAY_READY:
    case KORVO_MSG_DICTIONARY_ID:
    case KORVO_MSG_HEARTBEAT:
    case KORVO_MSG_ACCESS_DB_VERSION:
    case KORVO_MSG_DOOR_LOCKED:
    case KORVO_MSG_DOOR_OPENED:
    case KORVO_MSG_REX:
    case KORVO_MSG_CALL_BUTTON:
    case KORVO_MSG_ACCESS_GRANTED:
    case KORVO_MSG_ACCESS_DENIED:
    case KORVO_MSG_DOOR_OPEN:
    case KORVO_MSG_AUDIO_READY:
    case KORVO_MSG_AUDIO_START:
    case KORVO_MSG_AUDIO_STOP:
    case KORVO_MSG_AUDIO_STATUS:
        return 1;
    default:
        return 0;
    }
}
