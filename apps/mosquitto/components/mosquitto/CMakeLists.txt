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

# CMAKE_CURRENT_LIST_DIR, not _SOURCE_DIR: IDF includes this file once in a
# requirements pass that runs before project(), where the current source
# directory is the build tree.
set(MOSQ "${CMAKE_CURRENT_LIST_DIR}/../../third_party/mosquitto")

if(NOT EXISTS "${MOSQ}/src/mosquitto.c")
    message(FATAL_ERROR "mosquitto sources are missing from ${MOSQ}; run fetch-mosquitto.sh")
endif()

# Every broker source, less the two a device cannot have.
# Plain GLOB, not CONFIGURE_DEPENDS: this file is also evaluated in IDF's
# requirements pass, which is script mode, where CONFIGURE_DEPENDS is invalid.
# The checkout is pinned, so the file list cannot change under a build.
file(GLOB MOSQ_SRC "${MOSQ}/src/*.c")
list(REMOVE_ITEM MOSQ_SRC
    "${MOSQ}/src/http_api.c"
    "${MOSQ}/src/service.c")

# The protocol half, exactly the files upstream's broker target lists. The rest
# of lib/ is the client library and is not part of a broker.
foreach(f
    alias_mosq handle_ping handle_pubackcomp handle_pubrec handle_pubrel
    handle_suback handle_unsuback net_mosq_ocsp net_mosq net_ws
    packet_datatypes packet_mosq property_mosq send_mosq send_connect
    send_disconnect send_publish send_subscribe send_unsubscribe tls_mosq
    util_mosq will_mosq)
    list(APPEND MOSQ_SRC "${MOSQ}/lib/${f}.c")
endforeach()

foreach(f
    base64_common cjson_common file_common memory_common mqtt_common
    password_common property_common random_common strings_common time_common
    topic_common utf8_common)
    list(APPEND MOSQ_SRC "${MOSQ}/libcommon/${f}.c")
endforeach()

# json_help.c and the two security checks the broker embeds rather than loads.
list(APPEND MOSQ_SRC
    "${MOSQ}/common/json_help.c"
    "${MOSQ}/plugins/acl-file/acl_check.c"
    "${MOSQ}/plugins/acl-file/acl_parse.c"
    "${MOSQ}/plugins/password-file/password_check.c"
    "${MOSQ}/plugins/password-file/password_parse.c")

idf_component_register(SRCS ${MOSQ_SRC}
                       INCLUDE_DIRS "${MOSQ}/include"
                       PRIV_INCLUDE_DIRS
                           "${MOSQ}"
                           "${MOSQ}/src" "${MOSQ}/lib" "${MOSQ}/libcommon"
                           "${MOSQ}/common" "${MOSQ}/deps"
                           "${MOSQ}/plugins/acl-file"
                           "${MOSQ}/plugins/password-file"
                       REQUIRES mosquitto_espix cjson
                       PRIV_REQUIRES lwip esp_timer)

# Two names lwIP and newlib do not have, declared once for every source.
target_compile_options(${COMPONENT_LIB} PRIVATE -include espix_compat.h)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    VERSION="2.1.2"
    WITH_BROKER
    WITH_BRIDGE
    WITH_CONTROL
    WITH_PERSISTENCE
    WITH_SYS_TREE
    HAVE_GETRANDOM)

# Upstream's warnings are not this app's to fix, and its code is not modified.
# -Wno-error is also what lets a source say -D_GNU_SOURCE when the build
# already did: loop.c and mux_poll.c do exactly that.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error
    -Wno-format -Wno-unused-parameter -Wno-sign-compare -Wno-format-truncation)
