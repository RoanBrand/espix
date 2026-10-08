set(espix_audio_srcs "espix_audio.c")

# Only with the engine: espix_audio.c references this from inside its own
# CONFIG_ESPIX_AUDIO branch, and compiling it otherwise would link micro-mp3
# into an image with nothing to decode.
#
# A plain if(), not a generator expression in SRCS: idf_component_register()
# does not evaluate those the way add_library() would, and the component then
# registers without a library -- which fails the configure on the S31, where
# this branch is live, while the S3 (audio off) quietly produced nothing.
if(CONFIG_ESPIX_AUDIO)
    list(APPEND espix_audio_srcs "espix_mp3.cpp")
endif()

idf_component_register(
    SRCS ${espix_audio_srcs}
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        espix_kernel
        esp_audio_codec
        esp_asrc
        micro-mp3
)
