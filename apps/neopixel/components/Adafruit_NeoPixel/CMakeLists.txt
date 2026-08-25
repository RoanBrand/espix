# Adafruit_NeoPixel 1.12.0, vendored unmodified as an ESP-IDF component.
#
# Arduino libraries are not IDF components, so there is nothing on the registry
# to depend on: the library's own sources are compiled here against the Arduino
# core, which is what makes this a genuine "Arduino library cross-compiled for
# espix" rather than a reimplementation. LGPL-3.0; see COPYING.
#
# esp.c holds the ESP32 RMT backend that generates the WS2812 waveform in
# hardware, which is why the 100Hz FreeRTOS tick does not affect the signal.
idf_component_register(
    SRCS
        "Adafruit_NeoPixel.cpp"
        "esp.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        espressif__arduino-esp32
)
