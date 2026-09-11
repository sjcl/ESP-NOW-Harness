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
static void wait_deadline(uint64_t d){for(;;){uint64_t now=(uint64_t)esp_timer_get_time();if(now>=d)return;uint64_t rem=d-now;if(rem>2000)vTaskDelay(pdMS_TO_TICKS((rem-500)/1000));else{esp_rom_delay_us((uint32_t)rem);return;}}}
static void send_one(uint8_t type,uint16_t stream,uint32_t seq,uint16_t size,uint64_t stamp){uint8_t packet[HARNESS_MAX_PACKET_SIZE];memset(packet,0xa5,size);bench_header_t h={.type=type,.run_id=g_config.run_id,.stream_id=stream,.packet_len=size,.seq=seq,.timestamp_us=stamp};if(!bench_encode(packet,sizeof(packet),&h))return;metrics_tx_requested(stream);esp_err_t e=radio_send(packet,size,100);if(e==ESP_OK)metrics_tx_submitted(stream,size);else metrics_tx_api_failure();}
static void task(void*arg){(void)arg;uint32_t seq[HARNESS_MAX_STREAMS]={0};uint64_t start=(uint64_t)esp_timer_get_time(),end=start+g_config.duration_us,index=0;g_metrics.start_us=start;if(g_config.mode==MODE_LATENCY_LOAD){uint64_t bi=0,pi=0,next_b=start,next_p=start;while(g_state==STATE_RUNNING&&(uint64_t)esp_timer_get_time()<end){bool probe=next_p<=next_b;uint64_t d=probe?next_p:next_b;wait_deadline(d);if(probe){send_one(PACKET_PING,HARNESS_MAX_STREAMS-1,seq[HARNESS_MAX_STREAMS-1]++,g_config.packet_size,(uint64_t)esp_timer_get_time());pi++;next_p=start+pi*1000000ULL/g_config.probe_rate;}else{uint16_t s=(uint16_t)(bi%((g_config.streams>15)?15:g_config.streams));send_one(PACKET_STREAM,s,seq[s]++,g_config.background_packet_size,(uint64_t)esp_timer_get_time());bi++;next_b=start+bi*1000000ULL/g_config.background_rate;}}}else{uint64_t rate=g_config.mode==MODE_SATURATION?0:(uint64_t)g_config.rate_per_stream*g_config.streams;while(g_state==STATE_RUNNING&&(uint64_t)esp_timer_get_time()<end){if(rate)wait_deadline(start+index*1000000ULL/rate);uint16_t s=(uint16_t)(index%g_config.streams);uint8_t type=g_config.mode==MODE_PING?PACKET_PING:PACKET_STREAM;send_one(type,s,seq[s]++,g_config.packet_size,(uint64_t)esp_timer_get_time());index++;}}g_metrics.end_us=(uint64_t)esp_timer_get_time();g_state=STATE_FINISHED;handle=NULL;vTaskDelete(NULL);}
esp_err_t generator_start(void){if(handle)return ESP_ERR_INVALID_STATE;if(g_config.role!=ROLE_GENERATOR&&g_config.role!=ROLE_PING_INITIATOR)return ESP_OK;return xTaskCreate(task,"bench_gen",4096,NULL,10,&handle)==pdPASS?ESP_OK:ESP_ERR_NO_MEM;}
