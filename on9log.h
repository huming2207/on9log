#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "on9log_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ON9LOG_OK = 0,
    ON9LOG_ERR_FAIL = -1,
    ON9LOG_ERR_INVALID_ARG = -2,
    ON9LOG_ERR_NO_MEM = -3,
    ON9LOG_ERR_NOT_FOUND = -4,
    ON9LOG_ERR_INVALID_SIZE = -5,
} on9log_err_t;

typedef enum {
    ON9_LOG_LEVEL_NONE = 0,
    ON9_LOG_LEVEL_ERROR = 1,
    ON9_LOG_LEVEL_WARN = 2,
    ON9_LOG_LEVEL_INFO = 3,
    ON9_LOG_LEVEL_DEBUG = 4,
    ON9_LOG_LEVEL_VERBOSE = 5,
} on9log_level_t;

typedef void (*on9log_sink_start_cb_t)(const uint8_t *header, size_t header_len, void *ctx);
typedef void (*on9log_sink_payload_cb_t)(const uint8_t *payload,
                                         size_t payload_len,
                                         size_t total_arg_cnt,
                                         size_t curr_arg_index,
                                         void *ctx);
typedef void (*on9log_sink_end_cb_t)(void *ctx);

typedef struct {
    on9log_sink_start_cb_t start_cb;
    on9log_sink_payload_cb_t payload_cb;
    on9log_sink_end_cb_t end_cb;
} on9log_sink_t;

on9log_err_t on9log_add_sink(const on9log_sink_t *sink, void *ctx);
on9log_err_t on9log_remove_sink(const on9log_sink_t *sink, void *ctx);
void on9log_set_uart_enabled(bool enabled);
uint32_t on9log_get_dropped_count(void);

/*
 * Runtime level filtering. The compile-time ON9_LOG_LOCAL_LEVEL remains a hard
 * ceiling; runtime levels can only suppress logs that survived that ceiling.
 * Tag filters are matched by pointer first, then by string contents.
 */
void on9log_set_level(on9log_level_t level);
on9log_level_t on9log_get_level(void);
on9log_err_t on9log_set_tag_level(const char *tag, on9log_level_t level);
on9log_err_t on9log_clear_tag_level(const char *tag);

/* on9log_write() and ON9_LOGx() must not be called from ISR context. */
void on9log_write(on9log_level_t level,
                  const char *tag,
                  const char *format,
                  const char *arg_types,
                  ...) __attribute__((format(printf, 3, 5)));

/*
 * Encodes a bounded packet and enqueues it through the platform ISR backend.
 * Dynamic string arguments are not supported on the ISR path.
 */
bool on9log_write_isr(on9log_level_t level,
                      const char *tag,
                      const char *format,
                      const char *arg_types,
                      ...) __attribute__((format(printf, 3, 5)));

/* on9log_write_buffer() and ON9_LOG_BUF*() must not be called from ISR context. */
void on9log_write_buffer(on9log_level_t level,
                         const char *tag,
                         const void *buffer,
                         size_t buffer_len);

/* Dispatches one complete raw on9log packet through the normal sink path. */
on9log_err_t on9log_dispatch_packet(const uint8_t *packet, size_t packet_len);

#ifdef __cplusplus
}
#endif

#define ON9_LOG_STRINGIFY2(x) #x
#define ON9_LOG_STRINGIFY(x) ON9_LOG_STRINGIFY2(x)

#define ON9_LOG_VA_NARG(...) ON9_LOG_VA_NARG_IMPL(_, ##__VA_ARGS__, \
    16, 15, 14, 13, 12, 11, 10, 9, \
    8, 7, 6, 5, 4, 3, 2, 1, 0)
#define ON9_LOG_VA_NARG_IMPL(_0, _1, _2, _3, _4, _5, _6, _7, _8, _9, \
                             _10, _11, _12, _13, _14, _15, _16, N, ...) N

#ifndef ON9_LOG_NOLOAD_ATTR
#if defined(__APPLE__)
#define ON9_LOG_NOLOAD_ATTR
#else
#define ON9_LOG_NOLOAD_ATTR \
    __attribute__((section(".noload_keep_in_elf." ON9_LOG_STRINGIFY(__COUNTER__))))
#endif
#endif

#define ON9_LOG_NOLOAD_STR(str) \
    (__extension__({ \
        static const ON9_LOG_NOLOAD_ATTR char __on9log_str[] = (str); \
        (const char *)__on9log_str; \
    }))

#define ON9_LOG_ATTR_STR(str) (__builtin_constant_p(str) ? ON9_LOG_NOLOAD_STR(str) : (str))

#if defined(__GNUC__)
#define ON9_LOG_DIAG_PUSH _Pragma("GCC diagnostic push")
#define ON9_LOG_DIAG_POP _Pragma("GCC diagnostic pop")

// Avoid some weird issues on false-positive string formatting warning/errors from Clangd
#define ON9_LOG_DIAG_IGNORE_FORMAT_OVERFLOW \
    _Pragma("GCC diagnostic ignored \"-Wformat-overflow\"") \
    _Pragma("GCC diagnostic ignored \"-Wformat-nonliteral\"") \
    _Pragma("GCC diagnostic ignored \"-Wformat-security\"")
#else
#define ON9_LOG_DIAG_PUSH
#define ON9_LOG_DIAG_POP
#define ON9_LOG_DIAG_IGNORE_FORMAT_OVERFLOW
#endif

typedef enum {
    ON9_LOG_ARGS_TYPE_NONE = 0,
    ON9_LOG_ARGS_TYPE_32BITS = 1,
    ON9_LOG_ARGS_TYPE_64BITS = 2,
    ON9_LOG_ARGS_TYPE_POINTER = 3,
    ON9_LOG_ARGS_TYPE_DYNAMIC_STRING = 4,
} on9log_args_type_t;

typedef struct {
    unsigned tmp;
} on9log_args_end_t;

#define ON9_LOG_INIT_ARG_TYPE_N(n) ON9_LOG_INIT_ARG_TYPE_##n
#define ON9_LOG_INIT_ARG_TYPE(n, ...) ON9_LOG_INIT_ARG_TYPE_N(n)(__VA_ARGS__)
#define ON9_LOG_INIT_ARG_TYPE_1(a) (char)ON9_LOG_DETECT_TYPE(a)
#define ON9_LOG_INIT_ARG_TYPE_2(a, b) ON9_LOG_INIT_ARG_TYPE_1(a), ON9_LOG_INIT_ARG_TYPE_1(b)
#define ON9_LOG_INIT_ARG_TYPE_3(a, b, c) ON9_LOG_INIT_ARG_TYPE_2(a, b), ON9_LOG_INIT_ARG_TYPE_1(c)
#define ON9_LOG_INIT_ARG_TYPE_4(a, b, c, d) ON9_LOG_INIT_ARG_TYPE_3(a, b, c), ON9_LOG_INIT_ARG_TYPE_1(d)
#define ON9_LOG_INIT_ARG_TYPE_5(a, b, c, d, e) ON9_LOG_INIT_ARG_TYPE_4(a, b, c, d), ON9_LOG_INIT_ARG_TYPE_1(e)
#define ON9_LOG_INIT_ARG_TYPE_6(a, b, c, d, e, f) ON9_LOG_INIT_ARG_TYPE_5(a, b, c, d, e), ON9_LOG_INIT_ARG_TYPE_1(f)
#define ON9_LOG_INIT_ARG_TYPE_7(a, b, c, d, e, f, g) ON9_LOG_INIT_ARG_TYPE_6(a, b, c, d, e, f), ON9_LOG_INIT_ARG_TYPE_1(g)
#define ON9_LOG_INIT_ARG_TYPE_8(a, b, c, d, e, f, g, h) ON9_LOG_INIT_ARG_TYPE_7(a, b, c, d, e, f, g), ON9_LOG_INIT_ARG_TYPE_1(h)
#define ON9_LOG_INIT_ARG_TYPE_9(a, b, c, d, e, f, g, h, i) ON9_LOG_INIT_ARG_TYPE_8(a, b, c, d, e, f, g, h), ON9_LOG_INIT_ARG_TYPE_1(i)
#define ON9_LOG_INIT_ARG_TYPE_10(a, b, c, d, e, f, g, h, i, j) ON9_LOG_INIT_ARG_TYPE_9(a, b, c, d, e, f, g, h, i), ON9_LOG_INIT_ARG_TYPE_1(j)
#define ON9_LOG_INIT_ARG_TYPE_11(a, b, c, d, e, f, g, h, i, j, k) ON9_LOG_INIT_ARG_TYPE_10(a, b, c, d, e, f, g, h, i, j), ON9_LOG_INIT_ARG_TYPE_1(k)
#define ON9_LOG_INIT_ARG_TYPE_12(a, b, c, d, e, f, g, h, i, j, k, l) ON9_LOG_INIT_ARG_TYPE_11(a, b, c, d, e, f, g, h, i, j, k), ON9_LOG_INIT_ARG_TYPE_1(l)
#define ON9_LOG_INIT_ARG_TYPE_13(a, b, c, d, e, f, g, h, i, j, k, l, m) ON9_LOG_INIT_ARG_TYPE_12(a, b, c, d, e, f, g, h, i, j, k, l), ON9_LOG_INIT_ARG_TYPE_1(m)
#define ON9_LOG_INIT_ARG_TYPE_14(a, b, c, d, e, f, g, h, i, j, k, l, m, n) ON9_LOG_INIT_ARG_TYPE_13(a, b, c, d, e, f, g, h, i, j, k, l, m), ON9_LOG_INIT_ARG_TYPE_1(n)
#define ON9_LOG_INIT_ARG_TYPE_15(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o) ON9_LOG_INIT_ARG_TYPE_14(a, b, c, d, e, f, g, h, i, j, k, l, m, n), ON9_LOG_INIT_ARG_TYPE_1(o)
#define ON9_LOG_INIT_ARG_TYPE_16(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p) ON9_LOG_INIT_ARG_TYPE_15(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o), ON9_LOG_INIT_ARG_TYPE_1(p)

#ifndef __cplusplus
#define ON9_LOG_DETECT_TYPE(arg) ( \
    _Generic((arg), \
        on9log_args_end_t: ON9_LOG_ARGS_TYPE_NONE, \
        char*: ON9_LOG_ARGS_TYPE_DYNAMIC_STRING, \
        const char*: ON9_LOG_ARGS_TYPE_DYNAMIC_STRING, \
        uint8_t*: ON9_LOG_ARGS_TYPE_POINTER, \
        const uint8_t*: ON9_LOG_ARGS_TYPE_POINTER, \
        long long int: ON9_LOG_ARGS_TYPE_64BITS, \
        long long unsigned int: ON9_LOG_ARGS_TYPE_64BITS, \
        double: ON9_LOG_ARGS_TYPE_64BITS, \
        float: ON9_LOG_ARGS_TYPE_64BITS, \
        default: ON9_LOG_ARGS_TYPE_32BITS))
#else
extern "C++" {
template <typename T>
struct On9LogArgType {
    constexpr static unsigned long long log_type = ON9_LOG_ARGS_TYPE_32BITS;
    constexpr static unsigned long long constant_log_type = log_type;
};
template <>
struct On9LogArgType<on9log_args_end_t> {
    constexpr static unsigned long long log_type = ON9_LOG_ARGS_TYPE_NONE;
    constexpr static unsigned long long constant_log_type = log_type;
};
template <>
struct On9LogArgType<char *> {
    constexpr static unsigned long long log_type = ON9_LOG_ARGS_TYPE_DYNAMIC_STRING;
    constexpr static unsigned long long constant_log_type = log_type;
};
template <>
struct On9LogArgType<const char *> {
    constexpr static unsigned long long log_type = ON9_LOG_ARGS_TYPE_DYNAMIC_STRING;
    constexpr static unsigned long long constant_log_type = log_type;
};
template <>
struct On9LogArgType<uint8_t *> {
    constexpr static unsigned long long log_type = ON9_LOG_ARGS_TYPE_POINTER;
    constexpr static unsigned long long constant_log_type = log_type;
};
template <>
struct On9LogArgType<const uint8_t *> {
    constexpr static unsigned long long log_type = ON9_LOG_ARGS_TYPE_POINTER;
    constexpr static unsigned long long constant_log_type = log_type;
};
template <size_t N>
struct On9LogArgType<char[N]> {
    constexpr static unsigned long long log_type = ON9_LOG_ARGS_TYPE_DYNAMIC_STRING;
    constexpr static unsigned long long constant_log_type = log_type;
};
template <size_t N>
struct On9LogArgType<const char[N]> {
    constexpr static unsigned long long log_type = ON9_LOG_ARGS_TYPE_DYNAMIC_STRING;
    constexpr static unsigned long long constant_log_type = log_type;
};
template <size_t N>
struct On9LogArgType<uint8_t[N]> {
    constexpr static unsigned long long log_type = ON9_LOG_ARGS_TYPE_POINTER;
    constexpr static unsigned long long constant_log_type = log_type;
};
template <size_t N>
struct On9LogArgType<const uint8_t[N]> {
    constexpr static unsigned long long log_type = ON9_LOG_ARGS_TYPE_POINTER;
    constexpr static unsigned long long constant_log_type = log_type;
};
template <>
struct On9LogArgType<long long int> {
    constexpr static unsigned long long log_type = ON9_LOG_ARGS_TYPE_64BITS;
    constexpr static unsigned long long constant_log_type = log_type;
};
template <>
struct On9LogArgType<long long unsigned int> {
    constexpr static unsigned long long log_type = ON9_LOG_ARGS_TYPE_64BITS;
    constexpr static unsigned long long constant_log_type = log_type;
};
template <>
struct On9LogArgType<double> {
    constexpr static unsigned long long log_type = ON9_LOG_ARGS_TYPE_64BITS;
    constexpr static unsigned long long constant_log_type = log_type;
};
template <>
struct On9LogArgType<float> {
    constexpr static unsigned long long log_type = ON9_LOG_ARGS_TYPE_64BITS;
    constexpr static unsigned long long constant_log_type = log_type;
};
template <typename T>
constexpr unsigned long long ON9_LOG_DETECT_TYPE_IMPL(const T &, bool is_constant)
{
    return is_constant ? On9LogArgType<T>::constant_log_type : On9LogArgType<T>::log_type;
}
}
#define ON9_LOG_DETECT_TYPE(arg) ON9_LOG_DETECT_TYPE_IMPL((arg), __builtin_constant_p(arg))
#endif

#define ON9_LOG_ARGS_TYPE(...) (__extension__({ \
    static const char __on9log_arg_types[] = { __VA_OPT__(ON9_LOG_INIT_ARG_TYPE(ON9_LOG_VA_NARG(__VA_ARGS__), __VA_ARGS__), ) 0 }; \
    (const char *)&__on9log_arg_types; \
}))

#define ON9_LOG_ENABLED(level) ((level) != ON9_LOG_LEVEL_NONE && (level) <= ON9_LOG_LOCAL_LEVEL)

#define ON9_LOG_LEVEL(level, tag, format, ...) do { \
        if (ON9_LOG_ENABLED(level)) { \
            ON9_LOG_DIAG_PUSH; \
            ON9_LOG_DIAG_IGNORE_FORMAT_OVERFLOW; \
            on9log_write((level), (tag), ON9_LOG_ATTR_STR(format), ON9_LOG_ARGS_TYPE(__VA_ARGS__), ##__VA_ARGS__); \
            ON9_LOG_DIAG_POP; \
        } \
    } while (0)

#define ON9_LOGE(tag, format, ...) ON9_LOG_LEVEL(ON9_LOG_LEVEL_ERROR, tag, format, ##__VA_ARGS__)
#define ON9_LOGW(tag, format, ...) ON9_LOG_LEVEL(ON9_LOG_LEVEL_WARN, tag, format, ##__VA_ARGS__)
#define ON9_LOGI(tag, format, ...) ON9_LOG_LEVEL(ON9_LOG_LEVEL_INFO, tag, format, ##__VA_ARGS__)
#define ON9_LOGD(tag, format, ...) ON9_LOG_LEVEL(ON9_LOG_LEVEL_DEBUG, tag, format, ##__VA_ARGS__)
#define ON9_LOGV(tag, format, ...) ON9_LOG_LEVEL(ON9_LOG_LEVEL_VERBOSE, tag, format, ##__VA_ARGS__)

#define ON9_ISR_LOG_LEVEL(level, tag, format, ...) do { \
        if (ON9_LOG_ENABLED(level)) { \
            ON9_LOG_DIAG_PUSH; \
            ON9_LOG_DIAG_IGNORE_FORMAT_OVERFLOW; \
            const char __on9log_isr_arg_types[] = { __VA_OPT__(ON9_LOG_INIT_ARG_TYPE(ON9_LOG_VA_NARG(__VA_ARGS__), __VA_ARGS__), ) 0 }; \
            (void)on9log_write_isr((level), (tag), ON9_LOG_ATTR_STR(format), (const char *)__on9log_isr_arg_types, ##__VA_ARGS__); \
            ON9_LOG_DIAG_POP; \
        } \
    } while (0)

#define ON9_ISR_LOGE(tag, format, ...) ON9_ISR_LOG_LEVEL(ON9_LOG_LEVEL_ERROR, tag, format, ##__VA_ARGS__)
#define ON9_ISR_LOGW(tag, format, ...) ON9_ISR_LOG_LEVEL(ON9_LOG_LEVEL_WARN, tag, format, ##__VA_ARGS__)
#define ON9_ISR_LOGI(tag, format, ...) ON9_ISR_LOG_LEVEL(ON9_LOG_LEVEL_INFO, tag, format, ##__VA_ARGS__)
#define ON9_ISR_LOGD(tag, format, ...) ON9_ISR_LOG_LEVEL(ON9_LOG_LEVEL_DEBUG, tag, format, ##__VA_ARGS__)
#define ON9_ISR_LOGV(tag, format, ...) ON9_ISR_LOG_LEVEL(ON9_LOG_LEVEL_VERBOSE, tag, format, ##__VA_ARGS__)

#define ON9_LOG_BUF_LEVEL(level, tag, buffer, buffer_len) do { \
        if (ON9_LOG_ENABLED(level)) { \
            on9log_write_buffer((level), (tag), (buffer), (buffer_len)); \
        } \
    } while (0)

#define ON9_LOG_BUFE(tag, buffer, buffer_len) ON9_LOG_BUF_LEVEL(ON9_LOG_LEVEL_ERROR, tag, buffer, buffer_len)
#define ON9_LOG_BUFW(tag, buffer, buffer_len) ON9_LOG_BUF_LEVEL(ON9_LOG_LEVEL_WARN, tag, buffer, buffer_len)
#define ON9_LOG_BUFI(tag, buffer, buffer_len) ON9_LOG_BUF_LEVEL(ON9_LOG_LEVEL_INFO, tag, buffer, buffer_len)
#define ON9_LOG_BUFD(tag, buffer, buffer_len) ON9_LOG_BUF_LEVEL(ON9_LOG_LEVEL_DEBUG, tag, buffer, buffer_len)
#define ON9_LOG_BUFV(tag, buffer, buffer_len) ON9_LOG_BUF_LEVEL(ON9_LOG_LEVEL_VERBOSE, tag, buffer, buffer_len)
