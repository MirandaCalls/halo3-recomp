cmake_minimum_required(VERSION 3.20)
project(xenon_gpu CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# xxHash (header-only) from the ReXGlue tree for now.
set(XGPU_XXHASH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ref/rexglue-resync/thirdparty/xxHash"
    CACHE PATH "Directory containing xxhash.h")

add_library(xgpu_ucode STATIC src/ucode/cf_parse.cpp)
target_include_directories(xgpu_ucode PUBLIC include)

# Capture loading and the PM4 front end (no dependency on the emulator).
add_library(xgpu_frontend STATIC src/capture/capture.cpp src/pm4/front_end.cpp)
target_include_directories(xgpu_frontend PUBLIC include PRIVATE ${XGPU_XXHASH_DIR})

add_executable(xgpu-frame tools/frame_graph.cpp)
target_link_libraries(xgpu-frame PRIVATE xgpu_frontend)

# Native renderer (Vulkan).
find_package(Vulkan REQUIRED)
add_library(xgpu_native STATIC src/vk/context.cpp src/native/render_targets.cpp)
target_include_directories(xgpu_native PUBLIC include)
target_link_libraries(xgpu_native PUBLIC Vulkan::Vulkan)

add_executable(xgpu-replay tools/replay.cpp)
target_link_libraries(xgpu-replay PRIVATE xgpu_frontend xgpu_native)

add_executable(xgpu-shader-scan tools/shader_scan.cpp)
target_include_directories(xgpu-shader-scan PRIVATE ${XGPU_XXHASH_DIR})
target_link_libraries(xgpu-shader-scan PRIVATE xgpu_ucode)
