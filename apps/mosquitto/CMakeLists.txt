# espix example app -- the Eclipse Mosquitto broker, built as a standalone
# project.
#
# The broker is NOT in the tree. fetch-mosquitto.sh pulls it into third_party/
# (gitignored) before the component globs its sources, so a fresh clone builds
# with one network round trip and the repository carries none of it.
#
# Build product: build/mosquitto.app.elf, which tools/build-apps.sh stages into
# fsroot/bin/mosquitto so `mosquitto -c /etc/mosquitto.conf` runs from the
# shell.
cmake_minimum_required(VERSION 3.22)

execute_process(COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/fetch-mosquitto.sh"
                RESULT_VARIABLE mosq_fetch_rc)
if(NOT mosq_fetch_rc EQUAL 0)
    message(FATAL_ERROR "fetch-mosquitto.sh failed: no broker source")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mosquitto)

# An espix app: this one line is what says so, and what makes the app agree with
# the kernel about the ABI they share. See cmake/espix-app.cmake.
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/espix-app.cmake")

# The broker's own main() is the app entry: project_elf() compiles with
# -Dmain=app_main, so nothing wraps it.
#
# Every archive the ELF needs, in resolution order. project_elf() links these
# and nothing else, as plain archives scanned left to right: a library has to
# come *after* the component that calls it, or its members are never pulled in
# and the symbols stay undefined for the device to refuse at load time. So the
# broker comes first, then the seam and cJSON that answer it. A managed
# component is named by its directory (espressif__cjson): that is what names
# both the component target and the archive project_elf() links.
include(elf_loader)
set(ELF_COMPONENTS mosquitto mosquitto_espix espressif__cjson)
project_elf(mosquitto)
