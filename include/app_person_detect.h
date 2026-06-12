#pragma once

#include <cstddef>
#include <cstdint>

#include "app_config.h"
#include "esp_camera.h"
#include "esp_err.h"

struct app_person_detection_status_t {
  bool configured;
  bool model_loaded;
  bool ready;
  bool reference_ready;
  bool motion_detected;
  bool person_present;
  uint32_t detection_count;
  float motion_score;
  float score;
  int64_t reference_updated_at_us;
  int64_t analyzed_at_us;
  uint32_t inference_time_ms;
};

struct app_person_motion_debug_t {
  uint16_t width;
  uint16_t height;
  int64_t current_captured_at_us;
  int64_t reference_updated_at_us;
  bool reference_ready;
  float motion_score;
  uint8_t current[APP_PERSON_MOTION_GRID_WIDTH * APP_PERSON_MOTION_GRID_HEIGHT];
  uint8_t reference[APP_PERSON_MOTION_GRID_WIDTH * APP_PERSON_MOTION_GRID_HEIGHT];
  uint8_t diff[APP_PERSON_MOTION_GRID_WIDTH * APP_PERSON_MOTION_GRID_HEIGHT];
  uint8_t changed[APP_PERSON_MOTION_GRID_WIDTH * APP_PERSON_MOTION_GRID_HEIGHT];
};

esp_err_t app_person_detect_init();
esp_err_t app_person_detect_refresh_reference_from_latest_image();
esp_err_t app_person_detect_get_motion_reference(uint8_t *fingerprint,
                                                 size_t fingerprint_size,
                                                 uint16_t *width,
                                                 uint16_t *height,
                                                 int64_t *updated_at_us,
                                                 bool *valid,
                                                 uint32_t timeout_ms);
esp_err_t app_person_detect_get_motion_debug(app_person_motion_debug_t *debug_info, uint32_t timeout_ms);
esp_err_t app_person_detect_analyze_latest_image();
esp_err_t app_person_detect_analyze_jpeg_image(const uint8_t *jpeg_data,
                                               size_t jpeg_size,
                                               uint16_t width,
                                               uint16_t height,
                                               app_person_detection_status_t *status,
                                               float *frame_score);
esp_err_t app_person_detect_analyze_latest_image_with_result(app_person_detection_status_t *status,
                                                             float *frame_score);
esp_err_t app_person_detect_get_status(app_person_detection_status_t *status, uint32_t timeout_ms);
esp_err_t app_person_detect_set_status(const app_person_detection_status_t *status);
