/*
 * A setup()/loop() sketch on espix.
 *
 * This shim belongs to this app, not to espix. It exists so a sketch can look
 * like an Arduino sketch: it supplies the main() espix actually calls, and
 * keeps two espix-specific details out of the sketch entirely — the extern "C"
 * that the entry point needs, and how a running app learns it has been asked to
 * stop.
 *
 * Copy these two files beside your own sketch to get the same shape.
 */
#pragma once

#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* What your sketch provides                                           */
/* ------------------------------------------------------------------ */

void setup();
void loop();

/* ------------------------------------------------------------------ */
/* What espix adds                                                     */
/* ------------------------------------------------------------------ */

/*
 * True once someone has asked this app to stop — Ctrl-C in the shell that
 * started it, or `kill` from anywhere else.
 *
 * An Arduino sketch runs until the board is powered off, so nothing in Arduino
 * corresponds to this. On espix an app is a process someone can stop, and one
 * driving hardware is the reason it matters: the LED has to be turned off by
 * the app, because espix cannot know what a given app left switched on.
 *
 * Check it in loop() and tidy up when it goes true. An app that ignores it is
 * killed outright a fraction of a second later, hardware and all.
 */
bool espixStopping();

/*
 * End the app. Does not return — so anything after it in loop() does not run,
 * and loop() is not called again.
 *
 * `status` is what the shell reports: 0 is success, and a non-zero value shows
 * up as `[exit N]` the way any other command's failure does.
 */
void espixExit(int status = 0) __attribute__((noreturn));
