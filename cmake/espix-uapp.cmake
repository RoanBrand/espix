# The CMake toolchain file for U-mode apps on the S31 (docs/USER-MODE.md):
# tools/espix-cc as the C compiler, so an app's own CMake project builds a
# static U-mode executable with nothing else to say.
#
#   cmake -S apps/<app>/uapp -B apps/<app>/build-u -DCMAKE_TOOLCHAIN_FILE=cmake/espix-uapp.cmake
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR riscv32)

get_filename_component(ESPIX_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(CMAKE_C_COMPILER "${ESPIX_ROOT}/tools/espix-cc")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(ESPIX_UAPP ON)
