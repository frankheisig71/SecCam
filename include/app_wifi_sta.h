#pragma once

#include "esp_err.h"

esp_err_t app_wifi_sta_start();
esp_err_t app_wifi_sta_force_reconnect();
bool app_wifi_sta_is_busy();
