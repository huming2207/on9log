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
 * @brief Report an ID of the running firmware image for the BOOT packet.
 *
 * Called from task context when the BOOT packet is built. The host compares
 * it with the same value computed from the ELF it decodes with, to catch a
 * mismatched ELF.
 *
 * @param[out] id   Receives the ID bytes.
 * @param[in]  cap  Size of @p id (ON9LOG_IMAGE_ID_MAX_LEN).
 * @param[out] kind Receives how the ID was produced.
 *
 * @return ID length in bytes, or 0 if the port has none (the weak default).
 */
size_t on9log_port_image_id(uint8_t *id, size_t cap, on9log_image_id_kind_t *kind);
/**
 * @brief Find the GNU build-id in a block of ELF notes.
 *
 * A helper for ports that report ON9LOG_IMAGE_ID_GNU_BUILD_ID: pass the
 * running image's note segment or `.note.gnu.build-id` section, for example
 * bounded by linker-script symbols on a bare-metal target.
 *
 * @param[in]  notes Start of the notes, in the target's byte order.
 * @param[in]  len   Bytes of notes.
 * @param[in]  align Note alignment: the segment's p_align, 4 or 8.
 * @param[out] id    Receives the build-id bytes.
 * @param[in]  cap   Size of @p id.
 *
 * @return Build-id length, or 0 if none was found or it does not fit.
 */
size_t on9log_find_gnu_build_id(const void *notes, size_t len, size_t align, uint8_t *id, size_t cap);
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
