/**
 * @file on9log_esp_isr.c
 * @brief ESP-IDF ISR ringbuffer implementation for on9log.
 *
 * Provides an ISR-safe logging path: log packets are serialised in the ISR
 * context, enqueued into a FreeRTOS ringbuffer, and drained by a dedicated
 * FreeRTOS task that dispatches them through the normal sink pipeline.
 */

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

/** @brief Minimum ringbuffer size in bytes (two max-size packets plus safety margin). */
#define ON9LOG_ESP_ISR_RINGBUF_MIN_SIZE ((ON9LOG_ISR_PACKET_MAX * 2u) + 16u)

/**
 * @brief Internal state for the ESP-IDF ISR logging module.
 *
 * Holds the ringbuffer handle, the drain task handle, and an installed flag.
 */
typedef struct {
    RingbufHandle_t ringbuf; /**< FreeRTOS ringbuffer for ISR-to-task packet transfer. */
    TaskHandle_t task;       /**< Handle of the drain FreeRTOS task. */
    bool installed;          /**< @c true once @ref on9log_esp_isr_init completes. */
} on9log_esp_isr_t;

/** @brief Singleton state for the ISR logging subsystem. */
static on9log_esp_isr_t s_on9log_esp_isr = {
    .ringbuf = NULL,
    .task = NULL,
    .installed = false,
};

/**
 * @brief Drain task: receives packets from the ringbuffer and dispatches them.
 *
 * This task runs in an infinite loop, blocking on @c xRingbufferReceive with
 * @c portMAX_DELAY.  Every received packet is forwarded to
 * @ref on9log_dispatch_packet and the ringbuffer item is then released.
 *
 * @param[in] ctx  Unused context pointer (always NULL).
 */
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

/**
 * @brief Return a timestamp in milliseconds from an ISR-safe source.
 *
 * When called inside an ISR context (detected via @c xPortInIsrContext) the
 * function reads the FreeRTOS tick count via @c xTaskGetTickCountFromISR.
 * Otherwise it falls back to @c esp_log_timestamp().
 *
 * @return Timestamp in milliseconds (wraps around after ~49 days on 32-bit).
 */
uint32_t on9log_port_isr_timestamp_ms(void)
{
    if (xPortInIsrContext()) {
        return (uint32_t)(xTaskGetTickCountFromISR() * portTICK_PERIOD_MS);
    }

    return esp_log_timestamp();
}

/**
 * @brief Enqueue a serialised log packet into the ISR ringbuffer.
 *
 * Handles both ISR-context and task-context calls, routing to the appropriate
 * FreeRTOS ringbuffer API (@c xRingbufferSendFromISR vs @c xRingbufferSend).
 *
 * @param[in] packet  Pointer to the serialised log packet.
 * @param[in] len     Length of the packet in bytes.
 *
 * @return @c true on success, @c false if the ringbuffer is full or not yet
 *         initialised.
 */
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

/**
 * @brief Check whether the ISR logging ringbuffer is ready.
 *
 * @return @c true if the ringbuffer has been allocated by @ref on9log_esp_isr_init.
 */
bool on9log_port_isr_ready(void)
{
    return s_on9log_esp_isr.ringbuf != NULL;
}

/**
 * @brief Initialise the ESP-IDF ISR logging subsystem.
 *
 * Allocates the ringbuffer, creates the drain FreeRTOS task, and marks the
 * subsystem as installed.  Safe to call multiple times; subsequent calls
 * return @c ESP_OK without re-initialising.
 *
 * @return
 * - @c ESP_OK on success or if already initialised.
 * - @c ESP_ERR_INVALID_SIZE if @c CONFIG_ON9LOG_ESP_ISR_RINGBUF_SIZE is too small.
 * - @c ESP_ERR_NO_MEM if ringbuffer or task creation fails.
 */
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
