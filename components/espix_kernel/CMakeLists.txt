#
# The version lives in version.txt at the repository root. ESP-IDF reads the same
# file for PROJECT_VER, so the string compiled in here and the one OTA tooling
# reads out of the app descriptor are the same by construction rather than by
# discipline.
#
set(espix_version_file "${CMAKE_CURRENT_LIST_DIR}/../../version.txt")
if(NOT EXISTS "${espix_version_file}")
    message(FATAL_ERROR "espix: ${espix_version_file} is missing")
endif()
file(READ "${espix_version_file}" espix_version)
string(STRIP "${espix_version}" espix_version)
if(NOT espix_version MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "espix: version.txt must be MAJOR.MINOR.PATCH, got '${espix_version}'")
endif()
string(REPLACE "." ";" espix_version_parts "${espix_version}")
list(GET espix_version_parts 0 ESPIX_VERSION_MAJOR)
list(GET espix_version_parts 1 ESPIX_VERSION_MINOR)
list(GET espix_version_parts 2 ESPIX_VERSION_PATCH)

#
# A release build is the tagged commit, clean. Anything else is a development
# build, and the greeting says so -- which is the only reason motd needs to know.
#
execute_process(
    COMMAND git describe --tags --exact-match HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/../.."
    OUTPUT_VARIABLE espix_tag OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET RESULT_VARIABLE espix_tag_rc)
execute_process(
    COMMAND git status --porcelain
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/../.."
    OUTPUT_VARIABLE espix_status OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET RESULT_VARIABLE espix_status_rc)
execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/../.."
    OUTPUT_VARIABLE espix_git_describe OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)

if(espix_tag_rc EQUAL 0 AND espix_tag STREQUAL "v${espix_version}"
   AND espix_status_rc EQUAL 0 AND espix_status STREQUAL "")
    set(ESPIX_BUILD_IS_RELEASE 1)
else()
    set(ESPIX_BUILD_IS_RELEASE 0)
endif()

configure_file("${CMAKE_CURRENT_LIST_DIR}/espix_version.h.in"
               "${CMAKE_CURRENT_BINARY_DIR}/espix_version.h" @ONLY)

idf_component_register(
    SRCS
        "kernel.c"
        "klog.c"
    INCLUDE_DIRS
        "include"
        "${CMAKE_CURRENT_BINARY_DIR}"
    PRIV_INCLUDE_DIRS
        "priv_include"
    REQUIRES
        esp_common
    PRIV_REQUIRES
        esp_timer
        esp_hw_support
        esp_app_format
        log
        freertos
)
