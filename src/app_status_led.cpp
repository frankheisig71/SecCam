#include "app_status_led.h"

#include "app_config.h"
#include "esp_check.h"
#include "esp_log.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_common.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr char kTag[] = "app_status_led";
rmt_channel_handle_t g_status_channel = nullptr;
rmt_encoder_handle_t g_status_encoder = nullptr;
}

static esp_err_t transmit_color(uint8_t r, uint8_t g, uint8_t b) {
  if (g_status_channel == nullptr || g_status_encoder == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  const uint8_t pixel[3] = {r, g, b};
  rmt_transmit_config_t tx_config = {};
  tx_config.loop_count = 0;
  tx_config.flags.eot_level = 0;
  tx_config.flags.queue_nonblocking = 0;
  ESP_RETURN_ON_ERROR(rmt_transmit(g_status_channel, g_status_encoder, pixel, sizeof(pixel), &tx_config),
                      kTag,
                      "status led transmit failed");
  rmt_tx_wait_all_done(g_status_channel, 100);
  return ESP_OK;
}

esp_err_t app_status_led_init() {
  rmt_tx_channel_config_t tx_channel_config = {};
  tx_channel_config.gpio_num = APP_STATUS_LED_GPIO;
  tx_channel_config.clk_src = RMT_CLK_SRC_DEFAULT;
  tx_channel_config.resolution_hz = 10 * 1000 * 1000;
  tx_channel_config.mem_block_symbols = 64;
  tx_channel_config.trans_queue_depth = 1;
  tx_channel_config.intr_priority = 0;
  tx_channel_config.flags.invert_out = 0;
  tx_channel_config.flags.with_dma = 0;
  tx_channel_config.flags.io_loop_back = 0;
  tx_channel_config.flags.io_od_mode = 0;
  tx_channel_config.flags.allow_pd = 0;
  tx_channel_config.flags.init_level = 0;

  ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&tx_channel_config, &g_status_channel), kTag, "RMT new channel");

  rmt_bytes_encoder_config_t bytes_encoder_config = {};
  bytes_encoder_config.bit0.level0 = 1;
  bytes_encoder_config.bit0.duration0 = 4;
  bytes_encoder_config.bit0.level1 = 0;
  bytes_encoder_config.bit0.duration1 = 8;
  bytes_encoder_config.bit1.level0 = 1;
  bytes_encoder_config.bit1.duration0 = 8;
  bytes_encoder_config.bit1.level1 = 0;
  bytes_encoder_config.bit1.duration1 = 4;
  bytes_encoder_config.flags.msb_first = 1;

  ESP_RETURN_ON_ERROR(rmt_new_bytes_encoder(&bytes_encoder_config, &g_status_encoder), kTag, "RMT encoder");
  ESP_RETURN_ON_ERROR(rmt_enable(g_status_channel), kTag, "RMT enable");
  app_status_led_off();
  return ESP_OK;
}

void app_status_led_red() { transmit_color(255, 0, 0); }
void app_status_led_green() { transmit_color(0, 255, 0); }
void app_status_led_blue() { transmit_color(0, 0, 255); }
void app_status_led_yellow() { transmit_color(255, 255, 0); }
void app_status_led_off() { transmit_color(0, 0, 0); }

void app_status_led_blink_blue_ms(uint32_t ms) {
  // spawn a short task to blink without blocking caller
  struct BlinkArgs { uint32_t ms; };
  BlinkArgs *args = (BlinkArgs *)pvPortMalloc(sizeof(BlinkArgs));
  if (!args) return;
  args->ms = ms;
  xTaskCreate([](void *p) {
    BlinkArgs *a = static_cast<BlinkArgs *>(p);
    app_status_led_blue();
    vTaskDelay(pdMS_TO_TICKS(a->ms));
    app_status_led_off();
    vPortFree(a);
    vTaskDelete(nullptr);
  }, "status_blink", 2048, args, 5, nullptr);
}

void app_status_led_blink_yellow_ms(uint32_t ms) {
  // spawn a short task to blink without blocking caller
  struct BlinkArgs { uint32_t ms; };
  BlinkArgs *args = (BlinkArgs *)pvPortMalloc(sizeof(BlinkArgs));
  if (!args) return;
  args->ms = ms;
  xTaskCreate([](void *p) {
    BlinkArgs *a = static_cast<BlinkArgs *>(p);
    // use a moderated yellow (not full 255) to keep brightness reasonable
    transmit_color(32, 32, 0);
    vTaskDelay(pdMS_TO_TICKS(a->ms));
    app_status_led_off();
    vPortFree(a);
    vTaskDelete(nullptr);
  }, "status_blink_yellow", 2048, args, 5, nullptr);
}
