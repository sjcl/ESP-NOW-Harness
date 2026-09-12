#include "metrics.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

metrics_t g_metrics;

static void distribution_reset(sample_distribution_t *distribution)
{
    memset(distribution, 0, sizeof(*distribution));
    distribution->min = UINT32_MAX;
}

static uint64_t sample_hash(uint64_t value)
{
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

static void distribution_add(sample_distribution_t *distribution, uint64_t value)
{
    uint32_t bounded = value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
    distribution->sample_count++;
    distribution->sum += bounded;
    distribution->sum_sq += (long double)bounded * bounded;
    if (bounded < distribution->min) distribution->min = bounded;
    if (bounded > distribution->max) distribution->max = bounded;
    if (distribution->reservoir_count < METRIC_SAMPLE_CAPACITY) {
        distribution->samples[distribution->reservoir_count++] = bounded;
        return;
    }
    uint64_t slot = sample_hash(distribution->sample_count) % distribution->sample_count;
    if (slot < METRIC_SAMPLE_CAPACITY) distribution->samples[slot] = bounded;
}

void metrics_reset(void)
{
    memset(&g_metrics, 0, sizeof(g_metrics));
    g_metrics.rssi_min = 127;
    g_metrics.rssi_max = -127;
    distribution_reset(&g_metrics.rtt);
    distribution_reset(&g_metrics.dispatch_lateness);
}

void metrics_tx_requested(uint16_t stream)
{
    __atomic_fetch_add(&g_metrics.totals.tx_requested, 1, __ATOMIC_RELAXED);
    if (stream < HARNESS_MAX_STREAMS) {
        __atomic_fetch_add(&g_metrics.streams[stream].tx_requested, 1, __ATOMIC_RELAXED);
    }
}

void metrics_tx_submitted(uint16_t stream, uint16_t length, uint64_t dispatch_us)
{
    __atomic_fetch_add(&g_metrics.totals.tx_submitted, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_metrics.totals.tx_bytes, length, __ATOMIC_RELAXED);
    if (stream < HARNESS_MAX_STREAMS) {
        __atomic_fetch_add(&g_metrics.streams[stream].tx_submitted, 1, __ATOMIC_RELAXED);
    }
    if (g_metrics.first_tx_us == 0) g_metrics.first_tx_us = dispatch_us;
    g_metrics.last_tx_us = dispatch_us;
}

void metrics_tx_api_failure(void)
{
    __atomic_fetch_add(&g_metrics.totals.send_api_failure, 1, __ATOMIC_RELAXED);
}

void metrics_tx_slot_timeout(void)
{
    __atomic_fetch_add(&g_metrics.totals.tx_slot_timeout, 1, __ATOMIC_RELAXED);
}

void metrics_tx_complete(bool success)
{
    if (success) g_metrics.totals.send_cb_success++;
    else g_metrics.totals.send_cb_failure++;
}

void metrics_dispatch_lateness(uint64_t actual_dispatch_us, uint64_t scheduled_deadline_us)
{
    uint64_t lateness = actual_dispatch_us > scheduled_deadline_us ?
                        actual_dispatch_us - scheduled_deadline_us : 0;
    distribution_add(&g_metrics.dispatch_lateness, lateness);
}

static void sequence_observe(stream_metric_t *metric, uint32_t sequence)
{
    metric->rx++;
    if (!metric->init) {
        metric->init = true;
        metric->highest = sequence;
        metric->bitmap = 1;
        metric->gaps = sequence;
        return;
    }
    if (sequence > metric->highest) {
        uint32_t distance = sequence - metric->highest;
        if (distance > 1) metric->gaps += (uint64_t)distance - 1;
        metric->bitmap = distance >= 64 ? 1 : (metric->bitmap << distance) | 1;
        metric->highest = sequence;
        return;
    }
    uint32_t distance = metric->highest - sequence;
    if (distance >= 64) {
        metric->out_of_order++;
        return;
    }
    uint64_t bit = UINT64_C(1) << distance;
    if (metric->bitmap & bit) {
        metric->duplicate++;
        return;
    }
    metric->bitmap |= bit;
    metric->out_of_order++;
    if (metric->gaps) metric->gaps--;
}

void metrics_rx(const bench_header_t *header, int8_t rssi, uint64_t now_us)
{
    if (g_metrics.totals.rx_packets == 0) g_metrics.first_rx_us = now_us;
    g_metrics.last_rx_us = now_us;
    g_metrics.totals.rx_packets++;
    g_metrics.totals.rx_bytes += header->packet_len;
    stream_metric_t *stream = &g_metrics.streams[header->stream_id];
    uint64_t old_out_of_order = stream->out_of_order;
    uint64_t old_duplicate = stream->duplicate;
    sequence_observe(stream, header->seq);
    g_metrics.totals.out_of_order += stream->out_of_order - old_out_of_order;
    g_metrics.totals.duplicate += stream->duplicate - old_duplicate;

    g_metrics.rssi_count++;
    g_metrics.rssi_sum += rssi;
    if (rssi < g_metrics.rssi_min) g_metrics.rssi_min = rssi;
    if (rssi > g_metrics.rssi_max) g_metrics.rssi_max = rssi;
    int index = rssi + 127;
    if (index < 0) index = 0;
    if (index > 127) index = 127;
    g_metrics.rssi_hist[index]++;

    if (header->type == PACKET_ECHO && now_us >= header->timestamp_us) {
        uint64_t rtt = now_us - header->timestamp_us;
        uint32_t bounded_rtt = rtt > UINT32_MAX ? UINT32_MAX : (uint32_t)rtt;
        if (g_metrics.rtt.sample_count) {
            uint32_t jitter = bounded_rtt > g_metrics.last_rtt ?
                              bounded_rtt - g_metrics.last_rtt : g_metrics.last_rtt - bounded_rtt;
            g_metrics.jitter_sum += jitter;
            g_metrics.jitter_count++;
            if (jitter > g_metrics.jitter_max) g_metrics.jitter_max = jitter;
        }
        g_metrics.last_rtt = bounded_rtt;
        distribution_add(&g_metrics.rtt, bounded_rtt);
    }
}

void metrics_drop(bool stale, bool invalid)
{
    if (stale) g_metrics.totals.stale++;
    if (invalid) g_metrics.totals.invalid++;
}

static int compare_u32(const void *left, const void *right)
{
    uint32_t a = *(const uint32_t *)left;
    uint32_t b = *(const uint32_t *)right;
    return (a > b) - (a < b);
}

static uint32_t nearest_rank(const sample_distribution_t *distribution, double quantile)
{
    if (distribution->reservoir_count == 0) return 0;
    size_t rank = (size_t)ceil(quantile * distribution->reservoir_count);
    if (rank < 1) rank = 1;
    if (rank > distribution->reservoir_count) rank = distribution->reservoir_count;
    return distribution->samples[rank - 1];
}

static void add_u64(cJSON *object, const char *name, uint64_t value)
{
    cJSON_AddNumberToObject(object, name, (double)value);
}

static cJSON *distribution_json(sample_distribution_t *distribution)
{
    qsort(distribution->samples, distribution->reservoir_count, sizeof(uint32_t), compare_u32);
    double mean = distribution->sample_count ? (double)distribution->sum / distribution->sample_count : 0;
    double variance = distribution->sample_count ?
                      (double)(distribution->sum_sq / distribution->sample_count) - mean * mean : 0;
    cJSON *object = cJSON_CreateObject();
    add_u64(object, "sample_count", distribution->sample_count);
    cJSON_AddNumberToObject(object, "reservoir_count", distribution->reservoir_count);
    cJSON_AddNumberToObject(object, "min_us", distribution->sample_count ? distribution->min : 0);
    cJSON_AddNumberToObject(object, "mean_us", mean);
    cJSON_AddNumberToObject(object, "p50_us", nearest_rank(distribution, 0.50));
    cJSON_AddNumberToObject(object, "p90_us", nearest_rank(distribution, 0.90));
    cJSON_AddNumberToObject(object, "p95_us", nearest_rank(distribution, 0.95));
    cJSON_AddNumberToObject(object, "p99_us", nearest_rank(distribution, 0.99));
    cJSON_AddNumberToObject(object, "p999_us", nearest_rank(distribution, 0.999));
    cJSON_AddNumberToObject(object, "max_us", distribution->max);
    cJSON_AddNumberToObject(object, "stddev_us", sqrt(variance > 0 ? variance : 0));
    return object;
}

cJSON *metrics_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *totals = cJSON_AddObjectToObject(root, "totals");
    uint64_t gaps = 0;
    for (size_t i = 0; i < HARNESS_MAX_STREAMS; ++i) gaps += g_metrics.streams[i].gaps;
    g_metrics.totals.estimated_loss = gaps;
#define ADD_TOTAL(name) add_u64(totals, #name, g_metrics.totals.name)
    ADD_TOTAL(tx_requested); ADD_TOTAL(tx_submitted); ADD_TOTAL(tx_bytes);
    ADD_TOTAL(send_cb_success); ADD_TOTAL(send_cb_failure); ADD_TOTAL(send_api_failure);
    ADD_TOTAL(tx_slot_timeout);
    ADD_TOTAL(rx_packets); ADD_TOTAL(rx_bytes); ADD_TOTAL(duplicate); ADD_TOTAL(out_of_order);
    ADD_TOTAL(estimated_loss); ADD_TOTAL(stale); ADD_TOTAL(invalid); ADD_TOTAL(queue_drops);
#undef ADD_TOTAL
    uint64_t elapsed = g_metrics.end_us > g_metrics.start_us ? g_metrics.end_us - g_metrics.start_us : 0;
    uint64_t tx_window = g_metrics.last_tx_us > g_metrics.first_tx_us ? g_metrics.last_tx_us - g_metrics.first_tx_us : 0;
    uint64_t rx_window = g_metrics.last_rx_us > g_metrics.first_rx_us ? g_metrics.last_rx_us - g_metrics.first_rx_us : 0;
    uint64_t requested_pps = g_config.mode == MODE_SATURATION ? 0 :
        (g_config.mode == MODE_LATENCY_LOAD ? (uint64_t)g_config.background_rate + g_config.probe_rate :
         (uint64_t)g_config.rate_per_stream * g_config.streams);
    add_u64(totals, "elapsed_us", elapsed);
    add_u64(totals, "first_tx_us", g_metrics.first_tx_us);
    add_u64(totals, "last_tx_us", g_metrics.last_tx_us);
    add_u64(totals, "tx_window_us", tx_window);
    add_u64(totals, "first_rx_us", g_metrics.first_rx_us);
    add_u64(totals, "last_rx_us", g_metrics.last_rx_us);
    add_u64(totals, "rx_window_us", rx_window);
    add_u64(totals, "requested_pps", requested_pps);
    cJSON_AddNumberToObject(totals, "tx_pps", elapsed ? g_metrics.totals.tx_submitted * 1e6 / elapsed : 0);
    cJSON_AddNumberToObject(totals, "tx_pps_active", tx_window ? g_metrics.totals.tx_submitted * 1e6 / tx_window : 0);
    cJSON_AddNumberToObject(totals, "rx_pps", elapsed ? g_metrics.totals.rx_packets * 1e6 / elapsed : 0);
    cJSON_AddNumberToObject(totals, "rx_pps_active", rx_window ? g_metrics.totals.rx_packets * 1e6 / rx_window : 0);
    cJSON_AddNumberToObject(totals, "tx_application_bps", elapsed ? g_metrics.totals.tx_bytes * 8e6 / elapsed : 0);
    cJSON_AddNumberToObject(totals, "tx_application_bps_active", tx_window ? g_metrics.totals.tx_bytes * 8e6 / tx_window : 0);
    cJSON_AddNumberToObject(totals, "rx_application_bps", elapsed ? g_metrics.totals.rx_bytes * 8e6 / elapsed : 0);
    cJSON_AddNumberToObject(totals, "goodput_bps", elapsed ? g_metrics.totals.rx_bytes * 8e6 / elapsed : 0);
    cJSON_AddNumberToObject(totals, "goodput_bps_active", rx_window ? g_metrics.totals.rx_bytes * 8e6 / rx_window : 0);

    cJSON *rssi = cJSON_AddObjectToObject(root, "rssi");
    cJSON_AddNumberToObject(rssi, "min", g_metrics.rssi_count ? g_metrics.rssi_min : 0);
    cJSON_AddNumberToObject(rssi, "max", g_metrics.rssi_count ? g_metrics.rssi_max : 0);
    cJSON_AddNumberToObject(rssi, "mean", g_metrics.rssi_count ? (double)g_metrics.rssi_sum / g_metrics.rssi_count : 0);
    cJSON *histogram = cJSON_AddArrayToObject(rssi, "histogram_minus_127_to_0");
    for (size_t i = 0; i < 128; ++i) cJSON_AddItemToArray(histogram, cJSON_CreateNumber(g_metrics.rssi_hist[i]));

    cJSON *rtt = distribution_json(&g_metrics.rtt);
    cJSON_AddNumberToObject(rtt, "jitter_mean_abs_delta_us",
                            g_metrics.jitter_count ? (double)g_metrics.jitter_sum / g_metrics.jitter_count : 0);
    cJSON_AddNumberToObject(rtt, "jitter_max_abs_delta_us", g_metrics.jitter_max);
    cJSON_AddItemToObject(root, "rtt", rtt);
    cJSON_AddItemToObject(root, "dispatch_lateness", distribution_json(&g_metrics.dispatch_lateness));

    cJSON *streams = cJSON_AddArrayToObject(root, "streams");
    for (size_t i = 0; i < HARNESS_MAX_STREAMS; ++i) {
        stream_metric_t *metric = &g_metrics.streams[i];
        if (!metric->tx_requested && !metric->rx) continue;
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "stream_id", i);
        add_u64(item, "tx_requested", metric->tx_requested);
        add_u64(item, "tx_submitted", metric->tx_submitted);
        add_u64(item, "rx_packets", metric->rx);
        add_u64(item, "missing", metric->gaps);
        add_u64(item, "duplicate", metric->duplicate);
        add_u64(item, "out_of_order", metric->out_of_order);
        cJSON_AddItemToArray(streams, item);
    }
    return root;
}
