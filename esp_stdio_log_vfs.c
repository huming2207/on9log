#include "esp_stdio_log_vfs.h"

#include "sdkconfig.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "on9log_fmt.h"

#include "esp_log_write.h"
#include "esp_vfs.h"

#define ESP_STDIO_LOG_VFS_PATH "/dev/logger"
#define STRINGIFY2(s) #s
#define STRINGIFY(s) STRINGIFY2(s)

#define ESP_STDIO_LOG_VFS_MAX_OUTPUTS 3

#define LOG_TAG "esp_stdio_log_vfs"

typedef struct {
    int outputs[ESP_STDIO_LOG_VFS_MAX_OUTPUTS];
    size_t output_count;
    bool installed;
} esp_stdio_log_vfs_t;

static esp_stdio_log_vfs_t s_esp_stdio_log_vfs = {
    .outputs = {-1, -1, -1},
    .output_count = 0,
    .installed = false,
};

esp_err_t esp_stdio_log_vfs_add_output(const char *path)
{
    if (s_esp_stdio_log_vfs.output_count >= ESP_STDIO_LOG_VFS_MAX_OUTPUTS) {
        return ESP_ERR_NO_MEM;
    }

    int fd = open(path, O_WRONLY);
    if (fd >= 0) {
        s_esp_stdio_log_vfs.outputs[s_esp_stdio_log_vfs.output_count++] = fd;
    }

    return ESP_OK;
}

static ssize_t esp_stdio_log_vfs_write(void *ctx, int fd, const void *data, size_t size)
{
    (void)fd;

    esp_stdio_log_vfs_t *sink = (esp_stdio_log_vfs_t *)ctx;

    for (size_t i = 0; i < sink->output_count; ++i) {
        if (sink->outputs[i] >= 0) {
            (void)write(sink->outputs[i], data, size);
        }
    }

    return (ssize_t)size;
}

static int esp_stdio_log_vfs_open(void *ctx, const char *path, int flags, int mode)
{
    (void)ctx;
    (void)path;
    (void)flags;
    (void)mode;

    return 0;
}

static int esp_stdio_log_vfs_close(void *ctx, int fd)
{
    (void)ctx;
    (void)fd;

    return 0;
}

static int esp_stdio_log_vfs_fstat(void *ctx, int fd, struct stat *st)
{
    (void)ctx;
    (void)fd;

    st->st_mode = S_IFCHR;
    return 0;
}

static int esp_stdio_log_vfs_fsync(void *ctx, int fd)
{
    (void)fd;

    esp_stdio_log_vfs_t *sink = (esp_stdio_log_vfs_t *)ctx;
    int result = 0;

    for (size_t i = 0; i < sink->output_count; ++i) {
        if (sink->outputs[i] >= 0 && fsync(sink->outputs[i]) != 0) {
            result = -1;
        }
    }

    return result;
}

static int esp_stdio_log_vfs_vprintf(const char *fmt, va_list args)
{
    return vfprintf(stdout, fmt, args);
}

esp_err_t esp_stdio_log_vfs_init(void)
{
    if (s_esp_stdio_log_vfs.installed) {
        return ESP_OK;
    }

#if CONFIG_ESP_CONSOLE_UART
    esp_stdio_log_vfs_add_output("/dev/uart/" STRINGIFY(CONFIG_ESP_CONSOLE_UART_NUM));
#endif

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG || CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG
    esp_stdio_log_vfs_add_output("/dev/usbserjtag");
#endif

#if CONFIG_ESP_CONSOLE_USB_CDC
    esp_stdio_log_vfs_add_output("/dev/cdcacm");
#endif

    static const esp_vfs_t vfs = {
        .flags = ESP_VFS_FLAG_CONTEXT_PTR,
        .write_p = esp_stdio_log_vfs_write,
        .open_p = esp_stdio_log_vfs_open,
        .close_p = esp_stdio_log_vfs_close,
        .fstat_p = esp_stdio_log_vfs_fstat,
        .fsync_p = esp_stdio_log_vfs_fsync,
    };

    esp_err_t err = esp_vfs_register(ESP_STDIO_LOG_VFS_PATH, &vfs, &s_esp_stdio_log_vfs);
    if (err != ESP_OK) {
        return err;
    }

    fflush(stdout);
    fflush(stderr);

    if (freopen(ESP_STDIO_LOG_VFS_PATH, "w", stdout) == NULL) {
        return ESP_FAIL;
    }

    if (freopen(ESP_STDIO_LOG_VFS_PATH, "w", stderr) == NULL) {
        return ESP_FAIL;
    }

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    esp_log_set_vprintf(esp_stdio_log_vfs_vprintf);
    s_esp_stdio_log_vfs.installed = true;

    return ESP_OK;
}
