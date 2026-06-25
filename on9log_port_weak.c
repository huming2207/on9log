/**
 * @file on9log_port_weak.c
 * @brief Weak-linkage default stubs for the on9log platform porting layer.
 *
 * These symbols provide a safe fallback when the target platform does not
 * supply its own implementations.  On ESP-IDF builds (@c ESP_PLATFORM defined)
 * the ESP-specific port file overrides the lock/timestamp stubs; the write,
 * ISR-timestamp, ISR-ready, and ISR-enqueue stubs remain weak so that the
 * ESP port layer can supply them as well.
 */

#include "on9log_port.h"

#include <stdbool.h>

/** @brief Helper macro to attach the GNU weak attribute to a symbol. */
#if defined(__GNUC__)
#define ON9LOG_WEAK __attribute__((weak))
#else
#define ON9LOG_WEAK
#endif

/**
 * @brief Weak default lock acquisition.
 *
 * Non-ESP platforms that need locking must provide a real implementation.
 */
#ifndef ESP_PLATFORM
ON9LOG_WEAK void on9log_port_lock(void) {}

/**
 * @brief Weak default lock release.
 *
 * Non-ESP platforms that need locking must provide a real implementation.
 */
ON9LOG_WEAK void on9log_port_unlock(void) {}

/**
 * @brief Weak default monotonic timestamp in milliseconds.
 *
 * @return Always 0 on non-ESP platforms.
 */
ON9LOG_WEAK uint32_t on9log_port_timestamp_ms(void)
{
    return 0;
}
#endif

/**
 * @brief Weak default UART / serial byte output.
 *
 * @param[in] data  Pointer to the byte buffer to write.
 * @param[in] len   Number of bytes to write.
 *
 * @note The default is a no-op.  Platforms that enable UART output must
 *       override this symbol.
 */
ON9LOG_WEAK void on9log_port_write(const uint8_t *data, size_t len)
{
    (void)data;
    (void)len;
}

/**
 * @brief Weak default ISR-context timestamp in milliseconds.
 *
 * @return The value of @ref on9log_port_timestamp_ms() (the non-ISR variant).
 */
ON9LOG_WEAK uint32_t on9log_port_isr_timestamp_ms(void)
{
    return on9log_port_timestamp_ms();
}

/**
 * @brief Weak default ISR readiness check.
 *
 * @return @c false (ISR logging not available by default).
 */
ON9LOG_WEAK bool on9log_port_isr_ready(void)
{
    return false;
}

/**
 * @brief Weak default ISR-context packet enqueue.
 *
 * @param[in] packet  Pointer to the serialised log packet.
 * @param[in] len     Length of the packet in bytes.
 *
 * @return @c false (enqueue always fails; no ringbuffer allocated).
 */
ON9LOG_WEAK bool on9log_port_isr_enqueue_packet(const uint8_t *packet, size_t len)
{
    (void)packet;
    (void)len;
    return false;
}
