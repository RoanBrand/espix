# NFS

espix serves a mounted volume over NFSv3, so the stick in the USB port is a
network filesystem that a Linux or macOS machine can mount. It is the NAS case:
the board is small and quiet, the disk is the one you already had, and nothing
has to be installed on the other side.

```sh
$ cat /etc/exports
# espix NFS exports: <path> <client>[(options)], as Linux writes them.
# Read-write unless a line says ro.
/mnt/sda1  *(ro)
```

**What this is not** is NFSv4. An export is read-write unless its line says
`ro`, which is espix's own convention rather than Linux's: Linux's exports(5)
has the opposite default -- "the default is to disallow any request which
changes the filesystem" -- while BSD and macOS export files default to
read-write. espix follows its *mounts*, which are read-write unless told `ro`,
so an export with no option is writable as well: the two should not disagree
about what an unconfigured volume is.

A client with write access creates, writes, renames and removes, and every one
of those is the filesystem underneath answering: a read-only mount refuses with
EROFS, a full volume with ENOSPC, and the client gets the errno it was given.

NFSv4 is a different protocol rather than a later version of this one, and is not
implemented; a client that insists on it is told "Protocol not supported", which
is true.

## Mounting it

Linux, with no options at all:

```sh
$ sudo mount -t nfs 192.168.110.203:/mnt/sda1 /mnt/x
$ ls -l /mnt/x
-rwxrwxrwx 1 1000 1000  329008 Sep 30 09:57 doom
```

The mount comes back `vers=3` with `local_lock=none`: the client found statd,
so locking is negotiated and neither `nolock` nor `nolocks` is needed. On a
distribution whose `mount.nfs` tries NFSv4 first, add `-o vers=3` -- there is no
v4 here to negotiate with, and the failure says so.

macOS, the same way:

```sh
$ sudo mount -t nfs 192.168.110.203:/mnt/sda1 /tmp/stick
```

Verified with no options on both: Linux mounts it as `vers=3` with
`local_lock=none`, and macOS mounts it and lists the same ownership. A macOS
client asks the server for statd before it will mount with locking enabled, which
is why that program is served; `-o nolocks` remains the option that turns
locking off entirely.

## What is served, and where

Four ONC RPC programs, which is what an NFS mount is made of:

| Program | Version | Port | What it is |
|---|---|---|---|
| 100000 | 2 | 111 | portmapper: where the others are |
| 100005 | 1, 2, 3 | 20048 | mountd: the file handle for an export |
| 100003 | 3 | 2049 | nfsd: the filesystem |
| 100024 | 1 | 2049 | statd: the lock-status monitor |

mountd has no well-known port, which is the whole reason the portmapper exists:
a client asks 111 where mountd is, and that is the only place the number is
written down. `rpcinfo -p <host>` lists all four.

statd shares nfsd's port because the program number is what tells one RPC from
another, so it costs no socket, no task and no buffer. There are no locks here to
restore after a restart, so a monitor is accepted and forgotten -- but the reply
keeps the shape a client reads, because a short answer where it expects two
words is what abandons a mount.

## Exports, and who may mount

`/etc/exports` is Linux's syntax, less the hostnames: `<path> <client>[(options)]`,
where a client is `*` or an IPv4 address with an optional `/bits`. Each client
carries its own options, and **the most specific match wins**, so a wildcard and a
host can sit together and the host gets what it says:

```sh
$ cat /etc/exports
/mnt/sda1  *(ro)                everyone reads it
/mnt/sda1  192.168.1.5(rw)      one host may write
```

The options are Linux's: `ro` and `rw`, and the three squashes -- `root_squash`
(the default: a client's uid 0 becomes "nobody", 65534), `no_root_squash`, and
`all_squash` for a share nobody should own -- with `anonuid=`/`anongid=` to say
what nobody is. Anything else is ignored rather than refused, which is the only
way one file can serve two servers.

**A squash is enforced, not decorative.** Every request is answered as the client
it came from: espix's filesystem asks who is calling, and the NFS server has no
session of its own, so it supplies the client's identity rewritten by the rule.
That is what makes a permission check mean anything over there -- with the
default `root_squash` a client cannot write into a root-owned directory, and
`no_root_squash` lets it. What a squash does *not* change is ownership: espix
decides who owns a file from its path (the rootfs) or from the mount (FAT), never
from who created it.

The client list is checked on **every request**, not only at mount: a handle lives
for as long as the client likes, so a narrower export has to reach the requests
that follow a mount as well as the mount itself. A request from an address the
export does not list is answered `NFS3ERR_BADHANDLE`, which clients recover from;
mountd answers `MNT3ERR_ACCES`.

Editing the file does not need a restart:

```sh
$ sudo vi /etc/exports
$ nfsd reload            # the daemon re-reads it on its next pass, within 5 s
$ nfsd exports           # what it understood, options and all
```

`nfsd exports` prints each rule as the parser read it, which is the only way to
see that an option was taken as written -- a `ro` inside `crossmnt` is not a
`ro`. When the file is missing entirely, the daemon writes a template with every
example commented out, `0644` and root's, and serves nothing until a line names a
client.
## Handles, listings, and ownership

A file handle is 28 bytes and names a slot in a table of paths rather than
carrying a path: a 64-byte handle cannot hold one, and a path changes when a
directory is renamed. The table is 1024 slots in PSRAM -- 258 KB, because a slot
holds a path of the 256 bytes espix allows anywhere, and most paths are thirty --
allocated on the first handle a client asks for.

It is given back the moment no client can be holding one: mountd's UMNT, which a
client sends when it unmounts, or the daemon stopping. That signal and not a
timer, because a client using the handles it has mints none, so its silence says
nothing, and a table that aged out would take with it handles the client is still
holding. Measured on the board, PSRAM in use: 291 KB with nothing mounted, 557 KB
while a client has the export mounted, 297 KB once the client unmounts -- macOS
included. A client that never unmounts -- it crashed, or the cable went -- keeps
the table, which is the safe direction of that trade. A handle whose slot has been
reused, or whose client has lost its permission, answers `NFS3ERR_BADHANDLE`,
which clients are built to recover from.

A listing is **one** directory walk. `READDIRPLUS` carries each entry's
attributes and a ready-made handle, both taken from the walk rather than from a
stat per name -- a stat on an exFAT volume is a scan of the directory, and 500 of
them is what made a listing take seventeen seconds before this. What the walk
cannot report is an owner, so the mount's owner is asked once per page and
stamped on every entry; on a volume that keeps no ownership of its own (FAT,
exFAT) that is exactly right, since the volume has one owner and it is whoever
mounted it. `uid=`/`gid=` on the mount are therefore what a client sees.

## Commands

```sh
$ nfsd                   # is it running, and on what
$ nfsd exports           # what the loaded export table holds, options and all
$ nfsd reload            # re-read /etc/exports without stopping the unit
$ nfsd trace on          # append every RPC to /tmp/nfsd.trace
$ nfsd start             # run it in this session instead of as a unit
```

The unit is `nfsd always nfsd` in `/etc/units`, so it comes up at boot. Nothing
else is needed: the portmapper, mountd and nfsd are one task with six sockets and
two PSRAM buffers.

## Measured

On the ESP32-S31 board, over WiFi:

| Operation | Time |
|---|---|
| 500-entry directory, Linux client, `ls` | 1.3 s (client startup included) |
| `ls -l` of the export root | 0.04 s |
| A file read over NFS | line rate of the link |
| 4 MiB written, Linux client, `cp` | 10 s (~410 KB/s), sha256 identical |

## Writing

An export is a filesystem a client can change unless it says `ro`:

```sh
$ sudo mount -t nfs 192.168.110.203:/mnt/sda1 /mnt/x
$ cp big.bin /mnt/x/ && sha256sum big.bin /mnt/x/big.bin
$ mkdir /mnt/x/here && mv /mnt/x/big.bin /mnt/x/here/ && rm /mnt/x/here/big.bin
```

Three things are worth knowing before trusting it with something:

- **A mode or an owner is accepted and forgotten.** FAT has neither, so a chmod
  over the wire is not something the volume can honour -- but *refusing* it fails
  the create-then-setattr sequence every `cp` is made of, because the client asks
  for the mode it opened the file with. A Linux server exporting a vfat directory
  behaves the same way. Through the shell on the device, `chmod` still answers
  EPERM: that is espix's own filesystem surface, not this one.
- **A write is as durable as the client asked for.** With `stable` UNSTABLE the
  data sits in the filesystem's cache until the client's COMMIT; FILE_SYNC and
  DATA_SYNC are flushed before the reply. An ordinary `cp`, `dd` or editor does
  commit, and a file written that way survives a reboot -- which is how it was
  checked, by writing 2 MiB, rebooting the board and reading it back.
- **Hard links and symlinks are refused** with `NFS3ERR_NOTSUPP`, which is what
  they are: this filesystem has not got them.

Measured, 4 MiB written over WiFi from a Linux client in a container on the same
LAN: 10 seconds, about 410 KB/s, with the sha256 of the copy identical to the
source. The read and write sizes are both 8192 now; at 4096 the same link
measured about 260 KB/s, which is what halving the round trips buys.

Two host-side tools live in the tree for the times a kernel client is not enough:

    tools/nfs/rpc.py nsm|own|read|lull|release|create|write|setattr|...
    tools/nfs/linux-mount.sh [rw]

`rpc.py` speaks the protocol itself, so a disagreement is a number rather than
"RPC struct is bad" -- it is how the handle lifetime and the table release are
checked, and it can send a verb no client would. `linux-mount.sh` mounts the
export from a real Linux client in a container, which is the test that matters
for compatibility: a kernel client decodes replies with its own XDR, and two of
the bugs in this file were found by exactly that.

## Limits

- **Writes can be turned off per export.** A line that says `ro` answers
  `NFS3ERR_ROFS` to every writing procedure; with no option the export is
  writable, as espix's mounts are.
- **NFSv3 only.** No v4, and no NFS over RDMA, obviously.
- **No squashing.** A file is reported with the uid and gid of the mount's owner
  and no client identity is rewritten: root on the client is root on the wire.
  Exports are meant for a private LAN, which is also why `/etc/exports` is not
  narrowed by default.
- **No advisory locking.** statd answers so that a client will mount; there is no
  lock manager behind it, so `flock`/POSIX locks between clients are not
  arbitrated and a lock is local to the client that took it.
- **No hostname clients.** A name in `/etc/exports` is not resolved; write the
  address.
