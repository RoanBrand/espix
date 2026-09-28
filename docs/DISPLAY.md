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
  input sources                  the screen                backends
  -------------                  ----------                --------
  RFB connection  --+
                    +--> espix_display_input() --> owner->input()
  USB HID         --+                                    |
                                                         v
                            canvas (RGB565, PSRAM) + damage list ---+--> RFB
                                                                     +--> panel
```

Three pieces, and the seams between them are the point:

- **The canvas** (`display.c`) is an RGB565 surface in PSRAM plus a damage
  list. RGB565 because that is what the hardware path wants -- PPA converts and
  scales it, the JPEG encoder eats it, an RGB panel matches it.
- **Input is a call, not a queue.** One entry point, `espix_display_input()`,
  serves every source and dispatches straight into the current owner's
  `input()` in the poster's context -- the RFB task for a viewer, the HID task
  for a local keyboard or mouse, which is what that second input source turned
  out to be: `espix_usb` decodes boot-protocol HID reports into the same X11
  keysyms a viewer sends, so the console and the desktop cannot tell them apart,
  and neither of them had to change. There is no input queue and no display task,
  and a queue bought
  nothing: it cost a copy per event, a 4 KiB task, and a priority inversion to
  make one cursor move look synchronous. The pointer position belongs to the
  service, because two sources feed it -- a viewer says *where* it is, a local
  mouse says *how far* it moved.
- **The screen has exactly one owner.** `espix_display_claim()` / `release()` /
  `owner()` name whose model is rendered, not who owns the canvas: each side
  keeps its own state and a switch is a repaint, which is what lets the console
  keep running invisibly behind a desktop. A new owner takes over rather than
  being refused, because the console is where you would start a desktop from --
  the one place refusing could not work.
- **The backend** (`rfb.c`) serves the canvas over TCP. It is a client of the
  canvas, not part of it, which is what lets a second backend (an RGB panel, an
  SPI display) be added without either owner knowing.
- **What drives an update, and why a local mouse used to feel slow.** A viewer
  keeps one `FramebufferUpdateRequest` outstanding, and the server holds it
  rather than answering with an empty update -- so the only thing that decides
  how long a change made by *another task* waits is the loop's receive timeout.
  It is 10 ms while an update is owed and 250 ms while none is, because there is
  nothing to send until the client asks. That is the entire difference between a
  local pointer and a remote one: a remote move is sent the instant the client's
  own message is handled, while a local one has nothing to wake the loop but that
  timeout. At 250 ms it was measurable -- 5-12 updates a second, against 40-108
  after.

## The on-screen console

`components/espix_shell/canvas_console.c` is the third transport for the same
shell, after the UART console and the SSH channel: the same command registry,
the same history, the same line editor, drawn into the canvas instead of a
terminal. It is a screen owner like any other program -- it claims the screen
when a viewer attaches and nothing else owns it, which is why a board with a
VNC client and no desktop still gives you a shell. It runs as `esp`, not root:
the serial console is root because holding the board has already won, and a
viewer over the network has not. A headless board allocates none of it.

The output side is a terminal, because the shell's output assumes one: CSI
sequences that move the cursor or erase are acted on, unknown ones are parsed
and dropped, and UTF-8 is decoded one cell per code point -- box drawing is
three bytes per glyph, so counting bytes made the greeting wrap. Control
characters are synthesised from modifiers, because an RFB `KeyEvent` carries a
keysym and no modifier field: Ctrl-C arrives as Control, then `c`.

**The console answers the queries it is asked.** This is not optional: an RFB
viewer is a framebuffer, so on this side espix *is* the terminal, and nothing
else will answer. `esp_linenoise` finds the terminal by sending `ESC[5n` and
reading the reply, so with nobody answering it waits for ever inside
`create_instance()` -- before a frame is ever sent, which is what a black screen
was. It wants exactly `ESC[0n` within 500 ms; without it, it concludes the
terminal is dumb and turns line editing and history off. `ESC[6n` (cursor
position) and `ESC[c` (device attributes) are answered for the same reason:
every query answered is one fewer way to hang.

Three preconditions when starting the editor here, each of which cost a bug:

- **The key queue must exist before `esp_linenoise_create_instance()`.** The
  instance sends its query at creation and reads the reply back through the
  transport's read callback, which pulls from that queue -- so creating the
  editor first was an `xQueueReceive(NULL)` assert on the RFB task the moment a
  viewer connected, which is where the console is started from.
- **Never call `esp_linenoise_probe()`.** It `fcntl()`s the descriptor, and the
  descriptor here is a key rather than a terminal: both callbacks are supplied
  and nothing reads or writes it.
- **ICRNL is the transport's job.** A terminal sends CR for Enter and
  `esp_linenoise` tests for LF, so without it Enter does nothing at all. The
  UART gets this from IDF's VFS and SSH does it in its own read callback; espix
  *is* the pty here, so it is the console's job too. And `Ctrl-C` is the
  editor's `EAGAIN` -- abandon the line -- not end of input; treating the two
  alike drops the console on Ctrl-C, which no other shell does.

## What the S31 gives us

From `soc_caps.h` for `esp32s31`:

| Capability | Symbol | Where it goes |
|---|---|---|
| PPA: SRM, BLEND, FILL | `SOC_PPA_SUPPORTED` | fills, blits, scaling, rotation, alpha, colour convert |
| JPEG decode + encode | `SOC_JPEG_CODEC_SUPPORTED` | decoding a picture into a surface; the encoder is not wired up |
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

## What it costs

`display bench` times the two operations an accelerator replaces -- a rectangle
fill and a rectangle blit -- at five sizes, two below the accelerator's
break-even and three above it. Megapixels per second, not
megabytes: bytes per pixel is a convention and pixels are not.

Every number below was measured on the board named in its column, in the same
run as the others in that row. A number from another chip is not a comparison,
which is why the harness measures both paths on one board rather than quoting
two.

| | 32x32 | 64x64 | 128x128 | 256x256 | 800x600 |
|---|---|---|---|---|---|
| **S31 fill, software** | 26.4 | 27.6 | 27.8 | 21.9 | 22.0 |
| **S31 fill, PPA FILL** | 15.1 | 43.1 | 76.9 | 97.0 | **105.1** |
| **S31 blit, software** | 13.2 | 13.5 | 13.6 | 10.7 | 10.6 |
| **S31 blit, PPA SRM** | 7.2 | 18.9 | 36.2 | 47.2 | **49.1** |
| **S3, every row above** | — | — | — | — | — |

Mpx/s, higher is better. A dash is a row that has not been measured yet rather
than one that is slow.

Three more rows are not primitives at all, but the work the desktop actually
does, so they are quoted in milliseconds rather than in rates:

| | size | software | accelerated | |
|---|---|---|---|---|
| **repaint** -- background, then both windows | 800x600 | 31.6 ms | **7.1 ms** | 4.4x |
| **window content** -- frame, title, 20x56 of text | 456x186 | 11.2 ms | **8.3 ms** | 1.3x |
| **jpeg decode** -- the viewer's 23 KB test.jpg | 480x330 | 116.7 ms | **12.4 ms** | 9.4x |

And one number that is not a row, because it is not a primitive: **a window
drag**. Moving a window is the desktop's most obvious interaction and was its
worst, at **301,380 bytes per four-pixel motion** -- the union of where the window
was and where it went, with the window's own text in it, as Hextile.

It is **8,048** now, which is 37x, and the reason is a protocol feature rather
than a faster loop: RFC 6143's **CopyRect**. A dragged window is not new pixels,
it is the same pixels somewhere else, and a client that already has them can be
told to move them -- sixteen bytes against a hundred kilobytes. The desktop says
which rectangle moved and where from; the backend sends that as a copy and
subtracts it from the damage so the pixels are not sent twice.

Which is worth a note on how the number was found, because the first measurement
said something else. The test client asked for a *full* frame after every motion,
so every motion was answered with the whole canvas and cost what a whole canvas
costs -- 198 KiB, which looked like a plausible bad number and was really a
measurement of the harness. A viewer keeps one incremental request outstanding
and paints into a framebuffer it already has; the harness does that now, and the
real figure is the 301,380 above. Measured on the S31, and checked rather than
assumed: after a drag, a framebuffer built entirely from incremental updates and
copies agrees with a raw full frame to the pixel, 0 of 480,000.

The JPEG row is the one row that needs an input rather than a size: decoding is
not an operation to sweep, it is the whole of one file, so `display bench [jpeg]`
takes a path and defaults to the picture the launcher opens. It is absent when
there is no file to read, and absent on a target with no codec -- where it would
be the software number twice.

**Its "accelerated" column is the codec, and it is verified the same way,
against the software path rather than against the last run's numbers.** The two
decoders do not produce identical pixels and should not be expected to: they
differ in IDCT rounding and in how chroma is upsampled, and on a photograph that
is a few steps on a small minority of pixels. So the check is a count -- the
percentage of pixels whose channels are each within 16 of 255 of the software
path's -- and it reads **98%**. A wrong byte order, a wrong row pitch, or the
visible width taken from the MCU-padded one drops it to near zero, which is what
the threshold is for.

`repaint` is what a window move costs, because occlusion means what was
underneath is no longer known. `window content` is what one window costs on top
of that, and it is `window_paint()`: the frame, the title, the outline, and a
grid of text.

**The second row is the one that explained a slow drag, and it is not about the
accelerator.** Only the fills have an accelerated path: the glyphs are drawn
pixel by pixel by the CPU and nothing here touches them. So the accelerator
removes 2.9 ms of 11.2 and cannot reach the other 8.3, which is 20 rows of 56
8x8 glyphs -- 71,680 pixels written one branch at a time.

**That row is now mostly a bound rather than a routine cost, because the answer
was to draw fewer glyphs.** A window whose owner says which region changed is
redrawn in that region and no other, the terminal turns that into the two cells a
keystroke can touch, and a *structural* repaint -- a move, a raise -- no longer
repaints any surface at all: they already hold their content. So the two rows
above describe the work that used to be inside a drag motion and is not any more.

| | before | after |
|---|---|---|
| a drag motion | repaint + both window rows, ~21 ms | the repaint row, **7.1 ms** |
| a keystroke | one window row, ~11.2 ms | two cells, ~0.1 ms |

Those two "after" figures are arithmetic on measured rows rather than measurements
of the drag itself, and the difference matters. **The drag cannot be timed from a
VNC client**: 108 ms a motion either side of the change, because the number is the
full-frame send over WiFi and not the repaint. A client-side timing here measures
the network, so the only honest claims about a drag are the ones the benchmark
rows support.

**The crossover is the interesting part, and it is why the sizes run below it.**
PPA costs a fixed ~57 µs per transaction -- descriptor setup, the DMA start, and
waiting for completion -- where the software loop costs ~36 ns per pixel and
nothing else. So at 32x32 the accelerator is *slower* (0.55x), at 64x64 it is
1.5x, and by 800x600 it is 4.8x. `PPA_MIN_PIXELS` is 2048, between the two
measured points, and it matters because that range is where most of what a
desktop draws lives: a cursor, a character cell, a small icon. The accelerator
is not a blanket win and the threshold is not a guess.

**The software numbers are flat, which is the other half of it.** 26 to 28 Mpx/s
across a 470-fold range in size, because a loop has no fixed cost to amortise.
The accelerated column climbs with size for exactly the same reason, and the two
curves are what make the threshold a number rather than a preference.

**Every accelerated number is verified against the pixels before it is timed.** A
fast wrong answer is worse than a slow right one, and the failure modes here are
quiet: a cache that was not written back, an offset off by one, a colour mode
that is nearly right. It earned its keep immediately -- the first PPA run
reported `FAILED verification`, and the mismatch said why: `fill_color_val` is
documented as "a raw 32-bit value, the interpretation depends on fill_cm", which
reads as though RGB565 means an RGB565 word. It does not. The hardware takes
0x00RRGGBB and converts, so 0xABCD landed as 0x0559 -- R=0, G=0xAB, B=0xCD in
RGB565 is exactly that. Without the check the table above would have been four
times faster and wrong.

**The S3 has no accelerated row, and that is the design rather than a gap.** It
has no PPA, no 2D-DMA and no JPEG codec at all -- `SOC_PPA_SUPPORTED`,
`SOC_DMA2D_SUPPORTED` and `SOC_JPEG_CODEC_SUPPORTED` are simply absent from its
`soc_caps.h`, where the S31 and the P4 have all three. So on the S3 the software
path is not a fallback that nobody exercises: it is the implementation, and
every target keeps it for exactly that reason.

**The repaint is the row that decides something: 31.5 ms to 7.3 ms, 4.3x.** A
full repaint is what a window move costs, because occlusion means what was
underneath is no longer known. At 31.5 ms it is 32 updates a second at best, so
a drag is a slideshow; at 7.3 ms it is 137, so a drag is a drag. That is the
number the accelerators were wanted for, and it is the desktop's own shape
rather than a primitive's.

**The striding matters more than the size, and this row got it wrong first.**
Blitting a *contiguous* window surface *into* a strided canvas -- which is what
a compositor does -- is a different operation from copying a block *out of* a
wider buffer, where the source is strided. The first version of this row
measured the second, and reported 2.1x where the truth is 4.3x. Both are
checked now, because a block offset that is off by one and a row pitch that is
wrong look identical from the outside.

**The software loop is memory-bound, which is why the accelerator can win at
all.** A blit runs at about half a fill's rate -- 10.6 against 21.7 Mpx/s at
800x600 -- exactly what a copy that reads *and* writes should do against one that
only writes. There is no arithmetic left to remove; what PPA and 2D-DMA bring is
a better route to the memory, not fewer instructions, and the 4.8x says the route
was the problem.

**And a full-screen software fill is 20 ms**, which is the number that decides
whether repainting everything when a window moves is acceptable: at 50 Hz it is
the whole frame budget, so it is fine for a click and hopeless for a drag. PPA
makes it 4.5 ms, which is a drag. See [Milestones](#milestones).

## Milestones

**M0 -- the server, no acceleration.** *Done.* TCP on 5900, RFB 3.3 and
3.7/3.8 -- both, because the two real clients here each pick one -- security
type None and VNC authentication, `SetPixelFormat` / `SetEncodings` /
`FramebufferUpdateRequest` / `KeyEvent` / `PointerEvent` / `ClientCutText`,
raw and Hextile encoders, dirty-rectangle updates.

**M1 -- a desktop worth looking at.** *Done.* A solid background, a cursor
drawn server-side with save-under, a placeholder desktop, and an embedded 8x8
bitmap font (no freetype). One thing sits beside this milestone rather than in
it, because it was not obvious it came first: the **on-screen console** above.
It is what makes the screen useful before there is any GUI at all, and it is
the reason a VNC client with no desktop running still gives you a shell.

**M2 -- the accelerators.** PPA FILL for clears, PPA SRM for blit/scale/convert,
DMA2D for moves, JPEG for encode. Each one benchmarked against the CPU path it
replaces, with the VNC client as the visual check. This is the milestone that
answers "what do we actually need from these peripherals".

**M3 -- a surface and compositor contract.** *Done, except the last two.*
Surfaces, z-order, focus, a cursor layer and a window-manager API kept
deliberately thin. Alpha via PPA BLEND and a `DesktopSize` pseudo-encoding are
still unwritten, for the usual reason: nothing has needed them yet.

**M4 -- the first real app: an image viewer.** *Done.* It is the purest
JPEG-accelerated demo and it needs no text-input model, which makes it the right
first application -- before the file browser, the audio player, and the text
editor.

**M5 -- the shell around the apps.** *Started.* A taskbar along the bottom: a
launcher that opens a menu, a button per window in the order the windows were
opened, and a clock that reads the system clock the kernel already keeps. It is
deliberately a look rather than a feature -- the tray reports the time and
nothing else, and the menu has three items -- because it is the frame the rest of
those apps will arrive in, and a frame is worth seeing before it is worth
filling.

What is not a look is the terminal window: it runs a real session, and it is the
same `espix_term` the on-screen console runs rather than a second implementation
of one. The terminal was lifted out of the console for it -- grid, CSI parser,
UTF-8, editor, session -- and both are now a target and a task around a shared
middle. The seam is three functions (a cell, a row, a blank screen), and the two
things that want a terminal are drawn in completely different places: one into
the canvas as the screen owner, the other into a window's surface as a client of
the desktop.

The longer list (radio status and volume in the tray, a file browser, image and
movie viewer, audio player, text editor) is a GUI stack, and each of those is
only worth starting once the layer under it is honest.

## Using it

```
espix> vnc start              # 800x600 canvas + desktop, listening on 5900
espix> vnc status             # port, clients, authentication, and the address to use
espix> vnc password espix     # require VNC authentication from the next client
espix> vnc nopassword         # back to no authentication
espix> vnc stop               # stops the listener and frees the canvas

espix> display start          # the canvas alone: what a panel or local input uses
espix> desktop start          # the placeholder desktop, so there is something to draw on
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

### For a network you do not control

Tunnel it rather than adding a security type:

    ssh -N -L 5900:127.0.0.1:5900 esp@192.168.110.254

then point the VNC client at `localhost:5900`. espix already runs an SSH server
with a real key exchange and a host key, so this needs no certificate to manage
and covers *every* client -- including macOS Screen Sharing, which has no
VeNCrypt at all -- because from the client's side it is connecting to localhost.

The `-N` matters. espix's sshd carries one channel per connection, so a forward
and a shell cannot share one; `ssh -L` without it is refused, with that as the
stated reason rather than a hang. See the SSH section of [ROADMAP](docs/ROADMAP.md)
for what closing that gap would take.

Then point any VNC client at `espix:5900` (or the address `vnc status` prints).
Nothing is allocated at boot: `vnc start` (or `display start`) creates the
canvas, and stopping gives all of it back. A client attaching when nothing owns
the screen gets the console; `desktop start` is how you ask for the placeholder
instead.

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

- **Mouse buttons go nowhere.** A press is decoded and carried on a POINTER
  event, and an owner may read it, but nothing acts on one: there is no window
  manager to click at yet, so a click reaches the desktop and stops there. The
  position is still updated, which is the only reason it is sent at all.
- **The console is monochrome.** SGR sequences are parsed and dropped: the 8x8
  font is one bit per pixel and the grid holds one byte per cell, so colour
  wants an attribute per cell and a renderer that reads it. Until then a
  program that colours its output is perfectly readable, just not coloured.
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
