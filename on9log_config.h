#pragma once

#include <stdint.h>

#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#endif

#ifndef ON9_LOG_LOCAL_LEVEL
#ifdef CONFIG_ON9LOG_MAXIMUM_LEVEL
#define ON9_LOG_LOCAL_LEVEL CONFIG_ON9LOG_MAXIMUM_LEVEL
#else
#define ON9_LOG_LOCAL_LEVEL 3
#endif
#endif

#ifndef ON9LOG_MAX_SINKS
#ifdef CONFIG_ON9LOG_MAX_SINKS
#define ON9LOG_MAX_SINKS CONFIG_ON9LOG_MAX_SINKS
#else
#define ON9LOG_MAX_SINKS 4u
#endif
#endif

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
