# The Unix surface, and where espix differs

The deviations that are *deliberate* are written down twice, and this file is
neither of them: [ROADMAP.md](ROADMAP.md) carries the surface table with the
honest answer for each, and the code sites carry an `ESPIX_NOT_POSIX:` marker that
`grep -rn` finds. What follows is the inventory, not the argument.

What works today, against the Unix surface people expect. **planned** means
intended but not built yet; **no** means deliberately out of scope rather than
merely missing.

### Shell

| | | |
|---|---|---|
| Interactive shell over serial and SSH | **yes** | same dispatch, same output on both |
| Line editing, history, TAB completion | **yes** | history follows the user, not the connection |
| Output redirection `>` `>>` | **yes** | attached too: `>file`, `1>file` |
| Error redirection `2>` `2>>` `2>&1` | **yes** | attached or separated (`2>file`); diagnostics are their own stream, and over SSH they arrive as `CHANNEL_EXTENDED_DATA`, so the client's `2>` separates them too |
| Quoting and backslash escapes | **yes** | |
| Exit status | **yes** | `exit 3` reaches an SSH client's `$?` |
| A session's terminal, termios, `stty` | **yes** | every login has a tty (SSH, the serial console, a VNC terminal): echo, line editing and the signal keys are its line discipline's. An app's fds 0-2 are it in both builds -- `isatty`, `tcgetattr`/`tcsetattr`, `cfmakeraw`, `TIOCGWINSZ`, and `poll`/`select` that wait for a key; an exec without a pty gives a pipe. `stty` shows and sets it |
| Background jobs `&` | **yes** | `&`, `$!`, `jobs`/`fg`/`bg`/`wait`, Ctrl-Z stops the foreground app into a job; a job that reads the terminal stops on SIGTTIN (EIO if it ignores or blocks it), over SSH, on the serial console and in a VNC terminal; a job still dies at logout by design |
| Pipes <code>&#124;</code> | **yes** | builtins only; one StreamBuffer per pipe, and a writer fails once its reader is done (`cat f \| head -1`) |
| Input redirection `<` | **yes** | attached or separated (`<file`); a builtin that reads stdin uses it |
| Environment variables, `env`, `export`, `unset` | **yes** | a per-session table; `PATH` is consulted when running a program, and an app gets `getenv`, `setenv`, `unsetenv` and `putenv` |
| Globbing `*` | **planned** | |
| Shell scripts, `#!`, control flow | **planned** | the executable bit is now real, so `#!` needs only the dispatch: a file that is executable but not an ELF is where the interpreter line would be read |

### Processes

| | | |
|---|---|---|
| Run a cross-compiled native app by name | **yes** | `/bin` search, argv, exit status |
| `ps`, `top` | **yes** | live CPU and memory, per core |
| Stop a running command — Ctrl-C, `kill` | **yes** | a command is a process like an app; Ctrl-C is SIGINT to the foreground job's group; `kill` asks, then insists |
| Process tree — `$$`, `$!`, `getppid`, `getpgrp`, `getsid`, `kill -<pgid>`, `killpg` | **yes** | a shell is a process and its apps' parent; each job is its own group |
| Signals and handlers — `signal()`, `kill -9`, `-STOP`/`-CONT` | **yes** | real POSIX names, with `sigaction`'s mask and flags, `sigsuspend` and `pthread_sigmask`; an M-mode app takes them when it calls in, under one mask per process, and a U-mode app (S31) at any instruction, with per-thread masks and `SA_RESTART` |
| `kill -l`, `ps` showing `T` for stopped | **yes** | |
| A crashing app not taking the system down | **yes** | a fault ends the app with 128 + its signal, or runs its handler, in both modes and on both targets; what it wrote before the fault, a lock it held in the kernel and a blown stack are not undone ([crash handling](../README.md#crash-handling-and-isolation)) |
| `sed` | **planned** | |
| Apps using the filesystem | **yes** | `fopen`, `opendir`, `stat`, `chmod`, `dup`, `dup2`; `stat` reports the same mode and owner `ls -l` shows. A POSIX app has a descriptor table of its own, so `dup2` onto its stdin replaces it as on Linux |
| Per-process working directory | **yes** | an app's `chdir()` does not move the shell that ran it |
| `mmap`, `munmap`, `mprotect`, `msync`, `madvise`, `mincore` | **yes** for POSIX apps | U-mode apps: anonymous and file mappings, private or shared, top-down above the heap; a shared file's pages are written back by `msync`, `munmap` and exit; a `malloc` of 128 KiB or more is a mapping ([USER-MODE.md](USER-MODE.md#mappings)). S3 apps: a block of the app's memory, no `MAP_FIXED`, unmapped whole ([USER-MODE.md](USER-MODE.md#the-s3-posix-apps-without-an-mmu)) |
| `fork()`, `vfork()`, the `exec` family, `wait`/`waitpid` | **yes**, POSIX apps on both, but `fork` ENOSYS on S3 as on a Linux without an MMU | U-mode apps: `fork` on copy-on-write, `vfork` lending the parent's memory until the child's `exec` or end, `exec` in place keeping the pid and the descriptors not marked close-on-exec, and Linux's wait statuses; `posix_spawn`, `system`, `popen` and `daemon` on top ([USER-MODE.md](USER-MODE.md#processes-fork-vfork-exec-wait)). S3 apps: `vfork`'s child runs on the parent's task, `exec` reloads on the process's own, and `posix_spawn`, `system` and `popen` work over them ([USER-MODE.md](USER-MODE.md#the-s3-posix-apps-without-an-mmu)) |
| `pipe`, `pipe2` for apps | **yes**, POSIX apps (`espix-cc`) on both | a kernel pipe of 4 KB between two descriptors, blocking until data, room or the other end's close, with EPIPE and SIGPIPE; `PIPE_BUF` is 512 |
| `setsid`, `setpgid`, `getpgid`, `umask` | **yes**, POSIX apps on both | `setsid` leaves the shell's session, its streams becoming the terminal's own, so a daemon outlives the login that started it |
| MMU-backed process isolation | **no** on S3, **yes** on S31 | a U-mode app has its own address space, and its fault ends only it; see [hardware targets](../README.md#hardware-targets) and [crash handling](../README.md#crash-handling-and-isolation) |
| setuid / setgid / sticky | **yes** | all three consulted; setuid is a guardrail on S3 and a boundary on S31 |
| `reboot`, `poweroff` | **yes** | one sequence for both: every process, the units' daemons included, is SIGTERM'd and waited for — with no forced kill, because a task deleted while holding a lock is how a clean stop becomes a corrupt volume — every volume is flushed and unmounted, and then the reset. `poweroff` is deep sleep with a 10-minute timer, because the chip cannot cut its own power; both are root only |
| `shutdown(8)`, `wall`, a scheduled shutdown | **no** | `poweroff [minutes]` is the whole surface. Nothing broadcasts, and no protocol is told: an NFS hard mount just waits for the server to come back — [NFS](NFS.md#limits) |

### Filesystem

| | | |
|---|---|---|
| LittleFS mounted as the real `/` | **yes** | survives reboot and firmware reflash |
| `ls` `cd` `pwd` `cat` `cp` `mv` `rm` `rmdir` `mkdir` `touch` `chmod` `df` | **yes** | |
| `ls -1adhiltr` | **yes** | sorted by name, or by mtime with `-t`; `-d` describes an entry instead of listing it; `-R` is not implemented |
| `ls -i`, inode numbers | **no** | esp_littlefs reports `d_ino = 0` for every entry, and LittleFS exposes no file id |
| Per-session working directory | **yes** | your `cd` is not someone else's |
| File timestamps | **yes** | `ls -l` and `sftp ls -l` show mtime; files from the flashed image have none; a FAT volume reports the time it stores — two-second resolution, no access time — and an NFS client's SETATTR sets a file's time |
| `/proc` | **yes** | `meminfo`, `cpuinfo`, `uptime` and `version`, and per process `<pid>/status` and `<pid>/task/<tid>/status`, generated on every read and stored nowhere |
| `mount`, `umount` | **yes** | `mount sda1 /mnt` puts a FAT32/FAT16 volume from a USB device into the namespace, reached through espix's own VFS so the permission check applies to it. Root only; nothing is ever formatted; unplug while mounted is a gap; `mount -o uid=,gid=` hands the volume to a user without root, and `-o ro` mounts it read-only — [USB-HOST](USB-HOST.md#stage-2--mounting) |
| `/etc/fstab` | **yes** | applied on attach and undone on removal: the device column takes a name, a wildcard, or an identity from `blkid` (`LABEL=`, `UUID=`, `PARTUUID=`); the owner column — or `uid=`/`gid=` in the Linux-style options column, which also takes `noauto` and `ro` — becomes the volume's owner, and an entry naming none mounts as `esp` — [USB-HOST](USB-HOST.md) |
| `lsblk`, `blkid` | **yes** | USB storage is enumerated, identified and its partition table read — including the filesystems espix has no driver for, and disks with no partition table at all (a superfloppy's own volume is named). **Four device slots**, which is what a hub needs; two FAT volumes at once — [USB-HOST](USB-HOST.md) |
| Mode bits, `chmod` | **yes** | all twelve, octal or symbolic; `ls -l` and `sftp ls -l` show the same thing; `open` and `mkdir` apply their mode through the process's umask, `022` unless it sets another, so `cp` keeps a program runnable and a private file private |
| An executable bit | **yes** | enforced — `chmod -x` stops a program running. A new binary is executable without anyone setting it |
| Read and write bits enforced | **yes** | in espix's root VFS, so builtins, loaded apps and SFTP are all checked the same way |
| `chown`, `chgrp`, owner and group | **yes** | stored per file, plus a rule so an unstamped rootfs still answers |
| setuid, setgid, sticky | **yes** | each consulted; `/tmp` is `1777` and sticky is what makes that safe |
| Symlinks, `ln` | **no** | cost, not principle: LittleFS has no link type, and following one means loop detection in every path lookup |

### Networking

| | | |
|---|---|---|
| WiFi station, DHCP lease, default route | **yes** | `wlan0`, reconnects on boot |
| `ip`, `ifconfig`, `route`, `ping` | **yes** | `ping` resolves names |
| SSH server | **yes** | password auth — [read this first](../README.md#a-word-on-the-ssh-server) |
| `scp` / `sftp` | **yes** | SFTP subsystem, permission-checked like the shell; starts in your home |
| NFSv3 server | **yes** | the stick is a mount on a Linux or macOS client: the portmapper, mountd, nfsd and statd, an export writable unless it says `ro`, and an enforced squash that governs access rather than ownership — [NFS](NFS.md) |
| Ethernet | **yes** on S31 | `eth0` over RGMII, DHCP and the Ethernet-first route; the S3 has no wired peripheral |
| USB-NCM | **yes** | device role only: `usb0`, plug into a computer and it is an Ethernet adapter, `ssh esp@192.168.7.1` with no WiFi at all — [USB-NETWORKING](USB-NETWORKING.md) |
| USB host (storage) | **yes** | the OTG port's default role: a stick attaches on its own, `lsblk`/`blkid` report it, `mount sda1 /mnt` mounts its FAT volume, `lsusb` lists everything including hubs, `usbscan`/`usbprobe` claim by hand. Four device slots, for a hub; two FAT volumes at once, bounded by `CONFIG_FATFS_VOLUME_COUNT` — [USB-HOST](USB-HOST.md) |
| SSH publickey auth, rekeying | **planned** | a long session is dropped today |
| Apps on sockets — `socket` to `shutdown`, `poll`, `select`, `getaddrinfo` | **yes** | lwIP's BSD sockets. `select` and `poll` take any descriptor, and a handler ends them with EINTR; a signal that does nothing leaves them waiting, and a stop parks them. A U-mode app's (S31) blocking socket calls are cut short the same way, restarted under `SA_RESTART`, and a send to a gone peer raises SIGPIPE |
| `getifaddrs`, `syslog` | **yes** | the interfaces by the names `ip` prints, with their IPv4 and IPv6 addresses; `syslog` lands in the kernel log `dmesg` reads |
| Raw lwIP / `netconn` for the SSH transport | **planned** | BSD sockets today, deliberately: apps get the same API. Cut calls before changing API — one `send()` per packet instead of three was worth 1.7× |
| Time of day, over NTP | **yes** | `date`, `timedatectl`; server from DHCP option 42, else `pool.ntp.org` |

### Users

| | | |
|---|---|---|
| Password authentication | **yes** | PBKDF2-SHA256, per-user salt, `/etc/passwd` |
| `passwd`, `whoami`, `id` | **yes** | `passwd` refuses to change another account's, unless root |
| More than one account | **yes** | `useradd` allocates a free uid; 8 accounts and 12 groups |
| uid/gid and file ownership | **yes** | stored per file on the rootfs, plus a rule so an unstamped rootfs still answers; a volume that keeps none — FAT — reports the owner it was mounted as |
| Enforced read/write/execute | **yes** | in espix's VFS, for builtins and loaded apps alike |
| `chown`, `chgrp` | **yes** | changing an owner is root's, as in chown(2) |
| `sudo`, `sudo -u <user>` | **yes** | gated by `/etc/sudoers`, which takes names or `%group`; does not re-prompt, see below |
| `su` | **no** | `sudo` covers the need, and `su` wants the password prompt espix cannot give |
| Groups with members | **yes** | `/etc/group`, supplementary membership, and the group triad actually checked |
| `useradd`, `userdel`, `usermod` | **yes** | `-r` for a service account: locked, low uid, no home |
| `groupadd`, `groupdel`, `groups` | **yes** | |
| A root for an app, `confine <dir>` | **yes** | it cannot *name* a path outside, which is the question permissions never ask |
| Restricting what an app may call | **partial** | the ELF loader's export table is one, but it is fixed rather than per-app |
| An editor | **no** | no `nano` or `ed`, so editing a config on the device means `echo >` |

Those two rows were one row saying "per-app capabilities", which conflated a
*filesystem view* with a *subset of what an app may call*. They are different
things, and only the first is done.

**Why a root, when the app already runs as its own user?** Because users answer
"may uid X open path P" and never stop P being named. The mode rule here hands
out `0755` directories and `0644` files, so a service account can walk the whole
tree and read all of it bar what someone remembered to lock — and the two things
nobody had remembered were `/etc/wifi.conf`, holding the WiFi PSK, and the SSH
host private key, both world-readable until this landed. Discretionary
permissions start out open and are closed by exception; a root is the other
default, where nothing is reachable but what was handed over. It also survives
getting the uid wrong, and it is per *app* rather than per *user*, which two
services sharing an account cannot otherwise be.

It restricts rather than chroots: paths stay globally absolute, so the app sees
`/srv/www/db` and not `/db`. A real chroot needs bind mounts to be worth
anything — a jail with no `/bin`, no `/etc` and no `/tmp` is not somewhere a
program runs — and mounts are still on the roadmap. The binary is read before
the confinement starts, exactly as `execve` does it, so it may live outside.

Worth being clear about what any of this can buy on a chip with no MMU: an app
shares the address space with the kernel, so filesystem permissions and a root
alike are a guardrail against mistakes rather than a sandbox around hostile
code. The real boundary is the ELF loader's export table — an app can only call
what espix publishes to it. Permissions make that boundary usable; they do not
replace it. The same caveat applies to setuid, which is implemented because the
S31 makes it a real boundary rather than because it is one on the S3.

Root follows the model Debian uses: the account exists but is locked, so nothing
can log in as it, and `sudo` is how you reach it. `sudo passwd root <pw>` gives
it a password if you want one, and `passwd -l root` takes it away again.
`/etc/sudoers` is seeded with `%sudo`, so membership of that group is what
grants it — the arrangement Debian ships, where RHEL would say `%wheel`.

Services get their own identity rather than running as whoever started them:
`useradd -r www` makes a locked account with a low uid and no home, and
`sudo -u www /bin/httpd &` runs the app as it. Add `confine` and it gets its own
view of the filesystem as well — `sudo -u www confine /srv/www /bin/httpd &` is
an account that owns nothing else and a process that can see nothing else. No
service manager is involved — that is the whole mechanism.

**`sudo` does not ask for your password.** espix cannot read input without
echoing it — the same limitation that makes `passwd` take the password as an
argument — so a prompt would print the thing it was protecting. The session is
already authenticated, and sudo(8)'s timestamp caching means a real one often
does not re-ask either, but an unattended terminal is a way in that Linux would
have closed. It is the first thing to fix when the reentrant line editor lands.

### Syscalls

<!-- syscalls.py: begin -->
53 calls take Linux's rv32 numbers, with Linux's arguments and meaning (flag, errno and structure values are picolibc's and lwIP's); 46 are espix's own, from 2048. Generated by `tools/syscalls.py --write` from `components/espix_proc/include/espix_syscall.h`.

| call | number | Linux's | arguments |
|---|---|---|---|
| `getcwd` | 17 | the same | (buf, size): the length with its NUL |
| `dup` | 23 | the same | (fd) |
| `fcntl` | 25 | `fcntl64` | (fd, cmd, arg) |
| `ioctl` | 29 | the same | (fd, FIONBIO or FIONREAD, int *) |
| `truncate` | 45 | `truncate64` | (path, len_lo, len_hi) |
| `ftruncate` | 46 | `ftruncate64` | (fd, len_lo, len_hi) |
| `chdir` | 49 | the same | (path) |
| `close` | 57 | the same | (fd) |
| `pipe2` | 59 | the same | (int fds[2], O_NONBLOCK \| O_CLOEXEC) |
| `read` | 63 | the same | (fd, buf, len) |
| `write` | 64 | the same | (fd, buf, len) |
| `pread` | 67 | `pread64` | (fd, buf, len, off_lo, off_hi) |
| `pwrite` | 68 | `pwrite64` | (fd, buf, len, off_lo, off_hi) |
| `fsync` | 82 | the same | (fd) |
| `exit` | 94 | `exit_group` | (status) |
| `sched_yield` | 124 | the same | () |
| `kill` | 129 | the same | (pid, sig) |
| `sigreturn` | 139 | `rt_sigreturn` | (): a handler's restorer; its frame is at sp |
| `setpgid` | 154 | the same | (pid, pgid) |
| `getpgid` | 155 | the same | (pid) |
| `getsid` | 156 | the same | (pid) |
| `setsid` | 157 | the same | (): the new session's id |
| `umask` | 166 | the same | (mask): the old one |
| `getpid` | 172 | the same | () |
| `getppid` | 173 | the same | () |
| `getuid` | 174 | the same | () |
| `geteuid` | 175 | the same | () |
| `getgid` | 176 | the same | () |
| `getegid` | 177 | the same | () |
| `gettid` | 178 | the same | () |
| `socket` | 198 | the same | (domain, type \| SOCK_NONBLOCK \| SOCK_CLOEXEC, protocol) |
| `bind` | 200 | the same | (fd, addr, len) |
| `listen` | 201 | the same | (fd, backlog) |
| `connect` | 203 | the same | (fd, addr, len) |
| `getsockname` | 204 | the same | (fd, addr, socklen_t *len) |
| `getpeername` | 205 | the same | (fd, addr, socklen_t *len) |
| `sendto` | 206 | the same | (fd, buf, len, flags, to or 0, tolen) |
| `recvfrom` | 207 | the same | (fd, buf, len, flags, from or 0, socklen_t *fromlen) |
| `setsockopt` | 208 | the same | (fd, level, name, val, len) |
| `getsockopt` | 209 | the same | (fd, level, name, val, socklen_t *len) |
| `shutdown` | 210 | the same | (fd, how) |
| `brk` | 214 | the same | (addr): the break, moved there if it could be |
| `munmap` | 215 | the same | (addr, len) |
| `execve` | 221 | the same | (path, argv, envp): returns only with an error |
| `mmap` | 222 | `mmap2` | (addr, len, prot, flags, fd, offset in pages) |
| `mprotect` | 226 | the same | (addr, len, prot) |
| `msync` | 227 | the same | (addr, len, flags) |
| `mincore` | 232 | the same | (addr, len, uint8_t *vec): 1 a page resident |
| `madvise` | 233 | the same | (addr, len, advice) |
| `accept` | 242 | `accept4` | (fd, addr or 0, socklen_t *len, SOCK_* flags) |
| `getrandom` | 278 | the same | (buf, len, flags) |
| `clock_gettime` | 403 | `clock_gettime64` | (clock, struct espix_timespec *) |
| `futex` | 422 | `futex_time64` | (uaddr, op, val, const struct espix_timespec *) |
| `open` | 2048 | espix's own | (path, flags, mode) |
| `lseek` | 2049 | espix's own | (fd, off_lo, off_hi, whence, uint64_t *pos) |
| `fstat` | 2050 | espix's own | (fd, struct espix_stat *) |
| `stat` | 2051 | espix's own | (path, struct espix_stat *) |
| `unlink` | 2052 | espix's own | (path) |
| `rename` | 2053 | espix's own | (from, to) |
| `mkdir` | 2054 | espix's own | (path, mode) |
| `rmdir` | 2055 | espix's own | (path) |
| `utime` | 2056 | espix's own | (path, const struct espix_utimbuf *) |
| `chmod` | 2057 | espix's own | (path, mode) |
| `dup2` | 2058 | espix's own | (fd, newfd) |
| `getdents` | 2059 | espix's own | (fd, buf, len): struct espix_dirent records |
| `isatty` | 2060 | espix's own | (fd) |
| `nanosleep` | 2061 | espix's own | (const struct espix_timespec *, rem) |
| `getpgrp` | 2062 | espix's own | () |
| `thread_create` | 2063 | espix's own | (entry, arg, tls, stack or 0, tid word or 0): the tid |
| `thread_exit` | 2064 | espix's own | (): its tid word is zeroed and woken |
| `getcpu` | 2065 | espix's own | (): the core the caller runs on |
| `sigaction` | 2066 | espix's own | (sig, const struct espix_sigaction *, old) |
| `sigprocmask` | 2067 | espix's own | (how, const uint32_t *set, uint32_t *old): the thread's |
| `sigpending` | 2068 | espix's own | (uint32_t *): pending and blocked |
| `sigsuspend` | 2069 | espix's own | (const uint32_t *mask): -EINTR once a handler is due |
| `raise` | 2070 | espix's own | (sig): to the calling thread |
| `setitimer` | 2071 | espix's own | (which, const struct espix_itimerval *, old) |
| `term_size` | 2072 | espix's own | (int *cols, int *rows) |
| `poll` | 2073 | espix's own | (struct pollfd *, nfds, ms or -1) |
| `getaddrinfo` | 2074 | espix's own | (node, service, hints, out, max): a count, -errno or -EAI_* |
| `gethostname` | 2075 | espix's own | (buf, len) |
| `getpw` | 2076 | espix's own | (name or 0, uid, struct espix_pwent *, len) |
| `getgr` | 2077 | espix's own | (name or 0, gid, struct espix_pwent *, len) |
| `syslog` | 2078 | espix's own | (priority, ident or 0, message): to the kernel log |
| `getifaddrs` | 2079 | espix's own | (struct espix_ifaddr *, max): how many |
| `gfx_open` | 2080 | espix's own | (full_w, full_h): claims the screen; 0,0 keeps its size |
| `gfx_close` | 2081 | espix's own | () |
| `gfx_surface` | 2082 | espix's own | (w, h, struct espix_gfx_fb *): a handle; 0,0 is the screen's size |
| `gfx_free` | 2083 | espix's own | (handle): its pixels are unmapped |
| `gfx_present` | 2084 | espix's own | (handle, x, y, w, h): w 0 scales it to fill the screen |
| `gfx_poll` | 2085 | espix's own | (struct espix_input *): 1 with an event, 0 without |
| `audio_open` | 2086 | espix's own | (const struct espix_audio_fmt *): a handle |
| `audio_write` | 2087 | espix's own | (handle, buf, len): bytes taken, blocking while the ring is full |
| `audio_ctl` | 2088 | espix's own | (handle, ESPIX_AUDIO_*, arg) |
| `audio_close` | 2089 | espix's own | (handle): what is queued plays out |
| `meminfo` | 2090 | espix's own | (struct espix_meminfo *) |
| `wait4` | 2091 | espix's own | (pid, int *status or 0, WNOHANG \| WUNTRACED): a child's pid, 0 for none yet |
| `fork` | 2092 | espix's own | (): the child's pid, and 0 in the child |
| `vfork` | 2093 | espix's own | (): fork, the child borrowing the caller's memory and the caller waiting until it execs or ends |
<!-- syscalls.py: end -->
