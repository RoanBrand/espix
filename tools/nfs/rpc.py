#!/usr/bin/env python3
"""
Talk to espix's NFS server directly, without a kernel client.

A kernel client is the real test, and tools/nfs/linux-mount.sh runs one in a
container. This is for the times it is not enough: a decode error on the client
says nothing about which field was wrong, and a handle-lifetime bug only shows
itself after a pause. Here the request is built and the reply taken apart by
hand, so the answer is a number rather than "RPC struct is bad".

    tools/nfs/rpc.py nsm
    tools/nfs/rpc.py own /mnt/sda1
    tools/nfs/rpc.py read /mnt/sda1 silence44.wav 0 64
    tools/nfs/rpc.py lull 65 /mnt/sda1
    tools/nfs/rpc.py release /mnt/sda1

The write verbs exist because a write path has more ways to be wrong than a read
one: the reply carries a structure even when it fails, and a client decodes the
arm its status chose.

    tools/nfs/rpc.py create /mnt/sda1 hello.txt
    tools/nfs/rpc.py write /mnt/sda1 hello.txt 0 "hello"
    tools/nfs/rpc.py read /mnt/sda1 hello.txt
    tools/nfs/rpc.py mkdir /mnt/sda1 sub
    tools/nfs/rpc.py rename /mnt/sda1 hello.txt /mnt/sda1 sub/hello.txt
    tools/nfs/rpc.py remove /mnt/sda1 sub/hello.txt
    tools/nfs/rpc.py rmdir /mnt/sda1 sub

--host defaults to 192.168.110.203 and can also come from ESPIX_HOST.
"""

import argparse
import os
import socket
import struct
import sys
import time

MOUNT_PROG, MOUNT_VERS = 100005, 3
NFS_PROG, NFS_VERS = 100003, 3
NSM_PROG, NSM_VERS = 100024, 1

NFS3_OK = 0
NFS3ERR = {
    1: "PERM", 2: "NOENT", 5: "IO", 6: "NXIO", 13: "ACCES", 17: "EXIST",
    18: "XDEV", 20: "NOTDIR", 21: "ISDIR", 22: "INVAL", 27: "FBIG",
    28: "NOSPC", 30: "ROFS", 31: "MLINK", 63: "NAMETOOLONG", 66: "NOTEMPTY",
    70: "STALE", 10001: "BADHANDLE", 10002: "NOT_SYNC", 10003: "BAD_COOKIE",
    10004: "NOTSUPP", 10005: "TOOSMALL", 10006: "SERVERFAULT",
    10007: "BADTYPE", 10008: "JUKEBOX",
}
MNT3ERR = {0: "OK", 1: "PERM", 2: "NOENT", 13: "ACCES", 10006: "SERVERFAULT"}


class Rpc:
    def __init__(self, host, timeout=8.0):
        self.host = host
        self.timeout = timeout
        self.xid = 0x5A5A0000

    def call(self, port, prog, vers, proc, body=b""):
        self.xid += 1
        hdr = struct.pack(">IIIIII", self.xid, 0, 2, prog, vers, proc)
        hdr += struct.pack(">II", 0, 0)          # AUTH_NULL credential
        hdr += struct.pack(">II", 0, 0)          # AUTH_NULL verifier
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(self.timeout)
        try:
            s.sendto(hdr + body, (self.host, port))
            rep, _ = s.recvfrom(65536)
        finally:
            s.close()
        if len(rep) < 24:
            raise SystemExit("short reply: %d bytes" % len(rep))
        if struct.unpack(">I", rep[4:8])[0] != 1:
            raise SystemExit("not a reply")
        if struct.unpack(">I", rep[8:12])[0] != 0:
            raise SystemExit("call rejected: %d" % struct.unpack(">I", rep[12:16])[0])
        off = 12
        flen = struct.unpack(">I", rep[off + 4:off + 8])[0]
        off += 8 + (flen + 3) // 4 * 4
        accept = struct.unpack(">I", rep[off:off + 4])[0]
        off += 4
        if accept != 0:
            raise SystemExit("accept stat %d" % accept)
        return rep[off:]


def pad4(b):
    return b + b"\0" * ((4 - len(b) % 4) % 4)


def s_str(s):
    b = s.encode()
    return struct.pack(">I", len(b)) + pad4(b)


def s_opaque(b):
    return struct.pack(">I", len(b)) + pad4(b)


def u32(v):
    return struct.pack(">I", v & 0xFFFFFFFF)


def u64(v):
    return struct.pack(">Q", v)


class Reader:
    """Just enough XDR to take a reply apart."""

    def __init__(self, b, off=0):
        self.b = b
        self.o = off

    def u32(self):
        v = struct.unpack(">I", self.b[self.o:self.o + 4])[0]
        self.o += 4
        return v

    def u64(self):
        hi = self.u32()
        lo = self.u32()
        return (hi << 32) | lo

    def bool(self):
        return self.u32() != 0

    def string(self):
        p = self.u32()
        s = self.b[self.o:self.o + p].decode("utf-8", "replace")
        self.o += (p + 3) // 4 * 4
        return s

    def opaque(self):
        p = self.u32()
        v = self.b[self.o:self.o + p]
        self.o += (p + 3) // 4 * 4
        return v

    def fattr3(self):
        a = {}
        a["type"] = self.u32()
        a["mode"] = self.u32()
        a["nlink"] = self.u32()
        a["uid"] = self.u32()
        a["gid"] = self.u32()
        a["size"] = self.u64()
        a["used"] = self.u64()
        self.u32(); self.u32()               # rdev
        self.u64()                           # fsid
        a["fileid"] = self.u64()
        a["atime"] = self.u64()
        a["mtime"] = self.u64()
        a["ctime"] = self.u64()
        return a

    def post_attr(self):
        return self.fattr3() if self.bool() else None

    def wcc_attr(self):
        """wcc_attr is not a fattr3: size and two times, 24 bytes."""
        a = {}
        a["size"] = self.u64()
        a["mtime"] = self.u64()
        a["ctime"] = self.u64()
        return a

    def wcc(self):
        pre = self.wcc_attr() if self.bool() else None
        post = self.post_attr()
        return pre, post


TYPE = {1: "-", 2: "d", 3: "b", 4: "c", 5: "l", 6: "s", 7: "?"}
STABLE = {0: "UNSTABLE", 1: "DATA_SYNC", 2: "FILE_SYNC"}


def tell_status(st, what=""):
    name = NFS3ERR.get(st, "?%d" % st)
    print("  %-24s nfsstat3=%d (%s)" % (what, st, name))
    return st == NFS3_OK


def cmd_nsm(rpc):
    print("statd, program %d version %d on port 2049:" % (NSM_PROG, NSM_VERS))
    want = {0: 0, 1: 2, 2: 2, 3: 1, 4: 1, 5: 0, 6: 0}
    args = {
        0: b"",
        1: s_str("host.local"),
        2: s_str("host.local") + s_str("host.local") + u32(1) + u32(1) + u32(1) + b"\0" * 16,
        3: s_str("host.local") + s_str("host.local") + u32(1) + u32(1) + u32(1),
        4: s_str("host.local") + u32(1) + u32(1) + u32(1),
        5: b"",
        6: s_str("host.local") + u32(1),
    }
    for proc in range(7):
        body = rpc.call(2049, NSM_PROG, NSM_VERS, proc, args[proc])
        words = [struct.unpack(">I", body[i * 4:i * 4 + 4])[0] for i in range(want[proc])]
        print("  proc %d: %s" % (proc, words))


def mnt_fh(rpc, path):
    body = rpc.call(20048, MOUNT_PROG, MOUNT_VERS, 1, s_str(path))
    r = Reader(body)
    st = r.u32()
    if st != 0:
        raise SystemExit("MNT %s: %s" % (path, MNT3ERR.get(st, st)))
    fh = r.opaque()
    return fh


def export_roots(rpc):
    body = rpc.call(20048, MOUNT_PROG, MOUNT_VERS, 5)
    r = Reader(body)
    roots = []
    more = r.bool()
    while more:
        roots.append(r.string())
        r.bool()                              # no netgroups
        more = r.bool()
    return roots


def lookup_fh(rpc, dirfh, name):
    body = rpc.call(2049, NFS_PROG, NFS_VERS, 3, s_opaque(dirfh) + s_str(name))
    r = Reader(body)
    if r.u32() != NFS3_OK:
        raise SystemExit("lookup %s: %s" % (name, r.b[:4].hex()))
    return r.opaque()


def fh_for(rpc, path):
    """A handle for any path inside an export.

    MNT hands out the root of an export and nothing else, so anything deeper is
    the root and a LOOKUP per component -- which is exactly what a client does,
    and why a handle for the root is not reachable by asking mountd for a
    subdirectory.
    """
    for root in sorted(export_roots(rpc), key=len, reverse=True):
        if path == root or path.startswith(root.rstrip("/") + "/"):
            fh = mnt_fh(rpc, root)
            rest = path[len(root):].strip("/")
            for comp in (rest.split("/") if rest else []):
                fh = lookup_fh(rpc, fh, comp)
            return fh
    raise SystemExit("no export covers %s (mountd says %s)"
                     % (path, ", ".join(export_roots(rpc)) or "nothing"))


def cmd_mnt(rpc, a):
    fh = mnt_fh(rpc, a.path)
    print("MNT %s -> %d-byte handle %s" % (a.path, len(fh), fh.hex()))


def cmd_own(rpc, a):
    fh = fh_for(rpc, a.path)
    body = rpc.call(2049, NFS_PROG, NFS_VERS, 1, s_opaque(fh))
    r = Reader(body)
    st = r.u32()
    if not tell_status(st, "GETATTR " + a.path):
        return
    f = r.fattr3()
    print("  %s mode=0%o uid=%d gid=%d size=%d" %
          (TYPE.get(f["type"], "?"), f["mode"], f["uid"], f["gid"], f["size"]))
    body = rpc.call(2049, NFS_PROG, NFS_VERS, 17,
                    s_opaque(fh) + u64(0) + b"\0" * 8 + u32(1024) + u32(8192))
    r = Reader(body)
    st = r.u32()
    if not tell_status(st, "READDIRPLUS"):
        return
    r.post_attr()
    r.o += 8                                  # cookie verifier
    n = 0
    while r.bool():
        r.u64()
        name = r.string()
        r.u64()
        attrs = r.post_attr()
        if r.bool():
            r.opaque()
        n += 1
        if n <= a.first:
            print("  uid=%-5d gid=%-5d size=%-9d %s" %
                  (attrs["uid"], attrs["gid"], attrs["size"], name))
    print("  %d entries in this page" % n)


def cmd_lookup(rpc, a):
    fh = fh_for(rpc, a.path)
    body = rpc.call(2049, NFS_PROG, NFS_VERS, 3, s_opaque(fh) + s_str(a.name))
    r = Reader(body)
    st = r.u32()
    if not tell_status(st, "LOOKUP " + a.name):
        return
    child = r.opaque()
    r.post_attr()
    r.post_attr()
    print("  handle %s" % child.hex())


def cmd_read(rpc, a):
    fh = fh_for(rpc, a.path)
    body = rpc.call(2049, NFS_PROG, NFS_VERS, 3, s_opaque(fh) + s_str(a.name))
    r = Reader(body)
    st = r.u32()
    if not tell_status(st, "LOOKUP " + a.name):
        return
    fh = r.opaque()
    body = rpc.call(2049, NFS_PROG, NFS_VERS, 6,
                    s_opaque(fh) + u64(a.offset) + u32(a.count) + u32(0))
    r = Reader(body)
    st = r.u32()
    if not tell_status(st, "READ"):
        return
    r.post_attr()
    count = r.u32()
    eof = r.bool()
    data = r.opaque()
    print("  count=%d eof=%d data=%d bytes" % (count, eof, len(data)))
    if a.show:
        sys.stdout.write(data.decode("utf-8", "replace"))


def cmd_lull(rpc, a):
    """A handle taken now, used after a pause: the bug this catches is a table
    that ages out from under a client that is holding handles and not minting
    any."""
    fh = fh_for(rpc, a.path)
    print("handle taken; pausing %d s with it held" % a.seconds)
    time.sleep(a.seconds)
    body = rpc.call(2049, NFS_PROG, NFS_VERS, 3, s_opaque(fh) + s_str(a.name))
    r = Reader(body)
    tell_status(r.u32(), "LOOKUP " + a.name)
    body = rpc.call(2049, NFS_PROG, NFS_VERS, 1, s_opaque(fh))
    r = Reader(body)
    tell_status(r.u32(), "GETATTR the old handle")


def cmd_release(rpc, a):
    fh = fh_for(rpc, a.path)
    body = rpc.call(2049, NFS_PROG, NFS_VERS, 1, s_opaque(fh))
    tell_status(Reader(body).u32(), "GETATTR (allocates)")
    rpc.call(20048, MOUNT_PROG, MOUNT_VERS, 3, s_str(a.path))
    print("UMNT sent; the table should be back on the board's free list")


def cmd_create(rpc, a):
    dirfh = fh_for(rpc, a.dir)
    how = {"unchecked": 0, "guarded": 1, "exclusive": 2}[a.how]
    body = s_opaque(dirfh) + s_str(a.name) + u32(how)
    if how == 2:
        body += b"espix!!!"                      # verifier[8]
    else:
        body += u32(1) + u32(0o644) + u32(0) + u32(0) + u32(0) + u32(0) + u32(0)
    rep = rpc.call(2049, NFS_PROG, NFS_VERS, 8, body)
    r = Reader(rep)
    if tell_status(r.u32(), "CREATE " + a.name):
        if r.bool():
            print("  handle %s" % r.opaque().hex())
        else:
            print("  no handle returned")
        r.post_attr()
    # Parsed in both arms on purpose: the failure arm carries a wcc too, and a
    # reply that stops after the status is an I/O error on a real client.
    r.wcc()


def cmd_mkdir(rpc, a):
    dirfh = fh_for(rpc, a.dir)
    body = s_opaque(dirfh) + s_str(a.name) + u32(1) + u32(a.mode) + u32(0) + u32(0) + u32(0) + u32(0) + u32(0)
    rep = rpc.call(2049, NFS_PROG, NFS_VERS, 9, body)
    r = Reader(rep)
    if tell_status(r.u32(), "MKDIR " + a.name):
        print("  handle %s" % (r.opaque().hex() if r.bool() else "none"))
        r.post_attr()
    r.wcc()


def cmd_write(rpc, a):
    dirfh = fh_for(rpc, a.dir)
    body = rpc.call(2049, NFS_PROG, NFS_VERS, 3, s_opaque(dirfh) + s_str(a.name))
    r = Reader(body)
    if not tell_status(r.u32(), "LOOKUP " + a.name):
        return
    fh = r.opaque()
    data = a.data.encode() if a.data is not None else sys.stdin.buffer.read()
    stable = {"unstable": 0, "data": 1, "file": 2}[a.stable]
    body = (s_opaque(fh) + u64(a.offset) + u32(len(data)) + u32(stable) +
            s_opaque(data))
    rep = rpc.call(2049, NFS_PROG, NFS_VERS, 7, body)
    r = Reader(rep)
    st = r.u32()
    if tell_status(st, "WRITE %d bytes" % len(data)):
        r.wcc()
        print("  count=%d committed=%s" % (r.u32(), STABLE.get(r.u32())))
        print("  verf=%s" % r.b[r.o:r.o + 8].hex())
    else:
        r.wcc()


def cmd_setattr(rpc, a):
    fh = fh_for(rpc, a.dir + "/" + a.name)
    body = s_opaque(fh)
    body += u32(1) + u32(a.mode) if a.mode is not None else u32(0)
    body += u32(0)                                   # uid
    body += u32(0)                                   # gid
    body += u32(1) + u64(a.size) if a.size is not None else u32(0)
    if a.mtime is not None:
        body += u32(1) + u32(2) + u32(a.mtime) + u32(0)
    else:
        body += u32(0) + u32(0)
    body += u32(0)                                   # no guard
    rep = rpc.call(2049, NFS_PROG, NFS_VERS, 2, body)
    r = Reader(rep)
    tell_status(r.u32(), "SETATTR")
    r.wcc()


def cmd_remove(rpc, a):
    dirfh = fh_for(rpc, a.dir)
    rep = rpc.call(2049, NFS_PROG, NFS_VERS, 12, s_opaque(dirfh) + s_str(a.name))
    r = Reader(rep)
    tell_status(r.u32(), "REMOVE " + a.name)
    r.wcc()


def cmd_rmdir(rpc, a):
    dirfh = fh_for(rpc, a.dir)
    rep = rpc.call(2049, NFS_PROG, NFS_VERS, 13, s_opaque(dirfh) + s_str(a.name))
    r = Reader(rep)
    tell_status(r.u32(), "RMDIR " + a.name)
    r.wcc()


def cmd_rename(rpc, a):
    fromfh = fh_for(rpc, a.dir)
    tofh = fh_for(rpc, a.todir)
    body = (s_opaque(fromfh) + s_str(a.name) + s_opaque(tofh) + s_str(a.toname))
    rep = rpc.call(2049, NFS_PROG, NFS_VERS, 14, body)
    r = Reader(rep)
    tell_status(r.u32(), "RENAME %s" % a.name)
    r.wcc()
    r.wcc()


def cmd_commit(rpc, a):
    dirfh = fh_for(rpc, a.dir)
    body = rpc.call(2049, NFS_PROG, NFS_VERS, 3, s_opaque(dirfh) + s_str(a.name))
    r = Reader(body)
    if not tell_status(r.u32(), "LOOKUP " + a.name):
        return
    fh = r.opaque()
    rep = rpc.call(2049, NFS_PROG, NFS_VERS, 21, s_opaque(fh) + u64(0) + u32(0))
    r = Reader(rep)
    if tell_status(r.u32(), "COMMIT"):
        r.wcc()
        print("  verf=%s" % r.b[r.o:r.o + 8].hex())


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default=os.environ.get("ESPIX_HOST", "192.168.110.203"))
    sub = p.add_subparsers(dest="verb", required=True)

    sub.add_parser("nsm").set_defaults(fn=lambda rpc, a: cmd_nsm(rpc))
    q = sub.add_parser("mnt"); q.add_argument("path"); q.set_defaults(fn=cmd_mnt)
    q = sub.add_parser("own"); q.add_argument("path")
    q.add_argument("--first", type=int, default=4); q.set_defaults(fn=cmd_own)
    q = sub.add_parser("lookup"); q.add_argument("path"); q.add_argument("name")
    q.set_defaults(fn=cmd_lookup)
    q = sub.add_parser("read"); q.add_argument("path"); q.add_argument("name")
    q.add_argument("offset", nargs="?", type=int, default=0)
    q.add_argument("count", nargs="?", type=int, default=64)
    q.add_argument("--show", action="store_true"); q.set_defaults(fn=cmd_read)
    q = sub.add_parser("lull"); q.add_argument("seconds", type=int)
    q.add_argument("path"); q.add_argument("name", nargs="?", default=".")
    q.set_defaults(fn=cmd_lull)
    q = sub.add_parser("release"); q.add_argument("path"); q.set_defaults(fn=cmd_release)

    q = sub.add_parser("create"); q.add_argument("dir"); q.add_argument("name")
    q.add_argument("--how", choices=["unchecked", "guarded", "exclusive"],
                   default="unchecked"); q.set_defaults(fn=cmd_create)
    q = sub.add_parser("mkdir"); q.add_argument("dir"); q.add_argument("name")
    q.add_argument("--mode", type=lambda s: int(s, 8), default=0o755)
    q.set_defaults(fn=cmd_mkdir)
    q = sub.add_parser("write"); q.add_argument("dir"); q.add_argument("name")
    q.add_argument("offset", type=int); q.add_argument("data", nargs="?")
    q.add_argument("--stable", choices=["unstable", "data", "file"], default="file")
    q.set_defaults(fn=cmd_write)
    q = sub.add_parser("setattr"); q.add_argument("dir"); q.add_argument("name")
    q.add_argument("--size", type=int)
    q.add_argument("--mode", type=lambda s: int(s, 8))
    q.add_argument("--mtime", type=int)
    q.set_defaults(fn=cmd_setattr)
    q = sub.add_parser("remove"); q.add_argument("dir"); q.add_argument("name")
    q.set_defaults(fn=cmd_remove)
    q = sub.add_parser("rmdir"); q.add_argument("dir"); q.add_argument("name")
    q.set_defaults(fn=cmd_rmdir)
    q = sub.add_parser("rename"); q.add_argument("dir"); q.add_argument("name")
    q.add_argument("todir"); q.add_argument("toname"); q.set_defaults(fn=cmd_rename)
    q = sub.add_parser("commit"); q.add_argument("dir"); q.add_argument("name")
    q.set_defaults(fn=cmd_commit)

    a = p.parse_args()
    rpc = Rpc(a.host)
    try:
        a.fn(rpc, a)
    except socket.timeout:
        raise SystemExit("%s: no reply (server down, or the request was too big "
                         "for its buffer)" % a.host)


if __name__ == "__main__":
    main()
