#include "on9log_port.h"

#include "esp_log_timestamp.h"
#include "esp_private/log_lock.h"

void on9log_port_lock(void)
{
    esp_log_impl_lock();
}

void on9log_port_unlock(void)
{
    esp_log_impl_unlock();
}

uint32_t on9log_port_timestamp_ms(void)
{
    return esp_log_timestamp();
}
