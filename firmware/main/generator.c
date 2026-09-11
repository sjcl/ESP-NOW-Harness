#include "generator.h"
#include "harness.h"
#include "metrics.h"
#include "protocol.h"
#include "radio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static TaskHandle_t handle;

static void wait_deadline(uint64_t deadline_us)
{
    for (;;) {
        uint64_t now_us = (uint64_t)esp_timer_get_time();
        if (now_us >= deadline_us) return;
        uint64_t remaining_us = deadline_us - now_us;
        if (remaining_us > 2000) {
            vTaskDelay(pdMS_TO_TICKS((remaining_us - 500) / 1000));
        } else {
            esp_rom_delay_us((uint32_t)remaining_us);
            return;
        }
    }
}

static void send_one(uint8_t type, uint16_t stream, uint32_t sequence, uint16_t size,
                     uint64_t scheduled_deadline_us, bool scheduled)
{
    uint8_t packet[HARNESS_MAX_PACKET_SIZE];
    metrics_tx_requested(stream);
    esp_err_t error = radio_acquire_tx(100);
    if (error != ESP_OK) {
        metrics_tx_api_failure();
        return;
    }

    uint64_t dispatch_us = (uint64_t)esp_timer_get_time();
    if (scheduled) metrics_dispatch_lateness(dispatch_us, scheduled_deadline_us);
    memset(packet, 0xa5, size);
    bench_header_t header = {
        .type = type,
        .run_id = g_config.run_id,
        .stream_id = stream,
        .packet_len = size,
        .seq = sequence,
        .timestamp_us = dispatch_us,
    };
    if (!bench_encode(packet, sizeof(packet), &header)) {
        radio_release_tx();
        metrics_tx_api_failure();
        return;
    }
    error = radio_send_acquired(packet, size);
    if (error == ESP_OK) {
        metrics_tx_submitted(stream, size, dispatch_us);
    } else {
        metrics_tx_api_failure();
        if (!scheduled) taskYIELD();
    }
}

static void generator_task(void *argument)
{
    (void)argument;
    uint32_t sequences[HARNESS_MAX_STREAMS] = {0};
    uint64_t start_us = (uint64_t)esp_timer_get_time();
    uint64_t end_us = start_us + g_config.duration_us;
    uint64_t index = 0;
    g_metrics.start_us = start_us;

    if (g_config.mode == MODE_LATENCY_LOAD) {
        uint64_t background_index = 0;
        uint64_t probe_index = 0;
        uint64_t next_background_us = start_us;
        uint64_t next_probe_us = start_us;
        while (g_state == STATE_RUNNING && (uint64_t)esp_timer_get_time() < end_us) {
            bool probe = next_probe_us <= next_background_us;
            uint64_t deadline_us = probe ? next_probe_us : next_background_us;
            wait_deadline(deadline_us);
            if (probe) {
                uint16_t stream = HARNESS_MAX_STREAMS - 1;
                send_one(PACKET_PING, stream, sequences[stream]++, g_config.packet_size,
                         deadline_us, true);
                probe_index++;
                next_probe_us = start_us + probe_index * 1000000ULL / g_config.probe_rate;
            } else {
                uint16_t background_streams = g_config.streams > 15 ? 15 : g_config.streams;
                uint16_t stream = (uint16_t)(background_index % background_streams);
                send_one(PACKET_STREAM, stream, sequences[stream]++,
                         g_config.background_packet_size, deadline_us, true);
                background_index++;
                next_background_us = start_us +
                    background_index * 1000000ULL / g_config.background_rate;
            }
        }
    } else {
        uint64_t rate = g_config.mode == MODE_SATURATION ? 0 :
                        (uint64_t)g_config.rate_per_stream * g_config.streams;
        while (g_state == STATE_RUNNING && (uint64_t)esp_timer_get_time() < end_us) {
            uint64_t deadline_us = rate ? start_us + index * 1000000ULL / rate : 0;
            if (rate) wait_deadline(deadline_us);
            uint16_t stream = (uint16_t)(index % g_config.streams);
            uint8_t type = g_config.mode == MODE_PING ? PACKET_PING : PACKET_STREAM;
            send_one(type, stream, sequences[stream]++, g_config.packet_size,
                     deadline_us, rate != 0);
            index++;
        }
    }

    if (g_state == STATE_RUNNING) g_metrics.end_us = (uint64_t)esp_timer_get_time();
    if (g_state != STATE_ERROR) g_state = STATE_FINISHED;
    handle = NULL;
    vTaskDelete(NULL);
}

bool generator_wait_stopped(uint32_t timeout_ms)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (handle != NULL) {
        if ((int32_t)(xTaskGetTickCount() - deadline) >= 0) return false;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return true;
}

esp_err_t generator_start(void)
{
    if (handle) return ESP_ERR_INVALID_STATE;
    if (g_config.role != ROLE_GENERATOR && g_config.role != ROLE_PING_INITIATOR) return ESP_OK;
    return xTaskCreate(generator_task, "bench_gen", 4096, NULL, 10, &handle) == pdPASS ?
           ESP_OK : ESP_ERR_NO_MEM;
}
