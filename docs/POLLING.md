# Polling and sleeps: every place espix waits

Companion to WORKLIST R-P7.1. The rule this is measured against:

**A consumer should block on the thing it is waiting for. A timeout is a
backstop for a notification that could be missed, never the mechanism. A fixed
sleep whose only job is to sample a condition again is the thing to remove.**

IDF code is often written the other way -- short sleeps beside a watchdog that
wants idle time -- so each case is judged on what it is actually waiting for.

## Convert: a wake source exists, or is cheap to add

| where | waits for | now | replacement |
|---|---|---|---|
| tty_console.c:791 (boot settle) | boot work to finish, then the log to drain | 50 ms poll of espix_kernel_boot_pending + klog echo time | the kernel signals when pending reaches zero, and the flusher signals when it has drained; wait on those |
| session.c:853 (job stop) | background job tasks to leave | 10 ms poll of the job table | each job task gives a counting semaphore as its last act; wait with the existing deadline |
| canvas_console.c:334 | the console task to exit | 10 ms poll of s_con.task | the task notifies the waiter before it deletes itself |
| rfb.c:2194 | the VNC task to exit | 10 ms poll of s_task | same |
| espix_audio.c:153 and :648 | the audio task to exit | 10 ms poll of s_task | same |
| espix_audio.c:187 (feed) | the sink to have room | 10 ms delay when write returns 0 | block until the sink signals space |
| ssh_transport.c:266 and :374 | a socket to become readable / writable | vTaskDelay(1) in the retry loop | block in poll()/select() on the fd with the existing stall timeout |
| term.c:577 and tty_console read | a key | xQueueReceive with a 100 ms timeout | block on the queue; a stop is a separate notification |
| abi_gfx.c:295 (app-facing) | an input event | non-blocking queue read, so apps spin | add a blocking espix_gfx_wait_event(g, ev, timeout_ms) beside the poll |

## Keep: deliberate, or already event-driven with a documented backstop

| where | why it stays |
|---|---|
| klog.c:325 | notify-driven; the 100 ms is the documented backstop for an ISR or a race |
| ota.c:1110 | notify-driven (GOT_IP); the timeout is the 24 h re-check |
| usb/host.c:2451 | usb_host_lib_handle_events blocks on the library queue; 50 ms is the backstop for the monitor client, which has its own queue |
| usb/host.c:2456 | error backoff for a library that is not in a state to be serviced |
| ssh_auth.c:215 | failed-password rate limit, not a poll |
| ssh_server.c:607 | accept error backoff, not a poll |
| tty_console.c:685 | editor error backoff, not a poll |
| history.c:270 | deliberate pacing |
| abi_signal.c sleep/usleep/nanosleep/pause | these *are* the delivery points; blocking is the point |
| cmd_text.c:729 (sleep), cmd_sys.c:371 (ps -d), desktop.c:2164 (appdata panel) | the sleep is the requested behaviour |
| espix_audio.c:563 | the 5 s idle yield kept by R-P4.2 for the SMP tick |
| wifi.c:318 | uses a timer rather than blocking the event loop -- the right shape already |
| cmd_run.c:168 (foreground wait), cmd_sys.c:1049 (top frame) | **A task cannot wait on two sources at once.** The child half is already event-driven -- espix_proc_wait blocks on espix_proc's event group -- so the only thing polled is the key, and making that a push needs each transport to have an async producer. Only the VNC terminal has one (its key queue). Over SSH, chan_poll_interrupt *is* the connection task reading the wire, so a signal would need a second reader task per connection: a task and a wakeup per packet, to save the 20 Hz this costs while a command runs. The serial console is ready-but-unread rather than an event. Left as a bounded wait, documented, until a transport can push. |
| reaper.c:80 | portMAX_DELAY -- the model to copy |

## Shared primitives worth building once

1. **Task-exit notification.** Four sites hand-roll "wait for the task to clear
   its own handle". One helper -- the exiting task notifies, the waiter blocks
   on a binary semaphore or a task notification -- replaces all four.
2. **Session key bit.** A per-session event group with FINISH and KEY turns the
   three UI waits (cmd_run, top, and later Ctrl-Z) into blocking waits.
3. **The audio ring.** The sink should signal space rather than the producer
   sleeping and retrying.
4. **A blocking gfx event wait**, published for apps.

## esp_event as a bus

IDF's event loop is *already* created in every espix build (espix_time, espix_net
and espix_ota each call esp_event_loop_create_default(); the first wins), so an
espix event base costs a queue and heap, not another task.

It is the right tool for a **state broadcast with several independent
consumers** -- and the wrong one for a single waiter, which wants a semaphore or
a task notification (a default-loop handler must not block, so it cannot be the
thing that waits).

Today the multi-consumer cases are all IDF's already (WiFi/IP events, which
espix_net consumes). The candidates here are one-consumer each: boot-settled
(the console), USB hotplug (the monitor), net-up (SSH, SNTP, OTA -- already
event-sourced). So: build the direct primitives now, and reach for esp_event
when a third subsystem needs to observe the same state (power, services, a
reclaimer), rather than to move these.

## Order of work

1. ~~Task-exit notification helper, and the four callers.~~ Done (5be604c is
   not this one; see the commit that introduced espix_task.h):
   espix_task_exit_t in espix_kernel, and canvas_console, rfb and audio's two
   waits block on it. Verified by three display start/stop cycles on the
   board: no crash, and no "console task did not stop" warning, which is what
   a missed wakeup prints after the 3 s timeout.
2. ~~Session key bit: cmd_run and top.~~ Reclassified: see the last row of
   the keep table. Doing it properly for SSH costs more than it saves, and
   the VNC-only win is not worth the plumbing on its own.
3. Boot-settle signal (console start only, but it is a 5 s poll today).
4. Job-stop semaphore.
5. ssh_transport poll() waits.
6. Audio ring and the gfx blocking wait (larger; the gfx one is an ABI addition).
