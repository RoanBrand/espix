#!/usr/bin/env python3
"""A minimal RFB client, for testing the desktop.

Why this exists rather than "use vncviewer": every check worth making about the
desktop is a claim about *pixels*, and a viewer gives you a window to look at
instead. Reading the framebuffer back as bytes is what lets a test say "the
title bar of the focused window is blue", "these two frames are identical", or
"this frame cost 9 KiB on the wire".

Hextile matters more than it looks. The same screen is 1,920,016 bytes as Raw --
800x600 at four bytes a pixel, whatever it contains -- and 9,516 bytes as
Hextile when it is mostly flat, which is the difference between a usable desktop
over WiFi and a slideshow. A client that only offers Raw cannot tell you which
of those the server is actually doing, and every timing it produces is a
measurement of its own bandwidth.

Only what the server implements is here: Raw, Hextile, RFB 3.8, VNC
authentication, and the 32bpp true-colour format. No CopyRect, no ZRLE, no
colour maps -- and the day the server grows one, the decoder to match belongs
here beside the others.

    c = Rfb("192.168.110.203")
    c.connect()
    fb = c.frame()
    c.pointer(736, 64, 1); c.pointer(736, 64, 0)
    fb = c.frame()

The framebuffer comes back as `w * h * 4` bytes: B, G, R, 0, which is the
little-endian 32bpp format asked for at handshake, so `fb[(y*w + x)*4 + 2]` is
red.
"""

import socket
import struct
import time

# RFB encoding numbers, and the pixel format asked for at handshake.
ENC_RAW = 0
ENC_COPYRECT = 1
ENC_HEXTILE = 5

# The VNC authentication challenge: DES with the password left-justified and the
# *bits of each byte* reversed, which is the one detail of this that is not
# guessable and cost an afternoon.
DEFAULT_PASSWORD = "espix"


def _mirror(byte):
    """VNC reverses the bits of every password byte. This is why."""
    out = 0
    for i in range(8):
        out = (out << 1) | ((byte >> i) & 1)
    return out


def _vnc_key(password):
    key = [(b & 0x7F) for b in password.encode()[:8]]
    key += [0] * (8 - len(key))
    key = [_mirror(b) for b in key]
    try:
        from Crypto.Cipher import DES          # pycryptodome, if it is around
        return DES.new(bytes(key), DES.MODE_ECB)
    except ImportError:
        pass
    try:
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
        return Cipher(algorithms.TripleDES(bytes(key) * 3), modes.ECB())
    except ImportError:
        return None


class RfbError(Exception):
    pass


class Reader:
    """A byte stream with look-ahead.

    Hextile is variable-length -- a uniform tile is a couple of bytes and a
    textured one is five hundred -- so there is no count to read up front and
    the decoder has to pull as it goes.
    """

    def __init__(self, sock):
        self.sock = sock
        self.buf = b""
        self.pos = 0
        self.consumed = 0

    def take(self, n):
        while len(self.buf) - self.pos < n:
            chunk = self.sock.recv(262144)
            if not chunk:
                raise EOFError("connection closed")
            self.buf += chunk
        out = self.buf[self.pos:self.pos + n]
        self.pos += n
        self.consumed += n
        if self.pos > (1 << 20):               # keep the buffer from growing
            self.buf = self.buf[self.pos:]
            self.pos = 0
        return out

    def byte(self):
        return self.take(1)[0]

    def u16(self):
        return struct.unpack(">H", self.take(2))[0]

    def u32(self):
        return struct.unpack(">I", self.take(4))[0]

    def skip(self, n):
        self.take(n)


class Rfb:
    def __init__(self, host, port=5900, password=DEFAULT_PASSWORD, timeout=30.0,
                 raw_only=False, copyrect=True):
        self.host = host
        self.port = port
        self.password = password
        self.timeout = timeout
        self.raw_only = raw_only
        self.copyrect = copyrect
        # The client's own framebuffer, which incremental updates are applied
        # to. A viewer is a long-lived framebuffer with updates painted into it,
        # not a sequence of unrelated full frames -- and asking for a full frame
        # after every event is how a drag gets measured as a whole screen per
        # motion, which is what this harness did until it was asked why.
        self.fb = None
        self.sock = None
        self.width = 0
        self.height = 0
        # What actually crossed the socket, so a claim about the cost of a frame
        # is a measurement rather than a memory of one.
        self.wire = {"bytes": 0, "frames": 0, "encodings": {}}

    # -- handshake ----------------------------------------------------

    def connect(self):
        self.sock = socket.create_connection((self.host, self.port), self.timeout)
        self.sock.settimeout(self.timeout)
        # Nagle off on this side too: a five-byte event coalesced with a delayed
        # ACK otherwise paces a round trip at tens of milliseconds.
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

        version = self._read(12)
        self.sock.sendall(b"RFB 003.008\n")

        count = self._read(1)[0]
        types = self._read(count)
        # "None" first when the server offers it, because a test that hashes a
        # password is testing the password and not the desktop. The VNC challenge
        # is here for the server that will not offer anything else.
        if 1 in types:
            self.sock.sendall(bytes([1]))
            result = struct.unpack(">I", self._read(4))[0]
            if result != 0:
                raise RfbError("authentication failed")
        elif 2 in types:
            self.sock.sendall(bytes([2]))
            challenge = self._read(16)
            self.sock.sendall(self._vnc_response(challenge))
            result = struct.unpack(">I", self._read(4))[0]
            if result != 0:
                raise RfbError("VNC authentication failed")
        else:
            raise RfbError("no usable security type in %r" % (types,))

        self.sock.sendall(bytes([0]))                     # ClientInit, not shared
        self.width, self.height = struct.unpack(">HH", self._read(4))
        self._read(16)                                    # the server's format
        name_len = struct.unpack(">I", self._read(4))[0]
        self._read(name_len)

        # 32bpp, depth 24, little-endian, true colour: four bytes a pixel, so
        # the framebuffer indexes are arithmetic.
        self.sock.sendall(bytes([0, 0, 0, 0, 32, 24, 0, 1]) +
                          struct.pack(">HHH", 255, 255, 255) +
                          bytes([16, 8, 0, 0, 0, 0]))

        if self.raw_only:
            encodings = [ENC_RAW]
        elif self.copyrect:
            encodings = [ENC_HEXTILE, ENC_COPYRECT, ENC_RAW]
        else:
            encodings = [ENC_HEXTILE, ENC_RAW]
        self.sock.sendall(bytes([2, 0]) + struct.pack(">H", len(encodings)) +
                          b"".join(struct.pack(">i", e) for e in encodings))
        return self

    def _read(self, n):
        out = b""
        while len(out) < n:
            chunk = self.sock.recv(n - len(out))
            if not chunk:
                raise EOFError("connection closed during handshake")
            out += chunk
        return out

    def _vnc_response(self, challenge):
        cipher = _vnc_key(self.password)
        if cipher is None:
            raise RfbError("VNC authentication needs pycryptodome or cryptography")
        if hasattr(cipher, "encrypt"):                    # pycryptodome
            return cipher.encrypt(challenge)
        encryptor = cipher.encryptor()                    # cryptography
        return encryptor.update(challenge) + encryptor.finalize()

    def close(self):
        if self.sock is not None:
            try:
                self.sock.close()
            finally:
                self.sock = None

    def __enter__(self):
        return self.connect()

    def __exit__(self, *exc):
        self.close()

    # -- input --------------------------------------------------------

    def pointer(self, x, y, buttons=0):
        self.sock.sendall(bytes([5, buttons & 0xFF]) + struct.pack(">HH", x, y))

    def click(self, x, y, settle=0.2):
        self.pointer(x, y, 1)
        time.sleep(0.05)
        self.pointer(x, y, 0)
        time.sleep(settle)

    def key(self, keysym, down):
        self.sock.sendall(bytes([4, 1 if down else 0, 0, 0]) +
                          struct.pack(">I", keysym))

    def type_text(self, text, gap=0.05):
        for ch in text:
            sym = 0xFF0D if ch == "\n" else ord(ch)
            self.key(sym, True)
            time.sleep(gap / 2)
            self.key(sym, False)
            time.sleep(gap)

    # -- output -------------------------------------------------------

    def frame(self, full=False):
        """One FramebufferUpdate, into this client's framebuffer.

        Incremental by default -- which is what a viewer asks for and what makes
        a timing mean anything, because the server then sends only what changed.
        `full=True` asks for the lot, which is what a check wants when it has to
        be sure of every pixel; the first frame on a connection is always full.

        The return is a *copy*, so a caller can hold on to it while the next
        update lands.
        """
        if self.fb is None:
            full = True

        request = bytes([3, 0 if full else 1]) + \
            struct.pack(">HHHH", 0, 0, self.width, self.height)
        self.sock.sendall(request)

        if full:
            self.fb = bytearray(self.width * self.height * 4)
        fb = self.fb

        r = Reader(self.sock)
        while True:
            mtype = r.byte()
            if mtype == 0:                                  # FramebufferUpdate
                r.skip(1)
                for _ in range(r.u16()):
                    x, y, w, h = r.u16(), r.u16(), r.u16(), r.u16()
                    enc = struct.unpack(">i", r.take(4))[0]
                    key = {ENC_RAW: "raw", ENC_HEXTILE: "hextile"}.get(enc, enc)
                    self.wire["encodings"][key] = \
                        self.wire["encodings"].get(key, 0) + 1
                    if enc == ENC_RAW:
                        self._raw(fb, x, y, w, h, r)
                    elif enc == ENC_HEXTILE:
                        self._hextile(fb, x, y, w, h, r)
                    elif enc == ENC_COPYRECT:
                        self._copyrect(fb, x, y, w, h, r)
                    else:
                        raise RfbError("server sent encoding %d, unasked" % enc)
                self.wire["bytes"] += r.consumed
                self.wire["frames"] += 1
                return bytearray(fb)
            elif mtype == 1:                                # SetColourMapEntries
                r.skip(3)
                r.u16()
                r.skip(r.u16() * 6)
            elif mtype == 2:                                # Bell
                pass
            elif mtype == 3:                                # ServerCutText
                r.skip(3)
                r.skip(r.u32())
            else:
                raise RfbError("unexpected server message %d" % mtype)

    def _copyrect(self, fb, x, y, w, h, r):
        """Four bytes: where to copy from. The pixels are the ones already in
        this framebuffer, which is the whole point -- a dragged window costs a
        header instead of a rectangle of pixels."""
        sx, sy = r.u16(), r.u16()
        for row in range(h):
            src = ((sy + row) * self.width + sx) * 4
            dst = ((y + row) * self.width + x) * 4
            fb[dst:dst + w * 4] = fb[src:src + w * 4]

    def _raw(self, fb, x, y, w, h, r):
        data = r.take(w * h * 4)
        for row in range(h):
            o = ((y + row) * self.width + x) * 4
            fb[o:o + w * 4] = data[row * w * 4:(row + 1) * w * 4]

    def _hextile(self, fb, x, y, w, h, r):
        """Tiles are counted from the *rectangle's* corner. RFC 6143: "the
        rectangle is split into tiles starting at the top left", with the last
        tile in a row short when the width is not a multiple of 16 -- and that
        is what the server encodes, not framebuffer-aligned tiles."""
        for ty in range(0, h, 16):
            th = min(16, h - ty)
            for tx in range(0, w, 16):
                tw = min(16, w - tx)
                sub = r.byte()
                bg = fg = None
                if sub & 0x02:                              # BackgroundSpecified
                    bg = r.take(4)
                if sub & 0x04:                              # ForegroundSpecified
                    fg = r.take(4)
                if sub & 0x01:                              # a Raw tile
                    for row in range(th):
                        o = ((y + ty + row) * self.width + x + tx) * 4
                        fb[o:o + tw * 4] = r.take(tw * 4)
                    continue

                for row in range(th):
                    o = ((y + ty + row) * self.width + x + tx) * 4
                    fb[o:o + tw * 4] = bg * tw

                if not sub & 0x08:                          # AnySubrects
                    continue
                coloured = sub & 0x10
                for _ in range(r.byte()):
                    colour = r.take(4) if coloured else fg
                    sxy, swh = r.byte(), r.byte()
                    sx, sy = sxy >> 4, sxy & 0x0F
                    sw = min((swh >> 4) + 1, tw - sx)
                    sh = min((swh & 0x0F) + 1, th - sy)
                    if sw <= 0:
                        continue
                    for row in range(sy, sy + sh):
                        o = ((y + ty + row) * self.width + x + tx + sx) * 4
                        fb[o:o + sw * 4] = colour * sw


# -- reading a framebuffer back ---------------------------------------

def pixel(fb, w, x, y):
    o = (y * w + x) * 4
    return fb[o + 2], fb[o + 1], fb[o]          # R, G, B


def count_differing(a, b):
    return sum(1 for i in range(0, len(a), 4) if a[i:i + 4] != b[i:i + 4])


def load_font(path="components/espix_display/font8x8.c"):
    """The 8x8 font, read out of the C table it lives in.

    `espix_font8x8[128][8]`, one byte a row, bit 0 the leftmost pixel -- which is
    the only thing about it that cannot be guessed and is worth the comment it
    has in the source.
    """
    import re

    glyphs = []
    with open(path) as f:
        for line in f:
            found = re.findall(r"0x([0-9A-Fa-f]{2})", line)
            if len(found) == 8:
                glyphs.append(tuple(int(v, 16) for v in found))
    if len(glyphs) < 128:
        raise RfbError("font table looks wrong: %d glyphs" % len(glyphs))
    return glyphs


def _cell_bits(fb, w, x, y, fg):
    rows = []
    for r in range(8):
        bits = 0
        for c in range(8):
            if pixel(fb, w, x + c, y + r) == fg:
                bits |= 1 << c
        rows.append(bits)
    return tuple(rows)


def read_screen(fb, w, cols, rows, origin, fg, font):
    """Decode a grid of cells back into text.

    This is what makes "the console printed the command" a test rather than a
    count of lit pixels that happened to go up: the screen is read the same way
    it was written, through the font, and the result is a string.
    """
    lookup = {bits: i for i, bits in enumerate(font)}
    lines = []
    for r in range(rows):
        line = ""
        for c in range(cols):
            bits = _cell_bits(fb, w, origin[0] + c * 8, origin[1] + r * 8, fg)
            if bits == (0,) * 8:
                line += " "
            else:
                line += chr(lookup.get(bits, ord("?")))
        lines.append(line.rstrip())
    return lines


def count_in(fb, w, r, colour, tol=0):
    """How many pixels in a rectangle are within `tol` of a colour, per channel.
    The way to ask "did the focused title bar get drawn here" without trusting
    an eye that is not there."""
    x0, y0, x1, y1 = r
    n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            p = pixel(fb, w, x, y)
            if all(abs(p[i] - colour[i]) <= tol for i in range(3)):
                n += 1
    return n
