# espix example app — a standalone project, NOT part of the espix firmware.
#
# `project_elf()` comes from the elf_loader component and changes the link step
# so the output is a relocatable ELF the device can load at runtime, rather
# than a flashable firmware image. Build product: build/hello.app.elf
#
# Build:
#   idf.py -G 'Unix Makefiles' set-target esp32s3    # enables `idf.py elf`
#   idf.py elf
#
# Then copy build/hello.app.elf to ../../fsroot/bin/hello and run
# `idf.py storage-flash` in the firmware project, or push it over the wire once
# SCP exists.
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hello)

# off_t is 64 bits in the firmware, and struct stat is passed across the loader's
# ABI -- so the app has to agree. See cmake/offt64.h.
set(ESPIX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
include("${ESPIX_ROOT}/cmake/offt64.cmake")

include(elf_loader)
project_elf(hello)
