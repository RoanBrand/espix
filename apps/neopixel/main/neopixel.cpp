/*
 * espix example app — an Arduino sketch, cross-compiled and run on espix.
 *
 * Cycles the onboard WS2812 through the hue circle and turns it off on the way
 * out. The point is not the animation: it is that Adafruit_NeoPixel's genuine,
 * unmodified source and the Arduino core compile into an espix app, with
 * Arduino as a dependency of this app rather than of the firmware.
 *
 * Two things about this file are espix-specific and are the interesting part.
 *
 * 1. extern "C" on main(). project_elf() links with `-e app_main` and compiles
 *    with `-Dmain=app_main`; without extern "C" the entry point is mangled and
 *    the linker reports "cannot find entry symbol app_main", producing an ELF
 *    with no sections.
 *
 * 2. Every delay() is a multiple of 10ms. espix runs a 100Hz FreeRTOS tick, and
 *    Arduino's delay() is vTaskDelay(ms / portTICK_PERIOD_MS) -- so delay(5)
 *    truncates to vTaskDelay(0), which does not yield at all. Arduino ships a
 *    1000Hz tick and its build checks for one, which is what
 *    ARDUINO_SKIP_TICK_CHECK=1 bypasses. The WS2812 waveform is unaffected
 *    either way: it comes from the RMT peripheral, not from ticks.
 *
 * There is deliberately no global Adafruit_NeoPixel object, unlike the usual
 * Arduino idiom. espix's ELF loader does not run .ctors, so a global would be
 * left unconstructed -- see the note in apps/README.md.
 */

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>

#include <stdio.h>
#include <stdlib.h>

/* espix's cooperative stop. Ctrl-C sets this rather than deleting the task
 * outright, which is what lets the LED be turned off on the way out. */
extern "C" bool espix_app_stopping(void);

/* The onboard WS2812 on an ESP32-S3-DevKitC-1. Overridable, because plenty of
 * boards wire it elsewhere. */
#define DEFAULT_LED_PIN 48

/* 25% of full scale. WS2812s are painfully bright at 255, and this is a value
 * you can look at directly. */
#define MAX_BRIGHTNESS 64

/* 20ms per frame is 50fps, and a multiple of the 10ms tick. */
#define FRAME_MS 20

/* One trip round the hue circle, in frames. 6s is slow enough to read as a
 * sweep rather than a flicker. */
#define SWEEP_FRAMES (6000 / FRAME_MS)

extern "C" int main(int argc, char **argv)
{
    int pin = DEFAULT_LED_PIN;

    if (argc > 1) {
        char      *end = NULL;
        const long n   = strtol(argv[1], &end, 10);

        if (end == argv[1] || *end != '\0' || n < 0 || n > 48) {
            printf("usage: %s [gpio]   (default %d)\n", argv[0], DEFAULT_LED_PIN);
            return 1;
        }
        pin = (int)n;
    }

    Adafruit_NeoPixel pixel(1, pin, NEO_GRB + NEO_KHZ800);

    pixel.begin();
    pixel.clear();
    pixel.show();

    printf("neopixel: sweeping GPIO %d at %d%% brightness; Ctrl-C to stop\n",
           pin, (MAX_BRIGHTNESS * 100) / 255);

    uint16_t hue = 0;

    while (!espix_app_stopping()) {
        /*
         * ColorHSV walks the hue circle at constant saturation and value, which
         * is what makes the sweep even; gamma32 then corrects for the eye's
         * non-linear response, so the colours look equally bright rather than
         * the green stretch dominating. Both are the library's own -- doing
         * this by hand is what a hand-rolled version gets wrong.
         */
        pixel.setPixelColor(0, pixel.gamma32(pixel.ColorHSV(hue, 255, MAX_BRIGHTNESS)));
        pixel.show();

        hue += 65536 / SWEEP_FRAMES;
        delay(FRAME_MS);
    }

    /* Asked to stop: leave the LED dark rather than stuck on whatever colour
     * the sweep happened to reach. */
    pixel.clear();
    pixel.show();

    printf("neopixel: stopped\n");
    return 0;
}
