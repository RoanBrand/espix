idf_component_register(
    SRCS
        "fault.c"
        "reaper.c"
        "coredump.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "priv_include"
    REQUIRES
        esp_common
        freertos
        espix_kernel
    PRIV_REQUIRES
        espix_proc
        esp_system
        esp_timer
        espcoredump
        esp_app_format
    # __wrap_esp_panic_handler is referenced only by the linker, so without
    # WHOLE_ARCHIVE the object holding it gets dropped from the static library
    # and the wrap silently does nothing.
    WHOLE_ARCHIVE
)

# Route every panic through __wrap_esp_panic_handler. Same mechanism ESP-IDF
# uses in components/esp_system/test_apps/esp_system_unity_tests.
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")
