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

## 4. /etc/thread.conf: policy, not protocol state

The file holds four keys, all espix policy that OpenThread has no concept of:

    enabled=no          start the stack at boot?
    border_router=no    route the mesh to the backbone through NAT64?
    backbone=eth0       which interface the border router routes to
    attach=no           join the stored dataset at boot?

The dataset -- channel, PAN ID, network key, mesh prefix, PSKc, security policy --
is OpenThread's and stays in OpenThread's own NVS. None of it is duplicated in
/etc, and `thread dataset` reads it out rather than storing it.

The file is created by the kernel on the first boot that finds it missing, the way
`espix_auth` creates `/etc/sudoers`: `ensure_conf()` writes it under
`espix_fs_priv_begin()` with the keys commented and at their defaults. A zero-byte
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

There is one cost, and it is written into the patch: while the flash cache is
frozen during an OTA write, PSRAM is unreachable, so the instance may not be
touched. Do not OTA while OpenThread is running.

## 6. The kernel slot

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

## 8. The CLI answers the session that asked

`thread cli` uses `esp_openthread_cli_input()`, so the line runs on the
OpenThread task. The CLI's output callback, which IDF fixes when the stack is
created, is replaced at start with one that captures into a buffer and wakes the
task waiting on the prompt `ProcessLine()` prints last; the prompt itself is not
forwarded. Running `otCliInputLine()` in the calling task under the lock would
capture more directly but deadlock on any command that waits for the stack, since
the OT mainloop takes the same lock.

A command that never answers gives up after ten seconds and its later output goes
to the console, which is where unsolicited output belongs.

## 9. IPv6, and reaching IPv4 through NAT64

`thread0`'s addresses come from the stack, not from the glue's lwIP netif, and
are rendered by the same `ip`/`ifconfig` path as every other interface. The
border router advertises a NAT64 prefix (the ULA `fd9c::/48` by default) and
Thread clients reach IPv4 through it.

For a Thread client to resolve an IPv4 name, resolution has to come back as a
NAT64 address: IDF's DNS64 client (`CONFIG_OPENTHREAD_DNS64_CLIENT`) does that
by pointing OpenThread's DNS at `OPENTHREAD_DNS_SERVER_ADDR` encoded into the
NAT64 prefix. That address is an IPv4 resolver reachable from the backbone; the
default is 8.8.8.8, so set it to the uplink's resolver on a network that does not
reach it.

The prefix comes from the border router itself. OpenThread generates a local
NAT64 prefix from the BR ULA (`fd9c:a3f8:cd42:2::/96` on this board) and
publishes it as a route in the Thread network data -- but not on its own. The
prefix manager starts disabled, and with it disabled `Evaluate()` returns
immediately, so no route is published and IDF's DNS64 client, which reads that
route to learn what to encode a resolver into, stays quiet. espix turns it on
with `otNat64SetEnabled(instance, true)` as border routing is initialised, and
off again as it is torn down. Measured before and after: `nat64 state` goes
from Disabled to Active, `netdata show` gains
`fd9c:a3f8:cd42:2:0:0::/96 sn low`, and the klog gains
`Set dns server address: FD9C:A3F8:CD42:2::C0A8:6E01` -- the uplink resolver,
192.168.110.1, encoded in the prefix. `thread status` shows it as
`nat64 active`.

The Router Solicitation still matters for the other case: when the network does
have an IPv6 router, the border router learns a NAT64 prefix from it, and to ask
it has to send an RS on the backbone -- which needs an IPv6 link-local on that
interface. espix creates one on every esp_netif interface as it comes up
(`espix_net_ip6_linklocal()`, called from the GOT_IP path because creation only
works on an up netif), so `eth0` and `wlan0` carry a `fe80::` address and the
RS has somewhere to come from.

## 10. What is not done

- The dataset hex is ~500 digits and the shell caps a line at 255 bytes, which is
  why `thread join` takes `-f <file>`: `thread dataset > f`, then
  `thread join -f f`.
- `ping` is IPv4-only; the resolver it uses returns a 32-bit address. `host`
  resolves both families, and the AAAA path is verified against ipv6.google.com.
