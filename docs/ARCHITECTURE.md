# espix architecture

Notes on how the skeleton is put together and why. Design decisions that are
non-obvious from the code, and the ones that were verified against ESP-IDF
rather than assumed.

Target of record for this skeleton: **ESP32-S3, 16MB flash, 8MB octal PSRAM**,
on **ESP-IDF v6.1-beta1**.

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
espix_cmds       the actual commands
    ↑
  main           app_main = the init sequence, nothing else
```

`espix_net` sits outside this — a header-only placeholder until SSH work
begins.

## Decisions worth knowing

### LittleFS is mounted as the real root `/`

`esp_vfs_littlefs_register()` is called with `base_path = ""`. ESP-IDF
documents the empty base path as registering a *fallback* VFS that handles any
path no other VFS claims, so paths are `/bin/hello` and `/etc/motd` rather than
`/storage/bin/hello`, while `/dev/*` still resolves to its own driver by
longest-prefix match.

This is documented but not a well-trodden path. If it misbehaves, the fallback
is to mount at `/storage` and add a rewrite shim — contained to `espix_fs`.

### espix does not use `esp_console_run()`

The console component copies every command line through a single shared
`static char *s_tmp_line_buf` (`components/console/commands.c`), so it is not
reentrant. With one console session that is invisible; with a console plus an
SSH session it is corruption. espix therefore keeps its own registry and
dispatch (`espix_shell_exec`) and reuses only the reentrant pieces of that
component:

- `esp_console_split_argv()` — operates on the caller's buffer
- linenoise — line editing, history, completion
- argtable3 — available, not yet used (its arg structs are per-command
  statics, the same reentrancy trap; adopting it needs per-invocation
  allocation)

`esp_console_cmd_t.func_w_context` does not help: context is bound at
registration, not per invocation.

A session is `{read_line, write, cwd, ...}`. The console implements it over
UART/USB-Serial-JTAG; an SSH channel will implement the same two callbacks.
linenoise itself keeps global state and reads the calling task's stdin, so
exactly one console session can exist — which is why SSH sessions will supply
their own `read_line` rather than sharing this one.

Redirection (`>` / `>>`) is handled in dispatch, not per command, by setting
`session->redirect` for the duration of the call. It therefore works for every
command. It does *not* capture a spawned app's own stdout.

### There is no working directory outside a session

ESP-IDF has no process-wide CWD at all: `chdir()` is a hardcoded
`errno = ENOSYS; return -1;` stub and `getcwd()` unconditionally answers `"/"`
(`components/esp_libc/src/realpath.c`). espix's cwd therefore lives only in
`espix_session_t.cwd`, and every command resolves through `espix_fs_resolve()`
to an absolute path before touching the filesystem.

The consequence lands on the app ABI: **a loaded app that calls
`fopen("data.txt")` gets `/data.txt`**, never the directory you `cd`'d into.
`run` resolves its own `argv[0]` against the session cwd, so
`cd /home && run ./app` works, but an ambient cwd is not something apps can rely
on. Apps should take paths as arguments. If this becomes painful, the fix is
`--wrap` on `getcwd`/`chdir` backed by a per-task cwd and exported through the
ELF symbol table — deliberately not done yet, because it is an addition to the
app ABI and wants designing rather than bolting on.

### Apps are entered directly, not via esp_elf_request()

`espix_proc` calls `elf.entry(argc, argv)` itself. `esp_elf_request()` is
literally `elf->entry(argc, argv); return 0;` — it discards the app's return
value, so an exit status could never reach the shell. [exec.c](../components/espix_proc/exec.c)
replicates its only other behaviour (a NULL entry check) and keeps the result.

### The current session lives in FreeRTOS TLS

Command functions may run in a task other than the session's, so
`espix_shell_current()` reads TLS slot `ESPIX_TLS_SESSION_IDX` (1 — index 0 is
taken by ESP-IDF's pthread implementation). Hence
`CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS=2`.

### Fault handling intercepts, but does not yet recover

`-Wl,--wrap=esp_panic_handler`, applied from `espix_fault/CMakeLists.txt`. This
is the same seam ESP-IDF's own test suite uses. Two details matter:

- The component must be registered `WHOLE_ARCHIVE`. `__wrap_esp_panic_handler`
  is referenced only by the linker, so otherwise the object is dropped and the
  wrap silently does nothing.
- The handler reads task names and the process table, which needs flash cache,
  so `CONFIG_ESP_PANIC_HANDLER_IRAM` must stay off. There is an `#error`
  guarding that.

Today the hook records core / exception / faulting address / task name / pid
into a `__NOINIT_ATTR` struct that survives the reset, prints one line, and
delegates to the real handler. The next boot reports it in `dmesg`.

It deliberately does *not* reap and resume. See the comment block at the top of
`reaper.c` for the three things that block that: locks held by the dead task,
no per-process ownership of heap and fds, and the fact that without an MMU most
corruption never reaches the fault handler at all.

### The ELF loader needs memory protection disabled

`CONFIG_ESP_SYSTEM_MEMPROT_FEATURE=n` (and `ESP_SYSTEM_PMP_IDRAM_SPLIT=n` on
the RISC-V targets). The loader writes a relocated image into memory and then
executes it, which is precisely what that feature exists to prevent.
Espressif's own elf_loader examples do the same. It is also, bluntly, part of
why apps are not isolated from the kernel.

`espix_proc` reads app files itself rather than calling `esp_elf_open()`, which
only resolves names relative to `CONFIG_ELF_FILE_SYSTEM_BASE_PATH`; espix needs
to run an ELF at any absolute path the shell hands it.

### libc is pinned to newlib

IDF 6.1 defaults to picolibc. Under picolibc `stdin`/`stdout` are thread-local
and linenoise depends on a TLS-stdio shim in `components/console/private_include`,
which cannot be included from outside that component. Newlib also matches what
elf_loader's libc symbol table — how a loaded app resolves `printf` against the
firmware — was written against.

### The rootfs image is not flashed by `idf.py flash`

`littlefs_create_partition_image(storage fsroot)` without `FLASH_IN_PROJECT`.
With it, every firmware flash would rewrite the filesystem and destroy anything
created on the device. Flashing the rootfs is an explicit `idf.py storage-flash`.

## Two config traps, both hit once already

Worth knowing before editing `sdkconfig.defaults` or a component `Kconfig`:

- **`sdkconfig.defaults` only seeds a *new* `sdkconfig`.** It does not override
  a value already present in the generated file. Changing a default that has
  already been baked in requires deleting `sdkconfig` and re-running
  `idf.py reconfigure`, or editing the generated file directly.
- **An undefined bool is indistinguishable from `n`.** A `default y` Kconfig
  option that fails to regenerate silently compiles to its off path with no
  warning. espix therefore names boolean options as opt-*outs* where it can —
  `ESPIX_KLOG_QUIET` (default `n`) rather than `ESPIX_KLOG_ECHO_CONSOLE`
  (default `y`) — so a missing symbol yields the behaviour we wanted anyway.

Related: keep `CONFIG_LOG_MAXIMUM_LEVEL` at INFO. It is a compile-time ceiling,
not just a runtime permission — `espcoredump`'s macros test `LOG_LOCAL_LEVEL`
at compile time and write via `esp_rom_printf`, bypassing the runtime level
entirely, so a DEBUG ceiling buries every crash under a page of tracing (and
costs ~6 KB of format strings).

## Deferred

- **OTA slots.** The partition table is `factory`-only. Two 4MB OTA slots plus
  `otadata` would cost ~4MB of the 11.9MB rootfs but allow kernel updates over
  the network. Changing this later means reflashing everything, so it is worth
  deciding before the layout is in the field. A commented-out variant is in
  `partitions.csv`.
- **Per-app heap arenas.** The allocation path in `espix_proc` is the seam.
  Would shrink the blast radius of a crashing app without needing an MMU.
- **A real `top`.** `ps` reports cumulative CPU share since boot; an
  instantaneous reading needs two samples.
- **A working directory for apps** — see the app ABI note above.
- **`/etc/motd`.** The file exists in the rootfs but nothing reads it; the
  console prints a fixed banner instead. Printing motd at session start is the
  right home for it, and worth doing when SSH makes "logging in" a real event.
- **`espix_net`**, and SSH/SCP on top of it.
