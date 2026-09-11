#pragma once
#include "harness.h"
#include "protocol.h"
#include <stdint.h>
#include "cJSON.h"
#define RTT_CAPACITY 8192
typedef struct { uint64_t tx_requested,tx_submitted,tx_bytes,send_cb_success,send_cb_failure,send_api_failure,rx_packets,rx_bytes,duplicate,out_of_order,estimated_loss,stale,invalid,queue_drops; } metric_totals_t;
typedef struct { bool init;uint32_t highest;uint64_t bitmap;uint64_t rx,duplicate,out_of_order,gaps;uint64_t tx; } stream_metric_t;
typedef struct { metric_totals_t totals;stream_metric_t streams[HARNESS_MAX_STREAMS];uint64_t start_us,end_us;int64_t rssi_sum;uint64_t rssi_count;int8_t rssi_min,rssi_max;uint32_t rssi_hist[128];uint32_t rtt[RTT_CAPACITY];uint32_t rtt_count;uint64_t rtt_seen;uint64_t rtt_sum;long double rtt_sum_sq;uint32_t rtt_min,rtt_max,last_rtt,jitter_max;uint64_t jitter_sum,jitter_count; } metrics_t;
extern metrics_t g_metrics;
void metrics_reset(void);void metrics_tx_requested(uint16_t stream);void metrics_tx_submitted(uint16_t stream,uint16_t len);void metrics_tx_api_failure(void);void metrics_tx_complete(bool ok);void metrics_rx(const bench_header_t*h,int8_t rssi,uint64_t now);void metrics_drop(bool stale,bool invalid);cJSON *metrics_json(void);
