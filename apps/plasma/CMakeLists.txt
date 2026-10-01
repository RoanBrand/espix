# espix example app -- a standalone project, NOT part of the espix firmware.
#
# Build product: build/plasma.app.elf, which tools/build-apps.sh stages into
# fsroot/bin/plasma so `plasma` runs from the shell.
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(plasma)

# An espix app: this one line is what says so, and what makes the app agree with
# the kernel about the ABI they share. See cmake/espix-app.cmake.
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/espix-app.cmake")

include(elf_loader)
project_elf(plasma)
