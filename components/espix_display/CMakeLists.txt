idf_component_register(
    SRCS
        "display.c"
        "rfb.c"
        "vnc_des.c"
        "font8x8.c"
        "image.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "priv_include"
    PRIV_REQUIRES
        espix_kernel
        freertos
        lwip
        heap
        esp_timer
        esp_hw_support
        esp_system
        esp_rom
        # Unconditional, and free where it does not apply, the same way: both
        # guard their sources on a SOC_*_SUPPORTED capability, so on a target
        # without the block they compile nothing and this file's code for it is
        # not built either.
        esp_driver_jpeg
        # Unconditional, and free where it does not apply: esp_driver_ppa
        # guards its own sources on CONFIG_SOC_PPA_SUPPORTED, so on the S3 it
        # compiles nothing and this file's PPA code is not built either.
        esp_driver_ppa
)
