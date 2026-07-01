#pragma once

/**
 * @file on9log_unix_stdio.h
 * @brief Linux/macOS stdio sink for on9log.
 */

#include <stdio.h>

#include "on9log.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the Unix sink and write logs to stdout.
 *
 * In binary mode each packet is emitted as the same typed SLIP/CRC frame used
 * by the ESP stdio transport. In plain-text mode complete formatted lines are
 * written directly to stdout.
 */
on9log_err_t on9log_unix_stdio_init(void);

/**
 * @brief Initialize the Unix sink with a caller-selected stdio stream.
 *
 * The stream must remain open for the process lifetime. Initialization is
 * idempotent when called repeatedly with the same stream.
 */
on9log_err_t on9log_unix_stdio_init_file(FILE *stream);

#ifdef __cplusplus
}
#endif
