#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void receiver_start(void);
void receiver_post_rx(const uint8_t *data, size_t length, int8_t rssi, uint64_t timestamp_us);
void receiver_post_tx(bool success);
void receiver_post_drop(bool stale, bool invalid);
bool receiver_flush(uint32_t timeout_ms);
