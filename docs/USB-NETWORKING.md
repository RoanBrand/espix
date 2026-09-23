# Networking over USB

espix presents itself to a computer as a USB Ethernet adapter, so a board
plugged into a laptop is reachable over the cable with no WiFi credentials, no
access point and no router. The interface is `usb0`.

**What this is not** is the way to reach a device that has never been
configured — that is what the serial console is for, and flashing and then
`idf.py monitor` will always be the first contact with a new board. The console
gives you a shell.

What it adds is a *network*, without a network: `scp` and `sftp`, more than one
session at once, and every tool that speaks IP rather than a terminal. On a
bench that also makes it the dependable one, since WiFi is usually the least
reliable thing in the room.

## Which socket, and whether your board can do this at all

A development board normally has two USB sockets. One is a UART bridge and
carries the serial console; the other is the chip's own USB peripheral, and that
is the one this uses.

That is the ESP32-S3 devkit's layout and it is not universal, so it is worth
knowing the rule underneath it.

**An ESP32 has exactly one general-purpose USB controller.**
`SOC_USB_OTG_PERIPH_NUM` is 1 on the S3 and on the S31. Anything else on the
board labelled USB is one of two other things:

- **A UART bridge chip** (CP210x, CH34x and friends). It is a USB device in its
  own right and connects to the chip's serial pins. Nothing to do with the
  ESP32's own USB.
- **The USB-Serial-JTAG controller**, which is a separate peripheral and, in
  ESP-IDF's own words, "a fixed-function USB device that is implemented entirely
  in hardware, meaning that it cannot be reconfigured to perform any function
  other than a serial port and JTAG debugging functionality." It cannot carry
  USB-NCM, and no firmware change will make it.

**So the question for any board is: what does the OTG controller reach?** If it
reaches a socket you can plug into a computer, USB-NCM works. If the board
commits it to a **USB-A female** — a host socket, meant for plugging devices
*into* — then there is nowhere for a device-mode feature to go, and that is a
wiring decision no software can undo.

A worked example, inferred from connector layout rather than a schematic. An
S31 coreboard with three sockets — a USB-A female, a USB-C behind a programming
chip, and a USB-C wired to the chip — is very likely UART bridge on the second
and **USB-Serial-JTAG** on the third, because the S31's OTG controller is
high-speed only (`SOC_USB_FSLS_PHY_NUM` is 0). A connector advertised as USB 1.1
or full-speed therefore cannot be the OTG controller on that part; the speed
label is a reliable tell. Which leaves the OTG controller on the USB-A socket,
and USB-NCM with nowhere to run on that board.

The **P4** is the exception worth knowing: `SOC_USB_OTG_PERIPH_NUM` is 2 there,
with one full-speed and one high-speed PHY. It is the only espix target that
could be a USB host on one port and run USB-NCM on the other at the same time.

**On every other target the two roles are exclusive, and espix now builds one of
them.** `SOC_USB_OTG_PERIPH_NUM` is 1 on the S3 and the S31, and espix's default
is the **host** role: the same peripheral drives a USB stick rather than
presenting `usb0`, so a board flashed with the default image does not have `usb0`
at all. Switching back is the `ESPIX_USB_ROLE` choice in
`components/espix_net/Kconfig` — see [USB-HOST.md](USB-HOST.md), which also
covers the half that gets people: on a devkit whose two sockets sit close
together, a hub in the OTG socket can block the UART one, leaving SSH as the only
console.

**Plug in one at a time unless you know your board is happy with both.** They
are two independent 5V supplies and two independent grounds, and joining them is
an electrical question, not a software one:

- **Back-feeding.** Both sockets feed the board's 5V rail. Unless the board ORs
  them properly — a diode or a power-path controller per input — one supply ends
  up driving the other. Some boards do this correctly and say so in their
  schematic; plenty of cheap clones simply tie the rails together.
- **Ground loops.** Two cables means two ground paths. Into two different
  machines, or into one machine through separately-grounded hubs, that loop can
  carry real current through the board's ground plane.

Neither is certain to damage anything, and many people do run both for years.
The point is that it depends on your specific board and where the cables go, so
treat it as unsafe until the schematic tells you otherwise. Nothing here needs
both: espix is reachable over the USB link itself once it is up, and over WiFi
if that is configured, so the serial console is a convenience rather than a
requirement.

Some boards settle it for you — clones especially, where the two sockets are
close enough together that two cable housings will not physically fit.

With only the USB port plugged in, the board is powered through it, and once the
link is up you can log in over it and read everything the console would have
told you.

If nothing appears on the computer, the cable is usually in the wrong socket, or
is a charge-only cable with no data wires in it — which looks identical until
you try a different cable.

## The protocol

NCM (Network Control Model), one of three ways USB carries Ethernet:

| | |
|---|---|
| **NCM** | Modern, efficient — packs several Ethernet frames into one USB transfer. Linux, macOS and Windows 11 all bind it with in-box drivers. |
| ECM | Older and simpler, one frame per transfer. Never worked on Windows without a third-party driver file. |
| RNDIS | Microsoft's own. Works on Windows and Linux, not on macOS. |

NCM is the only one every current desktop takes without help, which is why it is
the one espix speaks.

## Two modes

Set with `usb mode`, stored in `/etc/usb.conf`, and reported by `usb status`.

### `server` — espix hands the computer an address (the default)

espix takes 192.168.7.1 and runs a DHCP server on `usb0`. Plug the cable in and
the computer gets an address on the same `/24` without being told anything.

```
$ usb status
mode:   server (espix hands the computer an address)
link:   attached
addr:   192.168.7.1/24
        ssh 192.168.7.1 from the computer
```

Then, from the computer:

```bash
ssh esp@192.168.7.1
```

That is the whole procedure. Nothing to configure on either end.

**The DHCP offer carries no router option**, and that is deliberate. espix is
not a router, and most systems treat a gateway in a DHCP offer as a candidate
default route — so a board advertising one could take over your laptop's routing
and cost it internet access the moment the cable went in. That failure looks
like the network breaking for no reason, which is a miserable thing to debug.
Your computer still reaches espix directly, because the two are on the same
link, and that is the entire job. Verified on the wire: `ipconfig getpacket` on
macOS, or `nmap --script broadcast-dhcp-discover` anywhere, shows no `router`.

**It does carry a DNS option naming espix, and espix cannot remove it.** ESP-IDF's
DHCP server emits one unconditionally: configure no DNS server and it advertises
its own address instead of omitting the option. So your computer is told to
resolve names at 192.168.7.1, where nothing is listening. In practice this is
mild — the machine keeps its own default route and its own resolver, and Linux
and macOS both scope a DNS server to the interface that supplied it — but it is
a pointer to a service that is not there. See
[UPSTREAM.md](UPSTREAM.md) for the exact code and what would have to change.

### `client` — espix asks the computer's network for an address

```bash
usb mode client
reboot
```

espix runs a DHCP client on `usb0` instead. Use this when the computer bridges
or shares its own network over the USB link, so the board joins the LAN that
computer is on and can reach the internet through it.

`usb mode` writes `/etc/usb.conf` and takes effect at the next boot. A DHCP
server and a DHCP client are different kinds of interface, fixed when it is
created, so switching means building it again.

## Setting up the computer for `client` mode

This part is not espix. Everything here configures the machine at the other end
of the cable, and it is included because `client` mode is useless without it.

**The goal, in one sentence:** the computer already has an uplink that gives it
a LAN address, a default gateway and DNS — put `usb0` on that same network, so
espix gets the same three things from the same place everything else on the LAN
does.

There are two ways, and which are available to you is decided by what that
uplink is, not by preference:

| | what it does | when you can use it |
|---|---|---|
| **Bridge** | `usb0` and the uplink become one segment. espix gets its address from the LAN's own DHCP server and other machines can reach it directly. | Uplink is **wired** |
| **Share** (NAT) | The computer runs DHCP and NAT on `usb0`. espix sits on a private network behind it: it can reach out, nothing reaches in unsolicited. | **Any** uplink, and the only option on WiFi |

**WiFi uplinks cannot be bridged**, and it is worth knowing why before you spend
an evening on it. An 802.11 frame from a station carries three addresses where
bridging needs four, so a station cannot forward traffic on behalf of another
MAC unless both ends do 4addr/WDS, which most access points and drivers do not.
`nmcli` and `brctl` will accept the configuration without complaint and pass no
traffic. If the computer is on WiFi, share instead.

### Linux — bridge to a wired uplink

Names below are the usual ones: `eth0` is the computer's wired uplink, `usb0` is
espix, `br0` is the bridge. Check yours with `ip link` first.

```bash
sudo ip link add br0 type bridge
sudo ip link set eth0 master br0
sudo ip link set usb0 master br0
sudo ip link set br0 up
```

That is all espix needs. A bridge forwards at layer 2, so espix's DHCP request
crosses it as a frame and your router answers it directly — the computer in the
middle is not involved, and `br0` does not need an address of its own for any of
it to work.

What the computer *does* need is an address for itself. Enslaving `eth0` to the
bridge means frames arriving on it go to `br0` instead of up `eth0`'s own IP
stack, so the address that used to work there does not any more:

```bash
sudo dhclient br0        # only if nothing else is going to do it
```

On most distributions nothing needs typing, because whatever manages networking
— `dhcpcd` or NetworkManager on a Raspberry Pi, depending on the release — sees
a new interface and configures it. That is why this step is easy to have never
run and not noticed.

Check both ports joined:

```
$ ip link show master br0
2: eth0: <BROADCAST,MULTICAST,UP,LOWER_UP> ... master br0 ...
3: usb0: <BROADCAST,MULTICAST,UP,LOWER_UP> ... master br0 ...
```

Because the computer's address moves off `eth0`, **running this over a remote
session to that machine will disconnect it** — there is a window between
enslaving `eth0` and `br0` being configured. Do it from a local console, or from
a script that gets to the end either way.

None of it survives a reboot. Making it permanent is however your distribution
configures networking, and is out of scope here.

### Linux — share, for any uplink

```bash
nmcli connection modify <usb-connection> ipv4.method shared
nmcli connection up <usb-connection>
```

NetworkManager then runs DHCP and NAT on that interface. This is the one to use
when the computer is on WiFi.

### macOS

System Settings → General → Sharing → Internet Sharing. Share from Wi-Fi, to the
USB Ethernet device that appeared when you plugged the board in, then switch it
on.

This is sharing, not bridging: macOS runs DHCP and NAT on that interface. There
is no supported way to bridge a USB Ethernet device into the built-in network.
If you need the board on the LAN proper, bridge from a Linux machine or a
single-board computer instead.

### Windows

Network Connections → right-click the adapter with the internet → Properties →
Sharing → "Allow other network users to connect through this computer's Internet
connection", and pick the USB Ethernet adapter. That is Internet Connection
Sharing, which is NAT.

Windows can also bridge, for a wired uplink: select both adapters, right-click,
"Bridge Connections".

### Confirming it worked, from espix

`ip addr show usb0` is the test: in `client` mode the address comes from the
LAN's DHCP server, so what you want to see is one in the computer's own range.
An address of 192.168.7.1 means espix is still in `server` mode and none of the
above is being used — check `usb status`, and remember `usb mode` only takes
effect after a reboot. See [Checking it](#checking-it) for the rest.

## Why bother — what the cable buys you

Beyond having a network without needing one:

- **No WiFi credentials on the device.** `/etc/wifi.conf` holds the PSK in
  plaintext — it is 0600 and espix explains why — but a board that gets its
  network over the cable never has one written at all. That is the difference
  between losing a board and losing your network's password, and it matters for
  anything handed to someone else or left where it might be picked up.
- **A device with WiFi unconfigured is a supported setup, not a broken one.**
  espix logs "no network configured", carries on booting, and everything works
  over `usb0`. You can simply never run `wifi connect`.
- **Latency that does not move.** No contention for airtime, no distance, no
  access point having an afternoon, and no power-save sleep schedule to wait
  on. The USB figure is the same on every run, which for an interactive shell
  is worth more than a higher peak.
- **Faster, on the parts where the USB is.** Careful here: the ESP32-S3's USB is
  **full-speed only**, 12 Mbit/s, so the measurement below shows USB ahead by
  about 12% and the bottleneck is the SSH transport rather than either link. The
  P4 and S31 have **high-speed** USB at 480 Mbit/s, where the headroom is real
  and the comparison would look quite different.
- **Less power.** The radio is the expensive part of an ESP32, and a device that
  never associates never pays for it — no scanning, no association, no beacons
  to wake for.
- **It stops competing with your own WiFi.** One more station associated is one
  more thing contending for airtime on that channel, retrying, and taking its
  turn. A board on a cable is off the air entirely, which matters most in the
  places where boards accumulate.
- **The radio is left free.** A station that is not holding an association is a
  radio you could spend on something else: ESP-NOW, scanning, or an access point
  for other devices. There is a real technical edge here rather than just spare
  capacity — ESP-NOW peers have to sit on the station's channel while it is
  associated, so a device whose uplink is the cable can choose its own channel
  instead of inheriting the one the access point happened to pick.

  **espix cannot use the radio that way yet.** Apps get lwIP sockets and the
  resolver and nothing else; there is no `esp_wifi_*` or `esp_now_*` in the ABI.
  The cable frees the radio, and giving apps a way to reach it is a roadmap item
  — see [ROADMAP.md](ROADMAP.md).

## Checking it

On espix:

```
usb status          mode, whether a host is attached, and the address
ip addr show usb0   the address, as any interface
ip link             usb0 is listed whenever the feature is built in
ip route            confirms wlan0 still holds the default route
dmesg               "usb0: host attached" as the cable goes in
```

All of that works over the USB link itself, or over WiFi — you do not need the
serial console for any of it, which matters given the caution about plugging in
both cables above.

`usb0` appears in `ip link` whether or not anything is plugged in, and reads
`UP` only once a computer has enumerated it. That mirrors what a physical
Ethernet port does with no cable, and it means "is it listed?" and "is it
connected?" stay separate questions.

With both interfaces up, `wlan0` keeps the default route: `usb0` has the same
route priority ESP-IDF gives Ethernet, which is below WiFi's. So plugging in a
cable adds a way to reach the board without redirecting what the board sends.

## Speed

Measured on an ESP32-S3, 5000 lines of app output over `ssh`, three runs each:

| | |
|---|---|
| over `usb0` | 12.8s, about 389 lines/s |
| over `wlan0` | 14.4s, about 347 lines/s |

So USB is a little quicker, and on this part that is not really the point. The
S3's USB is full-speed — 12 Mbit/s — so neither link is the bottleneck here; the
SSH transport at both ends is, which is why the gap is 12% and not an order of
magnitude. What USB gives you on an S3 is a number that does not move.

The P4 and S31 have high-speed USB at 480 Mbit/s. There the link stops being
comparable to WiFi at all, and this table would be worth measuring again.

## Building it out

The choice that decides whether any of this exists is `ESPIX_USB_ROLE` in
`components/espix_net/Kconfig`: **host** (the default) builds the host stack
instead, and **device** builds what is described here. Turning USB-NCM off in a
device-role build drops espix's driver and the whole TinyUSB stack from the
image — about 51KB of flash. The `usb` command is compiled out with it, so
`help` lists only what the image can do: host-role builds have
`lsusb`/`usbscan`, device-role builds have `usb`.
