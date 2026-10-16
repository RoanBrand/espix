idf_component_register(
    SRCS
        "auth.c"
        # The app-facing names for the accounts above: getpwnam and friends.
        "abi_user.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_common
    PRIV_REQUIRES
        espix_kernel
        espix_fs
        esp_timer
        mbedtls
        # abi_user.c publishes the lookups to loaded apps.
        espressif__elf_loader
)
