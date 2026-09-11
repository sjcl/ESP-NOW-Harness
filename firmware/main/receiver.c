#include "receiver.h"
#include "harness.h"
#include "metrics.h"
#include "protocol.h"
#include "radio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <string.h>

#define RECEIVER_QUEUE_LENGTH 64

typedef enum { EV_RX, EV_TX_OK, EV_TX_FAIL, EV_DROP, EV_BARRIER } event_type_t;
typedef struct {
    event_type_t type;
    uint16_t len;
    int8_t rssi;
    uint64_t at;
    TaskHandle_t waiter;
    uint8_t data[HARNESS_MAX_PACKET_SIZE];
} event_t;

static QueueHandle_t queue;
static StaticQueue_t queue_storage;
static uint8_t queue_bytes[RECEIVER_QUEUE_LENGTH * sizeof(event_t)];

void receiver_post_rx(const uint8_t *data, size_t length, int8_t rssi, uint64_t at_us)
{
    event_t event = {.type = EV_RX, .len = (uint16_t)length, .rssi = rssi, .at = at_us};
    memcpy(event.data, data, length);
    if (xQueueSend(queue, &event, 0) != pdTRUE) {
        __atomic_fetch_add(&g_metrics.totals.queue_drops, 1, __ATOMIC_RELAXED);
    }
}

void receiver_post_tx(bool success)
{
    event_t event = {.type = success ? EV_TX_OK : EV_TX_FAIL};
    if (xQueueSend(queue, &event, 0) != pdTRUE) {
        __atomic_fetch_add(&g_metrics.totals.queue_drops, 1, __ATOMIC_RELAXED);
    }
}

void receiver_post_drop(bool stale, bool invalid)
{
    (void)stale;
    (void)invalid;
    event_t event = {.type = EV_DROP};
    if (queue) xQueueSend(queue, &event, 0);
}

bool receiver_flush(uint32_t timeout_ms)
{
    if (!queue) return false;
    event_t event = {.type = EV_BARRIER, .waiter = xTaskGetCurrentTaskHandle()};
    if (xQueueSend(queue, &event, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return false;
    return ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeout_ms)) != 0;
}

static void receiver_task(void *argument)
{
    (void)argument;
    event_t event;
    for (;;) {
        if (xQueueReceive(queue, &event, portMAX_DELAY) != pdTRUE) continue;
        if (event.type == EV_BARRIER) {
            xTaskNotifyGive(event.waiter);
            continue;
        }
        if (event.type == EV_TX_OK) {
            metrics_tx_complete(true);
            continue;
        }
        if (event.type == EV_TX_FAIL) {
            metrics_tx_complete(false);
            continue;
        }
        if (event.type == EV_DROP) {
            metrics_drop(false, true);
            continue;
        }
        bench_header_t header;
        if (!bench_decode(event.data, event.len, &header)) {
            metrics_drop(false, true);
            continue;
        }
        bool measurement_state = g_state == STATE_ARMED || g_state == STATE_RUNNING ||
                                 g_state == STATE_FINISHED;
        if (header.run_id != g_config.run_id || !measurement_state) {
            metrics_drop(true, false);
            continue;
        }
        metrics_rx(&header, event.rssi, event.at);
        if (g_config.role == ROLE_ECHO_RESPONDER && header.type == PACKET_PING) {
            event.data[5] = PACKET_ECHO;
            metrics_tx_requested(header.stream_id);
            esp_err_t error = radio_acquire_tx(100);
            if (error == ESP_OK) {
                uint64_t dispatch_us = (uint64_t)esp_timer_get_time();
                error = radio_send_acquired(event.data, event.len);
                if (error == ESP_OK) {
                    metrics_tx_submitted(header.stream_id, event.len, dispatch_us);
                }
            }
            if (error != ESP_OK) metrics_tx_api_failure();
        }
    }
}

void receiver_start(void)
{
    queue = xQueueCreateStatic(RECEIVER_QUEUE_LENGTH, sizeof(event_t), queue_bytes, &queue_storage);
    xTaskCreate(receiver_task, "bench_rx", 4096, NULL, 12, NULL);
}
