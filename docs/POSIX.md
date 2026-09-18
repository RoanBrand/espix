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
| Output redirection `>` `>>` | **yes** | |
| Error redirection `2>` `2>>` `2>&1` | **yes** | diagnostics are their own stream; over SSH they arrive as `CHANNEL_EXTENDED_DATA`, so the client's `2>` separates them too |
| Quoting and backslash escapes | **yes** | |
| Exit status | **yes** | `exit 3` reaches an SSH client's `$?` |
| Background jobs `&` | **partial** | `cmd &` works, but dies at logout; no `jobs`, `fg`, `bg`, Ctrl-Z |
| Pipes <code>&#124;</code> | **planned** | |
| Input redirection `<` | **planned** | a loaded app has a real `stdin` (`ssh host 'prog' < file` works); no builtin reads it, and there is no `<` |
| Environment variables, `env`, `export`, `unset` | **yes** | a per-session table; `PATH` is consulted when running a program, and an app gets `getenv`, `setenv`, `unsetenv` and `putenv` |
| Globbing `*` | **planned** | |
| Shell scripts, `#!`, control flow | **planned** | the executable bit is now real, so `#!` needs only the dispatch: a file that is executable but not an ELF is where the interpreter line would be read |

### Processes

| | | |
|---|---|---|
| Run a cross-compiled native app by name | **yes** | `/bin` search, argv, exit status |
| `ps`, `top` | **yes** | live CPU and memory, per core |
| Stop a running app — Ctrl-C, `kill` | **yes** | Ctrl-C is SIGINT; `kill` asks, then insists |
| Signals and handlers — `signal()`, `kill -9`, `-STOP`/`-CONT` | **yes** | real POSIX names; delivered when the app calls in, not asynchronously |
| `kill -l`, `ps` showing `T` for stopped | **yes** | |
| A crashing app not taking the system down | **planned** | intercepts and reports; does not yet reap |
| `grep`, `sed`, `head`, `tail`, `wc`, `sort`, `find` | **planned** | |
| `sleep` | **planned** | |
| Apps using the filesystem | **yes** | `fopen`, `opendir`, `stat`, `chmod`; `stat` reports the same mode and owner `ls -l` shows |
| Per-process working directory | **yes** | an app's `chdir()` does not move the shell that ran it |
| `fork()` / `exec()` | **no** on S3, **planned** on S31 | needs an MMU for copy-on-write; the S31 has one |
| MMU-backed process isolation | **no** on S3, **planned** on S31 | see [hardware targets](../README.md#hardware-targets) and [crash handling](#crash-handling-and-isolation) |
| setuid / setgid / sticky | **yes** | all three consulted; setuid is a guardrail on S3 and a boundary on S31 |

### Filesystem

| | | |
|---|---|---|
| LittleFS mounted as the real `/` | **yes** | survives reboot and firmware reflash |
| `ls` `cd` `pwd` `cat` `cp` `mv` `rm` `rmdir` `mkdir` `touch` `chmod` `df` | **yes** | |
| `ls -1adhiltr` | **yes** | sorted by name, or by mtime with `-t`; `-d` describes an entry instead of listing it; `-R` is not implemented |
| `ls -i`, inode numbers | **no** | esp_littlefs reports `d_ino = 0` for every entry, and LittleFS exposes no file id |
| Per-session working directory | **yes** | your `cd` is not someone else's |
| File timestamps | **yes** | `ls -l` and `sftp ls -l` show mtime; files from the flashed image have none |
| `/proc` | **planned** | the one part of espix's own mount table still missing; a second mount now exists |
| `mount`, `umount` | **yes** | `mount sda1 /mnt` puts a FAT32/FAT16 volume from a USB device into the namespace, reached through espix's own VFS so the permission check applies to it. Root only; nothing is ever formatted; unplug while mounted is a gap; `mount -o uid=,gid=` hands the volume to a user without root, and `-o ro` mounts it read-only — [USB-HOST](USB-HOST.md#stage-2--mounting) |
| `/etc/fstab` | **yes** | applied on attach and undone on removal: the device column takes a name, a wildcard, or an identity from `blkid` (`LABEL=`, `UUID=`, `PARTUUID=`), and the owner named becomes the volume's owner — [USB-HOST](USB-HOST.md) |
| `lsblk`, `blkid` | **yes** | USB storage is enumerated, identified and its partition table read — including the filesystems espix has no driver for, and disks with no partition table at all (a superfloppy's own volume is named). **Four device slots**, which is what a hub needs; two FAT volumes at once — [USB-HOST](USB-HOST.md) |
| Mode bits, `chmod` | **yes** | all twelve, octal or symbolic; `ls -l` and `sftp ls -l` show the same thing |
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
| SSH server | **yes** | password auth — [read this first](#a-word-on-the-ssh-server) |
| `scp` / `sftp` | **yes** | SFTP subsystem, permission-checked like the shell; starts in your home |
| Ethernet | **planned** | P4 and S31 (Original ESP32 also has) |
| USB-NCM | **yes** | device role only: `usb0`, plug into a computer and it is an Ethernet adapter, `ssh esp@192.168.7.1` with no WiFi at all — [USB-NETWORKING](USB-NETWORKING.md) |
| USB host (storage) | **yes** | the OTG port's default role: a stick attaches on its own, `lsblk`/`blkid` report it, `mount sda1 /mnt` mounts its FAT volume, `lsusb` lists everything including hubs, `usbscan`/`usbprobe` claim by hand. Four device slots, for a hub; two FAT volumes at once, bounded by `CONFIG_FATFS_VOLUME_COUNT` — [USB-HOST](USB-HOST.md) |
| SSH publickey auth, rekeying | **planned** | a long session is dropped today |
| Raw lwIP / `netconn` for the SSH transport | **planned** | BSD sockets today, deliberately: apps get the same API. Cut calls before changing API — one `send()` per packet instead of three was worth 1.7× |
| Time of day, over NTP | **yes** | `date`, `timedatectl`; server from DHCP option 42, else `pool.ntp.org` |

### Users

| | | |
|---|---|---|
| Password authentication | **yes** | PBKDF2-SHA256, per-user salt, `/etc/passwd` |
| `passwd`, `whoami`, `id` | **yes** | `passwd` refuses to change another account's, unless root |
| More than one account | **yes** | `useradd` allocates a free uid; 8 accounts and 12 groups |
| uid/gid and file ownership | **yes** | stored per file, plus a rule so an unstamped rootfs still answers |
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
