# USB-NCM is a compile-time choice, and the point of it is what gets left out.
#
# The source is conditional; the requirement is not, and that asymmetry is
# forced rather than sloppy. IDF evaluates a component's CMakeLists twice: an
# early expansion that resolves REQUIRES/PRIV_REQUIRES, where CONFIG_ values do
# not exist yet, and a full pass where they do. Making the requirement
# conditional therefore drops it during early expansion and the build fails on
# a missing tinyusb.h while happily compiling usb_ncm.c.
#
# Leaving the requirement unconditional costs nothing: with the option off,
# usb_ncm.c is not built, nothing calls tinyusb_driver_install(), and
# --gc-sections drops the stack from the image.
set(espix_net_srcs "net.c" "wifi.c" "abi.c")
if(CONFIG_ESPIX_USB_NCM_ENABLED)
    list(APPEND espix_net_srcs "usb_ncm.c")
endif()
if(CONFIG_ESPIX_ETH_ENABLED)
    list(APPEND espix_net_srcs "eth.c")
endif()

idf_component_register(
    SRCS
        ${espix_net_srcs}
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "priv_include"
    REQUIRES
        esp_common
    PRIV_REQUIRES
        espix_kernel
        espix_fs
        esp_netif
        esp_wifi
        esp_eth
        esp_event
        esp_timer
        esp_hw_support
        nvs_flash
        lwip
        freertos
        espressif__elf_loader
        espressif__esp_tinyusb
)
