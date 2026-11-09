# The plasma as a U-mode app on the S31: the same source as the M-mode build,
# compiled by tools/espix-cc. tools/build-apps.sh stages it as /bin/plasma-u.
cmake_minimum_required(VERSION 3.22)
project(plasma-u C)

if(NOT ESPIX_UAPP)
    message(FATAL_ERROR "configure with -DCMAKE_TOOLCHAIN_FILE=<espix>/cmake/espix-uapp.cmake")
endif()

add_executable(plasma "${CMAKE_CURRENT_LIST_DIR}/../main/plasma.c")
target_include_directories(plasma PRIVATE
    "${ESPIX_ROOT}/components/espix_proc/include" "${ESPIX_ROOT}/components/espix_display/include")
# -Og, as the M-mode build is compiled, so the two compare.
target_compile_options(plasma PRIVATE -Og -Wall)
