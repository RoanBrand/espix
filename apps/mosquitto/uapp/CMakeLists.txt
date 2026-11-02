# The broker as a U-mode app on the S31: the same sources and seam as the
# M-mode build, compiled by tools/espix-cc. tools/build-apps.sh builds it after
# the M-mode app, whose build fetches the broker and cJSON, and stages it as
# /bin/mosquitto-u.
cmake_minimum_required(VERSION 3.22)
project(mosquitto-u C)

if(NOT ESPIX_UAPP)
    message(FATAL_ERROR "configure with -DCMAKE_TOOLCHAIN_FILE=<espix>/cmake/espix-uapp.cmake")
endif()

get_filename_component(APP "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
include("${APP}/sources.cmake")

set(CJSON "${APP}/managed_components/espressif__cjson/cJSON")
if(NOT EXISTS "${CJSON}/cJSON.c")
    message(FATAL_ERROR "no cJSON in ${CJSON}: build the M-mode app first (tools/build-apps.sh mosquitto)")
endif()

add_executable(mosquitto ${MOSQ_SRC} "${CJSON}/cJSON.c" "${APP}/components/mosquitto_espix/espix_platform.c")
target_include_directories(mosquitto PRIVATE
    "${MOSQ}/include" ${MOSQ_INC} "${APP}/components/mosquitto_espix/include" "${CJSON}")
target_compile_definitions(mosquitto PRIVATE ${MOSQ_DEFS})
target_compile_options(mosquitto PRIVATE -Os ${MOSQ_OPTS})
