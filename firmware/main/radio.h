#pragma once
#include "harness.h"
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
esp_err_t radio_init(void);
esp_err_t radio_configure(const harness_config_t *cfg);
esp_err_t radio_acquire_tx(uint32_t timeout_ms);
esp_err_t radio_send_acquired(const uint8_t *data,size_t len);
void radio_release_tx(void);
esp_err_t radio_send(const uint8_t *data,size_t len,uint32_t timeout_ms);
esp_err_t radio_wait_idle(uint32_t timeout_ms);
