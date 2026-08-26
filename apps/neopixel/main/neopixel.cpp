/*
 * espix example — an Arduino sketch, cross-compiled and run on espix.
 *
 * Cycles the onboard WS2812 through the hue circle and turns it off on the way
 * out. Adafruit_NeoPixel's genuine unmodified source and the Arduino core are
 * compiled into the app; espix itself knows nothing about Arduino.
 *
 * It is a normal setup()/loop() sketch, plus a teardown() espix calls when
 * someone stops the app — an Arduino sketch never needs one, because a board
 * runs until the power goes. Nothing else here is espix-specific: the shim in
 * espix_sketch.h supplies main(), and makes delay() the point at which a stop
 * takes effect.
 *
 * Three small departures from a stock Arduino sketch, all forced and all
 * explained where they appear:
 *
 *   - printf() rather than Serial (see setup)
 *   - delays are multiples of 10ms (see loop)
 */

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <stdio.h>
#include "espix_sketch.h"

/* The onboard WS2812 on an ESP32-S3-DevKitC-1. Change it for a board that
 * wires the LED elsewhere. */
#define LED_PIN         48

/* 20% of full scale. A WS2812 at 255 is painful to look at directly. */
#define MAX_BRIGHTNESS  51

/* 20ms per frame is 50fps. See the note in loop() about why it is a multiple
 * of ten. */
#define FRAME_MS        20

/* One trip round the hue circle, slow enough to read as a sweep. */
#define SWEEP_MS        6000

/*
 * At file scope, exactly as an Arduino sketch writes it.
 *
 * This needs ctors.ld and the startup shim to work: espix's ELF loader neither
 * runs global constructors nor tolerates them, so a sketch like this used to
 * take the process down during relocation. See the comment in ctors.ld.
 */
Adafruit_NeoPixel pixel(1, LED_PIN, NEO_GRB + NEO_KHZ800);

uint16_t hue = 0;

void setup()
{
    pixel.begin();
    pixel.setBrightness(MAX_BRIGHTNESS);
    pixel.clear();
    pixel.show();

    /*
     * printf() rather than Serial.println(). espix gives every app a stdout
     * belonging to whoever started it, so this appears in the shell that ran
     * the sketch — including over SSH, where Serial would write to the physical
     * UART and be seen by nobody.
     */
    printf("neopixel: sweeping GPIO %d at %d%% brightness; Ctrl-C to stop\n",
           LED_PIN, (MAX_BRIGHTNESS * 100) / 255);
}

void loop()
{
    /*
     * ColorHSV walks the hue circle at constant saturation and value, which is
     * what makes the sweep even, and gamma32 corrects for the eye's non-linear
     * response so the colours look equally bright. Both are the library's own;
     * hand-rolling either is what gets a NeoPixel demo looking lurid.
     */
    pixel.setPixelColor(0, pixel.gamma32(pixel.ColorHSV(hue)));
    pixel.show();

    hue += 65536 / (SWEEP_MS / FRAME_MS);

    /*
     * Keep delays to multiples of 10ms. espix runs a 100Hz FreeRTOS tick and
     * Arduino's delay() is vTaskDelay(ms / portTICK_PERIOD_MS), so delay(5)
     * truncates to zero and does not yield at all. The WS2812 waveform is
     * unaffected either way: it comes from the RMT peripheral, not from ticks.
     */
    delay(FRAME_MS);
}

/*
 * espix: called once when someone stops the app -- Ctrl-C, or `kill`. An
 * Arduino sketch has no equivalent, because a board simply runs until the power
 * goes; an espix app is a process, and what it switched on is its own to switch
 * off.
 *
 * Reached from inside delay() above, which the shim makes a cancellation point.
 */
void teardown()
{
    pixel.clear();
    pixel.show();

    /*
     * Hand the RMT channel back. Without this the channel and its GPIO
     * reservation outlive the app, which shows up as "GPIO 48 is not usable" on
     * the next run and eventually as no free channel at all. After the last
     * show(), which still needs it.
     */
    rmtDeinit(LED_PIN);

    printf("neopixel: stopped\n");
}
