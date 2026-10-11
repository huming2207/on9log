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
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/**
 * @brief Acquire the on9log global lock.
 *
 * Delegates to @c esp_log_impl_lock(), a non-recursive FreeRTOS mutex.
 * Task-context only; must not be called from ISR.
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

void on9log_port_yield(void)
{
    taskYIELD();
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

on9log_reset_reason_t on9log_port_reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:
        return ON9LOG_RESET_POWER_ON;
    case ESP_RST_EXT:
        return ON9LOG_RESET_EXTERNAL;
    case ESP_RST_SW:
        return ON9LOG_RESET_SOFTWARE;
    case ESP_RST_PANIC:
        return ON9LOG_RESET_PANIC;
    case ESP_RST_INT_WDT:
        return ON9LOG_RESET_INT_WDT;
    case ESP_RST_TASK_WDT:
        return ON9LOG_RESET_TASK_WDT;
    case ESP_RST_WDT:
        return ON9LOG_RESET_OTHER_WDT;
    case ESP_RST_DEEPSLEEP:
        return ON9LOG_RESET_DEEP_SLEEP;
    case ESP_RST_BROWNOUT:
        return ON9LOG_RESET_BROWNOUT;
    case ESP_RST_SDIO:
        return ON9LOG_RESET_SDIO;
    case ESP_RST_USB:
        return ON9LOG_RESET_USB;
    case ESP_RST_JTAG:
        return ON9LOG_RESET_JTAG;
    case ESP_RST_EFUSE:
        return ON9LOG_RESET_EFUSE;
    case ESP_RST_PWR_GLITCH:
        return ON9LOG_RESET_POWER_GLITCH;
    case ESP_RST_CPU_LOCKUP:
        return ON9LOG_RESET_CPU_LOCKUP;
    default:
        return ON9LOG_RESET_UNKNOWN;
    }
}
