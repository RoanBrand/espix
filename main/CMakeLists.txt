set(espix_main_requires
    espix_kernel
    espix_audio
    espix_bt
    espix_fault
    espix_fs
    espix_proc
    espix_shell
    espix_cmds
    espix_display
    espix_net
    espix_usb
    espix_auth
    espix_ssh
    espix_time
    espix_ota
    espix_svc
)

# espix_main.c overrides esp_trace_get_user_params() to name the esp_sysview
# encoder; that header only exists when the build has an external trace library,
# so require the component only then.
if(CONFIG_ESP_TRACE_LIB_EXTERNAL)
    list(APPEND espix_main_requires esp_trace)
endif()

idf_component_register(
    SRCS
        "espix_main.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        ${espix_main_requires}
)
