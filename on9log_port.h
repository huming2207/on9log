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
 * Called before sink add/remove and tag-filter mutation.  Non-reentrant;
 * callers must not log or mutate sinks/filters from within sink callbacks.
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
