if(NOT DEFINED APP_EDGE_IMPULSE_MODEL_DIR)
    set(APP_EDGE_IMPULSE_MODEL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../edge_impulse/Person_detection_inferencing")
endif()
if(DEFINED ENV{APP_EDGE_IMPULSE_MODEL_DIR})
    set(APP_EDGE_IMPULSE_MODEL_DIR "$ENV{APP_EDGE_IMPULSE_MODEL_DIR}")
endif()

set(APP_EDGE_IMPULSE_MODEL_SRC "${APP_EDGE_IMPULSE_MODEL_DIR}/src")

file(GLOB EI_TFLITE_CORE_SRCS
    "${APP_EDGE_IMPULSE_MODEL_SRC}/edge-impulse-sdk/tensorflow/lite/core/api/*.cpp"
)
file(GLOB EI_TFLITE_KERNEL_INTERNAL_SRCS
    "${APP_EDGE_IMPULSE_MODEL_SRC}/edge-impulse-sdk/tensorflow/lite/kernels/internal/*.cpp"
)
file(GLOB EI_TFLITE_MICRO_SRCS
    "${APP_EDGE_IMPULSE_MODEL_SRC}/edge-impulse-sdk/tensorflow/lite/micro/*.cpp"
)
file(GLOB EI_TFLITE_MICRO_KERNEL_SRCS
    "${APP_EDGE_IMPULSE_MODEL_SRC}/edge-impulse-sdk/tensorflow/lite/micro/kernels/*.cpp"
)
file(GLOB EI_TFLITE_MICRO_PLANNER_SRCS
    "${APP_EDGE_IMPULSE_MODEL_SRC}/edge-impulse-sdk/tensorflow/lite/micro/memory_planner/*.cpp"
)

set(EI_MODEL_SRCS
    "${APP_EDGE_IMPULSE_MODEL_SRC}/tflite-model/tflite_learn_1095583_4_compiled.cpp"
    "${APP_EDGE_IMPULSE_MODEL_SRC}/edge-impulse-sdk/porting/espressif/ei_classifier_porting.cpp"
)

set(EI_EXTRA_RUNTIME_SRCS
    "${APP_EDGE_IMPULSE_MODEL_SRC}/edge-impulse-sdk/tensorflow/lite/kernels/kernel_util_lite.cpp"
)

set(EI_RUNTIME_SRCS
    ${EI_TFLITE_CORE_SRCS}
    ${EI_TFLITE_KERNEL_INTERNAL_SRCS}
    ${EI_TFLITE_MICRO_SRCS}
    ${EI_TFLITE_MICRO_KERNEL_SRCS}
    ${EI_TFLITE_MICRO_PLANNER_SRCS}
    ${EI_EXTRA_RUNTIME_SRCS}
    ${EI_MODEL_SRCS}
)

idf_component_register(SRCS "main.cpp"
                            "app_camera.cpp"
                            "app_capture_uploader.cpp"
                            "app_http_server.cpp"
                            "app_ir_led.cpp"
                            "app_status_led.cpp"
                            "app_person_detect.cpp"
                            "app_wifi_ap.cpp"
                            "app_wifi_sta.cpp"
                            ${EI_RUNTIME_SRCS}
                       INCLUDE_DIRS "../include"
                                   "${APP_EDGE_IMPULSE_MODEL_SRC}"
                                   "${APP_EDGE_IMPULSE_MODEL_SRC}/edge-impulse-sdk"
                       REQUIRES esp-dl esp_driver_rmt esp_http_client)
