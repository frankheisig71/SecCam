#include "app_ir_led.h"

#include "app_config.h"
#include "driver/rmt_common.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_check.h"

namespace {

constexpr char kTag[] = "app_ir_led";
rmt_channel_handle_t g_ir_led_channel = nullptr;
rmt_encoder_handle_t g_ir_led_encoder = nullptr;
bool g_ir_led_enabled = false;

void transmit_level(bool enabled) {
  if (g_ir_led_channel == nullptr || g_ir_led_encoder == nullptr) {
    return;
  }

  const uint32_t brightness = enabled ? APP_STATUS_LED_BRIGHTNESS : 0;
  const uint8_t pixel_data[3] = {0, 0, static_cast<uint8_t>(brightness)};
  rmt_transmit_config_t tx_config = {};
  tx_config.loop_count = 0;
  tx_config.flags.eot_level = 0;
  tx_config.flags.queue_nonblocking = 0;

  if (rmt_transmit(g_ir_led_channel, g_ir_led_encoder, pixel_data, sizeof(pixel_data), &tx_config) != ESP_OK) {
    return;
  }
  rmt_tx_wait_all_done(g_ir_led_channel, 100);
}

}  // namespace

esp_err_t app_ir_led_init() {
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

  ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&tx_channel_config, &g_ir_led_channel),
                      kTag,
                      "IR LED RMT channel init failed");

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

  ESP_RETURN_ON_ERROR(rmt_new_bytes_encoder(&bytes_encoder_config, &g_ir_led_encoder),
                      kTag,
                      "IR LED RMT encoder init failed");
  ESP_RETURN_ON_ERROR(rmt_enable(g_ir_led_channel), kTag, "IR LED RMT enable failed");
  app_ir_led_set_enabled(false);
  return ESP_OK;
}

void app_ir_led_set_enabled(bool enabled) {
  g_ir_led_enabled = enabled;
  transmit_level(enabled);
}

bool app_ir_led_is_enabled() {
  return g_ir_led_enabled;
}