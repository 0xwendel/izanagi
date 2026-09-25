cmake_minimum_required(VERSION 3.20)

project(izanagi LANGUAGES CXX)

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

# /MT for release-like configurations and /MTd for Debug. This keeps CRT
# ownership inside the DLL and avoids a runtime dependency on the host CRT.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(izanagi SHARED
    src/debug_console.cpp
    src/dllmain.cpp
    src/runtime.cpp
)

target_include_directories(izanagi PRIVATE src)
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
target_link_libraries(izanagi PRIVATE kernel32 user32)

set_target_properties(izanagi PROPERTIES
    CXX_EXTENSIONS OFF
    OUTPUT_NAME "izanagi"
)
