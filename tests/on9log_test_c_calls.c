/*
 * Log calls made from C. The C macros classify arguments with _Generic, while
 * the same macros in C++ use templates, so both paths need their own calls.
 * The cases in on9log_test_cases.cpp call these and check the bytes.
 */

#include <inttypes.h>
#include <stdint.h>

#include "on9log.h"

void on9t_c_scalars(void)
{
    ON9_LOGI("c-tag", "%d %u %lld %llu %p", -5, 7u, (long long)INT64_MIN, (unsigned long long)UINT64_MAX, ON9_PTR(0x1234));
}

void on9t_c_strings(const char *text, const char *null_text)
{
    char array[] = "array";
    ON9_LOGI("c-tag", "%s|%s|%s|%s", text, "", null_text, array);
}

void on9t_c_precision(const char *text)
{
    ON9_LOGI("c-tag", "%.*s", 3, text);
}

void on9t_c_double(double d, float f)
{
    ON9_LOGI("c-tag", "%f %f %d", d, f, 7);
}

void on9t_c_long(long value, unsigned long uvalue)
{
    ON9_LOGI("c-tag", "%ld %lu", value, uvalue);
}

void on9t_c_sixteen_args(void)
{
    ON9_LOGI("c-tag", "%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d",
             1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16);
}

void on9t_c_all_levels(void)
{
    ON9_LOGE("c-tag", "error");
    ON9_LOGW("c-tag", "warn");
    ON9_LOGI("c-tag", "info");
    ON9_LOGD("c-tag", "debug");     /* above the default INFO ceiling: compiled out */
    ON9_LOGV("c-tag", "verbose");   /* compiled out */
}

void on9t_c_buffer(const void *data, size_t len)
{
    ON9_LOG_BUFW("c-tag", data, len);
}

void on9t_c_isr(uint32_t value)
{
    ON9_ISR_LOGW("c-isr", "isr %" PRIu32, value);
}
