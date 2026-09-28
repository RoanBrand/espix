idf_component_register(
    SRCS
        "display.c"
        "rfb.c"
        "vnc_des.c"
        "font8x8.c"
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
        # Unconditional, and free where it does not apply: esp_driver_ppa
        # guards its own sources on CONFIG_SOC_PPA_SUPPORTED, so on the S3 it
        # compiles nothing and this file's PPA code is not built either.
        esp_driver_ppa
)
