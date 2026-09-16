# The role is a compile-time choice, and the point of it is what gets left out.
#
# Same shape as espix_net/CMakeLists.txt, and for the same reason: IDF evaluates
# a component's CMakeLists twice, once to resolve REQUIRES/PRIV_REQUIRES where
# CONFIG_ values do not exist yet and once where they do. A conditional
# requirement therefore disappears during the first pass and the build fails on
# a missing usb_host.h while happily compiling host.c.
#
# Unconditional requirements cost nothing here either. With the role set to
# device, host.c is not built, nothing calls usb_host_install(), and only usb.c
# is linked -- which is the whole of the saving, since the component manager has
# already downloaded both stacks by the time Kconfig exists.
set(espix_usb_srcs "usb.c")
if(CONFIG_ESPIX_USB_ROLE_HOST)
    list(APPEND espix_usb_srcs "host.c")
endif()

idf_component_register(
    SRCS
        ${espix_usb_srcs}
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "priv_include"
    REQUIRES
        esp_common
    PRIV_REQUIRES
        espix_kernel
        freertos
        heap
        espressif__usb
        espressif__usb_host_msc
        espressif__esp_ext_part_tables
)
