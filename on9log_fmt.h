#pragma once

#include <stdint.h>

/** @brief Magic byte value placed in every on9log packet header for frame validation. */
#define ON9LOG_PACKET_MAGIC 0x9au
/** @brief Reserved payload length value meaning "unknown". The library no longer emits it; decoders may still see it from older firmware. */
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
    uint16_t payload_len; /**< @brief Bytes following this header. */
} __attribute__((packed)) on9log_packet_header_t;

/**
 * @brief Why the chip last restarted, carried in a BOOT packet.
 *
 * The values are part of the wire format; the platform maps them to names.
 * Reasons a chip family does not have are simply never reported.
 */
typedef enum {
    ON9LOG_RESET_UNKNOWN = 0,
    ON9LOG_RESET_POWER_ON = 1,
    ON9LOG_RESET_EXTERNAL = 2,
    ON9LOG_RESET_SOFTWARE = 3,
    ON9LOG_RESET_PANIC = 4,
    ON9LOG_RESET_INT_WDT = 5,
    ON9LOG_RESET_TASK_WDT = 6,
    ON9LOG_RESET_OTHER_WDT = 7,
    ON9LOG_RESET_DEEP_SLEEP = 8,
    ON9LOG_RESET_BROWNOUT = 9,
    ON9LOG_RESET_SDIO = 10,
    ON9LOG_RESET_USB = 11,
    ON9LOG_RESET_JTAG = 12,
    ON9LOG_RESET_EFUSE = 13,
    ON9LOG_RESET_POWER_GLITCH = 14,
    ON9LOG_RESET_CPU_LOCKUP = 15,
} on9log_reset_reason_t;
