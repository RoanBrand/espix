"""Raw serial capture for tools/serlog.sh. Never writes to the port.

Split out of the shell script so the running process has a command line that
says what it is -- the previous version exec'd python with a heredoc, which left
nothing for `pkill -f serlog` to match and made it survive being stopped.
"""

import sys
import time

import serial

HEARTBEAT_S = 60


def main() -> int:
    port, out = sys.argv[1], sys.argv[2]

    # Append, never truncate: a second capture against the same file must not
    # throw away the panic the first one caught.
    f = open(out, "ab", buffering=0)
    f.write(b"=== serlog start %s ===\n"
            % time.strftime("%Y-%m-%d %H:%M:%S").encode())

    buf = b""
    ser = None
    last_out = time.time()

    while True:
        try:
            if ser is None:
                # A blocking read with a deadline costs no CPU between lines,
                # and a test run has long quiet stretches.
                ser = serial.Serial(port, 115200, timeout=0.2)
            chunk = ser.read(4096)
        except (serial.SerialException, OSError) as exc:
            # Reached when something else opens the same cu.* device -- macOS
            # permits that, and both readers then get partial reads. Dying here
            # is what lost the capture the first time, so reopen and carry on: a
            # gap in the log beats no log.
            f.write(b"%s [serlog] %s -- reopening\n"
                    % (time.strftime("%H:%M:%S").encode(), str(exc).encode()))
            last_out = time.time()
            try:
                if ser is not None:
                    ser.close()
            except Exception:
                pass
            ser = None
            time.sleep(0.5)
            continue

        now = time.time()

        if not chunk:
            # The heartbeat, and the whole reason this loop is not just a read.
            #
            # Without it a dead capture and an idle board look identical: the
            # log simply stops. That misread cost three wrong conclusions in one
            # session, including "the device is emitting nothing" about a board
            # that was answering commands at the time. A line once a minute
            # makes silence provable rather than assumed.
            if now - last_out >= HEARTBEAT_S:
                f.write(b"%s [serlog] quiet\n"
                        % time.strftime("%H:%M:%S").encode())
                last_out = now
            continue

        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            # Timestamped per line, because attributing a panic to the suites
            # running at that moment is the point; run.sh's monitor records
            # which those were, against the same clock.
            f.write(b"%s %s\n" % (time.strftime("%H:%M:%S").encode(),
                                  line.rstrip(b"\r")))
        last_out = now


if __name__ == "__main__":
    sys.exit(main())
