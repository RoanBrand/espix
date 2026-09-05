#!/usr/bin/env python3
"""A persistent interactive SSH session to an espix device.

Why this exists rather than `ssh host <cmd>` per assertion: a login costs a key
exchange plus PBKDF2 at 20 000 iterations, several seconds, and a suite makes
dozens of assertions. One login for the suite turns minutes into seconds.

Why it cannot be `ssh -O` multiplexing instead: espix's SSH server supports
exactly one channel per connection ("our channel id; only ever one" in
ssh_channel.c), so ControlMaster has nowhere to put a second session. Do not
spend an afternoon rediscovering that.

The prompt is the delimiter, and it carries information: `esp:~$ ` versus
`root:~#` says which identity answered, so a session that silently lost its
privilege cannot masquerade as a passing test.

Usage:
    session.py --host H --user U --password P [--timeout N] < commands
Each input line is one command. Output is framed per command:
    <<<ESPIX-CMD n>>>
    ...output...
    <<<ESPIX-END n>>>
"""

import argparse
import os
import pty
import re
import select
import sys
import time

# `esp:~$ ` or `root:/x# `, with any colouring the shell might add stripped
# first. Anchored at end-of-buffer: a prompt string appearing inside output
# would otherwise end the read early.
PROMPT = re.compile(rb"[a-zA-Z0-9_.-]+:[^\r\n]*[#$] $")
ANSI = re.compile(rb"\x1b\[[0-9;?]*[a-zA-Z]")


def strip_ansi(data):
    return ANSI.sub(b"", data).replace(b"\x1b[6n", b"")


class Session:
    def __init__(self, host, user, password, timeout=25):
        self.timeout = timeout
        self.buf = b""
        argv = [
            "ssh", "-tt",
            "-o", "StrictHostKeyChecking=no",
            "-o", "UserKnownHostsFile=/dev/null",
            "-o", "NumberOfPasswordPrompts=1",
            # Deliberately NOT LogLevel=ERROR: it hides the one diagnostic that
            # explains a dropped connection, which cost this project a day.
            f"{user}@{host}",
        ]
        env = dict(os.environ)
        # A dumb terminal, deliberately. espix runs esp_linenoise, which in a
        # capable terminal echoes every keystroke and redraws the whole line --
        # so a three-word command comes back as a dozen partial echoes that any
        # output parser then has to unpick. Dumb mode echoes once and stops
        # probing for the cursor position, which is both simpler and quieter.
        env["TERM"] = "dumb"
        # An askpass would answer the prompt *instead of* us, with whatever it
        # was told to say -- which is how a root login test once got the esp
        # password and looked like a server-side refusal.
        for k in ("SSH_ASKPASS", "SSH_ASKPASS_REQUIRE", "DISPLAY"):
            env.pop(k, None)

        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.execvpe("ssh", argv, env)
            os._exit(127)

        self._expect(b"assword:")
        os.write(self.fd, password.encode() + b"\r")
        self._wait_prompt()
        self.prompt = self._last_prompt()

    def _last_prompt(self):
        """The prompt string this session synced on.

        Worth reporting rather than discarding: the sigil says which identity
        answered -- `$` for an ordinary account, `#` for root -- so a session
        that quietly has more privilege than the test intended cannot pass as
        one that does not.
        """
        for line in strip_ansi(self.buf).decode("utf-8", "replace").splitlines()[::-1]:
            if PROMPT.search((line.rstrip() + " ").encode()):
                return line.strip()
        return ""

    def _read_some(self, deadline):
        remaining = deadline - time.time()
        if remaining <= 0:
            return False
        r, _, _ = select.select([self.fd], [], [], min(remaining, 0.5))
        if not r:
            return True
        try:
            chunk = os.read(self.fd, 4096)
        except OSError:
            return False
        if not chunk:
            return False
        self.buf += chunk
        return True

    def _expect(self, needle):
        deadline = time.time() + self.timeout
        while needle not in self.buf:
            if not self._read_some(deadline):
                raise TimeoutError(
                    f"never saw {needle!r}; got: {self.buf[-400:]!r}")
        return True

    def _wait_prompt(self):
        deadline = time.time() + self.timeout
        while True:
            tail = strip_ansi(self.buf).rstrip(b"\x00")
            # Trailing CR/LF before the prompt is normal; only the very end
            # matters.
            if PROMPT.search(tail.split(b"\n")[-1] + b" ") or \
               PROMPT.search(tail[-120:]):
                return
            if not self._read_some(deadline):
                raise TimeoutError(f"no prompt; got: {self.buf[-400:]!r}")

    def run(self, command):
        """Send one command, return its output with the echo and prompt gone."""
        if ";" in command:
            raise ValueError(
                "espix's shell has no ';' -- send one command per call, "
                "or the second half runs as an argument to the first")
        self.buf = b""
        os.write(self.fd, command.encode() + b"\r")
        self._wait_prompt()

        text = strip_ansi(self.buf).decode("utf-8", "replace")
        lines = text.replace("\r\n", "\n").replace("\r", "\n").split("\n")

        # espix's line editor echoes every keystroke and redraws the whole line,
        # so a command arrives back as a stack of partial echoes:
        #
        #     w
        #     esp:~$ wh
        #     esp:~$ who
        #     esp:~$ whoami whoami
        #     esp:~$ whoami        <- the last one: the line as submitted
        #     esp                  <- output starts here
        #     esp:~$               <- next prompt
        #
        # The real output begins after the *last* line that is a prompt followed
        # by exactly the command. Matching on "contains the command" instead
        # would pick a line out of the output for something like `echo whoami`.
        echo_re = re.compile(
            r"^[a-zA-Z0-9_.-]+:[^\r\n]*[#$]\s+" + re.escape(command) + r"\s*$")
        last_echo = -1
        for i, line in enumerate(lines):
            if echo_re.match(line):
                last_echo = i
        if last_echo >= 0:
            lines = lines[last_echo + 1:]

        # Drop the trailing prompt, and any blank the redraw left behind.
        while lines and (not lines[-1].strip()
                         or PROMPT.search((lines[-1].rstrip() + " ").encode())):
            lines = lines[:-1]
        while lines and not lines[0].strip():
            lines = lines[1:]
        return "\n".join(lines)

    def close(self):
        try:
            os.write(self.fd, b"exit\r")
            time.sleep(0.3)
        except OSError:
            pass
        try:
            os.close(self.fd)
        except OSError:
            pass
        try:
            os.waitpid(self.pid, os.WNOHANG)
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--user", required=True)
    ap.add_argument("--password", required=True)
    ap.add_argument("--timeout", type=int, default=25)
    args = ap.parse_args()

    try:
        s = Session(args.host, args.user, args.password, args.timeout)
    except Exception as exc:                      # noqa: BLE001
        print(f"session.py: cannot log in: {exc}", file=sys.stderr)
        return 1

    print(f"<<<ESPIX-PROMPT {s.prompt}>>>", flush=True)

    rc = 0
    try:
        for n, line in enumerate(sys.stdin):
            cmd = line.rstrip("\n")
            if not cmd:
                continue
            print(f"<<<ESPIX-CMD {n}>>>", flush=True)
            try:
                print(s.run(cmd), flush=True)
            except Exception as exc:              # noqa: BLE001
                print(f"session.py: {exc}", file=sys.stderr)
                rc = 1
                break
            print(f"<<<ESPIX-END {n}>>>", flush=True)
    finally:
        s.close()
    return rc


if __name__ == "__main__":
    sys.exit(main())
