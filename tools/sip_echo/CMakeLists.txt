# sip_echo —— 独立的板端自测工具
#
# 刻意不放进主工程：主 CMakeLists 里 find_package(SDL2 REQUIRED)，
# 而应答器在板子上既不需要 SDL 也不需要 LVGL —— 单独一个 project 才好交叉编译。
#
#   PC 上：      cmake -S tools/sip_echo -B build-echo && cmake --build build-echo
#   RK3308 上：  cmake -S tools/sip_echo -B build-echo-arm \
#                     -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
#                     -DOSIP_ROOT=<aarch64 版 libosip2 前缀>
cmake_minimum_required(VERSION 3.12)
project(sip_echo C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(OSIP_ROOT "C:/osip_build/install" CACHE PATH "libosip2 install prefix")

find_path(OSIP_INCLUDE_DIR osip2/osip.h PATHS
    ${OSIP_ROOT}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/osip-install/include
    NO_DEFAULT_PATH)
find_library(OSIP_LIB osip2 PATHS
    ${OSIP_ROOT}/lib
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/osip-install/lib
    NO_DEFAULT_PATH)
find_library(OSIPPARSER_LIB osipparser2 PATHS
    ${OSIP_ROOT}/lib
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/osip-install/lib
    NO_DEFAULT_PATH)

if(NOT OSIP_INCLUDE_DIR OR NOT OSIP_LIB OR NOT OSIPPARSER_LIB)
    message(FATAL_ERROR
        "找不到 libosip2。请用 -DOSIP_ROOT=<prefix> 指定，"
        "或先按 third_party/BUILD_OSIP.md 构建。")
endif()

add_executable(sip_echo main.c)
target_include_directories(sip_echo PRIVATE ${OSIP_INCLUDE_DIR})
target_link_libraries(sip_echo ${OSIP_LIB} ${OSIPPARSER_LIB})

if(WIN32)
    target_link_libraries(sip_echo ws2_32)
else()
    target_link_libraries(sip_echo m)
endif()

set_target_properties(sip_echo PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../../bin)
