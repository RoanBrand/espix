# espix architecture

Notes on how the skeleton is put together and why. Design decisions that are
non-obvious from the code, and the ones that were verified against ESP-IDF
rather than assumed.

Target of record for this skeleton: **ESP32-S3, 16MB flash, 8MB octal PSRAM**,
on **ESP-IDF v6.1**.

## Component graph

`main` owns the init order; nothing else knows it. That is what keeps the graph
acyclic — otherwise a "kernel" component that boots everything would depend on
the commands, while the commands depend on the kernel.

```
espix_kernel     klog ring (dmesg), version, uptime — no espix dependencies
    ↑
espix_fs         LittleFS as /, path resolution, rm -rf
    ↑
espix_shell      sessions, command registry, dispatch, console transport
    ↑
espix_proc       process table, ELF exec
    ↑
espix_fault      panic interception, reaper skeleton
    ↑
espix_ota        A/B updates: slots, the passive-slot write, the manifest
    ↑
espix_cmds       the actual commands
    ↑
  main           app_main = the init sequence, nothing else
```

`espix_net` sits outside this chain, as do the components that grew on top of
it since — SSH, USB, display, services. The arrows above are the core whose
order must stay acyclic, not the whole tree.

## How espix differs from Unix

What follows is not a list of missing calls. It is a handful of structural
choices, and most of what surprises a Unix user follows from one of them.

### There is no MMU, so there is one address space

The S3, and the P4 to a lesser extent, have no real MMU. An app is loaded into
the kernel's own address space and shares it, so there is no process isolation
and no way to build one: `fork()` needs copy-on-write, which is why it is **no**
on the S3 and planned on the S31, which has an MMU. The ELF loader has to
disable memory protection outright (`CONFIG_ESP_SYSTEM_MEMPROT_FEATURE=n`)
because it writes a relocated image and then executes it — precisely what the
feature exists to prevent — and Espressif's own loader examples do the same.

The consequence sets the tone of the whole system: running code is trusted code,
compiled by whoever flashed the board. Permissions, `confine` and setuid are
**guardrails against mistakes**, not a sandbox; a wild write can corrupt the
kernel. The one boundary that is real is the ELF loader's export table, because
an app links no libc at all and can call only what espix publishes to it. The
four answers a name can have — publish it, override it, leave it out, carry it
elsewhere — are the table in
[ROADMAP.md](ROADMAP.md#the-app-abi-what-an-app-may-name-and-who-answers-for-it);
[POSIX.md](POSIX.md) tracks the surface against Unix.

Fault handling follows from the same fact. A panic hook records the core,
exception, faulting address, task and pid into memory that survives the reset,
and the next boot reports it. It deliberately does **not** reap and resume:
locks held by the dead task, no per-process ownership of heap or fds, and the
fact that most corruption never reaches the fault handler at all.
[README.md](../README.md#crash-handling-and-isolation) has the behaviour,
`reaper.c` the reasoning.

### Tasks are not processes

FreeRTOS tasks are what the scheduler runs. espix adds a process table for the
programs a shell starts, because those need a pid, an exit status and something
to kill; builtins run in the session's own task and have none of that. An app is
entered directly — `espix_proc` calls the ELF entry itself, which also keeps the
return value the loader's request wrapper discards.

Two Unix mechanisms are reshaped by having no scheduler you can stop:

- **Signals are delivered when a process calls in, not asynchronously.** A
  handler runs at a delivery point — the blocking calls espix publishes — in the
  process's own task. A pure compute loop that never calls in never sees one;
  `espix_sigcheck()` exists for it and `kill -9` is the answer for somebody
  else's binary. SIGSTOP parks a process *in itself*, because suspending it from
  outside would freeze whatever mutex it held.
- **`kill <pid>` escalates** — SIGTERM, a two-second grace, then SIGKILL —
  because there is no job control and no `fg` to deliver a later signal, and the
  only console may be the one you are typing into. Any signal but SIGSTOP lifts
  a stop. [ROADMAP.md](ROADMAP.md#signals) has the gaps.

A process is per-process and not per-session where it matters: it has its own
working directory, and its credentials are copied from the session at spawn
(following the session's pointer would be a use-after-free, because a
backgrounded process outlives the frame that holds it).

### The filesystem is a VFS with espix's own rules

Every file call goes through ESP-IDF's VFS. Linux and NuttX both check
permissions in the VFS rather than in each filesystem, and that is the shape
espix needs for a further reason: if a filesystem registers at the root, an
app's `fopen()` reaches it with no espix code on the path, so checks placed in
the commands can be stepped around. espix therefore registers its own VFS at
`""`, mounts LittleFS beneath it, and keeps one check (`espix_fs_access_check()`)
above every mount. Paths are real — `/bin/hello`, `/etc/hostname`; `/dev` is a
table espix answers for itself — and the layer below is reached **by pointer,
not by path**, so there is no second name such as `/.lfs` under which the checks
disappear.

### Modes and owners: the filesystem first, then the mount, then a rule

LittleFS keeps no permission bits but does carry user attributes, so a mode and
an owner can be stored there. A volume that keeps no metadata of its own (FAT)
has exactly one owner — whoever mounted it — and reports that through `stat`;
`mount -o uid=,gid=`, or the `/etc/fstab` row that mounted it, sets it. What is
left is the part a Unix reader should carry away: **ownership and the default
mode are derived from the path, not stored per inode.** A path with no stored
attribute and no mount owner belongs to the account whose home directory
contains it, longest home winning, and to root otherwise — the rootfs is root's
and `/home/esp` is esp's with nothing written to flash to say so. The mode rule
gives directories `0755` (`/tmp` is `01777`), a file whose first bytes are the
ELF magic `0755`, and everything else `0644`.

The design means a freshly flashed rootfs is executable with nothing written to
flash, and a `chmod` back to the rule's answer removes the attribute rather than
storing it, so `chmod +x` then `chmod -x` leaves no trace. It is not the whole of
Unix, and the gaps are stated: search permission is checked on the final
component and on the parent of anything that creates or removes a name, not on
every intermediate directory ([KNOWN-ISSUES.md](KNOWN-ISSUES.md)); a volume that
keeps no metadata refuses `chmod`; and there are no symlinks, LittleFS having no
link type. `root` on the console and `esp` over SSH are the two identities, and
all nine mode bits are stored and shown, with setuid a guardrail on the S3 and a
boundary only where an MMU exists.

### A session is a transport; the shell is one dispatch

The serial console and an SSH session are the same shell: one command registry
and dispatch, with a session supplying `{read_line, write, cwd}`. Redirection is
handled once in dispatch rather than per command, and pipes are refcounted
StreamBuffers. ESP-IDF's `esp_console_run()` is not used because it copies every
line through one shared static buffer and is not reentrant — invisible with one
console, corruption with two; only its reentrant pieces are reused.

Two output rules are load-bearing. Kernel messages go to the console itself, via
a bare `printf()`, while command output goes to the invoking session, so kernel
output never splats a remote prompt — the Linux arrangement, and not something
to "fix". And an app's own `printf()` writes to its task's stdio, which espix
rebinds per task, because SSH channel data has to be framed and encrypted rather
than written to a raw socket.

The network layer is deliberately thin: `esp_netif` provides interfaces,
addresses, DHCP and DNS, and `espix_net` maps them to Linux names (`wlan0`,
`eth0`, `usb0`, `lo`) so `ip`, `ifconfig` and `route` render one table.
[ROUTING.md](ROUTING.md), [USB-NETWORKING.md](USB-NETWORKING.md) and
[NFS.md](NFS.md) own the rest.

### Memory is a pool, not a hierarchy

There is no swap and no overcommit. Internal RAM and PSRAM are fixed pools, and
what boot does not claim is all there will be; the display is the claim to
watch. An app's allocations come from a private PSRAM region of its own,
registered when a request cannot otherwise be served and freed whole when the
process exits **or is killed** — the alternative was an app's `malloc()` being
the firmware's, so nothing could give it back.
[APP-MEMORY.md](APP-MEMORY.md) is the owner.

## What is not done yet

Open work has moved out of this file, so that it stays what its title says —
why things are the way they are, rather than what they are not yet.

- [ROADMAP.md](ROADMAP.md) — work espix might take on, and what it would cost.
- [KNOWN-ISSUES.md](KNOWN-ISSUES.md) — behaviour already implemented that will
  still surprise you.
- [UPSTREAM.md](UPSTREAM.md) — defects in ESP-IDF and its components, with the
  workaround espix carries for each.
