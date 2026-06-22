#pragma once

#include "esp_err.h"
#include <stdint.h>

esp_err_t app_status_led_init();
void app_status_led_red();
void app_status_led_green();
void app_status_led_blue();
void app_status_led_yellow();
void app_status_led_off();
void app_status_led_blink_blue_ms(uint32_t ms);
