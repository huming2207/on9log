/**
 * @file on9log_unix_stdio.c
 * @brief Packet-preserving Linux/macOS stdio sink for on9log.
 */

#include "on9log_unix_stdio.h"

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "on9log_transport.h"

#if defined(__APPLE__) && !ON9LOG_PLAIN_TEXT
#include <inttypes.h>
#include <mach-o/dyld.h>
#endif

#if !ON9LOG_PLAIN_TEXT
#define ON9LOG_UNIX_FRAME_START 0xa5u
#define ON9LOG_UNIX_FRAME_END 0xc0u
#define ON9LOG_UNIX_FRAME_ESCAPE 0xdbu
#define ON9LOG_UNIX_FRAME_TYPE_BINARY 0x01u
#endif

typedef struct {
    FILE *stream;
#if !ON9LOG_PLAIN_TEXT
    uint16_t crc;
    uint8_t frame_payload[ON9LOG_TRANSPORT_MAX_PAYLOAD];
    size_t frame_payload_len;
    bool drop_frame;
#endif
    bool write_failed;
} on9log_unix_stdio_state_t;

static on9log_unix_stdio_state_t s_stdio;
static pthread_mutex_t s_init_mutex = PTHREAD_MUTEX_INITIALIZER;

#if !ON9LOG_PLAIN_TEXT
static uint16_t on9log_unix_crc16_update(uint16_t crc, uint8_t byte)
{
    crc ^= (uint16_t)byte << 8u;
    for (unsigned bit = 0; bit < 8; ++bit) {
        crc = (crc & 0x8000u) != 0u
                  ? (uint16_t)((crc << 1u) ^ 0x1021u)
                  : (uint16_t)(crc << 1u);
    }
    return crc;
}

static void on9log_unix_write_raw(uint8_t byte)
{
    if (!s_stdio.write_failed && fputc((int)byte, s_stdio.stream) == EOF) {
        s_stdio.write_failed = true;
    }
}

static void on9log_unix_write_escaped(uint8_t byte)
{
    switch (byte) {
    case 0xa5u:
        on9log_unix_write_raw(ON9LOG_UNIX_FRAME_ESCAPE);
        on9log_unix_write_raw(0xdeu);
        break;
    case 0xc0u:
        on9log_unix_write_raw(ON9LOG_UNIX_FRAME_ESCAPE);
        on9log_unix_write_raw(0xdcu);
        break;
    case 0xdbu:
        on9log_unix_write_raw(ON9LOG_UNIX_FRAME_ESCAPE);
        on9log_unix_write_raw(0xddu);
        break;
    case 0x0du:
        on9log_unix_write_raw(ON9LOG_UNIX_FRAME_ESCAPE);
        on9log_unix_write_raw(0xd0u);
        break;
    case 0x0au:
        on9log_unix_write_raw(ON9LOG_UNIX_FRAME_ESCAPE);
        on9log_unix_write_raw(0xd1u);
        break;
    default:
        on9log_unix_write_raw(byte);
        break;
    }
}

static void on9log_unix_write_binary_bytes(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        s_stdio.crc = on9log_unix_crc16_update(s_stdio.crc, data[i]);
        on9log_unix_write_escaped(data[i]);
    }
}
#endif

static void on9log_unix_stdio_start(const uint8_t *header,
                                    size_t header_len,
                                    void *ctx)
{
    (void)ctx;
    flockfile(s_stdio.stream);
    s_stdio.write_failed = false;

#if ON9LOG_PLAIN_TEXT
    (void)header;
    (void)header_len;
#else
    s_stdio.frame_payload_len = 0;
    s_stdio.drop_frame = header == NULL || header_len > ON9LOG_TRANSPORT_MAX_PAYLOAD;
    if (!s_stdio.drop_frame) {
        memcpy(s_stdio.frame_payload, header, header_len);
        s_stdio.frame_payload_len = header_len;
    }
#endif
}

static void on9log_unix_stdio_payload(const uint8_t *payload,
                                      size_t payload_len,
                                      size_t total_arg_cnt,
                                      size_t curr_arg_index,
                                      void *ctx)
{
    (void)total_arg_cnt;
    (void)curr_arg_index;
    (void)ctx;

#if ON9LOG_PLAIN_TEXT
    if (!s_stdio.write_failed &&
        fwrite(payload, 1u, payload_len, s_stdio.stream) != payload_len) {
        s_stdio.write_failed = true;
    }
#else
    if (!s_stdio.drop_frame) {
        if ((payload == NULL && payload_len != 0) ||
            payload_len > ON9LOG_TRANSPORT_MAX_PAYLOAD - s_stdio.frame_payload_len) {
            s_stdio.drop_frame = true;
            s_stdio.frame_payload_len = 0;
        } else if (payload_len != 0) {
            memcpy(&s_stdio.frame_payload[s_stdio.frame_payload_len], payload, payload_len);
            s_stdio.frame_payload_len += payload_len;
        }
    }
#endif
}

static void on9log_unix_stdio_end(void *ctx)
{
    (void)ctx;

#if !ON9LOG_PLAIN_TEXT
    if (!s_stdio.drop_frame) {
        s_stdio.crc = 0xffffu;
        on9log_unix_write_raw(ON9LOG_UNIX_FRAME_START);
        on9log_unix_write_binary_bytes((const uint8_t[]){ON9LOG_UNIX_FRAME_TYPE_BINARY}, 1u);
        on9log_unix_write_binary_bytes(s_stdio.frame_payload, s_stdio.frame_payload_len);
        on9log_unix_write_escaped((uint8_t)(s_stdio.crc & 0xffu));
        on9log_unix_write_escaped((uint8_t)(s_stdio.crc >> 8u));
        on9log_unix_write_raw(ON9LOG_UNIX_FRAME_END);
    }
#endif
    (void)fflush(s_stdio.stream);
    funlockfile(s_stdio.stream);
}

static const on9log_sink_t s_stdio_sink = {
    .start_cb = on9log_unix_stdio_start,
    .payload_cb = on9log_unix_stdio_payload,
    .end_cb = on9log_unix_stdio_end,
};

static void on9log_unix_write_image_metadata(FILE *stream)
{
#if defined(__APPLE__) && !ON9LOG_PLAIN_TEXT
    const uint32_t slide = (uint32_t)(uintptr_t)_dyld_get_image_vmaddr_slide(0);
    flockfile(stream);
    (void)fprintf(stream, "@on9log-image-slide=%08" PRIx32 "\n", slide);
    (void)fflush(stream);
    funlockfile(stream);
#else
    (void)stream;
#endif
}

on9log_err_t on9log_unix_stdio_init_file(FILE *stream)
{
    if (stream == NULL) {
        return ON9LOG_ERR_INVALID_ARG;
    }

    on9log_err_t err = on9log_init();
    if (err != ON9LOG_OK) {
        return err;
    }

    (void)pthread_mutex_lock(&s_init_mutex);
    if (s_stdio.stream != NULL) {
        err = s_stdio.stream == stream ? ON9LOG_OK : ON9LOG_ERR_INVALID_ARG;
        (void)pthread_mutex_unlock(&s_init_mutex);
        return err;
    }

    s_stdio.stream = stream;
    err = on9log_add_sink(&s_stdio_sink, &s_stdio);
    if (err != ON9LOG_OK) {
        s_stdio.stream = NULL;
    } else {
        on9log_unix_write_image_metadata(stream);
    }
    (void)pthread_mutex_unlock(&s_init_mutex);
    return err;
}

on9log_err_t on9log_unix_stdio_init(void)
{
    return on9log_unix_stdio_init_file(stdout);
}
