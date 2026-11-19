#include "app_capture_uploader.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "app_camera.h"
#include "app_config.h"
#include "app_person_detect.h"
#include "app_wifi_sta.h"
#include "esp_http_client.h"
#include "app_logging.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"

namespace {

constexpr char kTag[] = "app_capture_upload";
constexpr size_t kCaptureReasonCapacity = 32;

struct QueuedCaptureUpload {
  uint8_t *image_data = nullptr;
  size_t image_size = 0;
  int64_t captured_at_us = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  char capture_reason[kCaptureReasonCapacity] = {};
  char file_name[64] = {};
  bool motion_capture = false;
  uint32_t sequence_index = 1;
  uint32_t sequence_size = 1;
  uint32_t upload_attempt = 0;
};

QueueHandle_t g_upload_queue = nullptr;
QueueHandle_t g_analysis_queue = nullptr;
TaskHandle_t g_upload_task = nullptr;
TaskHandle_t g_analysis_task = nullptr;
esp_pm_lock_handle_t g_cpu_max_lock = nullptr;
std::atomic<bool> g_analysis_in_progress{false};
std::atomic<bool> g_analysis_queue_backpressure_active{false};
std::atomic<uint32_t> g_exhausted_upload_failure_streak{0};
std::atomic<TickType_t> g_motion_capture_pause_until_tick{0};
std::atomic<bool> g_failure_reconnect_armed{true};

struct QueuedCaptureAnalysis {
  uint8_t *image_data = nullptr;
  size_t image_size = 0;
  int64_t captured_at_us = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  char capture_reason[kCaptureReasonCapacity] = {};
  bool motion_capture = false;
  uint32_t sequence_index = 1;
  uint32_t sequence_size = 1;
};

void set_header_number(esp_http_client_handle_t client, const char *name, unsigned long long value) {
  char buffer[32] = {};
  std::snprintf(buffer, sizeof(buffer), "%llu", value);
  esp_http_client_set_header(client, name, buffer);
}

void free_queued_capture(QueuedCaptureUpload *capture) {
  if (capture == nullptr) {
    return;
  }

  app_camera_free_image_copy(capture->image_data);
  *capture = {};
}

void free_queued_capture_analysis(QueuedCaptureAnalysis *capture) {
  if (capture == nullptr) {
    return;
  }

  app_camera_free_image_copy(capture->image_data);
  *capture = {};
}

bool requeue_capture_to_tail(QueuedCaptureUpload *capture) {
  if (capture == nullptr || g_upload_queue == nullptr) {
    return false;
  }

  if (xQueueSend(g_upload_queue, capture, 0) != pdTRUE) {
    return false;
  }

  *capture = {};
  return true;
}

void set_capture_file_name(QueuedCaptureUpload *capture,
                           const char *capture_reason,
                           bool person_present,
                           float score) {
  if (capture == nullptr) {
    return;
  }
  // If a score is available (>= 0.0f), use it with the configured
  // confidence threshold to decide labeling. Otherwise fall back to the
  // provided capture reason.
  if (score >= 0.0f) {
    const float person_thresh = APP_PERSON_SCORE_PERSON_MIN;
    const float unsure_min_thresh = APP_PERSON_SCORE_UNSURE_MIN;

    if (score >= person_thresh) {
      const char *label = person_present ? "person" : "no_person";
      std::snprintf(capture->file_name,
                    sizeof(capture->file_name),
                    "CAM_%s_%.2f.jpg",
                    label,
                    static_cast<double>(score));
      return;
    }

    if (score >= unsure_min_thresh) {
      std::snprintf(capture->file_name,
                    sizeof(capture->file_name),
                    "CAM_unsure+%.2f.jpg",
                    static_cast<double>(score));
      return;
    }

    const char *label = person_present ? "person" : "no_person";
    std::snprintf(capture->file_name,
                  sizeof(capture->file_name),
                  "CAM_%s_%.2f.jpg",
                  label,
                  static_cast<double>(score));
    return;
  }

  const char *fallback_reason = capture_reason != nullptr && capture_reason[0] != '\0' ? capture_reason : "unknown";
  std::snprintf(capture->file_name,
                sizeof(capture->file_name),
                "CAM_%s.jpg",
                fallback_reason);
}

void note_upload_success() {
  g_exhausted_upload_failure_streak.store(0, std::memory_order_relaxed);
  g_failure_reconnect_armed.store(true, std::memory_order_relaxed);
}

bool acquire_upload_cpu_boost() {
  if (g_cpu_max_lock == nullptr) {
    return true;
  }

  const esp_err_t err = esp_pm_lock_acquire(g_cpu_max_lock);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "HTTP upload CPU boost acquire failed: %s", esp_err_to_name(err));
    return false;
  }

  return true;
}

void release_upload_cpu_boost() {
  if (g_cpu_max_lock == nullptr) {
    return;
  }

  const esp_err_t err = esp_pm_lock_release(g_cpu_max_lock);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "HTTP upload CPU boost release failed: %s", esp_err_to_name(err));
  }
}

void note_exhausted_upload_failure() {
  const uint32_t failure_streak = g_exhausted_upload_failure_streak.fetch_add(1, std::memory_order_acq_rel) + 1;
  if (failure_streak < APP_DATASET_COLLECTOR_UPLOAD_FAILURE_GUARD_THRESHOLD) {
    return;
  }

  const TickType_t pause_until_tick = xTaskGetTickCount() + pdMS_TO_TICKS(APP_DATASET_COLLECTOR_UPLOAD_FAILURE_GUARD_MS);
  g_motion_capture_pause_until_tick.store(pause_until_tick, std::memory_order_relaxed);
  ESP_LOGE(kTag,
           "Pausing motion capture for %u ms after %u exhausted upload failures",
           static_cast<unsigned>(APP_DATASET_COLLECTOR_UPLOAD_FAILURE_GUARD_MS),
           static_cast<unsigned>(failure_streak));

#if APP_WIFI_MODE == APP_WIFI_MODE_STA
  const bool should_force_reconnect = g_failure_reconnect_armed.exchange(false, std::memory_order_acq_rel);
  if (should_force_reconnect) {
    const esp_err_t reconnect_err = app_wifi_sta_force_reconnect();
    if (reconnect_err != ESP_OK) {
      ESP_LOGW(kTag, "Forced STA reconnect failed after upload failures: %s", esp_err_to_name(reconnect_err));
    }
  }
#endif
}

esp_err_t upload_capture_once(const QueuedCaptureUpload &capture) {
  esp_http_client_config_t config = {};
  config.url = APP_DATASET_COLLECTOR_UPLOAD_URL;
  config.method = HTTP_METHOD_POST;
  config.timeout_ms = APP_DATASET_COLLECTOR_UPLOAD_TIMEOUT_MS;
  config.disable_auto_redirect = false;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    return ESP_ERR_NO_MEM;
  }

  esp_http_client_set_header(client, "Content-Type", "image/jpeg");
  esp_http_client_set_header(client, "Connection", "close");
  esp_http_client_set_header(client, "X-Device-Id", APP_DATASET_COLLECTOR_DEVICE_ID);
  if (capture.file_name[0] != '\0') {
    esp_http_client_set_header(client, "X-Original-Filename", capture.file_name);
  }
  esp_http_client_set_header(client, "X-Capture-Reason", capture.capture_reason[0] != '\0' ? capture.capture_reason : "unknown");
  esp_http_client_set_header(client, "X-Capture-Mode", capture.motion_capture ? "motion" : "idle");
  set_header_number(client, "X-Captured-At-Us", static_cast<unsigned long long>(capture.captured_at_us));
  set_header_number(client, "X-Image-Width", capture.width);
  set_header_number(client, "X-Image-Height", capture.height);
  set_header_number(client, "X-Sequence-Index", capture.sequence_index);
  set_header_number(client, "X-Sequence-Size", capture.sequence_size);

  if (capture.file_name[0] != '\0') {
    APP_LOG_PICTURE_TRANSFER(kTag,
                            capture.file_name,
                            capture.capture_reason[0] != '\0' ? capture.capture_reason : "unknown",
                            capture.image_size,
                            capture.sequence_index,
                            capture.sequence_size);
  }

  esp_err_t err = esp_http_client_open(client, static_cast<int>(capture.image_size));
  if (err == ESP_OK) {
    const int written = esp_http_client_write(client,
                                              reinterpret_cast<const char *>(capture.image_data),
                                              static_cast<int>(capture.image_size));
    if (written < 0 || static_cast<size_t>(written) != capture.image_size) {
      err = ESP_FAIL;
    }
  }

  int status_code = 0;
  if (err == ESP_OK) {
    const int fetch_result = esp_http_client_fetch_headers(client);
    if (fetch_result < 0) {
      err = ESP_FAIL;
    } else {
      status_code = esp_http_client_get_status_code(client);
    }
    if (err == ESP_OK && status_code != 200 && status_code != 201 && status_code != 202 && status_code != 204) {
      err = ESP_FAIL;
    }
  }

  if (err == ESP_OK) {
    ESP_LOGI(kTag,
              "Uploaded JPEG to collector: mode=%s reason=%s size=%u status=%d",
              capture.motion_capture ? "motion" : "idle",
              capture.capture_reason[0] != '\0' ? capture.capture_reason : "unknown",
              static_cast<unsigned>(capture.image_size),
              status_code);
  } else {
    ESP_LOGW(kTag,
              "JPEG upload failed: mode=%s reason=%s err=%s status=%d",
              capture.motion_capture ? "motion" : "idle",
              capture.capture_reason[0] != '\0' ? capture.capture_reason : "unknown",
              esp_err_to_name(err),
              status_code);
  }

  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  return err;
}

void analysis_task(void *) {
  ESP_LOGD("app", "Analysis task started\n");
  while (true) {
    QueuedCaptureAnalysis analysis = {};
    if (xQueueReceive(g_analysis_queue, &analysis, portMAX_DELAY) != pdTRUE) {
      continue;
    }

    g_analysis_in_progress.store(true, std::memory_order_release);
    ESP_LOGD("app", "Dequeued capture for analysis: reason=%s seq=%u/%u\n",
                    analysis.capture_reason[0] != '\0' ? analysis.capture_reason : "unknown",
                    static_cast<unsigned>(analysis.sequence_index),
                    static_cast<unsigned>(analysis.sequence_size));
    taskYIELD();

    ESP_LOGI(kTag,
            "Analysis task starts for queued capture: reason=%s seq=%u/%u size=%zu",
            analysis.capture_reason[0] != '\0' ? analysis.capture_reason : "unknown",
            static_cast<unsigned>(analysis.sequence_index),
            static_cast<unsigned>(analysis.sequence_size),
            analysis.image_size);

    app_person_detection_status_t detection_status = {};
    float frame_score = 0.0f;
    const esp_err_t analysis_err = app_person_detect_analyze_jpeg_image(analysis.image_data,
                                                                        analysis.image_size,
                                                                        analysis.width,
                                                                        analysis.height,
                                                                        &detection_status,
                                                                        &frame_score);
    ESP_LOGI(kTag,
            "Analysis result: err=%s person=%s score=%.4f reason=%s seq=%u/%u",
            esp_err_to_name(analysis_err),
            detection_status.person_present ? "yes" : "no",
            static_cast<double>(frame_score),
            analysis.capture_reason[0] != '\0' ? analysis.capture_reason : "unknown",
            static_cast<unsigned>(analysis.sequence_index),
            static_cast<unsigned>(analysis.sequence_size));
    taskYIELD();
    if (analysis_err != ESP_OK) {
      ESP_LOGW(kTag,
               "Classification failed for queued capture: reason=%s seq=%u/%u err=%s",
               analysis.capture_reason[0] != '\0' ? analysis.capture_reason : "unknown",
               static_cast<unsigned>(analysis.sequence_index),
               static_cast<unsigned>(analysis.sequence_size),
               esp_err_to_name(analysis_err));
      free_queued_capture_analysis(&analysis);
      g_analysis_in_progress.store(false, std::memory_order_release);
      continue;
    }

    const esp_err_t status_err = app_person_detect_set_status(&detection_status);
    if (status_err != ESP_OK) {
      ESP_LOGW(kTag,
               "Status update failed after queued analysis: reason=%s err=%s",
               analysis.capture_reason[0] != '\0' ? analysis.capture_reason : "unknown",
               esp_err_to_name(status_err));
    }

#if APP_DATASET_COLLECTOR_SAVE_BOTH_VARIANTS_FOR_TEST
    QueuedCaptureUpload upload_a = {};
    upload_a.image_data = analysis.image_data;
    upload_a.image_size = analysis.image_size;
    upload_a.captured_at_us = analysis.captured_at_us;
    upload_a.width = analysis.width;
    upload_a.height = analysis.height;
    std::snprintf(upload_a.capture_reason,
                  sizeof(upload_a.capture_reason),
                  "%s",
                  analysis.capture_reason[0] != '\0' ? analysis.capture_reason : "unknown");
    set_capture_file_name(&upload_a, upload_a.capture_reason, detection_status.person_present, frame_score);
    upload_a.motion_capture = analysis.motion_capture;
    upload_a.sequence_index = analysis.sequence_index;
    upload_a.sequence_size = analysis.sequence_size;

    QueuedCaptureUpload upload_b = {};
    const uint32_t alloc_caps = MALLOC_CAP_8BIT | (esp_psram_is_initialized() ? MALLOC_CAP_SPIRAM : MALLOC_CAP_DEFAULT);
    uint8_t *dup = static_cast<uint8_t *>(heap_caps_malloc(analysis.image_size, alloc_caps));
    if (dup != nullptr) {
      std::memcpy(dup, analysis.image_data, analysis.image_size);
      upload_b.image_data = dup;
      upload_b.image_size = analysis.image_size;
      upload_b.captured_at_us = analysis.captured_at_us;
      upload_b.width = analysis.width;
      upload_b.height = analysis.height;
      std::snprintf(upload_b.capture_reason,
                    sizeof(upload_b.capture_reason),
                    "%s",
                    analysis.capture_reason[0] != '\0' ? analysis.capture_reason : "unknown");
      set_capture_file_name(&upload_b, upload_b.capture_reason, !detection_status.person_present, frame_score);
      upload_b.motion_capture = analysis.motion_capture;
      upload_b.sequence_index = analysis.sequence_index;
      upload_b.sequence_size = analysis.sequence_size;
    } else {
      ESP_LOGW(kTag, "Failed to allocate duplicate image for test variant; uploading only primary");
    }

    analysis.image_data = nullptr;

    if (xQueueSend(g_upload_queue, &upload_a, pdMS_TO_TICKS(APP_DATASET_COLLECTOR_UPLOAD_QUEUE_TIMEOUT_MS)) != pdTRUE) {
      ESP_LOGW(kTag,
               "Upload queue full while forwarding analyzed capture: reason=%s seq=%u/%u",
               upload_a.capture_reason,
               static_cast<unsigned>(upload_a.sequence_index),
               static_cast<unsigned>(upload_a.sequence_size));
      free_queued_capture(&upload_a);
      if (upload_b.image_data != nullptr) {
        free_queued_capture(&upload_b);
      }
      free_queued_capture_analysis(&analysis);
      g_analysis_in_progress.store(false, std::memory_order_release);
      continue;
    }

    if (upload_b.image_data != nullptr) {
      if (xQueueSend(g_upload_queue, &upload_b, pdMS_TO_TICKS(APP_DATASET_COLLECTOR_UPLOAD_QUEUE_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(kTag,
                 "Upload queue full while forwarding test-variant capture: reason=%s seq=%u/%u",
                 upload_b.capture_reason,
                 static_cast<unsigned>(upload_b.sequence_index),
                 static_cast<unsigned>(upload_b.sequence_size));
        free_queued_capture(&upload_b);
      }
    }

    ESP_LOGI(kTag,
             "Classified queued capture (test-mode): mode=%s reason=%s filename=%s person=%s score=%.3f seq=%u/%u",
             upload_a.motion_capture ? "motion" : "idle",
             upload_a.capture_reason,
             upload_a.file_name[0] != '\0' ? upload_a.file_name : "(unnamed)",
             detection_status.person_present ? "yes" : "no",
             static_cast<double>(frame_score),
             static_cast<unsigned>(upload_a.sequence_index),
             static_cast<unsigned>(upload_a.sequence_size));

    free_queued_capture_analysis(&analysis);
    g_analysis_in_progress.store(false, std::memory_order_release);
#else
    QueuedCaptureUpload upload = {};
    upload.image_data = analysis.image_data;
    upload.image_size = analysis.image_size;
    upload.captured_at_us = analysis.captured_at_us;
    upload.width = analysis.width;
    upload.height = analysis.height;
    std::snprintf(upload.capture_reason,
                  sizeof(upload.capture_reason),
                  "%s",
                  analysis.capture_reason[0] != '\0' ? analysis.capture_reason : "unknown");
    set_capture_file_name(&upload, upload.capture_reason, detection_status.person_present, frame_score);
    upload.motion_capture = analysis.motion_capture;
    upload.sequence_index = analysis.sequence_index;
    upload.sequence_size = analysis.sequence_size;

    analysis.image_data = nullptr;
    if (xQueueSend(g_upload_queue, &upload, pdMS_TO_TICKS(APP_DATASET_COLLECTOR_UPLOAD_QUEUE_TIMEOUT_MS)) != pdTRUE) {
      ESP_LOGW(kTag,
               "Upload queue full while forwarding analyzed capture: reason=%s seq=%u/%u",
               upload.capture_reason,
               static_cast<unsigned>(upload.sequence_index),
               static_cast<unsigned>(upload.sequence_size));
      free_queued_capture(&upload);
      g_analysis_in_progress.store(false, std::memory_order_release);
      continue;
    }

    ESP_LOGI(kTag,
             "Classified queued capture: mode=%s reason=%s filename=%s person=%s score=%.3f seq=%u/%u",
             upload.motion_capture ? "motion" : "idle",
             upload.capture_reason,
             upload.file_name[0] != '\0' ? upload.file_name : "(unnamed)",
             detection_status.person_present ? "yes" : "no",
             static_cast<double>(frame_score),
             static_cast<unsigned>(upload.sequence_index),
             static_cast<unsigned>(upload.sequence_size));

    free_queued_capture_analysis(&analysis);
    g_analysis_in_progress.store(false, std::memory_order_release);
#endif
  }
}

void upload_task(void *) {
  while (true) {
    QueuedCaptureUpload capture = {};
    if (xQueueReceive(g_upload_queue, &capture, portMAX_DELAY) != pdTRUE) {
      continue;
    }

    esp_err_t upload_err = ESP_FAIL;
    while (capture.upload_attempt < APP_DATASET_COLLECTOR_UPLOAD_RETRY_COUNT) {
      capture.upload_attempt++;

#if APP_WIFI_MODE == APP_WIFI_MODE_STA
      while (app_wifi_sta_is_busy()) {
        ESP_LOGI(kTag, "Uploader waiting for STA reconnect before retrying queued capture");
        vTaskDelay(pdMS_TO_TICKS(1000));
      }
#endif

      if (!acquire_upload_cpu_boost()) {
        upload_err = ESP_FAIL;
        break;
      }

      upload_err = upload_capture_once(capture);
      release_upload_cpu_boost();
      if (upload_err == ESP_OK) {
        note_upload_success();
        break;
      }

      if (capture.upload_attempt >= APP_DATASET_COLLECTOR_UPLOAD_RETRY_COUNT) {
        break;
      }

#if !APP_DATASET_COLLECTOR_UPLOAD_RETRY_WITH_BACKLOG
      const UBaseType_t backlog_count = g_upload_queue != nullptr ? uxQueueMessagesWaiting(g_upload_queue) : 0;
      if (backlog_count > 0) {
        if (requeue_capture_to_tail(&capture)) {
          ESP_LOGW(kTag,
                   "Deferring failed upload to queue tail after attempt %u/%u because %u newer uploads are waiting",
                   static_cast<unsigned>(capture.upload_attempt + 1),
                   static_cast<unsigned>(APP_DATASET_COLLECTOR_UPLOAD_RETRY_COUNT),
                   static_cast<unsigned>(backlog_count));
          upload_err = ESP_OK;
        } else {
          ESP_LOGW(kTag,
                   "Failed to requeue upload after attempt %u/%u despite backlog=%u; dropping stale capture",
                   static_cast<unsigned>(capture.upload_attempt),
                   static_cast<unsigned>(APP_DATASET_COLLECTOR_UPLOAD_RETRY_COUNT),
                   static_cast<unsigned>(backlog_count));
        }
        break;
      }
#endif

      if (capture.upload_attempt < APP_DATASET_COLLECTOR_UPLOAD_RETRY_COUNT) {
        ESP_LOGW(kTag,
                 "Retrying queued upload attempt %u/%u in %u ms",
                 static_cast<unsigned>(capture.upload_attempt + 1),
                 static_cast<unsigned>(APP_DATASET_COLLECTOR_UPLOAD_RETRY_COUNT),
                 static_cast<unsigned>(APP_DATASET_COLLECTOR_UPLOAD_RETRY_DELAY_MS));
        vTaskDelay(pdMS_TO_TICKS(APP_DATASET_COLLECTOR_UPLOAD_RETRY_DELAY_MS));
      }
    }

    if (capture.image_data != nullptr && upload_err != ESP_OK) {
      note_exhausted_upload_failure();
      ESP_LOGE(kTag,
               "Dropping queued capture after %u failed upload attempts: reason=%s captured_at=%lld",
               static_cast<unsigned>(capture.upload_attempt),
               capture.capture_reason[0] != '\0' ? capture.capture_reason : "unknown",
               static_cast<long long>(capture.captured_at_us));
    }

    if (capture.image_data != nullptr) {
      free_queued_capture(&capture);
    }
  }
}

}  // namespace

esp_err_t app_capture_uploader_init(esp_pm_lock_handle_t cpu_max_lock) {
  if (g_upload_queue != nullptr && g_upload_task != nullptr && g_analysis_queue != nullptr && g_analysis_task != nullptr) {
    return ESP_OK;
  }

  g_cpu_max_lock = cpu_max_lock;

  if (g_analysis_queue == nullptr) {
    g_analysis_queue = xQueueCreate(APP_DATASET_COLLECTOR_ANALYSIS_QUEUE_DEPTH, sizeof(QueuedCaptureAnalysis));
    if (g_analysis_queue == nullptr) {
      return ESP_ERR_NO_MEM;
    }
  }

  if (g_analysis_task == nullptr) {
    const BaseType_t task_ok = xTaskCreate(analysis_task,
                                           "capture_analysis",
                                           APP_DATASET_COLLECTOR_UPLOAD_TASK_STACK_SIZE,
                                           nullptr,
                                           APP_DATASET_COLLECTOR_UPLOAD_TASK_PRIORITY,
                                           &g_analysis_task);
    if (task_ok != pdPASS) {
      vQueueDelete(g_analysis_queue);
      g_analysis_queue = nullptr;
      return ESP_ERR_NO_MEM;
    }
    /* Note: analysis_task is not registered with Task Watchdog because it waits
       on a queue with portMAX_DELAY, which is not a blocking condition but rather
       a normal producer-consumer pattern. The Task Watchdog is designed for
       workers that should always be making progress, not event waiters. */
  }

  if (g_upload_queue == nullptr) {
    g_upload_queue = xQueueCreate(APP_DATASET_COLLECTOR_UPLOAD_QUEUE_DEPTH, sizeof(QueuedCaptureUpload));
    if (g_upload_queue == nullptr) {
      vQueueDelete(g_analysis_queue);
      g_analysis_queue = nullptr;
      return ESP_ERR_NO_MEM;
    }
  }

  if (g_upload_task == nullptr) {
    const BaseType_t task_ok = xTaskCreatePinnedToCore(upload_task,
                                                     "capture_uploader",
                                                     APP_DATASET_COLLECTOR_UPLOAD_TASK_STACK_SIZE,
                                                     nullptr,
                                                     APP_DATASET_COLLECTOR_UPLOAD_TASK_PRIORITY,
                                                     &g_upload_task,
                                                     1);
    if (task_ok != pdPASS) {
      vQueueDelete(g_upload_queue);
      g_upload_queue = nullptr;
      vQueueDelete(g_analysis_queue);
      g_analysis_queue = nullptr;
      return ESP_ERR_NO_MEM;
    }
    /* Note: upload_task is not registered with Task Watchdog because it waits
       on a queue with portMAX_DELAY, which is not a blocking condition but rather
       a normal producer-consumer pattern. The Task Watchdog is designed for
       workers that should always be making progress, not event waiters. */
  }

  return ESP_OK;
}

esp_err_t app_capture_uploader_enqueue_analysis_image(const char *capture_reason,
                                                      bool motion_capture,
                                                      uint32_t sequence_index,
                                                      uint32_t sequence_size) {
  if (g_analysis_queue == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }

  const UBaseType_t pending_analysis_count = uxQueueMessagesWaiting(g_analysis_queue);
  const UBaseType_t in_flight_count = pending_analysis_count +
                                      (g_analysis_in_progress.load(std::memory_order_acquire) ? 1U : 0U);
  if (in_flight_count >= APP_DATASET_COLLECTOR_ANALYSIS_QUEUE_DEPTH) {
    ESP_LOGW(kTag,
             "Skipping queued analysis because the analysis queue is already full: reason=%s seq=%u/%u backlog=%u limit=%u",
             capture_reason != nullptr ? capture_reason : "unknown",
             static_cast<unsigned>(sequence_index),
             static_cast<unsigned>(sequence_size),
             static_cast<unsigned>(in_flight_count),
             static_cast<unsigned>(APP_DATASET_COLLECTOR_ANALYSIS_QUEUE_DEPTH));
    g_analysis_queue_backpressure_active.store(true, std::memory_order_release);
    return ESP_ERR_TIMEOUT;
  }

  QueuedCaptureAnalysis analysis = {};
  const esp_err_t copy_err = app_camera_copy_latest_image(&analysis.image_data,
                                                          &analysis.image_size,
                                                          &analysis.captured_at_us,
                                                          &analysis.width,
                                                          &analysis.height,
                                                          1000);
  if (copy_err != ESP_OK) {
    return copy_err;
  }

  std::snprintf(analysis.capture_reason,
                sizeof(analysis.capture_reason),
                "%s",
                capture_reason != nullptr ? capture_reason : "unknown");
  analysis.motion_capture = motion_capture;
  analysis.sequence_index = sequence_index;
  analysis.sequence_size = sequence_size;

  if (xQueueSend(g_analysis_queue, &analysis, pdMS_TO_TICKS(APP_DATASET_COLLECTOR_UPLOAD_QUEUE_TIMEOUT_MS)) != pdTRUE) {
    ESP_LOGW(kTag,
             "Analysis queue full, dropping capture for classification: reason=%s seq=%u/%u",
             analysis.capture_reason,
             static_cast<unsigned>(analysis.sequence_index),
             static_cast<unsigned>(analysis.sequence_size));
    free_queued_capture_analysis(&analysis);
    return ESP_ERR_TIMEOUT;
  }

  ESP_LOGI(kTag,
           "Queued image for analysis: mode=%s reason=%s size=%u seq=%u/%u",
           motion_capture ? "motion" : "idle",
           analysis.capture_reason,
           static_cast<unsigned>(analysis.image_size),
           static_cast<unsigned>(analysis.sequence_index),
           static_cast<unsigned>(analysis.sequence_size));
  return ESP_OK;
}

esp_err_t app_capture_uploader_upload_latest_image(const char *capture_reason,
                                                   bool motion_capture,
                                                   uint32_t sequence_index,
                                                   uint32_t sequence_size,
                                                   bool person_present,
                                                   float score) {
  if (g_upload_queue == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }

  QueuedCaptureUpload capture = {};
  const esp_err_t copy_err = app_camera_copy_latest_image(&capture.image_data,
                                                          &capture.image_size,
                                                          &capture.captured_at_us,
                                                          &capture.width,
                                                          &capture.height,
                                                          1000);
  if (copy_err != ESP_OK) {
    return copy_err;
  }

  std::snprintf(capture.capture_reason,
                sizeof(capture.capture_reason),
                "%s",
                capture_reason != nullptr ? capture_reason : "unknown");
  set_capture_file_name(&capture, capture.capture_reason, person_present, score);

  capture.motion_capture = motion_capture;
  capture.sequence_index = sequence_index;
  capture.sequence_size = sequence_size;

  if (xQueueSend(g_upload_queue, &capture, pdMS_TO_TICKS(APP_DATASET_COLLECTOR_UPLOAD_QUEUE_TIMEOUT_MS)) != pdTRUE) {
    ESP_LOGW(kTag,
             "Upload queue full, dropping new capture: reason=%s seq=%u/%u backlog=%u",
             capture.capture_reason,
             static_cast<unsigned>(capture.sequence_index),
             static_cast<unsigned>(capture.sequence_size),
             static_cast<unsigned>(g_upload_queue != nullptr ? uxQueueMessagesWaiting(g_upload_queue) : 0));
    free_queued_capture(&capture);
    return ESP_ERR_TIMEOUT;
  }

  ESP_LOGI(kTag,
           "Queued JPEG for upload: mode=%s reason=%s filename=%s size=%u seq=%u/%u",
           capture.motion_capture ? "motion" : "idle",
           capture.capture_reason,
           capture.file_name[0] != '\0' ? capture.file_name : "(unnamed)",
           static_cast<unsigned>(capture.image_size),
           static_cast<unsigned>(capture.sequence_index),
           static_cast<unsigned>(capture.sequence_size));
  return ESP_OK;
}

bool app_capture_uploader_should_pause_motion_capture() {
  bool should_pause = false;

  const TickType_t pause_until_tick = g_motion_capture_pause_until_tick.load(std::memory_order_relaxed);
  if (pause_until_tick != 0) {
    const TickType_t now = xTaskGetTickCount();
    if (now < pause_until_tick) {
      should_pause = true;
    } else {
      g_motion_capture_pause_until_tick.store(0, std::memory_order_relaxed);
      g_exhausted_upload_failure_streak.store(0, std::memory_order_relaxed);
      g_failure_reconnect_armed.store(true, std::memory_order_relaxed);
    }
  }

  if (g_analysis_queue != nullptr) {
    const UBaseType_t pending_count = uxQueueMessagesWaiting(g_analysis_queue);
    const UBaseType_t in_flight_count = pending_count +
                                        (g_analysis_in_progress.load(std::memory_order_acquire) ? 1U : 0U);
    const UBaseType_t queue_depth = static_cast<UBaseType_t>(APP_DATASET_COLLECTOR_ANALYSIS_QUEUE_DEPTH);
    const UBaseType_t free_slots = in_flight_count >= queue_depth ? 0U : (queue_depth - in_flight_count);
    const UBaseType_t resume_free_slots = std::min<UBaseType_t>(
        static_cast<UBaseType_t>(APP_DATASET_COLLECTOR_ANALYSIS_QUEUE_RESUME_FREE_SLOTS),
        queue_depth);

    const bool currently_paused = g_analysis_queue_backpressure_active.load(std::memory_order_acquire);
    if (currently_paused) {
      if (free_slots >= resume_free_slots) {
        g_analysis_queue_backpressure_active.store(false, std::memory_order_release);
      } else {
        should_pause = true;
      }
    } else if (free_slots == 0U) {
      g_analysis_queue_backpressure_active.store(true, std::memory_order_release);
      should_pause = true;
    }
  }

  return should_pause;
}