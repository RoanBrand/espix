# The ESP-IDF component for the libsmb2 fetched into external/libsmb2.
#
# Upstream ships its own ESP-IDF glue and it does not build: lib/CMakeLists.txt
# calls register_component(), the pre-IDF-5 API that IDF 6.1 no longer has.
# Registering the same sources here, rather than patching that file, is what
# keeps external/libsmb2 byte-identical to its commit; see
# tools/fetch-libsmb2.py. The list below is SMB2_CORE_SOURCES copied from
# lib/CMakeLists.txt, less the entries ESP cannot build. Bumping the revision
# in the fetch script means re-reading that list.

set(espix_libsmb2 "${CMAKE_CURRENT_LIST_DIR}/../../external/libsmb2")

if(NOT EXISTS "${espix_libsmb2}/lib/libsmb2.c")
    message(FATAL_ERROR
        "external/libsmb2 is not populated. tools/fetch-libsmb2.py puts it "
        "there and the top-level CMakeLists.txt runs it before project(); "
        "run that script, or reconfigure from the top level.")
endif()

# aes.c with aes_reference.c rather than aes_apple.c, which is Apple's
# CommonCrypto and only appended to SMB2_CORE_SOURCES off ESP. smbdirect.c and
# quic.c are the RDMA and QUIC transports: neither is TCP and neither builds
# without its stack's headers. krb5-wrapper.c needs a Kerberos library, and
# HAVE_LIBKRB5 is not defined on this target.
set(espix_libsmb2_sources
    aes.c
    aes_reference.c
    aes128ccm.c
    alloc.c
    asn1-ber.c
    compat.c
    errors.c
    hmac.c
    hmac-md5.c
    init.c
    libsmb2.c
    md4c.c
    md5.c
    ntlmssp.c
    pdu.c
    sha1.c
    sha224-256.c
    sha384-512.c
    smb2-cmd-cancel.c
    smb2-cmd-close.c
    smb2-cmd-create.c
    smb2-cmd-echo.c
    smb2-cmd-error.c
    smb2-cmd-flush.c
    smb2-cmd-ioctl.c
    smb2-cmd-lock.c
    smb2-cmd-logoff.c
    smb2-cmd-negotiate.c
    smb2-cmd-notify-change.c
    smb2-cmd-oplock-break.c
    smb2-cmd-query-directory.c
    smb2-cmd-query-info.c
    smb2-cmd-read.c
    smb2-cmd-session-setup.c
    smb2-cmd-set-info.c
    smb2-cmd-tree-connect.c
    smb2-cmd-tree-disconnect.c
    smb2-cmd-write.c
    smb2-data-file-info.c
    smb2-data-filesystem-info.c
    smb2-data-reparse-point.c
    smb2-data-security-descriptor.c
    smb2-share-enum.c
    smb2-signing.c
    smb3-seal.c
    socket.c
    spnego-wrapper.c
    sync.c
    timestamps.c
    unicode.c
    usha.c
)

# Absolute, because the sources are outside this component. IDF resolves SRCS
# against COMPONENT_DIR and leaves an absolute path alone, which is what lets
# the fetched tree stay untouched.
set(espix_libsmb2_abs "")
foreach(src ${espix_libsmb2_sources})
    list(APPEND espix_libsmb2_abs "${espix_libsmb2}/lib/${src}")
endforeach()

idf_component_register(
    SRCS ${espix_libsmb2_abs}
    INCLUDE_DIRS
        "${espix_libsmb2}/include"
        "${espix_libsmb2}/include/smb2"
    PRIV_INCLUDE_DIRS
        "${espix_libsmb2}/lib"
        "${espix_libsmb2}/include/esp"
    REQUIRES lwip
    # espix_net exports the one C library symbol upstream wants that nothing
    # here provides: gethostname(), which smb2_serve_port() calls. See abi.c.
    PRIV_REQUIRES espix_net
)

# HAVE_CONFIG_H is what makes the sources include include/esp/config.h instead
# of expecting a configure step, and _U_ is the unused-argument marker upstream's
# Makefiles pass.
#
# The NEED_ macros pick compat.c's fallbacks rather than leaving the linker to
# find the function in a C library that does not have it. NEED_READV and
# NEED_WRITEV are here because they are needed, not optional: ESP-IDF's
# lwipopts.h sets LWIP_POSIX_SOCKETS_IO_NAMES 0, so lwip/sockets.h defines no
# readv/writev macros, this platform's C library implements neither, and
# socket.c calls both directly. Without these two the link fails in
# smb2_readv_from_socket() and smb2_write_to_socket(); with them compat.c
# supplies both. (gethostname(), the third such gap, is espix_net's exported
# symbol, which is what PRIV_REQUIRES espix_net above is for.)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    HAVE_CONFIG_H
    NEED_GETLOGIN_R
    NEED_RANDOM
    NEED_SRANDOM
    NEED_READV
    NEED_WRITEV
)

# _U_ cannot be a definition here; see espix_libsmb2.h for why. The path is
# joined to the flag so the command line carries no separate argument, the same
# way cmake/offt64.cmake force-includes cmake/offt64.h.
target_compile_options(${COMPONENT_LIB} PRIVATE
    "-include${CMAKE_CURRENT_LIST_DIR}/espix_libsmb2.h"
)
# Two upstream warnings, promoted to errors by the firmware's -Werror. Both are
# in lib/, both are real, and neither is ours to fix without forking upstream:
#
#   libsmb2.c:1405  a case that does not break before the next case label
#   libsmb2.c:4661  rep.output_buffer_length is unsigned, so '< 0' is always false
#   libsmb2.c:4730  the same comparison in smb2_query_info_request_cb
#   init.c:842      another case that falls through
#
# The type-limits pair are genuinely dead comparisons. The fallthroughs look
# deliberate in upstream's code but carry no fallthrough annotation. Scoped to
# this component, so -Werror still applies to espix's own sources.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=implicit-fallthrough
    -Wno-error=type-limits
)
