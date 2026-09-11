#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
void receiver_start(void);void receiver_post_rx(const uint8_t*data,size_t len,int8_t rssi,uint64_t timestamp);void receiver_post_tx(bool success);void receiver_post_drop(bool stale,bool invalid);

