# The CMake toolchain file for POSIX apps (docs/USER-MODE.md): tools/espix-cc as
# the C compiler, so an app's own CMake project builds with nothing else to say.
# ESPIX_TARGET picks the chip: esp32s31 (the default), a static U-mode
# executable; esp32s3, an app without an MMU.
#
#   cmake -S apps/<app> -B apps/<app>/build -DCMAKE_TOOLCHAIN_FILE=cmake/espix-uapp.cmake
#   cmake ... -DESPIX_TARGET=esp32s3
set(CMAKE_SYSTEM_NAME Generic)
if(NOT ESPIX_TARGET)
    set(ESPIX_TARGET esp32s31)
endif()
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES ESPIX_TARGET)
if(ESPIX_TARGET STREQUAL "esp32s3")
    set(CMAKE_SYSTEM_PROCESSOR xtensa)
else()
    set(CMAKE_SYSTEM_PROCESSOR riscv32)
endif()

get_filename_component(ESPIX_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(CMAKE_C_COMPILER "${ESPIX_ROOT}/tools/espix-cc")
set(CMAKE_C_FLAGS_INIT "--target=${ESPIX_TARGET}")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(ESPIX_UAPP ON)
