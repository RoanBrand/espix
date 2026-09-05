#!/usr/bin/env python3
"""Drive espix's serial console.

The console is the only place root is exercised without unlocking the account
over SSH, so it is worth being able to script even though it is fiddly.

Two things make it fiddly, both learned the hard way:

  * esp_linenoise probes the terminal for its size with ESC[999C ESC[6n and
    *blocks until something answers*. A plain reader never answers, so the
    console appears wedged and the prompt never arrives. This driver answers.

  * A serial capture contains NUL bytes, so anything grepping it needs -a.
    Without that, grep declares the file binary and prints nothing, which reads
    exactly like the thing you searched for being absent.

Needs pyserial, which the system python usually lacks and the ESP-IDF venv
always has; the Makefile passes that interpreter down as ESPIX_PYTHON.

Usage:
    console.py --port /dev/ttyUSB0 [--reset] [--timeout N] < commands
Framing matches session.py: <<<ESPIX-CMD n>>> ... <<<ESPIX-END n>>>
"""

import argparse
import re
import subprocess
import sys
import time

try:
    import serial
except ImportError:
    print("console.py: pyserial not installed -- run via the ESP-IDF python "
          "(the Makefile passes it as ESPIX_PYTHON)", file=sys.stderr)
    sys.exit(2)

PROMPT = re.compile(rb"[a-zA-Z0-9_.-]+:[^\r\n]*[#$] $")
ANSI = re.compile(rb"\x1b\[[0-9;?]*[a-zA-Z]")
CURSOR_PROBE = b"\x1b[6n"


class Console:
    def __init__(self, port, baud=115200, timeout=30, reset=False):
        self.timeout = timeout
        self.ser = serial.Serial(port, baud, timeout=0.1)
        if reset:
            # DTR/RTS wiggle is what esptool uses to reset the board.
            self.ser.setDTR(False)
            self.ser.setRTS(True)
            time.sleep(0.1)
            self.ser.setRTS(False)

    def _pump(self, deadline):
        """Read what is available, answering any cursor-position probe."""
        chunk = self.ser.read(4096)
        if chunk:
            n = chunk.count(CURSOR_PROBE)
            for _ in range(n):
                self.ser.write(b"\x1b[1;120R")
                self.ser.flush()
        return chunk, time.time() < deadline

    def wait_prompt(self):
        buf = b""
        deadline = time.time() + self.timeout
        while True:
            chunk, alive = self._pump(deadline)
            buf += chunk
            tail = ANSI.sub(b"", buf)[-160:]
            for line in tail.split(b"\n")[::-1]:
                if PROMPT.search(line.rstrip() + b" "):
                    return buf
            if not alive:
                raise TimeoutError(f"no console prompt; got: {buf[-300:]!r}")

    def run(self, command):
        if ";" in command:
            raise ValueError("espix's shell has no ';' -- one command per call")

        # Ctrl-U first, to clear whatever is sitting in the line editor.
        #
        # Not defensive tidying: linenoise re-probes for the cursor position
        # each time it starts editing a line, and the reply this driver sends is
        # *typed into that line*. Without clearing, `whoami` goes to espix as
        # "\x1b[1;120Rwhoami" and comes back as "command not found" -- which
        # looks like a broken shell rather than a broken harness.
        self.ser.write(b"\x15")
        self.ser.flush()
        time.sleep(0.15)
        self.ser.reset_input_buffer()

        self.ser.write(command.encode() + b"\r")
        self.ser.flush()
        raw = self.wait_prompt()

        text = ANSI.sub(b"", raw).decode("utf-8", "replace")
        lines = text.replace("\r\n", "\n").replace("\r", "\n").split("\n")

        echo_re = re.compile(
            r"^[a-zA-Z0-9_.-]+:[^\r\n]*[#$]\s+" + re.escape(command) + r"\s*$")
        last_echo = -1
        for i, line in enumerate(lines):
            if echo_re.match(line) or line.strip() == command:
                last_echo = i
        if last_echo >= 0:
            lines = lines[last_echo + 1:]

        while lines and (not lines[-1].strip()
                         or PROMPT.search((lines[-1].rstrip() + " ").encode())):
            lines = lines[:-1]
        while lines and not lines[0].strip():
            lines = lines[1:]
        return "\n".join(lines)

    def close(self):
        try:
            self.ser.close()
        except Exception:                          # noqa: BLE001
            pass


def port_holder(port):
    """Which process, if any, has the serial port open. Empty string if none.

    lsof is on macOS and on most Linux images; if it is missing or says nothing
    we simply do not know, and the caller says so rather than guessing.
    """
    try:
        # No -t: it means "terse, pids only" and silently overrides -F, which
        # made an earlier version of this report "nothing holds the port" every
        # single time -- a check that could only ever say absent.
        out = subprocess.run(["lsof", "-F", "cn", port],
                             capture_output=True, text=True, timeout=5).stdout
    except (OSError, subprocess.SubprocessError):
        return ""

    pids, names = [], []
    for line in out.splitlines():
        if line.startswith("p"):
            pids.append(line[1:])
        elif line.startswith("c"):
            names.append(line[1:])
    if not pids:
        return ""
    return ", ".join("%s (pid %s)" % (n, p)
                     for n, p in zip(names or ["?"] * len(pids), pids))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--timeout", type=int, default=30)
    ap.add_argument("--reset", action="store_true")
    args = ap.parse_args()

    try:
        c = Console(args.port, args.baud, args.timeout, args.reset)
    except Exception as exc:                       # noqa: BLE001
        print(f"console.py: cannot open {args.port}: {exc}", file=sys.stderr)
        return 1

    # Sync on a prompt before sending anything, retrying a few times.
    #
    # One attempt is not enough in practice: the console may be sitting
    # mid-probe from an earlier reader, or part-way through a line somebody left
    # behind, and a single newline does not always shake that out. Ctrl-U first
    # clears any partial line, then a newline asks for a fresh prompt. Observed
    # failing about one run in three without this, which for a test suite is
    # worse than failing every time.
    synced = False
    for _ in range(3):
        try:
            c.ser.write(b"\x15\r")
            c.ser.flush()
            c.wait_prompt()
            synced = True
            break
        except TimeoutError:
            time.sleep(1)

    if not synced:
        # Name the culprit rather than asking the reader to go and look.
        #
        # By far the most common cause of this failure is not espix and not the
        # cable: it is a second reader on the same port -- a forgotten
        # `idf.py monitor`, or a serial capture left running while debugging
        # something else. Note that /dev/cu.* is *not* exclusive on macOS, so
        # both processes open it happily and then steal each other's bytes:
        # the prompt this is waiting for gets consumed by the other reader.
        # That makes it fail intermittently rather than always, which is worse.
        #
        # "Is anything else holding the port?" is a question the program can
        # answer for itself, so it does.
        print("console.py: no prompt after three attempts", file=sys.stderr)
        holder = port_holder(args.port)
        if holder:
            print("console.py: %s is holding %s -- stop it and re-run"
                  % (holder, args.port), file=sys.stderr)
        else:
            print("console.py: nothing else holds %s, so check the device is "
                  "up and the cable is in" % args.port, file=sys.stderr)
        c.close()
        return 1

    rc = 0
    try:
        for n, line in enumerate(sys.stdin):
            cmd = line.rstrip("\n")
            if not cmd:
                continue
            print(f"<<<ESPIX-CMD {n}>>>", flush=True)
            try:
                print(c.run(cmd), flush=True)
            except Exception as exc:               # noqa: BLE001
                print(f"console.py: {exc}", file=sys.stderr)
                rc = 1
                break
            print(f"<<<ESPIX-END {n}>>>", flush=True)
    finally:
        c.close()
    return rc


if __name__ == "__main__":
    sys.exit(main())
