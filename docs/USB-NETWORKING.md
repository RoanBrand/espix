# Networking over USB

espix presents itself to a computer as a USB Ethernet adapter, so a board
plugged into a laptop is reachable over the cable with no WiFi credentials, no
access point and no router. The interface is `usb0`.

This is the quickest way to reach a device that has never been configured, and
the most dependable way to reach one on a bench, where WiFi is usually the least
reliable thing in the room.

## Which socket

A development board normally has two USB sockets. One is a UART bridge and
carries the serial console; the other is the chip's own USB peripheral, and that
is the one this uses.

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

This part is not espix. Everything below configures the machine at the other end
of the cable, and it is included because `client` mode is useless without it —
not because espix has any opinion about how you do it.

Two shapes to choose between:

- **Bridging** puts the board directly on your existing network. It gets an
  address from the same DHCP server as everything else, and other machines on
  the LAN can reach it. This is usually what you want.
- **Sharing** (NAT) puts the board on a private network behind the computer. It
  can reach out; nothing can reach in without a port forward.

### Linux, bridging

The USB interface appears as `usb0`, or as `enx` followed by the device's MAC.
Check `ip link` after plugging in.

With NetworkManager, which is what most desktops run:

```bash
nmcli connection add type bridge con-name br0 ifname br0 stp no
nmcli connection add type bridge-slave ifname eth0 master br0
nmcli connection add type bridge-slave ifname usb0 master br0
nmcli connection up br0
```

Or, without NetworkManager, for one session:

```bash
sudo ip link add br0 type bridge
sudo ip link set eth0 master br0
sudo ip link set usb0 master br0
sudo ip link set br0 up
sudo dhclient br0
```

Note that bridging takes `eth0` into the bridge, so the machine's own address
moves to `br0`. Doing this over a remote session to that machine will disconnect
it.

### Linux, sharing instead

```bash
nmcli connection modify <usb-connection> ipv4.method shared
nmcli connection up <usb-connection>
```

NetworkManager then runs a DHCP server and NAT on that interface. espix in
`client` mode picks up an address from it.

### macOS

System Settings → General → Sharing → Internet Sharing. Share from Wi-Fi, to the
USB Ethernet device that appeared when you plugged the board in, then switch it
on. macOS runs DHCP and NAT on that interface, so this is the sharing shape
rather than the bridging one.

macOS has no supported bridging of a USB Ethernet device into the built-in
network. If you need the board on the LAN proper, use a Linux machine or a
single-board computer as the bridge.

### Windows

Network Connections → right-click the adapter with the internet → Properties →
Sharing → "Allow other network users to connect through this computer's Internet
connection", and pick the USB Ethernet adapter. This is Internet Connection
Sharing, which is again NAT rather than bridging.

Windows can also bridge: select both adapters, right-click, "Bridge
Connections".

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

So USB is a little quicker, and that is not really the point — the difference is
around 12%, where the link itself is full-speed USB and the bottleneck is the
SSH transport at both ends. What USB gives you is a number that does not move:
no contention, no distance, no access point having a bad afternoon.

## Building it out

`CONFIG_ESPIX_USB_NCM_ENABLED` is on by default and can be turned off, which
drops espix's driver and the whole TinyUSB stack from the image — about 51KB of
flash. The `usb` command still exists in such a build and says the feature was
not compiled in, rather than disappearing and leaving you comparing your device
against this page.
