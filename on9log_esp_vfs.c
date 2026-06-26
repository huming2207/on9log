/**
 * @file on9log_esp_vfs.c
 * @brief ESP VFS sink implementation for on9log.
 *
 * Implements an @ref on9log_sink_t that collects log-frame payloads into a
 * local buffer and writes a complete SLIP-encoded frame via the
 * @ref esp_stdio_log_vfs transport layer when the sink's @c end_cb is called.
 */

#include "on9log_esp_vfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_stdio_log_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "on9log.h"

/**
 * @brief Internal state for the on9log ESP VFS sink.
 *
 * Accumulates bytes during one log-frame cycle (header + payloads) and then
 * writes the assembled frame through the transport layer.
 */
typedef struct {
    bool installed;                                           /**< @c true once @ref on9log_esp_vfs_init completes. */
    bool overflow;                                            /**< @c true when the local buffer has overrun this frame. */
    size_t len;                                               /**< Number of valid bytes currently in @ref data. */
    uint8_t data[ESP_STDIO_LOG_VFS_FRAME_MAX_PAYLOAD];        /**< Accumulation buffer for the current frame. */
} on9log_esp_vfs_t;

/** @brief Singleton VFS sink state. */
static on9log_esp_vfs_t s_on9log_esp_vfs = {
    .installed = false,
    .overflow = false,
    .len = 0,
};

/** @brief Dedicated sink mutex protecting the singleton accumulator buffer.
 *
 * Replaces esp_log_impl_lock() so that on9log packet accumulation no longer
 * blocks ESP-IDF system logging (ESP_LOG*) on other tasks/cores during a
 * frame write.  Non-recursive; callers must not log from within sink
 * callbacks.
 */
static SemaphoreHandle_t s_sink_mutex = NULL;
/** @brief Static storage for the sink mutex (avoids dynamic allocation). */
static StaticSemaphore_t s_sink_mutex_storage;

/**
 * @brief Append bytes to the frame accumulation buffer.
 *
 * Silently discards data if the buffer overflows and sets the overflow flag.
 *
 * @param[in] data  Pointer to the bytes to append.
 * @param[in] len   Number of bytes to append.
 */
static void on9log_esp_vfs_append(const uint8_t *data, size_t len)
{
    if (s_on9log_esp_vfs.overflow) {
        return;
    }
    if (len > sizeof(s_on9log_esp_vfs.data) - s_on9log_esp_vfs.len) {
        s_on9log_esp_vfs.overflow = true;
        return;
    }
    for (size_t i = 0; i < len; ++i) {
        s_on9log_esp_vfs.data[s_on9log_esp_vfs.len++] = data[i];
    }
}

/**
 * @brief Sink @c start_cb: begin a new log frame.
 *
 * Acquires the dedicated sink mutex (serialising access to the singleton
 * accumulator buffer), resets the accumulation buffer, and appends the
 * packet header.
 *
 * @param[in] header      Pointer to the on9log packet header bytes.
 * @param[in] header_len  Length of the header in bytes.
 * @param[in] ctx         Sink context (unused).
 */
static void on9log_esp_vfs_start(const uint8_t *header, size_t header_len, void *ctx)
{
    (void)ctx;

    if (s_sink_mutex != NULL) {
        xSemaphoreTake(s_sink_mutex, portMAX_DELAY);
    }
    s_on9log_esp_vfs.overflow = false;
    s_on9log_esp_vfs.len = 0;
    on9log_esp_vfs_append(header, header_len);
}

/**
 * @brief Sink @c payload_cb: accumulate a payload chunk.
 *
 * @param[in] payload         Pointer to the payload bytes.
 * @param[in] payload_len     Number of payload bytes.
 * @param[in] total_arg_cnt   Total arguments in the frame (unused).
 * @param[in] curr_arg_index  Index of the current argument (unused).
 * @param[in] ctx             Sink context (unused).
 */
static void on9log_esp_vfs_payload(const uint8_t *payload,
                                   size_t payload_len,
                                   size_t total_arg_cnt,
                                   size_t curr_arg_index,
                                   void *ctx)
{
    (void)total_arg_cnt;
    (void)curr_arg_index;
    (void)ctx;

    on9log_esp_vfs_append(payload, payload_len);
}

/**
 * @brief Sink @c end_cb: write the accumulated frame and release the sink mutex.
 *
 * If no overflow occurred, the assembled buffer is written as an on9log frame
 * via @ref esp_stdio_log_vfs_write_frame.  The sink mutex is released after
 * the transport write completes so that the accumulator buffer is not
 * overwritten by a concurrent packet.
 *
 * @param[in] ctx  Sink context (unused).
 */
static void on9log_esp_vfs_end(void *ctx)
{
    (void)ctx;

    if (!s_on9log_esp_vfs.overflow) {
        (void)esp_stdio_log_vfs_write_frame(ESP_STDIO_LOG_VFS_FRAME_TYPE_ON9LOG,
                                            s_on9log_esp_vfs.data,
                                            s_on9log_esp_vfs.len);
    }
    if (s_sink_mutex != NULL) {
        xSemaphoreGive(s_sink_mutex);
    }
}

/** @brief Static sink descriptor for the ESP VFS output path. */
static const on9log_sink_t s_on9log_esp_vfs_sink = {
    .start_cb = on9log_esp_vfs_start,
    .payload_cb = on9log_esp_vfs_payload,
    .end_cb = on9log_esp_vfs_end,
};

/**
 * @brief Convert an on9log error code to the equivalent ESP-IDF error code.
 *
 * @param[in] err  An on9log error code.
 *
 * @return The corresponding @c esp_err_t value.
 */
static esp_err_t on9log_esp_vfs_to_esp_err(on9log_err_t err)
{
    switch (err) {
    case ON9LOG_OK:
        return ESP_OK;
    case ON9LOG_ERR_INVALID_ARG:
        return ESP_ERR_INVALID_ARG;
    case ON9LOG_ERR_NO_MEM:
        return ESP_ERR_NO_MEM;
    case ON9LOG_ERR_NOT_FOUND:
        return ESP_ERR_NOT_FOUND;
    case ON9LOG_ERR_INVALID_SIZE:
        return ESP_ERR_INVALID_SIZE;
    case ON9LOG_ERR_FAIL:
    default:
        return ESP_FAIL;
    }
}

/**
 * @brief Initialise the on9log ESP VFS sink.
 *
 * Initialises the underlying @ref esp_stdio_log_vfs transport, registers the
 * VFS sink with the on9log core.  Safe to call multiple times; subsequent
 * calls return @c ESP_OK without re-initialising.
 *
 * @return
 * - @c ESP_OK on success or if already initialised.
 * - @c ESP_ERR_INVALID_ARG if the sink registration fails on invalid arguments.
 * - @c ESP_ERR_NO_MEM if the sink table is full.
 * - Other @c esp_err_t propagated from @ref esp_stdio_log_vfs_init.
 */
esp_err_t on9log_esp_vfs_init(void)
{
    if (s_on9log_esp_vfs.installed) {
        return ESP_OK;
    }

    if (s_sink_mutex == NULL) {
        s_sink_mutex = xSemaphoreCreateMutexStatic(&s_sink_mutex_storage);
        if (s_sink_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t err = esp_stdio_log_vfs_init();
    if (err != ESP_OK) {
        return err;
    }

    on9log_err_t on9_err = on9log_add_sink(&s_on9log_esp_vfs_sink, &s_on9log_esp_vfs);
    if (on9_err != ON9LOG_OK) {
        return on9log_esp_vfs_to_esp_err(on9_err);
    }

    s_on9log_esp_vfs.installed = true;
    return ESP_OK;
}
