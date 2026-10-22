# h264_probe —— 独立的 H.264 离线解码探针
#
# 刻意不放进主工程：只依赖 libavcodec/libavutil/libswscale，
# 不需要 LVGL、SDL、oSIP，单独一个 project 才好交叉编译到板子上。
#
#   PC 上：      cmake -S tools/h264_probe -B build-h264 && cmake --build build-h264
#   RK3308 上：  cmake -S tools/h264_probe -B build-h264-arm \
#                     -DCMAKE_C_COMPILER=arm-linux-gnueabihf-gcc \
#                     -DAV_ROOT=<交叉编译版 ffmpeg 的 install 前缀>
#
# 交叉编译时 pkg-config 会去找宿主机的 .pc，所以用 AV_ROOT 显式指定，
# 见 docs/音视频编解码实现方案.md §5.1 的 FFmpeg 裁剪参数。
cmake_minimum_required(VERSION 3.12)
project(h264_probe C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(AV_ROOT "" CACHE PATH "交叉编译版 FFmpeg 的 install 前缀（留空则用 pkg-config）")

if(AV_ROOT)
    find_path(AV_INCLUDE_DIR libavcodec/avcodec.h PATHS ${AV_ROOT}/include NO_DEFAULT_PATH)
    find_library(AVCODEC_LIB avcodec PATHS ${AV_ROOT}/lib NO_DEFAULT_PATH)
    find_library(AVUTIL_LIB avutil PATHS ${AV_ROOT}/lib NO_DEFAULT_PATH)
    find_library(SWSCALE_LIB swscale PATHS ${AV_ROOT}/lib NO_DEFAULT_PATH)
    if(NOT AV_INCLUDE_DIR OR NOT AVCODEC_LIB)
        message(FATAL_ERROR "AV_ROOT=${AV_ROOT} 下找不到 libavcodec")
    endif()
    add_executable(h264_probe main.c)
    target_include_directories(h264_probe PRIVATE ${AV_INCLUDE_DIR})
    target_link_libraries(h264_probe PRIVATE ${AVCODEC_LIB} ${AVUTIL_LIB} ${SWSCALE_LIB})
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(AV REQUIRED IMPORTED_TARGET libavcodec libavutil libswscale)
    add_executable(h264_probe main.c)
    target_link_libraries(h264_probe PRIVATE PkgConfig::AV)
endif()

if(MINGW)
    target_link_libraries(h264_probe PRIVATE "-static-libgcc")
endif()

set_target_properties(h264_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../../bin)
