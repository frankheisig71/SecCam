#include <algorithm>

#include "app_camera.h"
#include "app_capture_uploader.h"
#include "app_config.h"
#include "app_http_server.h"
#include "app_ir_led.h"
#include "app_person_detect.h"
#include "app_wifi_ap.h"
#include "app_wifi_sta.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "app_status_led.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

namespace {

constexpr char kTag[] = "mini_cam";
SemaphoreHandle_t g_capture_mutex = nullptr;
esp_pm_lock_handle_t g_cpu_max_lock = nullptr;
TickType_t g_last_capture_tick = 0;
bool g_trigger_processing = false;
TickType_t g_trigger_block_until_tick = 0;
TickType_t g_last_reference_capture_tick = 0;
#if APP_DATASET_COLLECTOR_ENABLED
TickType_t g_last_idle_dataset_capture_tick = 0;
TickType_t g_motion_dataset_guard_until_tick = 0;
TickType_t g_motion_dataset_burst_guard_until_tick = 0;
uint32_t g_motion_dataset_sequence_count = 0;
#endif

bool should_pause_camera_activity();

struct BufferedCaptureImage {
  uint8_t *data = nullptr;
  size_t size = 0;
  int64_t captured_at_us = 0;
  uint16_t width = 0;
  uint16_t height = 0;
};

void yield_after_heavy_step() {
  vTaskDelay(1);
}

#if APP_WIFI_IS_AP
void setup_preview_task(void *) {
  while (true) {
    if (!app_http_server_is_transmitting_image()) {
      const esp_err_t capture_err = app_camera_capture_now();
      if (capture_err != ESP_OK) {
        ESP_LOGW(kTag, "Setup preview capture failed: %s", esp_err_to_name(capture_err));
      }
    }

    vTaskDelay(pdMS_TO_TICKS(APP_SETUP_PREVIEW_INTERVAL_MS));
  }
}
#endif

bool are_triggers_suppressed() {
  return g_trigger_processing || (xTaskGetTickCount() < g_trigger_block_until_tick);
}

void free_buffered_capture_image(BufferedCaptureImage *image) {
  if (image == nullptr) {
    return;
  }

  app_camera_free_image_copy(image->data);
  *image = {};
}

esp_err_t copy_latest_into_buffer(BufferedCaptureImage *image) {
  if (image == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }

  free_buffered_capture_image(image);
  return app_camera_copy_latest_image(&image->data,
                                      &image->size,
                                      &image->captured_at_us,
                                      &image->width,
                                      &image->height,
                                      1000);
}

esp_err_t perform_trigger_capture(const char *reason) {
  if (g_capture_mutex == nullptr || g_cpu_max_lock == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }

  if (should_pause_camera_activity() || are_triggers_suppressed()) {
    return ESP_ERR_INVALID_STATE;
  }

  if (xSemaphoreTake(g_capture_mutex, pdMS_TO_TICKS(10000)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  if (should_pause_camera_activity() || are_triggers_suppressed()) {
    xSemaphoreGive(g_capture_mutex);
    return ESP_ERR_INVALID_STATE;
  }

  g_trigger_processing = true;
  ESP_LOGI(kTag, "Capture triggered by %s", reason);

  const esp_err_t acquire_err = esp_pm_lock_acquire(g_cpu_max_lock);
  if (acquire_err != ESP_OK) {
    g_trigger_processing = false;
    xSemaphoreGive(g_capture_mutex);
    return acquire_err;
  }
  ESP_LOGI(kTag, "CPU boost active: %d MHz for capture processing", APP_CPU_FREQ_ACTIVE_MHZ);

  // Enable IR LEDs for capture
  app_ir_led_set_enabled(true);
  ESP_LOGI(kTag, "IR-LED enabled for capture");
  
  // Wait before first capture
  vTaskDelay(pdMS_TO_TICKS(APP_IR_LED_PRE_CAPTURE_MS));

  esp_err_t capture_err = ESP_OK;
  esp_err_t detect_err = ESP_OK;
  constexpr uint32_t kAnalyzeFrameCount = APP_CAPTURE_TRIGGER_ANALYZE_FRAMES;
  BufferedCaptureImage recent_images[kAnalyzeFrameCount] = {};
  uint32_t recent_image_count = 0;
  app_person_detection_status_t best_status = {};
  float best_frame_score = -1.0f;
  int best_index = -1;

#if APP_CAPTURE_IR_LED_SKIP_FRAMES_COUNT > 0
  // Adaptive IR-LED mode: skip N frames for sensor adaptation, then save the next one.
  const uint32_t kSkipFrameCount = APP_CAPTURE_IR_LED_SKIP_FRAMES_COUNT;
  const uint32_t kSaveCycles = APP_CAPTURE_TRIGGER_IR_SAVE_CYCLES;
  const uint32_t kFramesPerCycle = kSkipFrameCount + 1;
  const uint32_t kTotalCaptureCount = kFramesPerCycle * kSaveCycles;

  ESP_LOGI(kTag,
           "Adaptive IR-LED capture: %d cycles, skip %d frames then save 1, total %d captures", 
           kSaveCycles,
           kSkipFrameCount,
           kTotalCaptureCount);

  uint32_t frame_count = 0;
  uint32_t save_count = 0;

  for (uint32_t attempt = 0; attempt < kTotalCaptureCount; ++attempt) {
    capture_err = app_camera_capture_now();
    if (capture_err != ESP_OK) {
      break;
    }
    yield_after_heavy_step();

    const bool should_save_frame = ((attempt + 1) % kFramesPerCycle) == 0;
    frame_count++;

    if (should_save_frame) {
      BufferedCaptureImage captured_image = {};
      const esp_err_t copy_err = copy_latest_into_buffer(&captured_image);
      if (copy_err != ESP_OK) {
        capture_err = copy_err;
        break;
      }
      yield_after_heavy_step();

      if (recent_image_count == kAnalyzeFrameCount) {
        free_buffered_capture_image(&recent_images[0]);
        recent_images[0] = recent_images[1];
        recent_images[1] = recent_images[2];
        recent_images[2] = {};
        recent_image_count--;
      }

      recent_images[recent_image_count++] = captured_image;
      save_count++;
      ESP_LOGI(kTag, "Saved cycle %d at overall frame %d", save_count, frame_count);
    } else {
      vTaskDelay(pdMS_TO_TICKS(APP_CAPTURE_IR_LED_SKIP_INTERVAL_MS));
    }
  }
#else
  // Standard capture mode: capture frames with timing delays
  const uint32_t total_capture_count = APP_CAPTURE_TRIGGER_BURST_COUNT + APP_CAPTURE_TRIGGER_EXTRA_FRAMES;
  for (uint32_t attempt = 0; attempt < total_capture_count; ++attempt) {
    capture_err = app_camera_capture_now();
    if (capture_err != ESP_OK) {
      break;
    }
    yield_after_heavy_step();

    BufferedCaptureImage captured_image = {};
    const esp_err_t copy_err = copy_latest_into_buffer(&captured_image);
    if (copy_err != ESP_OK) {
      capture_err = copy_err;
      break;
    }
    yield_after_heavy_step();

    // IR-LED timing: wait after first capture, longer after second
    if (attempt == 0) {
      ESP_LOGI(kTag, "First image captured, waiting %d ms before second image", 
               APP_IR_LED_POST_FIRST_CAPTURE_MS);
      vTaskDelay(pdMS_TO_TICKS(APP_IR_LED_POST_FIRST_CAPTURE_MS));
    } else if (attempt == 1) {
      ESP_LOGI(kTag, "Second image captured, waiting %d ms before final processing", 
               APP_IR_LED_POST_SECOND_CAPTURE_MS);
      vTaskDelay(pdMS_TO_TICKS(APP_IR_LED_POST_SECOND_CAPTURE_MS));
    }

    if (recent_image_count == kAnalyzeFrameCount) {
      free_buffered_capture_image(&recent_images[0]);
      recent_images[0] = recent_images[1];
      recent_images[1] = recent_images[2];
      recent_images[2] = {};
      recent_image_count--;
    }

    recent_images[recent_image_count++] = captured_image;
  }
#endif

  if (capture_err == ESP_OK && recent_image_count > 0) {
    for (uint32_t index = 0; index < recent_image_count; ++index) {
      app_person_detection_status_t candidate_status = {};
      float candidate_score = 0.0f;
      detect_err = app_person_detect_analyze_jpeg_image(recent_images[index].data,
                                                        recent_images[index].size,
                                                        recent_images[index].width,
                                                        recent_images[index].height,
                                                        &candidate_status,
                                                        &candidate_score);
      yield_after_heavy_step();
      if (detect_err != ESP_OK) {
        break;
      }

      if (candidate_score > best_frame_score) {
        best_frame_score = candidate_score;
        best_status = candidate_status;
        best_index = static_cast<int>(index);
      }
    }
  }

  if (capture_err == ESP_OK && detect_err == ESP_OK && best_index >= 0) {
    BufferedCaptureImage &best_image = recent_images[best_index];
    const esp_err_t replace_err = app_camera_replace_latest_image(best_image.data,
                                                                  best_image.size,
                                                                  best_image.captured_at_us,
                                                                  best_image.width,
                                                                  best_image.height,
                                                                  1000);
    if (replace_err != ESP_OK) {
      capture_err = replace_err;
    } else {
      yield_after_heavy_step();
      best_image.data = nullptr;
      best_image.size = 0;
      const esp_err_t status_err = app_person_detect_set_status(&best_status);
      if (status_err != ESP_OK) {
        detect_err = status_err;
      }
    }
  }

  for (uint32_t index = 0; index < recent_image_count; ++index) {
    free_buffered_capture_image(&recent_images[index]);
  }

  // Disable IR LEDs after capture
  app_ir_led_set_enabled(false);
  ESP_LOGI(kTag, "IR-LED disabled after capture");

  g_last_capture_tick = xTaskGetTickCount();
  g_trigger_block_until_tick = g_last_capture_tick + pdMS_TO_TICKS(APP_CAPTURE_TRIGGER_GUARD_MS);
  g_trigger_processing = false;

  const esp_err_t release_err = esp_pm_lock_release(g_cpu_max_lock);
  if (release_err != ESP_OK) {
    ESP_LOGW(kTag, "CPU max-frequency lock release failed: %s", esp_err_to_name(release_err));
  } else {
    ESP_LOGI(kTag, "CPU returned to idle policy: %d MHz minimum", APP_CPU_FREQ_IDLE_MHZ);
  }

  xSemaphoreGive(g_capture_mutex);

  if (detect_err != ESP_OK && detect_err != ESP_ERR_NOT_FOUND && detect_err != ESP_ERR_NOT_SUPPORTED) {
    ESP_LOGW(kTag, "Person detection failed: %s", esp_err_to_name(detect_err));
  }

  return capture_err != ESP_OK ? capture_err : detect_err;
}

esp_err_t refresh_reference_capture() {
  if (g_capture_mutex == nullptr || g_cpu_max_lock == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }

  if (should_pause_camera_activity() || are_triggers_suppressed()) {
    return ESP_ERR_INVALID_STATE;
  }

  if (xSemaphoreTake(g_capture_mutex, pdMS_TO_TICKS(10000)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  if (should_pause_camera_activity() || are_triggers_suppressed()) {
    xSemaphoreGive(g_capture_mutex);
    return ESP_ERR_INVALID_STATE;
  }

  const esp_err_t acquire_err = esp_pm_lock_acquire(g_cpu_max_lock);
  if (acquire_err != ESP_OK) {
    xSemaphoreGive(g_capture_mutex);
    return acquire_err;
  }

  esp_err_t capture_err = app_camera_capture_now();
  if (capture_err == ESP_OK) {
    yield_after_heavy_step();
    capture_err = app_person_detect_refresh_reference_from_latest_image();
  }

  const esp_err_t release_err = esp_pm_lock_release(g_cpu_max_lock);
  if (release_err != ESP_OK) {
    ESP_LOGW(kTag, "CPU max-frequency lock release failed after reference capture: %s", esp_err_to_name(release_err));
  }
  xSemaphoreGive(g_capture_mutex);

  if (capture_err == ESP_OK) {
    g_last_reference_capture_tick = xTaskGetTickCount();
    ESP_LOGI(kTag, "Reference image refreshed after idle interval");
  }

  return capture_err;
}

#if APP_DATASET_COLLECTOR_ENABLED
esp_err_t perform_dataset_capture_and_upload(const char *reason,
                                             bool motion_capture,
                                             uint32_t sequence_index,
                                             uint32_t sequence_size) {
  if (g_capture_mutex == nullptr || g_cpu_max_lock == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }

  if (should_pause_camera_activity()) {
    return ESP_ERR_INVALID_STATE;
  }

  if (xSemaphoreTake(g_capture_mutex, pdMS_TO_TICKS(10000)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  const esp_err_t acquire_err = esp_pm_lock_acquire(g_cpu_max_lock);
  if (acquire_err != ESP_OK) {
    xSemaphoreGive(g_capture_mutex);
    return acquire_err;
  }

  ESP_LOGI(kTag,
           "Dataset capture triggered: mode=%s reason=%s seq=%u/%u",
           motion_capture ? "motion" : "idle",
           reason,
           static_cast<unsigned>(sequence_index),
           static_cast<unsigned>(sequence_size));

  esp_err_t capture_err = app_camera_capture_now();
  if (capture_err == ESP_OK) {
    yield_after_heavy_step();
    capture_err = app_capture_uploader_upload_latest_image(reason, motion_capture, sequence_index, sequence_size);
  }

  const esp_err_t release_err = esp_pm_lock_release(g_cpu_max_lock);
  if (release_err != ESP_OK) {
    ESP_LOGW(kTag, "CPU max-frequency lock release failed: %s", esp_err_to_name(release_err));
  }
  xSemaphoreGive(g_capture_mutex);

  if (capture_err == ESP_OK) {
    g_last_capture_tick = xTaskGetTickCount();
  }
  return capture_err;
}

esp_err_t perform_motion_dataset_batch_capture() {
  const uint32_t remaining_motion_captures =
      APP_DATASET_COLLECTOR_MAX_MOTION_CAPTURES > g_motion_dataset_sequence_count
          ? APP_DATASET_COLLECTOR_MAX_MOTION_CAPTURES - g_motion_dataset_sequence_count
          : 0;
  if (remaining_motion_captures == 0) {
    return ESP_ERR_INVALID_STATE;
  }

  // Enable IR LEDs for motion capture
  app_ir_led_set_enabled(true);
  ESP_LOGI(kTag, "IR-LED enabled for motion capture batch");
  vTaskDelay(pdMS_TO_TICKS(APP_IR_LED_PRE_CAPTURE_MS));

  const uint32_t kSkipFrameCount = APP_CAPTURE_IR_LED_SKIP_FRAMES_COUNT;
  const uint32_t kSaveCycles = APP_CAPTURE_TRIGGER_IR_SAVE_CYCLES;
  const uint32_t batch_capture_count = std::min<uint32_t>(kSaveCycles, remaining_motion_captures);
  esp_err_t batch_err = ESP_OK;
  esp_err_t capture_err = ESP_OK;

  ESP_LOGI(kTag,
           "Motion capture batch: %d cycles, skip %d frames then save 1", 
           batch_capture_count,
           kSkipFrameCount);

  for (uint32_t cycle_index = 0; cycle_index < batch_capture_count; ++cycle_index) {
    const uint32_t sequence_index = g_motion_dataset_sequence_count + 1;
    ESP_LOGI(kTag, "Starting motion cycle %d/%d", cycle_index + 1, batch_capture_count);

    for (uint32_t attempt = 0; attempt < (kSkipFrameCount + 1); ++attempt) {
      capture_err = app_camera_capture_now();
      if (capture_err != ESP_OK) {
        batch_err = capture_err;
        break;
      }
      yield_after_heavy_step();

      if (attempt < kSkipFrameCount) {
        ESP_LOGI(kTag, "Motion cycle %d: skipped frame %d/%d", 
                 cycle_index + 1,
                 attempt + 1,
                 kSkipFrameCount);
        vTaskDelay(pdMS_TO_TICKS(APP_CAPTURE_IR_LED_SKIP_INTERVAL_MS));
        continue;
      }

      batch_err = app_capture_uploader_upload_latest_image(
          "pir_motion",
          true,
          sequence_index,
          APP_DATASET_COLLECTOR_MAX_MOTION_CAPTURES);
      if (batch_err != ESP_OK) {
        break;
      }

      g_motion_dataset_sequence_count = sequence_index;
      ESP_LOGI(kTag, "Motion cycle %d saved image seq=%u/%u", 
               cycle_index + 1,
               sequence_index,
               APP_DATASET_COLLECTOR_MAX_MOTION_CAPTURES);
    }

    if (batch_err != ESP_OK) {
      break;
    }
  }

  // Disable IR LEDs after motion capture
  app_ir_led_set_enabled(false);
  ESP_LOGI(kTag, "IR-LED disabled after motion capture batch");

  const TickType_t guard_start_tick = xTaskGetTickCount();
  g_motion_dataset_guard_until_tick = guard_start_tick + pdMS_TO_TICKS(APP_DATASET_COLLECTOR_MOTION_GUARD_MS);
  if (g_motion_dataset_sequence_count >= APP_DATASET_COLLECTOR_MAX_MOTION_CAPTURES) {
    g_motion_dataset_sequence_count = 0;
    g_motion_dataset_burst_guard_until_tick = 0;
  }

  return batch_err;
}
#endif

bool should_pause_camera_activity() {
  return app_http_server_is_transmitting_image() ||
         (APP_WIFI_MODE == APP_WIFI_MODE_STA && app_wifi_sta_is_busy());
}

esp_err_t configure_power_management() {
  esp_pm_config_t pm_config = {};
  pm_config.max_freq_mhz = APP_CPU_FREQ_ACTIVE_MHZ;
  pm_config.min_freq_mhz = APP_CPU_FREQ_IDLE_MHZ;
  pm_config.light_sleep_enable = false;

  ESP_RETURN_ON_ERROR(esp_pm_configure(&pm_config), kTag, "Power management configure failed");
  ESP_RETURN_ON_ERROR(
      esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "capture_max_freq", &g_cpu_max_lock),
      kTag,
      "CPU max-frequency lock creation failed");
  ESP_LOGI(kTag,
           "Power management configured: idle=%d MHz, capture=%d MHz",
           APP_CPU_FREQ_IDLE_MHZ,
           APP_CPU_FREQ_ACTIVE_MHZ);
  return ESP_OK;
}
#if !APP_DATASET_COLLECTOR_ENABLED
esp_err_t perform_capture(const char *reason, uint32_t capture_count, bool deliver_best_frame) {
  if (g_capture_mutex == nullptr || g_cpu_max_lock == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }

  if (should_pause_camera_activity()) {
    return ESP_ERR_INVALID_STATE;
  }

  if (xSemaphoreTake(g_capture_mutex, pdMS_TO_TICKS(10000)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  ESP_LOGI(kTag, "Capture triggered by %s", reason);
  const esp_err_t acquire_err = esp_pm_lock_acquire(g_cpu_max_lock);
  if (acquire_err != ESP_OK) {
    xSemaphoreGive(g_capture_mutex);
    return acquire_err;
  }
  ESP_LOGI(kTag, "CPU boost active: %d MHz for capture processing", APP_CPU_FREQ_ACTIVE_MHZ);

  esp_err_t detect_err = ESP_OK;
  esp_err_t capture_err = ESP_OK;
  uint8_t *best_image = nullptr;
  size_t best_image_size = 0;
  int64_t best_captured_at_us = 0;
  uint16_t best_width = 0;
  uint16_t best_height = 0;
  float best_frame_score = -1.0f;
  app_person_detection_status_t best_status = {};

  const uint32_t effective_capture_count = capture_count == 0 ? 1 : capture_count;
  for (uint32_t attempt = 0; attempt < effective_capture_count; ++attempt) {
    capture_err = app_camera_capture_now();
    if (capture_err != ESP_OK) {
      break;
    }

    app_person_detection_status_t detection_status = {};
    float frame_score = 0.0f;
    detect_err = app_person_detect_analyze_latest_image_with_result(&detection_status, &frame_score);
    if (detect_err != ESP_OK) {
      break;
    }

    if (!deliver_best_frame || frame_score <= best_frame_score) {
      continue;
    }

    uint8_t *candidate_image = nullptr;
    size_t candidate_size = 0;
    int64_t candidate_captured_at_us = 0;
    uint16_t candidate_width = 0;
    uint16_t candidate_height = 0;
    const esp_err_t copy_err = app_camera_copy_latest_image(&candidate_image,
                                                             &candidate_size,
                                                             &candidate_captured_at_us,
                                                             &candidate_width,
                                                             &candidate_height,
                                                             1000);
    if (copy_err != ESP_OK) {
      capture_err = copy_err;
      break;
    }

    app_camera_free_image_copy(best_image);
    best_image = candidate_image;
    best_image_size = candidate_size;
    best_captured_at_us = candidate_captured_at_us;
    best_width = candidate_width;
    best_height = candidate_height;
    best_frame_score = frame_score;
    best_status = detection_status;
  }

  if (capture_err == ESP_OK && deliver_best_frame && best_image != nullptr) {
    const esp_err_t replace_err = app_camera_replace_latest_image(
        best_image, best_image_size, best_captured_at_us, best_width, best_height, 1000);
    if (replace_err != ESP_OK) {
      app_camera_free_image_copy(best_image);
      capture_err = replace_err;
    } else {
      best_image = nullptr;
      const esp_err_t status_err = app_person_detect_set_status(&best_status);
      if (status_err != ESP_OK) {
        detect_err = status_err;
      }
    }
  }

  app_camera_free_image_copy(best_image);

  g_last_capture_tick = xTaskGetTickCount();
  const esp_err_t release_err = esp_pm_lock_release(g_cpu_max_lock);
  if (release_err != ESP_OK) {
    ESP_LOGW(kTag, "CPU max-frequency lock release failed: %s", esp_err_to_name(release_err));
  } else {
    ESP_LOGI(kTag, "CPU returned to idle policy: %d MHz minimum", APP_CPU_FREQ_IDLE_MHZ);
  }

  xSemaphoreGive(g_capture_mutex);

  if (detect_err != ESP_OK && detect_err != ESP_ERR_NOT_FOUND && detect_err != ESP_ERR_NOT_SUPPORTED) {
    ESP_LOGW(kTag, "Person detection failed: %s", esp_err_to_name(detect_err));
  }

  return capture_err;
}
#endif

#if APP_CAPTURE
#if APP_HTTP_SERVER_ENABLED
esp_err_t capture_now_from_http() {
  if (are_triggers_suppressed()) {
    return ESP_ERR_INVALID_STATE;
  }

#if APP_DATASET_COLLECTOR_ENABLED
  if (g_capture_mutex == nullptr || g_cpu_max_lock == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }

  if (xSemaphoreTake(g_capture_mutex, pdMS_TO_TICKS(10000)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  const esp_err_t acquire_err = esp_pm_lock_acquire(g_cpu_max_lock);
  if (acquire_err != ESP_OK) {
    xSemaphoreGive(g_capture_mutex);
    return acquire_err;
  }

  esp_err_t capture_err = app_camera_capture_now();

  const esp_err_t release_err = esp_pm_lock_release(g_cpu_max_lock);
  if (release_err != ESP_OK) {
    ESP_LOGW(kTag, "CPU max-frequency lock release failed: %s", esp_err_to_name(release_err));
  }
  xSemaphoreGive(g_capture_mutex);

  return capture_err;
#else
  return perform_capture("HTTP request", 1, false);
#endif
}
#endif //APP_HTTP_SERVER_ENABLED
void capture_supervisor_task(void *) {
  gpio_config_t io_config = {};
  io_config.pin_bit_mask = 1ULL << APP_CAPTURE_TRIGGER_GPIO;
  io_config.mode = GPIO_MODE_INPUT;
  io_config.pull_down_en = GPIO_PULLDOWN_ENABLE;
  io_config.pull_up_en = GPIO_PULLUP_DISABLE;
  io_config.intr_type = GPIO_INTR_DISABLE;
  ESP_ERROR_CHECK(gpio_config(&io_config));

  ESP_ERROR_CHECK(app_ir_led_init());
  ESP_ERROR_CHECK(app_status_led_init());

  int last_gpio_level = gpio_get_level(APP_CAPTURE_TRIGGER_GPIO);
  TickType_t last_motion_tick = xTaskGetTickCount();
  app_ir_led_set_enabled(last_gpio_level != 0);

  while (true) {
    const TickType_t now = xTaskGetTickCount();
    const int current_gpio_level = gpio_get_level(APP_CAPTURE_TRIGGER_GPIO);
    app_ir_led_set_enabled(current_gpio_level != 0);

    if (current_gpio_level != 0) {
      last_motion_tick = now;
      // briefly blink status LED blue on PIR rising edge
      if (last_gpio_level == 0) {
        app_status_led_blink_yellow_ms(200);
      }
    }
#if APP_DATASET_COLLECTOR_ENABLED
    if (current_gpio_level == 0 && g_motion_dataset_sequence_count > 0 &&
        (now - last_motion_tick) >= pdMS_TO_TICKS(APP_DATASET_COLLECTOR_MOTION_GUARD_MS)) {
      g_motion_dataset_sequence_count = 0;
    }

    bool should_collect_motion = false;
    bool should_collect_idle = false;
    if (current_gpio_level != 0 && now >= g_motion_dataset_burst_guard_until_tick &&
        now >= g_motion_dataset_guard_until_tick &&
        !app_capture_uploader_should_pause_motion_capture() &&
        g_motion_dataset_sequence_count < APP_DATASET_COLLECTOR_MAX_MOTION_CAPTURES) {
      should_collect_motion = true;
    }
    if (current_gpio_level == 0 && now >= g_last_idle_dataset_capture_tick + pdMS_TO_TICKS(APP_DATASET_COLLECTOR_IDLE_INTERVAL_MS)) {
      should_collect_idle = true;
    }

    if (should_collect_motion) {
      const esp_err_t capture_err = perform_motion_dataset_batch_capture();
      if (capture_err != ESP_OK) {
        ESP_LOGW(kTag, "Dataset motion capture failed: %s", esp_err_to_name(capture_err));
      }
      vTaskDelay(pdMS_TO_TICKS(APP_CAPTURE_TRIGGER_POLL_MS));
      continue;
    }

    if (should_collect_idle) {
      const esp_err_t capture_err = perform_dataset_capture_and_upload("idle_interval", false, 1, 1);
      if (capture_err != ESP_OK) {
        ESP_LOGW(kTag, "Dataset idle capture failed: %s", esp_err_to_name(capture_err));
      } else {
        g_last_idle_dataset_capture_tick = now;
      }
      vTaskDelay(pdMS_TO_TICKS(APP_CAPTURE_TRIGGER_POLL_MS));
      continue;
    }

    vTaskDelay(pdMS_TO_TICKS(APP_CAPTURE_TRIGGER_POLL_MS));
    continue;
#endif

    bool should_capture = false;
    const char *reason = nullptr;
    bool should_refresh_reference = false;

    if (last_gpio_level == 0 && current_gpio_level == 1 &&
        (now - g_last_capture_tick) >= pdMS_TO_TICKS(APP_CAPTURE_TRIGGER_COOLDOWN_MS)) {
      should_capture = true;
      reason = "GPIO rising edge";
    }

    if (!should_capture && current_gpio_level == 0 && (now - last_motion_tick) >= pdMS_TO_TICKS(APP_CAPTURE_REFERENCE_IDLE_MS) &&
        (now - g_last_reference_capture_tick) >= pdMS_TO_TICKS(APP_CAPTURE_REFERENCE_REFRESH_MS)) {
      should_refresh_reference = true;
    }

    last_gpio_level = current_gpio_level;

    if (should_refresh_reference) {
      const esp_err_t reference_err = refresh_reference_capture();
      if (reference_err != ESP_OK && reference_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(kTag, "Reference refresh failed: %s", esp_err_to_name(reference_err));
      }
    }

    if (should_capture) {
      if (should_pause_camera_activity()) {
        vTaskDelay(pdMS_TO_TICKS(APP_CAPTURE_TRIGGER_POLL_MS));
        continue;
      }

      const esp_err_t capture_err = perform_trigger_capture(reason);
      if (capture_err == ESP_ERR_INVALID_STATE && should_pause_camera_activity()) {
        vTaskDelay(pdMS_TO_TICKS(APP_CAPTURE_TRIGGER_POLL_MS));
        continue;
      }

      if (capture_err != ESP_OK) {
        ESP_LOGW(kTag, "Capture failed: %s", esp_err_to_name(capture_err));
      }
    }

    vTaskDelay(pdMS_TO_TICKS(APP_CAPTURE_TRIGGER_POLL_MS));
  }
}
#endif //APP_CAPTURE

esp_err_t start_selected_wifi_mode() {
#if APP_WIFI_MODE == APP_WIFI_MODE_AP
  return app_wifi_ap_start();
#elif APP_WIFI_MODE == APP_WIFI_MODE_STA
  return app_wifi_sta_start();
#else
  return ESP_ERR_NOT_SUPPORTED;
#endif
}

}  // namespace

extern "C" void app_main(void) {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);

  g_capture_mutex = xSemaphoreCreateMutex();
  if (g_capture_mutex == nullptr) {
    ESP_LOGE(kTag, "Failed to create capture mutex");
    abort();
  }
#if APP_SETUP
  ESP_ERROR_CHECK(start_selected_wifi_mode());
  ESP_ERROR_CHECK(app_camera_init());
  ESP_ERROR_CHECK(app_ir_led_init());
  ESP_ERROR_CHECK(configure_power_management());
#if APP_HTTP_SERVER_ENABLED
  ESP_ERROR_CHECK(app_http_server_start(nullptr));
#endif
  const BaseType_t preview_task_ok = xTaskCreate(setup_preview_task, "setup_preview", 4096, nullptr, 5, nullptr);
  if (preview_task_ok != pdPASS) {
    ESP_LOGE(kTag, "Failed to start setup preview task");
    abort();
  }
#else
  ESP_ERROR_CHECK(start_selected_wifi_mode());
  vTaskDelay(pdMS_TO_TICKS(3000));

  ESP_ERROR_CHECK(app_camera_init());
  ESP_ERROR_CHECK(app_person_detect_init());
  ESP_ERROR_CHECK(configure_power_management());
#if APP_DATASET_COLLECTOR_ENABLED
  ESP_ERROR_CHECK(app_capture_uploader_init(g_cpu_max_lock));
#endif
#if APP_CAPTURE_ON_STARTUP
  ESP_ERROR_CHECK(perform_capture("startup", 1, false));
#endif
#if APP_HTTP_SERVER_ENABLED
  ESP_ERROR_CHECK(app_http_server_start(&capture_now_from_http));
#endif

  const BaseType_t task_ok = xTaskCreate(capture_supervisor_task, "capture_supervisor", 4096, nullptr, 5, nullptr);
  if (task_ok != pdPASS) {
    ESP_LOGE(kTag, "Failed to start capture supervisor task");
    abort();
  }
#endif

  ESP_LOGI(kTag, "Application started");
}