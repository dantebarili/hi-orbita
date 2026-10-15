/* Protocolo downlink, sin FreeRTOS ni I2S. JSON acotado a 512 B; cJSON solo
 * reserva memoria para controles. El camino PCM no reserva memoria: presta el
 * buffer al consumidor durante el callback. Se prueba con callbacks falsos en PC.
 */
#include "ws_downlink.h"
#include "cJSON.h"
#include "orbita_ws_message.h"
#include <string.h>

static bool deliver(ws_downlink_t *dl, orbita_ws_downlink_event_t event)
{
    event.connection_id = dl->connection;
    return dl->callback && dl->callback(dl->ctx, &event);
}
static void reject_id(ws_downlink_t *dl, uint64_t id)
{
    if (dl->reject)
        dl->reject(dl->reject_ctx, dl->connection, id);
}
void ws_downlink_reset(ws_downlink_t *dl, uint32_t connection)
{
    // RESET lleva la generacion anterior: descartar su audio incluso tras END.
    if (dl->connection)
        deliver(dl, (orbita_ws_downlink_event_t){.kind = ORBITA_WS_DOWNLINK_RESET, .id = dl->id});
    dl->connection = connection;
    dl->id = 0;
    dl->known = dl->receiving = false;
    dl->received = dl->next_seq = 0;
}
void ws_downlink_fail(ws_downlink_t *dl)
{
    if (!dl->known)
        return;
    deliver(dl, (orbita_ws_downlink_event_t){
                    .kind = ORBITA_WS_PLAY_STOP, .id = dl->id, .rejected = true});
    reject_id(dl, dl->id);
    dl->known = dl->receiving = false;
}
static bool hex_id(const cJSON *item, uint64_t *out)
{
    if (!cJSON_IsString(item) || strlen(item->valuestring) != 16)
        return false;
    uint64_t value = 0;
    for (unsigned i = 0; i < 16; ++i) {
        unsigned char c = item->valuestring[i];
        unsigned v = c >= '0' && c <= '9'   ? c - '0'
                     : c >= 'a' && c <= 'f' ? c - 'a' + 10
                     : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                            : 16;
        if (v == 16)
            return false;
        value = (value << 4) | v;
    }
    *out = value;
    return true;
}
static bool number(const cJSON *item, uint32_t *out)
{
    if (!cJSON_IsNumber(item) || !(item->valuedouble >= 0) || !(item->valuedouble <= UINT32_MAX))
        return false;
    *out = (uint32_t)item->valuedouble;
    return item->valuedouble == *out;
}
// Limitar recursion ANTES de cJSON. NUL escapado tampoco puede truncar un type/id.
static bool bounded_json(const uint8_t *p, size_t n)
{
    bool quoted = false, escape = false;
    unsigned depth = 0;
    if (!p || !n || n > ORBITA_WS_MAX_CONTROL_BYTES)
        return false;
    for (size_t i = 0; i < n; ++i) {
        if (!p[i] || (i + 6 <= n && memcmp(p + i, "\\u0000", 6) == 0))
            return false;
        if (quoted) {
            if (escape)
                escape = false;
            else if (p[i] == '\\')
                escape = true;
            else if (p[i] == '"')
                quoted = false;
        } else if (p[i] == '"')
            quoted = true;
        else if (p[i] == '{' || p[i] == '[') {
            if (++depth > 4)
                return false;
        } else if (p[i] == '}' || p[i] == ']') {
            if (!depth)
                return false;
            --depth;
        }
    }
    return !quoted && depth == 0;
}
static ws_dl_result_t control_object(ws_downlink_t *dl, const cJSON *root)
{
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type))
        return WS_DL_PASS;
    const char *name = type->valuestring;
    bool begin = strcmp(name, "play") == 0, end = strcmp(name, "play_end") == 0;
    bool stop = strcmp(name, "play_stop") == 0;
    bool conversation = strcmp(name, "orbita") == 0,
         conversation_end = strcmp(name, "conversation_end") == 0;
    if (!begin && !end && !stop && !conversation && !conversation_end)
        return WS_DL_PASS;
    uint64_t id;
    if (!hex_id(cJSON_GetObjectItemCaseSensitive(
                    root, conversation || conversation_end ? "id" : "play_id"),
                &id))
        return WS_DL_INVALID;
    orbita_ws_downlink_event_t event = {.id = id};
    if (conversation || conversation_end) {
        event.kind = conversation ? ORBITA_WS_CONVERSATION_BEGIN : ORBITA_WS_CONVERSATION_END;
        if (conversation &&
            !number(cJSON_GetObjectItemCaseSensitive(root, "at_index"), &event.at_index))
            return WS_DL_INVALID;
        deliver(dl, event);
        return WS_DL_HANDLED;
    }
    if (begin) {
        const cJSON *format = cJSON_GetObjectItemCaseSensitive(root, "format");
        const cJSON *bytes = cJSON_GetObjectItemCaseSensitive(root, "bytes");
        const cJSON *channels = cJSON_GetObjectItemCaseSensitive(root, "channels");
        uint32_t rate, channel_count;
        if (!number(cJSON_GetObjectItemCaseSensitive(root, "rate"), &rate) || rate != 16000 ||
            !cJSON_IsString(format) || strcmp(format->valuestring, "s16le") ||
            (channels && (!number(channels, &channel_count) || channel_count != 1)) ||
            (bytes && (!number(bytes, &event.total_bytes) || (event.total_bytes & 1) ||
                       event.total_bytes > ORBITA_WS_PLAY_MAX_PCM_BYTES))) {
            if (dl->known && dl->id == id)
                ws_downlink_fail(dl);
            else
                reject_id(dl, id);
            return WS_DL_INVALID;
        }
        // Tras END, playback decide si ya tiene lugar para otro BEGIN: puede
        // seguir sonando aunque no venga mas PCM. No reutilizar play_id.
        if (dl->receiving || (dl->known && id == dl->id)) {
            if (dl->known && id == dl->id)
                ws_downlink_fail(dl);
            else
                reject_id(dl, id);
            return WS_DL_INVALID;
        }
        event.kind = ORBITA_WS_PLAY_BEGIN;
        event.has_total_bytes = bytes != NULL;
        if (!deliver(dl, event)) {
            reject_id(dl, id);
            return WS_DL_HANDLED;
        }
        dl->id = id;
        dl->known = dl->receiving = true;
        dl->has_expected = event.has_total_bytes;
        dl->expected = event.total_bytes;
        dl->received = dl->next_seq = 0;
        return WS_DL_HANDLED;
    }
    if (!dl->known || id != dl->id)
        return WS_DL_INVALID; // STOP viejo no corta audio nuevo
    if (stop) {
        event.kind = ORBITA_WS_PLAY_STOP;
        deliver(dl, event);
        dl->known = dl->receiving = false;
        return WS_DL_HANDLED;
    }
    if (!dl->receiving)
        return WS_DL_INVALID;
    if (dl->has_expected && dl->received != dl->expected) {
        ws_downlink_fail(dl);
        return WS_DL_INVALID;
    }
    event.kind = ORBITA_WS_PLAY_END;
    if (!deliver(dl, event))
        ws_downlink_fail(dl);
    dl->receiving = false;
    // END es fin de entrada: play_done lo manda playback cuando TERMINA de sonar.
    return WS_DL_HANDLED;
}
ws_dl_result_t ws_downlink_control(ws_downlink_t *dl, const void *data, size_t size)
{
    if (size == 0)
        return WS_DL_PASS;
    if (!data || size > ORBITA_WS_MAX_CONTROL_BYTES)
        return WS_DL_INVALID;
    // Conservar la API de texto generico (por ejemplo respuestas del laboratorio v1).
    if (data && size) {
        const uint8_t *p = data;
        size_t i = 0;
        while (i < size && (p[i] == ' ' || p[i] == '\r' || p[i] == '\n' || p[i] == '\t'))
            ++i;
        if (i == size || p[i] != '{')
            return WS_DL_PASS;
    }
    if (!bounded_json(data, size))
        return WS_DL_INVALID;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(data, size, &end, false);
    if (!root)
        return WS_DL_INVALID;
    const char *limit = (const char *)data + size;
    while (end < limit && (*end == ' ' || *end == '\r' || *end == '\n' || *end == '\t'))
        ++end;
    bool valid = cJSON_IsObject(root) && end == limit;
    for (const cJSON *a = root->child; valid && a; a = a->next)
        for (const cJSON *b = a->next; b; b = b->next)
            if (a->string && b->string && strcmp(a->string, b->string) == 0)
                valid = false;
    ws_dl_result_t result = valid ? control_object(dl, root) : WS_DL_INVALID;
    cJSON_Delete(root);
    return result;
}
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
ws_dl_result_t ws_downlink_binary(ws_downlink_t *dl, const void *data, size_t size)
{
    const uint8_t *p = data;
    if (!p || size < ORBITA_WS_PLAY_HEADER_BYTES || size > ORBITA_WS_MAX_PLAY_BYTES ||
        memcmp(p, "PLAY", 4) || p[4] != 1 || p[5] != ORBITA_WS_PLAY_HEADER_BYTES || p[6] || p[7]) {
        ws_downlink_fail(dl);
        return WS_DL_INVALID;
    }
    uint64_t id = (uint64_t)le32(p + 8) | (uint64_t)le32(p + 12) << 32;
    uint32_t seq = le32(p + 16), length = le32(p + 20);
    if (!dl->known || id != dl->id)
        return WS_DL_INVALID;
    if (!dl->receiving || length != size - ORBITA_WS_PLAY_HEADER_BYTES || !length || (length & 1) ||
        seq != dl->next_seq || length > ORBITA_WS_PLAY_MAX_PCM_BYTES - dl->received ||
        (dl->has_expected && length > dl->expected - dl->received)) {
        ws_downlink_fail(dl);
        return WS_DL_INVALID;
    }
    if (!deliver(dl, (orbita_ws_downlink_event_t){.kind = ORBITA_WS_PLAY_DATA,
                                                  .id = id,
                                                  .seq = seq,
                                                  .pcm = p + ORBITA_WS_PLAY_HEADER_BYTES,
                                                  .pcm_bytes = length})) {
        ws_downlink_fail(dl);
        return WS_DL_HANDLED;
    }
    dl->received += length;
    ++dl->next_seq;
    return WS_DL_HANDLED;
}

// El consumidor ya termino/detuvo el audio. No devolverle otro STOP.
// Una notificacion tardia nunca debe cerrar el PLAY nuevo.
void ws_downlink_finish(ws_downlink_t *dl, uint32_t connection, uint64_t id)
{
    if (dl->connection == connection && dl->known && dl->id == id)
        dl->known = dl->receiving = false;
}
