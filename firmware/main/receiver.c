#include "receiver.h"
#include "harness.h"
#include "metrics.h"
#include "protocol.h"
#include "radio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <string.h>
typedef enum{EV_RX,EV_TX_OK,EV_TX_FAIL,EV_DROP}event_type_t;typedef struct{event_type_t type;uint16_t len;int8_t rssi;uint64_t at;uint8_t data[HARNESS_MAX_PACKET_SIZE];}event_t;
static QueueHandle_t queue;static StaticQueue_t queue_storage;static uint8_t queue_bytes[32*sizeof(event_t)];
void receiver_post_rx(const uint8_t*d,size_t n,int8_t r,uint64_t at){event_t e={.type=EV_RX,.len=(uint16_t)n,.rssi=r,.at=at};memcpy(e.data,d,n);if(xQueueSend(queue,&e,0)!=pdTRUE)__atomic_fetch_add(&g_metrics.totals.queue_drops,1,__ATOMIC_RELAXED);}
void receiver_post_tx(bool ok){event_t e={.type=ok?EV_TX_OK:EV_TX_FAIL};if(xQueueSend(queue,&e,0)!=pdTRUE)__atomic_fetch_add(&g_metrics.totals.queue_drops,1,__ATOMIC_RELAXED);}
void receiver_post_drop(bool stale,bool invalid){(void)stale;(void)invalid;event_t e={.type=EV_DROP};if(queue)xQueueSend(queue,&e,0);}
static void task(void*arg){(void)arg;event_t e;for(;;){if(xQueueReceive(queue,&e,portMAX_DELAY)!=pdTRUE)continue;if(e.type==EV_TX_OK){metrics_tx_complete(true);continue;}if(e.type==EV_TX_FAIL){metrics_tx_complete(false);continue;}if(e.type==EV_DROP){metrics_drop(false,true);continue;}bench_header_t h;if(!bench_decode(e.data,e.len,&h)){metrics_drop(false,true);continue;}if(h.run_id!=g_config.run_id||g_state<STATE_ARMED){metrics_drop(true,false);continue;}metrics_rx(&h,e.rssi,e.at);if(g_config.role==ROLE_ECHO_RESPONDER&&h.type==PACKET_PING){e.data[5]=PACKET_ECHO;esp_err_t err=radio_send(e.data,e.len,100);metrics_tx_requested(h.stream_id);if(err==ESP_OK)metrics_tx_submitted(h.stream_id,e.len);else metrics_tx_api_failure();}}}
void receiver_start(void){queue=xQueueCreateStatic(32,sizeof(event_t),queue_bytes,&queue_storage);xTaskCreate(task,"bench_rx",4096,NULL,12,NULL);}

