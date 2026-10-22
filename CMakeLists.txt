cmake_minimum_required(VERSION 3.12.4)
project(lvgl C CXX)

# Set the correct FreeRTOS port for your system (e.g., Posix for WSL)
set(FREERTOS_PORT GCC_POSIX CACHE STRING "Port for FreeRTOS on Posix environment")

set(LVGL_PRO_PROJECT_DIR
    ""
    CACHE PATH "Path to LVGL Pro Project folder")

option(USE_FREERTOS "Enable FreeRTOS" OFF) # Turn this on to enable FreeRTOS

if(USE_FREERTOS)
    message(STATUS "FreeRTOS is enabled")

    # FreeRTOS integration when USE_FREERTOS is enabled
    add_library(freertos_config INTERFACE)
    target_include_directories(freertos_config SYSTEM INTERFACE ${PROJECT_SOURCE_DIR}/config)
    target_compile_definitions(freertos_config INTERFACE projCOVERAGE_TEST=0)

    # Add FreeRTOS as a subdirectory
    add_subdirectory(FreeRTOS)

    # FreeRTOS-specific include directories
    include_directories(${PROJECT_SOURCE_DIR}/FreeRTOS/include)
    include_directories(${PROJECT_SOURCE_DIR}/FreeRTOS/portable/ThirdParty/GCC/Posix)
    include_directories(${PROJECT_SOURCE_DIR}/config)

    # Add FreeRTOS sources
    file(GLOB FREERTOS_SOURCES
        "${PROJECT_SOURCE_DIR}/FreeRTOS/*.c"
        "${PROJECT_SOURCE_DIR}/FreeRTOS/portable/MemMang/heap_4.c"
        "${PROJECT_SOURCE_DIR}/FreeRTOS/portable/ThirdParty/GCC/Posix/*.c"
    )
else()
    message(STATUS "FreeRTOS is disabled")
    set(FREERTOS_SOURCES "")  # No FreeRTOS sources if FreeRTOS is disabled
endif()

# Main include files of the project
include_directories(${PROJECT_SOURCE_DIR}/main/inc)

# Define options for LVGL with default values (OFF)
option(LV_USE_DRAW_SDL "Use SDL draw unit" OFF)
option(LV_USE_LIBPNG "Use libpng to decode PNG" OFF)
option(LV_USE_LIBJPEG_TURBO "Use libjpeg turbo to decode JPEG" OFF)
option(LV_USE_FFMPEG "Use libffmpeg to display video using lv_ffmpeg" OFF)
option(LV_USE_FREETYPE "Use freetype library" OFF)

# Set C and C++ standards
set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set the output path for the executable
set(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/bin)
set(WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

# Find and include SDL2 library
find_package(SDL2 REQUIRED)

# Add compile definitions based on the selected options
add_compile_definitions($<$<BOOL:${LV_USE_DRAW_SDL}>:LV_USE_DRAW_SDL=1>)
add_compile_definitions($<$<BOOL:${LV_USE_LIBPNG}>:LV_USE_LIBPNG=1>)
add_compile_definitions($<$<BOOL:${LV_USE_LIBJPEG_TURBO}>:LV_USE_LIBJPEG_TURBO=1>)
add_compile_definitions($<$<BOOL:${LV_USE_FFMPEG}>:LV_USE_FFMPEG=1>)

# ---------------------------------------------------------------------------
# 内存 / 调试参数（作用于 lvgl 库与 app，必须在 add_subdirectory(lvgl) 之前）
#   -DLV_MEM_SIZE=65536      LVGL 内存池大小（模拟 RK3308 这类小内存设备，默认 1MB）
#   -DLV_DEBUG_ASSERT=1      打开 LVGL 断言（找 bug 用；性能测试时必须关）
# ---------------------------------------------------------------------------
if(LV_MEM_SIZE)
    add_compile_definitions(LV_MEM_SIZE=${LV_MEM_SIZE})
    message(STATUS "LV_MEM_SIZE = ${LV_MEM_SIZE} bytes")
endif()
if(LV_DEBUG_ASSERT)
    add_compile_definitions(LV_DEBUG_ASSERT=1)
    message(STATUS "LVGL assertions: ON (性能会明显下降)")
endif()

# Add LVGL subdirectory
# 示例与 demo 默认会被编译（只是不链接），既浪费 2 分钟又会被 lv_conf 里的
# 字体裁剪搞挂（lv_demo_benchmark 硬引用 montserrat_20）。产品不需要它们。
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_DEMOS    OFF CACHE BOOL "" FORCE)
add_subdirectory(lvgl)
target_include_directories(lvgl PUBLIC ${PROJECT_SOURCE_DIR} ${SDL2_INCLUDE_DIRS})

# ---------------------------------------------------------------------------
# HAL 后端选择（一份 UI 代码，多平台）
#   -DLVGL_HAL=sdl      Windows / Linux 桌面模拟（默认，SDL2）
#   -DLVGL_HAL=fbdev    Linux 真机（/dev/fb0 + evdev，仅 Linux 可用，待实现）
# ---------------------------------------------------------------------------
set(LVGL_HAL "sdl" CACHE STRING "HAL backend: sdl | fbdev")

set(MAIN_SOURCES src/mouse_cursor_icon.c
                 src/ui/fonts/ui_font_cjk_16.c
                 src/ui/fonts/ui_font_cjk_24.c
                 src/oem/oem.c
                 src/oem/oem_default.c
                 src/oem/oem_alt.c
                 src/ui/ui_theme.c
                 src/ui/ui_common.c
                 src/ui/ui_nav.c
                 src/ui/ui_home.c
                 src/ui/ui_category.c
                 src/ui/ui_device.c
                 src/ui/ui_call.c
                 src/app/app_events.c
                 src/app/app_devices.c
                 src/app/app_stats.c
                 src/app/app_audio.c
                 src/app/g711.c
                 src/app/app_mock.c)

if(LVGL_HAL STREQUAL "fbdev")
    if(WIN32)
        message(FATAL_ERROR "LVGL_HAL=fbdev 只能在 Linux 上使用；Windows 请用 sdl")
    endif()
    if(NOT EXISTS ${PROJECT_SOURCE_DIR}/src/hal/hal_fbdev.c)
        message(FATAL_ERROR "src/hal/hal_fbdev.c 尚未实现（P1 任务：fbdev + evdev HAL）")
    endif()
    list(APPEND MAIN_SOURCES src/hal/hal_fbdev.c)
    message(STATUS "HAL backend: fbdev (Linux 真机)")
else()
    list(APPEND MAIN_SOURCES src/hal/hal_sdl.c)
    message(STATUS "HAL backend: SDL2 (桌面模拟)")
endif()

set(MAIN_LIBS lvgl ${SDL2_LIBRARIES})
# 可选目标：关掉示例/demo/ThorVG 时它们不会被创建（见 add_subdirectory(lvgl) 上方），
# 硬链接会直接让 cmake generate 失败 —— 用 TARGET 判断，裁剪开关才真正独立
foreach(_opt lvgl::examples lvgl::demos lvgl::thorvg)
    if(TARGET ${_opt})
        list(APPEND MAIN_LIBS ${_opt})
    endif()
endforeach()

# Create the main executable, depending on the FreeRTOS option
if(USE_FREERTOS)
    list(APPEND MAIN_SOURCES src/freertos_main.c src/freertos/freertos_posix_port.c ${FREERTOS_SOURCES})
    list(APPEND MAIN_LIBS freertos_config freertos_kernel)
else()
    list(APPEND MAIN_SOURCES src/main.c)
endif()


if(NOT MSVC)
    list(APPEND MAIN_LIBS m pthread)
endif()

# Custom target to run the executable
add_custom_target(run COMMAND ${EXECUTABLE_OUTPUT_PATH}/main DEPENDS main)

# Conditionally include and link SDL2_image if LV_USE_DRAW_SDL is enabled
if(LV_USE_DRAW_SDL)
    set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake")
    find_package(SDL2_image REQUIRED)
    target_include_directories(lvgl PUBLIC ${SDL2_IMAGE_INCLUDE_DIRS})
    list(APPEND MAIN_LIBS ${SDL2_IMAGE_LIBRARIES})
endif()

# Conditionally include and link libpng if LV_USE_LIBPNG is enabled
if(LV_USE_LIBPNG)
    find_package(PNG REQUIRED)
    target_include_directories(lvgl PUBLIC ${PNG_INCLUDE_DIRS})
    list(APPEND MAIN_LIBS ${PNG_LIBRARIES})
endif()

# Conditionally include and link libjpeg-turbo if LV_USE_LIBJPEG_TURBO is enabled
if(LV_USE_LIBJPEG_TURBO)
    find_package(JPEG REQUIRED)
    target_include_directories(lvgl PUBLIC ${JPEG_INCLUDE_DIRS})
    list(APPEND MAIN_LIBS ${JPEG_LIBRARIES})
endif()

# Conditionally include and link FFmpeg libraries if LV_USE_FFMPEG is enabled
if(LV_USE_FFMPEG)
    list(APPEND MAIN_LIBS avformat avcodec avutil swscale z)
endif()

# Conditionally include and link FreeType if LV_USE_FREETYPE is enabled
if(LV_USE_FREETYPE)
    find_package(Freetype REQUIRED)
    target_include_directories(lvgl PUBLIC ${FREETYPE_INCLUDE_DIRS})
    list(APPEND MAIN_LIBS ${FREETYPE_LIBRARIES})
endif()

if(LVGL_PRO_PROJECT_DIR)
  if(EXISTS "${LVGL_PRO_PROJECT_DIR}/CMakeLists.txt")
    add_subdirectory(${LVGL_PRO_PROJECT_DIR} ui)
    list(APPEND MAIN_LIBS lib-ui)

    target_compile_definitions(lib-ui PUBLIC LV_LVGL_H_INCLUDE_SIMPLE)
    target_include_directories(lib-ui PUBLIC
      ${LVGL_PRO_PROJECT_DIR}  # local headers like ui.h, generated files, etc.
      ${CMAKE_SOURCE_DIR}/lvgl # include "lvgl.h" works
    )
    message(STATUS "LVGL Pro Project enabled from: ${LVGL_PRO_PROJECT_DIR}")
  else()
    message(
      WARNING
        "LVGL_PRO_PROJECT_DIR specified but no CMakeLists.txt found at: ${LVGL_PRO_PROJECT_DIR}"
    )
  endif()
endif()

# ---------------------------------------------------------------------------
# SIP 对讲（可选）：基于 libosip2
#   -DOSIP_ROOT=<prefix>  指定 libosip2 安装前缀（默认 C:/osip_build/install）
#   找不到就自动关闭，不影响 UI demo 编译（构建脚本见 third_party/BUILD_OSIP.md）
# ---------------------------------------------------------------------------
set(OSIP_ROOT "C:/osip_build/install" CACHE PATH "libosip2 install prefix")
set(APP_WITH_SIP OFF)

# 查找顺序：用户指定前缀 → third_party/osip-install（本机源码构建的产物目录）
find_path(OSIP_INCLUDE_DIR osip2/osip.h PATHS
    ${OSIP_ROOT}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/osip-install/include
    NO_DEFAULT_PATH)
find_library(OSIP_LIB osip2 PATHS
    ${OSIP_ROOT}/lib
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/osip-install/lib
    NO_DEFAULT_PATH)
find_library(OSIPPARSER_LIB osipparser2 PATHS
    ${OSIP_ROOT}/lib
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/osip-install/lib
    NO_DEFAULT_PATH)

if(OSIP_INCLUDE_DIR AND OSIP_LIB AND OSIPPARSER_LIB)
    set(APP_WITH_SIP ON)
    list(APPEND MAIN_SOURCES src/app/app_sip.c src/app/sip_md5.c)
    message(STATUS "SIP: enabled (libosip2 at ${OSIP_ROOT})")
else()
    message(STATUS "SIP: disabled (libosip2 not found, see third_party/BUILD_OSIP.md)")
endif()

add_executable(main ${MAIN_SOURCES})

# 可执行文件名可覆盖：不同内存池/断言版本各出自己的文件，避免互相覆盖 bin/main.exe
#   例：-DMAIN_EXE_NAME=main64 → bin/main64.exe
set(MAIN_EXE_NAME "main" CACHE STRING "Output executable name (variants: main64, main-dbg, ...)")
if(NOT MAIN_EXE_NAME STREQUAL "main")
    set_target_properties(main PROPERTIES OUTPUT_NAME "${MAIN_EXE_NAME}")
endif()

# 分层头文件路径：ui / app / hal 各自 include 时不用写相对路径
target_include_directories(main PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/ui
    ${PROJECT_SOURCE_DIR}/src/app
    ${PROJECT_SOURCE_DIR}/src/oem
    ${PROJECT_SOURCE_DIR}/src/hal)
target_compile_definitions(main PRIVATE LV_CONF_INCLUDE_SIMPLE)
if(APP_WITH_SIP)
    target_compile_definitions(main PRIVATE APP_WITH_SIP=1)
    target_include_directories(main PRIVATE ${OSIP_INCLUDE_DIR})
    target_link_libraries(main ${OSIP_LIB} ${OSIPPARSER_LIB})
    if(WIN32)
        target_link_libraries(main ws2_32)
    endif()
endif()
target_link_libraries(main ${MAIN_LIBS})

# On Windows, GUI applications do not show a console by default,
# which hides log output. This ensures a console is available for logging.
if (WIN32)
    if (MSVC)
        target_link_options(main PRIVATE "/SUBSYSTEM:CONSOLE")
    else()
        target_link_options(main PRIVATE "-mconsole")
    endif()
endif()

# Apply additional compile options if the build type is Debug
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(STATUS "Debug mode enabled")

    target_compile_options(lvgl PRIVATE
        -pedantic-errors
        -Wall
        -Wclobbered
        -Wdeprecated
        -Wdouble-promotion
        -Wempty-body
        -Wextra
        -Wformat-security
        -Wmaybe-uninitialized
        # -Wmissing-prototypes
        -Wpointer-arith
        -Wmultichar
        -Wno-pedantic # ignored for now, we convert functions to pointers for properties table.
        -Wreturn-type
        -Wshadow
        -Wshift-negative-value
        -Wsizeof-pointer-memaccess
        -Wtype-limits
        -Wundef
        -Wuninitialized
        -Wunreachable-code
        -Wfloat-conversion
        -Wstrict-aliasing
    )

if (ASAN)
    message(STATUS "AddressSanitizer enabled")

    # Add AddressSanitizer flags
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -fno-omit-frame-pointer")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address -fno-omit-frame-pointer")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=address")
else()
    message(STATUS "AddressSanitizer disabled")
endif()
endif()
