# The ext driver is a compile-time choice, and the point of it is what gets left
# out.
#
# Same shape as espix_net/CMakeLists.txt and espix_usb/CMakeLists.txt, and for
# the same reason: IDF evaluates a component's CMakeLists twice, once to resolve
# REQUIRES/PRIV_REQUIRES where CONFIG_ values do not exist yet and once where they
# do. A conditional requirement therefore disappears during the first pass and the
# build fails on a missing ext4.h while happily compiling ext.c -- which is
# exactly what happened here the first time.
#
# The unconditional requirement costs nothing: with the option off, ext.c is not
# built, nothing calls lwext4, and only what espix_fs references is linked.
set(espix_fs_srcs
    "fs.c"
    "mode.c"
    "vfs.c"
    "access.c"
    "dev.c"
    "fat.c"
    "part.c"
)
if(CONFIG_ESPIX_FS_EXT4)
    list(APPEND espix_fs_srcs "ext.c")
endif()

idf_component_register(
    SRCS
        ${espix_fs_srcs}
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "priv_include"
    REQUIRES
        esp_common
        esp_blockdev
    PRIV_REQUIRES
        esp_partition
        fatfs
        esp_lwext4
        espix_kernel
        espix_proc
        espix_shell
        joltwallet__littlefs
        vfs
)
