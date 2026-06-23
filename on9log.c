#include "on9log.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "on9log_config.h"
#include "on9log_fmt.h"
#include "on9log_port.h"

#define ON9LOG_NULL_STRING_LEN UINT32_MAX
#define ON9LOG_PAYLOAD_META_ARG_INDEX SIZE_MAX

typedef struct {
    atomic_uintptr_t sink;
    atomic_uintptr_t ctx;
} on9log_sink_entry_t;

typedef struct {
    bool overflow;
    size_t len;
    size_t cap;
    uint8_t *data;
} on9log_encoder_t;

typedef struct {
    const on9log_sink_t *sink;
    void *ctx;
} on9log_stream_sink_t;

typedef struct {
    bool uart_enabled;
    size_t sink_count;
    on9log_stream_sink_t sinks[ON9LOG_MAX_SINKS];
} on9log_stream_t;

static on9log_sink_entry_t s_sinks[ON9LOG_MAX_SINKS];
static atomic_bool s_uart_enabled = ATOMIC_VAR_INIT(true);
static atomic_uint_fast16_t s_seq = ATOMIC_VAR_INIT(0);
static atomic_uint_fast32_t s_dropped_count = ATOMIC_VAR_INIT(0);

static void on9log_put_u8(on9log_encoder_t *enc, uint8_t val)
{
    if (enc->len >= enc->cap) {
        enc->overflow = true;
        return;
    }

    enc->data[enc->len++] = val;
}

static void on9log_put_u16(on9log_encoder_t *enc, uint16_t val)
{
    on9log_put_u8(enc, (uint8_t)(val & 0xffu));
    on9log_put_u8(enc, (uint8_t)(val >> 8u));
}

static void on9log_put_u32(on9log_encoder_t *enc, uint32_t val)
{
    on9log_put_u8(enc, (uint8_t)(val & 0xffu));
    on9log_put_u8(enc, (uint8_t)((val >> 8u) & 0xffu));
    on9log_put_u8(enc, (uint8_t)((val >> 16u) & 0xffu));
    on9log_put_u8(enc, (uint8_t)(val >> 24u));
}

static void on9log_put_u64(on9log_encoder_t *enc, uint64_t val)
{
    on9log_put_u32(enc, (uint32_t)(val & 0xffffffffu));
    on9log_put_u32(enc, (uint32_t)(val >> 32u));
}

static uint8_t on9log_arg_type_at(const char *arg_types, unsigned idx)
{
    if (arg_types == NULL) {
        return ON9_LOG_ARGS_TYPE_NONE;
    }
    return (uint8_t)arg_types[idx];
}

static uint8_t on9log_arg_count(const char *arg_types)
{
    uint8_t count = 0;

    while (on9log_arg_type_at(arg_types, count) != ON9_LOG_ARGS_TYPE_NONE) {
        if (count == UINT8_MAX) {
            break;
        }
        ++count;
    }

    return count;
}

static size_t on9log_bounded_strlen(const char *str, size_t max_len)
{
    size_t len = 0;

    while (len < max_len && str[len] != '\0') {
        ++len;
    }

    return len;
}

static void on9log_uart_write(const on9log_stream_t *stream, const uint8_t *data, size_t len)
{
    if (!stream->uart_enabled) {
        return;
    }

    on9log_port_write(data, len);
}

static void on9log_stream_start(on9log_stream_t *stream, const uint8_t *header, size_t header_len)
{
    stream->uart_enabled = atomic_load_explicit(&s_uart_enabled, memory_order_relaxed);
    stream->sink_count = 0;

    for (size_t i = 0; i < ON9LOG_MAX_SINKS; ++i) {
        const on9log_sink_t *sink = (const on9log_sink_t *)atomic_load_explicit(&s_sinks[i].sink, memory_order_acquire);
        if (sink != NULL) {
            void *ctx = (void *)atomic_load_explicit(&s_sinks[i].ctx, memory_order_relaxed);
            stream->sinks[stream->sink_count].sink = sink;
            stream->sinks[stream->sink_count].ctx = ctx;
            ++stream->sink_count;
        }
    }

    on9log_uart_write(stream, header, header_len);

    for (size_t i = 0; i < stream->sink_count; ++i) {
        stream->sinks[i].sink->start_cb(header, header_len, stream->sinks[i].ctx);
    }
}

static void on9log_stream_payload(on9log_stream_t *stream,
                                  const uint8_t *payload,
                                  size_t payload_len,
                                  size_t total_arg_cnt,
                                  size_t curr_arg_index)
{
    if (payload_len == 0) {
        return;
    }

    on9log_uart_write(stream, payload, payload_len);

    for (size_t i = 0; i < stream->sink_count; ++i) {
        stream->sinks[i].sink->payload_cb(payload, payload_len, total_arg_cnt, curr_arg_index, stream->sinks[i].ctx);
    }
}

static void on9log_stream_end(on9log_stream_t *stream)
{
    for (size_t i = 0; i < stream->sink_count; ++i) {
        stream->sinks[i].sink->end_cb(stream->sinks[i].ctx);
    }
}

static void on9log_emit_encoder(on9log_stream_t *stream,
                                const on9log_encoder_t *enc,
                                size_t total_arg_cnt,
                                size_t curr_arg_index)
{
    if (!enc->overflow) {
        on9log_stream_payload(stream, enc->data, enc->len, total_arg_cnt, curr_arg_index);
    }
}

static void on9log_emit_u8(on9log_stream_t *stream, uint8_t val, size_t total_arg_cnt, size_t curr_arg_index)
{
    on9log_stream_payload(stream, &val, sizeof(val), total_arg_cnt, curr_arg_index);
}

static void on9log_emit_u32(on9log_stream_t *stream, uint32_t val, size_t total_arg_cnt, size_t curr_arg_index)
{
    uint8_t data[sizeof(uint32_t)];
    on9log_encoder_t enc = {
        .cap = sizeof(data),
        .data = data,
    };

    on9log_put_u32(&enc, val);
    on9log_emit_encoder(stream, &enc, total_arg_cnt, curr_arg_index);
}

static void on9log_emit_u64(on9log_stream_t *stream, uint64_t val, size_t total_arg_cnt, size_t curr_arg_index)
{
    uint8_t data[sizeof(uint64_t)];
    on9log_encoder_t enc = {
        .cap = sizeof(data),
        .data = data,
    };

    on9log_put_u64(&enc, val);
    on9log_emit_encoder(stream, &enc, total_arg_cnt, curr_arg_index);
}

static void on9log_emit_arg(on9log_stream_t *stream,
                            uint8_t arg_type,
                            va_list args,
                            size_t total_arg_cnt,
                            size_t curr_arg_index)
{
    switch (arg_type) {
    case ON9_LOG_ARGS_TYPE_32BITS: {
        uint32_t val = va_arg(args, uint32_t);
        on9log_emit_u32(stream, val, total_arg_cnt, curr_arg_index);
        return;
    }
    case ON9_LOG_ARGS_TYPE_64BITS: {
        uint64_t val = va_arg(args, uint64_t);
        on9log_emit_u64(stream, val, total_arg_cnt, curr_arg_index);
        return;
    }
    case ON9_LOG_ARGS_TYPE_POINTER: {
        const void *ptr = va_arg(args, const void *);
        on9log_emit_u32(stream, (uint32_t)(uintptr_t)ptr, total_arg_cnt, curr_arg_index);
        return;
    }
    case ON9_LOG_ARGS_TYPE_DYNAMIC_STRING: {
        const char *str = va_arg(args, const char *);
        if (str == NULL) {
            on9log_emit_u32(stream, ON9LOG_NULL_STRING_LEN, total_arg_cnt, curr_arg_index);
            return;
        }

        size_t str_len = on9log_bounded_strlen(str, UINT32_MAX - 1u);
        on9log_emit_u32(stream, (uint32_t)str_len, total_arg_cnt, curr_arg_index);
        if (str_len != 0) {
            on9log_stream_payload(stream, (const uint8_t *)str, str_len, total_arg_cnt, curr_arg_index);
        }
        return;
    }
    case ON9_LOG_ARGS_TYPE_NONE:
    default:
        return;
    }
}

static void on9log_emit_payload_args(on9log_stream_t *stream, const char *arg_types, va_list args, size_t total_arg_cnt)
{
    for (unsigned idx = 0; on9log_arg_type_at(arg_types, idx) != ON9_LOG_ARGS_TYPE_NONE; ++idx) {
        on9log_emit_arg(stream, on9log_arg_type_at(arg_types, idx), args, total_arg_cnt, idx);
    }
}

static void on9log_put_header(on9log_encoder_t *enc,
                              on9log_packet_type_t type,
                              on9log_level_t level,
                              uint16_t seq,
                              uint32_t time_ms,
                              uint32_t tag_id,
                              uint32_t fmt_id,
                              uint16_t payload_len)
{
    on9log_put_u8(enc, ON9LOG_PACKET_MAGIC);
    on9log_put_u8(enc, (uint8_t)((((uint8_t)type & 0x0fu) << 4u) | ((uint8_t)level & 0x0fu)));
    on9log_put_u16(enc, seq);
    on9log_put_u32(enc, time_ms);
    on9log_put_u32(enc, tag_id);
    on9log_put_u32(enc, fmt_id);
    on9log_put_u16(enc, payload_len);
}

static bool on9log_encode_header(uint8_t *header,
                                 on9log_packet_type_t type,
                                 on9log_level_t level,
                                 uint16_t seq,
                                 uint32_t time_ms,
                                 uint32_t tag_id,
                                 uint32_t fmt_id,
                                 uint16_t payload_len)
{
    on9log_encoder_t enc = {
        .cap = sizeof(on9log_packet_header_t),
        .data = header,
    };

    on9log_put_header(&enc, type, level, seq, time_ms, tag_id, fmt_id, payload_len);
    return !enc.overflow && enc.len == sizeof(on9log_packet_header_t);
}

static void on9log_emit_header(on9log_stream_t *stream,
                               on9log_packet_type_t type,
                               on9log_level_t level,
                               uint32_t tag_id,
                               uint32_t fmt_id,
                               uint16_t payload_len)
{
    uint8_t header[sizeof(on9log_packet_header_t)];

    if (on9log_encode_header(header,
                             type,
                             level,
                             (uint16_t)atomic_fetch_add_explicit(&s_seq, 1, memory_order_relaxed),
                             on9log_port_timestamp_ms(),
                             tag_id,
                             fmt_id,
                             payload_len)) {
        on9log_stream_start(stream, header, sizeof(header));
    }
}

static void on9log_emit_dropped_packet(uint32_t dropped_count)
{
    on9log_stream_t stream = {0};

    on9log_emit_header(&stream, ON9LOG_PKT_DROPPED, ON9_LOG_LEVEL_NONE, 0, 0, sizeof(uint32_t));
    on9log_emit_u32(&stream, dropped_count, 1, 0);
    on9log_stream_end(&stream);
}

static bool on9log_level_enabled(on9log_level_t level, const char *tag)
{
    (void)tag;

    if (level == ON9_LOG_LEVEL_NONE ||
        level > ON9_LOG_LEVEL_VERBOSE ||
        level > ON9_LOG_LOCAL_LEVEL) {
        return false;
    }
    return true;
}

on9log_err_t on9log_add_sink(const on9log_sink_t *sink, void *ctx)
{
    if (sink == NULL || sink->start_cb == NULL || sink->payload_cb == NULL || sink->end_cb == NULL) {
        return ON9LOG_ERR_INVALID_ARG;
    }

    on9log_port_lock();
    for (size_t i = 0; i < ON9LOG_MAX_SINKS; ++i) {
        const on9log_sink_t *slot_sink = (const on9log_sink_t *)atomic_load_explicit(&s_sinks[i].sink, memory_order_acquire);
        void *slot_ctx = (void *)atomic_load_explicit(&s_sinks[i].ctx, memory_order_relaxed);
        if (slot_sink == sink && slot_ctx == ctx) {
            on9log_port_unlock();
            return ON9LOG_OK;
        }
    }
    for (size_t i = 0; i < ON9LOG_MAX_SINKS; ++i) {
        if (atomic_load_explicit(&s_sinks[i].sink, memory_order_acquire) == (uintptr_t)NULL) {
            atomic_store_explicit(&s_sinks[i].ctx, (uintptr_t)ctx, memory_order_relaxed);
            atomic_store_explicit(&s_sinks[i].sink, (uintptr_t)sink, memory_order_release);
            on9log_port_unlock();
            return ON9LOG_OK;
        }
    }
    on9log_port_unlock();

    return ON9LOG_ERR_NO_MEM;
}

on9log_err_t on9log_remove_sink(const on9log_sink_t *sink, void *ctx)
{
    on9log_port_lock();
    for (size_t i = 0; i < ON9LOG_MAX_SINKS; ++i) {
        const on9log_sink_t *slot_sink = (const on9log_sink_t *)atomic_load_explicit(&s_sinks[i].sink, memory_order_acquire);
        void *slot_ctx = (void *)atomic_load_explicit(&s_sinks[i].ctx, memory_order_relaxed);
        if (slot_sink == sink && slot_ctx == ctx) {
            atomic_store_explicit(&s_sinks[i].sink, (uintptr_t)NULL, memory_order_release);
            atomic_store_explicit(&s_sinks[i].ctx, (uintptr_t)NULL, memory_order_relaxed);
            on9log_port_unlock();
            return ON9LOG_OK;
        }
    }
    on9log_port_unlock();

    return ON9LOG_ERR_NOT_FOUND;
}

void on9log_set_uart_enabled(bool enabled)
{
    atomic_store_explicit(&s_uart_enabled, enabled, memory_order_relaxed);
}

uint32_t on9log_get_dropped_count(void)
{
    return (uint32_t)atomic_load_explicit(&s_dropped_count, memory_order_relaxed);
}

void on9log_write(on9log_level_t level,
                  const char *tag,
                  const char *format,
                  const char *arg_types,
                  ...)
{
    uint32_t dropped_count = 0;
    on9log_stream_t stream = {0};

    if (!on9log_level_enabled(level, tag)) {
        return;
    }

    va_list args;
    va_start(args, arg_types);

    dropped_count = (uint32_t)atomic_exchange_explicit(&s_dropped_count, 0, memory_order_acq_rel);
    if (dropped_count != 0) {
        on9log_emit_dropped_packet(dropped_count);
    }

    uint8_t arg_count = on9log_arg_count(arg_types);

    on9log_emit_header(&stream,
                       ON9LOG_PKT_LOG,
                       level,
                       (uint32_t)(uintptr_t)tag,
                       (uint32_t)(uintptr_t)format,
                       ON9LOG_PAYLOAD_LEN_STREAMING);
    on9log_emit_u8(&stream, arg_count, arg_count, ON9LOG_PAYLOAD_META_ARG_INDEX);
    if (arg_count != 0) {
        on9log_stream_payload(&stream, (const uint8_t *)arg_types, arg_count, arg_count, ON9LOG_PAYLOAD_META_ARG_INDEX);
    }
    on9log_emit_payload_args(&stream, arg_types, args, arg_count);
    on9log_stream_end(&stream);

    va_end(args);
}

void on9log_write_buffer(on9log_level_t level,
                         const char *tag,
                         const void *buffer,
                         size_t buffer_len)
{
    uint32_t dropped_count = 0;
    on9log_stream_t stream = {0};
    const uint8_t *bytes = (const uint8_t *)buffer;

    if (!on9log_level_enabled(level, tag)) {
        return;
    }
    if (buffer_len > UINT32_MAX || (bytes == NULL && buffer_len != 0)) {
        atomic_fetch_add_explicit(&s_dropped_count, 1, memory_order_relaxed);
        return;
    }

    dropped_count = (uint32_t)atomic_exchange_explicit(&s_dropped_count, 0, memory_order_acq_rel);
    if (dropped_count != 0) {
        on9log_emit_dropped_packet(dropped_count);
    }

    uint32_t total_len = (uint32_t)buffer_len;

    on9log_emit_header(&stream,
                       ON9LOG_PKT_BUFFER,
                       level,
                       (uint32_t)(uintptr_t)tag,
                       0,
                       ON9LOG_PAYLOAD_LEN_STREAMING);
    on9log_emit_u32(&stream, total_len, 1, ON9LOG_PAYLOAD_META_ARG_INDEX);
    on9log_emit_u32(&stream, 0, 1, ON9LOG_PAYLOAD_META_ARG_INDEX);
    on9log_emit_u32(&stream, total_len, 1, ON9LOG_PAYLOAD_META_ARG_INDEX);
    if (total_len != 0) {
        on9log_stream_payload(&stream, bytes, total_len, 1, 0);
    }
    on9log_stream_end(&stream);
}
