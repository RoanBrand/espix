# Doom: the doomgeneric engine on espix's screen, sound and keyboard. A POSIX app,
# built with tools/espix-cc (cmake/espix-uapp.cmake). The engine and the game data
# are not in the tree: fetch-doom.sh pulls both into third_party/ at configure.
cmake_minimum_required(VERSION 3.22)
project(doom C)

if(NOT ESPIX_UAPP)
    message(FATAL_ERROR "configure with -DCMAKE_TOOLCHAIN_FILE=<espix>/cmake/espix-uapp.cmake")
endif()

execute_process(COMMAND "${CMAKE_CURRENT_LIST_DIR}/fetch-doom.sh" RESULT_VARIABLE doom_fetch_rc)
if(NOT doom_fetch_rc EQUAL 0)
    message(FATAL_ERROR "fetch-doom.sh failed: no engine source or no WAD")
endif()

set(APP "${CMAKE_CURRENT_LIST_DIR}")
include("${APP}/sources.cmake")
if(NOT DG_SRCS)
    message(FATAL_ERROR "no engine in ${DG_DIR}")
endif()

add_library(engine OBJECT ${DG_SRCS})
target_include_directories(engine PUBLIC ${DG_INC})
target_compile_definitions(engine PUBLIC ${DG_DEFS})
target_compile_options(engine PRIVATE -Og ${DG_OPTS})

set(PLATFORM "${APP}/components/doomplatform")
add_executable(doom "${APP}/main/main.c" "${PLATFORM}/doom_espix.c" "${PLATFORM}/doom_sound.c"
               $<TARGET_OBJECTS:engine>)
target_include_directories(doom PRIVATE ${DG_INC} "${PLATFORM}"
    "${ESPIX_ROOT}/components/espix_proc/include"
    "${ESPIX_ROOT}/components/espix_display/include"
    "${ESPIX_ROOT}/components/espix_audio/include")
target_compile_definitions(doom PRIVATE ${DG_DEFS} ESPIX_SFX_TRACE=0)
target_compile_options(doom PRIVATE -Og -Wall)
target_link_libraries(doom m)
