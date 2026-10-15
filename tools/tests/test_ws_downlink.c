// Pruebas de protocolo y rearmado reales, con un playback falso que copia PCM.
#include "ws_downlink.h"
#include "ws_rx.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned events[7], rejects;
static uint8_t captured[8192];
static size_t captured_size;
static bool accept_begin = true, accept_data = true;
static uint32_t last_connection;
static bool callback(void *ctx, const orbita_ws_downlink_event_t *e)
{
    (void)ctx;
    ++events[e->kind];
    last_connection = e->connection_id;
    if (e->kind == ORBITA_WS_PLAY_BEGIN) {
        captured_size = 0;
        return accept_begin;
    }
    if (e->kind == ORBITA_WS_PLAY_DATA) {
        assert(e->pcm_bytes + captured_size <= sizeof(captured));
        memcpy(captured + captured_size, e->pcm, e->pcm_bytes);
        captured_size += e->pcm_bytes;
        return accept_data;
    }
    return true;
}
static void rejected(void *ctx, uint32_t connection, uint64_t id)
{
    (void)ctx;
    (void)id;
    assert(connection == 7);
    ++rejects;
}
static ws_downlink_t fresh(void)
{
    memset(events, 0, sizeof(events));
    rejects = 0;
    captured_size = 0;
    accept_begin = accept_data = true;
    return (ws_downlink_t){.callback = callback, .reject = rejected, .connection = 7};
}
static ws_dl_result_t control(ws_downlink_t *d, const char *s)
{
    return ws_downlink_control(d, s, strlen(s));
}
#define BEGIN                                                                                      \
    "{\"type\":\"play\",\"play_id\":\"0123456789abcdef\",\"rate\":16000,\"format\":\"s16le\"}"
#define END "{\"type\":\"play_end\",\"play_id\":\"0123456789abcdef\"}"
#define STOP "{\"type\":\"play_stop\",\"play_id\":\"0123456789abcdef\"}"
static void put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i)
        p[i] = (uint8_t)(v >> (8 * i));
}
static void packet(uint8_t *p, size_t n, uint32_t seq)
{
    assert(n >= 26 && n <= 4096);
    memcpy(p, "PLAY\1\30\0\0", 8);
    put32(p + 8, 0x89abcdef);
    put32(p + 12, 0x01234567);
    put32(p + 16, seq);
    put32(p + 20, (uint32_t)n - 24);
    for (size_t i = 24; i < n; ++i)
        p[i] = (uint8_t)(i * 7);
}
static void test_stream(void)
{
    ws_downlink_t d = fresh();
    uint8_t p[4096];
    packet(p, sizeof(p), 0);
    assert(control(&d, BEGIN) == WS_DL_HANDLED);
    assert(ws_downlink_binary(&d, p, sizeof(p)) == WS_DL_HANDLED);
    assert(captured_size == 4072 && !memcmp(captured, p + 24, 4072));
    assert(last_connection == 7 && d.next_seq == 1);
    packet(p, 26, 1);
    assert(ws_downlink_binary(&d, p, 26) == WS_DL_HANDLED);
    assert(control(&d, END) == WS_DL_HANDLED);
    assert(events[ORBITA_WS_PLAY_END] == 1 && !rejects);
    assert(!d.receiving && d.known);
    assert(control(&d, STOP) == WS_DL_HANDLED); // se puede cortar aun despues de END
    assert(!d.known && events[ORBITA_WS_PLAY_STOP] == 1);
}
static void test_controls(void)
{
    ws_downlink_t d = fresh();
    assert(control(&d, "{\"type\":\"ack\",\"index\":8}") == WS_DL_PASS);
    assert(control(&d, "saved id clean") == WS_DL_PASS);
    assert(control(&d, "{\"type\":\"orbita\",\"id\":\"0123456789abcdef\",\"at_index\":8}") ==
           WS_DL_HANDLED);
    assert(control(&d, "{\"type\":\"conversation_end\",\"id\":\"0123456789abcdef\"}") ==
           WS_DL_HANDLED);
    assert(events[ORBITA_WS_CONVERSATION_BEGIN] == 1 && events[ORBITA_WS_CONVERSATION_END] == 1);
    const char *bad[] = {
        "{} trailing",
        "{\"type\":\"play\",\"type\":\"ack\"}",
        "{\"type\":\"play\\u0000bad\"}",
        "{\"type\":\"play\",\"play_id\":2}",
        "{\"type\":\"orbita\",\"id\":\"0123456789abcdef\",\"at_index\":-1}",
        "{\"type\":\"orbita\",\"id\":\"0123456789abcdef\",\"at_index\":0.5}",
        "{\"type\":\"orbita\",\"id\":\"0123456789abcdef\",\"at_index\":4294967296}",
        "{\"x\":[[[[[]]]]]}"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(*bad); ++i)
        assert(control(&d, bad[i]) == WS_DL_INVALID);
    char nul[] = "{\"type\":\"ack\"}";
    nul[2] = 0;
    assert(ws_downlink_control(&d, nul, sizeof(nul) - 1) == WS_DL_INVALID);
    accept_begin = false;
    assert(control(&d, BEGIN) == WS_DL_HANDLED && rejects == 1 && !d.known);
}
static void test_errors(void)
{
    uint8_t p[26];
    ws_downlink_t d = fresh();
    packet(p, 26, 0);
    assert(ws_downlink_binary(&d, p, 26) == WS_DL_INVALID); // datos antes del play
    assert(control(&d, BEGIN) == WS_DL_HANDLED);
    p[8] ^= 1;
    assert(ws_downlink_binary(&d, p, 26) == WS_DL_INVALID && d.receiving); // id viejo
    p[8] ^= 1;
    put32(p + 16, 2);
    assert(ws_downlink_binary(&d, p, 26) == WS_DL_INVALID && !d.known);
    assert(rejects == 1 && events[ORBITA_WS_PLAY_STOP] == 1);
    d = fresh();
    control(&d, BEGIN);
    packet(p, 26, 0);
    accept_data = false;
    assert(ws_downlink_binary(&d, p, 26) == WS_DL_HANDLED && !d.known && rejects == 1);
    d = fresh();
    control(&d, BEGIN);
    assert(control(&d, BEGIN) == WS_DL_INVALID && !d.known); // duplicado no reinicia seq
    d = fresh();
    control(&d, BEGIN);
    ws_downlink_reset(&d, 0);
    assert(events[ORBITA_WS_DOWNLINK_RESET] == 1 && last_connection == 7 && !d.known);
    assert(ws_downlink_binary(&d, p, 26) == WS_DL_INVALID);
    const unsigned offsets[] = {0, 4, 5, 6, 20};
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(*offsets); ++i) {
        d = fresh();
        control(&d, BEGIN);
        packet(p, 26, 0);
        p[offsets[i]] ^= 1;
        assert(ws_downlink_binary(&d, p, 26) == WS_DL_INVALID && !d.known);
    }
}
static void test_declared_length(void)
{
    ws_downlink_t d = fresh();
    uint8_t p[26];
    packet(p, 26, 0);
    const char *begin = "{\"type\":\"play\",\"play_id\":\"0123456789abcdef\",\"rate\":16000,"
                        "\"format\":\"s16le\",\"bytes\":2}";
    assert(control(&d, begin) == WS_DL_HANDLED);
    assert(control(&d, END) == WS_DL_INVALID && rejects == 1);
    d = fresh();
    control(&d, begin);
    ws_downlink_binary(&d, p, 26);
    assert(control(&d, END) == WS_DL_HANDLED && !rejects);
    d = fresh();
    control(&d, begin);
    ws_downlink_binary(&d, p, 26);
    put32(p + 16, 1);
    assert(ws_downlink_binary(&d, p, 26) == WS_DL_INVALID && rejects == 1);
}
static void test_reassembly(void)
{
    uint8_t binary[4096], packet_bytes[4096];
    packet(packet_bytes, 4096, 0);
    ws_rx_t rx = {.binary_buffer = binary, .binary_capacity = sizeof(binary)};
    // Cada posible division del frame, incluyendo en medio del header PLAY.
    for (size_t split = 1; split < 4096; ++split) {
        ws_rx_reset(&rx);
        assert(ws_rx_feed(&rx, 2, true, 4096, 0, packet_bytes, split) == WS_RX_MORE);
        assert(ws_rx_feed(&rx, 9, true, 0, 0, NULL, 0) == WS_RX_CONTROL);
        assert(ws_rx_feed(&rx, 2, true, 4096, split, packet_bytes + split, 4096 - split) ==
               WS_RX_COMPLETE);
        assert(rx.message.size == 4096 && !memcmp(binary, packet_bytes, 4096));
    }
    ws_rx_reset(&rx);
    assert(ws_rx_feed(&rx, 2, false, 1024, 0, packet_bytes, 1024) == WS_RX_MORE);
    assert(ws_rx_feed(&rx, 0, true, 3072, 0, packet_bytes + 1024, 100) == WS_RX_MORE);
    assert(ws_rx_feed(&rx, 0, true, 3072, 100, packet_bytes + 1124, 2972) == WS_RX_COMPLETE);
    assert(!memcmp(binary, packet_bytes, 4096));
    ws_rx_reset(&rx);
    assert(ws_rx_feed(&rx, 1, true, 513, 0, packet_bytes, 513) == WS_RX_TOO_LARGE);
    assert(ws_rx_feed(&rx, 2, true, 4097, 0, packet_bytes, 1) == WS_RX_TOO_LARGE);
    assert(ws_rx_feed(&rx, 0, true, 1, 0, packet_bytes, 1) == WS_RX_INVALID);
    assert(ws_rx_feed(&rx, 2, true, 100, 0, packet_bytes, 50) == WS_RX_MORE);
    assert(ws_rx_feed(&rx, 2, true, 100, 51, packet_bytes + 50, 49) == WS_RX_INVALID);
    assert(ws_rx_feed(&rx, 2, false, 100, 0, packet_bytes, 100) == WS_RX_MORE);
    assert(ws_rx_feed(&rx, 1, true, 2, 0, packet_bytes, 2) == WS_RX_INVALID); // no intercalar texto
    ws_rx_reset(&rx);
    assert(ws_rx_feed(&rx, 1, true, 512, 0, packet_bytes, 512) == WS_RX_COMPLETE);
    assert(!memcmp(rx.message.data, packet_bytes, 512));
}

static void test_stream_limit_and_lifecycle(void)
{
    ws_downlink_t d = fresh();
    uint8_t p[4096];
    assert(ws_downlink_control(&d, NULL, 0) == WS_DL_PASS);
    d.callback = NULL;
    assert(control(&d, BEGIN) == WS_DL_HANDLED && rejects == 1 && !d.known);
    d = fresh();
    assert(control(&d, BEGIN) == WS_DL_HANDLED);
    // Consumidor que no retiene una copia: solo contar bytes del stream largo.
    d.callback = NULL;
    // Se restaura callback y reinicia su buffer de test entre paquetes.
    for (uint32_t i = 0; i < 117; ++i) {
        d.callback = callback;
        captured_size = 0;
        packet(p, 4096, i);
        assert(ws_downlink_binary(&d, p, 4096) == WS_DL_HANDLED);
    }
    assert(d.received == 476424);
    captured_size = 0;
    packet(p, 4096, 117);
    assert(ws_downlink_binary(&d, p, 4096) == WS_DL_INVALID && rejects == 1);
    d = fresh();
    control(&d, BEGIN);
    control(&d, END);
    assert(control(&d, END) == WS_DL_INVALID); // END duplicado no reproduce de nuevo
    ws_downlink_reset(&d, 0);
    assert(events[ORBITA_WS_DOWNLINK_RESET] == 1); // aun despues de END
    d = fresh();
    control(&d, BEGIN);
    assert(control(&d, "{\"type\":\"play_stop\",\"play_id\":\"ffffffffffffffff\"}") ==
           WS_DL_INVALID);
    assert(d.receiving && !events[ORBITA_WS_PLAY_STOP]);
    assert(control(&d, "{\"type\":\"play\",\"play_id\":\"0123456789abcdef\",\"rate\":8000,"
                       "\"format\":\"s16le\"}") == WS_DL_INVALID);
    assert(!d.known && events[ORBITA_WS_PLAY_STOP] == 1);
}

static void test_async_completion(void)
{
    ws_downlink_t d = fresh();
    uint8_t p[26];
    control(&d, BEGIN);
    ws_downlink_finish(&d, 6, d.id);
    assert(d.receiving); // generacion vieja
    ws_downlink_finish(&d, 7, d.id);
    assert(!d.receiving && !events[ORBITA_WS_PLAY_STOP]);
    packet(p, sizeof(p), 0);
    assert(ws_downlink_binary(&d, p, sizeof(p)) == WS_DL_INVALID);
    assert(!events[ORBITA_WS_PLAY_DATA]);
    assert(control(&d, "{\"type\":\"play\",\"play_id\":\"0000000000000002\","
                       "\"rate\":16000,\"format\":\"s16le\"}") == WS_DL_HANDLED);
    assert(d.receiving && d.id == 2 && !rejects);
    ws_downlink_finish(&d, 7, UINT64_C(0x0123456789abcdef));
    assert(d.receiving && d.id == 2); // cierre tardio de A no afecta B
}

int main(void)
{
    test_async_completion();
    test_stream_limit_and_lifecycle();
    test_stream();
    test_controls();
    test_errors();
    test_declared_length();
    test_reassembly();
    puts("PASS: downlink protocol, cancellation, reset, limits and 4095 frame splits");
    return 0;
}
