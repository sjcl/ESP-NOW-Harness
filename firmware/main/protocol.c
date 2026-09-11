#include "protocol.h"
static void put16(uint8_t*p,uint16_t v){p[0]=(uint8_t)(v>>8);p[1]=(uint8_t)v;}
static void put32(uint8_t*p,uint32_t v){p[0]=(uint8_t)(v>>24);p[1]=(uint8_t)(v>>16);p[2]=(uint8_t)(v>>8);p[3]=(uint8_t)v;}
static void put64(uint8_t*p,uint64_t v){put32(p,(uint32_t)(v>>32));put32(p+4,(uint32_t)v);}
static uint16_t get16(const uint8_t*p){return (uint16_t)(((uint16_t)p[0]<<8)|p[1]);}
static uint32_t get32(const uint8_t*p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static uint64_t get64(const uint8_t*p){return ((uint64_t)get32(p)<<32)|get32(p+4);}
bool bench_encode(uint8_t*d,size_t n,const bench_header_t*h){if(!d||!h||n<h->packet_len||h->packet_len<BENCH_HEADER_SIZE)return false;put32(d,BENCH_MAGIC);d[4]=BENCH_PROTOCOL_VERSION;d[5]=h->type;put16(d+6,h->flags);put32(d+8,h->run_id);put16(d+12,h->stream_id);put16(d+14,h->packet_len);put32(d+16,h->seq);put64(d+20,h->timestamp_us);return true;}
bool bench_decode(const uint8_t*s,size_t n,bench_header_t*h){if(!s||!h||n<BENCH_HEADER_SIZE||get32(s)!=BENCH_MAGIC||s[4]!=BENCH_PROTOCOL_VERSION)return false;h->type=s[5];h->flags=get16(s+6);h->run_id=get32(s+8);h->stream_id=get16(s+12);h->packet_len=get16(s+14);h->seq=get32(s+16);h->timestamp_us=get64(s+20);return h->packet_len==n&&h->stream_id<16;}

