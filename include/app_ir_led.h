#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t app_ir_led_init();
void app_ir_led_set_enabled(bool enabled);
bool app_ir_led_is_enabled();