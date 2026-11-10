cmake_minimum_required(VERSION 3.23)
project(nvfair LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
enable_testing()

# Open GPU Kernel Modules 官方头(RM ioctl 协议唯一事实源,见 docs/04)
# submodule pin = 570.211.01,与本机驱动一致;升级 = 换 pin + 重跑 ABI 兼容测试
set(OGKM_DIR ${CMAKE_SOURCE_DIR}/third_party/open-gpu-kernel-modules)
if(NOT EXISTS ${OGKM_DIR}/src/nvidia/arch/nvalloc/unix/include/nv_escape.h)
    message(FATAL_ERROR
        "open-gpu-kernel-modules 子模块未初始化,请执行:\n"
        "  git submodule update --init")
endif()

add_library(nvfair SHARED src/ioctl_hook.cpp)
target_compile_options(nvfair PRIVATE -fvisibility=hidden -Wall -Wextra)
target_include_directories(nvfair SYSTEM PRIVATE
    ${OGKM_DIR}/src/common/sdk/nvidia/inc
    ${OGKM_DIR}/src/nvidia/arch/nvalloc/unix/include
    ${OGKM_DIR}/kernel-open/common/inc)

include(FetchContent)
FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz)
FetchContent_MakeAvailable(googletest)
