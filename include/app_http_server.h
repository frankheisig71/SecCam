#pragma once

#include "esp_err.h"

using app_http_capture_request_fn = esp_err_t (*)();

esp_err_t app_http_server_start(app_http_capture_request_fn capture_request_fn);
bool app_http_server_is_transmitting_image();
