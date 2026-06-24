#include "on9log_port.h"

#include <stdbool.h>

#if defined(__GNUC__)
#define ON9LOG_WEAK __attribute__((weak))
#else
#define ON9LOG_WEAK
#endif

#ifndef ESP_PLATFORM
ON9LOG_WEAK void on9log_port_lock(void) {}
ON9LOG_WEAK void on9log_port_unlock(void) {}
ON9LOG_WEAK uint32_t on9log_port_timestamp_ms(void)
{
    return 0;
}
#endif
ON9LOG_WEAK void on9log_port_write(const uint8_t *data, size_t len)
{
    (void)data;
    (void)len;
}

ON9LOG_WEAK uint32_t on9log_port_isr_timestamp_ms(void)
{
    return on9log_port_timestamp_ms();
}

ON9LOG_WEAK bool on9log_port_isr_ready(void)
{
    return false;
}

ON9LOG_WEAK bool on9log_port_isr_enqueue_packet(const uint8_t *packet, size_t len)
{
    (void)packet;
    (void)len;
    return false;
}
