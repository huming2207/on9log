#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void on9log_port_lock(void);
void on9log_port_unlock(void);
uint32_t on9log_port_timestamp_ms(void);
uint32_t on9log_port_isr_timestamp_ms(void);
void on9log_port_write(const uint8_t *data, size_t len);
bool on9log_port_isr_ready(void);
bool on9log_port_isr_enqueue_packet(const uint8_t *packet, size_t len);

#ifdef __cplusplus
}
#endif
