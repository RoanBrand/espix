/*
 * A setup()/loop() sketch on espix.
 *
 * This shim belongs to the app, not to espix. It supplies the main() espix
 * actually calls, and keeps the espix-specific details out of the sketch: the
 * extern "C" the entry point needs, running global constructors that the ELF
 * loader does not, and stopping cleanly when someone asks.
 *
 * Copy these two files and ctors.ld beside your own sketch to get the same
 * shape.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* What your sketch provides                                           */
/* ------------------------------------------------------------------ */

void setup();
void loop();

/*
 * Put your hardware back: turn the LED off, stop the motor, release the
 * peripheral. Called once when the app is stopping, before it exits.
 *
 * Optional — there is an empty default, so a sketch that owns no hardware can
 * leave it out. An Arduino sketch has no equivalent because a board runs until
 * the power goes; an espix app is a process someone can stop, and espix cannot
 * know what a given app switched on.
 *
 * Named teardown rather than shutdown on purpose: lwip's sockets.h defines
 * shutdown as a two-argument macro, so a sketch that later added networking
 * would stop compiling for a baffling reason.
 */
void teardown();

/* ------------------------------------------------------------------ */
/* What espix adds                                                     */
/* ------------------------------------------------------------------ */

/*
 * delay() is a cancellation point.
 *
 * The shim wraps Arduino's delay() so that a stop request — Ctrl-C in the shell
 * that started the app, or `kill` from anywhere else — ends it there and then:
 * teardown() runs and the app exits, and that delay() never returns. A sketch
 * that delays anywhere in loop() is therefore interruptible with nothing
 * written for it.
 *
 * A loop() that never delays is still stopped, checked between iterations.
 */

/* True once a stop has been requested. Only needed by a loop() that does long
 * work without delaying and wants to bail out mid-way; otherwise ignore it. */
bool espixStopping();

/*
 * End the app. Runs teardown() and does not return, so anything after it in
 * loop() does not run. `status` is what the shell reports — non-zero shows up
 * as `[exit N]`.
 */
void espixExit(int status = 0) __attribute__((noreturn));
