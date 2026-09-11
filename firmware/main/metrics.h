#pragma once
#include "harness.h"
#include "protocol.h"
#include "cJSON.h"
#include <stdbool.h>
#include <stdint.h>

#define METRIC_SAMPLE_CAPACITY 8192

typedef struct {
    uint64_t tx_requested;
    uint64_t tx_submitted;
    uint64_t tx_bytes;
    uint64_t send_cb_success;
    uint64_t send_cb_failure;
    uint64_t send_api_failure;
    uint64_t rx_packets;
    uint64_t rx_bytes;
    uint64_t duplicate;
    uint64_t out_of_order;
    uint64_t estimated_loss;
    uint64_t stale;
    uint64_t invalid;
    uint64_t queue_drops;
} metric_totals_t;

typedef struct {
    bool init;
    uint32_t highest;
    uint64_t bitmap;
    uint64_t rx;
    uint64_t duplicate;
    uint64_t out_of_order;
    uint64_t gaps;
    uint64_t tx;
} stream_metric_t;

typedef struct {
    uint32_t samples[METRIC_SAMPLE_CAPACITY];
    uint32_t reservoir_count;
    uint64_t sample_count;
    uint64_t sum;
    long double sum_sq;
    uint32_t min;
    uint32_t max;
} sample_distribution_t;

typedef struct {
    metric_totals_t totals;
    stream_metric_t streams[HARNESS_MAX_STREAMS];
    uint64_t start_us;
    uint64_t end_us;
    uint64_t first_tx_us;
    uint64_t last_tx_us;
    uint64_t first_rx_us;
    uint64_t last_rx_us;
    int64_t rssi_sum;
    uint64_t rssi_count;
    int8_t rssi_min;
    int8_t rssi_max;
    uint32_t rssi_hist[128];
    sample_distribution_t rtt;
    sample_distribution_t dispatch_lateness;
    uint32_t last_rtt;
    uint32_t jitter_max;
    uint64_t jitter_sum;
    uint64_t jitter_count;
} metrics_t;

extern metrics_t g_metrics;
void metrics_reset(void);
void metrics_tx_requested(uint16_t stream);
void metrics_tx_submitted(uint16_t stream, uint16_t length, uint64_t dispatch_us);
void metrics_tx_api_failure(void);
void metrics_tx_complete(bool success);
void metrics_dispatch_lateness(uint64_t actual_dispatch_us, uint64_t scheduled_deadline_us);
void metrics_rx(const bench_header_t *header, int8_t rssi, uint64_t now_us);
void metrics_drop(bool stale, bool invalid);
cJSON *metrics_json(void);
