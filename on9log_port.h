#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Acquire the on9log mutex/lock.
 *
 * Called before any non-ISR write or sink operation. Must be reentrant-safe
 * if the platform supports nested logging.
 */
void on9log_port_lock(void);
/**
 * @brief Release the on9log mutex/lock previously acquired via on9log_port_lock().
 */
void on9log_port_unlock(void);
/**
 * @brief Return a monotonic millisecond timestamp usable from task (non-ISR) context.
 *
 * @return Milliseconds since an arbitrary epoch (e.g. system boot).
 */
uint32_t on9log_port_timestamp_ms(void);
/**
 * @brief Return a monotonic millisecond timestamp usable from ISR context.
 *
 * @return Milliseconds since an arbitrary epoch, safe to call from interrupt handlers.
 */
uint32_t on9log_port_isr_timestamp_ms(void);
/**
 * @brief Write a byte buffer to the platform's output transport.
 *
 * Used for task-context writes. The implementation must be thread-safe or
 * rely on the caller holding on9log_port_lock().
 *
 * @param[in] data Pointer to the byte buffer to write.
 * @param[in] len  Number of bytes to write.
 */
void on9log_port_write(const uint8_t *data, size_t len);
/**
 * @brief Check whether the ISR packet backend is ready to accept a packet.
 *
 * @return true  if the ISR queue/ringbuffer has capacity,
 * @return false otherwise.
 */
bool on9log_port_isr_ready(void);
/**
 * @brief Enqueue a fully-formed on9log packet from ISR context.
 *
 * The implementation copies the packet into an ISR-safe ringbuffer for later
 * processing by a drain task.
 *
 * @param[in] packet Pointer to the complete on9log packet (header + payload).
 * @param[in] len    Total byte length of the packet.
 *
 * @return true  if the packet was successfully enqueued,
 * @return false if the ringbuffer was full (packet dropped).
 */
bool on9log_port_isr_enqueue_packet(const uint8_t *packet, size_t len);

#ifdef __cplusplus
}
#endif
