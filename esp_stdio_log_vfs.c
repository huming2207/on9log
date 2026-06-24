#include "esp_stdio_log_vfs.h"

#include "sdkconfig.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log_write.h"
#include "esp_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define ESP_STDIO_LOG_VFS_PATH "/dev/logger"
#define STRINGIFY2(s) #s
#define STRINGIFY(s) STRINGIFY2(s)

#define ESP_STDIO_LOG_VFS_MAX_OUTPUTS 3

#define LOG_TAG "esp_stdio_log_vfs"

#define ESP_STDIO_LOG_VFS_SLIP_START 0xa5u
#define ESP_STDIO_LOG_VFS_SLIP_END 0xc0u
#define ESP_STDIO_LOG_VFS_SLIP_ESC 0xdbu
#define ESP_STDIO_LOG_VFS_SLIP_ESC_END 0xdcu
#define ESP_STDIO_LOG_VFS_SLIP_ESC_ESC 0xddu
#define ESP_STDIO_LOG_VFS_SLIP_ESC_START 0xdeu
#define ESP_STDIO_LOG_VFS_SLIP_ESC_CR 0xd0u
#define ESP_STDIO_LOG_VFS_SLIP_ESC_LF 0xd1u

#define ESP_STDIO_LOG_VFS_CRC16_CCITT_INIT 0xffffu

// Transport frame bytes are batched into this many bytes before calling
// write(). Sized to the ESP32 UART TX FIFO depth (128 bytes): a batch that fits
// in an empty FIFO returns without blocking on TX drain, and most on9log frames
// fit in a single batch, so a typical packet costs one write() instead of one
// per payload byte. Larger frames flush in 128-byte chunks; the SLIP start/end
// markers delimit frames on the wire, not write() call boundaries.
#define ESP_STDIO_LOG_VFS_WRITE_BATCH 128u

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
static SemaphoreHandle_t s_transport_mutex = NULL;
static StaticSemaphore_t s_transport_mutex_storage;
static portMUX_TYPE s_transport_mutex_init_lock = portMUX_INITIALIZER_UNLOCKED;

static SemaphoreHandle_t esp_stdio_log_vfs_get_transport_mutex(void)
{
    SemaphoreHandle_t mutex = s_transport_mutex;
    if (mutex != NULL) {
        return mutex;
    }

    taskENTER_CRITICAL(&s_transport_mutex_init_lock);
    if (s_transport_mutex == NULL) {
        s_transport_mutex = xSemaphoreCreateMutexStatic(&s_transport_mutex_storage);
    }
    mutex = s_transport_mutex;
    taskEXIT_CRITICAL(&s_transport_mutex_init_lock);

    return mutex;
}

static void esp_stdio_log_vfs_transport_lock(void)
{
    SemaphoreHandle_t mutex = esp_stdio_log_vfs_get_transport_mutex();
    if (mutex == NULL) {
        return;
    }
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) {
        return;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
}

static void esp_stdio_log_vfs_transport_unlock(void)
{
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) {
        return;
    }
    SemaphoreHandle_t mutex = s_transport_mutex;
    if (mutex != NULL) {
        xSemaphoreGive(mutex);
    }
}

static const uint16_t s_crc16_ccitt_table[256] = {
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50a5, 0x60c6, 0x70e7,
    0x8108, 0x9129, 0xa14a, 0xb16b, 0xc18c, 0xd1ad, 0xe1ce, 0xf1ef,
    0x1231, 0x0210, 0x3273, 0x2252, 0x52b5, 0x4294, 0x72f7, 0x62d6,
    0x9339, 0x8318, 0xb37b, 0xa35a, 0xd3bd, 0xc39c, 0xf3ff, 0xe3de,
    0x2462, 0x3443, 0x0420, 0x1401, 0x64e6, 0x74c7, 0x44a4, 0x5485,
    0xa56a, 0xb54b, 0x8528, 0x9509, 0xe5ee, 0xf5cf, 0xc5ac, 0xd58d,
    0x3653, 0x2672, 0x1611, 0x0630, 0x76d7, 0x66f6, 0x5695, 0x46b4,
    0xb75b, 0xa77a, 0x9719, 0x8738, 0xf7df, 0xe7fe, 0xd79d, 0xc7bc,
    0x48c4, 0x58e5, 0x6886, 0x78a7, 0x0840, 0x1861, 0x2802, 0x3823,
    0xc9cc, 0xd9ed, 0xe98e, 0xf9af, 0x8948, 0x9969, 0xa90a, 0xb92b,
    0x5af5, 0x4ad4, 0x7ab7, 0x6a96, 0x1a71, 0x0a50, 0x3a33, 0x2a12,
    0xdbfd, 0xcbdc, 0xfbbf, 0xeb9e, 0x9b79, 0x8b58, 0xbb3b, 0xab1a,
    0x6ca6, 0x7c87, 0x4ce4, 0x5cc5, 0x2c22, 0x3c03, 0x0c60, 0x1c41,
    0xedae, 0xfd8f, 0xcdec, 0xddcd, 0xad2a, 0xbd0b, 0x8d68, 0x9d49,
    0x7e97, 0x6eb6, 0x5ed5, 0x4ef4, 0x3e13, 0x2e32, 0x1e51, 0x0e70,
    0xff9f, 0xefbe, 0xdfdd, 0xcffc, 0xbf1b, 0xaf3a, 0x9f59, 0x8f78,
    0x9188, 0x81a9, 0xb1ca, 0xa1eb, 0xd10c, 0xc12d, 0xf14e, 0xe16f,
    0x1080, 0x00a1, 0x30c2, 0x20e3, 0x5004, 0x4025, 0x7046, 0x6067,
    0x83b9, 0x9398, 0xa3fb, 0xb3da, 0xc33d, 0xd31c, 0xe37f, 0xf35e,
    0x02b1, 0x1290, 0x22f3, 0x32d2, 0x4235, 0x5214, 0x6277, 0x7256,
    0xb5ea, 0xa5cb, 0x95a8, 0x8589, 0xf56e, 0xe54f, 0xd52c, 0xc50d,
    0x34e2, 0x24c3, 0x14a0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
    0xa7db, 0xb7fa, 0x8799, 0x97b8, 0xe75f, 0xf77e, 0xc71d, 0xd73c,
    0x26d3, 0x36f2, 0x0691, 0x16b0, 0x6657, 0x7676, 0x4615, 0x5634,
    0xd94c, 0xc96d, 0xf90e, 0xe92f, 0x99c8, 0x89e9, 0xb98a, 0xa9ab,
    0x5844, 0x4865, 0x7806, 0x6827, 0x18c0, 0x08e1, 0x3882, 0x28a3,
    0xcb7d, 0xdb5c, 0xeb3f, 0xfb1e, 0x8bf9, 0x9bd8, 0xabbb, 0xbb9a,
    0x4a75, 0x5a54, 0x6a37, 0x7a16, 0x0af1, 0x1ad0, 0x2ab3, 0x3a92,
    0xfd2e, 0xed0f, 0xdd6c, 0xcd4d, 0xbdaa, 0xad8b, 0x9de8, 0x8dc9,
    0x7c26, 0x6c07, 0x5c64, 0x4c45, 0x3ca2, 0x2c83, 0x1ce0, 0x0cc1,
    0xef1f, 0xff3e, 0xcf5d, 0xdf7c, 0xaf9b, 0xbfba, 0x8fd9, 0x9ff8,
    0x6e17, 0x7e36, 0x4e55, 0x5e74, 0x2e93, 0x3eb2, 0x0ed1, 0x1ef0,
};

static uint16_t esp_stdio_log_vfs_crc16_ccitt_update(uint16_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        crc = (uint16_t)((crc << 8u) ^ s_crc16_ccitt_table[((crc >> 8u) ^ data[i]) & 0xffu]);
    }

    return crc;
}

static bool esp_stdio_log_vfs_write_all(int fd, const uint8_t *data, size_t len)
{
    while (len != 0) {
        ssize_t written = write(fd, data, len);
        if (written <= 0) {
            return false;
        }
        data += (size_t)written;
        len -= (size_t)written;
    }

    return true;
}

typedef struct {
    uint8_t bytes[ESP_STDIO_LOG_VFS_WRITE_BATCH];
    size_t len;
    bool ok;
} esp_stdio_log_vfs_frame_writer_t;

static void esp_stdio_log_vfs_frame_writer_flush(esp_stdio_log_vfs_frame_writer_t *writer)
{
    if (writer->len == 0) {
        return;
    }

    for (size_t i = 0; i < s_esp_stdio_log_vfs.output_count; ++i) {
        if (s_esp_stdio_log_vfs.outputs[i] >= 0 &&
            !esp_stdio_log_vfs_write_all(s_esp_stdio_log_vfs.outputs[i], writer->bytes, writer->len)) {
            writer->ok = false;
        }
    }

    writer->len = 0;
}

static void esp_stdio_log_vfs_frame_writer_put_raw(esp_stdio_log_vfs_frame_writer_t *writer,
                                                   const uint8_t *data,
                                                   size_t len)
{
    while (len != 0) {
        if (writer->len == sizeof(writer->bytes)) {
            esp_stdio_log_vfs_frame_writer_flush(writer);
        }

        size_t available = sizeof(writer->bytes) - writer->len;
        size_t chunk_len = len < available ? len : available;
        memcpy(&writer->bytes[writer->len], data, chunk_len);
        writer->len += chunk_len;
        data += chunk_len;
        len -= chunk_len;
    }
}

static void esp_stdio_log_vfs_frame_writer_put_raw_byte(esp_stdio_log_vfs_frame_writer_t *writer,
                                                        uint8_t byte)
{
    esp_stdio_log_vfs_frame_writer_put_raw(writer, &byte, sizeof(byte));
}

static void esp_stdio_log_vfs_frame_writer_put_slip_byte(esp_stdio_log_vfs_frame_writer_t *writer,
                                                         uint8_t byte)
{
    if (byte == ESP_STDIO_LOG_VFS_SLIP_START) {
        const uint8_t escaped[] = {ESP_STDIO_LOG_VFS_SLIP_ESC, ESP_STDIO_LOG_VFS_SLIP_ESC_START};
        esp_stdio_log_vfs_frame_writer_put_raw(writer, escaped, sizeof(escaped));
        return;
    }
    if (byte == ESP_STDIO_LOG_VFS_SLIP_END) {
        const uint8_t escaped[] = {ESP_STDIO_LOG_VFS_SLIP_ESC, ESP_STDIO_LOG_VFS_SLIP_ESC_END};
        esp_stdio_log_vfs_frame_writer_put_raw(writer, escaped, sizeof(escaped));
        return;
    }
    if (byte == ESP_STDIO_LOG_VFS_SLIP_ESC) {
        const uint8_t escaped[] = {ESP_STDIO_LOG_VFS_SLIP_ESC, ESP_STDIO_LOG_VFS_SLIP_ESC_ESC};
        esp_stdio_log_vfs_frame_writer_put_raw(writer, escaped, sizeof(escaped));
        return;
    }
    if (byte == (uint8_t)'\r') {
        const uint8_t escaped[] = {ESP_STDIO_LOG_VFS_SLIP_ESC, ESP_STDIO_LOG_VFS_SLIP_ESC_CR};
        esp_stdio_log_vfs_frame_writer_put_raw(writer, escaped, sizeof(escaped));
        return;
    }
    if (byte == (uint8_t)'\n') {
        const uint8_t escaped[] = {ESP_STDIO_LOG_VFS_SLIP_ESC, ESP_STDIO_LOG_VFS_SLIP_ESC_LF};
        esp_stdio_log_vfs_frame_writer_put_raw(writer, escaped, sizeof(escaped));
        return;
    }

    esp_stdio_log_vfs_frame_writer_put_raw_byte(writer, byte);
}

static void esp_stdio_log_vfs_frame_writer_put_slip(esp_stdio_log_vfs_frame_writer_t *writer,
                                                    const uint8_t *data,
                                                    size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        esp_stdio_log_vfs_frame_writer_put_slip_byte(writer, data[i]);
    }
}

esp_err_t esp_stdio_log_vfs_write_frame(uint8_t type, const uint8_t *payload, size_t payload_len)
{
    if (payload == NULL && payload_len != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (payload_len > ESP_STDIO_LOG_VFS_FRAME_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint16_t crc = ESP_STDIO_LOG_VFS_CRC16_CCITT_INIT;
    crc = esp_stdio_log_vfs_crc16_ccitt_update(crc, &type, sizeof(type));
    crc = esp_stdio_log_vfs_crc16_ccitt_update(crc, payload, payload_len);

    esp_stdio_log_vfs_frame_writer_t writer = {
        .bytes = {0},
        .len = 0,
        .ok = true,
    };

    esp_stdio_log_vfs_transport_lock();
    esp_stdio_log_vfs_frame_writer_put_raw_byte(&writer, ESP_STDIO_LOG_VFS_SLIP_START);
    esp_stdio_log_vfs_frame_writer_put_slip_byte(&writer, type);
    esp_stdio_log_vfs_frame_writer_put_slip(&writer, payload, payload_len);
    esp_stdio_log_vfs_frame_writer_put_slip_byte(&writer, (uint8_t)(crc & 0xffu));
    esp_stdio_log_vfs_frame_writer_put_slip_byte(&writer, (uint8_t)(crc >> 8u));
    esp_stdio_log_vfs_frame_writer_put_raw_byte(&writer, ESP_STDIO_LOG_VFS_SLIP_END);
    esp_stdio_log_vfs_frame_writer_flush(&writer);
    esp_stdio_log_vfs_transport_unlock();

    return writer.ok ? ESP_OK : ESP_FAIL;
}

esp_err_t esp_stdio_log_vfs_add_output(const char *path)
{
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_stdio_log_vfs_transport_lock();

    if (s_esp_stdio_log_vfs.output_count >= ESP_STDIO_LOG_VFS_MAX_OUTPUTS) {
        esp_stdio_log_vfs_transport_unlock();
        return ESP_ERR_NO_MEM;
    }

    int fd = open(path, O_WRONLY);
    if (fd >= 0) {
        s_esp_stdio_log_vfs.outputs[s_esp_stdio_log_vfs.output_count++] = fd;
    }

    esp_stdio_log_vfs_transport_unlock();

    return ESP_OK;
}

static ssize_t esp_stdio_log_vfs_write(void *ctx, int fd, const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    esp_err_t err = ESP_OK;
    (void)fd;
    (void)ctx;

    if (bytes == NULL && size != 0) {
        return -1;
    }

    size_t remaining = size;
    while (remaining != 0) {
        size_t chunk_len = remaining;
        if (chunk_len > ESP_STDIO_LOG_VFS_FRAME_MAX_PAYLOAD) {
            chunk_len = ESP_STDIO_LOG_VFS_FRAME_MAX_PAYLOAD;
        }
        err = esp_stdio_log_vfs_write_frame(ESP_STDIO_LOG_VFS_FRAME_TYPE_TEXT, bytes, chunk_len);
        if (err != ESP_OK) {
            break;
        }
        bytes += chunk_len;
        remaining -= chunk_len;
    }

    if (err != ESP_OK) {
        return -1;
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

    esp_stdio_log_vfs_transport_lock();
    for (size_t i = 0; i < sink->output_count; ++i) {
        if (sink->outputs[i] >= 0 && fsync(sink->outputs[i]) != 0) {
            result = -1;
        }
    }
    esp_stdio_log_vfs_transport_unlock();

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
