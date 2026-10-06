# espix example app -- Doom, built as a standalone project.
#
# The engine and the game data are NOT in the tree. fetch-doom.sh pulls both
# into third_party/ (gitignored) before the engine component globs its
# sources, so a fresh clone builds with one network round trip and the
# repository carries neither GPL engine code nor game data.
#
# Build product: build/doom.app.elf, which tools/build-apps.sh stages into
# fsroot/bin/doom so `doom` runs from the shell or a desktop icon.
cmake_minimum_required(VERSION 3.22)

execute_process(COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/fetch-doom.sh"
                RESULT_VARIABLE doom_fetch_rc)
if(NOT doom_fetch_rc EQUAL 0)
    message(FATAL_ERROR "fetch-doom.sh failed: no engine source or no WAD")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# _MB_EXTENDED_CHARSETS_ISO: the ctype names must be calls, not macros. The Xtensa backend's
# islower/toupper macro answers wrong for a runtime argument (docs/UPSTREAM.md),
# which is what broke Doom's lump-name lookup; espix publishes the functions.
# The builtins are disabled too, so the compiler cannot fold the calls back.
idf_build_set_property(COMPILE_DEFINITIONS "_MB_EXTENDED_CHARSETS_ISO" APPEND)
idf_build_set_property(COMPILE_OPTIONS
    "-fno-builtin-toupper;-fno-builtin-tolower" APPEND)

project(doom)

# An espix app: this one line is what says so, and what makes the app agree with
# the kernel about the ABI they share. See cmake/espix-app.cmake.
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/espix-app.cmake")

include(elf_loader)
set(ELF_COMPONENTS doomgeneric doomplatform)
project_elf(doom)
