cmake_minimum_required(VERSION 3.20)
project(ImageEdit LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT MSVC)
  message(FATAL_ERROR "This project targets MSVC / Windows.")
endif()

set(APP_DEFS UNICODE _UNICODE WINVER=0x0A00 _WIN32_WINNT=0x0A00 NOMINMAX)

add_library(imagecore STATIC src/image_util.cpp)
target_include_directories(imagecore PUBLIC src)
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
