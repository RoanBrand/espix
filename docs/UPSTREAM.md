# Defects in what espix builds on

Bugs and gaps in ESP-IDF and its managed components, each with the workaround
espix carries and enough detail to file a report. Kept separate from
[KNOWN-ISSUES.md](KNOWN-ISSUES.md) because the action is different: these are
someone else's to fix, and a future IDF release may remove the need for the
workaround — at which point it is useful to know exactly what the workaround was
for.

Verified against ESP-IDF v6.1-beta1 and xtensa-esp-elf GCC 15.2 unless noted.

## ESP-IDF

### `adjtime()` overflows, silently, and defeats smooth SNTP sync

`delta->tv_sec * 1000000L` is computed in a 32-bit `long`. A 56-year correction
— which is what any first sync from the epoch is — is 1.798e15 µs, and wraps to
about 2.14e9 µs. That is ~35 minutes: precisely the threshold that was supposed
to reject it as too large. So `adjtime()` accepts the wrapped value and returns
success, `sntp_sync_time()` never reaches its `settimeofday()` fallback, and the
clock slews a fictional 35-minute error forever while reporting that it synced.
The symptom is a sync callback that fires, a log line that says the clock was
set, and `date` still reading 1970.

**espix's workaround:** `SNTP_SYNC_MODE_IMMED`, stepping unconditionally. The
full analysis is in the comment at `components/espix_time/time.c`.

### Signal vocabulary with no machinery, including one function that lies

`<signal.h>` declares the whole POSIX surface and the toolchain defines almost
none of it:

- `signal()` is a phantom — `libc_a-signal.o` contains a single unused variable,
  because newlib's `signal.c` was compiled with `SIGNAL_PROVIDED` on the
  assumption that the platform supplies it. ESP-IDF does not.
- `sigaction`, `sigprocmask`, `sigsuspend`, `sigpending`, `pause`, `alarm`,
  `killpg`, `pthread_kill` and `nanosleep` are declared and defined **nowhere**.
  Referencing any of them compiles cleanly and fails at link.
- `kill()` resolves to `_kill_r`, a stub returning `ENOSYS`.
- `raise()` resolves to `_raise_r`, which calls **`abort()`** — so `raise()`
  panics the chip rather than failing.
- `pthread_sigmask()` **returns success while doing nothing**, and is
  force-linked with `-u` so it cannot be replaced. Its own comment says signals
  are not supported and it exists so external libraries link. A no-op that
  reports success is worse than an absent symbol, because nothing detects it.
- `pthread_cancel()` returns `ENOSYS` (this one is documented).

The pthread documentation does not mention signals at all — neither as
unsupported nor as a roadmap item.

**espix's workaround:** all of it is implemented from scratch in
`components/espix_proc/abi_signal.c` and published to apps under the real POSIX
names, which is possible precisely because the namespace is unclaimed.

### FreeRTOS-Plus-POSIX is vendored, but only the message queues

`components/rt` is FreeRTOS-Plus-POSIX V1.0.0, reduced to `mqueue` and `utils`.
Not a defect — worth recording only because its presence suggests a POSIX layer
that is not there. Upstream never implemented signals either, so it is not a
source to draw on.

## `espressif/esp_linenoise`

### Dumb mode corrupts input, two ways

The terminal probe runs as the console starts — before anyone has attached a
terminal, and always before `idf.py monitor --no-reset` reattaches to a board
already running. Nothing answers, the probe fails, and the instance latches dumb
mode until reboot. Dumb mode is not merely "no line editing":

- **The line is terminated one byte late.** `esp_linenoise_dumb()` writes
  `buffer[count + 1] = '\0'`, so `buffer[count]` keeps a stale byte from the
  previous command. `df` typed after `whoami` runs as `dfo`.
- **ESC is dropped but its sequence is kept.** ESC is `<= UNIT_SEP` and so
  treated as non-printable, while the rest of the escape sequence is retained —
  an arrow key is entered as the literal text `[A`.

Both were reported by users as "the console goes strange until reboot".

**espix's workaround:** `esp_linenoise_set_dumb_mode(false)` regardless of what
the probe decided. Assuming a capable terminal and being wrong puts escape codes
on screen; assuming a dumb one and being wrong costs the integrity of every
command typed.

### No way to redraw, and multi-line refresh walks private state

The library exposes no redraw entry point, and its multi-line refresh clears the
rows it used last time by walking upward from `max_rows_used` — private, sticky
within an instance, and reset only when `esp_linenoise_edit()` is entered.
Anything printed asynchronously therefore cannot be drawn around from outside:
repairing the line means guessing where the editor believes its prompt is, and
being wrong erases rows above it.

**espix's workaround:** end the input line and let the session loop start a new
one, which is the single operation that leaves the editor's idea of the screen
correct. See the console section of [ARCHITECTURE.md](ARCHITECTURE.md).

## `espressif/elf_loader`

Not a defect, but a constraint worth knowing: `elf_find_sym_default()` searches
the loader's own libc table **first**, and that table already answers for
`sleep` and `usleep`. A table added with `esp_elf_register_symbol()` is consulted
after it and cannot shadow them. The component provides
`elf_set_symbol_resolver()` for exactly this, documented for "symbol
interception and hooking", so no fork is needed — but a table alone will
silently fail to override.
