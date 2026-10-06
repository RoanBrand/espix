idf_component_register(
    SRCS
        "espix_audio.c"
        # Only with the engine: espix_audio.c references this from inside its
        # own CONFIG_ESPIX_AUDIO branch, and compiling it otherwise would link
        # micro-mp3 into an image with nothing to decode.
        $<$<BOOL:${CONFIG_ESPIX_AUDIO}>:"espix_mp3.cpp">
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        espix_kernel
        espix_bt
        esp_audio_codec
        micro-mp3
)
