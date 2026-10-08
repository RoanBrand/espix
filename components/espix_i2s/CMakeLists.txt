# Required unconditionally by main so the header is always reachable. The body of
# espix_i2s.c is empty unless CONFIG_ESPIX_I2S, so with the codec off nothing
# references esp_codec_dev and the linker drops it -- the dependency costs build
# time, not flash. (A CONFIG_ conditional here is unreliable across IDF's
# multi-pass configure.)
idf_component_register(
    SRCS "espix_i2s.c"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        espix_audio
        esp_codec_dev
        esp_driver_i2c
        esp_driver_i2s
        esp_driver_gpio
)
