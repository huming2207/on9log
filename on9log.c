#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/**
 * @file on9log.c
 * @brief Core implementation of the on9log logging library.
 *
 * Provides the encoding, filtering, sink-dispatch, and ISR-safe logging
 * primitives.  The public API is declared in @ref on9log.h.
 *
 * Architecture overview:
 *   - Log calls (on9log_write etc.) build a "stream" of payload chunks that
 *     are forwarded to every registered sink via callbacks (start / payload /
 *     end).
 *   - A header is emitted once per log packet; payloads carry the argument
 *     data.
 *   - ISR-safe logging serialises the full packet into a caller-provided
 *     buffer which is then enqueued into a platform-provided ringbuffer.
 *   - Tag-level filtering sits in front of the dispatch path to skip
 *     disabled log levels early.
 */

#include "on9log.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>

#if ON9LOG_PLAIN_TEXT
#include <stdio.h>
#include <sys/types.h>
#endif

#include "on9log_config.h"
#include "on9log_fmt.h"
#include "on9log_port.h"

/** @brief Sentinel value encoding a NULL dynamic string (length field). */
#define ON9LOG_NULL_STRING_LEN UINT32_MAX
/**
 * @brief Argument index sentinel used for payload meta-data that is not a
 *        regular log argument.
 */
#define ON9LOG_PAYLOAD_META_ARG_INDEX SIZE_MAX
/** @brief Compact bitset for precision-bounded string argument indexes. */
typedef uint32_t on9log_arg_mask_t;
#define ON9LOG_ARG_MASK_BITS ((unsigned)(sizeof(on9log_arg_mask_t) * 8u))

/**
 * @brief An atomic slot in the global sink table.
 *
 * Each slot holds a pointer to a @ref on9log_sink_t and its opaque context.
 * Writers publish @c ctx first and then publish @c sink with release ordering;
 * readers load @c sink with acquire ordering before reading @c ctx.  Removal
 * clears only @c sink, leaving @c ctx available for any in-flight reader that
 * already observed the old sink pointer.
 */
typedef struct {
    atomic_uintptr_t sink;      /**< Pointer to the sink descriptor (@ref on9log_sink_t). */
    atomic_uintptr_t ctx;       /**< Opaque user context passed to every sink callback. */
} on9log_sink_entry_t;

/**
 * @brief Lightweight byte-array encoder used to serialise header/payload data.
 *
 * Writing past the buffer capacity sets @ref overflow to @c true and all
 * subsequent writes are silently discarded — the caller checks the flag after
 * encoding.
 */
typedef struct {
    bool overflow;   /**< @c true if a write past @ref cap was attempted. */
    size_t len;      /**< Number of valid bytes in @ref data. */
    size_t cap;      /**< Capacity of @ref data in bytes. */
    uint8_t *data;   /**< Backing byte buffer (not owned by this struct). */
} on9log_encoder_t;

/**
 * @brief A single sink reference captured during a stream snapshot.
 *
 * This pairs a sink descriptor with its context so the stream iteration is
 * fast and does not touch the atomic slot table on every payload call.
 */
typedef struct {
    const on9log_sink_t *sink; /**< The sink descriptor. */
    void *ctx;                 /**< Opaque user context for the sink. */
} on9log_stream_sink_t;

/**
 * @brief Per-log-call stream state.
 *
 * A snapshot of the sink table is taken once at stream-start; this avoids
 * holding a lock during the entire multi-payload output sequence.
 */
typedef struct {
    size_t sink_count;                    /**< Number of sinks captured in @ref sinks. */
    on9log_stream_sink_t sinks[ON9LOG_MAX_SINKS]; /**< Captured sink table. */
} on9log_stream_t;

/**
 * @brief A node in the per-tag filter linked list.
 *
 * Each node stores a tag pointer, a minimum log level, and an active flag.
 * The list is walked atomically (via @ref s_tag_filter_head) by the
 * fast-path filter check.
 */
typedef struct on9log_tag_filter_node {
    SLIST_ENTRY(on9log_tag_filter_node) entries; /**< BSD sys/queue list entry. */
    const char *tag;                              /**< Tag string (pointer-identity or strcmp match). */
    atomic_int level;                             /**< Minimum enabled level for this tag. */
    atomic_bool active;                           /**< @c true while this node is in the live filter list. */
} on9log_tag_filter_node_t;

/** @brief Type alias for the BSD sys/queue singly-linked list header. */
SLIST_HEAD(on9log_tag_filter_list, on9log_tag_filter_node);

/**
 * @brief Global sink table.
 *
 * Each slot can hold one sink.  Slots are read with acquire ordering and
 * written with release ordering for lock-free ISR safety.
 */
static on9log_sink_entry_t s_sinks[ON9LOG_MAX_SINKS];

/** @brief Head of the per-tag filter linked list (protected by on9log_port_lock). */
static struct on9log_tag_filter_list s_tag_filters = SLIST_HEAD_INITIALIZER(s_tag_filters);

/**
 * @brief Atomic snapshot of the filter list head for lock-free reader access.
 *
 * Updated every time the filter list is modified.
 */
static atomic_uintptr_t s_tag_filter_head = ATOMIC_VAR_INIT((uintptr_t)NULL);

/** @brief Number of active (non-deactivated) tag filter nodes (atomic counter). */
static atomic_uint_fast32_t s_active_tag_filter_count = ATOMIC_VAR_INIT(0);

/** @brief Default minimum log level (applied when no per-tag filter matches). */
static atomic_int s_default_level = ATOMIC_VAR_INIT(ON9_LOG_LEVEL_VERBOSE);

/** @brief Monotonically increasing packet sequence number. */
static atomic_uint_fast16_t s_seq = ATOMIC_VAR_INIT(0);

/** @brief Pending dropped-packet count since the last dropped notification. */
static atomic_uint_fast32_t s_dropped_count = ATOMIC_VAR_INIT(0);

/**
 * @brief Write a single uint8_t to the encoder buffer.
 *
 * Sets the overflow flag if the buffer is already full.
 *
 * @param[in,out] enc  Encoder state.
 * @param[in]     val  Value to write.
 */
static void on9log_put_u8(on9log_encoder_t *enc, uint8_t val)
{
    if (enc->len >= enc->cap) {
        enc->overflow = true;
        return;
    }

    enc->data[enc->len++] = val;
}

/**
 * @brief Write a uint16_t in little-endian byte order to the encoder.
 *
 * @param[in,out] enc  Encoder state.
 * @param[in]     val  Value to write.
 */
static void on9log_put_u16(on9log_encoder_t *enc, uint16_t val)
{
    on9log_put_u8(enc, (uint8_t)(val & 0xffu));
    on9log_put_u8(enc, (uint8_t)(val >> 8u));
}

/**
 * @brief Write a uint32_t in little-endian byte order to the encoder.
 *
 * @param[in,out] enc  Encoder state.
 * @param[in]     val  Value to write.
 */
static void on9log_put_u32(on9log_encoder_t *enc, uint32_t val)
{
    on9log_put_u8(enc, (uint8_t)(val & 0xffu));
    on9log_put_u8(enc, (uint8_t)((val >> 8u) & 0xffu));
    on9log_put_u8(enc, (uint8_t)((val >> 16u) & 0xffu));
    on9log_put_u8(enc, (uint8_t)(val >> 24u));
}

/**
 * @brief Write a uint64_t in little-endian byte order to the encoder.
 *
 * @param[in,out] enc  Encoder state.
 * @param[in]     val  Value to write.
 */
static void on9log_put_u64(on9log_encoder_t *enc, uint64_t val)
{
    on9log_put_u32(enc, (uint32_t)(val & 0xffffffffu));
    on9log_put_u32(enc, (uint32_t)(val >> 32u));
}

static void on9log_put_bytes(on9log_encoder_t *enc, const void *data, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;

    if (len == 0) {
        return;
    }
    if (len > enc->cap - enc->len) {
        enc->overflow = true;
        return;
    }
    memcpy(&enc->data[enc->len], bytes, len);
    enc->len += len;
}

/**
 * @brief Look up the argument type at a given index.
 *
 * @param[in] arg_types  Packed argument-type string, or NULL.
 * @param[in] idx        Zero-based argument index.
 *
 * @return The type byte at @c arg_types[idx], or @c ON9_LOG_ARGS_TYPE_NONE
 *         if @c arg_types is NULL.
 */
static uint8_t on9log_arg_type_at(const char *arg_types, unsigned idx)
{
    if (arg_types == NULL) {
        return ON9_LOG_ARGS_TYPE_NONE;
    }
    return (uint8_t)arg_types[idx];
}

/**
 * @brief Count the number of arguments described by a type string.
 *
 * Iterates until the first @c ON9_LOG_ARGS_TYPE_NONE sentinel, up to
 * @c UINT8_MAX.
 *
 * @param[in] arg_types  Packed argument-type string (NULL yields 0).
 *
 * @return Number of non-sentinel argument type bytes.
 */
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

/**
 * @brief Safe bounded string length computation.
 *
 * Scans @c str for a NUL terminator, stopping at @c max_len bytes to avoid
 * unbounded access on non-NULL-terminated inputs.
 *
 * @param[in] str     String to measure (may be NULL — returns 0).
 * @param[in] max_len Maximum number of bytes to check.
 *
 * @return The string length, capped at @c max_len.
 */
static size_t on9log_bounded_strlen(const char *str, size_t max_len)
{
    size_t len = 0;

    while (len < max_len && str[len] != '\0') {
        ++len;
    }

    return len;
}

/**
 * @brief Begin a new log stream: snapshot sink table and emit the header.
 *
 * Reads the atomic sink table (with acquire ordering), captures every
 * non-NULL slot into the stream structure, and forwards the header bytes to
 * every captured sink's @c start_cb.
 *
 * @param[out] stream      Stream state to initialise.
 * @param[in]  header      Packet header bytes, or NULL for plain text.
 * @param[in]  header_len  Header length, or zero for plain text.
 */
static void on9log_stream_start(on9log_stream_t *stream, const uint8_t *header, size_t header_len)
{
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

    for (size_t i = 0; i < stream->sink_count; ++i) {
        stream->sinks[i].sink->start_cb(header, header_len, stream->sinks[i].ctx);
    }
}

/**
 * @brief Emit a payload chunk through the current stream.
 *
 * The payload bytes are forwarded to every captured sink's @c payload_cb.
 *
 * @param[in] stream          Stream snapshot.
 * @param[in] payload         Pointer to the payload bytes.
 * @param[in] payload_len     Number of payload bytes.
 * @param[in] total_arg_cnt   Total number of arguments expected in this frame.
 * @param[in] curr_arg_index  Index of the current argument (or @ref ON9LOG_PAYLOAD_META_ARG_INDEX for meta-data).
 */
static void on9log_stream_payload(on9log_stream_t *stream,
                                  const uint8_t *payload,
                                  size_t payload_len,
                                  size_t total_arg_cnt,
                                  size_t curr_arg_index)
{
    if (payload_len == 0) {
        return;
    }

    for (size_t i = 0; i < stream->sink_count; ++i) {
        stream->sinks[i].sink->payload_cb(payload, payload_len, total_arg_cnt, curr_arg_index, stream->sinks[i].ctx);
    }
}

/**
 * @brief End a log stream: call every captured sink's @c end_cb.
 *
 * @param[in] stream  Stream snapshot.
 */
static void on9log_stream_end(on9log_stream_t *stream)
{
    for (size_t i = 0; i < stream->sink_count; ++i) {
        stream->sinks[i].sink->end_cb(stream->sinks[i].ctx);
    }
}

#if ON9LOG_PLAIN_TEXT
#define ON9LOG_ANSI_RED     "\033[0;31m"
#define ON9LOG_ANSI_GREEN   "\033[0;32m"
#define ON9LOG_ANSI_YELLOW  "\033[0;33m"
#define ON9LOG_ANSI_CYAN    "\033[0;36m"
#define ON9LOG_ANSI_WHITE   "\033[0;37m"
#define ON9LOG_ANSI_RESET   "\033[0m"

static void on9log_text_stream_output(const char *data, size_t len, void *ctx)
{
    on9log_stream_payload((on9log_stream_t *)ctx,
                          (const uint8_t *)data,
                          len,
                          0,
                          ON9LOG_PAYLOAD_META_ARG_INDEX);
}

static void on9log_text_encoder_output(const char *data, size_t len, void *ctx)
{
    on9log_put_bytes((on9log_encoder_t *)ctx, data, len);
}

static const char *on9log_text_level_color(on9log_level_t level)
{
    switch (level) {
    case ON9_LOG_LEVEL_ERROR:
        return ON9LOG_ANSI_RED;
    case ON9_LOG_LEVEL_WARN:
        return ON9LOG_ANSI_YELLOW;
    case ON9_LOG_LEVEL_INFO:
        return ON9LOG_ANSI_GREEN;
    case ON9_LOG_LEVEL_DEBUG:
        return ON9LOG_ANSI_CYAN;
    case ON9_LOG_LEVEL_VERBOSE:
    default:
        return ON9LOG_ANSI_WHITE;
    }
}

static char on9log_text_level_letter(on9log_level_t level)
{
    static const char letters[] = "?EWIDV";
    return (level >= ON9_LOG_LEVEL_ERROR && level <= ON9_LOG_LEVEL_VERBOSE)
               ? letters[level]
               : '?';
}

static void on9log_text_output_u32(on9log_text_output_cb_t output,
                                   void *output_ctx,
                                   uint32_t value)
{
    char digits[10];
    size_t len = 0;

    do {
        digits[len++] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value != 0u);

    for (size_t left = 0, right = len - 1; left < right; ++left, --right) {
        char tmp = digits[left];
        digits[left] = digits[right];
        digits[right] = tmp;
    }
    output(digits, len, output_ctx);
}

static void on9log_text_output_prefix(on9log_text_output_cb_t output,
                                      void *output_ctx,
                                      on9log_level_t level,
                                      uint32_t timestamp_ms,
                                      const char *tag)
{
    const char *color = on9log_text_level_color(level);
    const char level_letter = on9log_text_level_letter(level);
    const char *safe_tag = tag != NULL ? tag : "";

    output(color, strlen(color), output_ctx);
    output(&level_letter, sizeof(level_letter), output_ctx);
    output(" (", 2, output_ctx);
    on9log_text_output_u32(output, output_ctx, timestamp_ms);
    output(") ", 2, output_ctx);
    output(safe_tag, strlen(safe_tag), output_ctx);
    output(": ", 2, output_ctx);
}

static void on9log_text_output_suffix(on9log_text_output_cb_t output, void *output_ctx)
{
    static const char suffix[] = ON9LOG_ANSI_RESET "\n";
    output(suffix, sizeof(suffix) - 1u, output_ctx);
}

static void on9log_dispatch_text_line(on9log_level_t level,
                                      const char *tag,
                                      uint32_t timestamp_ms,
                                      on9log_text_formatter_cb_t formatter,
                                      void *formatter_ctx)
{
    on9log_stream_t stream = {0};

    on9log_stream_start(&stream, NULL, 0);
    on9log_text_output_prefix(on9log_text_stream_output, &stream, level, timestamp_ms, tag);
    if (formatter != NULL) {
        formatter(on9log_text_stream_output, &stream, formatter_ctx);
    }
    on9log_text_output_suffix(on9log_text_stream_output, &stream);
    on9log_stream_end(&stream);
}
#endif

/**
 * @brief Emit the contents of an encoder as a payload chunk (if no overflow).
 *
 * @param[in] stream          Stream snapshot.
 * @param[in] enc             Encoder state (overflow flag is checked).
 * @param[in] total_arg_cnt   Total argument count.
 * @param[in] curr_arg_index  Current argument index.
 */
static void on9log_emit_encoder(on9log_stream_t *stream,
                                const on9log_encoder_t *enc,
                                size_t total_arg_cnt,
                                size_t curr_arg_index)
{
    if (!enc->overflow) {
        on9log_stream_payload(stream, enc->data, enc->len, total_arg_cnt, curr_arg_index);
    }
}

/**
 * @brief Emit a single uint8_t as a payload chunk.
 *
 * @param[in] stream          Stream snapshot.
 * @param[in] val             Value to emit.
 * @param[in] total_arg_cnt   Total argument count.
 * @param[in] curr_arg_index  Current argument index.
 */
static void on9log_emit_u8(on9log_stream_t *stream, uint8_t val, size_t total_arg_cnt, size_t curr_arg_index)
{
    on9log_stream_payload(stream, &val, sizeof(val), total_arg_cnt, curr_arg_index);
}

/**
 * @brief Emit a uint32_t (little-endian) as a payload chunk.
 *
 * @param[in] stream          Stream snapshot.
 * @param[in] val             Value to emit.
 * @param[in] total_arg_cnt   Total argument count.
 * @param[in] curr_arg_index  Current argument index.
 */
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

/**
 * @brief Emit a uint64_t (little-endian) as a payload chunk.
 *
 * @param[in] stream          Stream snapshot.
 * @param[in] val             Value to emit.
 * @param[in] total_arg_cnt   Total argument count.
 * @param[in] curr_arg_index  Current argument index.
 */
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

static void on9log_emit_string_bytes(on9log_stream_t *stream,
                                     const char *str,
                                     size_t str_len,
                                     size_t total_arg_cnt,
                                     size_t curr_arg_index)
{
    on9log_emit_u32(stream, (uint32_t)str_len, total_arg_cnt, curr_arg_index);
    if (str_len != 0) {
        on9log_stream_payload(stream, (const uint8_t *)str, str_len, total_arg_cnt, curr_arg_index);
    }
}

static void on9log_emit_cstr(on9log_stream_t *stream,
                             const char *str,
                             size_t total_arg_cnt,
                             size_t curr_arg_index)
{
    if (str == NULL) {
        on9log_emit_u32(stream, ON9LOG_NULL_STRING_LEN, total_arg_cnt, curr_arg_index);
        return;
    }

    size_t str_len = on9log_bounded_strlen(str, ON9LOG_MAX_DYNAMIC_STRING_LEN);
    on9log_emit_string_bytes(stream, str, str_len, total_arg_cnt, curr_arg_index);
}

static void on9log_emit_precision_string(on9log_stream_t *stream,
                                         const char *str,
                                         int32_t precision,
                                         size_t total_arg_cnt,
                                         size_t curr_arg_index)
{
    if (precision < 0) {
        on9log_emit_cstr(stream, str, total_arg_cnt, curr_arg_index);
        return;
    }
    if (str == NULL) {
        on9log_emit_u32(stream, ON9LOG_NULL_STRING_LEN, total_arg_cnt, curr_arg_index);
        return;
    }

    size_t actual_len = on9log_bounded_strlen(str, ON9LOG_MAX_DYNAMIC_STRING_LEN);
    size_t str_len = ((size_t)precision < actual_len) ? (size_t)precision : actual_len;
    on9log_emit_string_bytes(stream, str, str_len, total_arg_cnt, curr_arg_index);
}

/**
 * @brief Emit a single typed argument to the stream.
 *
 * Handles all four argument types:
 *   - @c ON9_LOG_ARGS_TYPE_32BITS: emit a uint32_t.
 *   - @c ON9_LOG_ARGS_TYPE_64BITS: emit a uint64_t.
 *   - @c ON9_LOG_ARGS_TYPE_POINTER: emit the pointer as uint32_t (truncated on 64-bit platforms).
 *   - @c ON9_LOG_ARGS_TYPE_DYNAMIC_STRING: emit the string length followed by the string bytes.
 *   - @c ON9_LOG_ARGS_TYPE_DYNAMIC_STRING_VIEW: emit the descriptor length followed by the bytes.
 *
 * @param[in] stream          Stream snapshot.
 * @param[in] arg_type        Argument type code.
 * @param[in] args            Variable argument list pointer (consumed from).
 * @param[in] total_arg_cnt   Total number of arguments expected in this frame.
 * @param[in] curr_arg_index  Index of the current argument.
 */
static void on9log_emit_arg(on9log_stream_t *stream,
                            uint8_t arg_type,
                            va_list *args,
                            size_t total_arg_cnt,
                            size_t curr_arg_index)
{
    switch (arg_type) {
    case ON9_LOG_ARGS_TYPE_32BITS: {
        uint32_t val = va_arg(*args, uint32_t);
        on9log_emit_u32(stream, val, total_arg_cnt, curr_arg_index);
        return;
    }
    case ON9_LOG_ARGS_TYPE_64BITS: {
        uint64_t val = va_arg(*args, uint64_t);
        on9log_emit_u64(stream, val, total_arg_cnt, curr_arg_index);
        return;
    }
    case ON9_LOG_ARGS_TYPE_POINTER: {
        const void *ptr = va_arg(*args, const void *);
        on9log_emit_u32(stream, (uint32_t)(uintptr_t)ptr, total_arg_cnt, curr_arg_index);
        return;
    }
    case ON9_LOG_ARGS_TYPE_DYNAMIC_STRING: {
        const char *str = va_arg(*args, const char *);
        on9log_emit_cstr(stream, str, total_arg_cnt, curr_arg_index);
        return;
    }
    case ON9_LOG_ARGS_TYPE_DYNAMIC_STRING_VIEW: {
        const on9log_string_view_t *str = va_arg(*args, const on9log_string_view_t *);
        if (str == NULL || (str->data == NULL && str->len != 0)) {
            on9log_emit_u32(stream, ON9LOG_NULL_STRING_LEN, total_arg_cnt, curr_arg_index);
            return;
        }

        size_t str_len = str->len;
        if (str_len > ON9LOG_MAX_DYNAMIC_STRING_LEN) {
            str_len = ON9LOG_MAX_DYNAMIC_STRING_LEN;
        }
        on9log_emit_string_bytes(stream, str->data, str_len, total_arg_cnt, curr_arg_index);
        return;
    }
    case ON9_LOG_ARGS_TYPE_NONE:
    default:
        return;
    }
}

/**
 * @brief Emit all arguments described by the type string to the stream.
 *
 * Iterates through the argument types and delegates each to
 * @ref on9log_emit_arg.
 *
 * @param[in] stream        Stream snapshot.
 * @param[in] arg_types     Packed argument-type string.
 * @param[in] args          Variable argument list pointer.
 * @param[in] total_arg_cnt Total number of arguments (passed to each emit).
 */
static void on9log_emit_payload_args(on9log_stream_t *stream,
                                     const char *arg_types,
                                     on9log_arg_mask_t precision_string_arg_mask,
                                     va_list *args,
                                     size_t total_arg_cnt)
{
    uint8_t prev_arg_type = ON9_LOG_ARGS_TYPE_NONE;
    uint32_t prev_u32 = 0;

    for (unsigned idx = 0; idx < total_arg_cnt && on9log_arg_type_at(arg_types, idx) != ON9_LOG_ARGS_TYPE_NONE; ++idx) {
        uint8_t arg_type = on9log_arg_type_at(arg_types, idx);
        switch (arg_type) {
        case ON9_LOG_ARGS_TYPE_32BITS:
            prev_u32 = va_arg(*args, uint32_t);
            on9log_emit_u32(stream, prev_u32, total_arg_cnt, idx);
            break;
        case ON9_LOG_ARGS_TYPE_DYNAMIC_STRING:
            if (idx < ON9LOG_ARG_MASK_BITS &&
                (precision_string_arg_mask & ((on9log_arg_mask_t)1u << idx)) != 0u &&
                prev_arg_type == ON9_LOG_ARGS_TYPE_32BITS) {
                const char *str = va_arg(*args, const char *);
                on9log_emit_precision_string(stream, str, (int32_t)prev_u32, total_arg_cnt, idx);
            } else {
                on9log_emit_arg(stream, arg_type, args, total_arg_cnt, idx);
            }
            break;
        default:
            on9log_emit_arg(stream, arg_type, args, total_arg_cnt, idx);
            break;
        }
        prev_arg_type = arg_type;
    }
}

/**
 * @brief Encode a packet header into the encoder.
 *
 * Wire format:
 *   - Magic byte        (1B)
 *   - Type|Level nibble (1B)
 *   - Sequence number   (2B, LE)
 *   - Timestamp         (4B, LE)
 *   - Tag ID            (4B, LE)
 *   - Format ID         (4B, LE)
 *   - Payload length    (2B, LE)
 *
 * @param[in,out] enc         Encoder to write into.
 * @param[in]     type        Packet type (on9log_packet_type_t).
 * @param[in]     level       Log level.
 * @param[in]     seq         Sequence number.
 * @param[in]     time_ms     Timestamp in milliseconds.
 * @param[in]     tag_id      Tag identifier (typically a pointer cast to uint32_t).
 * @param[in]     fmt_id      Format string identifier (typically a pointer cast to uint32_t).
 * @param[in]     payload_len Payload length (may be @c ON9LOG_PAYLOAD_LEN_STREAMING).
 */
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

/**
 * @brief Encode a packet header into a caller-provided header buffer.
 *
 * @param[out] header      Buffer of at least @c sizeof(on9log_packet_header_t) bytes.
 * @param[in]  type        Packet type.
 * @param[in]  level       Log level.
 * @param[in]  seq         Sequence number.
 * @param[in]  time_ms     Timestamp in milliseconds.
 * @param[in]  tag_id      Tag identifier.
 * @param[in]  fmt_id      Format identifier.
 * @param[in]  payload_len Payload length.
 *
 * @return @c true if the header was fully written without overflow.
 */
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

/**
 * @brief Emit a complete packet header and begin the stream.
 *
 * Allocates a temporary header buffer, fills it with the encoded header
 * (including an auto-incremented sequence number and the current timestamp),
 * and calls @ref on9log_stream_start.
 *
 * @param[in,out] stream      Stream state to initialise.
 * @param[in]     type        Packet type.
 * @param[in]     level       Log level.
 * @param[in]     tag_id      Tag identifier.
 * @param[in]     fmt_id      Format identifier.
 * @param[in]     payload_len Payload length (may be @c ON9LOG_PAYLOAD_LEN_STREAMING).
 */
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

/**
 * @brief Check whether all argument types are safe to encode in an ISR context.
 *
 * Dynamic strings are not ISR-safe because reading user memory from an ISR
 * can trigger a page fault or take too long.
 *
 * @param[in] arg_types  Packed argument-type string.
 *
 * @return @c true if all types are @c _32BITS, @c _64BITS, or @c _POINTER.
 *         @c false if any dynamic string type or unknown type is present.
 */
static bool on9log_arg_types_are_isr_safe(const char *arg_types)
{
    for (unsigned idx = 0;; ++idx) {
        uint8_t arg_type = on9log_arg_type_at(arg_types, idx);
        switch (arg_type) {
        case ON9_LOG_ARGS_TYPE_NONE:
            return true;
        case ON9_LOG_ARGS_TYPE_32BITS:
        case ON9_LOG_ARGS_TYPE_64BITS:
        case ON9_LOG_ARGS_TYPE_POINTER:
            break;
        case ON9_LOG_ARGS_TYPE_DYNAMIC_STRING:
        case ON9_LOG_ARGS_TYPE_DYNAMIC_STRING_VIEW:
        default:
            return false;
        }
        if (idx == UINT8_MAX) {
            return false;
        }
    }
}

/**
 * @brief Serialise a complete log packet into a pre-allocated buffer for ISR
 *        use.
 *
 * Encodes the header (with ISR-safe timestamp), argument count and type bytes,
 * and all argument values.  Dynamic-string arguments cause encoding to fail
 * (overflow set) because they are not ISR-safe.
 *
 * @param[out] packet      Buffer to write the serialised packet into.
 * @param[in]  packet_cap  Capacity of @c packet in bytes.
 * @param[out] packet_len  Receives the actual packet length on success.
 * @param[in]  level       Log level.
 * @param[in]  tag         Tag string (stored as pointer).
 * @param[in]  format      Format string (stored as pointer).
 * @param[in]  arg_types   Packed argument-type string.
 * @param[in]  args        Variable argument list to consume.
 *
 * @return @c true on success, @c false if encoding overflowed (e.g. dynamic
 *         string encountered or buffer too small).
 */
static bool on9log_encode_isr_log_packet(uint8_t *packet,
                                         size_t packet_cap,
                                         size_t *packet_len,
                                         on9log_level_t level,
                                         const char *tag,
                                         const char *format,
                                         const char *arg_types,
                                         va_list *args)
{
    on9log_encoder_t enc = {
        .cap = packet_cap,
        .data = packet,
    };
    uint8_t arg_count = on9log_arg_count(arg_types);

    on9log_put_header(&enc,
                      ON9LOG_PKT_LOG,
                      level,
                      (uint16_t)atomic_fetch_add_explicit(&s_seq, 1, memory_order_relaxed),
                      on9log_port_isr_timestamp_ms(),
                      (uint32_t)(uintptr_t)tag,
                      (uint32_t)(uintptr_t)format,
                      ON9LOG_PAYLOAD_LEN_STREAMING);
    on9log_put_u8(&enc, arg_count);
    for (unsigned idx = 0; idx < arg_count; ++idx) {
        on9log_put_u8(&enc, on9log_arg_type_at(arg_types, idx));
    }

    for (unsigned idx = 0; idx < arg_count; ++idx) {
        switch (on9log_arg_type_at(arg_types, idx)) {
        case ON9_LOG_ARGS_TYPE_32BITS:
            on9log_put_u32(&enc, va_arg(*args, uint32_t));
            break;
        case ON9_LOG_ARGS_TYPE_64BITS:
            on9log_put_u64(&enc, va_arg(*args, uint64_t));
            break;
        case ON9_LOG_ARGS_TYPE_POINTER: {
            const void *ptr = va_arg(*args, const void *);
            on9log_put_u32(&enc, (uint32_t)(uintptr_t)ptr);
            break;
        }
        case ON9_LOG_ARGS_TYPE_NONE:
        case ON9_LOG_ARGS_TYPE_DYNAMIC_STRING:
        case ON9_LOG_ARGS_TYPE_DYNAMIC_STRING_VIEW:
        default:
            enc.overflow = true;
            break;
        }
    }

    if (enc.overflow) {
        return false;
    }

    *packet_len = enc.len;
    return true;
}

/**
 * @brief Emit a "dropped packets" notification packet (type @c ON9LOG_PKT_DROPPED).
 *
 * Sent to all sinks when at least one packet has been dropped since the last
 * successful dispatch.
 *
 * @param[in] dropped_count  Number of packets dropped.
 */
#if ON9LOG_PLAIN_TEXT
typedef struct {
    uint32_t count;
} on9log_dropped_text_ctx_t;

static void on9log_dropped_text_formatter(on9log_text_output_cb_t output,
                                          void *output_ctx,
                                          void *formatter_ctx)
{
    const on9log_dropped_text_ctx_t *ctx = (const on9log_dropped_text_ctx_t *)formatter_ctx;
    on9log_text_output_u32(output, output_ctx, ctx->count);
    output(" log message(s) dropped", sizeof(" log message(s) dropped") - 1u, output_ctx);
}
#endif

static void on9log_emit_dropped_packet(uint32_t dropped_count)
{
#if ON9LOG_PLAIN_TEXT
    on9log_dropped_text_ctx_t ctx = {.count = dropped_count};
    on9log_dispatch_text_line(ON9_LOG_LEVEL_WARN,
                              "on9log",
                              on9log_port_timestamp_ms(),
                              on9log_dropped_text_formatter,
                              &ctx);
#else
    on9log_stream_t stream = {0};

    on9log_emit_header(&stream, ON9LOG_PKT_DROPPED, ON9_LOG_LEVEL_NONE, 0, 0, sizeof(uint32_t));
    on9log_emit_u32(&stream, dropped_count, 1, 0);
    on9log_stream_end(&stream);
#endif
}

/**
 * @brief Emit a buffer-data packet (type @c ON9LOG_PKT_BUFFER).
 *
 * Used by @ref on9log_write_buffer to transmit raw binary data.  Large
 * buffers are split into multiple packets; this function emits one chunk.
 *
 * @param[in] level      Log level.
 * @param[in] tag        Tag string (stored as pointer).
 * @param[in] bytes      Pointer to the buffer bytes.
 * @param[in] total_len  Total buffer length across all chunks.
 * @param[in] offset     Byte offset of this chunk in the original buffer.
 * @param[in] chunk_len  Length of this chunk in bytes.
 */
static void on9log_emit_buffer_packet(on9log_level_t level,
                                       const char *tag,
                                       const uint8_t *bytes,
                                       uint32_t total_len,
                                       uint32_t offset,
                                       uint32_t chunk_len)
{
    on9log_stream_t stream = {0};

    on9log_emit_header(&stream,
                       ON9LOG_PKT_BUFFER,
                       level,
                       (uint32_t)(uintptr_t)tag,
                       0,
                       ON9LOG_PAYLOAD_LEN_STREAMING);
    on9log_emit_u32(&stream, total_len, 1, ON9LOG_PAYLOAD_META_ARG_INDEX);
    on9log_emit_u32(&stream, offset, 1, ON9LOG_PAYLOAD_META_ARG_INDEX);
    on9log_emit_u32(&stream, chunk_len, 1, ON9LOG_PAYLOAD_META_ARG_INDEX);
    if (chunk_len != 0) {
        on9log_stream_payload(&stream, &bytes[offset], chunk_len, 1, 0);
    }
    on9log_stream_end(&stream);
}

/**
 * @brief Validate a log level value.
 *
 * @param[in] level  Level to validate.
 *
 * @return @c true if level is in the range @c ON9_LOG_LEVEL_NONE ..
 *         @c ON9_LOG_LEVEL_VERBOSE.
 */
static bool on9log_valid_level(on9log_level_t level)
{
    return level >= ON9_LOG_LEVEL_NONE && level <= ON9_LOG_LEVEL_VERBOSE;
}

/**
 * @brief Check whether a configured tag string matches a given tag.
 *
 * Performs pointer-identity comparison first, then falls back to @c strcmp.
 *
 * @param[in] configured  Tag from the filter list (may be NULL).
 * @param[in] tag         Tag to match against (may be NULL).
 *
 * @return @c true if the tags match.
 */
static bool on9log_tag_matches(const char *configured, const char *tag)
{
    if (configured == tag) {
        return true;
    }
    if (configured == NULL || tag == NULL) {
        return false;
    }
    return strcmp(configured, tag) == 0;
}

/**
 * @brief Determine the effective minimum log level for a given tag.
 *
 * If no per-tag filters are active, returns the default level.  Otherwise
 * walks the atomic filter list snapshot and returns the level of the first
 * matching active node, falling back to the default level.
 *
 * @param[in] tag  Tag string to look up.
 *
 * @return The minimum enabled log level for this tag.
 */
static on9log_level_t on9log_filter_level_for_tag(const char *tag)
{
    if (atomic_load_explicit(&s_active_tag_filter_count, memory_order_acquire) == 0) {
        return (on9log_level_t)atomic_load_explicit(&s_default_level, memory_order_relaxed);
    }

    const on9log_tag_filter_node_t *node = (const on9log_tag_filter_node_t *)atomic_load_explicit(
        &s_tag_filter_head,
        memory_order_acquire);
    while (node != NULL) {
        if (atomic_load_explicit(&node->active, memory_order_acquire) &&
            on9log_tag_matches(node->tag, tag)) {
            return (on9log_level_t)atomic_load_explicit(&node->level, memory_order_relaxed);
        }
        node = SLIST_NEXT(node, entries);
    }

    return (on9log_level_t)atomic_load_explicit(&s_default_level, memory_order_relaxed);
}

/**
 * @brief Check whether a given log level is enabled for a given tag.
 *
 * The check combines:
 * - Compile-time filtering via @c ON9_LOG_LOCAL_LEVEL.
 * - Runtime per-tag filtering via configured tag levels.
 * - Runtime default level.
 *
 * @param[in] level  Requested log level.
 * @param[in] tag    Tag string.
 *
 * @return @c true if the level is enabled.
 */
static bool on9log_level_enabled(on9log_level_t level, const char *tag)
{
    on9log_level_t runtime_level;

    if (level == ON9_LOG_LEVEL_NONE ||
        level > ON9_LOG_LEVEL_VERBOSE ||
        level > ON9_LOG_LOCAL_LEVEL) {
        return false;
    }

    runtime_level = on9log_filter_level_for_tag(tag);
    if (!on9log_valid_level(runtime_level) || runtime_level == ON9_LOG_LEVEL_NONE) {
        return false;
    }
    return level <= runtime_level;
}

#if ON9LOG_PLAIN_TEXT
void on9log_write_text(on9log_level_t level,
                       const char *tag,
                       on9log_text_formatter_cb_t formatter,
                       void *formatter_ctx)
{
    if (!on9log_level_enabled(level, tag)) {
        return;
    }

    uint32_t dropped_count =
        (uint32_t)atomic_exchange_explicit(&s_dropped_count, 0, memory_order_acq_rel);
    if (dropped_count != 0) {
        on9log_emit_dropped_packet(dropped_count);
    }

    on9log_dispatch_text_line(level,
                              tag,
                              on9log_port_timestamp_ms(),
                              formatter,
                              formatter_ctx);
}
#endif

/**
 * @brief Register a log sink.
 *
 * Adds the sink to the global atomic sink table.  Duplicate (sink, ctx)
 * pairs are silently accepted (returns @c ON9LOG_OK).  Thread-safe.
 *
 * @param[in] sink  Pointer to the sink descriptor.  Must have all three
 *                  callbacks (@c start_cb, @c payload_cb, @c end_cb) set.
 * @param[in] ctx   Opaque user context passed to every sink callback.
 *
 * @return
 * - @c ON9LOG_OK on success.
 * - @c ON9LOG_ERR_INVALID_ARG if sink or any callback pointer is NULL.
 * - @c ON9LOG_ERR_NO_MEM if the sink table is full.
 */
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

/**
 * @brief Remove a previously registered log sink.
 *
 * Finds the matching (sink, ctx) entry in the sink table and clears the slot
 * with release ordering so that concurrent readers see the update.
 *
 * @param[in] sink  Pointer to the sink descriptor.
 * @param[in] ctx   Opaque user context that was passed to @ref on9log_add_sink.
 *
 * @return
 * - @c ON9LOG_OK on success.
 * - @c ON9LOG_ERR_NOT_FOUND if the (sink, ctx) pair is not in the table.
 */
on9log_err_t on9log_remove_sink(const on9log_sink_t *sink, void *ctx)
{
    on9log_port_lock();
    for (size_t i = 0; i < ON9LOG_MAX_SINKS; ++i) {
        const on9log_sink_t *slot_sink = (const on9log_sink_t *)atomic_load_explicit(&s_sinks[i].sink, memory_order_acquire);
        void *slot_ctx = (void *)atomic_load_explicit(&s_sinks[i].ctx, memory_order_relaxed);
        if (slot_sink == sink && slot_ctx == ctx) {
            atomic_store_explicit(&s_sinks[i].sink, (uintptr_t)NULL, memory_order_release);
            on9log_port_unlock();
            return ON9LOG_OK;
        }
    }
    on9log_port_unlock();

    return ON9LOG_ERR_NOT_FOUND;
}

/**
 * @brief Read the pending dropped-packet counter.
 *
 * @return The number of packets dropped since the last emitted dropped-packet
 *         notification, or since system initialisation if none has been emitted.
 */
uint32_t on9log_get_dropped_count(void)
{
    return (uint32_t)atomic_load_explicit(&s_dropped_count, memory_order_relaxed);
}

/**
 * @brief Set the default minimum log level.
 *
 * Affects all tags that do not have an explicit tag-level filter.
 *
 * @param[in] level  New default log level.  Invalid levels are silently ignored.
 */
void on9log_set_level(on9log_level_t level)
{
    if (!on9log_valid_level(level)) {
        return;
    }
    atomic_store_explicit(&s_default_level, (int)level, memory_order_relaxed);
}

/**
 * @brief Get the current default minimum log level.
 *
 * @return The current default log level.
 */
on9log_level_t on9log_get_level(void)
{
    return (on9log_level_t)atomic_load_explicit(&s_default_level, memory_order_relaxed);
}

/**
 * @brief Set the minimum log level for a specific tag.
 *
 * If a filter node already exists for the tag its level is updated; otherwise
 * a new node is allocated and inserted at the head of the filter list.
 *
 * @param[in] tag    Tag string (pointer is stored, not copied).
 * @param[in] level  Minimum log level for this tag.
 *
 * @return
 * - @c ON9LOG_OK on success.
 * - @c ON9LOG_ERR_INVALID_ARG if tag is NULL or level is invalid.
 * - @c ON9LOG_ERR_NO_MEM if a new node could not be allocated.
 */
on9log_err_t on9log_set_tag_level(const char *tag, on9log_level_t level)
{
    if (tag == NULL || !on9log_valid_level(level)) {
        return ON9LOG_ERR_INVALID_ARG;
    }

    on9log_port_lock();
    on9log_tag_filter_node_t *node = NULL;
    SLIST_FOREACH(node, &s_tag_filters, entries) {
        if (on9log_tag_matches(node->tag, tag)) {
            bool was_active = atomic_load_explicit(&node->active, memory_order_acquire);
            atomic_store_explicit(&node->level, (int)level, memory_order_relaxed);
            if (!was_active) {
                atomic_store_explicit(&node->active, true, memory_order_release);
                atomic_fetch_add_explicit(&s_active_tag_filter_count, 1, memory_order_acq_rel);
            }
            on9log_port_unlock();
            return ON9LOG_OK;
        }
    }

    node = (on9log_tag_filter_node_t *)calloc(1, sizeof(*node));
    if (node == NULL) {
        on9log_port_unlock();
        return ON9LOG_ERR_NO_MEM;
    }

    node->tag = tag;
    atomic_init(&node->level, (int)level);
    atomic_init(&node->active, true);
    SLIST_INSERT_HEAD(&s_tag_filters, node, entries);
    atomic_fetch_add_explicit(&s_active_tag_filter_count, 1, memory_order_acq_rel);
    atomic_store_explicit(&s_tag_filter_head, (uintptr_t)SLIST_FIRST(&s_tag_filters), memory_order_release);
    on9log_port_unlock();

    return ON9LOG_OK;
}

/**
 * @brief Remove the minimum log level override for a specific tag.
 *
 * Marks the filter node inactive (without freeing it) so that the tag falls
 * back to the default level.  The node remains in the linked list but is
 * skipped during filter lookups.
 *
 * @param[in] tag  Tag string to clear.
 *
 * @return
 * - @c ON9LOG_OK on success.
 * - @c ON9LOG_ERR_INVALID_ARG if tag is NULL.
 * - @c ON9LOG_ERR_NOT_FOUND if no filter exists for the given tag.
 */
on9log_err_t on9log_clear_tag_level(const char *tag)
{
    if (tag == NULL) {
        return ON9LOG_ERR_INVALID_ARG;
    }

    on9log_port_lock();
    on9log_tag_filter_node_t *node = NULL;
    SLIST_FOREACH(node, &s_tag_filters, entries) {
        if (on9log_tag_matches(node->tag, tag)) {
            bool was_active = atomic_exchange_explicit(&node->active, false, memory_order_acq_rel);
            if (was_active) {
                atomic_fetch_sub_explicit(&s_active_tag_filter_count, 1, memory_order_acq_rel);
            }
            on9log_port_unlock();
            return ON9LOG_OK;
        }
    }
    on9log_port_unlock();

    return ON9LOG_ERR_NOT_FOUND;
}

/**
 * @brief Dispatch a pre-serialised log packet to all registered sinks.
 *
 * Used by the ISR drain path (and any other code that has already serialised
 * a packet).  The packet must start with a valid magic byte and have at least
 * @c sizeof(on9log_packet_header_t) bytes.  Any accumulated dropped-count is
 * emitted before the packet.
 *
 * @param[in] packet      Pointer to the serialised packet (header + payload).
 * @param[in] packet_len  Total packet length in bytes.
 *
 * @return
 * - @c ON9LOG_OK on success.
 * - @c ON9LOG_ERR_INVALID_ARG if packet is NULL, too short, or has an invalid magic.
 */
on9log_err_t on9log_dispatch_packet(const uint8_t *packet, size_t packet_len)
{
    uint32_t dropped_count = 0;
    on9log_stream_t stream = {0};

#if ON9LOG_PLAIN_TEXT
    if (packet == NULL || packet_len == 0) {
        return ON9LOG_ERR_INVALID_ARG;
    }
#else
    if (packet == NULL || packet_len < sizeof(on9log_packet_header_t)) {
        return ON9LOG_ERR_INVALID_ARG;
    }
    if (packet[0] != ON9LOG_PACKET_MAGIC) {
        return ON9LOG_ERR_INVALID_ARG;
    }
#endif

    dropped_count = (uint32_t)atomic_exchange_explicit(&s_dropped_count, 0, memory_order_acq_rel);
    if (dropped_count != 0) {
        on9log_emit_dropped_packet(dropped_count);
    }

#if ON9LOG_PLAIN_TEXT
    on9log_stream_start(&stream, NULL, 0);
    on9log_stream_payload(&stream,
                          packet,
                          packet_len,
                          0,
                          ON9LOG_PAYLOAD_META_ARG_INDEX);
#else
    on9log_stream_start(&stream, packet, sizeof(on9log_packet_header_t));
    on9log_stream_payload(&stream,
                          &packet[sizeof(on9log_packet_header_t)],
                          packet_len - sizeof(on9log_packet_header_t),
                          0,
                          ON9LOG_PAYLOAD_META_ARG_INDEX);
#endif
    on9log_stream_end(&stream);

    return ON9LOG_OK;
}

static bool on9log_arg_types_have_dynamic_string(const char *arg_types)
{
    for (unsigned idx = 0; idx < UINT8_MAX; ++idx) {
        uint8_t arg_type = on9log_arg_type_at(arg_types, idx);
        if (arg_type == ON9_LOG_ARGS_TYPE_NONE) {
            return false;
        }
        if (arg_type == ON9_LOG_ARGS_TYPE_DYNAMIC_STRING ||
            arg_type == ON9_LOG_ARGS_TYPE_DYNAMIC_STRING_VIEW) {
            return true;
        }
    }
    return false;
}

static const char *on9log_skip_printf_integer(const char *p)
{
    while (*p >= '0' && *p <= '9') {
        ++p;
    }
    return p;
}

static const char *on9log_skip_printf_length(const char *p)
{
    switch (*p) {
    case 'h':
        return p + (p[1] == 'h' ? 2 : 1);
    case 'l':
        return p + (p[1] == 'l' ? 2 : 1);
    case 'j':
    case 'z':
    case 't':
    case 'L':
        return p + 1;
    default:
        return p;
    }
}

static on9log_arg_mask_t on9log_mark_precision_string_args(const char *format, size_t arg_count)
{
    if (format == NULL || arg_count == 0 || strstr(format, "%.*s") == NULL) {
        return 0;
    }

    on9log_arg_mask_t mask = 0;
    size_t arg_index = 0;
    for (const char *p = format; *p != '\0'; ++p) {
        if (*p != '%') {
            continue;
        }
        ++p;
        if (*p == '%') {
            continue;
        }

        while (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0') {
            ++p;
        }

        if (*p == '*') {
            ++arg_index;
            ++p;
        } else {
            p = on9log_skip_printf_integer(p);
        }

        bool precision_star = false;
        if (*p == '.') {
            ++p;
            if (*p == '*') {
                precision_star = true;
                ++arg_index;
                ++p;
            } else {
                p = on9log_skip_printf_integer(p);
            }
        }

        p = on9log_skip_printf_length(p);
        if (*p == '\0') {
            break;
        }

        size_t value_arg_index = arg_index++;
        if (*p == 's' && precision_star &&
            value_arg_index < arg_count &&
            value_arg_index < ON9LOG_ARG_MASK_BITS) {
            mask |= ((on9log_arg_mask_t)1u << value_arg_index);
        }
    }

    return mask;
}

#if ON9LOG_PLAIN_TEXT
#if defined(ESP_PLATFORM) || defined(__APPLE__) || defined(__FreeBSD__) || \
    defined(__NetBSD__) || defined(__OpenBSD__)
#define ON9LOG_USE_FUNOPEN 1
#else
#define ON9LOG_USE_FUNOPEN 0
#endif

typedef struct {
    on9log_text_output_cb_t output;
    void *output_ctx;
} on9log_printf_cookie_t;

typedef struct {
    const char *format;
    va_list *args;
} on9log_printf_formatter_ctx_t;

static on9log_printf_cookie_t s_on9log_printf_cookie;
static atomic_uintptr_t s_on9log_printf_stream = ATOMIC_VAR_INIT((uintptr_t)NULL);

#if ON9LOG_USE_FUNOPEN
static int on9log_printf_cookie_write(void *cookie, const char *data, int len)
{
    on9log_printf_cookie_t *stream = (on9log_printf_cookie_t *)cookie;
    stream->output(data, (size_t)len, stream->output_ctx);
    return len;
}
#else
static ssize_t on9log_printf_cookie_write(void *cookie, const char *data, size_t len)
{
    on9log_printf_cookie_t *stream = (on9log_printf_cookie_t *)cookie;
    stream->output(data, len, stream->output_ctx);
    return (ssize_t)len;
}
#endif

on9log_err_t on9log_init(void)
{
    if (atomic_load_explicit(&s_on9log_printf_stream, memory_order_acquire) != (uintptr_t)NULL) {
        return ON9LOG_OK;
    }

    on9log_port_lock();
    if (atomic_load_explicit(&s_on9log_printf_stream, memory_order_relaxed) != (uintptr_t)NULL) {
        on9log_port_unlock();
        return ON9LOG_OK;
    }

#if ON9LOG_USE_FUNOPEN
    FILE *stream = funopen(&s_on9log_printf_cookie,
                           NULL,
                           on9log_printf_cookie_write,
                           NULL,
                           NULL);
#else
    cookie_io_functions_t functions = {
        .read = NULL,
        .write = on9log_printf_cookie_write,
        .seek = NULL,
        .close = NULL,
    };
    FILE *stream = fopencookie(&s_on9log_printf_cookie, "w", functions);
#endif
    if (stream == NULL) {
        on9log_port_unlock();
        return ON9LOG_ERR_NO_MEM;
    }
    if (setvbuf(stream, NULL, _IONBF, 0) != 0) {
        (void)fclose(stream);
        on9log_port_unlock();
        return ON9LOG_ERR_FAIL;
    }

    atomic_store_explicit(&s_on9log_printf_stream, (uintptr_t)stream, memory_order_release);
    on9log_port_unlock();
    return ON9LOG_OK;
}

static void on9log_printf_formatter(on9log_text_output_cb_t output,
                                    void *output_ctx,
                                    void *formatter_ctx)
{
    on9log_printf_formatter_ctx_t *format = (on9log_printf_formatter_ctx_t *)formatter_ctx;
    FILE *stream = (FILE *)atomic_load_explicit(&s_on9log_printf_stream, memory_order_acquire);
    if (stream == NULL) {
        return;
    }

    flockfile(stream);
    s_on9log_printf_cookie.output = output;
    s_on9log_printf_cookie.output_ctx = output_ctx;
    (void)vfprintf(stream, format->format != NULL ? format->format : "", *format->args);
    (void)fflush(stream);
    s_on9log_printf_cookie.output = NULL;
    s_on9log_printf_cookie.output_ctx = NULL;
    funlockfile(stream);
}
#else
on9log_err_t on9log_init(void)
{
    return ON9LOG_OK;
}
#endif

static void on9log_vwrite(on9log_level_t level,
                          const char *tag,
                          const char *format,
                          const char *format_scan,
                          const char *arg_types,
                          uint8_t arg_count,
                          bool has_dynamic_string,
                          va_list *args)
{
#if ON9LOG_PLAIN_TEXT
    (void)format_scan;
    (void)arg_types;
    (void)arg_count;
    (void)has_dynamic_string;

    if (atomic_load_explicit(&s_on9log_printf_stream, memory_order_acquire) == (uintptr_t)NULL) {
        if (on9log_level_enabled(level, tag)) {
            atomic_fetch_add_explicit(&s_dropped_count, 1, memory_order_relaxed);
        }
        return;
    }

    on9log_printf_formatter_ctx_t formatter_ctx = {
        .format = format,
        .args = args,
    };
    on9log_write_text(level, tag, on9log_printf_formatter, &formatter_ctx);
#else
    uint32_t dropped_count = 0;
    on9log_stream_t stream = {0};

    if (!on9log_level_enabled(level, tag)) {
        return;
    }

    dropped_count = (uint32_t)atomic_exchange_explicit(&s_dropped_count, 0, memory_order_acq_rel);
    if (dropped_count != 0) {
        on9log_emit_dropped_packet(dropped_count);
    }

    on9log_arg_mask_t precision_string_arg_mask = 0;
    if (arg_count != 0 && format_scan != NULL && has_dynamic_string) {
        precision_string_arg_mask = on9log_mark_precision_string_args(format_scan, arg_count);
    }

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
    on9log_emit_payload_args(&stream, arg_types, precision_string_arg_mask, args, arg_count);
    on9log_stream_end(&stream);
#endif
}

/**
 * @brief Write a formatted log message (task-context, variadic).
 *
 * This is the primary logging entry point.  It checks the runtime level
 * filter, emits any accumulated dropped-packet notification, creates a
 * @c ON9LOG_PKT_LOG frame with header, argument count, argument type bytes,
 * and the serialised argument values via the stream.
 *
 * @param[in] level     Log level.
 * @param[in] tag       Tag string.
 * @param[in] format    Format string (stored as pointer; not interpreted here).
 * @param[in] arg_types Packed argument-type string.
 * @param[in] ...       Variable arguments matching @c arg_types.
 */
void on9log_write(on9log_level_t level,
                  const char *tag,
                  const char *format,
                  const char *arg_types,
                  ...)
{
    va_list args;
    va_start(args, arg_types);
    on9log_vwrite(level, tag, format, NULL, arg_types, on9log_arg_count(arg_types), false, &args);
    va_end(args);
}

void on9log_write_with_format_scan(on9log_level_t level,
                                   const char *tag,
                                   const char *format,
                                   const char *format_scan,
                                   const char *arg_types,
                                   ...)
{
    va_list args;
    uint8_t arg_count = on9log_arg_count(arg_types);
    va_start(args, arg_types);
    on9log_vwrite(level,
                  tag,
                  format,
                  format_scan,
                  arg_types,
                  arg_count,
                  on9log_arg_types_have_dynamic_string(arg_types),
                  &args);
    va_end(args);
}

void on9log_write_with_format_scan_metadata(on9log_level_t level,
                                            const char *tag,
                                            const char *format,
                                            const char *format_scan,
                                            const char *arg_types,
                                            uint8_t arg_count,
                                            int has_dynamic_string,
                                            ...)
{
    va_list args;
    va_start(args, has_dynamic_string);
    on9log_vwrite(level,
                  tag,
                  format,
                  format_scan,
                  arg_types,
                  arg_count,
                  has_dynamic_string != 0,
                  &args);
    va_end(args);
}

/**
 * @brief Write a formatted log message from an ISR context.
 *
 * The message is serialised into a stack-allocated packet buffer, then
 * enqueued into the platform ringbuffer for deferred dispatch.  Only
 * ISR-safe argument types (32/64-bit integers and pointers) are accepted;
 * dynamic strings cause the message to be dropped and the dropped counter
 * incremented.
 *
 * @param[in] level     Log level.
 * @param[in] tag       Tag string.
 * @param[in] format    Format string (stored as pointer).
 * @param[in] arg_types Packed argument-type string (must be ISR-safe).
 * @param[in] ...       Variable arguments matching @c arg_types.
 *
 * @return @c true if the packet was successfully enqueued, @c false if it
 *         was dropped (ringbuffer full, non-ISR-safe args, or encoding failure).
 */
#if ON9LOG_PLAIN_TEXT
bool on9log_write_text_isr(on9log_level_t level,
                           const char *tag,
                           const char *text,
                           size_t text_len)
{
    uint8_t packet[ON9LOG_ISR_PACKET_MAX];
    on9log_encoder_t enc = {
        .cap = sizeof(packet),
        .data = packet,
    };

    if (!on9log_level_enabled(level, tag)) {
        return true;
    }
    if (!on9log_port_isr_ready()) {
        return true;
    }
    if (text == NULL && text_len != 0) {
        atomic_fetch_add_explicit(&s_dropped_count, 1, memory_order_relaxed);
        return false;
    }

    on9log_text_output_prefix(on9log_text_encoder_output,
                              &enc,
                              level,
                              on9log_port_isr_timestamp_ms(),
                              tag);
    on9log_put_bytes(&enc, text, text_len);
    on9log_text_output_suffix(on9log_text_encoder_output, &enc);

    if (enc.overflow || !on9log_port_isr_enqueue_packet(packet, enc.len)) {
        atomic_fetch_add_explicit(&s_dropped_count, 1, memory_order_relaxed);
        return false;
    }

    return true;
}
#endif

bool on9log_write_isr(on9log_level_t level,
                      const char *tag,
                      const char *format,
                      const char *arg_types,
                      ...)
{
#if ON9LOG_PLAIN_TEXT
    char text[ON9LOG_ISR_PACKET_MAX];

    if (!on9log_level_enabled(level, tag)) {
        return true;
    }
    if (!on9log_port_isr_ready()) {
        return true;
    }
    if (!on9log_arg_types_are_isr_safe(arg_types)) {
        atomic_fetch_add_explicit(&s_dropped_count, 1, memory_order_relaxed);
        return false;
    }

    va_list args;
    va_start(args, arg_types);
    int text_len = vsnprintf(text, sizeof(text), format != NULL ? format : "", args);
    va_end(args);

    if (text_len < 0 || (size_t)text_len >= sizeof(text)) {
        atomic_fetch_add_explicit(&s_dropped_count, 1, memory_order_relaxed);
        return false;
    }

    return on9log_write_text_isr(level, tag, text, (size_t)text_len);
#else
    uint8_t packet[ON9LOG_ISR_PACKET_MAX];
    size_t packet_len = 0;
    bool ok = false;

    if (!on9log_level_enabled(level, tag)) {
        return true;
    }
    if (!on9log_port_isr_ready()) {
        return true;
    }
    if (!on9log_arg_types_are_isr_safe(arg_types)) {
        atomic_fetch_add_explicit(&s_dropped_count, 1, memory_order_relaxed);
        return false;
    }

    va_list args;
    va_start(args, arg_types);
    ok = on9log_encode_isr_log_packet(packet, sizeof(packet), &packet_len, level, tag, format, arg_types, &args);
    va_end(args);

    if (!ok || !on9log_port_isr_enqueue_packet(packet, packet_len)) {
        atomic_fetch_add_explicit(&s_dropped_count, 1, memory_order_relaxed);
        return false;
    }

    return true;
#endif
}

/**
 * @brief Write a raw binary buffer through the logging system.
 *
 * The buffer is split into chunks of up to @c ON9LOG_BUFFER_CHUNK_SIZE bytes
 * and each chunk is emitted as a @c ON9LOG_PKT_BUFFER frame.  Buffers larger
 * than @c UINT32_MAX are rejected on 64-bit platforms.
 *
 * @param[in] level      Log level.
 * @param[in] tag        Tag string.
 * @param[in] buffer     Pointer to the raw data buffer (may be NULL if @c buffer_len is 0).
 * @param[in] buffer_len Length of the buffer in bytes.
 */
#if ON9LOG_PLAIN_TEXT
typedef struct {
    const uint8_t *bytes;
    uint32_t len;
} on9log_buffer_text_ctx_t;

static void on9log_buffer_text_formatter(on9log_text_output_cb_t output,
                                         void *output_ctx,
                                         void *formatter_ctx)
{
    static const char hex[] = "0123456789abcdef";
    const on9log_buffer_text_ctx_t *ctx = (const on9log_buffer_text_ctx_t *)formatter_ctx;
    char encoded[3u * 32u];
    size_t encoded_len = 0;

    output("buffer[", sizeof("buffer[") - 1u, output_ctx);
    on9log_text_output_u32(output, output_ctx, ctx->len);
    output("]:", sizeof("]:") - 1u, output_ctx);

    for (uint32_t i = 0; i < ctx->len; ++i) {
        uint8_t byte = ctx->bytes[i];
        encoded[encoded_len++] = ' ';
        encoded[encoded_len++] = hex[byte >> 4u];
        encoded[encoded_len++] = hex[byte & 0x0fu];
        if (encoded_len == sizeof(encoded)) {
            output(encoded, encoded_len, output_ctx);
            encoded_len = 0;
        }
    }
    if (encoded_len != 0) {
        output(encoded, encoded_len, output_ctx);
    }
}
#endif

void on9log_write_buffer(on9log_level_t level,
                         const char *tag,
                         const void *buffer,
                         size_t buffer_len)
{
#if !ON9LOG_PLAIN_TEXT
    uint32_t dropped_count = 0;
#endif
    const uint8_t *bytes = (const uint8_t *)buffer;

    if (!on9log_level_enabled(level, tag)) {
        return;
    }
    if ((bytes == NULL && buffer_len != 0)
#if SIZE_MAX > UINT32_MAX
        || buffer_len > UINT32_MAX
#endif
    ) {
        atomic_fetch_add_explicit(&s_dropped_count, 1, memory_order_relaxed);
        return;
    }

#if ON9LOG_PLAIN_TEXT
    on9log_buffer_text_ctx_t formatter_ctx = {
        .bytes = bytes,
        .len = (uint32_t)buffer_len,
    };
    on9log_write_text(level, tag, on9log_buffer_text_formatter, &formatter_ctx);
    return;
#else

    dropped_count = (uint32_t)atomic_exchange_explicit(&s_dropped_count, 0, memory_order_acq_rel);
    if (dropped_count != 0) {
        on9log_emit_dropped_packet(dropped_count);
    }

    uint32_t total_len = (uint32_t)buffer_len;

    if (total_len == 0) {
        on9log_emit_buffer_packet(level, tag, bytes, total_len, 0, 0);
        return;
    }

    for (uint32_t offset = 0; offset < total_len;) {
        uint32_t chunk_len = total_len - offset;
        if (chunk_len > ON9LOG_BUFFER_CHUNK_SIZE) {
            chunk_len = ON9LOG_BUFFER_CHUNK_SIZE;
        }
        on9log_emit_buffer_packet(level, tag, bytes, total_len, offset, chunk_len);
        offset += chunk_len;
    }
#endif
}
