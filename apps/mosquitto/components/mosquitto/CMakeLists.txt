# The broker, compiled from the upstream sources fetch-mosquitto.sh checks out.
#
# Nothing upstream is patched. The file list is upstream's src/CMakeLists.txt
# translated; the feature set is stated here because this build is the app's,
# not upstream's CMake's.
#
# What is deliberately off, and why:
#   * TLS and websockets -- an app links no OpenSSL, and the LAN does not need
#     it; a viewer tunnels instead.
#   * unix sockets and epoll -- lwIP is a TCP/IP stack, not a Unix kernel.
#   * the http_api listener -- it is libmicrohttpd.
#   * plugins loaded from disk -- espix apps are not shared objects; the
#     built-in password-file and acl-file checks are compiled in instead.
#   * systemd, DLT, libwrap, kqueue, jemalloc -- none of them exist here.

include("${CMAKE_CURRENT_LIST_DIR}/../../sources.cmake")

idf_component_register(SRCS ${MOSQ_SRC}
                       INCLUDE_DIRS "${MOSQ}/include"
                       PRIV_INCLUDE_DIRS ${MOSQ_INC}
                       REQUIRES mosquitto_espix cjson
                       PRIV_REQUIRES lwip esp_timer)

target_compile_definitions(${COMPONENT_LIB} PRIVATE ${MOSQ_DEFS})
target_compile_options(${COMPONENT_LIB} PRIVATE ${MOSQ_OPTS})
