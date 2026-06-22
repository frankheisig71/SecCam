#include "app_ir_led.h"

#include "app_config.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"

namespace {

constexpr char kTag[] = "app_ir_led";
bool g_ir_led_enabled = false;

}  // namespace

esp_err_t app_ir_led_init() {
  gpio_config_t config = {};
  config.pin_bit_mask = 1ULL << APP_IR_LED_GPIO;
  config.mode = GPIO_MODE_OUTPUT;
  config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  config.pull_up_en = GPIO_PULLUP_DISABLE;
  config.intr_type = GPIO_INTR_DISABLE;

  ESP_RETURN_ON_ERROR(gpio_config(&config), kTag, "IR LED GPIO configuration failed");
  gpio_set_level(APP_IR_LED_GPIO, 0);
  g_ir_led_enabled = false;
  ESP_LOGI(kTag, "IR LED GPIO initialized (GPIO%d)", APP_IR_LED_GPIO);
  return ESP_OK;
}

void app_ir_led_set_enabled(bool enabled) {
  g_ir_led_enabled = enabled;
  gpio_set_level(APP_IR_LED_GPIO, enabled ? 1 : 0);
}

bool app_ir_led_is_enabled() {
  return g_ir_led_enabled;
}
