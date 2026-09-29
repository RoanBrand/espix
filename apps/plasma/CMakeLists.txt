# espix example app -- a standalone project, NOT part of the espix firmware.
#
# Build product: build/plasma.app.elf, which tools/build-apps.sh stages into
# fsroot/bin/plasma so `plasma` runs from the shell.
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(plasma)

set(ESPIX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
include("${ESPIX_ROOT}/cmake/offt64.cmake")

include(elf_loader)
project_elf(plasma)
