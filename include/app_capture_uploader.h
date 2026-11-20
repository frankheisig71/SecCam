#pragma once

#include <cstdint>

#include "esp_err.h"
#include "esp_pm.h"

esp_err_t app_capture_uploader_init(esp_pm_lock_handle_t cpu_max_lock);

// Queue a freshly captured image for asynchronous classification before it is uploaded.
esp_err_t app_capture_uploader_enqueue_analysis_image(const char *capture_reason,
                                                      bool motion_capture,
                                                      uint32_t sequence_index,
                                                      uint32_t sequence_size);

// Queue a pre-captured JPEG buffer for asynchronous classification.
// The buffer is copied into uploader-owned storage before the call returns.
esp_err_t app_capture_uploader_enqueue_analysis_buffer(const uint8_t *image_data,
                                                       size_t image_size,
                                                       int64_t captured_at_us,
                                                       uint16_t width,
                                                       uint16_t height,
                                                       const char *capture_reason,
                                                       bool motion_capture,
                                                       uint32_t sequence_index,
                                                       uint32_t sequence_size);

// Copies the latest JPEG from the camera buffer into an internal FIFO.
// A dedicated worker task uploads queued images one-by-one and retries failed transfers.
esp_err_t app_capture_uploader_upload_latest_image(const char *capture_reason,
                                                   bool motion_capture,
                                                   uint32_t sequence_index,
                                                   uint32_t sequence_size,
                                                   bool person_present = false,
                                                   float score = -1.0f);

bool app_capture_uploader_should_pause_motion_capture();