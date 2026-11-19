#pragma once

#include <cstdarg>
#include "app_config.h"
#include "esp_log.h"

// Simple application trace subsystem.
// Traces are gated by APP_DEBUG_LEVEL and forwarded to ESP_LOGI so they
// appear with the standard ESP-IDF prefix/timestamp.

static inline void app_trace_v(int trace_level, const char *tag, const char *fmt, va_list args) {
  if (APP_DEBUG_LEVEL >= trace_level) {
    esp_log_va(ESP_LOG_CONFIG_INIT(ESP_LOG_INFO | ESP_LOG_CONFIGS_DEFAULT), tag, fmt, args);
  }
}

static inline void app_trace(int trace_level, const char *tag, const char *fmt, ...) {
  if (APP_DEBUG_LEVEL >= trace_level) {
    va_list args;
    va_start(args, fmt);
    app_trace_v(trace_level, tag, fmt, args);
    va_end(args);
  }
}

#define APP_TRACE(level, tag, fmt, ...) \
  do { \
    if (APP_DEBUG_LEVEL >= (level)) { \
      app_trace((level), (tag), (fmt), ##__VA_ARGS__); \
    } \
  } while (0)

#define APP_TRACE1(tag, fmt, ...) APP_TRACE(DEBUG_LEVEL_1, tag, fmt, ##__VA_ARGS__)
#define APP_TRACE2(tag, fmt, ...) APP_TRACE(DEBUG_LEVEL_2, tag, fmt, ##__VA_ARGS__)
#define APP_TRACE3(tag, fmt, ...) APP_TRACE(DEBUG_LEVEL_3, tag, fmt, ##__VA_ARGS__)
