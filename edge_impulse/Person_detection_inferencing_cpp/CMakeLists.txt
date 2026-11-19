cmake_minimum_required(VERSION 3.16)

project(PersonDetectionCppEval LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(EI_MODEL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" CACHE PATH "Root directory of the generated Edge Impulse export")

if(NOT EXISTS "${EI_MODEL_ROOT}/model-parameters/model_metadata.h")
    message(FATAL_ERROR
        "The generated Edge Impulse model metadata was not found under ${EI_MODEL_ROOT}/model-parameters.\n"
        "Generate the Linux C++ library export into this directory first."
    )
endif()

file(GLOB_RECURSE EI_SDK_SOURCES
    "${EI_MODEL_ROOT}/edge-impulse-sdk/**/*.c"
    "${EI_MODEL_ROOT}/edge-impulse-sdk/**/*.cc"
    "${EI_MODEL_ROOT}/edge-impulse-sdk/**/*.cpp"
)

file(GLOB_RECURSE EI_MODEL_SOURCES
    "${EI_MODEL_ROOT}/tflite-model/*.c"
    "${EI_MODEL_ROOT}/tflite-model/*.cc"
    "${EI_MODEL_ROOT}/tflite-model/*.cpp"
)

list(APPEND EI_ALL_SOURCES ${EI_SDK_SOURCES} ${EI_MODEL_SOURCES})
list(REMOVE_DUPLICATES EI_ALL_SOURCES)

if(EI_ALL_SOURCES STREQUAL "")
    message(FATAL_ERROR "No Edge Impulse SDK or model sources were found in the generated export")
endif()

add_executable(person_detection_eval src/main.cpp ${EI_ALL_SOURCES})

target_include_directories(person_detection_eval PRIVATE
    "${EI_MODEL_ROOT}"
    "${EI_MODEL_ROOT}/edge-impulse-sdk"
    "${EI_MODEL_ROOT}/model-parameters"
    "${EI_MODEL_ROOT}/tflite-model"
)

target_compile_definitions(person_detection_eval PRIVATE EI_CLASSIFIER_TFLITE_ENABLE=1)
target_link_libraries(person_detection_eval PRIVATE m)
