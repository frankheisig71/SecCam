#include "app_person_detect.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <new>

#include "app_camera.h"
#include "app_config.h"
#include "dl_detect_base.hpp"
#include "dl_image_define.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "img_converters.h"
#if APP_PERSON_DETECT_BACKEND == APP_PERSON_DETECT_BACKEND_COCO_320
#include "coco_detect.hpp"
#elif APP_PERSON_DETECT_BACKEND == APP_PERSON_DETECT_BACKEND_PEDESTRIAN
#include "pedestrian_detect.hpp"
#endif

namespace {

constexpr char kTag[] = "app_person_detect";
SemaphoreHandle_t g_person_status_mutex = nullptr;
dl::detect::Detect *g_detector = nullptr;
app_person_detection_status_t g_status = {};
constexpr size_t kMotionGridCellCount =
    static_cast<size_t>(APP_PERSON_MOTION_GRID_WIDTH) * static_cast<size_t>(APP_PERSON_MOTION_GRID_HEIGHT);

struct MotionReference {
  std::array<uint8_t, kMotionGridCellCount> fingerprint = {};
  int64_t updated_at_us = 0;
  bool valid = false;
};

SemaphoreHandle_t g_motion_reference_mutex = nullptr;
MotionReference g_motion_reference = {};

const char *model_partition_label() {
#if APP_PERSON_DETECT_BACKEND == APP_PERSON_DETECT_BACKEND_COCO_320
  return "coco_det";
#elif APP_PERSON_DETECT_BACKEND == APP_PERSON_DETECT_BACKEND_EDGE_IMPULSE
  return nullptr;
#else
  return "pedestrian_det";
#endif
}

const char *backend_name() {
#if APP_PERSON_DETECT_BACKEND == APP_PERSON_DETECT_BACKEND_COCO_320
  return "coco_yolo11n_320";
#elif APP_PERSON_DETECT_BACKEND == APP_PERSON_DETECT_BACKEND_EDGE_IMPULSE
  return "edge_impulse";
#else
  return "pedestrian_pico";
#endif
}

bool has_valid_model_partition() {
  if (model_partition_label() == nullptr) {
    return true;
  }

  const esp_partition_t *partition =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, model_partition_label());
  if (partition == nullptr) {
    ESP_LOGW(kTag, "Model partition '%s' not found", model_partition_label());
    return false;
  }

  char header[4] = {};
  if (esp_partition_read(partition, 0, header, sizeof(header)) != ESP_OK) {
    ESP_LOGW(kTag, "Failed to read model partition header");
    return false;
  }

  const bool valid_format =
      (memcmp(header, "EDL1", 4) == 0) || (memcmp(header, "EDL2", 4) == 0) ||
      (memcmp(header, "PDL1", 4) == 0) || (memcmp(header, "PDL2", 4) == 0);
  if (!valid_format) {
    ESP_LOGW(kTag,
             "Model partition '%s' has invalid header %02X %02X %02X %02X",
             model_partition_label(),
             static_cast<unsigned char>(header[0]),
             static_cast<unsigned char>(header[1]),
             static_cast<unsigned char>(header[2]),
             static_cast<unsigned char>(header[3]));
  }
  return valid_format;
}

bool is_person_detection(const dl::detect::result_t &result) {
#if APP_PERSON_DETECT_BACKEND == APP_PERSON_DETECT_BACKEND_COCO_320
  return result.category == APP_PERSON_DETECT_COCO_PERSON_CATEGORY;
#elif APP_PERSON_DETECT_BACKEND == APP_PERSON_DETECT_BACKEND_EDGE_IMPULSE
  (void)result;
  return false;
#else
  (void)result;
  return true;
#endif
}

void update_status(const app_person_detection_status_t &new_status) {
  if (g_person_status_mutex == nullptr) {
    return;
  }

  if (xSemaphoreTake(g_person_status_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    g_status = new_status;
    xSemaphoreGive(g_person_status_mutex);
  }
}

void build_motion_fingerprint(const uint8_t *rgb_data, uint16_t width, uint16_t height, uint8_t *fingerprint) {
  if (rgb_data == nullptr || fingerprint == nullptr || width == 0 || height == 0) {
    return;
  }

  for (int grid_y = 0; grid_y < APP_PERSON_MOTION_GRID_HEIGHT; ++grid_y) {
    const uint32_t y0 = (static_cast<uint32_t>(grid_y) * height) / APP_PERSON_MOTION_GRID_HEIGHT;
    const uint32_t y1 = ((static_cast<uint32_t>(grid_y) + 1U) * height) / APP_PERSON_MOTION_GRID_HEIGHT;
    for (int grid_x = 0; grid_x < APP_PERSON_MOTION_GRID_WIDTH; ++grid_x) {
      const uint32_t x0 = (static_cast<uint32_t>(grid_x) * width) / APP_PERSON_MOTION_GRID_WIDTH;
      const uint32_t x1 = ((static_cast<uint32_t>(grid_x) + 1U) * width) / APP_PERSON_MOTION_GRID_WIDTH;
      uint32_t luminance_sum = 0;
      uint32_t pixel_count = 0;

      for (uint32_t y = y0; y < y1; ++y) {
        for (uint32_t x = x0; x < x1; ++x) {
          const size_t pixel_offset = (static_cast<size_t>(y) * width + x) * 3;
          const uint32_t red = rgb_data[pixel_offset];
          const uint32_t green = rgb_data[pixel_offset + 1];
          const uint32_t blue = rgb_data[pixel_offset + 2];
          luminance_sum += ((red * 77U) + (green * 150U) + (blue * 29U)) >> 8;
          pixel_count++;
        }
      }

      const size_t cell_index = static_cast<size_t>(grid_y) * APP_PERSON_MOTION_GRID_WIDTH + grid_x;
      fingerprint[cell_index] = pixel_count == 0 ? 0 : static_cast<uint8_t>(luminance_sum / pixel_count);
    }
  }
}

esp_err_t store_motion_reference(const uint8_t *fingerprint, int64_t updated_at_us) {
  if (fingerprint == nullptr || g_motion_reference_mutex == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }

  if (xSemaphoreTake(g_motion_reference_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  std::copy_n(fingerprint, kMotionGridCellCount, g_motion_reference.fingerprint.begin());
  g_motion_reference.updated_at_us = updated_at_us;
  g_motion_reference.valid = true;

  if (g_person_status_mutex != nullptr && xSemaphoreTake(g_person_status_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    g_status.reference_ready = true;
    g_status.reference_updated_at_us = updated_at_us;
    xSemaphoreGive(g_person_status_mutex);
  }

  xSemaphoreGive(g_motion_reference_mutex);
  return ESP_OK;
}

esp_err_t compute_motion_score(const uint8_t *fingerprint, float *motion_score, bool *reference_ready) {
  if (motion_score == nullptr || reference_ready == nullptr || fingerprint == nullptr ||
      g_motion_reference_mutex == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }

  *motion_score = 0.0f;
  *reference_ready = false;
  if (xSemaphoreTake(g_motion_reference_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  *reference_ready = g_motion_reference.valid;
  if (g_motion_reference.valid) {
    uint32_t changed_cells = 0;
    for (size_t index = 0; index < kMotionGridCellCount; ++index) {
      const int diff = static_cast<int>(fingerprint[index]) - static_cast<int>(g_motion_reference.fingerprint[index]);
      if (std::abs(diff) >= APP_PERSON_MOTION_CELL_DIFF_THRESHOLD) {
        changed_cells++;
      }
    }
    *motion_score = static_cast<float>(changed_cells) / static_cast<float>(kMotionGridCellCount);
  }

  xSemaphoreGive(g_motion_reference_mutex);
  return ESP_OK;
}

esp_err_t fill_motion_debug_from_fingerprint(const uint8_t *current_fingerprint,
                                             int64_t current_captured_at_us,
                                             app_person_motion_debug_t *debug_info,
                                             float *motion_score) {
  if (current_fingerprint == nullptr || debug_info == nullptr || motion_score == nullptr ||
      g_motion_reference_mutex == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }

  if (xSemaphoreTake(g_motion_reference_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  debug_info->width = APP_PERSON_MOTION_GRID_WIDTH;
  debug_info->height = APP_PERSON_MOTION_GRID_HEIGHT;
  debug_info->current_captured_at_us = current_captured_at_us;
  debug_info->reference_updated_at_us = g_motion_reference.updated_at_us;
  debug_info->reference_ready = g_motion_reference.valid;

  uint32_t changed_cells = 0;
  for (size_t index = 0; index < kMotionGridCellCount; ++index) {
    const uint8_t current_value = current_fingerprint[index];
    const uint8_t reference_value = g_motion_reference.valid ? g_motion_reference.fingerprint[index] : 0;
    const uint8_t diff_value = static_cast<uint8_t>(std::abs(static_cast<int>(current_value) - static_cast<int>(reference_value)));
    const bool changed = diff_value >= APP_PERSON_MOTION_CELL_DIFF_THRESHOLD;

    debug_info->current[index] = current_value;
    debug_info->reference[index] = reference_value;
    debug_info->diff[index] = diff_value;
    debug_info->changed[index] = changed ? 255 : 0;
    if (changed) {
      changed_cells++;
    }
  }

  *motion_score = g_motion_reference.valid
                      ? static_cast<float>(changed_cells) / static_cast<float>(kMotionGridCellCount)
                      : 0.0f;
  debug_info->motion_score = *motion_score;

  xSemaphoreGive(g_motion_reference_mutex);
  return ESP_OK;
}

esp_err_t refresh_reference_from_jpeg(const uint8_t *jpeg_data,
                                      size_t jpeg_size,
                                      uint16_t width,
                                      uint16_t height,
                                      int64_t updated_at_us) {
  if (jpeg_data == nullptr || jpeg_size == 0 || width == 0 || height == 0) {
    return ESP_ERR_INVALID_STATE;
  }

  const size_t rgb_size = static_cast<size_t>(width) * static_cast<size_t>(height) * 3;
  const uint32_t alloc_caps = MALLOC_CAP_8BIT |
                              (esp_psram_is_initialized() ? MALLOC_CAP_SPIRAM : MALLOC_CAP_DEFAULT);
  auto *rgb_data = static_cast<uint8_t *>(heap_caps_malloc(rgb_size, alloc_caps));
  if (rgb_data == nullptr) {
    return ESP_ERR_NO_MEM;
  }

  if (!fmt2rgb888(jpeg_data, jpeg_size, PIXFORMAT_JPEG, rgb_data)) {
    heap_caps_free(rgb_data);
    return ESP_FAIL;
  }

  std::array<uint8_t, kMotionGridCellCount> fingerprint = {};
  build_motion_fingerprint(rgb_data, width, height, fingerprint.data());
  heap_caps_free(rgb_data);

  return store_motion_reference(fingerprint.data(), updated_at_us);
}

esp_err_t analyze_jpeg_image(const uint8_t *jpeg_data,
                             size_t jpeg_size,
                             uint16_t width,
                             uint16_t height,
                             app_person_detection_status_t *status,
                             float *frame_score,
                             bool publish_status) {
  if (g_person_status_mutex == nullptr) {
    return ESP_ERR_NOT_FOUND;
  }

#if APP_PERSON_DETECT_BACKEND != APP_PERSON_DETECT_BACKEND_EDGE_IMPULSE
  if (g_detector == nullptr) {
    return ESP_ERR_NOT_FOUND;
  }
#endif

  if (jpeg_data == nullptr || jpeg_size == 0 || width == 0 || height == 0) {
    return ESP_ERR_INVALID_STATE;
  }

  const size_t rgb_size = static_cast<size_t>(width) * static_cast<size_t>(height) * 3;
  const uint32_t alloc_caps = MALLOC_CAP_8BIT |
                              (esp_psram_is_initialized() ? MALLOC_CAP_SPIRAM : MALLOC_CAP_DEFAULT);
  auto *rgb_data = static_cast<uint8_t *>(heap_caps_malloc(rgb_size, alloc_caps));
  if (rgb_data == nullptr) {
    return ESP_ERR_NO_MEM;
  }

  if (!fmt2rgb888(jpeg_data, jpeg_size, PIXFORMAT_JPEG, rgb_data)) {
    heap_caps_free(rgb_data);
    return ESP_FAIL;
  }

  const int64_t started_at_us = esp_timer_get_time();
  std::array<uint8_t, kMotionGridCellCount> fingerprint = {};
  build_motion_fingerprint(rgb_data, width, height, fingerprint.data());

  float motion_score_value = 0.0f;
  bool reference_ready = false;
  const esp_err_t motion_err = compute_motion_score(fingerprint.data(), &motion_score_value, &reference_ready);
  if (motion_err != ESP_OK) {
    heap_caps_free(rgb_data);
    return motion_err;
  }

  const bool motion_detected = !reference_ready || motion_score_value >= APP_PERSON_MOTION_CHANGED_CELL_RATIO;
  app_person_detection_status_t new_status = {};
  new_status.configured = true;
  new_status.model_loaded = true;
  new_status.ready = true;
  new_status.reference_ready = reference_ready;
  new_status.motion_detected = motion_detected;
  new_status.motion_score = motion_score_value;
  new_status.reference_updated_at_us = g_status.reference_updated_at_us;
  new_status.analyzed_at_us = esp_timer_get_time();

  if (!motion_detected) {
    heap_caps_free(rgb_data);
    new_status.person_present = false;
    new_status.detection_count = 0;
    new_status.score = 0.0f;
    new_status.inference_time_ms = 0;

    if (publish_status) {
      update_status(new_status);
    }
    if (status != nullptr) {
      *status = new_status;
    }
    if (frame_score != nullptr) {
      *frame_score = 0.0f;
    }

    ESP_LOGI(kTag,
             "Motion gate skipped inference: reference=%s motion=%.3f threshold=%.3f",
             reference_ready ? "ready" : "missing",
             static_cast<double>(motion_score_value),
             static_cast<double>(APP_PERSON_MOTION_CHANGED_CELL_RATIO));
    return ESP_OK;
  }

#if APP_PERSON_DETECT_BACKEND == APP_PERSON_DETECT_BACKEND_EDGE_IMPULSE
  heap_caps_free(rgb_data);
  new_status.person_present = false;
  new_status.detection_count = 0;
  new_status.score = 0.0f;
  new_status.inference_time_ms = 0;

  if (publish_status) {
    update_status(new_status);
  }
  if (status != nullptr) {
    *status = new_status;
  }
  if (frame_score != nullptr) {
    *frame_score = 0.0f;
  }

  ESP_LOGI(kTag,
           "Edge Impulse backend placeholder active: motion=%.3f, classifier not integrated yet",
           static_cast<double>(new_status.motion_score));
  return ESP_OK;
#endif

  dl::image::img_t image = {
      .data = rgb_data,
      .width = width,
      .height = height,
      .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888,
  };

  auto &results = g_detector->run(image);

  float max_score = 0.0f;
  uint32_t detection_count = 0;
  for (const auto &result : results) {
    if (!is_person_detection(result)) {
      continue;
    }

    max_score = std::max(max_score, result.score);
    if (result.score >= APP_PERSON_DETECT_CANDIDATE_THRESHOLD) {
      detection_count++;
    }
  }

  heap_caps_free(rgb_data);

  new_status.person_present = max_score >= APP_PERSON_CLASSIFIER_PRESENT_THRESHOLD;
  new_status.detection_count = detection_count;
  new_status.score = max_score;
  new_status.inference_time_ms = static_cast<uint32_t>((new_status.analyzed_at_us - started_at_us) / 1000);

  if (publish_status) {
    update_status(new_status);
  }
  if (status != nullptr) {
    *status = new_status;
  }
  if (frame_score != nullptr) {
    *frame_score = max_score;
  }

  ESP_LOGI(kTag,
           "Person detect result (%s): motion=%.3f present=%s detections=%u probability=%.3f inference=%u ms",
           backend_name(),
           static_cast<double>(new_status.motion_score),
           new_status.person_present ? "yes" : "no",
           static_cast<unsigned>(new_status.detection_count),
           static_cast<double>(new_status.score),
           static_cast<unsigned>(new_status.inference_time_ms));
  return ESP_OK;
}

}  // namespace

esp_err_t app_person_detect_init() {
  if (g_person_status_mutex == nullptr) {
    g_person_status_mutex = xSemaphoreCreateMutex();
    if (g_person_status_mutex == nullptr) {
      return ESP_ERR_NO_MEM;
    }
  }
  if (g_motion_reference_mutex == nullptr) {
    g_motion_reference_mutex = xSemaphoreCreateMutex();
    if (g_motion_reference_mutex == nullptr) {
      return ESP_ERR_NO_MEM;
    }
  }

  g_status = {};
  g_status.configured = APP_PERSON_DETECT_ENABLED;

#if !APP_PERSON_DETECT_ENABLED
  update_status(g_status);
  return ESP_OK;
#else
  if (!has_valid_model_partition()) {
    update_status(g_status);
    return APP_PERSON_DETECT_MODEL_REQUIRED ? ESP_ERR_NOT_FOUND : ESP_OK;
  }

#if APP_PERSON_DETECT_BACKEND == APP_PERSON_DETECT_BACKEND_EDGE_IMPULSE
  g_status.ready = true;
  g_status.model_loaded = false;
  update_status(g_status);
  ESP_LOGI(kTag, "%s target initialized without embedded classifier yet", backend_name());
  return ESP_OK;
#endif

    #if APP_PERSON_DETECT_BACKEND != APP_PERSON_DETECT_BACKEND_EDGE_IMPULSE
    if (g_detector == nullptr) {
#if APP_PERSON_DETECT_BACKEND == APP_PERSON_DETECT_BACKEND_COCO_320
    auto *detector = new (std::nothrow) COCODetect(COCODetect::YOLO11N_320_S8_V1, true);
#elif APP_PERSON_DETECT_BACKEND == APP_PERSON_DETECT_BACKEND_PEDESTRIAN
    auto *detector = new (std::nothrow) PedestrianDetect(PedestrianDetect::PICO_S8_V1, true);
#endif
    if (detector == nullptr) {
      return ESP_ERR_NO_MEM;
    }
    detector->set_score_thr(APP_PERSON_DETECT_CANDIDATE_THRESHOLD, 0);
    detector->set_nms_thr(APP_PERSON_DETECT_NMS_THRESHOLD, 0);
    if (detector->get_raw_model() == nullptr) {
      update_status(g_status);
      ESP_LOGW(kTag, "%s model not available from component configuration", backend_name());
      delete detector;
      return APP_PERSON_DETECT_MODEL_REQUIRED ? ESP_ERR_NOT_FOUND : ESP_OK;
    }
    g_detector = detector;
  }
  #endif

  g_status.model_loaded = true;
  update_status(g_status);
  ESP_LOGI(kTag,
           "%s initialized (candidate=%.2f present=%.2f nms=%.2f)",
           backend_name(),
           static_cast<double>(APP_PERSON_DETECT_CANDIDATE_THRESHOLD),
           static_cast<double>(APP_PERSON_DETECT_PRESENT_THRESHOLD),
           static_cast<double>(APP_PERSON_DETECT_NMS_THRESHOLD));
  return ESP_OK;
#endif
}

esp_err_t app_person_detect_refresh_reference_from_latest_image() {
  const uint8_t *jpeg_data = nullptr;
  size_t jpeg_size = 0;
  int64_t captured_at_us = 0;
  uint16_t width = 0;
  uint16_t height = 0;

  const esp_err_t lock_err = app_camera_lock_latest_image(
      &jpeg_data, &jpeg_size, &captured_at_us, &width, &height, 1000);
  if (lock_err != ESP_OK) {
    return lock_err;
  }

  const esp_err_t refresh_err = refresh_reference_from_jpeg(jpeg_data, jpeg_size, width, height, captured_at_us);
  app_camera_unlock_latest_image();
  if (refresh_err == ESP_OK) {
    ESP_LOGI(kTag, "Motion reference refreshed from latest image");
  }
  return refresh_err;
}

esp_err_t app_person_detect_get_motion_reference(uint8_t *fingerprint,
                                                 size_t fingerprint_size,
                                                 uint16_t *width,
                                                 uint16_t *height,
                                                 int64_t *updated_at_us,
                                                 bool *valid,
                                                 uint32_t timeout_ms) {
  if (fingerprint == nullptr || fingerprint_size < kMotionGridCellCount || width == nullptr || height == nullptr ||
      updated_at_us == nullptr || valid == nullptr || g_motion_reference_mutex == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }

  if (xSemaphoreTake(g_motion_reference_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  std::copy(g_motion_reference.fingerprint.begin(), g_motion_reference.fingerprint.end(), fingerprint);
  *width = APP_PERSON_MOTION_GRID_WIDTH;
  *height = APP_PERSON_MOTION_GRID_HEIGHT;
  *updated_at_us = g_motion_reference.updated_at_us;
  *valid = g_motion_reference.valid;
  xSemaphoreGive(g_motion_reference_mutex);
  return ESP_OK;
}

esp_err_t app_person_detect_get_motion_debug(app_person_motion_debug_t *debug_info, uint32_t timeout_ms) {
  if (debug_info == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }

  const uint8_t *jpeg_data = nullptr;
  size_t jpeg_size = 0;
  int64_t captured_at_us = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  const esp_err_t lock_err = app_camera_lock_latest_image(
      &jpeg_data, &jpeg_size, &captured_at_us, &width, &height, timeout_ms);
  if (lock_err != ESP_OK) {
    return lock_err;
  }

  if (jpeg_data == nullptr || jpeg_size == 0 || width == 0 || height == 0) {
    app_camera_unlock_latest_image();
    return ESP_ERR_INVALID_STATE;
  }

  const size_t rgb_size = static_cast<size_t>(width) * static_cast<size_t>(height) * 3;
  const uint32_t alloc_caps = MALLOC_CAP_8BIT |
                              (esp_psram_is_initialized() ? MALLOC_CAP_SPIRAM : MALLOC_CAP_DEFAULT);
  auto *rgb_data = static_cast<uint8_t *>(heap_caps_malloc(rgb_size, alloc_caps));
  if (rgb_data == nullptr) {
    app_camera_unlock_latest_image();
    return ESP_ERR_NO_MEM;
  }

  if (!fmt2rgb888(jpeg_data, jpeg_size, PIXFORMAT_JPEG, rgb_data)) {
    heap_caps_free(rgb_data);
    app_camera_unlock_latest_image();
    return ESP_FAIL;
  }

  std::array<uint8_t, kMotionGridCellCount> fingerprint = {};
  build_motion_fingerprint(rgb_data, width, height, fingerprint.data());
  heap_caps_free(rgb_data);
  app_camera_unlock_latest_image();

  float motion_score = 0.0f;
  return fill_motion_debug_from_fingerprint(fingerprint.data(), captured_at_us, debug_info, &motion_score);
}

esp_err_t app_person_detect_analyze_latest_image() {
  return app_person_detect_analyze_latest_image_with_result(nullptr, nullptr);
}

esp_err_t app_person_detect_analyze_latest_image_with_result(app_person_detection_status_t *status,
                                                             float *frame_score) {
#if !APP_PERSON_DETECT_ENABLED
  if (status != nullptr) {
    *status = g_status;
  }
  if (frame_score != nullptr) {
    *frame_score = 0.0f;
  }
  return ESP_OK;
#else
  const uint8_t *jpeg_data = nullptr;
  size_t jpeg_size = 0;
  int64_t captured_at_us = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  const esp_err_t lock_err = app_camera_lock_latest_image(
      &jpeg_data, &jpeg_size, &captured_at_us, &width, &height, 1000);
  if (lock_err != ESP_OK) {
    return lock_err;
  }

  if (jpeg_data == nullptr || jpeg_size == 0 || width == 0 || height == 0) {
    app_camera_unlock_latest_image();
    return ESP_ERR_INVALID_STATE;
  }

  const esp_err_t analyze_err = analyze_jpeg_image(jpeg_data, jpeg_size, width, height, status, frame_score, true);
  app_camera_unlock_latest_image();
  return analyze_err;
#endif
}

esp_err_t app_person_detect_analyze_jpeg_image(const uint8_t *jpeg_data,
                                               size_t jpeg_size,
                                               uint16_t width,
                                               uint16_t height,
                                               app_person_detection_status_t *status,
                                               float *frame_score) {
#if !APP_PERSON_DETECT_ENABLED
  if (status != nullptr) {
    *status = g_status;
  }
  if (frame_score != nullptr) {
    *frame_score = 0.0f;
  }
  return ESP_OK;
#endif

  return analyze_jpeg_image(jpeg_data, jpeg_size, width, height, status, frame_score, false);
}

esp_err_t app_person_detect_get_status(app_person_detection_status_t *status, uint32_t timeout_ms) {
  if (status == nullptr || g_person_status_mutex == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }

  if (xSemaphoreTake(g_person_status_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  *status = g_status;
  xSemaphoreGive(g_person_status_mutex);
  return ESP_OK;
}

esp_err_t app_person_detect_set_status(const app_person_detection_status_t *status) {
  if (status == nullptr || g_person_status_mutex == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }

  update_status(*status);
  return ESP_OK;
}