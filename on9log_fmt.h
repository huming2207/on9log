#pragma once

#include <stdint.h>

#define ON9LOG_PACKET_MAGIC 0x9au

typedef enum {
    ON9LOG_PKT_LOG = 0,
    ON9LOG_PKT_DROPPED = 1,
    ON9LOG_PKT_TIME_SYNC = 2,
    ON9LOG_PKT_BOOT = 3,
} on9log_packet_type_t;

typedef struct {
    uint8_t magic;        // ON9LOG_PACKET_MAGIC
    uint8_t type_level;   // high nibble: on9log_packet_type_t, low nibble: esp_log_level_t
    uint16_t seq;         // wraps naturally
    uint32_t time_ms;     // milliseconds since boot, wraps naturally
    uint32_t tag_id;      // tag string address in ELF
    uint32_t fmt_id;      // format string address in ELF
    uint16_t payload_len; // bytes after this header
} __attribute__((packed)) on9log_packet_header_t;
