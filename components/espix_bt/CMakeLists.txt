# espix_bt publishes its A2DP source to the audio engine as a sink, so it
# requires espix_audio. The edge points this way on purpose: espix_audio must
# not depend on Bluetooth, which is what lets an S3 build carry audio with no
# Bluetooth stack.
#
# The requirement is unconditional, not keyed on CONFIG_ESPIX_AUDIO_SINK_BT_A2DP:
# espix_audio is always registered and costs nothing to name, and a conditional
# here left the include path off in a build whose sdkconfig already had the
# symbol -- the failure being a missing header, not a misconfiguration.
idf_component_register(
    SRCS
        "espix_bt.c"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        espix_kernel
        espix_audio
        bt
        nvs_flash
        esp_timer
        # The coexistence arbiter, for telling it to favour Bluetooth while a
        # sink is connected. Present because the radio is shared with Wi-Fi.
        esp_coex
        # And the station itself, so a connected sink can put Wi-Fi to sleep
        # rather than merely out-rank it. Guarded in the source by
        # CONFIG_ESP_WIFI_ENABLED, because a board without Wi-Fi still gets the
        # component named and links fine.
        esp_wifi
)
