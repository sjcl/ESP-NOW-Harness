#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define BENCH_MAGIC 0x4553504eu
#define BENCH_PROTOCOL_VERSION 1
#define BENCH_HEADER_SIZE 28
typedef enum { PACKET_STREAM=1, PACKET_PING=2, PACKET_ECHO=3 } bench_packet_type_t;
typedef struct { uint8_t type; uint16_t flags; uint32_t run_id; uint16_t stream_id; uint16_t packet_len; uint32_t seq; uint64_t timestamp_us; } bench_header_t;
bool bench_encode(uint8_t *dst,size_t capacity,const bench_header_t *h);
bool bench_decode(const uint8_t *src,size_t len,bench_header_t *h);

