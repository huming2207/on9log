#include "on9log.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_log_level.h"
#include "esp_log_timestamp.h"
#include "esp_private/log_lock.h"
#include "esp_rom_serial_output.h"
#include "on9log_fmt.h"
#include "sdkconfig.h"

#define ON9LOG_MAX_PACKET_LEN 512u
#define ON9LOG_MAX_SINKS 4u
#define ON9LOG_NULL_STRING_LEN UINT16_MAX
#define ON9LOG_MAX_MEASURED_STRING_LEN ON9LOG_MAX_PACKET_LEN

typedef struct {
    atomic_uintptr_t sink;
    atomic_uintptr_t ctx;
} on9log_sink_entry_t;

typedef struct {
    bool overflow;
    size_t len;
    uint8_t data[ON9LOG_MAX_PACKET_LEN];
} on9log_encoder_t;

static on9log_sink_entry_t s_sinks[ON9LOG_MAX_SINKS];
static atomic_bool s_uart_enabled = ATOMIC_VAR_INIT(true);
static atomic_uint_fast16_t s_seq = ATOMIC_VAR_INIT(0);
static atomic_uint_fast32_t s_dropped_count = ATOMIC_VAR_INIT(0);

static void on9log_put_u8(on9log_encoder_t *enc, uint8_t val)
{
    if (enc->len >= ON9LOG_MAX_PACKET_LEN) {
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

static void on9log_put_bytes(on9log_encoder_t *enc, const void *data, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;

    for (size_t i = 0; i < len; ++i) {
        on9log_put_u8(enc, bytes[i]);
    }
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

static size_t on9log_process_arg(on9log_encoder_t *enc,
                                 uint8_t arg_type,
                                 va_list args)
{
    switch (arg_type) {
    case ON9_LOG_ARGS_TYPE_32BITS: {
        uint32_t val = va_arg(args, uint32_t);
        if (enc != NULL) {
            on9log_put_u32(enc, val);
        }
        return sizeof(uint32_t);
    }
    case ON9_LOG_ARGS_TYPE_64BITS: {
        uint64_t val = va_arg(args, uint64_t);
        if (enc != NULL) {
            on9log_put_u64(enc, val);
        }
        return sizeof(uint64_t);
    }
    case ON9_LOG_ARGS_TYPE_POINTER: {
        const void *ptr = va_arg(args, const void *);
        if (enc != NULL) {
            on9log_put_u32(enc, (uint32_t)(uintptr_t)ptr);
        }
        return sizeof(uint32_t);
    }
    case ON9_LOG_ARGS_TYPE_DYNAMIC_STRING: {
        const char *str = va_arg(args, const char *);
        if (str == NULL) {
            if (enc != NULL) {
                on9log_put_u16(enc, ON9LOG_NULL_STRING_LEN);
            }
            return sizeof(uint16_t);
        }

        size_t str_len = on9log_bounded_strlen(str, ON9LOG_MAX_MEASURED_STRING_LEN);
        if (enc != NULL) {
            on9log_put_u16(enc, (uint16_t)str_len);
            on9log_put_bytes(enc, str, str_len);
        }
        return sizeof(uint16_t) + str_len;
    }
    case ON9_LOG_ARGS_TYPE_NONE:
    default:
        if (enc != NULL) {
            enc->overflow = true;
        }
        return 0;
    }
}

static size_t on9log_process_payload_args(on9log_encoder_t *enc, const char *arg_types, va_list args)
{
    size_t len = 0;

    for (unsigned idx = 0; on9log_arg_type_at(arg_types, idx) != ON9_LOG_ARGS_TYPE_NONE; ++idx) {
        len += on9log_process_arg(enc, on9log_arg_type_at(arg_types, idx), args);
    }

    return len;
}

static size_t on9log_measure_payload_len(const char *arg_types, va_list args)
{
    uint8_t arg_count = on9log_arg_count(arg_types);

    return sizeof(uint8_t) + arg_count + on9log_process_payload_args(NULL, arg_types, args);
}

static void on9log_put_payload(on9log_encoder_t *enc, const char *arg_types, va_list args)
{
    uint8_t arg_count = on9log_arg_count(arg_types);

    on9log_put_u8(enc, arg_count);
    on9log_put_bytes(enc, arg_types, arg_count);
    (void)on9log_process_payload_args(enc, arg_types, args);
}

static void on9log_put_header(on9log_encoder_t *enc,
                              on9log_packet_type_t type,
                              esp_log_level_t level,
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

static bool on9log_build_dropped_packet(on9log_encoder_t *enc, uint32_t dropped_count)
{
    on9log_put_header(enc,
                      ON9LOG_PKT_DROPPED,
                      ESP_LOG_NONE,
                      (uint16_t)atomic_fetch_add_explicit(&s_seq, 1, memory_order_relaxed),
                      esp_log_timestamp(),
                      0,
                      0,
                      sizeof(uint32_t));
    on9log_put_u32(enc, dropped_count);
    return !enc->overflow;
}

static bool on9log_build_log_packet(on9log_encoder_t *enc,
                                    esp_log_level_t level,
                                    const char *tag,
                                    const char *format,
                                    const char *arg_types,
                                    va_list args)
{
    va_list measure_args;
    va_copy(measure_args, args);
    size_t payload_len = on9log_measure_payload_len(arg_types, measure_args);
    va_end(measure_args);

    if (payload_len > UINT16_MAX ||
            payload_len + sizeof(on9log_packet_header_t) > ON9LOG_MAX_PACKET_LEN) {
        return false;
    }

    on9log_put_header(enc,
                      ON9LOG_PKT_LOG,
                      level,
                      (uint16_t)atomic_fetch_add_explicit(&s_seq, 1, memory_order_relaxed),
                      esp_log_timestamp(),
                      (uint32_t)(uintptr_t)tag,
                      (uint32_t)(uintptr_t)format,
                      (uint16_t)payload_len);
    on9log_put_payload(enc, arg_types, args);
    return !enc->overflow;
}

static bool on9log_level_enabled(esp_log_level_t level, const char *tag)
{
    if (level == ESP_LOG_NONE || level > LOG_LOCAL_LEVEL) {
        return false;
    }
#if CONFIG_LOG_MASTER_LEVEL
    if (esp_log_get_level_master() < level) {
        return false;
    }
#endif
    return esp_log_level_get(tag) >= level;
}

static void on9log_dispatch_packet(const uint8_t *packet, size_t packet_len)
{
    bool uart_enabled = atomic_load_explicit(&s_uart_enabled, memory_order_relaxed);

    if (uart_enabled) {
        for (size_t i = 0; i < packet_len; ++i) {
            esp_rom_output_tx_one_char(packet[i]);
        }
    }

    for (size_t i = 0; i < ON9LOG_MAX_SINKS; ++i) {
        on9log_sink_t sink = (on9log_sink_t)atomic_load_explicit(&s_sinks[i].sink, memory_order_acquire);
        if (sink != NULL) {
            void *ctx = (void *)atomic_load_explicit(&s_sinks[i].ctx, memory_order_relaxed);
            sink(packet, packet_len, ctx);
        }
    }
}

esp_err_t on9log_add_sink(on9log_sink_t sink, void *ctx)
{
    if (sink == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_log_impl_lock();
    for (size_t i = 0; i < ON9LOG_MAX_SINKS; ++i) {
        on9log_sink_t slot_sink = (on9log_sink_t)atomic_load_explicit(&s_sinks[i].sink, memory_order_acquire);
        void *slot_ctx = (void *)atomic_load_explicit(&s_sinks[i].ctx, memory_order_relaxed);
        if (slot_sink == sink && slot_ctx == ctx) {
            esp_log_impl_unlock();
            return ESP_OK;
        }
    }
    for (size_t i = 0; i < ON9LOG_MAX_SINKS; ++i) {
        if (atomic_load_explicit(&s_sinks[i].sink, memory_order_acquire) == (uintptr_t)NULL) {
            atomic_store_explicit(&s_sinks[i].ctx, (uintptr_t)ctx, memory_order_relaxed);
            atomic_store_explicit(&s_sinks[i].sink, (uintptr_t)sink, memory_order_release);
            esp_log_impl_unlock();
            return ESP_OK;
        }
    }
    esp_log_impl_unlock();

    return ESP_ERR_NO_MEM;
}

esp_err_t on9log_remove_sink(on9log_sink_t sink, void *ctx)
{
    esp_log_impl_lock();
    for (size_t i = 0; i < ON9LOG_MAX_SINKS; ++i) {
        on9log_sink_t slot_sink = (on9log_sink_t)atomic_load_explicit(&s_sinks[i].sink, memory_order_acquire);
        void *slot_ctx = (void *)atomic_load_explicit(&s_sinks[i].ctx, memory_order_relaxed);
        if (slot_sink == sink && slot_ctx == ctx) {
            atomic_store_explicit(&s_sinks[i].sink, (uintptr_t)NULL, memory_order_release);
            atomic_store_explicit(&s_sinks[i].ctx, (uintptr_t)NULL, memory_order_relaxed);
            esp_log_impl_unlock();
            return ESP_OK;
        }
    }
    esp_log_impl_unlock();

    return ESP_ERR_NOT_FOUND;
}

void on9log_set_uart_enabled(bool enabled)
{
    atomic_store_explicit(&s_uart_enabled, enabled, memory_order_relaxed);
}

uint32_t on9log_get_dropped_count(void)
{
    return (uint32_t)atomic_load_explicit(&s_dropped_count, memory_order_relaxed);
}

void on9log_write(esp_log_level_t level,
                  const char *tag,
                  const char *format,
                  const char *arg_types,
                  ...)
{
    uint32_t dropped_count = 0;
    on9log_encoder_t dropped_packet = {0};
    on9log_encoder_t log_packet = {0};

    if (!on9log_level_enabled(level, tag)) {
        return;
    }

    va_list args;
    va_start(args, arg_types);

    dropped_count = (uint32_t)atomic_exchange_explicit(&s_dropped_count, 0, memory_order_acq_rel);
    if (dropped_count != 0) {
        if (!on9log_build_dropped_packet(&dropped_packet, dropped_count)) {
            atomic_fetch_add_explicit(&s_dropped_count, dropped_count + 1u, memory_order_relaxed);
            dropped_count = 0;
        }
    }
    if (!on9log_build_log_packet(&log_packet, level, tag, format, arg_types, args)) {
        atomic_fetch_add_explicit(&s_dropped_count, 1, memory_order_relaxed);
        log_packet.len = 0;
    }

    va_end(args);

    if (dropped_count != 0) {
        on9log_dispatch_packet(dropped_packet.data, dropped_packet.len);
    }
    if (log_packet.len != 0) {
        on9log_dispatch_packet(log_packet.data, log_packet.len);
    }
}
