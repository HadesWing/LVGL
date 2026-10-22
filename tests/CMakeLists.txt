# =============================================================================
# 自研代码的单元测试（UT）
#
# 刻意做成**独立工程**，不塞进主 CMakeLists，原因和 tools/h264_probe 一样：
#
#   1) UT 只测纯逻辑（编解码、摘要、解析、重组、状态机），不需要 LVGL/SDL/oSIP。
#      塞进主工程就得先把这些重依赖装齐才能跑一个"测查表"的用例，本末倒置。
#   2) 交叉编译到板子上时，`find_package(SDL2 REQUIRED)` 会直接把 configure 挡死 ——
#      而"UT 能在目标机上跑"恰恰是最有价值的部分（板子的字节序、int 宽度、
#      浮点行为都可能和 PC 不同）。
#   3) 测试代码不进产品二进制。主工程一行都没被 UT 污染。
#
# 用法：
#   bash scripts/dev.sh ut              # 编译 + 跑全部
#   bash scripts/dev.sh ut g711         # 只跑 suite 名含 g711 的
#   make ut                             # 同上
#   交叉编译：
#     cmake -S tests -B build-ut-arm -DCMAKE_C_COMPILER=arm-linux-gnueabihf-gcc
#     cmake --build build-ut-arm
# =============================================================================
cmake_minimum_required(VERSION 3.12)
project(ut C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

# ---------------------------------------------------------------------------
# 被测源码（只挑不依赖 SDL/LVGL/oSIP 的部分）
#
# 每加一个模块在这里登记；没被登记的模块就等于没测试，一眼能看出来。
# ---------------------------------------------------------------------------
set(APP_SOURCES
    ${ROOT}/src/app/g711.c
    ${ROOT}/src/app/sip_md5.c
    ${ROOT}/src/app/sdp.c
    ${ROOT}/src/app/wav.c
    ${ROOT}/src/media/h264_depack.c
    ${ROOT}/src/oem/oem.c
    ${ROOT}/src/oem/oem_default.c     # oem.c 里的 profile 表在这两个文件里
    ${ROOT}/src/oem/oem_alt.c
)

# 用例文件（用 GLOB 便于随手加文件，CONFIGURE_DEPENDS 让新增文件自动触发重配）
file(GLOB UT_CASES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/ut_*.c)
list(SORT UT_CASES)

add_executable(ut ${CMAKE_CURRENT_SOURCE_DIR}/ut.c ${UT_CASES} ${APP_SOURCES})

target_include_directories(ut PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${ROOT}/src/app
    ${ROOT}/src/media
    ${ROOT}/src/oem)

# 警告开满：UT 的源码本身就是"给人看怎么用这些接口"的样板，
# 里面藏着隐式转换/未使用变量会带坏后来照着写的人。
if(NOT MSVC)
    target_compile_options(ut PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# 数学库：g711 的 SNR 用例要用 log10
if(NOT MSVC)
    target_link_libraries(ut m)
endif()

if(MINGW)
    target_link_libraries(ut "-static-libgcc")
endif()

set_target_properties(ut PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${ROOT}/bin)

message(STATUS "UT: ${CMAKE_CURRENT_SOURCE_DIR}/ut_*.c + ${APP_SOURCES}")
