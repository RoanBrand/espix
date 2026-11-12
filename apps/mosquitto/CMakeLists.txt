# The mosquitto MQTT broker. A POSIX app, built with tools/espix-cc
# (cmake/espix-uapp.cmake); fetch-mosquitto.sh pulls the broker and cJSON into
# third_party/ at configure.
cmake_minimum_required(VERSION 3.22)
project(mosquitto C)

if(NOT ESPIX_UAPP)
    message(FATAL_ERROR "configure with -DCMAKE_TOOLCHAIN_FILE=<espix>/cmake/espix-uapp.cmake")
endif()

execute_process(COMMAND "${CMAKE_CURRENT_LIST_DIR}/fetch-mosquitto.sh" RESULT_VARIABLE mosq_fetch_rc)
if(NOT mosq_fetch_rc EQUAL 0)
    message(FATAL_ERROR "fetch-mosquitto.sh failed: no broker source")
endif()

set(APP "${CMAKE_CURRENT_LIST_DIR}")
include("${APP}/sources.cmake")

set(CJSON "${APP}/third_party/cJSON")

add_executable(mosquitto ${MOSQ_SRC} "${CJSON}/cJSON.c" "${APP}/components/mosquitto_espix/espix_platform.c")
target_include_directories(mosquitto PRIVATE
    "${MOSQ}/include" ${MOSQ_INC} "${APP}/components/mosquitto_espix/include" "${CJSON}")
target_compile_definitions(mosquitto PRIVATE ${MOSQ_DEFS})
target_compile_options(mosquitto PRIVATE -Os ${MOSQ_OPTS})
