/**
 * @file on9log_esp_port.c
 * @brief ESP-IDF platform port for the on9log logging library.
 *
 * Provides the lock/timestamp primitives required by the on9log core using
 * the standard ESP-IDF log infrastructure.  These symbols override the weak
 * stubs in @ref on9log_port_weak.c when linked on ESP targets.
 */
#include "on9log_port.h"

#include "esp_log_timestamp.h"
#include "esp_private/log_lock.h"

/**
 * @brief Acquire the on9log global lock.
 *
 * Delegates to @c esp_log_impl_lock() which provides a spin-lock compatible
 * with both task and ISR contexts on ESP-IDF.
 *
 * @note Must be matched by a call to @ref on9log_port_unlock.
 */
void on9log_port_lock(void)
{
    esp_log_impl_lock();
}

/**
 * @brief Release the on9log global lock.
 *
 * Delegates to @c esp_log_impl_unlock().
 */
void on9log_port_unlock(void)
{
    esp_log_impl_unlock();
}

/**
 * @brief Return a monotonic system timestamp in milliseconds.
 *
 * Delegates to @c esp_log_timestamp() which returns the FreeRTOS tick count
 * converted to milliseconds.
 *
 * @return Timestamp in milliseconds (wraps around after ~49 days on 32-bit).
 */
uint32_t on9log_port_timestamp_ms(void)
{
    return esp_log_timestamp();
}
