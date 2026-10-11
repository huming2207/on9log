#pragma once

#include <stdint.h>

/** @brief Magic byte value placed in every on9log packet header for frame validation. */
#define ON9LOG_PACKET_MAGIC 0x9au
/** @brief Reserved payload length value meaning "unknown". The library no longer emits it; decoders may still see it from older firmware. */
#define ON9LOG_PAYLOAD_LEN_STREAMING 0xffffu

/**
 * @brief tag_id / fmt_id value meaning "the string is copied into the payload".
 *
 * Used when the string's address cannot be resolved from the firmware image,
 * for example a tag built on the stack or a runtime format in RAM. The payload
 * then starts with the inline tag (if any) followed by the inline format (if
 * any), each encoded as a little-endian uint32_t length and that many bytes
 * without a trailing NUL, before the packet's normal payload.
 */
#define ON9LOG_ID_INLINE 0xffffffffu

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
 * @brief Wire format version carried in every BOOT packet.
 *
 * Bumped on any change a decoder must know about. Version 1: exact
 * payload_len in every packet, inline tag/format strings (ON9LOG_ID_INLINE),
 * and the BOOT payload below.
 */
#define ON9LOG_WIRE_FORMAT_VERSION 1u

/**
 * @brief Longest firmware image ID a BOOT packet carries.
 */
#define ON9LOG_IMAGE_ID_MAX_LEN 32u

/**
 * @brief What the image ID in a BOOT packet is, so the host knows how to
 *        compute the same value from the ELF it decodes with.
 *
 * The ID only detects decoding with the wrong ELF. It is self-reported over
 * an unauthenticated stream, so it proves nothing about who built or sent the
 * firmware.
 *
 * BOOT payload (little-endian):
 *   u32 boot_id | u8 reset_reason | u8 wire_format_version |
 *   u8 image_id_kind | u8 image_id_len | u8 image_id[image_id_len]
 */
typedef enum {
    /** No ID available; image_id_len is 0. */
    ON9LOG_IMAGE_ID_NONE = 0,
    /** SHA-256 of the whole ELF file (ESP-IDF's esp_app_desc.app_elf_sha256). */
    ON9LOG_IMAGE_ID_ELF_SHA256 = 1,
    /** Contents of the ELF's NT_GNU_BUILD_ID note (linker --build-id). */
    ON9LOG_IMAGE_ID_GNU_BUILD_ID = 2,
    /** Mach-O LC_UUID of the main image. */
    ON9LOG_IMAGE_ID_MACHO_UUID = 3,
} on9log_image_id_kind_t;

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
