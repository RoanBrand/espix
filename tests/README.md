# espix tests

```bash
make test                            # everything, four suites at a time
make test J=8                        # more workers
make test SERIAL=1                   # one at a time, printing as it goes
make test SEED=48213                 # replay a particular random order
make test SUITE=fs                   # suites whose name contains "fs"
make test PORT=/dev/ttyUSB0          # include the serial-console suites
./tests/run.sh --host 10.0.0.5       # a device somewhere else
./tests/run.sh --overlap-transfers   # let transfers run concurrently
```

`make test` finds the ESP-IDF and the serial port for itself, builds the test
app if it is stale, and copies it over. Nothing needs to be sourced first.

## What is here

| | |
|---|---|
| `run.sh` | finds the suites, schedules them, watches the device, reports |
| `lib/pool.sh` | the worker pool, the random order, and the live grid |
| `lib/portable.sh` | the macOS/Linux differences, and the bash 3.2 floor |
| `lib/assert.sh` | `assert_eq`, `assert_contains`, `assert_status`, and the counters |
| `lib/device.sh` | connections, transfers, the test app, the health check |
| `lib/session.py` | one long-lived interactive SSH session |
| `lib/console.py` | the serial console |
| `suites/*.sh` | the tests |
| `app/` | the test app: levers for the suites to pull |

## Adding a suite

Drop a `NN-name.sh` in `suites/`. It is *sourced*, so everything in `lib/` is
already available and there is no boilerplate:

```sh
# What this covers, and why.
#
# RESOURCES: none -- what it needs the device to itself for, if anything.

assert_eq "pwd is the home" "/home/$ESPIX_USER" "$(dev_run 'pwd')"
assert_contains "id reports a uid" "uid=" "$(dev_run 'id')"
assert_status "a bad command exits 127" 127 dev_status 'nosuchthing'
```

Number them so a listing reads in a sensible order — they no longer *run* in
that order, see below. Even `--serial` runs the `exclusive` ones last rather
than in numeric order, so that the measurements mean the same thing in both
modes.

`RESOURCES:` is read by the runner and decides how the suite is scheduled:

| | |
|---|---|
| `none` | runs beside anything. The default when the line is missing. |
| `console` | needs the one serial port |
| `exclusive` | needs the whole device: measurements, and anything that saturates it |

A `none` suite must not depend on the device being otherwise idle, and anything
it creates on the device must carry `$ESPIX_WORKER` in its name — two workers
share one filesystem, and a fixed name is a collision waiting for a fast enough
machine. Note the shape of that carefully: `10-fs.sh` asserts against a listing
of `/tmp`, where a *neighbour's* `espix-test-fs-2` is also sitting, so it matches
the full name and not the stem.

### Which helper to reach for

- **`dev_run`** for nearly everything. It uses the session the runner already
  logged in with, so it costs no connection.
- **`dev_status`** when the *exit code* is the assertion. espix's shell has no
  `$?`, so this opens its own connection to get one — a few seconds each; do not
  use it where output would do.
- **`dev_console_run`** for the serial console. That session is **root**, which
  is its point: it exercises privileged paths without unlocking the root
  account for the network.
- **`dev_push` / `dev_pull` / `dev_sftp`** for transfers.
- **`dev_capture`** when the output is large. It writes on the device and fetches
  the file, instead of reading a long answer back over the channel — output big
  enough to matter is exactly what stresses the transport, so reading it inline
  makes the transport part of the measurement.

## Things that are the way they are for a reason

Every one of these cost real time before it was written down. They are enforced
in `lib/`, not left to memory.

- **bash 3.2.** macOS ships it and always will — Apple froze bash at the last
  GPLv2 release. No associative arrays, no `mapfile`, no `${var^^}`. Installing
  bash 5 does *not* help: `#!/bin/bash` still finds Apple's, and
  `#!/usr/bin/env bash` only finds a newer one when Homebrew is ahead of `/bin`
  in `PATH` — which differs between your shell, a make recipe and CI. Please do
  not "fix" this with an associative array that works on your Linux box.
- **No `timeout`.** macOS has neither `timeout` nor `gtimeout`. Use
  `espix_timeout`.
- **`grep -a` on anything from the serial port.** A capture contains NUL bytes,
  and without `-a` grep calls the file binary and prints nothing — which reads
  exactly like the thing you searched for being absent.
- **`sftp -b` turns on batch mode, and batch mode disables password auth.**
  `dev_sftp` passes `-o BatchMode=no`. Without it the failure looks like the
  device refusing your login.
- **`SSH_ASKPASS_REQUIRE=force` answers the prompt *instead of* you.** Right for
  one-shot commands, wrong for an interactive session — `session.py` unsets all
  three, because a root-login test once got the `esp` password this way and the
  result looked like a server-side refusal.
- **Never `-o LogLevel=ERROR`.** It hides the one line that explains a dropped
  connection, leaving `rc=255` and empty stderr.
- **One command per call — espix's shell has no `;`.** Both drivers refuse a
  command containing one rather than running the wrong thing quietly.
- **Do not filter output before reading it.** A `grep` for "denied" once matched
  sftp's own "Fetching" line and turned a working refusal into a phantom
  regression.
- **No SSH multiplexing.** espix's server has exactly one channel per connection
  (`"our channel id; only ever one"`), so `ControlMaster` cannot help. The
  long-lived session in `session.py` is the speed answer, and running several of
  them at once is the parallel answer — several *connections*, not several
  channels on one.
- **Never a bare `wait`, and never background a shell function.** `wait` with no
  argument waits for `session.py` too, which never exits; that hung a run for
  half an hour. And `func &` gives back a *subshell* pid, so signalling it
  leaves the `ssh` underneath alive holding a session slot until the device
  reboots. Background the binary, and `wait` a named pid.

## Speed, and parallelism

A login costs a key exchange plus PBKDF2 at 20 000 iterations, so the runner
logs in **once per suite** and `dev_run` reuses it. `dev_status` is the
exception and is deliberately rationed.

Four suites run at once by default. That is a speed change and a coverage
change, and the second is the one that matters: unrelated suites running side by
side put the device under a shape of load nothing else here produces — several
sessions, several shells, transfers beside them. **A failure that only appears
under the pool is a finding, not flakiness**, and the runner is built to tell
the difference rather than to avoid the question.

The device allows eight concurrent SSH sessions
(`CONFIG_ESPIX_SSH_MAX_SESSIONS`), which is where four workers came from: one
long-lived session each, and room for the one-shot connections a suite opens for
an exit status or a transfer, with slack left over for a person watching `top`
from another window. `55-sessions.sh` measures that limit on the device rather
than trusting the config.

### The order is random, and the seed comes back

Suites are shuffled and handed to workers as they free up, so successive runs
try different combinations rather than the same one for ever. Dynamic rather
than a static split: suite durations here differ by an order of magnitude, and a
static split leaves three workers idle while the fourth finishes the long one.

The seed is printed at the top of every run and taken back by `--seed`, and that
is not a nicety. A random order that cannot be replayed makes a concurrency
finding worthless — it is the difference between "this failed once" and "this
fails".

Not `$RANDOM`: bash seeds it per implementation, so one seed gives a different
order under bash 3.2 and bash 5, and a seed that silently fails to replay is
worse than no seed at all. `awk`'s `srand()` has the same problem. The shuffle
goes through python, whose Mersenne Twister is stable across versions.

### Three phases, and why the measurements are last

1. **The pool.** Everything tagged `none` or `console`, shuffled, N at a time.
2. **A settle**, then **serial re-runs of anything that failed**.
3. **The measurements, alone**, on a quiet device: `25-cpu`, `45-throughput`,
   `55-sessions`, `90-stress`.

Throughput floors and CPU-share invariants measure the machine, so measuring
them beside three other suites measures the pool instead. Running them last and
alone is what makes the numbers in a parallel run comparable with the ones from
`--serial`.

### What a red suite means, and how the runner says which

Any suite that fails in the pool is re-run **alone**, and the report says which
of two things it was:

```
failures found by the pool, re-run alone:
  40-transfer    fails alone too (2) -- a bug, not a collision
  15-streams     passes alone -- only fails under load; seed 48213 reproduces it
```

The two want completely different next steps, and guessing which one you have is
how a real concurrency bug gets written off as a flaky test.

### Transfers do not overlap

`device.sh` holds a lock across every `scp` and `sftp`. Concurrent SFTP
transfers break, and did so long before there was a parallel runner (see
[KNOWN-ISSUES](../docs/KNOWN-ISSUES.md); proven pre-existing by a control with an
ordinary file). A default run that goes red on a known bug it is not testing
teaches everyone to ignore the colour. `--overlap-transfers` removes the lock
for anyone who wants that bug rather than the run.

Today exactly one pool suite transfers, so the lock is a guard rather than a
bottleneck — it is there so the next transfer-using suite is safe by default.

## The health check

A background monitor with **one persistent session** asks the device how it is
every ten seconds: uptime going backwards, and a new core dump. A reboot or a
dump stops the run, and the alert names the suites that were running at the
time — which under a pool is better attribution than "the suite that just
finished", not worse.

This used to run after every suite, over four *one-shot* logins each time. At
roughly four seconds a login and twelve suites that was around three minutes of
a ten-minute run spent asking the device how it feels. It also cannot work under
a pool at all: its leak test counts `sshd:conn` tasks, and with N workers holding
a session each, the count it reads is the runner itself. So the leak test moved
to the end of the run, where the number means something again.

It exists because the most expensive bug in this project's history presented as a
WiFi-task assert and an ipc0 scheduler fault, and was really a filesystem
recursion — found only once somebody thought to ask the device how it was.
Nobody remembers to ask, so the runner does.

Note the uptime comparison in particular: checking the *reset reason* alone
misses a reboot, because two software reboots in a row read identically. That
was caught by rebooting a device mid-check and watching the first version not
notice.

And note what the core-dump check matches on. It used to look for the *absence*
of "no core dump stored", so a query that simply went unanswered — empty output —
reported a core dump that did not exist. It did exactly that once, against a
device that answered "no core dump stored" to the very next question. It now
matches the positive form, `core dump: N bytes at flash`, and reports an
unanswered query as an unanswered query. A check that invents findings is worse
than no check, because the next real one gets waved away with it.

## Stress, and why it is not in the default run

```bash
make stress                 # 30 runs at 2000 lines, expects zero failures
make stress N=100           # longer
./tests/run.sh --suite stress --stress --stress-lines 5000
```

`make test` does not chase intermittent faults, which is why this is separate: a
check that fails a few times in thirty makes the default run red, and an
intermittently red suite is ignored within a week — taking the credibility of
every other assertion with it.

The default is 2000 lines, and the history is the reason. This suite was written
to characterise the `Corrupted MAC` bug in
[KNOWN-ISSUES](../docs/KNOWN-ISSUES.md), and it established that the fault was a
cliff rather than a slope: clean at 8, 25, 50 and 100 lines, and 26 bad in 30 at
200. While that was open the default sat at 100, *below* the cliff, so that any
failure meant a new regression rather than the known bug.

The bug is fixed — the send and receive paths shared one buffer under two
different locks — so the default now sits where the fault used to be reliable
(6 runs in 8 at 2000 lines). It guards the regression instead of avoiding it.

Two rules survive from chasing it, and they apply to the next intermittent fault
as much as they did to this one. Do not add firmware logging: that fault
vanished under instrumentation (0/140 with per-packet tracing, 2/30 without), so
a fix has to be proven on a build with no tracing in it. And do not trust a short
clean run — 0/60 was recorded with the bug demonstrably present. What finally
caught it was none of this: it was watching the serial console, on `dmesg -n
debug`, while the reproducer ran.

## Ways this harness has lied, and what stops them now

Each of these had already cost a session before it was found. They are recorded
because the failure they produce looks nothing like their cause, and because
this project's real problem has never been finding bugs — it has been trusting
what the harness says about them.

A third belongs in this list and is written up under [The health
check](#the-health-check) instead, because that is where the code is: the
core-dump test matched on the *absence* of "no core dump stored", so a query
that went unanswered reported a dump that did not exist.

**A dead session used to read as empty output.** `session.py` reported its
errors only on stderr — into a file `device.sh` opened, never read, and then
deleted — and broke its loop without closing the frame. `dev_run` read to EOF
and returned `""`. Every subsequent assertion in that suite then compared
against `""`, and **all seven** `assert_eq "..." ""` assertions in the tree are
in `15-streams.sh`, so losing the session turned the suite covering the stream
split green. `dev_run` now returns `<<<dead-session>>>` instead, which fails
those comparisons instead of satisfying them.

The fix needed two goes, and the reason is worth keeping: `dev_run` is almost
always called as `$(dev_run ...)`, which is a subshell. Writing to the FIFO
after `session.py` has gone killed that subshell with SIGPIPE *before any guard
could run*, and command substitution renders a killed subshell as `""` — the
very value being guarded against. So the function ignores `PIPE` and checks the
write. It also cannot remember anything between calls, being a subshell, which
is why the check is on the write rather than on a flag.

**`espix_timeout` used to leave the process it killed.** It runs `"$@" &` and
signalled that pid — but `run.sh`'s preflight passed it `dev_status`, a shell
*function*, so the pid was a subshell and the `ssh` beneath it survived. espix
accepts four connections and each orphan holds one until the machine is
rebooted, so they accumulate across runs until the device answers nobody and
every suite hangs with no output. Three were found alive on the author's
machine — `uptime`, `coredump` and a mistyped command, hours apart — while
investigating exactly that symptom, and it had been read as device flakiness.
It now kills the process group, and the one-shot helpers wrap the `ssh` binary
directly rather than a function of ours.

The wider lesson, which is the same one the transport bug taught: prove the
harness before believing what it says about the system. Both of these were
confirmed by making the *old* behaviour fail a test that the new behaviour
passes, not by reasoning about the code.

## The console suite used to be flaky, and why it was not the harness

For a long time this suite failed about one full run in three while passing
every time on its own, and it was written up here as a harness problem. That was
wrong, and the way it was wrong is worth keeping.

The device's console genuinely stopped: silent in both directions, `main` alive
and blocked, nothing on the host holding the port, SSH perfectly healthy
throughout, and only a reset recovering it. A core dump taken *while wedged* --
`crash` over SSH, then reading the `main` thread rather than the faulting one --
named it in one step:

```
esp_linenoise_get_columns -> esp_linenoise_get_cursor_position
  -> console_read_bytes -> esp_vfs_select(..., timeout=0x0)
```

The editor asks the terminal where the cursor is twice per prompt and then reads
until it gets an answer. `console_read_bytes()` waited with a NULL timeout,
deliberately, so an idle prompt costs nothing. With no terminal attached the
answer never came, and the timeout that was supposed to cover exactly this --
`s_report_deadline_us`, with `s_terminal_mute` and a synthesised reply behind it
-- was only ever consulted *after a byte arrived*. No byte, no deadline check,
console parked for good.

The fix is to bound the wait to what is left of the report window while a report
is outstanding, which makes the existing timeout reachable. See
`console_read_bytes()` in `tty_console.c`.

Two things this cost, both avoidable. The suite spawning a `console.py` per
command was blamed first; that was a real fault and worth fixing, but fixing it
changed nothing here, and "the obvious suspect improved and the symptom
remained" should have been the signal to stop guessing and take a dump. And the
harness was blamed before the device, when the device was answering every
question put to it over SSH the whole time.

## The test app

`tests/app/` is its own IDF project, so `tools/build-apps.sh` — which globs
`apps/*/` — leaves it alone and it never lands in a normal rootfs image. `make
test-app` builds it and stages it to `fsroot/home/esp/testapp`, the same path
the suite copies to, so the image route and the copy route agree.

Staging under a home directory is not just tidiness: espix's ownership rule
gives a file to the account whose home contains it, and its mode rule sees ELF
magic — so it arrives `esp:esp` and `0755` with no chown, no chmod, and no
attribute data in the image.

The suite copies it only when it is stale, compared by a SHA-256 in a sidecar
(`.testapp.sha`) because espix has no checksum command. Binary first, sidecar
second: an interrupted copy then leaves a stale hash and the next run copies
again, rather than a wrong binary vouched for by a correct one.

`testapp sig` used to be a separate project, `apps/sigtest`. It sat in the
*examples* directory, which meant `tools/build-apps.sh` built it on every
firmware build and shipped it in every rootfs image, while `apps/README.md`'s
table never listed it — and the signal behaviour it demonstrated had no
automated coverage at all. Folding it in cost one binary, one build and one
`scp` less than a second staging path would have, and bought
`tests/suites/35-signals.sh`.

Run `testapp` with no arguments for its subcommands. `out <n>` prints n lines
and exists specifically to give the `Corrupted MAC` bug in
[KNOWN-ISSUES](../docs/KNOWN-ISSUES.md) a cheap, repeatable handle.
