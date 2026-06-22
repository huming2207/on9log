#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <esp_err.h>

esp_err_t esp_stdio_log_vfs_init(void);
esp_err_t esp_stdio_log_vfs_add_output(const char *path);

#ifdef __cplusplus
}
#endif
