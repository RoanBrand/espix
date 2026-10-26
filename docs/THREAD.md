# espix Thread: OpenThread on the S31's own radio

**Status: built and running on the S31.** Thread is IEEE 802.15.4 at 2.4 GHz, and
the S31 carries both the radio and the application core, so the stack runs here
in-process rather than across a UART to a radio co-processor.

The code lives in `components/espix_net/thread/`, is presented as `thread0`
beside `wlan0` and `eth0`, and is driven by the `thread` command.

## 1. One SoC, not an RCP

IDF ships two OpenThread examples and both assume two chips: `ot_cli` talks to
an RCP over Spinel, and `ot_br` expects a host with a co-processor beside it. On
the S31 there is no second chip, so the stack is configured for the radio it has:

    radio_mode      = RADIO_MODE_NATIVE
    host_connection = HOST_CONNECTION_MODE_NONE

There is no RCP, no Spinel stream, no `/dev/esp_rcp0` and no daemon beside it.
The radio driver, the FTD stack, the netif glue, the border router and the CLI are
all IDF's; espix adds the name, the boot policy and the commands.

One consequence worth stating plainly: a Thread network here is one OpenThread
instance inside the firmware, not a host stack driving a co-processor. The S3 has
no 802.15.4 radio at all, so `ESPIX_THREAD` is S31-only.

## 2. thread0

`esp_openthread_start()` spawns the OpenThread task, initialises the stack,
attaches IDF's netif glue and its CLI, then returns; the main loop runs on the OT
task. espix registers the netif under the name `thread0` with kind
`ESPIX_IF_THREAD`, so `ip`, `ifconfig`, `thread status` and the ABI all see
one interface with one name.

`thread stop` takes `thread0` out of the interface table *before* the stack goes
down, because the OpenThread task destroys that netif on its way out: an `ip` or
`ifconfig` reading the old entry afterwards would read freed memory. The slot is
kept and reused when Thread starts again, so a start/stop cycle neither leaks
table entries nor moves anyone else's.

```
$ thread status
thread0: UP  role leader
  ipv6        enabled
  rloc16      0xe800
  border      on
  inet6      fd9c:a3f8:cd42:1:b667:794e:85ad:6529
             fd00:db8:a0:0:0:ff:fe00:fc10
             ...
```

`ip addr thread0` renders the same addresses in the same shape as any other
interface, and `ifconfig thread0` in the net-tools shape.

## 3. The commands

| command | what it does |
| --- | --- |
| `thread status` | role, rloc16, border state, IPv6 addresses |
| `thread start` | bring the stack up on the native radio |
| `thread stop` | take it down |
| `thread attach` | join the network in the stored dataset |
| `thread br on\|off` | border routing, if built in |
| `thread conf [key value]` | show or set the boot policy in /etc/thread.conf |
| `thread dataset` | the active dataset as hex TLVs |
| `thread join <hex>\|-f <file>` | set that dataset and join |
| `thread cli <line...>` | OpenThread's own CLI; the reply comes back here |

None of these go through esp_console: their output is written to the session that
asked, so an SSH session sees what the console would.

Everything but `thread status`, and `thread conf` with no arguments, is root's.
The verbs move the radio and the network, `thread cli` reaches all of OpenThread
(`factoryreset` included), and `thread dataset` prints the network key.

## 4. /etc/thread.conf: policy, not protocol state

The file holds four keys, all espix policy that OpenThread has no concept of:

    enabled=no          start the stack at boot?
    border_router=no    route the mesh to the backbone through NAT64?
    backbone=eth0       which interface the border router routes to
    attach=no           join the stored dataset at boot?

The dataset -- channel, PAN ID, network key, mesh prefix, PSKc, security policy --
is OpenThread's and stays in OpenThread's own NVS. None of it is duplicated in
/etc, and `thread dataset` reads it out rather than storing it.

The file is created by the kernel on the first boot that finds it missing:
`ensure_conf()` writes it, mode 0644, with the keys commented and at their
defaults. It runs in the boot task, which has no session or process and so is
espix itself to the permission check -- no `espix_fs_priv_begin()`, which stays
espix_fs's and espix_auth's alone. A zero-byte
file counts as missing, because a failed redirect leaves one behind. It is not
shipped in the repo -- this tree's `fsroot` is the dev overlay -- so it is born on
the board.

Writing it is root's, so `thread conf` runs as the caller and needs sudo, exactly
as `usb mode` and `wifi connect` do:

```
$ sudo thread conf enabled yes        # persist, and start now
$ sudo thread conf attach yes         # persist, and join now
$ sudo thread conf border_router yes  # persist, and route now
```

The two that mean something to a live stack move it as well; `backbone` takes
effect at the next start, because the border router reads it while OpenThread is
being initialised. Turning on `border_router` before the interface is up is
refused with a line telling you to attach first, rather than asserting.

## 5. Where the instance lives

`ot::Instance` is the largest single internal allocation espix makes: 143,856
bytes of `.bss` on the S31, and 149,088 with border routing built in. espix has
PSRAM and no shortage of it, so `tools/patch-openthread.py` (run from the
top-level CMakeLists when `CONFIG_OPENTHREAD_ENABLED`) marks `gInstanceRaw`
`EXT_RAM_BSS_ATTR`, which lands it in `.ext_ram.bss` under
`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`.

Measured on the S31 with border routing:

| | before | after |
| --- | --- | --- |
| openthread internal `.bss` | ~146 KB | 2,044 B |
| openthread external RAM | 0 | 149,088 B |
| internal RAM to start the stack | would not fit | ~10 KB |

PSRAM sits behind the flash cache, so the instance is unreachable while the cache
is disabled for a flash write -- but so is every other PSRAM byte, and every
littlefs write already does this. IDF suspends the other core's tasks for the
duration, so the OpenThread task cannot run then; what could is an interrupt, and
the radio's IRAM callbacks (`esp_openthread_radio.c`) touch only the port's own
statics in internal RAM, never the instance. So there is nothing to avoid, OTA
included; the thing to re-check is that callback list if IDF's radio port changes.

## 6. Building it, and the kernel slot

Thread is its own build, a profile beside the normal one:

    make TARGET=esp32s31 PROFILE=thread build
    make TARGET=esp32s31 PROFILE=thread flash-ota

`profiles/thread.conf` turns on `ESPIX_THREAD` and the border router;
everything else follows from Kconfig.
Its own `sdkconfig.esp32s31-thread` and `build-esp32s31-thread`, so switching
between it and the normal build is not a rebuild.

The Thread-plus-border-router image is about 3.92 MB, so `ota_1` is 4.5 MB and
the loader's `ota_0` is 256 KB. The S31 and S3 tables are the same shape, differ
only in the storage partition, and every app offset is 64 KB aligned. Adopting the
layout moves `storage`, so it is a full reflash once; after that a kernel can be
written to `ota_1` directly or pushed as a file.

## 7. The lock: one rule

Any task that is not the OpenThread task must hold `esp_openthread_lock_acquire()`
while calling into the stack. That is not just the stack mutex: `acquire()` also
takes the task-switching mutex, and a task that reaches OT without it asserts in
`esp_openthread_task_switching_lock_release()`. This was measured, as a panic in
the `thread` task, by calling `esp_openthread_border_router_init()` without the
lock. Border routing, attach and dataset reads all take it now.

espix has a second lock of its own around start, stop, attach, join and border
routing, so two sessions cannot change the stack's state at once. `thread status`
waits for it at most half a second: `ip` and `ifconfig` ask for status while
holding the interface table, and `stop` holds this lock while it takes the table
to unregister `thread0`, so an unbounded wait there could deadlock.

## 8. The CLI answers the session that asked

`thread cli` uses `esp_openthread_cli_input()`, so the line runs on the
OpenThread task. The CLI's output callback, which IDF fixes when the stack is
created, is replaced at start with one that captures into a buffer and wakes the
task waiting on the prompt `ProcessLine()` prints last; the prompt itself is not
forwarded. Running `otCliInputLine()` in the calling task under the lock would
capture more directly but deadlock on any command that waits for the stack, since
the OT mainloop takes the same lock.

A command that never answers gives up after ten seconds: what it had printed is
still delivered, and its later output goes to the console, which is where
unsolicited output belongs. Until its prompt does arrive the next `thread cli` is
refused ("an earlier line is still running"), so the late remainder of one reply
can never be read as the start of another; after thirty seconds without a prompt
the CLI is assumed never to print one and is free again.

## 9. IPv6, and reaching IPv4 through NAT64

`thread0`'s addresses come from the stack, not from the glue's lwIP netif, and
are rendered by the same `ip`/`ifconfig` path as every other interface. The
border router advertises a NAT64 prefix (the ULA `fd9c::/48` by default) and
Thread clients reach IPv4 through it.

Thread clients do their own DNS64: they send queries to the border router,
which forwards them to the backbone's resolver, and synthesize an IPv4-only
name's address in the NAT64 prefix it publishes. The border router itself has
IPv4 and the backbone's resolver, and needs none of it.

IDF's DNS64 client (`CONFIG_OPENTHREAD_DNS64_CLIENT`) is the other half: a
device whose own lookups go out over `thread0`. With `thread0` the default
route it resolves the A record and synthesizes the address, and it points
`thread0`'s resolver at `OPENTHREAD_DNS_SERVER_ADDR` (8.8.8.8 by default) encoded
in the prefix. So `ESPIX_THREAD` builds it in only without the border router --
an espix Thread device whose way out is the mesh -- and a border router leaves it
out. It is compiled in or not; IDF has no runtime switch, and its cost is
static.

`ESPIX_THREAD` also selects `ESP_NETIF_SET_DNS_PER_DEFAULT_NETIF`: each interface
keeps its own DNS servers and lwIP's resolver follows the default route, as a
Linux resolver follows the uplink. Without it lwIP has one global list, and the
DNS64 client overwrote it -- measured, on a border router that still had it:
after a `thread stop`, `host google.com` failed until reboot.

The prefix comes from the border router itself. OpenThread generates a local
NAT64 prefix from the BR ULA (`fd9c:a3f8:cd42:2::/96` on this board) and
publishes it as a route in the Thread network data -- but not on its own. The
prefix manager starts disabled, and with it disabled `Evaluate()` returns
immediately, so no route is published and clients have no prefix to synthesize
addresses in. espix turns it on
with `otNat64SetEnabled(instance, true)` as border routing is initialised, and
off again as it is torn down. Measured before and after: `nat64 state` goes
from Disabled to Active, `netdata show` gains
`fd9c:a3f8:cd42:2:0:0::/96 sn low`, and `thread status` shows it as
`nat64 active`.

The Router Solicitation still matters for the other case: when the network does
have an IPv6 router, the border router learns a NAT64 prefix from it, and to ask
it has to send an RS on the backbone -- which needs an IPv6 link-local on that
interface. espix creates one on every esp_netif interface as its link comes up
(`espix_net_ip6_linklocal()`, on `ETHERNET_EVENT_CONNECTED` and
`WIFI_EVENT_STA_CONNECTED`, with the GOT_IP path as a fallback), as Linux does --
not on DHCP, so an IPv6-only link, or one whose DHCP fails, still gets its
`fe80::`.

IPv6 ping is lwIP's, the same `esp_ping` the IPv4 one uses: `ping -6`, `ping6`,
or `ping` given an IPv6 literal. lwIP's `ip6_input()` hands every packet to its
raw sockets before `icmp6_input()` sees it, so an ICMPv6 socket gets its echo
replies. It reaches what lwIP routes to -- `thread0` included, since the netif
glue is an lwIP netif -- and a link-local destination needs its interface,
`fe80::1%eth0` or `-I eth0`, which binds the socket to it. `ip -6 route` shows each
interface's on-link prefixes and `ip -br` the brief, one-line-per-interface view
-- all rendered from the same `espix_ifinfo_t` as `ip addr`, so they cannot drift.

Reaching IPv4 *through* NAT64 is a Thread client's path, not the border router's:
it is the mesh side that sends to the NAT64 prefix. `thread cli ping <ipv4>`
synthesizes the address and sends from the border router itself, and gets no
reply. IDF's translator is an lwIP netif inside the prebuilt library rather than
OpenThread's own (`thread cli nat64 counters` is not built), and it translates
what arrives from the mesh; whether it should also take the border router's own
traffic needs a second Thread device to answer.

The backbone side is measured. The border router advertises its on-link prefix
(`fdde:ad00:beef:cafe::/64` here) and a route to the mesh on `eth0`; a Mac on
the same LAN took an address in the first, installed
`fd9c:a3f8:cd42:1::/64 via fe80::1c29:4ff:fed0:762b%en0`, and pinged the border
router's mesh address across it.

## 10. The prebuilt library's struct netif

IDF ships the border router as `libopenthread_br.a`, built by Espressif from
sources it does not publish (`OPENTHREAD_ESP_BR_LIB_FROM_INTERNAL_SRC` only works
inside Espressif). It is C compiled against lwIP's `struct netif`, so it reads
and writes that struct at the offsets of the lwIP configuration *it* was built
with -- and lwIP's options decide the layout. The library carries its own record
of those offsets, as `esp_openthread_check_netif_*_offset()`, and never indexes
`client_data[]` itself: only the array's length matters to it, and it expects
four entries.

espix had six. lwIP reserves DHCP, ACD, IGMP and MLD6; esp_netif allocates one,
which it must -- `wg0` is a plain lwIP netif whose `state` is WireGuard's, and
without its own slot esp_netif reads every netif's `state` as its handle (`vpn
up` faulted in `esp_netif_internal_dhcpc_cb()` the one time this was tried); and
lwIP's bridge allocates one. Everything after the array sat 8 bytes later than
the library expects. Measured: the library read the backbone's interface index
from what was, here, a byte of the MAC, so IDF's
`otPlatGetInfraIfLinkLayerAddress()` failed its index check and logged "Failed to
get LL address, error: Invalid If index" every few seconds, and OpenThread sent
its RAs, RSs and NSs without a link-layer address option. From the disassembly:
the library sets up its own NAT64 netif's name, address length and MTU on what
were the MAC, `mtu6` and a client-data slot, and lwIP, working on a 568-byte
struct, wrote 8 bytes past every 560-byte netif the library allocated.

Two of the six held nothing that needs a slot, and `tools/patch-lwip.py` removes
them from lwIP in the IDF tree:

- ACD's. `netif.h` reserves it whenever `LWIP_ACD` is on, and nothing reads or
  writes it: `acd.c` keeps its state in `netif->acd_list`.
- The bridge's. `bridgeif.c` parked each port's record in the port netif's
  client data only so `bridgeif_input()` could find it again; it now keeps a
  list of bridges and finds the port by its netif, among at most
  `BRIDGEIF_MAX_PORTS`. Bridges are never freed, so the list cannot dangle.

That leaves DHCP, IGMP, MLD6 and esp_netif's: four, on every espix build, with
DHCP's conflict check, `br0` and `wg0` all working. A hidden Kconfig symbol keeps
esp_netif's slot on even in a build without the bridge. `espix_thread.c`
asserts the offsets the library touches at compile time, so a layout that drifts
-- an lwIP option, an IDF update -- fails to build rather than running.

Measured with everything on: the error is gone; with `eth0` a port of `br0` and
`backbone=br0`, `br0` took the DHCP lease and a SLAAC address in the border
router's on-link prefix, and a Mac on the LAN pinged the mesh through it. Two
fixes that needed: `br0` now asks for SLAAC, which IDF's bridge defaults leave
out, and a bridge port no longer gets a link-local of its own -- it shares
`br0`'s MAC, so it would be the same address twice.

## 11. What is not done

- The dataset hex is ~500 digits and the shell caps a line at 255 bytes, which is
  why `thread join` takes `-f <file>`: `thread dataset > f`, then
  `thread join -f f`.
- NAT64 end to end, from a Thread client through the border router to IPv4, has
  not been shown: it needs a second Thread device (section 9). `host` resolves
  both families, and the AAAA path is verified against ipv6.google.com.
