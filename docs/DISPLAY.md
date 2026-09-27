# The display service, and why it comes before a screen

espix has no display, and the interesting hardware on this chip is all display
hardware: PPA (fill, blit, scale, rotate, mirror, blend, colour convert), the
JPEG codec (decode *and* encode), the 2D-DMA engine, and LCD_CAM for a real
panel. None of it can be judged without something to look at, and a physical
panel is a poor development loop anyway -- small, fixed, and on the wrong side
of the desk.

The way out is not to wait for a screen. It is to notice that a remote
framebuffer needs no display hardware at all: the "screen" is a buffer in
PSRAM, and a VNC client on a laptop is the monitor. That is a better loop than
a 4-inch panel, and it is what this component is.

## What VNC actually is

VNC is a family name, not a protocol. Every VNC client -- TigerVNC, RealVNC,
macOS Screen Sharing, noVNC in a browser -- speaks **RFB**, the Remote
Framebuffer protocol, standardised as [RFC 6143](https://www.rfc-editor.org/rfc/rfc6143.html).
That is the contract worth checking against, and it is a wire format rather
than a codebase, which is why the interoperability comes free.

### Why not port TigerVNC

The tempting move is to compile TigerVNC with the IDF toolchain and run it as
an espix app. It is the wrong layer. TigerVNC's server is `Xvnc`, which is not
a VNC server that happens to have a display -- it **is** an X server, an
Xorg-derived one, with an RFB backend attached. "Compiling it" means porting
X11 server internals, `pixman`, `libXfont2`, `freetype`/`fontconfig`, `fork`
and `exec`, shared memory, and a process model built for an MMU-based OS. The
size of the tarball is not the problem; the shape of the dependency graph is.
And having ported it you would have a grey screen and no window manager.

There is a legitimate version of that instinct: porting something real is how
you discover which POSIX surface espix is missing. But the same lesson comes
from porting **libvncserver** (small C, `rfbNewFramebuffer()` and callbacks),
which is two orders of magnitude less work and leaves a working server behind.
Neither is needed for the first milestone; both are a later, optional spike.

## Architecture

```
  input sources                    the desktop                backends
  -------------                    -----------                --------
  RFB connection  --+                                        +--> RFB (rfb.c)
                    +--> input queue --> desktop task --+    |
  USB HID (later) --+                  (cursor, text)   |    |
                                        |               |    |
                                        v               v    |
                                   canvas (RGB565, PSRAM) ----+
```

Three pieces, and the seams between them are the point:

- **The canvas** (`display.c`) is an RGB565 surface in PSRAM plus a damage
  list. RGB565 because that is what the hardware path wants -- PPA converts and
  scales it, the JPEG encoder eats it, an RGB panel matches it.
- **The input queue** is one FreeRTOS queue with many sources. The desktop
  consumes events and never learns whether a pointer moved because a VNC client
  said so or because a USB mouse did. This is the abstraction that delivers
  "keyboard and mouse support" from two directions at once, and it is why a
  remote mouse works today while a local one is still roadmapped.
- **The backend** (`rfb.c`) serves the canvas over TCP. It is a client of the
  canvas, not part of it, which is what lets a second backend (an RGB panel, an
  SPI display) be added without the desktop knowing.

## What the S31 gives us

From `soc_caps.h` for `esp32s31`:

| Capability | Symbol | Where it goes |
|---|---|---|
| PPA: SRM, BLEND, FILL | `SOC_PPA_SUPPORTED` | fills, blits, scaling, rotation, alpha, colour convert |
| JPEG decode + encode | `SOC_JPEG_CODEC_SUPPORTED` | the photographic encoding path |
| 2D-DMA | `SOC_DMA2D_SUPPORTED` | rectangle copies without the CPU |
| LCD_CAM: RGB, I80, camera | `SOC_LCDCAM_*_SUPPORTED` | a real panel later; the camera is a separate prize |
| CORDIC | `SOC_CORDIC_SUPPORTED` | sin/cos/atan2 for rotation, gradients, arcs |
| USB OTG host | `SOC_USB_OTG_SUPPORTED` | the real keyboard and mouse |

`miniz` is already in `esp_rom`, so zlib/deflate (and therefore the Tight
encoding) needs no new dependency when it is wanted.

## The encoding ladder

This is the part that decides whether the thing is usable, and it is where the
accelerators earn their keep.

Raw RFB sends uncompressed pixels: an 800x600 desktop at 32bpp is 1.9 MiB per
frame. Over WiFi that is a slideshow. So:

1. **Raw** (encoding 0) -- mandatory, implemented. The fallback, and the
   reference the others are measured against.
2. **Hextile** (encoding 5) -- implemented. A uniform 16x16 tile costs five
   bytes instead of 512, so a flat desktop first-paints in about 10 KiB. This
   is the *lossless* path, and it is the one that stays for text and UI edges.
3. **JPEG via Tight** (pseudo-encoding) -- not yet. RFB supports carrying JPEG
   inside the Tight encoding, which is exactly the standard path for
   photographic content, and this chip has a **hardware JPEG encoder**. It is
   lossy and blocky on sharp edges, which is why it complements Hextile rather
   than replacing it: a hybrid encoder picks per rectangle.
4. **PPA SRM** then does the crop, the scale to the client's size, and the
   RGB565 to RGB888/YUV conversion on the way in -- the CPU loop in
   `row_to_pf()` is precisely what it replaces. That loop already has two fast
   paths: RGB565 copies straight through when a client accepts the format
   `ServerInit` advertised, and 32bpp rgb888 (what every desktop client asks
   for) skips the division entirely. The 8bpp 3-3-2 that a phone picks still
   walks the general scale, three multiplies per pixel -- deliberately left
   alone until something shows it matters, because at 480k pixels a frame it is
   milliseconds and nothing like the audio decoder's memory wall.

## Milestones

**M0 -- the server, no acceleration.** TCP on 5900, RFB 3.7/3.8 handshake,
security type None, `SetPixelFormat` / `SetEncodings` / `FramebufferUpdateRequest`
/ `KeyEvent` / `PointerEvent` / `ClientCutText`, raw and Hextile encoders,
dirty-rectangle updates. This is what exists now.

**M1 -- a desktop worth looking at.** A solid background, a cursor drawn
server-side with save-under, and a window that echoes keystrokes so the
keyboard round-trip is visibly proven. Also in this milestone: a bitmap font
(embedded 8x8, no freetype).

**M2 -- the accelerators.** PPA FILL for clears, PPA SRM for blit/scale/convert,
DMA2D for moves, JPEG for encode. Each one benchmarked against the CPU path it
replaces, with the VNC client as the visual check. This is the milestone that
answers "what do we actually need from these peripherals".

**M3 -- a surface and compositor contract.** Surfaces, z-order, alpha via PPA
BLEND, a cursor layer, and a `DesktopSize` pseudo-encoding so the canvas can
change size. The window-manager API, in other words, kept deliberately thin.

**M4 -- the first real app: an image viewer.** It is the purest JPEG-accelerated
demo and it needs no text-input model, which makes it the right first
application -- before the file browser, the audio player, and the text editor.

The longer list (system tray with clock and radio status, file browser, image
and movie viewer, audio player, text editor) is a GUI stack, and each of those
is only worth starting once the layer under it is honest.

## Using it

```
espix> vnc start              # 800x600 desktop, listening on 5900
espix> vnc status             # port, clients, authentication, and the address to use
espix> vnc password espix     # require VNC authentication from the next client
espix> vnc nopassword         # back to no authentication
espix> vnc stop               # stops the listener and frees the canvas
```

There is a password out of the box, so there is nothing to set up: it is the
built-in default `espix`. That default is **public**, and deliberately so -- it
exists so that a client which insists on a password works with no ceremony, not
to keep anyone out. macOS Screen Sharing is that client; against security type
None it waits for a challenge that never comes until it gives up. Two
consequences, and only the first is obvious:

- The 3.3 handshake (Screen Sharing) is dictated VNC authentication, and
  `espix` connects it.
- The 3.7+ handshake offers **both** types, None first. So TigerVNC and RealVNC
  keep connecting without a password, and a viewer that wants to authenticate
  can take the second entry.

Which means, bluntly: **the password here is a compatibility mechanism, not a
security boundary.** A client that can choose will choose None, and the default
is printed on this page. Narrowing the list to type 2 alone is a one-line change
once there is something worth protecting.

`vnc password <pw>` replaces the default, and `vnc nopassword` turns
authentication off for the rest of the boot. Only the eight-byte DES key is
stored (`/etc/vnc.key`, 0600, read back at `vnc start`), never the password --
and it is a password equivalent, which is what RFB type 2 requires and what
every VNC server keeps on disk.

Then point any VNC client at `espix:5900` (or the address `vnc status` prints).
Nothing is allocated at boot: `vnc start` creates the canvas and the desktop
task, and `vnc stop` gives all of it back.

## Clients, and what they need

Two clients were tried first, and both taught something.

- **TigerVNC** negotiates 3.8, security None, and switches itself to 32bpp
  rgb888; it works. On a Retina Mac it renders the framebuffer at one device
  pixel per framebuffer pixel into a window sized in *points*, so an 800x600
  desktop lands in the bottom-left quarter of an 800x600-point window. That is
  a client-side scaling quirk -- macOS puts the view origin at the bottom left,
  which is why it is the *bottom* quarter, and the server has no say in it.
  The log line to check is "client asks for WxH": if that reads 800x600, the
  size the server advertised is the size the client believes.
- **macOS Screen Sharing** opens with RFB **3.3**, not 3.8 (it falls back to
  3.3 whenever the server advertises 3.7 or 3.8). That is supported -- 3.3
  differs only in the security handshake, where the server dictates the type as
  one u32 rather than offering a list -- but it is the fussiest client here by
  some way, and both of its quirks are easy to get wrong in opposite directions.
  It will not connect without a password: against security type None it waits
  for a challenge that never comes, then times out. And after a *successful*
  VNC-auth exchange it does want the SecurityResult, unlike the None case --
  omit that and it waits ten seconds in exactly the same place, which is how the
  two got confused here. In return it is Retina-aware, which makes it the better
  client on a Mac.

## Known limitations

- **One client at a time**, like `sshd` for now: memory and failure isolation
  are worth proving with a single session before multiplying them.
- **Authentication is weak and not enforced against a client that can
  choose.** It is RFB security type 2: a single DES challenge-response (which is
  why there is a hand-written DES in `vnc_des.c` -- mbedtls 4.1.1 no longer
  ships one). It proves the client knows the password and protects nothing
  after the handshake. Because security type None is advertised alongside it, a
  client that prefers None gets it, and the default password is public. Real
  privacy needs TLS, which is a separate piece of work. Do not expose port 5900
  to a network you do not control.
- **The VNC password is independent of the account password.** RFB type 2
  carries no username, and a DES challenge cannot be checked against a hash, so
  the server needs a *key* rather than `/etc/passwd`. It ships with a public
  default instead of deriving one; making the two the same would mean storing a
  second password-derived secret beside the account, which is a change to
  espix_auth's on-disk format rather than to this component.
- **The canvas is fixed at build time** (800x600 RGB565, 960 KiB). A
  `DesktopSize` pseudo-encoding makes it resizable; that is M3.
- **Colour-map clients are refused**, and the pixel format stays true colour.
  Every desktop client asks for true colour; this only affects a client that
  explicitly asks for a palette, and the log says so when it happens.
- **A cursor move is sent as damage**, so the client sees it as pixels rather
  than as a cursor layer. Correct, and it costs two small rectangles per move;
  the RFB cursor pseudo-encoding is the tidier answer later.
- **RFB is unencrypted and uncompressed at the transport.** It is for a LAN.
- **3.3, or 3.7 and up.** Both are served because the two real clients here
  each picked one; 3.4-3.6 is a version nobody shipped a viewer for.
