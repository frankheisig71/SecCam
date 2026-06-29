#pragma once

#include <app_secrets.h>

// ============================================================================
// Configuration overview
// - This header centralizes defaults while allowing overrides from
//   PlatformIO via `build_flags = -D...` or from a board-specific header.
// - Recommended pattern: define a target (e.g. APP_TARGET_XIAO_ESP32S3_SENSE)
//   and sensor (e.g. APP_CAMERA_SENSOR_OV3660) in `platformio.ini` and only
//   override values that differ from these defaults.
//
// Example PlatformIO build_flags (see doc/PLATFORMIO_EXAMPLES.md):
//   -DAPP_TARGET_XIAO_ESP32S3_SENSE -DAPP_CAMERA_SENSOR_OV3660
// ============================================================================

// ----------------------- Target / Sensor selection -------------------------
// Define one of these from build flags to pick board-specific pin mappings.
#if !defined(APP_TARGET_ESP32_S3_CAMERA) && !defined(APP_TARGET_XIAO_ESP32S3_SENSE)
// default target: generic ESP32-S3 + OV2640 wiring used historically
#define APP_TARGET_ESP32_S3_CAMERA 1
#endif

// Camera sensor selection: override with build flag if needed.
#ifndef APP_CAMERA_SENSOR_OV2640
#define APP_CAMERA_SENSOR_OV2640 1
#endif
#ifndef APP_CAMERA_SENSOR_OV3660
#define APP_CAMERA_SENSOR_OV3660 2
#endif
#ifndef APP_CAMERA_SENSOR
#define APP_CAMERA_SENSOR APP_CAMERA_SENSOR_OV2640
#endif

// ----------------------- WiFi / Setup -------------------------------------
// Single workspace switch: 1 = setup AP mode, 0 = normal STA mode.
#ifndef APP_WIFI_IS_AP
#define APP_WIFI_IS_AP 0
#endif

#define APP_WIFI_MODE_AP 1
#define APP_WIFI_MODE_STA 2
#if APP_WIFI_IS_AP
#define APP_WIFI_MODE APP_WIFI_MODE_AP
#else
#define APP_WIFI_MODE APP_WIFI_MODE_STA
#endif

#if APP_WIFI_MODE == APP_WIFI_MODE_AP
#ifndef APP_WIFI_AP_SSID
#define APP_WIFI_AP_SSID "GooUuuu-CAM"
#endif
#ifndef APP_WIFI_AP_PASSWORD
#define APP_WIFI_AP_PASSWORD "goouuuu123"
#endif
#ifndef APP_WIFI_AP_CHANNEL
#define APP_WIFI_AP_CHANNEL 1
#endif
#ifndef APP_WIFI_AP_MAX_CLIENTS
#define APP_WIFI_AP_MAX_CLIENTS 4
#endif
#ifndef APP_WIFI_AP_IP_ADDR
#define APP_WIFI_AP_IP_ADDR "192.168.4.1"
#endif
#endif

// Ensure AP-related macros exist even when AP mode is disabled so files
// referencing them (e.g. `app_wifi_ap.cpp`) compile in all build envs.
#ifndef APP_WIFI_AP_SSID
#define APP_WIFI_AP_SSID "GooUuuu-CAM"
#endif
#ifndef APP_WIFI_AP_PASSWORD
#define APP_WIFI_AP_PASSWORD "goouuuu123"
#endif
#ifndef APP_WIFI_AP_CHANNEL
#define APP_WIFI_AP_CHANNEL 1
#endif
#ifndef APP_WIFI_AP_MAX_CLIENTS
#define APP_WIFI_AP_MAX_CLIENTS 4
#endif
#ifndef APP_WIFI_AP_IP_ADDR
#define APP_WIFI_AP_IP_ADDR "192.168.4.1"
#endif

// Setup mode keeps only AP preview capture and the IR toggle UI.
#ifndef APP_SETUP
#define APP_SETUP 0
#endif
#ifndef APP_CAPTURE
#define APP_CAPTURE 0
#endif
#ifndef APP_SETUP_PREVIEW_INTERVAL_MS
#define APP_SETUP_PREVIEW_INTERVAL_MS 500
#endif

// ----------------------- Capture & Triggering ------------------------------
// Default trigger pins and timings can be overridden per-board.
#ifndef APP_CAPTURE_TRIGGER_GPIO
#define APP_CAPTURE_TRIGGER_GPIO GPIO_NUM_21
#endif
#ifndef APP_IR_LED_GPIO
#define APP_IR_LED_GPIO GPIO_NUM_20
#endif

#ifndef APP_IR_LED_PRE_CAPTURE_MS
#define APP_IR_LED_PRE_CAPTURE_MS 500
#endif
#ifndef APP_IR_LED_POST_FIRST_CAPTURE_MS
#define APP_IR_LED_POST_FIRST_CAPTURE_MS 500
#endif
#ifndef APP_IR_LED_POST_SECOND_CAPTURE_MS
#define APP_IR_LED_POST_SECOND_CAPTURE_MS 1000
#endif

#ifndef APP_STATUS_LED_GPIO
#define APP_STATUS_LED_GPIO GPIO_NUM_48
#endif
#ifndef APP_STATUS_LED_PIXEL_COUNT
#define APP_STATUS_LED_PIXEL_COUNT 1
#endif
#ifndef APP_STATUS_LED_BRIGHTNESS
#define APP_STATUS_LED_BRIGHTNESS 16
#endif

// Shared debounce/cooldown window for GPIO and manual triggers.
#ifndef APP_CAPTURE_TRIGGER_COOLDOWN_MS
#define APP_CAPTURE_TRIGGER_COOLDOWN_MS (5 * 1000)
#endif
#ifndef APP_CAPTURE_TRIGGER_EXTRA_FRAMES
#define APP_CAPTURE_TRIGGER_EXTRA_FRAMES 2
#endif
#ifndef APP_CAPTURE_TRIGGER_ANALYZE_FRAMES
#define APP_CAPTURE_TRIGGER_ANALYZE_FRAMES 3
#endif
#ifndef APP_CAPTURE_TRIGGER_SUPPRESSION_MS
#define APP_CAPTURE_TRIGGER_SUPPRESSION_MS (10 * 1000)
#endif
#ifndef APP_CAPTURE_REFERENCE_REFRESH_MS
#define APP_CAPTURE_REFERENCE_REFRESH_MS (60 * 1000)
#endif
#ifndef APP_CAPTURE_REFERENCE_IDLE_MS
#define APP_CAPTURE_REFERENCE_IDLE_MS (10 * 1000)
#endif

#define APP_CAPTURE_TRIGGER_POLL_MS 50

// Idle low, capture high: ESP-IDF power management
#ifndef APP_CPU_FREQ_IDLE_MHZ
#define APP_CPU_FREQ_IDLE_MHZ 80
#endif
#ifndef APP_CPU_FREQ_ACTIVE_MHZ
#define APP_CPU_FREQ_ACTIVE_MHZ 240
#endif


// ----------------------- HTTP / Dataset Collector -------------------------
#ifndef APP_HTTP_SERVER_ENABLED
#define APP_HTTP_SERVER_ENABLED (APP_SETUP ? 1 : 0)
#endif
#ifndef APP_DATASET_COLLECTOR_ENABLED
#define APP_DATASET_COLLECTOR_ENABLED 1
#endif
#ifndef APP_DATASET_COLLECTOR_DEVICE_ID
#define APP_DATASET_COLLECTOR_DEVICE_ID "goouuuu-cam"
#endif
#ifndef APP_DATASET_COLLECTOR_UPLOAD_URL
#define APP_DATASET_COLLECTOR_UPLOAD_URL "http://192.168.178.149:8080/api/v1/captures"
#endif
#ifndef APP_DATASET_COLLECTOR_UPLOAD_TIMEOUT_MS
#define APP_DATASET_COLLECTOR_UPLOAD_TIMEOUT_MS 5000
#endif
#ifndef APP_DATASET_COLLECTOR_UPLOAD_QUEUE_DEPTH
#define APP_DATASET_COLLECTOR_UPLOAD_QUEUE_DEPTH 8
#endif
#ifndef APP_DATASET_COLLECTOR_UPLOAD_QUEUE_TIMEOUT_MS
#define APP_DATASET_COLLECTOR_UPLOAD_QUEUE_TIMEOUT_MS 0
#endif
#ifndef APP_DATASET_COLLECTOR_UPLOAD_RETRY_COUNT
#define APP_DATASET_COLLECTOR_UPLOAD_RETRY_COUNT 5
#endif
#ifndef APP_DATASET_COLLECTOR_UPLOAD_RETRY_DELAY_MS
#define APP_DATASET_COLLECTOR_UPLOAD_RETRY_DELAY_MS (2 * 1000)
#endif
#ifndef APP_DATASET_COLLECTOR_UPLOAD_FAILURE_GUARD_THRESHOLD
#define APP_DATASET_COLLECTOR_UPLOAD_FAILURE_GUARD_THRESHOLD 3
#endif
#ifndef APP_DATASET_COLLECTOR_UPLOAD_FAILURE_GUARD_MS
#define APP_DATASET_COLLECTOR_UPLOAD_FAILURE_GUARD_MS (2 * 60 * 1000)
#endif
#ifndef APP_DATASET_COLLECTOR_UPLOAD_RETRY_WITH_BACKLOG
#define APP_DATASET_COLLECTOR_UPLOAD_RETRY_WITH_BACKLOG 0
#endif
#ifndef APP_DATASET_COLLECTOR_UPLOAD_TASK_STACK_SIZE
#define APP_DATASET_COLLECTOR_UPLOAD_TASK_STACK_SIZE 6144
#endif
#ifndef APP_DATASET_COLLECTOR_UPLOAD_TASK_PRIORITY
#define APP_DATASET_COLLECTOR_UPLOAD_TASK_PRIORITY 4
#endif
#ifndef APP_DATASET_COLLECTOR_IDLE_INTERVAL_MS
#define APP_DATASET_COLLECTOR_IDLE_INTERVAL_MS (15 * 60 * 1000)
#endif
#ifndef APP_DATASET_COLLECTOR_MOTION_GUARD_MS
#define APP_DATASET_COLLECTOR_MOTION_GUARD_MS (10 * 1000)
#endif
#ifndef APP_DATASET_COLLECTOR_MOTION_IMAGE_COUNT
#define APP_DATASET_COLLECTOR_MOTION_IMAGE_COUNT 2
#endif
#ifndef APP_DATASET_COLLECTOR_MOTION_IMAGE_SPACING_MS
#define APP_DATASET_COLLECTOR_MOTION_IMAGE_SPACING_MS (1 * 1000)
#endif
#ifndef APP_DATASET_COLLECTOR_POST_BURST_GUARD_MS
#define APP_DATASET_COLLECTOR_POST_BURST_GUARD_MS (5 * 60 * 1000)
#endif
#ifndef APP_DATASET_COLLECTOR_MAX_MOTION_CAPTURES
#define APP_DATASET_COLLECTOR_MAX_MOTION_CAPTURES 3
#endif

// ----------------------- Person detection / classifier -------------------
#ifndef APP_PERSON_DETECT_ENABLED
#define APP_PERSON_DETECT_ENABLED 1
#endif
#ifndef APP_PERSON_DETECT_MODEL_REQUIRED
#define APP_PERSON_DETECT_MODEL_REQUIRED 0
#endif

#define APP_PERSON_DETECT_BACKEND_PEDESTRIAN 1
#define APP_PERSON_DETECT_BACKEND_COCO_320 2
#define APP_PERSON_DETECT_BACKEND_EDGE_IMPULSE 3
#ifndef APP_PERSON_DETECT_BACKEND
#define APP_PERSON_DETECT_BACKEND APP_PERSON_DETECT_BACKEND_EDGE_IMPULSE
#endif

#if APP_PERSON_DETECT_ENABLED && APP_PERSON_DETECT_BACKEND != APP_PERSON_DETECT_BACKEND_EDGE_IMPULSE
#define APP_PERSON_DETECT_HAS_CLASSIFIER 1
#else
#define APP_PERSON_DETECT_HAS_CLASSIFIER 0
#endif

#ifndef APP_PERSON_DETECT_COCO_PERSON_CATEGORY
#define APP_PERSON_DETECT_COCO_PERSON_CATEGORY 0
#endif
#ifndef APP_PERSON_DETECT_CANDIDATE_THRESHOLD
#define APP_PERSON_DETECT_CANDIDATE_THRESHOLD 0.45f
#endif
#ifndef APP_PERSON_DETECT_PRESENT_THRESHOLD
#define APP_PERSON_DETECT_PRESENT_THRESHOLD 0.60f
#endif
#ifndef APP_PERSON_CLASSIFIER_PRESENT_THRESHOLD
#define APP_PERSON_CLASSIFIER_PRESENT_THRESHOLD 0.45f
#endif
#ifndef APP_PERSON_DETECT_NMS_THRESHOLD
#define APP_PERSON_DETECT_NMS_THRESHOLD 0.45f
#endif
#ifndef APP_PERSON_DETECT_SCORE_SMOOTHING_ALPHA
#define APP_PERSON_DETECT_SCORE_SMOOTHING_ALPHA 0.35f
#endif
#ifndef APP_PERSON_DETECT_PRESENCE_HOLD_FRAMES
#define APP_PERSON_DETECT_PRESENCE_HOLD_FRAMES 2
#endif

#ifndef APP_PERSON_MOTION_GRID_WIDTH
#define APP_PERSON_MOTION_GRID_WIDTH 16
#endif
#ifndef APP_PERSON_MOTION_GRID_HEIGHT
#define APP_PERSON_MOTION_GRID_HEIGHT 12
#endif
#ifndef APP_PERSON_MOTION_CELL_DIFF_THRESHOLD
#define APP_PERSON_MOTION_CELL_DIFF_THRESHOLD 18
#endif
#ifndef APP_PERSON_MOTION_CHANGED_CELL_RATIO
#define APP_PERSON_MOTION_CHANGED_CELL_RATIO 0.18f
#endif
#ifndef APP_PERSON_DETECT_MODEL_NAME
#define APP_PERSON_DETECT_MODEL_NAME "person_detect.espdl"
#endif

// ----------------------- Camera defaults ---------------------------------
#ifndef APP_CAMERA_XCLK_HZ
#define APP_CAMERA_XCLK_HZ 20000000
#endif
#ifndef APP_CAMERA_JPEG_QUALITY
#define APP_CAMERA_JPEG_QUALITY 10
#endif
#ifndef APP_CAMERA_WARMUP_FRAMES
#define APP_CAMERA_WARMUP_FRAMES 3
#endif
#ifndef APP_CAMERA_TRIGGER_FRAMES
#define APP_CAMERA_TRIGGER_FRAMES 2
#endif
#ifndef APP_CAPTURE_ON_STARTUP
#define APP_CAPTURE_ON_STARTUP 1
#endif
#ifndef APP_CAPTURE_TRIGGER_BURST_COUNT
#define APP_CAPTURE_TRIGGER_BURST_COUNT 3
#endif

#ifndef APP_CAMERA_AE_LEVEL
#define APP_CAMERA_AE_LEVEL 0
#endif
#ifndef APP_CAMERA_GAINCEILING
#define APP_CAMERA_GAINCEILING GAINCEILING_8X
#endif
#ifndef APP_CAMERA_BRIGHTNESS
#define APP_CAMERA_BRIGHTNESS 0
#endif
#ifndef APP_CAMERA_CONTRAST
#define APP_CAMERA_CONTRAST 0
#endif

// Use the largest practical PSRAM-backed frame for better detector input detail.
#ifndef APP_CAMERA_FRAME_SIZE_PSRAM
#define APP_CAMERA_FRAME_SIZE_PSRAM FRAMESIZE_UXGA
#endif
#ifndef APP_CAMERA_FRAME_SIZE_NO_PSRAM
#define APP_CAMERA_FRAME_SIZE_NO_PSRAM FRAMESIZE_VGA
#endif

// ----------------------- Board-specific pin mappings ----------------------
// Provide mapping for known targets. Prefer overriding `APP_TARGET_*` via
// `-D` build flags in platformio.ini.
#if defined(APP_TARGET_XIAO_ESP32S3_SENSE)
// Example: See https://www.reichelt.de/... (XIAO ESP32-S3 Sense camera)
#define CAM_PIN_PWDN -1
#define CAM_PIN_RESET -1
#define CAM_PIN_XCLK 21
#define CAM_PIN_SIOD 22
#define CAM_PIN_SIOC 23
#define CAM_PIN_D7 33
#define CAM_PIN_D6 32
#define CAM_PIN_D5 35
#define CAM_PIN_D4 34
#define CAM_PIN_D3 39
#define CAM_PIN_D2 36
#define CAM_PIN_D1 37
#define CAM_PIN_D0 38
#define CAM_PIN_VSYNC 25
#define CAM_PIN_HREF 26
#define CAM_PIN_PCLK 27
#elif defined(APP_TARGET_ESP32_S3_CAMERA) || defined(APP_TARGET_ESP32_S3_CAM)
// Historical default mapping (ESP32-S3 + OV2640)
#define CAM_PIN_PWDN -1
#define CAM_PIN_RESET -1
#define CAM_PIN_XCLK 15
#define CAM_PIN_SIOD 4
#define CAM_PIN_SIOC 5
#define CAM_PIN_D7 16
#define CAM_PIN_D6 17
#define CAM_PIN_D5 18
#define CAM_PIN_D4 12
#define CAM_PIN_D3 10
#define CAM_PIN_D2 8
#define CAM_PIN_D1 9
#define CAM_PIN_D0 11
#define CAM_PIN_VSYNC 6
#define CAM_PIN_HREF 7
#define CAM_PIN_PCLK 13
#else
// Fallback generic pins (may need override)
#define CAM_PIN_PWDN -1
#define CAM_PIN_RESET -1
#define CAM_PIN_XCLK 15
#define CAM_PIN_SIOD 4
#define CAM_PIN_SIOC 5
#define CAM_PIN_D7 16
#define CAM_PIN_D6 17
#define CAM_PIN_D5 18
#define CAM_PIN_D4 12
#define CAM_PIN_D3 10
#define CAM_PIN_D2 8
#define CAM_PIN_D1 9
#define CAM_PIN_D0 11
#define CAM_PIN_VSYNC 6
#define CAM_PIN_HREF 7
#define CAM_PIN_PCLK 13
#endif

// End of configuration

