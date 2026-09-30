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
project(doom)

set(ESPIX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
include("${ESPIX_ROOT}/cmake/offt64.cmake")

include(elf_loader)
set(ELF_COMPONENTS doomgeneric doomplatform)
project_elf(doom)
