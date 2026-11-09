# Doom as a U-mode app on the S31: the same engine, platform layer and entry as
# the M-mode build, compiled by tools/espix-cc. tools/build-apps.sh builds it
# after the M-mode app, whose configure fetches the engine, and stages it as
# /bin/doom-u.
cmake_minimum_required(VERSION 3.22)
project(doom-u C)

if(NOT ESPIX_UAPP)
    message(FATAL_ERROR "configure with -DCMAKE_TOOLCHAIN_FILE=<espix>/cmake/espix-uapp.cmake")
endif()

get_filename_component(APP "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
include("${APP}/sources.cmake")
if(NOT DG_SRCS)
    message(FATAL_ERROR "no engine in ${DG_DIR}: build the M-mode app first (tools/build-apps.sh doom)")
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
# -Og, as the M-mode build is compiled, so the two compare.
target_compile_options(doom PRIVATE -Og -Wall)
target_link_libraries(doom m)
