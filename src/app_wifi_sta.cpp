#include "app_wifi_sta.h"

#include <cstring>

#include "app_config.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <atomic>

namespace {

constexpr char kTag[] = "app_wifi_sta";
constexpr TickType_t kReconnectRetryDelay = pdMS_TO_TICKS(250);
bool g_handlers_registered = false;
esp_netif_t *g_sta_netif = nullptr;
TaskHandle_t g_connect_task = nullptr;
std::atomic<bool> g_sta_busy{false};
std::atomic<bool> g_connect_requested{false};

void request_sta_connect() {
  g_sta_busy.store(true, std::memory_order_relaxed);
  if (g_connect_task == nullptr) {
    return;
  }

  const bool already_requested = g_connect_requested.exchange(true, std::memory_order_acq_rel);
  if (!already_requested) {
    xTaskNotifyGive(g_connect_task);
  }
}

void sta_connect_task(void *) {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    while (g_connect_requested.exchange(false, std::memory_order_acq_rel)) {
      if (g_sta_busy.load(std::memory_order_relaxed)) {
        vTaskDelay(kReconnectRetryDelay);
      }

      const esp_err_t connect_err = esp_wifi_connect();
      if (connect_err == ESP_OK) {
        ESP_LOGI(kTag, "STA connect requested for %s", APP_WIFI_STA_SSID);
        break;
      }

      ESP_LOGW(kTag, "esp_wifi_connect failed: %s", esp_err_to_name(connect_err));
      vTaskDelay(kReconnectRetryDelay);
      g_connect_requested.store(true, std::memory_order_relaxed);
    }
  }
}

const char *wifi_disconnect_reason_name(uint8_t reason) {
  switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:
      return "AUTH_EXPIRE";
    case WIFI_REASON_BEACON_TIMEOUT:
      return "BEACON_TIMEOUT";
    case WIFI_REASON_NO_AP_FOUND:
      return "NO_AP_FOUND";
    case WIFI_REASON_AUTH_FAIL:
      return "AUTH_FAIL";
    case WIFI_REASON_ASSOC_FAIL:
      return "ASSOC_FAIL";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
      return "HANDSHAKE_TIMEOUT";
    case WIFI_REASON_CONNECTION_FAIL:
      return "CONNECTION_FAIL";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
      return "NO_AP_FOUND_W_COMPATIBLE_SECURITY";
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
      return "NO_AP_FOUND_IN_AUTHMODE_THRESHOLD";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
      return "NO_AP_FOUND_IN_RSSI_THRESHOLD";
    default:
      return "UNKNOWN";
  }
}

void on_wifi_event(void *, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  if (event_base != WIFI_EVENT) {
    return;
  }

  if (event_id == WIFI_EVENT_STA_START) {
    request_sta_connect();
  } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
    g_sta_busy.store(true, std::memory_order_relaxed);
    ESP_LOGI(kTag, "STA associated with %s, waiting for DHCP lease", APP_WIFI_STA_SSID);
    /*
    if (g_sta_netif != nullptr) {
      const esp_err_t dhcp_err = esp_netif_dhcpc_start(g_sta_netif);
      if (dhcp_err != ESP_OK && dhcp_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        ESP_LOGW(kTag, "DHCP client start failed: %s", esp_err_to_name(dhcp_err));
      }
    }
    */
  } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
    const auto *disconnected = static_cast<wifi_event_sta_disconnected_t *>(event_data);
    const int reason = disconnected != nullptr ? disconnected->reason : -1;
    const int rssi = disconnected != nullptr ? disconnected->rssi : 0;
    ESP_LOGW(kTag,
             "STA disconnected from %s, reason=%d (%s), rssi=%d, retrying",
             APP_WIFI_STA_SSID,
             reason,
             disconnected != nullptr ? wifi_disconnect_reason_name(disconnected->reason) : "UNKNOWN",
             rssi);
    request_sta_connect();
  }
}

void on_ip_event(void *, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  if (event_base != IP_EVENT || event_id != IP_EVENT_STA_GOT_IP) {
    return;
  }

  g_sta_busy.store(false, std::memory_order_relaxed);
  const auto *got_ip = static_cast<ip_event_got_ip_t *>(event_data);
  ESP_LOGI(kTag,
           "STA got IP: http://" IPSTR " (netmask " IPSTR ", gateway " IPSTR ")",
           IP2STR(&got_ip->ip_info.ip),
           IP2STR(&got_ip->ip_info.netmask),
           IP2STR(&got_ip->ip_info.gw));
}

esp_err_t ensure_wifi_stack() {
  static bool initialized = false;
  if (initialized) {
    return ESP_OK;
  }

  ESP_RETURN_ON_ERROR(esp_netif_init(), kTag, "esp_netif_init failed");

  const esp_err_t loop_err = esp_event_loop_create_default();
  if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
    return loop_err;
  }

  g_sta_netif = esp_netif_create_default_wifi_sta();
  if (g_sta_netif == nullptr) {
    return ESP_FAIL;
  }

  wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
  ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), kTag, "esp_wifi_init failed");

  if (!g_handlers_registered) {
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, nullptr),
        kTag,
        "Register WiFi event handler failed");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip_event, nullptr),
        kTag,
        "Register IP event handler failed");
    g_handlers_registered = true;
  }

  if (g_connect_task == nullptr) {
    const BaseType_t task_ok = xTaskCreate(sta_connect_task, "wifi_sta_connect", 3072, nullptr, 6, &g_connect_task);
    if (task_ok != pdPASS) {
      g_connect_task = nullptr;
      return ESP_ERR_NO_MEM;
    }
  }

  initialized = true;
  return ESP_OK;
}

}  // namespace

esp_err_t app_wifi_sta_start() {
  ESP_RETURN_ON_ERROR(ensure_wifi_stack(), kTag, "WiFi stack init failed");

  wifi_config_t wifi_config = {};
  std::memcpy(wifi_config.sta.ssid, APP_WIFI_STA_SSID, sizeof(APP_WIFI_STA_SSID) - 1);
  std::memcpy(wifi_config.sta.password, APP_WIFI_STA_PASSWORD, sizeof(APP_WIFI_STA_PASSWORD) - 1);
  wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
  wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  wifi_config.sta.failure_retry_cnt = 5;
  wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  wifi_config.sta.pmf_cfg.capable = true;
  wifi_config.sta.pmf_cfg.required = false;

  ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), kTag, "Set STA mode failed");
  ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), kTag, "Set STA config failed");
  ESP_RETURN_ON_ERROR(esp_wifi_start(), kTag, "Start STA failed");
  ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), kTag, "Disable STA power save failed");

  ESP_LOGI(kTag,
           "STA connect started: ssid=%s, scan=all-channel, retries=%u, auth-threshold=%d",
           APP_WIFI_STA_SSID,
           static_cast<unsigned>(wifi_config.sta.failure_retry_cnt),
           static_cast<int>(wifi_config.sta.threshold.authmode));
  return ESP_OK;
}

bool app_wifi_sta_is_busy() {
  return g_sta_busy.load(std::memory_order_relaxed);
}
