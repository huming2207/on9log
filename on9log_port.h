#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "on9log_fmt.h"

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
/** Yield execution while an infrequent task-context mutation waits for readers. */
void on9log_port_yield(void);
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
 * @brief Return why the chip last restarted, for the BOOT packet.
 *
 * @return A reset reason, or ON9LOG_RESET_UNKNOWN if the port cannot tell.
 */
on9log_reset_reason_t on9log_port_reset_reason(void);
/**
 * @brief Check whether the host can resolve a string from its address alone.
 *
 * Called for every tag and format pointer, from task and ISR context, so it
 * must be cheap and must not block, allocate, or read the string. Return true
 * only when @p addr lies in a part of the firmware image the host decoder
 * reads (read-only data, or the `.noload_keep_in_elf` section) and fits the
 * 32-bit wire ID. Return false otherwise, and the logger copies the string
 * into the packet instead (see ON9LOG_ID_INLINE).
 *
 * A bare-metal port typically compares @p addr against linker-script symbols
 * around its flash read-only data and its `.noload_keep_in_elf` output
 * section. The weak default returns true, which sends every address as is.
 *
 * @param[in] addr Tag or format pointer; never NULL.
 *
 * @return true if the address can be sent instead of the string.
 */
bool on9log_port_addr_in_image(const void *addr);
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
