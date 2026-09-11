#include "protocol.h"
#include <assert.h>
#include <string.h>
int main(void) {
    uint8_t wire[64]; memset(wire, 0xa5, sizeof(wire));
    bench_header_t in = {.type=PACKET_PING,.flags=0x1234,.run_id=42,.stream_id=15,.packet_len=64,.seq=0x10203040,.timestamp_us=UINT64_C(0x0102030405060708)};
    assert(bench_encode(wire,sizeof(wire),&in));
    assert(wire[0]==0x45 && wire[1]==0x53 && wire[2]==0x50 && wire[3]==0x4e);
    assert(wire[16]==0x10 && wire[17]==0x20 && wire[18]==0x30 && wire[19]==0x40);
    bench_header_t out; assert(bench_decode(wire,sizeof(wire),&out));
    assert(out.run_id==in.run_id && out.seq==in.seq && out.timestamp_us==in.timestamp_us);
    wire[4]=9; assert(!bench_decode(wire,sizeof(wire),&out));
    return 0;
}
