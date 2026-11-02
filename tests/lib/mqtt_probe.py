#!/usr/bin/env python3
"""mqtt_probe.py <host> <port> <message>: subscribe, publish, and print what came back.

An MQTT 3.1.1 round trip over the standard library, so the broker suites need no
client installed: CONNECT, SUBSCRIBE to a topic of its own, PUBLISH the message
at QoS 0 and wait for the broker to deliver it. Prints "got <message>", or why not.
"""
import os
import socket
import struct
import sys


def remaining(n):
    out = bytearray()
    while True:
        b = n % 128
        n //= 128
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def string(s):
    b = s.encode()
    return struct.pack("!H", len(b)) + b


def packet(kind, body):
    return bytes([kind]) + remaining(len(body)) + body


def read_packet(sock):
    head = sock.recv(1)
    if not head:
        raise EOFError("the broker closed the connection")
    n, shift = 0, 0
    while True:
        b = sock.recv(1)[0]
        n |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            break
    body = b""
    while len(body) < n:
        chunk = sock.recv(n - len(body))
        if not chunk:
            raise EOFError("short packet")
        body += chunk
    return head[0], body


def main():
    host, port, message = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    topic = "espix/probe/%d" % os.getpid()
    with socket.create_connection((host, port), timeout=10) as s:
        s.sendall(packet(0x10, string("MQTT") + bytes([4, 0x02]) + struct.pack("!H", 30) +
                         string("probe-%d" % os.getpid())))
        kind, body = read_packet(s)
        if kind != 0x20 or body[1] != 0:
            print("connack: %02x %s" % (kind, body.hex()))
            return 1
        s.sendall(packet(0x82, struct.pack("!H", 1) + string(topic) + b"\x00"))
        kind, _ = read_packet(s)
        if kind != 0x90:
            print("suback: %02x" % kind)
            return 1
        s.sendall(packet(0x30, string(topic) + message.encode()))
        kind, body = read_packet(s)
        if kind & 0xF0 != 0x30:
            print("publish: %02x" % kind)
            return 1
        tlen = struct.unpack("!H", body[:2])[0]
        print("got %s" % body[2 + tlen:].decode(errors="replace"))
        s.sendall(packet(0xE0, b""))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, EOFError) as e:
        print("mqtt: %s" % e)
        sys.exit(1)
