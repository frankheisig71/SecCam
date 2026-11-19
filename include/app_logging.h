#pragma once

#include <cstdarg>

#include "app_config.h"
#include "esp_log.h"
#include "app_trace.h"

// Keep a convenience check for legacy use.
#define APP_LOG_LEVEL_ENABLED(level) (APP_DEBUG_LEVEL >= (level))

static inline void app_log_write_level(esp_log_level_t level,
                                       const char *tag,
                                       const char *fmt,
                                       ...) {
  va_list args;
  va_start(args, fmt);
  esp_log_va(ESP_LOG_CONFIG_INIT(level | ESP_LOG_CONFIGS_DEFAULT), tag, fmt, args);
  va_end(args);
}

// Application-specific helpers should use the APP_TRACE API so trace
// messages are consistently gated by APP_DEBUG_LEVEL and forwarded to
// the standard ESP-IDF logging system (with timestamp/tag prefix).
#define APP_LOG_PIR_TRIGGER(tag, reason) \
  do { \
    APP_TRACE2(tag, "PIR trigger: %s", reason); \
  } while (0)

#define APP_LOG_PICTURE_TRANSFER(tag, file_name, reason, size, seq_index, seq_total) \
  do { \
    APP_TRACE2(tag, "Transmitting JPEG filename=%s reason=%s size=%u seq=%u/%u", \
               file_name, reason, static_cast<unsigned>(size), \
               static_cast<unsigned>(seq_index), static_cast<unsigned>(seq_total)); \
  } while (0)

