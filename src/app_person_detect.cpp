#include "app_person_detect.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <new>
#include <vector>

#include "app_camera.h"
#include "app_config.h"
#include "dl_detect_base.hpp"
#include "dl_image_define.hpp"
#include "app_logging.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "img_converters.h"

#if __has_include("edge-impulse-sdk/classifier/ei_run_classifier.h")
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wuninitialized"
#endif
#include "edge-impulse-sdk/classifier/ei_run_classifier.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#define APP_PERSON_DETECT_HAS_EI_SDK 1
#else
#define APP_PERSON_DETECT_HAS_EI_SDK 0
#endif

namespace {

constexpr char kTag[] = "app_person_detect";
SemaphoreHandle_t g_person_status_mutex = nullptr;
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

float clamp01(float value) {
  if (value < 0.0f) {
    return 0.0f;
  }
  if (value > 1.0f) {
    return 1.0f;
  }
  return value;
}

float approximate_edge_impulse_score(const uint8_t *rgb_data, uint16_t width, uint16_t height) {
  if (rgb_data == nullptr || width == 0 || height == 0) {
    return 0.0f;
  }

  const uint32_t x_start = width / 6U;
  const uint32_t x_end = (width * 5U) / 6U;
  const uint32_t y_start = height / 6U;
  const uint32_t y_end = (height * 5U) / 6U;
  uint64_t sum = 0;
  uint64_t variance_sum = 0;
  uint32_t pixel_count = 0;

  for (uint32_t y = y_start; y < y_end; ++y) {
    for (uint32_t x = x_start; x < x_end; ++x) {
      const size_t pixel_offset = (static_cast<size_t>(y) * width + x) * 3U;
      const uint32_t red = rgb_data[pixel_offset];
      const uint32_t green = rgb_data[pixel_offset + 1U];
      const uint32_t blue = rgb_data[pixel_offset + 2U];
      const uint32_t luminance = ((red * 77U) + (green * 150U) + (blue * 29U)) >> 8U;
      sum += luminance;
      ++pixel_count;
    }
  }

  if (pixel_count == 0) {
    return 0.0f;
  }

  const float mean_luminance = static_cast<float>(sum) / static_cast<float>(pixel_count);
  for (uint32_t y = y_start; y < y_end; ++y) {
    for (uint32_t x = x_start; x < x_end; ++x) {
      const size_t pixel_offset = (static_cast<size_t>(y) * width + x) * 3U;
      const uint32_t red = rgb_data[pixel_offset];
      const uint32_t green = rgb_data[pixel_offset + 1U];
      const uint32_t blue = rgb_data[pixel_offset + 2U];
      const uint32_t luminance = ((red * 77U) + (green * 150U) + (blue * 29U)) >> 8U;
      const float diff = static_cast<float>(static_cast<int32_t>(luminance) - static_cast<int32_t>(mean_luminance));
      variance_sum += static_cast<uint64_t>(std::abs(diff) * 100.0f);
    }
  }

  const float contrast = static_cast<float>(variance_sum) / static_cast<float>(pixel_count * 25500U);
  const float brightness_bias = clamp01((mean_luminance - 60.0f) / 120.0f);
  const float score = clamp01(0.6f * brightness_bias + 0.4f * contrast);
  return score;
}

std::vector<float> resize_rgb_to_model_input(const uint8_t *rgb_data, uint16_t width, uint16_t height) {
#if defined(EI_CLASSIFIER_INPUT_WIDTH) && defined(EI_CLASSIFIER_INPUT_HEIGHT)
  const size_t model_width = static_cast<size_t>(EI_CLASSIFIER_INPUT_WIDTH);
  const size_t model_height = static_cast<size_t>(EI_CLASSIFIER_INPUT_HEIGHT);
  std::vector<float> sample(model_width * model_height, 0.0f);

  if (rgb_data == nullptr || width == 0 || height == 0) {
    return sample;
  }

  const uint32_t src_width = static_cast<uint32_t>(width);
  const uint32_t src_height = static_cast<uint32_t>(height);
  const uint32_t crop_size = std::min(src_width, src_height);
  const uint32_t crop_x = (src_width - crop_size) / 2U;
  const uint32_t crop_y = (src_height - crop_size) / 2U;

  // EI image DSP expects one packed RGB value (0xRRGGBB) per output pixel.
  // For 1024x768 this becomes an exact center crop (768x768) before resize.
  for (size_t y = 0; y < model_height; ++y) {
    if ((y % 8U) == 0U) {
      // Keep the idle task alive during larger preprocessing loops.
      vTaskDelay(pdMS_TO_TICKS(1));
    }
    const uint32_t src_y = crop_y + static_cast<uint32_t>((y * crop_size) / model_height);
    for (size_t x = 0; x < model_width; ++x) {
      const uint32_t src_x = crop_x + static_cast<uint32_t>((x * crop_size) / model_width);
      const size_t src_offset = (static_cast<size_t>(src_y) * static_cast<size_t>(width) + src_x) * 3U;
      const size_t dst_offset = (y * model_width) + x;

      const uint8_t r = rgb_data[src_offset];
      const uint8_t g = rgb_data[src_offset + 1U];
      const uint8_t b = rgb_data[src_offset + 2U];

      const uint32_t packed_rgb = (static_cast<uint32_t>(r) << 16) |
                                  (static_cast<uint32_t>(g) << 8) |
                                  static_cast<uint32_t>(b);
      sample[dst_offset] = static_cast<float>(packed_rgb);
    }
  }

  return sample;
#else
  return {};
#endif
}

enum class EdgeImpulseClassification {
  kPerson,
  kNoPerson,
  kNotSure,
  kOnlyOneClassifier,
};

const char *edge_impulse_classification_name(EdgeImpulseClassification classification) {
  switch (classification) {
    case EdgeImpulseClassification::kPerson:
      return "person";
    case EdgeImpulseClassification::kNoPerson:
      return "no_person";
    case EdgeImpulseClassification::kNotSure:
      return "not_sure";
    case EdgeImpulseClassification::kOnlyOneClassifier:
      return "only_one_classifier";
    default:
      return "unknown";
  }
}

struct EdgeImpulseEvaluation {
  float person_score = 0.0f;
  float no_person_score = 0.0f;
  EdgeImpulseClassification classification = EdgeImpulseClassification::kNotSure;
};

constexpr float kEdgeImpulseDecisionThreshold = APP_PERSON_SCORE_PERSON_MIN;

EdgeImpulseEvaluation evaluate_edge_impulse_result(const uint8_t *rgb_data, uint16_t width, uint16_t height) {
  EdgeImpulseEvaluation evaluation = {};

#if APP_PERSON_DETECT_HAS_EI_SDK
  static bool logged_export_hint = false;
  if (!logged_export_hint) {
    ESP_LOGI(kTag,
              "Edge Impulse SDK detected. The exported model is expected under edge_impulse/edge-impulse-sdk/");
    logged_export_hint = true;
  }

#if defined(EI_CLASSIFIER_INPUT_WIDTH) && defined(EI_CLASSIFIER_INPUT_HEIGHT)
  if (rgb_data != nullptr && width > 0 && height > 0) {
    ESP_LOGI(kTag,
             "EI preprocess: raw=%ux%u (%lu bytes RGB), center-crop=%ux%u, resize=%dx%d",
             static_cast<unsigned>(width),
             static_cast<unsigned>(height),
             static_cast<unsigned long>(static_cast<size_t>(width) * static_cast<size_t>(height) * 3U),
             static_cast<unsigned>(std::min<uint16_t>(width, height)),
             static_cast<unsigned>(std::min<uint16_t>(width, height)),
             EI_CLASSIFIER_INPUT_WIDTH,
             EI_CLASSIFIER_INPUT_HEIGHT);

    const std::vector<float> sample = resize_rgb_to_model_input(rgb_data, width, height);
    if (!sample.empty()) {
#if defined(EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE)
      if (sample.size() != static_cast<size_t>(EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE)) {
        ESP_LOGE(kTag,
                 "EI input size mismatch: sample=%u expected=%u",
                 static_cast<unsigned>(sample.size()),
                 static_cast<unsigned>(EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE));
        return evaluation;
      }
#endif

      ei::signal_t signal = {};
      signal.total_length = sample.size();
      uint32_t get_data_call_count = 0;
      signal.get_data = [&sample, &get_data_call_count](size_t offset, size_t length, float *out_ptr) {
        if (out_ptr == nullptr || offset >= sample.size()) {
          return 0;
        }
        ++get_data_call_count;
        if ((get_data_call_count % 16U) == 0U) {
          // Long EI DSP reads can run for multiple seconds; yield cooperatively.
          vTaskDelay(pdMS_TO_TICKS(1));
        }
        const size_t copy_len = std::min(length, sample.size() - offset);
        std::copy_n(sample.begin() + static_cast<std::ptrdiff_t>(offset), static_cast<std::ptrdiff_t>(copy_len), out_ptr);
        return static_cast<int>(copy_len);
      };

      ei_impulse_result_t result = {};
      if (run_classifier(&signal, &result, false) == EI_IMPULSE_OK) {
        // Model output order is the exported label order. For this model it is
        // effectively ["no_person", "person"], so the last entry is the person
        // confidence and the first entry is the no-person confidence.
        if (EI_CLASSIFIER_LABEL_COUNT > 1U) {
          const size_t person_index = EI_CLASSIFIER_LABEL_COUNT - 1U;
          const size_t no_person_index = 0U;
          evaluation.person_score = clamp01(result.classification[person_index].value);
          evaluation.no_person_score = clamp01(result.classification[no_person_index].value);
          ESP_LOGI(kTag,
            "classification[0]: %s, %f", result.classification[0].label, result.classification[0].value);
          ESP_LOGI(kTag,
            "classification[1]: %s, %f", result.classification[1].label, result.classification[1].value);
          
          if (evaluation.person_score >= kEdgeImpulseDecisionThreshold) {
            evaluation.classification = EdgeImpulseClassification::kPerson;
          } else if (evaluation.no_person_score >= kEdgeImpulseDecisionThreshold) {
            evaluation.classification = EdgeImpulseClassification::kNoPerson;
          } else {
            evaluation.classification = EdgeImpulseClassification::kNotSure;
          }
          return evaluation;
        }

        evaluation.person_score = clamp01(result.classification[0].value);
        evaluation.no_person_score = 0.0f;
        evaluation.classification = EdgeImpulseClassification::kOnlyOneClassifier;
        return evaluation;
      }
    }
  }
#endif
#endif

  evaluation.person_score = approximate_edge_impulse_score(rgb_data, width, height);
  evaluation.no_person_score = 1.0f - evaluation.person_score;
  evaluation.classification = EdgeImpulseClassification::kNotSure;
  return evaluation;
}

void log_edge_impulse_result(float score, EdgeImpulseClassification classification, bool person_present) {
  char evaluation_name[32] = {};
  if (score >= APP_PERSON_SCORE_PERSON_MIN) {
    std::snprintf(evaluation_name,
                  sizeof(evaluation_name),
                  "CAM_%s_%.2f.jpg",
                  person_present ? "person" : "no_person",
                  static_cast<double>(score));
  } else if (score >= APP_PERSON_SCORE_UNSURE_MIN) {
    std::snprintf(evaluation_name,
                  sizeof(evaluation_name),
                  "CAM_unsure+%.2f.jpg",
                  static_cast<double>(score));
  } else {
    std::snprintf(evaluation_name,
                  sizeof(evaluation_name),
                  "CAM_%s_%.2f.jpg",
                  person_present ? "person" : "no_person",
                  static_cast<double>(score));
  }
  ESP_LOGI(kTag,
            "Edge Impulse evaluation: %s -> class=%s person=%s score=%.2f",
            evaluation_name,
            edge_impulse_classification_name(classification),
            person_present ? "yes" : "no",
            static_cast<double>(score));
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

#if APP_PERSON_DETECT_ENABLED
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

  ESP_LOGD("app", "Computing motion score from fingerprint\n");

  *motion_score = 0.0f;
  *reference_ready = false;
  if (xSemaphoreTake(g_motion_reference_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    ESP_LOGD("app", "Failed to acquire motion reference mutex for score computation\n");
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
  
  ESP_LOGD("app", "Motion score computed: reference_ready=%s motion_score=%.3f\n",
                *reference_ready ? "yes" : "no",
                static_cast<double>(*motion_score));

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

  /* JPEG to RGB conversion can be lengthy */
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

  if (jpeg_data == nullptr || jpeg_size == 0 || width == 0 || height == 0) {
    return ESP_ERR_INVALID_STATE;
  }

  const size_t rgb_size = static_cast<size_t>(width) * static_cast<size_t>(height) * 3;
  const uint32_t alloc_caps = MALLOC_CAP_8BIT |
                              (esp_psram_is_initialized() ? MALLOC_CAP_SPIRAM : MALLOC_CAP_DEFAULT);
  auto *rgb_data = static_cast<uint8_t *>(heap_caps_malloc(rgb_size, alloc_caps));
  if (rgb_data == nullptr) {
    ESP_LOGE(kTag, "Failed to allocate RGB buffer for analysis: %ux%u bytes=%zu", static_cast<unsigned>(width), static_cast<unsigned>(height), rgb_size);
    return ESP_ERR_NO_MEM;
  }

  ESP_LOGI(kTag,
          "Starting analysis: jpeg_size=%zu image=%ux%u motion_threshold=%.3f",
          jpeg_size,
          static_cast<unsigned>(width),
          static_cast<unsigned>(height),
          static_cast<double>(APP_PERSON_MOTION_CHANGED_CELL_RATIO));

  /* JPEG to RGB conversion can be lengthy */
  if (!fmt2rgb888(jpeg_data, jpeg_size, PIXFORMAT_JPEG, rgb_data)) {
    ESP_LOGE(kTag, "JPEG->RGB conversion failed: jpeg_size=%zu image=%ux%u", jpeg_size, static_cast<unsigned>(width), static_cast<unsigned>(height));
    heap_caps_free(rgb_data);
    return ESP_FAIL;
  }
  ESP_LOGI(kTag, "JPEG conversion succeeded: %ux%u -> RGB bytes=%zu", static_cast<unsigned>(width), static_cast<unsigned>(height), rgb_size);

  std::array<uint8_t, kMotionGridCellCount> fingerprint = {};
  build_motion_fingerprint(rgb_data, width, height, fingerprint.data());
  ESP_LOGI(kTag, "Motion fingerprint built for image: %u x %u", static_cast<unsigned>(width), static_cast<unsigned>(height));

  float motion_score_value = 0.0f;
  bool reference_ready = false;
  const esp_err_t motion_err = compute_motion_score(fingerprint.data(), &motion_score_value, &reference_ready);
  ESP_LOGI(kTag,
          "Motion scoring result: err=%d reference_ready=%s motion_score=%.4f threshold=%.4f",
          static_cast<int>(motion_err),
          reference_ready ? "yes" : "no",
          static_cast<double>(motion_score_value),
          static_cast<double>(APP_PERSON_MOTION_CHANGED_CELL_RATIO));
  if (motion_err != ESP_OK) {
    ESP_LOGE(kTag, "Motion scoring failed: err=%s", esp_err_to_name(motion_err));
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

  ESP_LOGI(kTag, "Motion gate passed -> running Edge Impulse inference on %ux%u image", static_cast<unsigned>(width), static_cast<unsigned>(height));
  const EdgeImpulseEvaluation edge_result = evaluate_edge_impulse_result(rgb_data, width, height);
  heap_caps_free(rgb_data);

  const float approx_score = edge_result.person_score;
  ESP_LOGI(kTag,
          "Edge Impulse raw score: person=%.4f no_person=%.4f decision=%s",
          static_cast<double>(edge_result.person_score),
          static_cast<double>(edge_result.no_person_score),
          edge_impulse_classification_name(edge_result.classification));

  // Decision logic:
  // 1) Binary model with both classes: require a strong class confidence of 0.75.
  //    - person >= 0.75 => "person"
  //    - no_person >= 0.75 => "no_person"
  //    - everything else => "not_sure"
  // 2) Single-class model: we can only report the single available score and label it
  //    as "only_one_classifier" because there is no competing class to compare.
  const bool classification_is_person = edge_result.classification == EdgeImpulseClassification::kPerson;
  const bool classification_is_no_person = edge_result.classification == EdgeImpulseClassification::kNoPerson;
  new_status.person_present = classification_is_person;
  new_status.detection_count = classification_is_person ? 1U : 0U;
  new_status.score = approx_score;
  new_status.inference_time_ms = 0;

  if (publish_status) {
    update_status(new_status);
  }
  if (status != nullptr) {
    *status = new_status;
  }
  if (frame_score != nullptr) {
    *frame_score = approx_score;
  }

  log_edge_impulse_result(new_status.score, edge_result.classification, new_status.person_present);
  if (classification_is_no_person || edge_result.classification == EdgeImpulseClassification::kNotSure) {
    ESP_LOGI(kTag,
            "Classifier decision: class=%s -> person_present=%s",
            edge_impulse_classification_name(edge_result.classification),
            new_status.person_present ? "yes" : "no");
  }
  return ESP_OK;
}
#endif

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
  g_status.ready = true;
  g_status.model_loaded = false;
  update_status(g_status);
  ESP_LOGI(kTag, "Edge Impulse backend initialized");
  return ESP_OK;
#endif
}

esp_err_t app_person_detect_refresh_reference_from_latest_image() {
#if !APP_PERSON_DETECT_ENABLED
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

  const esp_err_t refresh_err = refresh_reference_from_jpeg(jpeg_data, jpeg_size, width, height, captured_at_us);
  app_camera_unlock_latest_image();
  if (refresh_err == ESP_OK) {
    ESP_LOGI(kTag, "Motion reference refreshed from latest image");
  }
  return refresh_err;
#endif
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

#if !APP_PERSON_DETECT_ENABLED
  (void)timeout_ms;
  return ESP_ERR_NOT_SUPPORTED;
#else

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
#endif
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

ESP_LOGD("app", "app_person_detect_analyze_jpeg_image: Analyzing JPEG image: size=%zu width=%u height=%u\n",
                jpeg_size,
                static_cast<unsigned>(width),
                static_cast<unsigned>(height));
#if APP_PERSON_DETECT_ENABLED
  return analyze_jpeg_image(jpeg_data, jpeg_size, width, height, status, frame_score, false);
#else
  if (status != nullptr) {
    *status = g_status;
  }
  if (frame_score != nullptr) {
    *frame_score = 0.0f;
  }
  return ESP_OK;
#endif
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