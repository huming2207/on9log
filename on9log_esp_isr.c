#include "on9log_esp_isr.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_log_timestamp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "on9log.h"
#include "on9log_config.h"
#include "on9log_port.h"

#define ON9LOG_ESP_ISR_RINGBUF_MIN_SIZE ((ON9LOG_ISR_PACKET_MAX * 2u) + 16u)

typedef struct {
    RingbufHandle_t ringbuf;
    TaskHandle_t task;
    bool installed;
} on9log_esp_isr_t;

static on9log_esp_isr_t s_on9log_esp_isr = {
    .ringbuf = NULL,
    .task = NULL,
    .installed = false,
};

static void on9log_esp_isr_drain_task(void *ctx)
{
    (void)ctx;

    for (;;) {
        size_t packet_len = 0;
        uint8_t *packet = (uint8_t *)xRingbufferReceive(s_on9log_esp_isr.ringbuf, &packet_len, portMAX_DELAY);
        if (packet == NULL) {
            continue;
        }

        (void)on9log_dispatch_packet(packet, packet_len);
        vRingbufferReturnItem(s_on9log_esp_isr.ringbuf, packet);
    }
}

uint32_t on9log_port_isr_timestamp_ms(void)
{
    if (xPortInIsrContext()) {
        return (uint32_t)(xTaskGetTickCountFromISR() * portTICK_PERIOD_MS);
    }

    return esp_log_timestamp();
}

bool on9log_port_isr_enqueue_packet(const uint8_t *packet, size_t len)
{
    RingbufHandle_t ringbuf = s_on9log_esp_isr.ringbuf;
    if (ringbuf == NULL || (packet == NULL && len != 0)) {
        return false;
    }

    if (xPortInIsrContext()) {
        BaseType_t higher_priority_task_woken = pdFALSE;
        BaseType_t ok = xRingbufferSendFromISR(ringbuf, packet, len, &higher_priority_task_woken);
        if (higher_priority_task_woken == pdTRUE) {
            portYIELD_FROM_ISR();
        }
        return ok == pdTRUE;
    }

    return xRingbufferSend(ringbuf, packet, len, 0) == pdTRUE;
}

bool on9log_port_isr_ready(void)
{
    return s_on9log_esp_isr.ringbuf != NULL;
}

esp_err_t on9log_esp_isr_init(void)
{
    if (s_on9log_esp_isr.installed) {
        return ESP_OK;
    }
    if (CONFIG_ON9LOG_ESP_ISR_RINGBUF_SIZE < ON9LOG_ESP_ISR_RINGBUF_MIN_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    RingbufHandle_t ringbuf = xRingbufferCreate(CONFIG_ON9LOG_ESP_ISR_RINGBUF_SIZE, RINGBUF_TYPE_NOSPLIT);
    if (ringbuf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_on9log_esp_isr.ringbuf = ringbuf;
    BaseType_t task_ok = xTaskCreate(on9log_esp_isr_drain_task,
                                     "on9log_isr",
                                     CONFIG_ON9LOG_ESP_ISR_DRAIN_TASK_STACK_SIZE,
                                     NULL,
                                     CONFIG_ON9LOG_ESP_ISR_DRAIN_TASK_PRIORITY,
                                     &s_on9log_esp_isr.task);
    if (task_ok != pdPASS) {
        s_on9log_esp_isr.ringbuf = NULL;
        vRingbufferDelete(ringbuf);
        return ESP_ERR_NO_MEM;
    }

    s_on9log_esp_isr.installed = true;

    return ESP_OK;
}
