#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "on9log_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Error codes returned by on9log API functions.
 */
typedef enum {
    /** @brief Operation completed successfully. */
    ON9LOG_OK = 0,
    /** @brief Generic or unspecified failure. */
    ON9LOG_ERR_FAIL = -1,
    /** @brief Invalid argument provided to a function. */
    ON9LOG_ERR_INVALID_ARG = -2,
    /** @brief Memory allocation failed. */
    ON9LOG_ERR_NO_MEM = -3,
    /** @brief Requested resource or entry not found. */
    ON9LOG_ERR_NOT_FOUND = -4,
    /** @brief Supplied size is outside the valid range. */
    ON9LOG_ERR_INVALID_SIZE = -5,
} on9log_err_t;

/**
 * @brief Log severity levels.
 *
 * Levels are ordered from most severe (NONE) to least severe (VERBOSE).
 * A filter at a given level passes all messages at that level and above
 * (i.e. lower numeric values).
 */
typedef enum {
    /** @brief No logging; all messages suppressed. */
    ON9_LOG_LEVEL_NONE = 0,
    /** @brief Critical errors that require immediate attention. */
    ON9_LOG_LEVEL_ERROR = 1,
    /** @brief Warning conditions that are not fatal. */
    ON9_LOG_LEVEL_WARN = 2,
    /** @brief Informational messages about normal operation. */
    ON9_LOG_LEVEL_INFO = 3,
    /** @brief Debug messages useful during development. */
    ON9_LOG_LEVEL_DEBUG = 4,
    /** @brief Verbose diagnostic output (lowest severity). */
    ON9_LOG_LEVEL_VERBOSE = 5,
} on9log_level_t;

/**
 * @brief Callback invoked at the start of processing one log packet.
 *
 * @param[in] header     Raw packet header, or NULL for a plain-text record.
 * @param[in] header_len Header size, or zero for a plain-text record.
 * @param[in] ctx        User-supplied context pointer from on9log_sink_t registration.
 */
typedef void (*on9log_sink_start_cb_t)(const uint8_t *header, size_t header_len, void *ctx);

/**
 * @brief Callback invoked for each packet payload or formatted text chunk.
 *
 * @param[in] payload         Pointer to the serialised argument data.
 * @param[in] payload_len     Byte length of the argument data.
 * @param[in] total_arg_cnt   Total number of arguments in this log packet.
 * @param[in] curr_arg_index  Zero-based index of the current argument.
 * @param[in] ctx             User-supplied context pointer from on9log_sink_t registration.
 */
typedef void (*on9log_sink_payload_cb_t)(const uint8_t *payload,
                                         size_t payload_len,
                                         size_t total_arg_cnt,
                                         size_t curr_arg_index,
                                         void *ctx);

/**
 * @brief Callback invoked after all arguments in a log packet have been dispatched.
 *
 * @param[in] ctx User-supplied context pointer from on9log_sink_t registration.
 */
typedef void (*on9log_sink_end_cb_t)(void *ctx);

/**
 * @brief Descriptor for a single log sink.
 *
 * A sink is a set of three callbacks that together consume one decoded on9log
 * packet. Sinks are registered with on9log_add_sink() and removed with
 * on9log_remove_sink().
 */
typedef struct {
    on9log_sink_start_cb_t start_cb;   /**< @brief Called once at the beginning of a packet. */
    on9log_sink_payload_cb_t payload_cb; /**< @brief Called for each argument in the packet. */
    on9log_sink_end_cb_t end_cb;         /**< @brief Called once after all arguments have been dispatched. */
} on9log_sink_t;

/** Plain-text presentation flags. Binary mode accepts but ignores them. */
typedef enum {
    ON9LOG_TEXT_PRETTY = 1u << 0,
    ON9LOG_TEXT_PRETTYLINE = 1u << 1,
    ON9LOG_TEXT_SHOWLEVEL = 1u << 2,
    ON9LOG_TEXT_SHOWTOPIC = 1u << 3,
    ON9LOG_TEXT_SHOWTIMESTAMP = 1u << 4,
    ON9LOG_TEXT_CR = 1u << 5,
} on9log_text_flag_t;

#define ON9LOG_TEXT_DEFAULT_FLAGS \
    ((uint8_t)(ON9LOG_TEXT_PRETTY | ON9LOG_TEXT_SHOWTOPIC | \
               ON9LOG_TEXT_SHOWTIMESTAMP | ON9LOG_TEXT_CR))

#if ON9LOG_PLAIN_TEXT
/** @brief Output callback used by a plain-text formatter. */
typedef void (*on9log_text_output_cb_t)(const char *data, size_t len, void *ctx);

/**
 * @brief Formatter callback used by the C++ wrapper and other streaming text
 *        producers.
 */
typedef void (*on9log_text_formatter_cb_t)(on9log_text_output_cb_t output,
                                           void *output_ctx,
                                           void *formatter_ctx);
#endif

/**
 * @brief Initialize global on9log resources.
 *
 * Plain-text mode creates one unbuffered callback-backed FILE used by all
 * C-style printf logs. Binary mode is an idempotent no-op.
 */
on9log_err_t on9log_init(void);

/**
 * @brief Register a log sink to receive decoded log packets.
 *
 * @param[in] sink Pointer to the sink descriptor. The pointed-to data must remain
 *                 valid for the lifetime of the sink.
 * @param[in] ctx  Opaque user context passed to every sink callback invocation.
 *
 * @return ON9LOG_OK on success, or an error code if the sink registry is full.
 */
on9log_err_t on9log_add_sink(const on9log_sink_t *sink, void *ctx);

/**
 * @brief Remove a previously registered log sink.
 *
 * Both the sink descriptor pointer and the context pointer must match the
 * values used during registration.
 *
 * @param[in] sink Pointer to the sink descriptor to remove.
 * @param[in] ctx  User context that was passed at registration time.
 *
 * @return ON9LOG_OK on success, or ON9LOG_ERR_NOT_FOUND if the sink was not registered.
 */
on9log_err_t on9log_remove_sink(const on9log_sink_t *sink, void *ctx);

/**
 * @brief Return the pending dropped-packet count.
 *
 * A packet is counted as dropped when the logger cannot encode it or the ISR
 * ringbuffer cannot accept it. The count is cleared internally when the logger
 * emits an ON9LOG_PKT_DROPPED notification before a later successful packet.
 * Calling this function only reads the current pending value.
 *
 * @return Pending dropped-packet count.
 */
uint32_t on9log_get_dropped_count(void);

/*
 * Runtime level filtering. The compile-time ON9_LOG_LOCAL_LEVEL remains a hard
 * ceiling; runtime levels can only suppress logs that survived that ceiling.
 * Tag filters are matched by pointer first, then by string contents.
 */

/**
 * @brief Set the global runtime log level.
 *
 * Messages with a severity above (numeric value greater than) this level are
 * suppressed. The compile-time ON9_LOG_LOCAL_LEVEL is the hard upper bound.
 *
 * @param[in] level Maximum log level to allow.
 */
void on9log_set_level(on9log_level_t level);

/**
 * @brief Get the current global runtime log level.
 *
 * @return The currently active on9log_level_t value.
 */
on9log_level_t on9log_get_level(void);

/**
 * @brief Override the runtime log level for a specific tag.
 *
 * When a tag-level override is set, messages with that tag use the override
 * level instead of the global level, provided the override is more restrictive.
 *
 * @param[in] tag   The tag string to filter.
 * @param[in] level Maximum log level allowed for this tag.
 *
 * @return ON9LOG_OK on success, or an error code on failure.
 */
on9log_err_t on9log_set_tag_level(const char *tag, on9log_level_t level);

/**
 * @brief Remove a tag-level filter previously set with on9log_set_tag_level().
 *
 * After removal, messages with that tag fall back to the global runtime level.
 *
 * @param[in] tag The tag string whose override should be cleared.
 *
 * @return ON9LOG_OK on success, or ON9LOG_ERR_NOT_FOUND if no override exists.
 */
on9log_err_t on9log_clear_tag_level(const char *tag);

/* on9log_write() and ON9_LOGx() must not be called from ISR context. */

/**
 * @brief Write a formatted log message (task context only).
 *
 * Encodes the level, tag, format string address, argument types and variadic
 * arguments into an on9log packet and dispatches it through registered sinks.
 *
 * @warning Must not be called from ISR context. Use on9log_write_isr() instead.
 *
 * @param[in] level     Severity level for this message.
 * @param[in] tag       Tag string (stored in .noload section when a compile-time constant).
 * @param[in] format    printf-style format string.
 * @param[in] arg_types Compact type-descriptor string built by ON9_LOG_ARGS_TYPE().
 * @param[in] ...       Variadic arguments matching the format string.
 */
void on9log_write(on9log_level_t level,
                  const char *tag,
                  const char *format,
                  const char *arg_types,
                  ...) __attribute__((format(printf, 3, 5)));

/**
 * @brief Write a log with per-Logger plain-text presentation flags.
 *
 * In binary mode @p text_flags is deliberately ignored and the emitted
 * packet is identical to on9log_write().
 */
void on9log_write_with_text_flags(on9log_level_t level,
                                  const char *tag,
                                  uint8_t text_flags,
                                  const char *format,
                                  const char *arg_types,
                                  ...) __attribute__((format(printf, 4, 6)));

/**
 * @brief Write a log message with an optional original-format scan hint.
 *
 * Direct callers may pass @c format_scan when firmware-side `%.*s` detection is
 * wanted. @c ON9_LOGx() callsites normally use
 * @ref on9log_write_with_format_scan_metadata instead to avoid rescanning
 * argument metadata.
 */
void on9log_write_with_format_scan(on9log_level_t level,
                                   const char *tag,
                                   const char *format,
                                   const char *format_scan,
                                   const char *arg_types,
                                   ...) __attribute__((format(printf, 3, 6)));

/**
 * @brief Write a log message with precomputed argument metadata.
 *
 * This is the optimized entry point used by @c ON9_LOGx(); @c arg_count and
 * @c has_dynamic_string are computed by the macro so the C encoder can avoid
 * rescanning @c arg_types on the hot path.
 */
void on9log_write_with_format_scan_metadata(on9log_level_t level,
                                            const char *tag,
                                            const char *format,
                                            const char *format_scan,
                                            const char *arg_types,
                                            uint8_t arg_count,
                                            int has_dynamic_string,
                                            ...) __attribute__((format(printf, 3, 8)));

#if ON9LOG_PLAIN_TEXT
/**
 * @brief Stream one already-formatted plain-text log body through registered
 *        sinks.
 *
 * The logger emits the ANSI level prefix and final reset/newline around the
 * bytes produced by @p formatter. Sink start callbacks receive a NULL header
 * with length zero in plain-text mode.
 */
void on9log_write_text(on9log_level_t level,
                       const char *tag,
                       on9log_text_formatter_cb_t formatter,
                       void *formatter_ctx);

void on9log_write_text_with_flags(on9log_level_t level,
                                  const char *tag,
                                  uint8_t text_flags,
                                  on9log_text_formatter_cb_t formatter,
                                  void *formatter_ctx);

/**
 * @brief Enqueue an already-formatted plain-text body from ISR context.
 *
 * The final ANSI log line is bounded by ON9LOG_ISR_PACKET_MAX and dispatched
 * by the normal ISR drain task.
 */
bool on9log_write_text_isr(on9log_level_t level,
                           const char *tag,
                           const char *text,
                           size_t text_len);

bool on9log_write_text_isr_with_flags(on9log_level_t level,
                                      const char *tag,
                                      uint8_t text_flags,
                                      const char *text,
                                      size_t text_len);
#endif

/*
 * Encodes a bounded packet and enqueues it through the platform ISR backend.
 * Dynamic string arguments are not supported on the ISR path.
 */

/**
 * @brief Write a formatted log message from ISR context.
 *
 * Encodes a bounded on9log packet and enqueues it through the platform ISR
 * ringbuffer for later processing by a drain task.
 *
 * @note Dynamic string arguments are NOT supported on the ISR path; only
 *       32-bit scalar, 64-bit scalar, and pointer arguments are safe.
 *
 * @param[in] level     Severity level for this message.
 * @param[in] tag       Tag string.
 * @param[in] format    printf-style format string.
 * @param[in] arg_types Compact type-descriptor string built by ON9_LOG_ARGS_TYPE().
 * @param[in] ...       Variadic arguments matching the format string.
 *
 * @return true  if the packet was successfully enqueued,
 * @return false if the ISR ringbuffer was full (packet dropped).
 */
bool on9log_write_isr(on9log_level_t level,
                      const char *tag,
                      const char *format,
                      const char *arg_types,
                      ...) __attribute__((format(printf, 3, 5)));

bool on9log_write_isr_with_text_flags(on9log_level_t level,
                                      const char *tag,
                                      uint8_t text_flags,
                                      const char *format,
                                      const char *arg_types,
                                      ...) __attribute__((format(printf, 4, 6)));

/* on9log_write_buffer() and ON9_LOG_BUF*() must not be called from ISR context. */

/**
 * @brief Write a raw binary buffer as a log message (task context only).
 *
 * The buffer contents are packed into one or more ON9LOG_PKT_BUFFER packets
 * and dispatched through registered sinks.
 *
 * @warning Must not be called from ISR context.
 *
 * @param[in] level      Severity level for this message.
 * @param[in] tag        Tag string.
 * @param[in] buffer     Pointer to the binary data to log.
 * @param[in] buffer_len Byte length of the buffer.
 */
void on9log_write_buffer(on9log_level_t level,
                         const char *tag,
                         const void *buffer,
                         size_t buffer_len);

void on9log_write_buffer_with_text_flags(on9log_level_t level,
                                         const char *tag,
                                         uint8_t text_flags,
                                         const void *buffer,
                                         size_t buffer_len);

/**
 * @brief Dispatch a complete raw on9log packet through the normal sink path.
 *
 * Parses the packet header and dispatches each argument payload through
 * registered sink callbacks. Useful for forwarding or replaying packets from
 * an external source.
 *
 * @param[in] packet     Pointer to the complete on9log packet (header + payload).
 * @param[in] packet_len Total byte length of the packet.
 *
 * @return ON9LOG_OK on success, or an error code if the packet is malformed.
 */
on9log_err_t on9log_dispatch_packet(const uint8_t *packet, size_t packet_len);

#ifdef __cplusplus
}
#endif

/**
 * @brief Stringification helper (indirection layer).
 *
 * @param[in] x Macro argument to stringify.
 * @return String literal of the expanded argument.
 */
#define ON9_LOG_STRINGIFY2(x) #x

/**
 * @brief Stringify a macro argument after expansion.
 *
 * @param[in] x Macro argument to expand and stringify.
 * @return String literal of the fully expanded argument.
 */
#define ON9_LOG_STRINGIFY(x) ON9_LOG_STRINGIFY2(x)

/**
 * @brief Count the number of variadic macro arguments (up to 16).
 *
 * @return Integer literal representing the argument count (0–16).
 */
#define ON9_LOG_VA_NARG(...) ON9_LOG_VA_NARG_IMPL(_, ##__VA_ARGS__, \
    16, 15, 14, 13, 12, 11, 10, 9, \
    8, 7, 6, 5, 4, 3, 2, 1, 0)

/** @cond INTERNAL */
#define ON9_LOG_VA_NARG_IMPL(_0, _1, _2, _3, _4, _5, _6, _7, _8, _9, \
                             _10, _11, _12, _13, _14, _15, _16, N, ...) N
/** @endcond */

/**
 * @brief Macro attribute for placing a string literal in the .noload ELF section.
 *
 * On non-Apple platforms, strings marked with this attribute are kept in the
 * ELF binary but never loaded into RAM. Apple platforms define this as empty
 * because the mach-o format does not support .noload sections natively.
 */
#if ON9LOG_PLAIN_TEXT
#ifdef ON9_LOG_NOLOAD_ATTR
#undef ON9_LOG_NOLOAD_ATTR
#endif
#define ON9_LOG_NOLOAD_ATTR
#elif !defined(ON9_LOG_NOLOAD_ATTR)
#if defined(__APPLE__)
#define ON9_LOG_NOLOAD_ATTR
#else
#define ON9_LOG_NOLOAD_ATTR \
    __attribute__((section(".noload_keep_in_elf." ON9_LOG_STRINGIFY(__COUNTER__))))
#endif
#endif

/**
 * @brief Store a string literal in the .noload ELF section so it is
 *        available for offline symbolisation but never loaded into RAM.
 *
 * The string is declared as a static const variable with a unique section
 * name generated via __COUNTER__.
 *
 * @param[in] str String literal to store.
 *
 * @return Pointer to the .noload-resident string.
 */
#if ON9LOG_PLAIN_TEXT
#define ON9_LOG_NOLOAD_STR(str) (str)
#else
#define ON9_LOG_NOLOAD_STR(str) \
    (__extension__({ \
        static const ON9_LOG_NOLOAD_ATTR char __on9log_str[] = (str); \
        (const char *)__on9log_str; \
    }))
#endif

/**
 * @brief Return a .noload-resident pointer for compile-time constant strings,
 *        or pass through runtime strings unchanged.
 *
 * @param[in] str String that may or may not be a compile-time constant.
 *
 * @return Pointer to the string (possibly in the .noload section).
 */
#if ON9LOG_PLAIN_TEXT
#define ON9_LOG_ATTR_STR(str) (str)
#else
#define ON9_LOG_ATTR_STR(str) (__builtin_constant_p(str) ? ON9_LOG_NOLOAD_STR(str) : (str))
#endif

/**
 * @defgroup diag_macros Compiler diagnostic pragma helpers
 * @{
 */

/** @brief Push the current compiler diagnostic state. */
#if defined(__GNUC__)
#define ON9_LOG_DIAG_PUSH _Pragma("GCC diagnostic push")

/** @brief Pop to the previous compiler diagnostic state. */
#define ON9_LOG_DIAG_POP _Pragma("GCC diagnostic pop")

/*
 * Avoid some weird issues on false-positive string formatting warning/errors from Clangd
 */

/** @brief Suppress GCC format-overflow, format-nonliteral, and format-security warnings. */
#define ON9_LOG_DIAG_IGNORE_FORMAT_OVERFLOW \
    _Pragma("GCC diagnostic ignored \"-Wformat-overflow\"") \
    _Pragma("GCC diagnostic ignored \"-Wformat-nonliteral\"") \
    _Pragma("GCC diagnostic ignored \"-Wformat-security\"")
#else
#define ON9_LOG_DIAG_PUSH
#define ON9_LOG_DIAG_POP
#define ON9_LOG_DIAG_IGNORE_FORMAT_OVERFLOW
#endif
/** @} */

/**
 * @brief Enumeration of argument type tags used in the compact type-descriptor string.
 *
 * Each variadic argument to a log call is classified into one of these types
 * so the decoder can correctly deserialise the packet payload.
 */
typedef enum {
    /** @brief No argument (sentinel). */
    ON9_LOG_ARGS_TYPE_NONE = 0,
    /** @brief 32-bit integer or other 4-byte scalar. */
    ON9_LOG_ARGS_TYPE_32BITS = 1,
    /** @brief 64-bit integer, double-precision float, or other 8-byte scalar. */
    ON9_LOG_ARGS_TYPE_64BITS = 2,
    /** @brief Pointer type encoded as a 32-bit address. */
    ON9_LOG_ARGS_TYPE_POINTER = 3,
    /** @brief Dynamic string (char* or const char*) with inline length prefix. */
    ON9_LOG_ARGS_TYPE_DYNAMIC_STRING = 4,
    /** @brief Length-aware dynamic string descriptor with inline length prefix. */
    ON9_LOG_ARGS_TYPE_DYNAMIC_STRING_VIEW = 5,
} on9log_args_type_t;

/**
 * @brief Length-aware string argument descriptor.
 *
 * Used by the C++ wrapper for std::string and std::string_view arguments so the
 * logger can copy exactly len bytes without requiring NUL termination.
 */
typedef struct {
    const char *data;
    size_t len;
} on9log_string_view_t;

/**
 * @brief Sentinel type used to terminate a variadic argument list in
 *        compile-time type-detection logic.
 */
typedef struct {
    unsigned tmp; /**< @brief Unused placeholder field. */
} on9log_args_end_t;

/** @cond INTERNAL */
/* Argument type initialisation macros (one per supported argument count). */
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
/** @endcond */

#ifndef __cplusplus
/**
 * @brief Detect the on9log argument type of an expression at compile time (C11 _Generic).
 *
 * Maps common C types to their on9log_args_type_t classification.
 *
 * @param[in] arg Expression whose type is to be detected.
 *
 * @return An on9log_args_type_t value as a char.
 */
#define ON9_LOG_DETECT_TYPE(arg) ( \
    _Generic((arg), \
        on9log_args_end_t: ON9_LOG_ARGS_TYPE_NONE, \
        char*: ON9_LOG_ARGS_TYPE_DYNAMIC_STRING, \
        const char*: ON9_LOG_ARGS_TYPE_DYNAMIC_STRING, \
        void*: ON9_LOG_ARGS_TYPE_POINTER, \
        const void*: ON9_LOG_ARGS_TYPE_POINTER, \
        uint8_t*: ON9_LOG_ARGS_TYPE_POINTER, \
        const uint8_t*: ON9_LOG_ARGS_TYPE_POINTER, \
        long long int: ON9_LOG_ARGS_TYPE_64BITS, \
        long long unsigned int: ON9_LOG_ARGS_TYPE_64BITS, \
        double: ON9_LOG_ARGS_TYPE_64BITS, \
        float: ON9_LOG_ARGS_TYPE_64BITS, \
        default: ON9_LOG_ARGS_TYPE_32BITS))
#else
extern "C++" {
/**
 * @brief Compile-time argument type trait (C++ template, primary template).
 *
 * The primary template maps any unknown type to ON9_LOG_ARGS_TYPE_32BITS.
 * Explicit specialisations below handle specific types.
 */
template <typename T>
struct On9LogArgType {
    constexpr static unsigned long long log_type = ON9_LOG_ARGS_TYPE_32BITS;
    constexpr static unsigned long long constant_log_type = log_type;
};

/** @brief Specialisation for on9log_args_end_t (sentinel). */
template <>
struct On9LogArgType<on9log_args_end_t> {
    constexpr static unsigned long long log_type = ON9_LOG_ARGS_TYPE_NONE;
    constexpr static unsigned long long constant_log_type = log_type;
};

/** @brief Specialisation for char* (dynamic string). */
template <>
struct On9LogArgType<char *> {
    constexpr static unsigned long long log_type = ON9_LOG_ARGS_TYPE_DYNAMIC_STRING;
    constexpr static unsigned long long constant_log_type = log_type;
};

/** @brief Specialisation for const char* (dynamic string). */
template <>
struct On9LogArgType<const char *> {
    constexpr static unsigned long long log_type = ON9_LOG_ARGS_TYPE_DYNAMIC_STRING;
    constexpr static unsigned long long constant_log_type = log_type;
};

/** @brief Specialisation for void* (pointer). */
template <>
struct On9LogArgType<void *> {
    constexpr static unsigned long long log_type = ON9_LOG_ARGS_TYPE_POINTER;
    constexpr static unsigned long long constant_log_type = log_type;
};

/** @brief Specialisation for const void* (pointer). */
template <>
struct On9LogArgType<const void *> {
    constexpr static unsigned long long log_type = ON9_LOG_ARGS_TYPE_POINTER;
    constexpr static unsigned long long constant_log_type = log_type;
};

/** @brief Specialisation for uint8_t* (pointer). */
template <>
struct On9LogArgType<uint8_t *> {
    constexpr static unsigned long long log_type = ON9_LOG_ARGS_TYPE_POINTER;
    constexpr static unsigned long long constant_log_type = log_type;
};

/** @brief Specialisation for const uint8_t* (pointer). */
template <>
struct On9LogArgType<const uint8_t *> {
    constexpr static unsigned long long log_type = ON9_LOG_ARGS_TYPE_POINTER;
    constexpr static unsigned long long constant_log_type = log_type;
};

/** @brief Specialisation for char[N] arrays (dynamic string). */
template <size_t N>
struct On9LogArgType<char[N]> {
    constexpr static unsigned long long log_type = ON9_LOG_ARGS_TYPE_DYNAMIC_STRING;
    constexpr static unsigned long long constant_log_type = log_type;
};

/** @brief Specialisation for const char[N] arrays (dynamic string). */
template <size_t N>
struct On9LogArgType<const char[N]> {
    constexpr static unsigned long long log_type = ON9_LOG_ARGS_TYPE_DYNAMIC_STRING;
    constexpr static unsigned long long constant_log_type = log_type;
};

/** @brief Specialisation for uint8_t[N] arrays (pointer). */
template <size_t N>
struct On9LogArgType<uint8_t[N]> {
    constexpr static unsigned long long log_type = ON9_LOG_ARGS_TYPE_POINTER;
    constexpr static unsigned long long constant_log_type = log_type;
};

/** @brief Specialisation for const uint8_t[N] arrays (pointer). */
template <size_t N>
struct On9LogArgType<const uint8_t[N]> {
    constexpr static unsigned long long log_type = ON9_LOG_ARGS_TYPE_POINTER;
    constexpr static unsigned long long constant_log_type = log_type;
};

/** @brief Specialisation for long long int (64-bit). */
template <>
struct On9LogArgType<long long int> {
    constexpr static unsigned long long log_type = ON9_LOG_ARGS_TYPE_64BITS;
    constexpr static unsigned long long constant_log_type = log_type;
};

/** @brief Specialisation for long long unsigned int (64-bit). */
template <>
struct On9LogArgType<long long unsigned int> {
    constexpr static unsigned long long log_type = ON9_LOG_ARGS_TYPE_64BITS;
    constexpr static unsigned long long constant_log_type = log_type;
};

/** @brief Specialisation for double (64-bit floating point). */
template <>
struct On9LogArgType<double> {
    constexpr static unsigned long long log_type = ON9_LOG_ARGS_TYPE_64BITS;
    constexpr static unsigned long long constant_log_type = log_type;
};

/** @brief Specialisation for float (64-bit — promoted to double in variadic args). */
template <>
struct On9LogArgType<float> {
    constexpr static unsigned long long log_type = ON9_LOG_ARGS_TYPE_64BITS;
    constexpr static unsigned long long constant_log_type = log_type;
};

/**
 * @brief C++ implementation of compile-time argument type detection.
 *
 * @tparam T         The deduced argument type.
 * @param[in] is_constant Whether the argument is a compile-time constant.
 *
 * @return The on9log_args_type_t classification as unsigned long long.
 */
template <typename T>
constexpr unsigned long long ON9_LOG_DETECT_TYPE_IMPL(const T &, bool is_constant)
{
    return is_constant ? On9LogArgType<T>::constant_log_type : On9LogArgType<T>::log_type;
}
}

/**
 * @brief Detect the on9log argument type of an expression at compile time (C++).
 *
 * Delegates to the ON9_LOG_DETECT_TYPE_IMPL template function.
 */
#define ON9_LOG_DETECT_TYPE(arg) ON9_LOG_DETECT_TYPE_IMPL((arg), __builtin_constant_p(arg))
#endif

/**
 * @brief Build a compact NUL-terminated type-descriptor string for the given variadic arguments.
 *
 * Each argument is classified via ON9_LOG_DETECT_TYPE and the resulting byte
 * values are stored in a static char array.
 *
 * @param[in] ... Variadic arguments whose types should be recorded.
 *
 * @return Pointer to the NUL-terminated type-descriptor string.
 */
#define ON9_LOG_ARGS_TYPE(...) (__extension__({ \
    static const char __on9log_arg_types[] = { __VA_OPT__(ON9_LOG_INIT_ARG_TYPE(ON9_LOG_VA_NARG(__VA_ARGS__), __VA_ARGS__), ) 0 }; \
    (const char *)&__on9log_arg_types; \
}))

#define ON9_LOG_ARGS_HAVE_STRING(arg_types) (__extension__({ \
    bool __on9log_has_string = false; \
    const char *__on9log_type_it = (arg_types); \
    if (__on9log_type_it != NULL) { \
        for (unsigned __on9log_type_idx = 0; __on9log_type_idx < 255u; ++__on9log_type_idx) { \
            unsigned char __on9log_type = (unsigned char)__on9log_type_it[__on9log_type_idx]; \
            if (__on9log_type == ON9_LOG_ARGS_TYPE_NONE) { \
                break; \
            } \
            if (__on9log_type == ON9_LOG_ARGS_TYPE_DYNAMIC_STRING || \
                __on9log_type == ON9_LOG_ARGS_TYPE_DYNAMIC_STRING_VIEW) { \
                __on9log_has_string = true; \
                break; \
            } \
        } \
    } \
    __on9log_has_string; \
}))

#if ON9LOG_ENABLE_FORMAT_SCAN_HINT
#define ON9_LOG_FORMAT_SCAN_HINT(format, has_string) ((has_string) ? (format) : NULL)
#else
#define ON9_LOG_FORMAT_SCAN_HINT(format, has_string) ((void)(format), (void)(has_string), (const char *)NULL)
#endif

/**
 * @brief Force an argument to be encoded as a pointer, for example with %p.
 *
 * char* and const char* arguments are encoded as dynamic strings by default;
 * wrap them in ON9_PTR() when the pointer value itself should be logged.
 */
#define ON9_PTR(x) ((const void *)(x))

/**
 * @brief Check whether the given log level is enabled at compile time.
 *
 * @param[in] level Log level to test.
 *
 * @return true if the level is non-NONE and <= ON9_LOG_LOCAL_LEVEL,
 * @return false otherwise.
 */
#define ON9_LOG_ENABLED(level) ((level) != ON9_LOG_LEVEL_NONE && (level) <= ON9_LOG_LOCAL_LEVEL)

/**
 * @brief Core logging macro.
 *
 * Checks ON9_LOG_ENABLED(), suppresses compiler format warnings, and calls
 * on9log_write() with the format string stored in the .noload section.
 *
 * @param[in] level   Severity level.
 * @param[in] tag     Tag string.
 * @param[in] format  printf-style format string.
 * @param[in] ...     Variadic arguments.
 */
#if ON9LOG_PLAIN_TEXT
#define ON9_LOG_LEVEL(level, tag, format, ...) do { \
        if (ON9_LOG_ENABLED(level)) { \
            ON9_LOG_DIAG_PUSH; \
            ON9_LOG_DIAG_IGNORE_FORMAT_OVERFLOW; \
            on9log_write((level), (tag), ON9_LOG_ATTR_STR(format), NULL, ##__VA_ARGS__); \
            ON9_LOG_DIAG_POP; \
        } \
    } while (0)
#else
#define ON9_LOG_LEVEL(level, tag, format, ...) do { \
        if (ON9_LOG_ENABLED(level)) { \
            ON9_LOG_DIAG_PUSH; \
            ON9_LOG_DIAG_IGNORE_FORMAT_OVERFLOW; \
            const char *__on9log_arg_types = ON9_LOG_ARGS_TYPE(__VA_ARGS__); \
            bool __on9log_has_string = ON9_LOG_ARGS_HAVE_STRING(__on9log_arg_types); \
            const char *__on9log_format_scan = ON9_LOG_FORMAT_SCAN_HINT((format), __on9log_has_string); \
            on9log_write_with_format_scan_metadata((level), \
                                                   (tag), \
                                                   ON9_LOG_ATTR_STR(format), \
                                                   __on9log_format_scan, \
                                                   __on9log_arg_types, \
                                                   (uint8_t)ON9_LOG_VA_NARG(__VA_ARGS__), \
                                                   __on9log_has_string, \
                                                   ##__VA_ARGS__); \
            ON9_LOG_DIAG_POP; \
        } \
    } while (0)
#endif

/**
 * @brief Log an ERROR-level message (task context).
 *
 * @param[in] tag     Tag string.
 * @param[in] format  printf-style format string.
 * @param[in] ...     Variadic arguments.
 */
#define ON9_LOGE(tag, format, ...) ON9_LOG_LEVEL(ON9_LOG_LEVEL_ERROR, tag, format, ##__VA_ARGS__)

/**
 * @brief Log a WARN-level message (task context).
 *
 * @param[in] tag     Tag string.
 * @param[in] format  printf-style format string.
 * @param[in] ...     Variadic arguments.
 */
#define ON9_LOGW(tag, format, ...) ON9_LOG_LEVEL(ON9_LOG_LEVEL_WARN, tag, format, ##__VA_ARGS__)

/**
 * @brief Log an INFO-level message (task context).
 *
 * @param[in] tag     Tag string.
 * @param[in] format  printf-style format string.
 * @param[in] ...     Variadic arguments.
 */
#define ON9_LOGI(tag, format, ...) ON9_LOG_LEVEL(ON9_LOG_LEVEL_INFO, tag, format, ##__VA_ARGS__)

/**
 * @brief Log a DEBUG-level message (task context).
 *
 * @param[in] tag     Tag string.
 * @param[in] format  printf-style format string.
 * @param[in] ...     Variadic arguments.
 */
#define ON9_LOGD(tag, format, ...) ON9_LOG_LEVEL(ON9_LOG_LEVEL_DEBUG, tag, format, ##__VA_ARGS__)

/**
 * @brief Log a VERBOSE-level message (task context).
 *
 * @param[in] tag     Tag string.
 * @param[in] format  printf-style format string.
 * @param[in] ...     Variadic arguments.
 */
#define ON9_LOGV(tag, format, ...) ON9_LOG_LEVEL(ON9_LOG_LEVEL_VERBOSE, tag, format, ##__VA_ARGS__)

/**
 * @brief ISR-safe core logging macro.
 *
 * Builds the argument-types array on the stack (not in .noload) and calls
 * on9log_write_isr(). Dynamic string arguments are not supported on the
 * ISR path.
 *
 * @param[in] level   Severity level.
 * @param[in] tag     Tag string.
 * @param[in] format  printf-style format string.
 * @param[in] ...     Variadic arguments.
 */
#define ON9_ISR_LOG_LEVEL(level, tag, format, ...) do { \
        if (ON9_LOG_ENABLED(level)) { \
            ON9_LOG_DIAG_PUSH; \
            ON9_LOG_DIAG_IGNORE_FORMAT_OVERFLOW; \
            const char __on9log_isr_arg_types[] = { __VA_OPT__(ON9_LOG_INIT_ARG_TYPE(ON9_LOG_VA_NARG(__VA_ARGS__), __VA_ARGS__), ) 0 }; \
            (void)on9log_write_isr((level), (tag), ON9_LOG_ATTR_STR(format), (const char *)__on9log_isr_arg_types, ##__VA_ARGS__); \
            ON9_LOG_DIAG_POP; \
        } \
    } while (0)

/**
 * @brief Log an ERROR-level message from ISR context.
 *
 * @param[in] tag     Tag string.
 * @param[in] format  printf-style format string.
 * @param[in] ...     Variadic arguments.
 */
#define ON9_ISR_LOGE(tag, format, ...) ON9_ISR_LOG_LEVEL(ON9_LOG_LEVEL_ERROR, tag, format, ##__VA_ARGS__)

/**
 * @brief Log a WARN-level message from ISR context.
 *
 * @param[in] tag     Tag string.
 * @param[in] format  printf-style format string.
 * @param[in] ...     Variadic arguments.
 */
#define ON9_ISR_LOGW(tag, format, ...) ON9_ISR_LOG_LEVEL(ON9_LOG_LEVEL_WARN, tag, format, ##__VA_ARGS__)

/**
 * @brief Log an INFO-level message from ISR context.
 *
 * @param[in] tag     Tag string.
 * @param[in] format  printf-style format string.
 * @param[in] ...     Variadic arguments.
 */
#define ON9_ISR_LOGI(tag, format, ...) ON9_ISR_LOG_LEVEL(ON9_LOG_LEVEL_INFO, tag, format, ##__VA_ARGS__)

/**
 * @brief Log a DEBUG-level message from ISR context.
 *
 * @param[in] tag     Tag string.
 * @param[in] format  printf-style format string.
 * @param[in] ...     Variadic arguments.
 */
#define ON9_ISR_LOGD(tag, format, ...) ON9_ISR_LOG_LEVEL(ON9_LOG_LEVEL_DEBUG, tag, format, ##__VA_ARGS__)

/**
 * @brief Log a VERBOSE-level message from ISR context.
 *
 * @param[in] tag     Tag string.
 * @param[in] format  printf-style format string.
 * @param[in] ...     Variadic arguments.
 */
#define ON9_ISR_LOGV(tag, format, ...) ON9_ISR_LOG_LEVEL(ON9_LOG_LEVEL_VERBOSE, tag, format, ##__VA_ARGS__)

/**
 * @brief Core buffer-logging macro (task context).
 *
 * Calls on9log_write_buffer() to log a raw binary buffer.
 *
 * @param[in] level      Severity level.
 * @param[in] tag        Tag string.
 * @param[in] buffer     Pointer to the binary data.
 * @param[in] buffer_len Byte length of the buffer.
 */
#define ON9_LOG_BUF_LEVEL(level, tag, buffer, buffer_len) do { \
        if (ON9_LOG_ENABLED(level)) { \
            on9log_write_buffer((level), (tag), (buffer), (buffer_len)); \
        } \
    } while (0)

/**
 * @brief Log a raw ERROR-level binary buffer (task context).
 *
 * @param[in] tag        Tag string.
 * @param[in] buffer     Pointer to the binary data.
 * @param[in] buffer_len Byte length of the buffer.
 */
#define ON9_LOG_BUFE(tag, buffer, buffer_len) ON9_LOG_BUF_LEVEL(ON9_LOG_LEVEL_ERROR, tag, buffer, buffer_len)

/**
 * @brief Log a raw WARN-level binary buffer (task context).
 *
 * @param[in] tag        Tag string.
 * @param[in] buffer     Pointer to the binary data.
 * @param[in] buffer_len Byte length of the buffer.
 */
#define ON9_LOG_BUFW(tag, buffer, buffer_len) ON9_LOG_BUF_LEVEL(ON9_LOG_LEVEL_WARN, tag, buffer, buffer_len)

/**
 * @brief Log a raw INFO-level binary buffer (task context).
 *
 * @param[in] tag        Tag string.
 * @param[in] buffer     Pointer to the binary data.
 * @param[in] buffer_len Byte length of the buffer.
 */
#define ON9_LOG_BUFI(tag, buffer, buffer_len) ON9_LOG_BUF_LEVEL(ON9_LOG_LEVEL_INFO, tag, buffer, buffer_len)

/**
 * @brief Log a raw DEBUG-level binary buffer (task context).
 *
 * @param[in] tag        Tag string.
 * @param[in] buffer     Pointer to the binary data.
 * @param[in] buffer_len Byte length of the buffer.
 */
#define ON9_LOG_BUFD(tag, buffer, buffer_len) ON9_LOG_BUF_LEVEL(ON9_LOG_LEVEL_DEBUG, tag, buffer, buffer_len)

/**
 * @brief Log a raw VERBOSE-level binary buffer (task context).
 *
 * @param[in] tag        Tag string.
 * @param[in] buffer     Pointer to the binary data.
 * @param[in] buffer_len Byte length of the buffer.
 */
#define ON9_LOG_BUFV(tag, buffer, buffer_len) ON9_LOG_BUF_LEVEL(ON9_LOG_LEVEL_VERBOSE, tag, buffer, buffer_len)
