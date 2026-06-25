#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <esp_err.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Maximum payload byte size for a single VFS transport frame. */
#define ESP_STDIO_LOG_VFS_FRAME_MAX_PAYLOAD 3072u
/** @brief Frame type tag for on9log binary packets. */
#define ESP_STDIO_LOG_VFS_FRAME_TYPE_ON9LOG 0x01u
/** @brief Frame type tag for plain-text log output. */
#define ESP_STDIO_LOG_VFS_FRAME_TYPE_TEXT 0x02u

/**
 * @brief Initialise the ESP stdio VFS log transport.
 *
 * Registers the output paths and prepares the VFS layer for receiving
 * on9log binary frames and text log output.
 *
 * @return ESP_OK on success, or an appropriate esp_err_t error code.
 */
esp_err_t esp_stdio_log_vfs_init(void);

/**
 * @brief Add an output path (file or device) for VFS log transport.
 *
 * @param[in] path Filesystem path to the output target (e.g. UART device).
 *
 * @return ESP_OK on success, or an appropriate esp_err_t error code.
 */
esp_err_t esp_stdio_log_vfs_add_output(const char *path);

/**
 * @brief Write one already-typed transport frame directly to all configured outputs.
 *
 * @param[in] type         Frame type tag (ESP_STDIO_LOG_VFS_FRAME_TYPE_ON9LOG or _TEXT).
 * @param[in] payload      Pointer to the frame payload bytes.
 * @param[in] payload_len  Byte length of the payload.
 *
 * @return ESP_OK on success, or an appropriate esp_err_t error code.
 */
/* Writes one already-typed transport frame directly to configured outputs. */
esp_err_t esp_stdio_log_vfs_write_frame(uint8_t type, const uint8_t *payload, size_t payload_len);

#ifdef __cplusplus
}
#endif
