#include "app_wifi_ap.h"

#include <cstring>

#include "app_config.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"

namespace {

constexpr char kTag[] = "app_wifi_ap";
esp_netif_t *g_ap_netif = nullptr;

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

  g_ap_netif = esp_netif_create_default_wifi_ap();
  if (g_ap_netif == nullptr) {
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

  esp_netif_ip_info_t ip_info = {};
  ip4_addr_t ap_ip = {};
  ip4_addr_t netmask = {};
  if (!ip4addr_aton(APP_WIFI_AP_IP_ADDR, &ap_ip)) {
    ESP_LOGE(kTag, "Invalid AP IP address string: %s", APP_WIFI_AP_IP_ADDR);
    return ESP_ERR_INVALID_ARG;
  }
  IP4_ADDR(&netmask, 255, 255, 255, 0);
  ip_info.ip.addr = ap_ip.addr;
  ip_info.gw.addr = ap_ip.addr;
  ip_info.netmask.addr = netmask.addr;
  if (g_ap_netif != nullptr) {
    const esp_err_t dhcp_stop_err = esp_netif_dhcps_stop(g_ap_netif);
    if (dhcp_stop_err != ESP_OK && dhcp_stop_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
      ESP_LOGE(kTag, "Stopping AP DHCP server failed: %s", esp_err_to_name(dhcp_stop_err));
      return dhcp_stop_err;
    }

    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(g_ap_netif, &ip_info), kTag, "Setting AP IP info failed");
  }

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

  if (g_ap_netif != nullptr) {
    const esp_err_t dhcp_start_err = esp_netif_dhcps_start(g_ap_netif);
    if (dhcp_start_err != ESP_OK && dhcp_start_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
      ESP_LOGE(kTag, "Starting AP DHCP server failed: %s", esp_err_to_name(dhcp_start_err));
      return dhcp_start_err;
    }
  }

  esp_netif_ip_info_t active_ip_info = {};
  if (g_ap_netif != nullptr && esp_netif_get_ip_info(g_ap_netif, &active_ip_info) == ESP_OK) {
    ESP_LOGI(kTag,
             "SoftAP active: ssid=%s password=%s ip=" IPSTR,
             APP_WIFI_AP_SSID,
             APP_WIFI_AP_PASSWORD,
             IP2STR(&active_ip_info.ip));
  } else {
    ESP_LOGI(kTag, "SoftAP active: ssid=%s password=%s ip=" IPSTR, APP_WIFI_AP_SSID, APP_WIFI_AP_PASSWORD, IP2STR(&ip_info.ip));
  }
  return ESP_OK;
}
