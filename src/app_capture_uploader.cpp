#include "app_capture_uploader.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "app_camera.h"
#include "app_config.h"
#include "app_wifi_sta.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

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
  bool motion_capture = false;
  uint32_t sequence_index = 1;
  uint32_t sequence_size = 1;
  uint32_t upload_attempt = 0;
};

QueueHandle_t g_upload_queue = nullptr;
TaskHandle_t g_upload_task = nullptr;
std::atomic<uint32_t> g_exhausted_upload_failure_streak{0};
std::atomic<TickType_t> g_motion_capture_pause_until_tick{0};
std::atomic<bool> g_failure_reconnect_armed{true};

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

void note_upload_success() {
  g_exhausted_upload_failure_streak.store(0, std::memory_order_relaxed);
  g_failure_reconnect_armed.store(true, std::memory_order_relaxed);
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
  esp_http_client_set_header(client, "X-Capture-Reason", capture.capture_reason[0] != '\0' ? capture.capture_reason : "unknown");
  esp_http_client_set_header(client, "X-Capture-Mode", capture.motion_capture ? "motion" : "idle");
  set_header_number(client, "X-Captured-At-Us", static_cast<unsigned long long>(capture.captured_at_us));
  set_header_number(client, "X-Image-Width", capture.width);
  set_header_number(client, "X-Image-Height", capture.height);
  set_header_number(client, "X-Sequence-Index", capture.sequence_index);
  set_header_number(client, "X-Sequence-Size", capture.sequence_size);

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

      upload_err = upload_capture_once(capture);
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

esp_err_t app_capture_uploader_init() {
  if (g_upload_queue != nullptr && g_upload_task != nullptr) {
    return ESP_OK;
  }

  if (g_upload_queue == nullptr) {
    g_upload_queue = xQueueCreate(APP_DATASET_COLLECTOR_UPLOAD_QUEUE_DEPTH, sizeof(QueuedCaptureUpload));
    if (g_upload_queue == nullptr) {
      return ESP_ERR_NO_MEM;
    }
  }

  if (g_upload_task == nullptr) {
    const BaseType_t task_ok = xTaskCreate(upload_task,
                                           "capture_uploader",
                                           APP_DATASET_COLLECTOR_UPLOAD_TASK_STACK_SIZE,
                                           nullptr,
                                           APP_DATASET_COLLECTOR_UPLOAD_TASK_PRIORITY,
                                           &g_upload_task);
    if (task_ok != pdPASS) {
      vQueueDelete(g_upload_queue);
      g_upload_queue = nullptr;
      return ESP_ERR_NO_MEM;
    }
  }

  return ESP_OK;
}

esp_err_t app_capture_uploader_upload_latest_image(const char *capture_reason,
                                                   bool motion_capture,
                                                   uint32_t sequence_index,
                                                   uint32_t sequence_size) {
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
           "Queued JPEG for upload: mode=%s reason=%s size=%u seq=%u/%u",
           capture.motion_capture ? "motion" : "idle",
           capture.capture_reason,
           static_cast<unsigned>(capture.image_size),
           static_cast<unsigned>(capture.sequence_index),
           static_cast<unsigned>(capture.sequence_size));
  return ESP_OK;
}

bool app_capture_uploader_should_pause_motion_capture() {
  const TickType_t pause_until_tick = g_motion_capture_pause_until_tick.load(std::memory_order_relaxed);
  if (pause_until_tick == 0) {
    return false;
  }

  const TickType_t now = xTaskGetTickCount();
  if (now < pause_until_tick) {
    return true;
  }

  g_motion_capture_pause_until_tick.store(0, std::memory_order_relaxed);
  g_exhausted_upload_failure_streak.store(0, std::memory_order_relaxed);
  g_failure_reconnect_armed.store(true, std::memory_order_relaxed);
  return false;
}