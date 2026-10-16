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

Both targets build from these sources -- 208 KB for esp32s31, 232 KB for
esp32s3. The S31 has been run on hardware (a client connects, subscribes and
gets its message back); the S3 is build- and ABI-verified, and is where
`__bswapsi2` came from: Xtensa calls libgcc for a byte swap that RISC-V with
Zbb inlines, so the S31 app never asked for it and the S3 app did.

## The point of this port

It is an ABI exercise. The broker is a real Unix daemon -- sockets, `poll()`,
`getaddrinfo()`, a single-threaded event loop, file I/O, config parsing,
timers -- and it was taken **unmodified**: not one line of mosquitto is
patched, and none is vendored. What had to be added is the answer to "what is
the app ABI still missing".

The result is lopsided on purpose. The broker's own `main()` **is** the app
entry (`project_elf()` compiles with `-Dmain=app_main`), so there is no
wrapper there. Everything that could be honest on espix is a kernel call the
broker already makes. What is left is `components/mosquitto_espix/`:

* 7 header shims, 158 lines in total -- `<poll.h>`, `<syslog.h>`,
  `<sys/syslog.h>`, `<dlfcn.h>`, `<ifaddrs.h>`, a `<cjson/cJSON.h>` case fix
  and one force-included `espix_compat.h`, none of which newlib or lwIP ships;
* `espix_platform.c`, 214 lines and 18 definitions, each one a Unix host
  facility a single-user, no-fork, no-shared-object device cannot have:

| the broker asks | espix answers |
|---|---|
| `getpwnam`, `getpwuid`, `getpwuid_r`, `getgrgid_r` | **espix's own now**, from `espix_auth` -- see below |
| `setuid`, `setgid`, `setgroups`, `initgroups` | no process credential is writable yet, and an app already runs as its user |
| `fork`, `setsid`, `umask` | there is no process to fork or detach |
| `dlopen`, `dlsym`, `dlclose`, `dlerror` | an app is the loadable object; no loader inside it |
| `openlog`, `syslog`, `vsyslog`, `closelog` | there is one log: the app's stderr |
| `getifaddrs`, `freeifaddrs` | no interface list is published to apps |
| `gai_strerror` | lwIP has the EAI_* codes but not the text |

## What the exercise put into espix

The eight names the app needed that espix did not publish are espix's now, each
in the file that owns the surface, and the app's undefined-symbol list holds
nothing the kernel cannot answer:

| name | where | how |
|---|---|---|
| `poll` | `espix_proc/abi_drivers.c` | a wrapper over the interruptible `select()`. IDF's own `poll()` calls the **libc** select, which a signal cannot break, so a broker whose loop is a poll could not be stopped. |
| `getrandom` | `espix_proc/abi_libc.c` | `ESP_ELFSYM_EXPORT`, which is also what links it |
| `freopen` | `espix_proc/abi_libc.c` | `ESP_ELFSYM_EXPORT`; it reaches the same VFS `fopen` does |
| `__floatdidf`, `__moddi3`, `__unorddf2` | `espix_proc/abi_drivers.c` | named beside the other libgcc helpers |
| `gethostname` | `espix_net/abi.c` | a wrapper over `espix_net_hostname()`, so an app sees what `hostname` prints |
| `sysconf` | `espix_proc/abi_libc.c` | `_SC_OPEN_MAX` (which is `FD_SETSIZE`), `_SC_CLK_TCK`, `_SC_NPROCESSORS_ONLN`, `_SC_PAGESIZE` |
| `getpwnam`, `getpwuid`, `getpwnam_r`, `getpwuid_r`, `getgrnam`, `getgrgid`, `getgrnam_r`, `getgrgid_r` | `espix_auth/abi_user.c` | over the account model that was already there -- espix had the users, `useradd` through `chgrp`, and no libc door onto them |

The measurement, after the additions: **111 undefined symbols, 0 not published
by espix**.

## What the exercise turned up

**The POSIX name is not always the symbol.** ESP-IDF builds lwIP with
`LWIP_COMPAT_SOCKETS=0`, so `<sys/poll.h>` declares `poll()` and lwIP's
`lwip_poll()` is a *different* function -- sockets only. An earlier version of
the `<poll.h>` shim aliased `poll` to `lwip_poll`, which compiles and then
quietly ignores every non-socket descriptor. What an app asks for has to be
what espix publishes, and for `poll` that is esp_libc's.

**`CONFIG_LWIP_MAX_SOCKETS` is an ABI constant, and it is now in
`tools/app-abi.defaults`.** lwIP numbers its sockets from `LWIP_SOCKET_OFFSET`,
which is `FD_SETSIZE - CONFIG_LWIP_MAX_SOCKETS`, and the kernel's `select()`
and `poll()` translate descriptors with *its* value. An app's headers can also
take `FD_SETSIZE` from the same number, but only when lwIP's `sockets.h`
reaches the `FD_SET` definition before newlib's does -- and then an app built
with IDF's default of 10 passes a word-sized `fd_set` to a kernel filling in
one sized for 40. Same class as `CONFIG_FREERTOS_HZ`: the two builds have to
say the same number.

## What is off

No TLS, no websockets, no unix sockets, no epoll/kqueue, no http_api listener,
no plugins loaded from disk, no systemd/DLT/libwrap. The built-in password-file
and acl-file security checks are compiled in and do work. See the feature list
and its reasoning in `components/mosquitto/CMakeLists.txt`.
