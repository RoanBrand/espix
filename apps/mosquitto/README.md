# Mosquitto on espix

[Eclipse Mosquitto](https://mosquitto.org/), the reference MQTT broker, built as
a standalone espix app. The shape it is for: an MQTT **edge** broker on the
board -- a small, always-on aggregator for sensors on a LAN that bridges to a
larger upstream broker or cluster.

    cmake -S apps/mosquitto -B build -DCMAKE_TOOLCHAIN_FILE=cmake/espix-uapp.cmake
    cmake --build build                               # build/mosquitto; add -DESPIX_TARGET=esp32s3 for the S3
    scp build/mosquitto esp@<host>:/bin/mosquitto

Configuring runs `fetch-mosquitto.sh`, which pins upstream 2.1.2 and cJSON 1.7.19
into `third_party/`. `tools/build-apps.sh` does all of this for both targets.

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

It is a POSIX app, compiled by `tools/espix-cc`: on the S31 it runs in U-mode,
in an address space of its own (271 KB, its libc linked in); on the S3 without an
MMU (230 KB, sharing `/lib/libc.so`), where `-d` fails as `fork` does on a Linux
without an MMU. `sources.cmake` is its source list.

## The point of this port

It is an ABI exercise. The broker is a real Unix daemon -- sockets, `poll()`,
`getaddrinfo()`, a single-threaded event loop, file I/O, config parsing,
timers, syslog -- and it was taken **unmodified**: not one line of mosquitto is
patched, and none is vendored.

The broker's own `main()` is the entry, so there is no wrapper. What espix
does not have yet is `components/mosquitto_espix/`: `espix_platform.c`, which
answers `setuid`, `setgid`, `setgroups` and `initgroups` with "already that
user" (no process credential is writable yet, and an app runs as the user that
started it), and a `<cjson/cJSON.h>` include fix for cJSON's layout. Everything
else (sockets, `poll`, name lookup, `getifaddrs`, `syslog`, the user database,
`fork` and `setsid` for `-d`) is libespix's syscalls.

## What is off

No TLS, no websockets, no unix sockets, no epoll/kqueue, no http_api listener,
no plugins loaded from disk (elf_loader has a `dlopen`, but a plugin's calls
back into the broker would not resolve), no systemd/DLT/libwrap. The built-in password-file
and acl-file security checks are compiled in and do work. See the feature list
in `sources.cmake`.
