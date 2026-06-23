#include "on9log_esp_vfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_private/log_lock.h"
#include "esp_stdio_log_vfs.h"
#include "on9log.h"

typedef struct {
    bool installed;
    bool overflow;
    size_t len;
    uint8_t data[ESP_STDIO_LOG_VFS_FRAME_MAX_PAYLOAD];
} on9log_esp_vfs_t;

static on9log_esp_vfs_t s_on9log_esp_vfs = {
    .installed = false,
    .overflow = false,
    .len = 0,
};

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

static void on9log_esp_vfs_start(const uint8_t *header, size_t header_len, void *ctx)
{
    (void)ctx;

    esp_log_impl_lock();
    flockfile(stdout);
    fflush(stdout);
    s_on9log_esp_vfs.overflow = false;
    s_on9log_esp_vfs.len = 0;
    on9log_esp_vfs_append(header, header_len);
}

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

static void on9log_esp_vfs_end(void *ctx)
{
    (void)ctx;

    if (!s_on9log_esp_vfs.overflow) {
        (void)esp_stdio_log_vfs_write_frame(ESP_STDIO_LOG_VFS_FRAME_TYPE_ON9LOG,
                                            s_on9log_esp_vfs.data,
                                            s_on9log_esp_vfs.len);
    }
    funlockfile(stdout);
    esp_log_impl_unlock();
}

static const on9log_sink_t s_on9log_esp_vfs_sink = {
    .start_cb = on9log_esp_vfs_start,
    .payload_cb = on9log_esp_vfs_payload,
    .end_cb = on9log_esp_vfs_end,
};

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

esp_err_t on9log_esp_vfs_init(void)
{
    if (s_on9log_esp_vfs.installed) {
        return ESP_OK;
    }

    esp_err_t err = esp_stdio_log_vfs_init();
    if (err != ESP_OK) {
        return err;
    }

    on9log_err_t on9_err = on9log_add_sink(&s_on9log_esp_vfs_sink, &s_on9log_esp_vfs);
    if (on9_err != ON9LOG_OK) {
        return on9log_esp_vfs_to_esp_err(on9_err);
    }

    on9log_set_uart_enabled(false);
    s_on9log_esp_vfs.installed = true;
    return ESP_OK;
}
