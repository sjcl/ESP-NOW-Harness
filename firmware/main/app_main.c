#include "control.h"
#include "harness.h"
#include "radio.h"
#include "receiver.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static void control_task(void*arg){(void)arg;control_run();}
void app_main(void){receiver_start();if(radio_init()!=ESP_OK)g_state=STATE_ERROR;xTaskCreate(control_task,"control",6144,NULL,11,NULL);}
