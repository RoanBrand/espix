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
        "spawn_fn.c"
        "fg.c"
        "pipe.c"
        "posix_uaccess.c"
        "posix.c"
        "app_fault.c"
        "app_fault_xtensa.S"
        "abi_exit.c"
        "abi_resolver.c"
        "libc_so.c"
        "abi_fs.c"
        "abi_libc.c"
        "abi_stdio.c"
        "abi_libm.c"
        "abi_posix.c"
        "abi_ident.c"
        "abi_signal.c"
        "abi_time.c"
        "abi_gfx.c"
        "abi_audio.c"
        "umode.c"
        "umode_futex.c"
        "umode_mm.c"
        "umode_sig.c"
        "umode_sys.c"
        "umode_net.c"
        "umode_gfx.c"
        "umode_entry.S"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "priv_include"
        # syslog.h and the rest, the same headers the apps on both sides see.
        "../../user/include/common"
    REQUIRES
        esp_common
        freertos
        espix_kernel
    PRIV_REQUIRES
        espix_fs
        esp_mm
        hal
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
        # umode_net.c: a U-mode app's sockets and name lookups are lwIP's.
        lwip
        # A session's terminal: proc.c lends it signals and the wait a signal ends.
        espix_tty
        heap
        esp_driver_rmt
        esp_driver_gpio
        soc
        # app_fault.c asks whether the cache is on before it touches PSRAM.
        spi_flash
)

# The kernel names an app's own libc may call (user/libc-os.<target>), for libc_so.c.
set(espix_libc_os "${CMAKE_CURRENT_LIST_DIR}/../../user/libc-os.${IDF_TARGET}")
set(espix_libc_os_inc "${CMAKE_CURRENT_BINARY_DIR}/libc_os_names.inc")
set(espix_libc_os_names "")
if(EXISTS "${espix_libc_os}")
    file(STRINGS "${espix_libc_os}" espix_libc_os_lines)
    foreach(line IN LISTS espix_libc_os_lines)
        string(REGEX REPLACE "#.*" "" line "${line}")
        string(REGEX MATCHALL "[^ \t]+" names "${line}")
        foreach(name IN LISTS names)
            string(APPEND espix_libc_os_names "\"${name}\",\n")
        endforeach()
    endforeach()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${espix_libc_os}")
endif()
file(CONFIGURE OUTPUT "${espix_libc_os_inc}" CONTENT "${espix_libc_os_names}\"\"\n")
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

# A fault in an M-mode app: the RISC-V vectors' panic entry, wrapped (app_fault.c).
if(CONFIG_IDF_TARGET_ARCH_RISCV)
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=xt_unhandled_exception")
endif()
