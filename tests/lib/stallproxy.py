#!/usr/bin/env python3
"""A TCP relay that goes deliberately, permanently quiet mid-session.

For testing what espix does when a peer stops reading *without closing*. That
distinction is the whole point: a client that stops reading and then closes was
always handled correctly -- the send fails and teardown runs. The one that
strands a connection task is the peer that just goes silent and holds the socket
open, because a blocking send() on a full window waits forever.

An SSH client cannot express this. Its own reader thread keeps draining the
socket, so the TCP window never shuts. Hence a relay: it forwards normally until
the session is streaming, then stops reading from the device and holds both
sockets open indefinitely.

Only the standard library, so the test suite gains no new dependency.

    stallproxy.py --port 2223 --host 192.168.110.55 --stall-after 8192 \
                  --marker /tmp/stalled

Writes the marker file once it has gone quiet, so a shell script can wait for
that moment rather than guessing at a sleep.
"""

import argparse
import os
import socket
import sys
import threading
import time


def pump(src, dst, on_bytes=None):
    """Forward until the source ends. Returns quietly on a dead socket."""
    try:
        while True:
            data = src.recv(4096)
            if not data:
                return
            dst.sendall(data)
            if on_bytes is not None and on_bytes(len(data)):
                return          # caller says stop; do NOT close anything
    except OSError:
        return


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True, help="local listen port")
    ap.add_argument("--host", required=True, help="device address")
    ap.add_argument("--device-port", type=int, default=22)
    ap.add_argument("--stall-after", type=int, default=8192,
                    help="bytes to relay from the device before going silent")
    ap.add_argument("--marker", required=True,
                    help="file to create once stalled")
    args = ap.parse_args()

    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", args.port))
    listener.listen(1)
    print("listening on 127.0.0.1:%d" % args.port, flush=True)

    client, _ = listener.accept()
    device = socket.socket()
    device.connect((args.host, args.device_port))

    threading.Thread(target=pump, args=(client, device), daemon=True).start()

    relayed = [0]

    def count(n):
        relayed[0] += n
        if relayed[0] < args.stall_after:
            return False
        # Stop reading the device. Both sockets stay open: closing either one
        # is the case that already worked, and would prove nothing.
        with open(args.marker, "w") as fh:
            fh.write("%d\n" % relayed[0])
        print("stalled after %d bytes; sockets held open" % relayed[0],
              flush=True)
        return True

    pump(device, client, count)

    if not os.path.exists(args.marker):
        # The session ended before it ever streamed enough to stall, so the
        # test that follows would be measuring nothing. Say so rather than
        # letting it look like a pass.
        print("never reached --stall-after; relayed only %d bytes" % relayed[0],
              file=sys.stderr, flush=True)
        sys.exit(1)

    while True:                     # hold everything open until killed
        time.sleep(3600)


if __name__ == "__main__":
    main()
