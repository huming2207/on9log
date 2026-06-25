#pragma once

#include <stdint.h>

/** @brief Magic byte value placed in every on9log packet header for frame validation. */
#define ON9LOG_PACKET_MAGIC 0x9au
/** @brief Special payload length value indicating the payload is streamed (length unknown at header time). */
#define ON9LOG_PAYLOAD_LEN_STREAMING 0xffffu

/** @brief Enumeration of on9log packet types carried in the high nibble of type_level in the header. */
typedef enum {
    /** @brief Normal log message with formatted arguments. */
    ON9LOG_PKT_LOG = 0,
    /** @brief Dropped-packet notification, emitted when the ISR ringbuffer overflows. */
    ON9LOG_PKT_DROPPED = 1,
    /** @brief Time-synchronisation packet used to align sender and receiver timestamps. */
    ON9LOG_PKT_TIME_SYNC = 2,
    /** @brief Boot event packet emitted once after startup. */
    ON9LOG_PKT_BOOT = 3,
    /** @brief Binary buffer dump logged via ON9_LOG_BUF*() macros. */
    ON9LOG_PKT_BUFFER = 4,
} on9log_packet_type_t;

/** @brief Packed binary header for every on9log transport packet. Total size: 18 bytes. */
typedef struct {
    uint8_t magic;        /**< @brief Magic byte; must equal ON9LOG_PACKET_MAGIC (0x9a) for frame validation. */
    uint8_t type_level;   /**< @brief High nibble: on9log_packet_type_t; low nibble: on9log_level_t. */
    uint16_t seq;         /**< @brief Sequence number; wraps naturally on overflow. */
    uint32_t time_ms;     /**< @brief Milliseconds since boot; wraps naturally on overflow. */
    uint32_t tag_id;      /**< @brief ELF address of the tag string for offline symbolisation. */
    uint32_t fmt_id;      /**< @brief ELF address of the format string for offline symbolisation. */
    uint16_t payload_len; /**< @brief Bytes following this header, or ON9LOG_PAYLOAD_LEN_STREAMING for streaming payloads. */
} __attribute__((packed)) on9log_packet_header_t;
