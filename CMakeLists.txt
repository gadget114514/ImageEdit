cmake_minimum_required(VERSION 3.20)
project(ImageEdit VERSION 0.9.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT MSVC)
  message(FATAL_ERROR "This project targets MSVC / Windows.")
endif()

set(APP_DEFS UNICODE _UNICODE WINVER=0x0A00 _WIN32_WINNT=0x0A00 NOMINMAX)

include(FetchContent)
set(WEBP_BUILD_ANIM_UTILS OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_CWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_DWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_GIF2WEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_IMG2WEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_VWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPINFO OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_LIBWEBPMUX OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPMUX OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_FUZZTEST OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  libwebp
  URL https://github.com/webmproject/libwebp/archive/refs/tags/v1.6.0.tar.gz
  URL_HASH SHA256=93a852c2b3efafee3723efd4636de855b46f9fe1efddd607e1f42f60fc8f2136
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(libwebp)

add_library(imagecore STATIC src/image_util.cpp src/webp_codec.cpp)
target_include_directories(imagecore PUBLIC src)
target_link_libraries(imagecore PUBLIC webp)
target_compile_definitions(imagecore PUBLIC ${APP_DEFS})
target_compile_options(imagecore PRIVATE /utf-8 /W4 /EHsc /permissive-)

add_executable(ImageEdit WIN32 src/main.cpp src/editor.cpp)
target_link_libraries(ImageEdit PRIVATE
  imagecore user32 gdi32 gdiplus comctl32 comdlg32 shlwapi shell32 ole32 uuid)
target_compile_options(ImageEdit PRIVATE /utf-8 /W4 /EHsc /permissive-)

add_executable(image_util_test tests/test_image_util.cpp)
target_link_libraries(image_util_test PRIVATE
  imagecore user32 gdi32 gdiplus shlwapi ole32)
target_compile_options(image_util_test PRIVATE /utf-8 /W4 /EHsc /permissive-)
