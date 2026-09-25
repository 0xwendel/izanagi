cmake_minimum_required(VERSION 3.20)

project(izanagi LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "izanagi requires a Windows target")
endif()

if(NOT MSVC)
    message(FATAL_ERROR "izanagi requires the MSVC toolchain")
endif()

if(CMAKE_GENERATOR_PLATFORM AND NOT CMAKE_GENERATOR_PLATFORM STREQUAL "x64")
    message(FATAL_ERROR "izanagi requires the x64 generator platform")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "izanagi requires a 64-bit target")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(MINHOOK_SOURCES
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde64.c
)

add_library(izanagi_minhook STATIC ${MINHOOK_SOURCES})

target_include_directories(izanagi_minhook
    PUBLIC
        third_party/minhook/include
    PRIVATE
        third_party/minhook/src
        third_party/minhook/src/hde
)

target_compile_definitions(izanagi_minhook PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_compile_options(izanagi_minhook PRIVATE /W3 /WX-)

add_library(izanagi SHARED
    src/debug_console.cpp
    src/dllmain.cpp
    src/runtime.cpp
    src/dx11_hook.cpp
    src/gui.cpp
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp
    third_party/imgui/backends/imgui_impl_dx11.cpp
)

target_include_directories(izanagi PRIVATE
    src
    third_party/minhook/include
    third_party/imgui
    third_party/imgui/backends
)
target_compile_features(izanagi PRIVATE cxx_std_20)
target_compile_options(izanagi PRIVATE
    /W4
    /WX
    /permissive-
    /EHsc
    /utf-8
)
target_compile_definitions(izanagi PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

set(IMGUI_SOURCES
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp
    third_party/imgui/backends/imgui_impl_dx11.cpp
)
set_source_files_properties(${IMGUI_SOURCES} PROPERTIES
    COMPILE_OPTIONS "/W3;/WX-"
)

target_link_libraries(izanagi PRIVATE
    izanagi_minhook
    d3d11
    dxgi
    d3dcompiler
    user32
    kernel32
    gdi32
    dwmapi
)

set_target_properties(izanagi PROPERTIES
    CXX_EXTENSIONS OFF
    OUTPUT_NAME "izanagi"
)
