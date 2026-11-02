# Mosquitto on espix

[Eclipse Mosquitto](https://mosquitto.org/), the reference MQTT broker, built as
a standalone espix app. The shape it is for: an MQTT **edge** broker on the
board -- a small, always-on aggregator for sensors on a LAN that bridges to a
larger upstream broker or cluster.

    ./fetch-mosquitto.sh                            # upstream 2.1.2, pinned
    idf.py -G 'Unix Makefiles' set-target esp32s31
    idf.py elf                                      # build/mosquitto.app.elf
    scp build/mosquitto.app.elf esp@<host>:/bin/mosquitto

Then, on the board:

    mosquitto -c /etc/mosquitto.conf

Copying the binary by hand has one trap: sftp lands it mode 0644, so after
`sudo mv /tmp/mosquitto /bin/mosquitto` it needs `sudo chmod 755`, or the
shell refuses to run it and nothing says why. `make flash-fs` and a release
image keep the mode the staging step gave it.

A configuration is required to listen beyond loopback: since 2.0 an empty
configuration binds only 127.0.0.1. The minimum that works is

    listener 1883 0.0.0.0
    allow_anonymous true

Both targets build from these sources. On the S31, `tools/build-apps.sh` also
builds it as a U-mode app, in an address space of its own, and stages it as
`/bin/mosquitto-u` beside the M-mode `/bin/mosquitto` (208 KB; the U-mode one
is 271 KB, its libc linked in): the same sources and the same seam, compiled by
`tools/espix-cc` through `uapp/CMakeLists.txt`. `sources.cmake` is the one
source list both builds use.

## The point of this port

It is an ABI exercise. The broker is a real Unix daemon -- sockets, `poll()`,
`getaddrinfo()`, a single-threaded event loop, file I/O, config parsing,
timers, syslog -- and it was taken **unmodified**: not one line of mosquitto is
patched, and none is vendored.

The broker's own `main()` **is** the app entry (`project_elf()` compiles with
`-Dmain=app_main`; a U-mode app's crt0 calls `main()`), so there is no wrapper
there. What espix does not have yet is `components/mosquitto_espix/`:
`espix_platform.c`, seven definitions, and a `<cjson/cJSON.h>` case fix for the
registry component's layout.

| the broker asks | the seam answers |
|---|---|
| `setuid`, `setgid`, `setgroups`, `initgroups` | "already that user": no process credential is writable yet, and an app runs as the user that started it |
| `fork`, `setsid`, `umask` | `fork` fails, so `-d` does not half-work; there is no umask yet |

Everything else is espix's, the same in both builds: the M-mode ABI tables for
an M-mode app, libespix and syscalls for a U-mode one.

| name | M-mode | U-mode |
|---|---|---|
| sockets, `poll`, `select` | lwIP's, and `espix_proc/abi_drivers.c`'s interruptible `select` and `poll` | `umode_net.c`, the same wait |
| `getaddrinfo`, `gai_strerror` | lwIP's, and the text from `espix_net/abi.c` | `GETADDRINFO`, over lwIP's |
| `getifaddrs` | `espix_net/abi.c`, from the interface table: the names `ip` prints | `GETIFADDRS`, the same list |
| `openlog`, `syslog`, `vsyslog` | `espix_proc/abi_posix.c`, into the kernel log `dmesg` reads | `SYSLOG`, the same log |
| `dlopen` and the rest | fail with "Dynamic loading not supported", as a static musl build does | the same, in libespix |
| `getpwnam`, `getpwuid`, `getgrgid_r`... | `espix_auth/abi_user.c`, over the account model | `GETPW`, `GETGR` |
| `getrandom`, `gethostname`, `sysconf` | `abi_libc.c`, `espix_net/abi.c` | syscalls, and libespix |

The headers IDF lacks (`<syslog.h>`, `<ifaddrs.h>`, `<dlfcn.h>`, `<poll.h>`)
are espix's, in `user/include/common`, which both builds put on the include
path (`cmake/espix-app.cmake`, `tools/espix-cc`).

## What the exercise turned up

**The POSIX name is not always the symbol.** ESP-IDF builds lwIP with
`LWIP_COMPAT_SOCKETS=0`, so `<sys/poll.h>` declares esp_libc's `poll()`, and
lwIP's `lwip_poll()` is a different function that sees sockets only.

**`CONFIG_LWIP_MAX_SOCKETS` is an ABI constant, and it is in
`tools/app-abi.defaults`.** lwIP numbers its sockets from `LWIP_SOCKET_OFFSET`,
which is `FD_SETSIZE - CONFIG_LWIP_MAX_SOCKETS`, and the kernel's `select()`
and `poll()` translate descriptors with *its* value. An app's headers can also
take `FD_SETSIZE` from the same number, but only when lwIP's `sockets.h`
reaches the `FD_SET` definition before newlib's does -- and then an app built
with IDF's default of 10 passes a word-sized `fd_set` to a kernel filling in
one sized for 40. Same class as `CONFIG_FREERTOS_HZ`: the two builds have to
say the same number. A U-mode app has its own descriptor table and never meets
it.

## What is off

No TLS, no websockets, no unix sockets, no epoll/kqueue, no http_api listener,
no plugins loaded from disk (elf_loader has a `dlopen`, but a plugin's calls
back into the broker would not resolve), no systemd/DLT/libwrap. The built-in password-file
and acl-file security checks are compiled in and do work. See the feature list
and its reasoning in `components/mosquitto/CMakeLists.txt`.
