#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <esp_err.h>
#include <stddef.h>
#include <stdint.h>

#define ESP_STDIO_LOG_VFS_FRAME_MAX_PAYLOAD 3072u
#define ESP_STDIO_LOG_VFS_FRAME_TYPE_ON9LOG 0x01u
#define ESP_STDIO_LOG_VFS_FRAME_TYPE_TEXT 0x02u

esp_err_t esp_stdio_log_vfs_init(void);
esp_err_t esp_stdio_log_vfs_add_output(const char *path);
/* Writes one already-typed transport frame directly to configured outputs. */
esp_err_t esp_stdio_log_vfs_write_frame(uint8_t type, const uint8_t *payload, size_t payload_len);

#ifdef __cplusplus
}
#endif
