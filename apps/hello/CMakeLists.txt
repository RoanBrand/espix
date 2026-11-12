# hello: the smallest espix app. A POSIX program, built with tools/espix-cc:
#   cmake -S apps/hello -B build -DCMAKE_TOOLCHAIN_FILE=cmake/espix-uapp.cmake [-DESPIX_TARGET=esp32s3]
cmake_minimum_required(VERSION 3.22)
project(hello C)

if(NOT ESPIX_UAPP)
    message(FATAL_ERROR "configure with -DCMAKE_TOOLCHAIN_FILE=<espix>/cmake/espix-uapp.cmake")
endif()

add_executable(hello main/hello.c)
target_compile_options(hello PRIVATE -Os -Wall)
