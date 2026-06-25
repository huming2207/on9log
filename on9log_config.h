#pragma once

#include <stdint.h>

#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#endif

/**
 * @brief Compile-time maximum log level for this translation unit.
 *
 * Log statements whose level exceeds this value are compiled out entirely.
 * When CONFIG_ON9LOG_MAXIMUM_LEVEL is defined (via sdkconfig), it is used;
 * otherwise the default is 3 (ON9_LOG_LEVEL_INFO).
 *
 * @note This acts as a hard ceiling; runtime level filtering can only further
 *       restrict, never exceed, this value.
 */
#ifndef ON9_LOG_LOCAL_LEVEL
#ifdef CONFIG_ON9LOG_MAXIMUM_LEVEL
#define ON9_LOG_LOCAL_LEVEL CONFIG_ON9LOG_MAXIMUM_LEVEL
#else
#define ON9_LOG_LOCAL_LEVEL 3
#endif
#endif

/**
 * @brief Maximum number of concurrently registered log sinks.
 *
 * Controls the static allocation size of the internal sink registry.
 * When CONFIG_ON9LOG_MAX_SINKS is defined (via sdkconfig), it is used;
 * otherwise the default is 4.
 */
#ifndef ON9LOG_MAX_SINKS
#ifdef CONFIG_ON9LOG_MAX_SINKS
#define ON9LOG_MAX_SINKS CONFIG_ON9LOG_MAX_SINKS
#else
#define ON9LOG_MAX_SINKS 4u
#endif
#endif

/**
 * @brief Size of the data chunk in ON9LOG_PKT_BUFFER packets.
 *
 * When CONFIG_ON9LOG_BUFFER_CHUNK_SIZE is defined (via sdkconfig), it is used;
 * otherwise the default is 3042, which is the largest payload that fits the
 * default ESP stdio VFS transport frame (3072 - 18 byte header - 12 byte buffer metadata).
 *
 * @warning Must be at least 1 (enforced by static assert below).
 */
#ifndef ON9LOG_BUFFER_CHUNK_SIZE
#ifdef CONFIG_ON9LOG_BUFFER_CHUNK_SIZE
#define ON9LOG_BUFFER_CHUNK_SIZE CONFIG_ON9LOG_BUFFER_CHUNK_SIZE
#else
/*
 * Default to the largest ON9LOG_PKT_BUFFER data chunk that fits the default
 * ESP stdio VFS transport frame:
 * 3072 byte transport payload - 18 byte on9log header - 12 byte buffer metadata.
 */
#define ON9LOG_BUFFER_CHUNK_SIZE 3042u
#endif
#endif

#if ON9LOG_BUFFER_CHUNK_SIZE < 1
#error "ON9LOG_BUFFER_CHUNK_SIZE must be at least 1"
#endif

/**
 * @brief Maximum byte length for runtime dynamic string arguments passed to on9log_write().
 *
 * Dynamic strings (char* / const char*) that exceed this length are truncated.
 * When CONFIG_ON9LOG_MAX_DYNAMIC_STRING_LEN is defined (via sdkconfig), it is used;
 * otherwise the default is 1024.
 *
 * @warning Must fit in a non-null uint32_t string length field (enforced by static assert below).
 */
#ifndef ON9LOG_MAX_DYNAMIC_STRING_LEN
#ifdef CONFIG_ON9LOG_MAX_DYNAMIC_STRING_LEN
#define ON9LOG_MAX_DYNAMIC_STRING_LEN CONFIG_ON9LOG_MAX_DYNAMIC_STRING_LEN
#else
#define ON9LOG_MAX_DYNAMIC_STRING_LEN 1024u
#endif
#endif

#if ON9LOG_MAX_DYNAMIC_STRING_LEN > UINT32_MAX - 1u
#error "ON9LOG_MAX_DYNAMIC_STRING_LEN must fit in a non-null uint32_t string length"
#endif

/**
 * @brief Enable firmware-side scanning of original format literals for `%.*s`.
 *
 * Disabled by default because passing the original format literal as a runtime
 * scan hint can keep that literal in the flashed firmware binary in addition to
 * the `.noload` ELF-only copy. When disabled, C `%.*s` string arguments are
 * emitted like normal `%s` dynamic strings: copied up to the first NUL byte or
 * `ON9LOG_MAX_DYNAMIC_STRING_LEN`; the host still applies precision while
 * rendering. Use C++ `std::string_view` for length-aware non-NUL-terminated
 * string slices without retaining format literals in firmware.
 */
#ifndef ON9LOG_ENABLE_FORMAT_SCAN_HINT
#ifdef CONFIG_ON9LOG_ENABLE_FORMAT_SCAN_HINT
#define ON9LOG_ENABLE_FORMAT_SCAN_HINT CONFIG_ON9LOG_ENABLE_FORMAT_SCAN_HINT
#else
#define ON9LOG_ENABLE_FORMAT_SCAN_HINT 0
#endif
#endif

/**
 * @brief Maximum byte size for a single ISR-safe log packet.
 *
 * ISR log packets are pre-allocated from a fixed-size ringbuffer; this constant
 * determines the maximum single-packet capacity. When CONFIG_ON9LOG_ISR_PACKET_MAX
 * is defined (via sdkconfig), it is used; otherwise the default is 128 bytes.
 *
 * @warning Must be at least 32 (enforced by static assert below) to accommodate
 *          the header and minimal payload.
 */
#ifndef ON9LOG_ISR_PACKET_MAX
#ifdef CONFIG_ON9LOG_ISR_PACKET_MAX
#define ON9LOG_ISR_PACKET_MAX CONFIG_ON9LOG_ISR_PACKET_MAX
#else
#define ON9LOG_ISR_PACKET_MAX 128u
#endif
#endif

#if ON9LOG_ISR_PACKET_MAX < 32
#error "ON9LOG_ISR_PACKET_MAX must be at least 32"
#endif
