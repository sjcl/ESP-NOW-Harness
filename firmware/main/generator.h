#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
esp_err_t generator_start(void);
bool generator_wait_stopped(uint32_t timeout_ms);
