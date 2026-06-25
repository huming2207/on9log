#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the on9log ESP-IDF VFS (Virtual File System) transport.
 *
 * Registers the on9log binary output as an ESP VFS driver so that log packets
 * are written through the standard ESP-IDF VFS layer.
 *
 * @return ESP_OK on success, or an appropriate esp_err_t error code.
 */
esp_err_t on9log_esp_vfs_init(void);

#ifdef __cplusplus
}
#endif
