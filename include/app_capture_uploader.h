#pragma once

#include <cstdint>

#include "esp_err.h"

esp_err_t app_capture_uploader_init();

// Copies the latest JPEG from the camera buffer into an internal FIFO.
// A dedicated worker task uploads queued images one-by-one and retries failed transfers.
esp_err_t app_capture_uploader_upload_latest_image(const char *capture_reason,
                                                   bool motion_capture,
                                                   uint32_t sequence_index,
                                                   uint32_t sequence_size);