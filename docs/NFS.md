# NFS

espix serves a mounted volume over NFSv3, so the stick in the USB port is a
network filesystem that a Linux or macOS machine can mount. It is the NAS case:
the board is small and quiet, the disk is the one you already had, and nothing
has to be installed on the other side.

```sh
$ cat /etc/exports
# espix NFS exports: <path> <client>[(options)], as Linux writes them.
# Read-only until the write path exists.
/mnt/sda1  *(ro)
```

**What this is not** is a writable server, or NFSv4. Every procedure that would
write answers `NFS3ERR_ROFS` -- the honest answer rather than a half-working one,
and the one the export already claims. NFSv4 is a different protocol rather than
a later version of this one, and is not implemented; a client that insists on it
is told "Protocol not supported", which is true.

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
where a client is `*` or an IPv4 address with an optional `/bits`, and `rw` would
clear the read-only default if there were a write path to allow.

The client list is checked on **every request**, not only at mount: a file handle
lives for as long as the client likes, so narrowing the file has to reach the
requests that follow a mount as well as the mount itself. A request from an
address the export does not list is answered `NFS3ERR_BADHANDLE`, which a client
recovers from by mounting again; mountd answers `MNT3ERR_ACCES`.

Exports are read when the daemon starts. After editing the file:

```sh
$ sudo service stop nfsd     # wait for the unit to leave
$ sudo service start nfsd
$ nfsd exports
```

`service restart nfsd` sets both intents at once, and the daemon only looks at
its stop flag between requests, so a restart can be missed where a stop waited
for is not.

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
$ nfsd exports           # what the loaded export table holds
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

## Limits

- **Read-only.** `SETATTR`, `WRITE`, `CREATE`, `MKDIR`, `REMOVE`, `RMDIR`,
  `RENAME`, `LINK` and `COMMIT` all answer `NFS3ERR_ROFS`.
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
