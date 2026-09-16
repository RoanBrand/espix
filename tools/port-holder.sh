#!/usr/bin/env bash
#
# Who holds the serial port, if anyone.
#
#   tools/port-holder.sh /dev/cu.usbserial-210
#
# Exits 0 with no output when the port is free, and 1 having listed the holders
# when it is not. Callers use that to refuse rather than to share.
#
# Why this is needed at all: on macOS a /dev/cu.* device can be opened by any
# number of processes and they simply split the byte stream between them, so two
# readers produce a capture with holes in it rather than an error. The /dev/tty.*
# node for the *same* UART is exclusive, which makes it the question the kernel
# can answer: open it non-blocking and EBUSY means somebody else has the port,
# whichever node they opened it through. That is the test below, and lsof then
# supplies the names, because "in use" without "by whom" is only half an answer.
#
# Deliberately does not kill anything. A reader is usually a capture somebody
# started on purpose -- tools/serlog.sh most of the time -- and the useful thing
# is to say so and let the caller decide.
set -u

if [ "$#" -ne 1 ] || [ -z "$1" ]; then
    echo "usage: port-holder.sh /dev/cu.something" >&2
    exit 2
fi

port=$1
tty_port=${port/cu./tty.}

# A tty.* that does not exist at all means the port is gone, not busy.
if [ ! -e "$tty_port" ]; then
    echo "port-holder.sh: $tty_port does not exist" >&2
    exit 2
fi

if python3 - "$tty_port" <<'PY'
import errno, os, sys
try:
    fd = os.open(sys.argv[1], os.O_RDWR | os.O_NONBLOCK)
    os.close(fd)
except OSError as exc:
    # EBUSY is "someone has it"; anything else is a different problem, and the
    # caller should not be told the port is in use when it is not.
    sys.exit(1 if exc.errno == errno.EBUSY else 0)
PY
then
    exit 0
fi

# Held. Name the holders when we can, and fail either way -- a missing lsof
# (minimal installs) must not turn a busy port into a free one.
# shellcheck disable=SC2009
lsof 2>/dev/null | grep -i "$(basename "$port")" | sed 's/^/  /'
exit 1
