# htop on espix

[htop](https://htop.dev/) 3.5.3 over [ncurses](https://invisible-island.net/ncurses/) 6.4,
both unmodified upstream and built with `tools/espix-cc`: an ordinary POSIX app on
either chip, reading Linux's `/proc` and `/sys` formats from the kernel.

    tools/build-apps.sh htop                 # stages fsroot/bin/htop for the active target
    apps/htop/build.sh esp32s3 htop.staged   # or by hand: a target and where the stripped app goes
    htop

`fetch-htop.sh` and `fetch-ncurses.sh` pin the sources into `third_party/` (the
release tarball by SHA-256, ncurses at its v6.4 commit); `build-<target>/` holds
the build. The host needs a `tic` that reads ncurses 6.4's terminfo, which
macOS's does not: `build.sh` takes Homebrew's (`brew install ncurses`).

## What the build does

- **ncurses**: narrow characters, no terminfo database, with `xterm` and
  `xterm-256color` compiled in, the `TERM` a modern terminal sends and the
  shell's default. Two fixes to those compiled-in entries: `MKfallback.sh` types
  their numbers with GNU sed's `\<`, which BSD sed reads as nothing, and
  `xterm-256color`'s 65,536 colour pairs become 256, because a narrow ncurses
  keeps the numbers in 16 bits, where 65,536 is 0 and means no colour.
- **htop**: Linux's platform code as it is, with `CRT.c` taught `ESC [ A`-`D`
  for the arrows as well as `ESC O A`-`D`, since a terminal not in keypad mode
  sends those. `compat/linux/capability.h` gives htop's `capget()` call the
  structures it names (the call answers ENOSYS), and `sensors/` is the
  libsensors calls htop makes, over the kernel's thermal zone, for the
  temperature meter.
- **The S3**: an app there links as a shared object, which leaves a missing
  function for load time, so its `configure` cannot see one. It takes the S31
  `configure`'s answers about functions instead (the same libc and libespix);
  `build.sh` builds for the S31 first when they are not there. The S3 cannot grow
  a stack, so htop links with `-z stack-size=32768`: it reaches 22 KB, against
  the 16 KB an app gets by default (docs/USER-MODE.md).

Staged, htop is 394 KB on the S31 and 369 KB on the S3, sharing `/lib/libc.so`.

`tests/suites/66-htop.sh` drives it on a pty as a person would: the process
table, Setup still open three seconds after F2, a burst of keys landing whole,
and twenty launches and quits.
