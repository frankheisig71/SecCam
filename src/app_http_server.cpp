#include "app_http_server.h"

#include <array>
#include <cstdio>

#include "app_camera.h"
#include "app_config.h"
#include "app_person_detect.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_pm.h"

#include <atomic>

namespace {

constexpr size_t kMotionCellCount = static_cast<size_t>(APP_PERSON_MOTION_GRID_WIDTH) * APP_PERSON_MOTION_GRID_HEIGHT;

app_http_capture_request_fn g_capture_request_fn = nullptr;
std::atomic<uint32_t> g_active_image_transfers{0};
esp_pm_lock_handle_t g_http_transfer_cpu_lock = nullptr;

esp_err_t append_motion_array_json(char *payload, size_t payload_size, size_t *offset, const uint8_t *values, size_t count) {
  if (payload == nullptr || offset == nullptr || values == nullptr || *offset >= payload_size) {
    return ESP_ERR_INVALID_ARG;
  }

  for (size_t index = 0; index < count; ++index) {
    const int written = std::snprintf(payload + *offset,
                                      payload_size - *offset,
                                      "%s%u",
                                      index == 0 ? "" : ",",
                                      static_cast<unsigned>(values[index]));
    if (written < 0 || static_cast<size_t>(written) >= (payload_size - *offset)) {
      return ESP_ERR_NO_MEM;
    }
    *offset += static_cast<size_t>(written);
  }

  return ESP_OK;
}

esp_err_t begin_image_transfer() {
  g_active_image_transfers.fetch_add(1, std::memory_order_relaxed);
  if (g_http_transfer_cpu_lock == nullptr) {
    return ESP_OK;
  }

  const esp_err_t err = esp_pm_lock_acquire(g_http_transfer_cpu_lock);
  if (err != ESP_OK) {
    g_active_image_transfers.fetch_sub(1, std::memory_order_relaxed);
    return err;
  }

  return ESP_OK;
}

void end_image_transfer() {
  if (g_http_transfer_cpu_lock != nullptr) {
    const esp_err_t err = esp_pm_lock_release(g_http_transfer_cpu_lock);
    if (err != ESP_OK) {
      ESP_LOGW("app_http_server", "HTTP transfer CPU lock release failed: %s", esp_err_to_name(err));
    }
  }

  g_active_image_transfers.fetch_sub(1, std::memory_order_relaxed);
}

// Serves the small built-in browser UI for manual capture and image preview.
esp_err_t root_get_handler(httpd_req_t *req) {
  static constexpr char kPage[] = R"HTML(
<!doctype html>
<html lang="de">
  <head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width,initial-scale=1">
    <title>GooUuuu CAM</title>
    <style>
      :root { color-scheme: light; font-family: Arial, sans-serif; }
      body { margin: 0; padding: 24px; background: #f2f5f7; color: #173042; }
      main { max-width: 960px; margin: 0 auto; }
      h1 { margin-bottom: 8px; }
      .card { background: #fff; border-radius: 14px; padding: 16px; box-shadow: 0 12px 30px rgba(0,0,0,.08); }
      .grid { display: grid; grid-template-columns: 1.2fr 1fr; gap: 16px; }
      .preview-grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 12px; }
      button { border: 0; border-radius: 999px; padding: 12px 18px; background: #0d7a5f; color: #fff; cursor: pointer; }
      img { width: 100%; margin-top: 16px; border-radius: 10px; background: #d8e1e7; min-height: 240px; object-fit: contain; }
      canvas { width: 100%; margin-top: 16px; border-radius: 10px; background: #d8e1e7; min-height: 180px; image-rendering: pixelated; }
      #meta, #metrics { margin-top: 12px; color: #4b6473; white-space: pre-wrap; }
      .caption { margin-top: 8px; font-size: 12px; color: #4b6473; text-align: center; }
      @media (max-width: 780px) { .grid { grid-template-columns: 1fr; } }
    </style>
  </head>
  <body>
    <main>
      <h1>GooUuuu CAM</h1>
      <div class="card">
        <p>Das letzte Bild wird alle 60 Sekunden aufgenommen und im RAM des Moduls gehalten.</p>
        <button type="button" onclick="captureNow()">Sofortaufnahme</button>
        <button type="button" onclick="reloadImage()">Aktuelles RAM-Bild laden</button>
        <div id="meta">Lade Status...</div>
        <div id="metrics">Messwerte werden geladen...</div>
        <div class="grid">
          <div>
            <img id="frame" alt="Letztes Kamerabild" />
          </div>
          <div>
            <div class="preview-grid">
              <div>
                <canvas id="reference" width="16" height="12" aria-label="Referenzbild Vorschau"></canvas>
                <div class="caption">Referenz</div>
              </div>
              <div>
                <canvas id="current-grid" width="16" height="12" aria-label="Aktuelles Raster"></canvas>
                <div class="caption">Aktuell</div>
              </div>
              <div>
                <canvas id="diff-grid" width="16" height="12" aria-label="Differenzbild"></canvas>
                <div class="caption">Diff</div>
              </div>
            </div>
            <canvas id="mask-grid" width="16" height="12" aria-label="Geaenderte Zellen"></canvas>
            <div class="caption">Schwellwert-Maske</div>
          </div>
        </div>
      </div>
    </main>
    <script>
      async function fetchStatus() {
        const res = await fetch('/status');
        return res.json();
      }
      async function fetchReference() {
        const res = await fetch('/reference');
        return res.json();
      }
      async function fetchMotionDebug() {
        const res = await fetch('/motion-debug');
        return res.json();
      }
      async function waitForFreshCapture(previousCapturedAtMs) {
        for (let attempt = 0; attempt < 20; attempt += 1) {
          const data = await fetchStatus();
          if ((data.captured_at_ms || 0) > previousCapturedAtMs) {
            return data;
          }
          await new Promise((resolve) => setTimeout(resolve, 200));
        }
        return fetchStatus();
      }
      function drawGrid(canvasId, width, height, pixels, palette = 'gray') {
        const canvas = document.getElementById(canvasId);
        const context = canvas.getContext('2d');
        if (!pixels || !pixels.length || width === 0 || height === 0) {
          context.clearRect(0, 0, canvas.width, canvas.height);
          return;
        }

        canvas.width = width;
        canvas.height = height;
        const image = context.createImageData(width, height);
        for (let index = 0; index < pixels.length; index += 1) {
          const level = pixels[index] || 0;
          const offset = index * 4;
          if (palette === 'heat') {
            image.data[offset] = level;
            image.data[offset + 1] = Math.max(0, 180 - level);
            image.data[offset + 2] = 0;
          } else if (palette === 'mask') {
            image.data[offset] = level > 0 ? 255 : 40;
            image.data[offset + 1] = level > 0 ? 80 : 40;
            image.data[offset + 2] = level > 0 ? 80 : 40;
          } else {
            image.data[offset] = level;
            image.data[offset + 1] = level;
            image.data[offset + 2] = level;
          }
          image.data[offset + 3] = 255;
        }
        context.putImageData(image, 0, 0);
      }
      async function updateStatus() {
        const [data, reference, debug] = await Promise.all([fetchStatus(), fetchReference(), fetchMotionDebug()]);
        const stamp = data.captured_at_ms > 0 ? new Date(data.captured_at_ms).toLocaleString() : 'noch kein Bild';
        const referenceStamp = data.person_reference_updated_at_ms > 0
          ? new Date(data.person_reference_updated_at_ms).toLocaleString()
          : 'keine Referenz';
        let person = 'Person: deaktiviert';
        if (data.person_configured && !data.person_model_loaded) {
          person = 'Person: Modell fehlt oder nicht geladen';
        } else if (data.person_ready) {
          const motion = data.person_reference_ready
            ? `${(data.person_motion_score * 100).toFixed(1)}% Aenderung`
            : 'keine Referenz';
          person = `Person: ${data.person_present} (${(data.person_score * 100).toFixed(1)}%), Bewegung: ${motion}`;
        } else if (data.person_configured) {
          person = 'Person: Analyse noch nicht bereit';
        }
        document.getElementById('meta').textContent = `Bild vorhanden: ${data.has_image} | Bytes: ${data.size} | Zeit: ${stamp} | ${person}`;
        document.getElementById('metrics').textContent =
          `motion_score=${data.person_motion_score.toFixed(3)}\n` +
          `motion_detected=${data.person_motion_detected}\n` +
          `changed_ratio_threshold=${data.person_motion_ratio_threshold.toFixed(3)}\n` +
          `cell_diff_threshold=${data.person_motion_cell_diff_threshold}\n` +
          `reference=${referenceStamp} (${data.person_reference_width}x${data.person_reference_height})\n` +
          `inference_ms=${data.person_inference_ms}`;
        drawGrid('reference', reference.width, reference.height, reference.pixels, 'gray');
        drawGrid('current-grid', debug.width, debug.height, debug.current, 'gray');
        drawGrid('diff-grid', debug.width, debug.height, debug.diff, 'heat');
        drawGrid('mask-grid', debug.width, debug.height, debug.changed, 'mask');
        return data;
      }
      async function reloadImage() {
        await updateStatus();
        document.getElementById('frame').src = `/image.jpg?t=${Date.now()}`;
      }
      async function captureNow() {
        const before = await fetchStatus();
        const res = await fetch('/capture', { method: 'POST' });
        const data = await res.json();
        document.getElementById('meta').textContent = data.message;
        if (data.ok) {
          await waitForFreshCapture(before.captured_at_ms || 0);
          await updateStatus();
          document.getElementById('frame').src = `/image.jpg?t=${Date.now()}`;
        }
      }
      reloadImage();
      setInterval(updateStatus, 5000);
    </script>
  </body>
</html>
)HTML";

  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, kPage, HTTPD_RESP_USE_STRLEN);
}

// Returns lightweight metadata for the currently buffered RAM image.
esp_err_t status_get_handler(httpd_req_t *req) {
  char payload[1536] = {};
  const uint8_t *data = nullptr;
  size_t size = 0;
  int64_t captured_at_us = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  app_person_detection_status_t person_status = {};

  if (app_camera_lock_latest_image(&data, &size, &captured_at_us, &width, &height, 1000) == ESP_OK) {
    app_camera_unlock_latest_image();
  }

  app_person_detect_get_status(&person_status, 1000);

  std::snprintf(payload,
                sizeof(payload),
                "{\"has_image\":%s,\"size\":%u,\"width\":%u,\"height\":%u,\"captured_at_ms\":%lld,\"person_configured\":%s,\"person_model_loaded\":%s,\"person_ready\":%s,\"person_reference_ready\":%s,\"person_motion_detected\":%s,\"person_present\":%s,\"person_detections\":%u,\"person_motion_score\":%.3f,\"person_motion_ratio_threshold\":%.3f,\"person_motion_cell_diff_threshold\":%u,\"person_reference_width\":%u,\"person_reference_height\":%u,\"person_score\":%.3f,\"person_reference_updated_at_ms\":%lld,\"person_analyzed_at_ms\":%lld,\"person_inference_ms\":%u}",
                size > 0 ? "true" : "false",
                static_cast<unsigned>(size),
                static_cast<unsigned>(width),
                static_cast<unsigned>(height),
                static_cast<long long>(captured_at_us / 1000),
                person_status.configured ? "true" : "false",
                person_status.model_loaded ? "true" : "false",
                person_status.ready ? "true" : "false",
                person_status.reference_ready ? "true" : "false",
                person_status.motion_detected ? "true" : "false",
                person_status.person_present ? "true" : "false",
                static_cast<unsigned>(person_status.detection_count),
                static_cast<double>(person_status.motion_score),
                static_cast<double>(APP_PERSON_MOTION_CHANGED_CELL_RATIO),
                static_cast<unsigned>(APP_PERSON_MOTION_CELL_DIFF_THRESHOLD),
                static_cast<unsigned>(APP_PERSON_MOTION_GRID_WIDTH),
                static_cast<unsigned>(APP_PERSON_MOTION_GRID_HEIGHT),
                static_cast<double>(person_status.score),
                static_cast<long long>(person_status.reference_updated_at_us / 1000),
                static_cast<long long>(person_status.analyzed_at_us / 1000),
                static_cast<unsigned>(person_status.inference_time_ms));
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, payload, HTTPD_RESP_USE_STRLEN);
}

esp_err_t reference_get_handler(httpd_req_t *req) {
  std::array<uint8_t, APP_PERSON_MOTION_GRID_WIDTH * APP_PERSON_MOTION_GRID_HEIGHT> fingerprint = {};
  uint16_t width = 0;
  uint16_t height = 0;
  int64_t updated_at_us = 0;
  bool valid = false;

  const esp_err_t err = app_person_detect_get_motion_reference(fingerprint.data(),
                                                               fingerprint.size(),
                                                               &width,
                                                               &height,
                                                               &updated_at_us,
                                                               &valid,
                                                               1000);
  if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
    return httpd_resp_send_500(req);
  }

  char payload[2048] = {};
  int written = std::snprintf(payload,
                              sizeof(payload),
                              "{\"valid\":%s,\"width\":%u,\"height\":%u,\"updated_at_ms\":%lld,\"pixels\":[",
                              valid ? "true" : "false",
                              static_cast<unsigned>(width),
                              static_cast<unsigned>(height),
                              static_cast<long long>(updated_at_us / 1000));
  if (written < 0 || static_cast<size_t>(written) >= sizeof(payload)) {
    return httpd_resp_send_500(req);
  }

  size_t offset = static_cast<size_t>(written);
  for (size_t index = 0; index < fingerprint.size(); ++index) {
    const int entry_written = std::snprintf(payload + offset,
                                            sizeof(payload) - offset,
                                            "%s%u",
                                            index == 0 ? "" : ",",
                                            static_cast<unsigned>(fingerprint[index]));
    if (entry_written < 0 || static_cast<size_t>(entry_written) >= (sizeof(payload) - offset)) {
      return httpd_resp_send_500(req);
    }
    offset += static_cast<size_t>(entry_written);
  }

  const int tail_written = std::snprintf(payload + offset, sizeof(payload) - offset, "]}");
  if (tail_written < 0 || static_cast<size_t>(tail_written) >= (sizeof(payload) - offset)) {
    return httpd_resp_send_500(req);
  }

  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, payload, HTTPD_RESP_USE_STRLEN);
}

esp_err_t motion_debug_get_handler(httpd_req_t *req) {
  app_person_motion_debug_t debug_info = {};
  const esp_err_t err = app_person_detect_get_motion_debug(&debug_info, 1000);
  if (err == ESP_ERR_INVALID_STATE || err == ESP_ERR_NOT_FOUND) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"valid\":false}");
  }
  if (err != ESP_OK) {
    return httpd_resp_send_500(req);
  }

  char payload[8192] = {};
  int written = std::snprintf(payload,
                              sizeof(payload),
                              "{\"valid\":true,\"width\":%u,\"height\":%u,\"current_captured_at_ms\":%lld,\"reference_updated_at_ms\":%lld,\"reference_ready\":%s,\"motion_score\":%.3f,\"current\":[",
                              static_cast<unsigned>(debug_info.width),
                              static_cast<unsigned>(debug_info.height),
                              static_cast<long long>(debug_info.current_captured_at_us / 1000),
                              static_cast<long long>(debug_info.reference_updated_at_us / 1000),
                              debug_info.reference_ready ? "true" : "false",
                              static_cast<double>(debug_info.motion_score));
  if (written < 0 || static_cast<size_t>(written) >= sizeof(payload)) {
    return httpd_resp_send_500(req);
  }

  size_t offset = static_cast<size_t>(written);
  if (append_motion_array_json(payload, sizeof(payload), &offset, debug_info.current, kMotionCellCount) != ESP_OK) {
    return httpd_resp_send_500(req);
  }

  written = std::snprintf(payload + offset, sizeof(payload) - offset, "],\"reference\":[");
  if (written < 0 || static_cast<size_t>(written) >= (sizeof(payload) - offset)) {
    return httpd_resp_send_500(req);
  }
  offset += static_cast<size_t>(written);
  if (append_motion_array_json(payload, sizeof(payload), &offset, debug_info.reference, kMotionCellCount) != ESP_OK) {
    return httpd_resp_send_500(req);
  }

  written = std::snprintf(payload + offset, sizeof(payload) - offset, "],\"diff\":[");
  if (written < 0 || static_cast<size_t>(written) >= (sizeof(payload) - offset)) {
    return httpd_resp_send_500(req);
  }
  offset += static_cast<size_t>(written);
  if (append_motion_array_json(payload, sizeof(payload), &offset, debug_info.diff, kMotionCellCount) != ESP_OK) {
    return httpd_resp_send_500(req);
  }

  written = std::snprintf(payload + offset, sizeof(payload) - offset, "],\"changed\":[");
  if (written < 0 || static_cast<size_t>(written) >= (sizeof(payload) - offset)) {
    return httpd_resp_send_500(req);
  }
  offset += static_cast<size_t>(written);
  if (append_motion_array_json(payload, sizeof(payload), &offset, debug_info.changed, kMotionCellCount) != ESP_OK) {
    return httpd_resp_send_500(req);
  }

  written = std::snprintf(payload + offset, sizeof(payload) - offset, "]}");
  if (written < 0 || static_cast<size_t>(written) >= (sizeof(payload) - offset)) {
    return httpd_resp_send_500(req);
  }

  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, payload, HTTPD_RESP_USE_STRLEN);
}

// Sends the current JPEG snapshot exactly as stored in RAM.
esp_err_t image_get_handler(httpd_req_t *req) {
  uint8_t *data = nullptr;
  size_t size = 0;
  int64_t captured_at_us = 0;
  const esp_err_t transfer_begin_err = begin_image_transfer();
  if (transfer_begin_err != ESP_OK) {
    return httpd_resp_send_500(req);
  }

  const esp_err_t copy_err = app_camera_copy_latest_image(&data, &size, &captured_at_us, nullptr, nullptr, 1000);
  if (copy_err == ESP_ERR_NOT_FOUND) {
    end_image_transfer();
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_sendstr(req, "No image captured yet");
  }

  if (copy_err != ESP_OK) {
    end_image_transfer();
    return httpd_resp_send_500(req);
  }

  if (data == nullptr || size == 0) {
    app_camera_free_image_copy(data);
    end_image_transfer();
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_sendstr(req, "No image captured yet");
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  const esp_err_t err = httpd_resp_send(req, reinterpret_cast<const char *>(data), static_cast<ssize_t>(size));
  app_camera_free_image_copy(data);
  end_image_transfer();
  return err;
}

// Requests a fresh capture through the central callback owned by main.cpp.
esp_err_t capture_post_handler(httpd_req_t *req) {
  if (g_capture_request_fn == nullptr) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":false,\"message\":\"Capture callback missing\"}");
  }

  const esp_err_t err = g_capture_request_fn();
  httpd_resp_set_type(req, "application/json");
  if (err != ESP_OK) {
    httpd_resp_set_status(req, "503 Service Unavailable");

    char payload[160] = {};
    std::snprintf(payload,
                  sizeof(payload),
                  "{\"ok\":false,\"message\":\"Capture failed: %s\"}",
                  esp_err_to_name(err));
    return httpd_resp_send(req, payload, HTTPD_RESP_USE_STRLEN);
  }

  return httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"New image captured\"}");
}

}  // namespace

esp_err_t app_http_server_start(app_http_capture_request_fn capture_request_fn) {
  // The HTTP layer stays thin and forwards manual capture requests back into the main control path.
  g_capture_request_fn = capture_request_fn;

  if (g_http_transfer_cpu_lock == nullptr) {
    ESP_RETURN_ON_ERROR(
        esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "http_transfer_max_freq", &g_http_transfer_cpu_lock),
        "app_http_server",
        "HTTP CPU lock creation failed");
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.max_uri_handlers = 8;
  config.stack_size = 8192;

  httpd_handle_t server = nullptr;
  ESP_RETURN_ON_ERROR(httpd_start(&server, &config), "app_http_server", "HTTP server start failed");

  httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_get_handler, .user_ctx = nullptr};
  httpd_uri_t status = {.uri = "/status", .method = HTTP_GET, .handler = status_get_handler, .user_ctx = nullptr};
  httpd_uri_t reference = {.uri = "/reference", .method = HTTP_GET, .handler = reference_get_handler, .user_ctx = nullptr};
  httpd_uri_t motion_debug = {.uri = "/motion-debug", .method = HTTP_GET, .handler = motion_debug_get_handler, .user_ctx = nullptr};
  httpd_uri_t image = {.uri = "/image.jpg", .method = HTTP_GET, .handler = image_get_handler, .user_ctx = nullptr};
  httpd_uri_t capture = {.uri = "/capture", .method = HTTP_POST, .handler = capture_post_handler, .user_ctx = nullptr};

  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &root), "app_http_server", "Register root failed");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &status), "app_http_server", "Register status failed");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &reference), "app_http_server", "Register reference failed");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &motion_debug), "app_http_server", "Register motion debug failed");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &image), "app_http_server", "Register image failed");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &capture), "app_http_server", "Register capture failed");
  return ESP_OK;
}

bool app_http_server_is_transmitting_image() {
  return g_active_image_transfers.load(std::memory_order_relaxed) > 0;
}
