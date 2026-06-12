#include "app_wifi_ap.h"

#include <cstring>

#include "app_config.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

namespace {

constexpr char kTag[] = "app_wifi_ap";

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

  esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
  if (ap_netif == nullptr) {
    return ESP_FAIL;
  }

  wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
  ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), kTag, "esp_wifi_init failed");
  initialized = true;
  return ESP_OK;
}

}  // namespace

esp_err_t app_wifi_ap_start() {
  ESP_RETURN_ON_ERROR(ensure_wifi_stack(), kTag, "WiFi stack init failed");

  wifi_config_t wifi_config = {};
  std::memcpy(wifi_config.ap.ssid, APP_WIFI_AP_SSID, sizeof(APP_WIFI_AP_SSID) - 1);
  std::memcpy(wifi_config.ap.password, APP_WIFI_AP_PASSWORD, sizeof(APP_WIFI_AP_PASSWORD) - 1);
  wifi_config.ap.ssid_len = sizeof(APP_WIFI_AP_SSID) - 1;
  wifi_config.ap.channel = APP_WIFI_AP_CHANNEL;
  wifi_config.ap.max_connection = APP_WIFI_AP_MAX_CLIENTS;
  wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
  wifi_config.ap.pmf_cfg.capable = true;
  wifi_config.ap.pmf_cfg.required = false;

  if (sizeof(APP_WIFI_AP_PASSWORD) <= 1) {
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;
  }

  ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), kTag, "Set AP mode failed");
  ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &wifi_config), kTag, "Set AP config failed");
  ESP_RETURN_ON_ERROR(esp_wifi_start(), kTag, "Start AP failed");

  ESP_LOGI(kTag, "SoftAP active: ssid=%s password=%s", APP_WIFI_AP_SSID, APP_WIFI_AP_PASSWORD);
  return ESP_OK;
}
