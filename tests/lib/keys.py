#!/usr/bin/env python3
"""Run one command on a pty and type at it: what a person at a terminal does.

    tests/lib/keys.py --host H --user U --password P --cmd CMD STEP...

Each STEP, in order:
    expect:TEXT     wait until TEXT has been written since the command started
    send:HEX        write these bytes in one write, as a key's bytes arrive
    sleep:SECONDS   wait
    mark            what is printed starts here instead
    prompt          wait for the shell's prompt: the command has finished
    type:TEXT       type TEXT and Enter at the prompt, another command

Then wait for the prompt, and print what the command wrote from the last mark,
escapes removed (kept with --raw). A step that times out prints
<<<KEYS-TIMEOUT ...>>> and exits 1.
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from session import Session, strip_ansi  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--user", required=True)
    ap.add_argument("--password", required=True)
    ap.add_argument("--cmd", required=True)
    ap.add_argument("--timeout", type=int, default=25)
    ap.add_argument("--raw", action="store_true")
    ap.add_argument("steps", nargs="*")
    a = ap.parse_args()
    shown = (lambda b: b) if a.raw else strip_ansi

    s = Session(a.host, a.user, a.password, a.timeout)
    start = len(s.buf)
    os.write(s.fd, a.cmd.encode() + b"\r")
    mark = start
    try:
        for step in a.steps:
            kind, _, arg = step.partition(":")
            if kind == "expect":
                deadline = time.time() + a.timeout
                while arg.encode() not in strip_ansi(s.buf[start:]):
                    if not s._read_some(deadline):
                        raise TimeoutError(f"never saw {arg!r}")
            elif kind == "send":
                os.write(s.fd, bytes.fromhex(arg))
            elif kind == "sleep":
                deadline = time.time() + float(arg)
                while s._read_some(deadline):
                    pass
            elif kind == "mark":
                mark = len(s.buf)
            elif kind == "prompt":
                s._wait_prompt()
                start = len(s.buf)
            elif kind == "type":
                os.write(s.fd, arg.encode() + b"\r")
            else:
                raise ValueError(f"no step {step!r}")
        s._wait_prompt()
    except Exception as exc:                      # noqa: BLE001
        print(shown(s.buf[mark:]).decode("utf-8", "replace"))
        print(f"<<<KEYS-TIMEOUT {exc}>>>")
        return 1
    finally:
        s.close()
    print(shown(s.buf[mark:]).decode("utf-8", "replace"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
