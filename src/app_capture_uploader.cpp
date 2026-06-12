#include "app_capture_uploader.h"

#include <cstdio>

#include "app_camera.h"
#include "app_config.h"
#include "esp_http_client.h"
#include "esp_log.h"

namespace {

constexpr char kTag[] = "app_capture_upload";

void set_header_number(esp_http_client_handle_t client, const char *name, unsigned long long value) {
  char buffer[32] = {};
  std::snprintf(buffer, sizeof(buffer), "%llu", value);
  esp_http_client_set_header(client, name, buffer);
}

}  // namespace

esp_err_t app_capture_uploader_upload_latest_image(const char *capture_reason,
                                                   bool motion_capture,
                                                   uint32_t sequence_index,
                                                   uint32_t sequence_size) {
  uint8_t *image_data = nullptr;
  size_t image_size = 0;
  int64_t captured_at_us = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  const esp_err_t copy_err = app_camera_copy_latest_image(
      &image_data, &image_size, &captured_at_us, &width, &height, 1000);
  if (copy_err != ESP_OK) {
    return copy_err;
  }

  esp_http_client_config_t config = {};
  config.url = APP_DATASET_COLLECTOR_UPLOAD_URL;
  config.method = HTTP_METHOD_POST;
  config.timeout_ms = APP_DATASET_COLLECTOR_UPLOAD_TIMEOUT_MS;
  config.disable_auto_redirect = false;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    app_camera_free_image_copy(image_data);
    return ESP_ERR_NO_MEM;
  }

  esp_http_client_set_header(client, "Content-Type", "image/jpeg");
  esp_http_client_set_header(client, "X-Device-Id", APP_DATASET_COLLECTOR_DEVICE_ID);
  esp_http_client_set_header(client, "X-Capture-Reason", capture_reason != nullptr ? capture_reason : "unknown");
  esp_http_client_set_header(client, "X-Capture-Mode", motion_capture ? "motion" : "idle");
  set_header_number(client, "X-Captured-At-Us", static_cast<unsigned long long>(captured_at_us));
  set_header_number(client, "X-Image-Width", width);
  set_header_number(client, "X-Image-Height", height);
  set_header_number(client, "X-Sequence-Index", sequence_index);
  set_header_number(client, "X-Sequence-Size", sequence_size);

  esp_err_t err = esp_http_client_open(client, static_cast<int>(image_size));
  if (err == ESP_OK) {
    const int written = esp_http_client_write(client,
                                              reinterpret_cast<const char *>(image_data),
                                              static_cast<int>(image_size));
    if (written < 0 || static_cast<size_t>(written) != image_size) {
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
             motion_capture ? "motion" : "idle",
             capture_reason != nullptr ? capture_reason : "unknown",
             static_cast<unsigned>(image_size),
             status_code);
  } else {
    ESP_LOGW(kTag,
             "JPEG upload failed: mode=%s reason=%s err=%s status=%d",
             motion_capture ? "motion" : "idle",
             capture_reason != nullptr ? capture_reason : "unknown",
             esp_err_to_name(err),
             status_code);
  }

  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  app_camera_free_image_copy(image_data);
  return err;
}