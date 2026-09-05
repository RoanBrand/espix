# espix tests

```bash
make test                            # everything
make test SUITE=fs                   # suites whose name contains "fs"
make test PORT=/dev/ttyUSB0          # include the serial-console suites
./tests/run.sh --host 10.0.0.5       # a device somewhere else
```

`make test` finds the ESP-IDF and the serial port for itself, builds the test
app if it is stale, and copies it over. Nothing needs to be sourced first.

## What is here

| | |
|---|---|
| `run.sh` | finds the suites, runs them, checks the device between each, reports |
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
# PARALLEL_SAFE=no -- says whether it can ever run beside another suite.

assert_eq "pwd is the home" "/home/$ESPIX_USER" "$(dev_run 'pwd')"
assert_contains "id reports a uid" "uid=" "$(dev_run 'id')"
assert_status "a bad command exits 127" 127 dev_status 'nosuchthing'
```

Number them so the order is obvious; cheap and read-only first.

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
  long-lived session in `session.py` is the speed answer instead.

## Speed, and parallelism

A login costs a key exchange plus PBKDF2 at 20 000 iterations, so the runner
logs in **once per suite** and `dev_run` reuses it. `dev_status` is the
exception and is deliberately rationed.

Nothing runs in parallel yet, and that is a choice rather than an omission. The
device is one target with four session slots and one serial port, and suites
share filesystem state. This project's actual problem is *not trusting results*;
a flaky suite is worse than a slow one. Each suite declares `PARALLEL_SAFE` so
it can be switched on later against evidence.

## The health check

After every suite the runner asks the device how it is: reset reason, uptime
going backwards, a new core dump, and how many `sshd:conn` tasks are alive. A
failure is attributed to the suite that just ran.

This exists because the most expensive bug in this project's history presented
as a WiFi-task assert and an ipc0 scheduler fault, and was really a filesystem
recursion — found only once somebody thought to ask the device how it was.
Nobody remembers to ask, so the runner does.

Note the uptime comparison in particular: checking the *reset reason* alone
misses a reboot, because two software reboots in a row read identically. That
was caught by rebooting a device mid-check and watching the first version not
notice.

## Stress, and why it is not in the default run

```bash
make stress                 # 30 runs at 100 lines, expects zero failures
make stress N=100           # longer
./tests/run.sh --suite stress --stress --stress-lines 200 --stress-limit 100
```

`make test` does not chase intermittent faults. A check that fails a few times
in thirty would make the default run red for a bug that is already documented
and open, and an intermittently red suite is ignored within a week — taking the
credibility of every other assertion with it.

The default sits at 100 lines **below** the threshold where the transport starts
failing, so any failure there is a real regression. Above it — `--stress-lines
200` — is the reproducer for the `Corrupted MAC` entry in
[KNOWN-ISSUES](../docs/KNOWN-ISSUES.md), which turns out to be a cliff rather
than a slope: clean at 8, 25, 50 and 100 lines, and 26 bad in 30 at 200.

Two rules for anything that chases it. Do not add firmware logging: the fault
vanishes under instrumentation (0/140 with per-packet tracing, 2/30 without).
And do not trust a short clean run — 0/60 was recorded with the bug demonstrably
present.

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

Run `testapp` with no arguments for its subcommands. `out <n>` prints n lines
and exists specifically to give the `Corrupted MAC` bug in
[KNOWN-ISSUES](../docs/KNOWN-ISSUES.md) a cheap, repeatable handle.
