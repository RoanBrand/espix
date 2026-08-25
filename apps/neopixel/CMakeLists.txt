# espix example app — an Arduino sketch, cross-compiled for espix.
#
# Arduino is a dependency of THIS APP, not of the espix firmware: the app is its
# own IDF project, so it can pull arduino-esp32 in as a component while espix
# itself stays Arduino-free. CONFIG_AUTOSTART_ARDUINO=n keeps Arduino a library
# rather than letting it own app_main.
#
# Build:
#   export ARDUINO_SKIP_TICK_CHECK=1     # espix runs a 100Hz tick; see the sketch
#   idf.py -G 'Unix Makefiles' set-target esp32s3
#   idf.py elf
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(neopixel)

# arduino-esp32 compiles its whole bundled library set, and some of it trips
# IDF 6.1 headers with -Werror=missing-field-initializers. Not our code and not
# our bug; downgrade it rather than patch a managed component.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=missing-field-initializers" APPEND)

include(elf_loader)

# project_elf() links only `main` unless told otherwise, so the Arduino core and
# the NeoPixel library have to be named here or their objects never reach the
# ELF and every Arduino call stays undefined.
# Order matters: these become plain static archives on one link line, resolved
# left to right. Adafruit_NeoPixel references Arduino's rmtInit/pinMode, so it
# has to come first or the linker has already walked past the core's archive by
# the time those references appear -- leaving them undefined, with -nostdlib
# -shared happily producing an ELF anyway.
set(ELF_COMPONENTS Adafruit_NeoPixel espressif__arduino-esp32)

# Fold global C++ constructors into .data so the loader can relocate them, and
# name their bounds so the startup shim can run them. See ctors.ld. project_elf()
# appends ELF_LIBS verbatim to its link command; the comma form is required
# because it puts the list through spaces2list().
set(ELF_LIBS "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/ctors.ld")

project_elf(neopixel)
