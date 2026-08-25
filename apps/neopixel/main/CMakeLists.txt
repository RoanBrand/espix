idf_component_register(
    SRCS
        "neopixel.cpp"
        "espix_sketch.cpp"
    REQUIRES
        espressif__arduino-esp32
        Adafruit_NeoPixel
)
