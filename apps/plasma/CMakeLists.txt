# Plasma: an animated effect on espix's screen. A POSIX app, built with tools/espix-cc:
#   cmake -S apps/plasma -B build -DCMAKE_TOOLCHAIN_FILE=cmake/espix-uapp.cmake [-DESPIX_TARGET=esp32s3]
cmake_minimum_required(VERSION 3.22)
project(plasma C)

if(NOT ESPIX_UAPP)
    message(FATAL_ERROR "configure with -DCMAKE_TOOLCHAIN_FILE=<espix>/cmake/espix-uapp.cmake")
endif()

add_executable(plasma main/plasma.c)
target_include_directories(plasma PRIVATE
    "${ESPIX_ROOT}/components/espix_proc/include" "${ESPIX_ROOT}/components/espix_display/include")
target_compile_options(plasma PRIVATE -Og -Wall)
