#pragma once

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the ESP-IDF ringbuffer and drain task for ISR logging.
 *
 * The drain task forwards queued ISR packets through the normal on9log sinks.
 * Call this after registering the sinks/transports that should receive those
 * packets, for example after on9log_esp_vfs_init().
 *
 * @return ESP_OK on success, or an appropriate esp_err_t error code.
 */
/*
 * Initializes the ESP-IDF ringbuffer and drain task used by ON9_ISR_LOGx().
 * The drain task forwards queued ISR packets through the normal on9log sinks.
 * Call this after registering the sinks/transports that should receive those
 * packets, for example after on9log_esp_vfs_init().
 */
esp_err_t on9log_esp_isr_init(void);

#ifdef __cplusplus
}
#endif
