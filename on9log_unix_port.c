/**
 * @file on9log_unix_port.c
 * @brief Linux/macOS platform hooks for the on9log core.
 */

#include "on9log_port.h"

#include <pthread.h>
#include <time.h>

static pthread_mutex_t s_on9log_lock = PTHREAD_MUTEX_INITIALIZER;

void on9log_port_lock(void)
{
    (void)pthread_mutex_lock(&s_on9log_lock);
}

void on9log_port_unlock(void)
{
    (void)pthread_mutex_unlock(&s_on9log_lock);
}

uint32_t on9log_port_timestamp_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }

    uint64_t milliseconds = (uint64_t)now.tv_sec * 1000u;
    milliseconds += (uint64_t)now.tv_nsec / 1000000u;
    return (uint32_t)milliseconds;
}

uint32_t on9log_port_isr_timestamp_ms(void)
{
    return on9log_port_timestamp_ms();
}

bool on9log_port_isr_ready(void)
{
    return false;
}

bool on9log_port_isr_enqueue_packet(const uint8_t *packet, size_t len)
{
    (void)packet;
    (void)len;
    return false;
}
