#include "app_camera.h"

#include <cstdlib>
#include <cstring>

#include "app_config.h"
#include "esp_camera.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace {

constexpr char kTag[] = "app_camera";

struct ImageBuffer {
  uint8_t *data = nullptr;
  size_t size = 0;
  int64_t captured_at_us = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  SemaphoreHandle_t mutex = nullptr;
};

ImageBuffer g_latest_image;

bool apply_sensor_setting(const char *name, int (*setter)(sensor_t *, int), sensor_t *sensor, int value) {
  if (setter == nullptr) {
    return false;
  }

  const int result = setter(sensor, value);
  if (result != 0) {
    ESP_LOGW(kTag, "Sensor setting %s=%d failed (%d)", name, value, result);
    return false;
  }

  return true;
}

void configure_sensor_profile() {
  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor == nullptr) {
    ESP_LOGW(kTag, "Sensor handle not available for camera tuning");
    return;
  }

  apply_sensor_setting("whitebal", sensor->set_whitebal, sensor, 1);
  apply_sensor_setting("awb_gain", sensor->set_awb_gain, sensor, 1);
  apply_sensor_setting("gain_ctrl", sensor->set_gain_ctrl, sensor, 1);
  apply_sensor_setting("exposure_ctrl", sensor->set_exposure_ctrl, sensor, 1);
  apply_sensor_setting("aec2", sensor->set_aec2, sensor, 1);
  apply_sensor_setting("ae_level", sensor->set_ae_level, sensor, APP_CAMERA_AE_LEVEL);
  apply_sensor_setting("brightness", sensor->set_brightness, sensor, APP_CAMERA_BRIGHTNESS);
  apply_sensor_setting("contrast", sensor->set_contrast, sensor, APP_CAMERA_CONTRAST);

  if (sensor->set_gainceiling != nullptr) {
    const int result = sensor->set_gainceiling(sensor, APP_CAMERA_GAINCEILING);
    if (result != 0) {
      ESP_LOGW(kTag, "Sensor setting gainceiling failed (%d)", result);
    }
  }

  ESP_LOGI(kTag,
           "Camera sensor tuned (ae_level=%d, brightness=%d, contrast=%d)",
           APP_CAMERA_AE_LEVEL,
           APP_CAMERA_BRIGHTNESS,
           APP_CAMERA_CONTRAST);
}

void warm_up_sensor() {
  for (int frame_index = 0; frame_index < APP_CAMERA_WARMUP_FRAMES; ++frame_index) {
    camera_fb_t *frame = esp_camera_fb_get();
    if (frame == nullptr) {
      ESP_LOGW(kTag, "Camera warmup frame %d failed", frame_index + 1);
      return;
    }
    esp_camera_fb_return(frame);
  }
}

camera_config_t make_camera_config() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = CAM_PIN_D0;
  config.pin_d1 = CAM_PIN_D1;
  config.pin_d2 = CAM_PIN_D2;
  config.pin_d3 = CAM_PIN_D3;
  config.pin_d4 = CAM_PIN_D4;
  config.pin_d5 = CAM_PIN_D5;
  config.pin_d6 = CAM_PIN_D6;
  config.pin_d7 = CAM_PIN_D7;
  config.pin_xclk = CAM_PIN_XCLK;
  config.pin_pclk = CAM_PIN_PCLK;
  config.pin_vsync = CAM_PIN_VSYNC;
  config.pin_href = CAM_PIN_HREF;
  config.pin_sccb_sda = CAM_PIN_SIOD;
  config.pin_sccb_scl = CAM_PIN_SIOC;
  config.pin_pwdn = CAM_PIN_PWDN;
  config.pin_reset = CAM_PIN_RESET;
  config.xclk_freq_hz = APP_CAMERA_XCLK_HZ;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = esp_psram_is_initialized() ? APP_CAMERA_FRAME_SIZE_PSRAM
                                                 : APP_CAMERA_FRAME_SIZE_NO_PSRAM;
  config.jpeg_quality = APP_CAMERA_JPEG_QUALITY;
  config.fb_count = 1;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = esp_psram_is_initialized() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  return config;
}

esp_err_t capture_into_ram() {
  camera_fb_t *frame = nullptr;
  for (int frame_index = 0; frame_index < APP_CAMERA_TRIGGER_FRAMES; ++frame_index) {
    frame = esp_camera_fb_get();
    if (frame == nullptr) {
      ESP_LOGE(kTag, "Camera capture failed at frame %d/%d", frame_index + 1, APP_CAMERA_TRIGGER_FRAMES);
      return ESP_FAIL;
    }

    if (frame_index + 1 < APP_CAMERA_TRIGGER_FRAMES) {
      esp_camera_fb_return(frame);
      frame = nullptr;
    }
  }

  const size_t frame_len = frame->len;
  const uint32_t alloc_caps = MALLOC_CAP_8BIT |
                              (esp_psram_is_initialized() ? MALLOC_CAP_SPIRAM : MALLOC_CAP_DEFAULT);
  uint8_t *copy = static_cast<uint8_t *>(heap_caps_malloc(frame_len, alloc_caps));
  if (copy == nullptr) {
    esp_camera_fb_return(frame);
    ESP_LOGE(kTag, "Failed to allocate %u bytes for image buffer", static_cast<unsigned>(frame_len));
    return ESP_ERR_NO_MEM;
  }

  std::memcpy(copy, frame->buf, frame_len);
  const uint16_t frame_width = frame->width;
  const uint16_t frame_height = frame->height;
  esp_camera_fb_return(frame);

  if (xSemaphoreTake(g_latest_image.mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    heap_caps_free(copy);
    return ESP_ERR_TIMEOUT;
  }

  heap_caps_free(g_latest_image.data);
  g_latest_image.data = copy;
  g_latest_image.size = frame_len;
  g_latest_image.captured_at_us = esp_timer_get_time();
  g_latest_image.width = frame_width;
  g_latest_image.height = frame_height;
  xSemaphoreGive(g_latest_image.mutex);

  ESP_LOGI(kTag,
           "Stored JPEG in RAM (%u bytes, using frame %d/%d)",
           static_cast<unsigned>(frame_len),
           APP_CAMERA_TRIGGER_FRAMES,
           APP_CAMERA_TRIGGER_FRAMES);
  return ESP_OK;
}

}  // namespace

esp_err_t app_camera_init() {
  if (g_latest_image.mutex == nullptr) {
    g_latest_image.mutex = xSemaphoreCreateMutex();
    if (g_latest_image.mutex == nullptr) {
      ESP_LOGE(kTag, "Failed to create image mutex");
      return ESP_ERR_NO_MEM;
    }
  }

  const camera_config_t config = make_camera_config();
  ESP_RETURN_ON_ERROR(esp_camera_init(&config), kTag, "Camera init failed");
  configure_sensor_profile();
  warm_up_sensor();
  ESP_LOGI(kTag,
           "Camera initialized (psram=%s, frame_size=%d)",
           esp_psram_is_initialized() ? "yes" : "no",
           static_cast<int>(config.frame_size));
  return ESP_OK;
}

esp_err_t app_camera_capture_now() {
  return capture_into_ram();
}

esp_err_t app_camera_lock_latest_image(const uint8_t **data,
                                       size_t *size,
                                       int64_t *captured_at_us,
                                       uint16_t *width,
                                       uint16_t *height,
                                       uint32_t timeout_ms) {
  if (data == nullptr || size == nullptr || captured_at_us == nullptr || g_latest_image.mutex == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }

  if (xSemaphoreTake(g_latest_image.mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  *data = g_latest_image.data;
  *size = g_latest_image.size;
  *captured_at_us = g_latest_image.captured_at_us;
  if (width != nullptr) {
    *width = g_latest_image.width;
  }
  if (height != nullptr) {
    *height = g_latest_image.height;
  }
  return ESP_OK;
}

esp_err_t app_camera_copy_latest_image(uint8_t **data,
                                       size_t *size,
                                       int64_t *captured_at_us,
                                       uint16_t *width,
                                       uint16_t *height,
                                       uint32_t timeout_ms) {
  if (data == nullptr || size == nullptr || captured_at_us == nullptr || g_latest_image.mutex == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }

  *data = nullptr;
  *size = 0;
  *captured_at_us = 0;

  if (xSemaphoreTake(g_latest_image.mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  if (g_latest_image.data == nullptr || g_latest_image.size == 0) {
    if (width != nullptr) {
      *width = g_latest_image.width;
    }
    if (height != nullptr) {
      *height = g_latest_image.height;
    }
    xSemaphoreGive(g_latest_image.mutex);
    return ESP_ERR_NOT_FOUND;
  }

  const uint32_t alloc_caps = MALLOC_CAP_8BIT |
                              (esp_psram_is_initialized() ? MALLOC_CAP_SPIRAM : MALLOC_CAP_DEFAULT);
  uint8_t *copy = static_cast<uint8_t *>(heap_caps_malloc(g_latest_image.size, alloc_caps));
  if (copy == nullptr) {
    xSemaphoreGive(g_latest_image.mutex);
    ESP_LOGE(kTag, "Failed to allocate %u bytes for HTTP image copy", static_cast<unsigned>(g_latest_image.size));
    return ESP_ERR_NO_MEM;
  }

  std::memcpy(copy, g_latest_image.data, g_latest_image.size);
  *data = copy;
  *size = g_latest_image.size;
  *captured_at_us = g_latest_image.captured_at_us;
  if (width != nullptr) {
    *width = g_latest_image.width;
  }
  if (height != nullptr) {
    *height = g_latest_image.height;
  }

  xSemaphoreGive(g_latest_image.mutex);
  return ESP_OK;
}

esp_err_t app_camera_replace_latest_image(uint8_t *data,
                                          size_t size,
                                          int64_t captured_at_us,
                                          uint16_t width,
                                          uint16_t height,
                                          uint32_t timeout_ms) {
  if (data == nullptr || size == 0 || g_latest_image.mutex == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }

  if (xSemaphoreTake(g_latest_image.mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  heap_caps_free(g_latest_image.data);
  g_latest_image.data = data;
  g_latest_image.size = size;
  g_latest_image.captured_at_us = captured_at_us;
  g_latest_image.width = width;
  g_latest_image.height = height;

  xSemaphoreGive(g_latest_image.mutex);
  return ESP_OK;
}

void app_camera_free_image_copy(uint8_t *data) {
  heap_caps_free(data);
}

void app_camera_unlock_latest_image() {
  if (g_latest_image.mutex != nullptr) {
    xSemaphoreGive(g_latest_image.mutex);
  }
}
