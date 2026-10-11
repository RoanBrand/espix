idf_component_register(
    SRCS
        "proc.c"
        "exec.c"
        "abi_cxx.cpp"
        "abi_drivers.c"
        "abi_env.c"
        "abi_alloc.c"
        "abi_pthread.c"
        "proc_threads.c"
        "abi_exit.c"
        "abi_resolver.c"
        "abi_fs.c"
        "abi_libc.c"
        "abi_libm.c"
        "abi_ident.c"
        "abi_signal.c"
        "abi_time.c"
        "abi_gfx.c"
        "abi_audio.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "priv_include"
    REQUIRES
        esp_common
        freertos
        espix_kernel
    PRIV_REQUIRES
        espix_fs
        espix_shell
        # abi_gfx.c publishes the screen and its input queue to apps.
        espix_display
        # abi_audio.c publishes the engine's stream API to apps. The header it
        # needs with it is espix_audio.h, which is app-facing by design.
        espix_audio
        espressif__elf_loader
        # abi_pthread.c wraps pthread_create, to give an app's threads the
        # process's slot.
        pthread
        esp_timer
        # abi_drivers.c wraps select() around esp_vfs_select() and names
        # eventfd().
        vfs
        heap
        esp_driver_rmt
        esp_driver_gpio
        soc
)
