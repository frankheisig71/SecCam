#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_camera.h"
#include "esp_err.h"

esp_err_t app_camera_init();
esp_err_t app_camera_capture_now();
esp_err_t app_camera_lock_latest_image(const uint8_t **data,
                                       size_t *size,
                                       int64_t *captured_at_us,
                                       uint16_t *width,
                                       uint16_t *height,
                                       uint32_t timeout_ms);
esp_err_t app_camera_copy_latest_image(uint8_t **data,
                                       size_t *size,
                                       int64_t *captured_at_us,
                                       uint16_t *width,
                                       uint16_t *height,
                                       uint32_t timeout_ms);
esp_err_t app_camera_replace_latest_image(uint8_t *data,
                                          size_t size,
                                          int64_t captured_at_us,
                                          uint16_t width,
                                          uint16_t height,
                                          uint32_t timeout_ms);
void app_camera_free_image_copy(uint8_t *data);
void app_camera_unlock_latest_image();

