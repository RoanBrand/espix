# Defects in what espix builds on

Bugs and gaps in ESP-IDF and its managed components, each with the workaround
espix carries and enough detail to file a report. Kept separate from
[KNOWN-ISSUES.md](KNOWN-ISSUES.md) because the action is different: these are
someone else's to fix, and a future IDF release may remove the need for the
workaround — at which point it is useful to know exactly what the workaround was
for.

Verified against ESP-IDF v6.1 and xtensa-esp-elf GCC 15.2 unless noted.

For behaviour that is documented and merely surprising, rather than broken --
the platform differing from what a POSIX or FreeRTOS habit expects -- see
[GOTCHAS.md](GOTCHAS.md).

Re-checked against the v6.1 release: the beta1-to-v6.1 changelog adds no VFS
`chmod` hook, no `chmod()` fix, no LittleFS changes at all, no `adjtime()` fix,
and nothing touching the signal vocabulary or FreeRTOS-Plus-POSIX -- so every
entry below still stands and every workaround is still load-bearing.

## ESP-IDF

### The DHCP server always advertises itself as a DNS server

`dhcps_dns = 0` is documented as "do not offer DNS" and behaves as "offer
myself". In `components/lwip/apps/dhcpserver/dhcpserver.c`, the block commented
*"Add DNS option if either main or backup DNS is set"* has an `else` that emits
the option anyway, carrying the server's own address:

```c
} else {
    *optptr++ = DHCP_OPTION_DNS_SERVER;
    *optptr++ = 4;
    optptr = dhcps_option_ip(optptr, &ipadd);   /* the server's own IP */
}
```

So there is no value of `ESP_NETIF_DOMAIN_NAME_SERVER` that removes the option.
Setting it to 0 through `esp_netif_dhcps_option()` returns `ESP_OK`, clears
`OFFER_DNS` as documented, and changes nothing on the wire, because the cleared
flag simply selects the branch that hardcodes the server's address. Zeroing the
address with `esp_netif_set_dns_info()` does not help either — that is the other
half of the same `&&`, so it selects the same `else`.

Measured rather than read: with both calls made and both returning `ESP_OK`, a
macOS client still records

    domain_name_server (ip_mult): {192.168.7.1}

in `ipconfig getpacket`. The neighbouring router option *is* suppressible by the
same mechanism and is verifiably absent from the same packet, which is what
makes this a defect in one option rather than a misunderstanding of the API.

**Why it matters.** A DHCP server that is not a resolver should not name itself
as one. espix's USB-NCM link (`usb0` in server mode) hands a computer an address
and then tells it to resolve names at an address where nothing is listening.
The damage is limited — the client keeps its own default route and its own DNS,
and both Linux and macOS scope a resolver to the interface that supplied it —
but it is a pointer to a service that does not exist, and no caller can decline
it.

**espix's workaround: none, because there is none.** The router option is
suppressed and the DNS option is documented as unavoidable. Worth re-testing if
the DHCP server ever gains a real "offer nothing" value; `components/espix_net/usb_ncm.c`
says so at the point where the suppression would go.

### `esp_core_dump_image_check()` drives the SHA peripheral with no lock

`espcoredump` checksums with the **ROM** SHA on every target but the original
ESP32 (`components/espcoredump/src/core_dump_sha.c`):

```c
static void core_dump_sha256_start(core_dump_sha_ctx_t *sha_ctx)
{
    ets_sha_enable();                 /* resets the peripheral */
    ets_sha_init(&sha_ctx->ctx, SHA2_256);
}
...
static void core_dump_sha256_finish(core_dump_sha_ctx_t *sha_ctx)
{
    ets_sha_finish(&sha_ctx->ctx, sha_ctx->result);
    ets_sha_disable();                /* turns it off */
}
```

Those ROM calls take no lock. That is correct for the panic path they were
written for, where nothing else is running -- but `esp_core_dump_image_check()`,
`esp_core_dump_image_get()` and `esp_core_dump_get_summary()` are public APIs
called from ordinary tasks, and they go through the same checksum.

So a runtime call lands in the middle of another task's SHA. `ets_sha_enable()`
resets the peripheral and `ets_sha_disable()` turns it off under the other
operation, which then reads its digest state back as all zeroes -- and IDF's own
fault-injection guard in `sha_hal_read_digest()`
(`components/esp_hal_security/sha_hal.c:128`) calls `abort()` on precisely that.
The device panics and reboots.

Reproduced on an ESP32-S3 running four test suites at once: a health monitor
asking `coredump` every ten seconds beside SSH logins, each of which is PBKDF2
at 20 000 iterations of HMAC-SHA256. The core dump named it in one step -- one
`sshd:conn` task in `sha_hal_read_digest()` under
`psa_key_derivation_output_bytes()`, another `sshd:conn` sitting in
`esp_core_dump_image_check()`.

**Workaround.** espix takes `esp_crypto_sha_aes_lock_acquire()` -- the same lock
`esp_sha_acquire_hardware()` uses -- around every espcoredump call that verifies
an image; see `components/espix_fault/coredump.c`. Nothing inside those calls
takes it again, the ROM SHA being lockless, so there is no re-entry to deadlock
on.

**The fix upstream** would be for espcoredump to take that lock itself when it
is not on the panic path, or to use the mbedtls port at runtime and keep the ROM
path for the crash handler. Note that any application calling
`esp_core_dump_image_check()` from a task is exposed, not just this one: it needs
no unusual configuration and no unusual hardware, only a second task using SHA
at the wrong moment. TLS does, and so does anything hashing a password.

### PSA's PBKDF2 re-derives the HMAC key on every iteration

`psa_key_derivation_pbkdf2_generate_block()`
(`tf-psa-crypto/core/psa_crypto.c`, around line 6187) runs its iteration loop
through the **one-shot** MAC entry point, passing the raw password each time:

```c
for (i = 1; i < pbkdf2->input_cost; i++) {
    status = psa_driver_wrapper_mac_compute(attributes,
                                            pbkdf2->password,      /* the key */
                                            pbkdf2->password_length,
                                            prf_alg, U_i, prf_output_length,
                                            U_i, prf_output_length,
                                            &mac_output_length);
```

So every iteration re-derives the HMAC key schedule from scratch — hash the key
if oversized, build the 64-byte ipad and opad, absorb both — before doing the
two compressions the algorithm actually needs. That is roughly **twice the
hashing**, plus a full driver setup and teardown per iteration. PBKDF2 is the
one construction where this cost is multiplied by a deliberately large number.

Every other PBKDF2 implementation, including Mbed TLS's own
`mbedtls_pkcs5_pbkdf2_hmac_ext()`, prepares the key schedule once and reuses it.

**Measured on an ESP32-S3 at 240MHz**, 20000 iterations of
PBKDF2-HMAC-SHA256 for a 32-byte key, all four producing identical output:

| | |
|---|---|
| via `psa_key_derivation_*`, hardware SHA | **2030 ms** |
| via `psa_key_derivation_*`, software SHA | **1936 ms** |
| via `mbedtls_pkcs5_pbkdf2_hmac_ext()`, hardware SHA | **1675 ms** |
| driving `psa_hash_clone()` directly, hardware SHA | **1116 ms** |

Row two is the tell. Turning the SHA accelerator *off* made it slightly faster,
which is only possible if the hashing is a minority of the work — accelerating a
minority cannot help, and on this target the hardware driver allocates
(`esp_sha_hash_setup()` calls `heap_caps_malloc()`), so twenty thousand
malloc/free pairs are paid per password check.

Row three is worth having for a second reason. `mbedtls_pkcs5_pbkdf2_hmac_ext()`
*is* reachable — "private" in Mbed TLS 4.x means unstable API, not inaccessible:
`MBEDTLS_PKCS5_C` is on, the symbol is exported from `libmbedcrypto.a`, and
`drivers/builtin/include` is already on the include path, so it links. It is
still 50% slower than cloning a prepared PSA hash state, because it reaches the
same hardware driver through the MD layer and pays that per-hash setup on all
40000 hashes. So the dedicated, purpose-built PBKDF2 loses to generic hashing
used carefully — which says the cost is in the driver's per-operation overhead,
not in any one caller.

espix works around it in `components/espix_auth/auth.c`: absorb ipad and opad
once, then `psa_hash_clone()` those states per iteration. Same construction and
byte-identical output — checked three ways: against RFC 7914 §11's first vector
at init, against `mbedtls_pkcs5_pbkdf2_hmac_ext()` directly, and by every
existing `/etc/passwd` record still verifying — for 45% less time and no extra
memory. It is ~90 lines of hand-driven HMAC that nobody should have to write.

Worth fixing upstream because the API shape already supports it:
`psa_mac_sign_setup()`/`update()`/`sign_finish()` exist, and the loop could hold
one operation across iterations. Mbed-TLS issue
[#7801](https://github.com/Mbed-TLS/mbedtls/issues/7801) is already cited in a
comment two lines above this loop, for a different quirk in the same function.

### The VFS has no `chmod`

`esp_vfs_fs_ops_t` carries `truncate`, `ftruncate` and `utime` and nothing else
of that family, so a filesystem that *could* store a mode has no way to be told
about one and `chmod()` cannot be implemented behind the standard interface at
all. espix's `chmod` is therefore an espix call (`espix_fs_chmod()`), not a
libc one.

Not an oversight anyone has missed — IDF's own test says so:

    //TODO f_chmod the file and re-test the access rights (this requires
    // f_chmod support to be implemented in VFS)
    -- components/vfs/test_apps/main/test_vfs_access.c

### `chmod()` returns success and does nothing

`esp_libc/src/realpath.c`:

    /* std::filesystem functions call chmod and exit with an exception if it
     * fails, so not failing with ENOSYS seems a better solution. */
    int chmod(const char *path, mode_t mode)
    {
        return 0;
    }

The reasoning is understandable and the result is a call that tells every
caller it changed a mode and changed nothing. Same shape as `pthread_sigmask()`
below: a no-op that reports success is worse than one that reports ENOSYS,
because only the second can be detected.

espix has a real `chmod`, so `abi_fs.c` publishes its own under that name rather
than exporting libc's to apps. `chdir()` in the same file at least fails
honestly with ENOSYS, and `getcwd()` answering `"/"` unconditionally is the
same trap in a quieter form.

### `adjtime()` overflows, silently, and defeats smooth SNTP sync

`delta->tv_sec * 1000000L` is computed in a 32-bit `long`. A 56-year correction
— which is what any first sync from the epoch is — is 1.798e15 µs, and wraps to
about 2.14e9 µs. That is ~35 minutes: precisely the threshold that was supposed
to reject it as too large. So `adjtime()` accepts the wrapped value and returns
success, `sntp_sync_time()` never reaches its `settimeofday()` fallback, and the
clock slews a fictional 35-minute error forever while reporting that it synced.
The symptom is a sync callback that fires, a log line that says the clock was
set, and `date` still reading 1970.

**espix's workaround:** `SNTP_SYNC_MODE_IMMED`, stepping unconditionally. The
full analysis is in the comment at `components/espix_time/time.c`.

### Signal vocabulary with no machinery, including one function that lies

`<signal.h>` declares the whole POSIX surface and the toolchain defines almost
none of it:

- `signal()` is a phantom — `libc_a-signal.o` contains a single unused variable,
  because newlib's `signal.c` was compiled with `SIGNAL_PROVIDED` on the
  assumption that the platform supplies it. ESP-IDF does not.
- `sigaction`, `sigprocmask`, `sigsuspend`, `sigpending`, `pause`, `alarm`,
  `killpg`, `pthread_kill` and `nanosleep` are declared and defined **nowhere**.
  Referencing any of them compiles cleanly and fails at link.
- `kill()` resolves to `_kill_r`, a stub returning `ENOSYS`.
- `raise()` resolves to `_raise_r`, which calls **`abort()`** — so `raise()`
  panics the chip rather than failing.
- `pthread_sigmask()` **returns success while doing nothing**, and is
  force-linked with `-u` so it cannot be replaced. Its own comment says signals
  are not supported and it exists so external libraries link. A no-op that
  reports success is worse than an absent symbol, because nothing detects it.
- `pthread_cancel()` returns `ENOSYS` (this one is documented).

The pthread documentation does not mention signals at all — neither as
unsupported nor as a roadmap item.

**espix's workaround:** all of it is implemented from scratch in
`components/espix_proc/abi_signal.c` and published to apps under the real POSIX
names, which is possible precisely because the namespace is unclaimed.

### FreeRTOS-Plus-POSIX is vendored, but only the message queues

`components/rt` is FreeRTOS-Plus-POSIX V1.0.0, reduced to `mqueue` and `utils`.
Not a defect — worth recording only because its presence suggests a POSIX layer
that is not there. Upstream never implemented signals either, so it is not a
source to draw on.

## `espressif/esp_linenoise`

### ENTER decrements the history length without checking it

`esp_linenoise_edit()` handles a newline like this:

```c
case ENTER:
    state->history_length--;
    free(config->history[state->history_length]);
```

and `CTRL_D` on an empty line does the same. Neither checks that
`history_length` is above zero first. At zero the decrement underflows and the
`free()` reads one pointer *before* the array — whatever the heap happens to
have put there.

Reaching zero is not obviously impossible from outside the component.
`esp_linenoise_edit_start()` adds a `""` placeholder for the line being typed,
but `esp_linenoise_history_add()` refuses a line identical to the last entry —
so an application that rebuilds the editor's history itself and leaves `""` at
the end (which is what espix's `espix_history_apply()` does when the user's list
is empty) gets no placeholder added, and ENTER then takes the length from one to
zero. One more read down that path and the decrement is an underflow.

**Workaround.** None carried: espix has not been able to construct the second
read, and the one crash that looked like this turned out to be espix's own
double free feeding the editor a stale pointer (see
[KNOWN-ISSUES](KNOWN-ISSUES.md)). Recorded because a missing `> 0` on a
decrement that indexes a `free()` is worth a line upstream regardless of who
can currently reach it.

### Dumb mode corrupts input, two ways

The terminal probe runs as the console starts — before anyone has attached a
terminal, and always before `idf.py monitor --no-reset` reattaches to a board
already running. Nothing answers, the probe fails, and the instance latches dumb
mode until reboot. Dumb mode is not merely "no line editing":

- **The line is terminated one byte late.** `esp_linenoise_dumb()` writes
  `buffer[count + 1] = '\0'`, so `buffer[count]` keeps a stale byte from the
  previous command. `df` typed after `whoami` runs as `dfo`.
- **ESC is dropped but its sequence is kept.** ESC is `<= UNIT_SEP` and so
  treated as non-printable, while the rest of the escape sequence is retained —
  an arrow key is entered as the literal text `[A`.

Both were reported by users as "the console goes strange until reboot".

**espix's workaround:** `esp_linenoise_set_dumb_mode(false)` regardless of what
the probe decided. Assuming a capable terminal and being wrong puts escape codes
on screen; assuming a dumb one and being wrong costs the integrity of every
command typed.

### No way to redraw, and multi-line refresh walks private state

The library exposes no redraw entry point, and its multi-line refresh clears the
rows it used last time by walking upward from `max_rows_used` — private, sticky
within an instance, and reset only when `esp_linenoise_edit()` is entered.
Anything printed asynchronously therefore cannot be drawn around from outside:
repairing the line means guessing where the editor believes its prompt is, and
being wrong erases rows above it.

**espix's workaround:** end the input line and let the session loop start a new
one, which is the single operation that leaves the editor's idea of the screen
correct. See the console section of [ARCHITECTURE.md](ARCHITECTURE.md).

## `espressif/elf_loader`

### `esp_elf_arch_flush()` writes back the cache outside the lock it then takes

`esp_elf_adapter.c:129` does, for every target but the S31:

```c
extern void Cache_WriteBack_All(void);
Cache_WriteBack_All();
spi_flash_disable_interrupts_caches_and_other_cpu();
spi_flash_enable_interrupts_caches_and_other_cpu();
```

The second and third lines exist only to force an instruction-cache invalidate
by toggling the flash lock — so the author plainly knew the lock was there. The
write-back on the first line is outside it. Another task starting a flash
operation at that moment disables the cache underneath it, and the write-back
faults with `exccause 0x47 (CacheError)` inside `Cache_WriteBack_Items`.

Reached on espix by loading an app while other tasks were busy: faulting task
`app:testapp`, `Cache_WriteBack_All` ← `esp_elf_arch_flush` ←
`esp_elf_relocate`. It needs `CONFIG_ELF_LOADER_LOAD_PSRAM`, which is the
default, and it needs something else touching flash — a filesystem read is
enough, and on espix every app load *is* a filesystem read, so the window is
open on every launch.

Ordering it correctly is awkward rather than obvious: the write-back needs the
cache **on**, and the only public call that excludes concurrent flash
operations turns the cache **off**. The clean answer is probably
`esp_cache_msync()` over the relocated image's own address range, which is what
the S31 branch above stopped doing for a different reason.

**Workaround.** None carried yet; the alternatives all cost something. Turning
off `CONFIG_ELF_LOADER_LOAD_PSRAM` removes the call entirely but moves every
loaded app's image into internal RAM, which on a board already carrying eight
SSH sessions is not free. Recorded in [KNOWN-ISSUES](KNOWN-ISSUES.md) with the
options.



Not a defect, but a constraint worth knowing: `elf_find_sym_default()` searches
the loader's own libc table **first**, and that table already answers for
`sleep` and `usleep`. A table added with `esp_elf_register_symbol()` is consulted
after it and cannot shadow them. The component provides
`elf_set_symbol_resolver()` for exactly this, documented for "symbol
interception and hooking", so no fork is needed — but a table alone will
silently fail to override.

### `esp_elf_relocate()` knows the missing symbol and will not say

An app whose ELF references a name no registered table publishes fails to load,
and the loader logs the name it wanted:

    E (41906) ELF: Can't find symbol strtok

It then returns a plain non-zero, so the caller gets "something failed" and
cannot say which symbol without the user going to read the log themselves.

What would fix it: return the name, or take a callback the caller can install.
Either lets espix print `undefined symbol: strtok` in its own voice, which is
what `ld.so` does on Linux and what anybody debugging an app expects.

**Until then espix reads it back out of the kernel log.** espix captures esp_log
into its ring, so the line is available; `missing_symbol_name()` in
`espix_proc/exec.c` scans for `Can't find symbol ` and lifts the name. That is
string-matching another component's log text and breaks silently if the wording
changes — deliberately survivable, since it falls back to the old generic
message. **Delete `missing_symbol_name()`, `missing_sym_visit()` and
`ELF_MISSING_SYM_PREFIX` when this lands.**

## `joltwallet/littlefs`

### Nothing can reach LittleFS custom attributes

LittleFS has exactly the right mechanism for POSIX-style metadata.
`lfs_setattr`, `lfs_getattr` and `lfs_removeattr` are public API in `lfs.h`, and
`SPEC.md` describes user attributes as intended for "timestamps, hashes" --
metadata attached to the entry, moved by rename (`lfs.c`, `// move over all
attributes`, via `LFS_FROM_MOVE`) and dropped with the file. The ESP port
already uses one, type `'t'`, to store mtime.

What is missing is any way to call them from outside. `esp_littlefs.c` holds its
state in `static esp_littlefs_t * _efs[CONFIG_LITTLEFS_MAX_PARTITIONS]` with
every lookup `static`, and registers `PRIV_INCLUDE_DIRS src` so
`littlefs_api.h` -- where `esp_littlefs_t.fs` is declared -- is not on a
dependent's include path. The public header offers register, unregister,
mounted, format and info, and no handle. None of the 35 Kconfig options exposes
attributes, and `fcntl` handles `F_GETFL` and `F_GETPATH` and returns `ENOSYS`
for everything else.

Checked against 1.22.3, the latest release, and against upstream `master`:
identical. Not a version problem, and there is no alternative source -- the
registry's other littlefs entries are wrappers and applications rather than
ports, ESP-IDF bundles none, and `muvox-io/esp_littlefs`, the only real fork, is
a 2023 PSRAM variant with no attribute API either.

### Mounting and registering cannot be separated

`esp_vfs_littlefs_register()` mounts the filesystem *and* registers the driver at
a base path. A VFS stacked on top needs the first without the second — it has to
be the only name in the namespace, because a base path on the layer beneath
would reach the filesystem with the upper layer's checks bypassed. There is no
mount-only entry point, and no way to obtain the ops table and context of an
already-registered VFS.

espix needs exactly that, since it registers the root VFS itself and forwards to
LittleFS by pointer; see [ARCHITECTURE.md](ARCHITECTURE.md). The addition is
`esp_vfs_littlefs_register()` minus its final `esp_vfs_register_fs()`, and that
function could reasonably be reimplemented in terms of it.

ESP-IDF already does this for itself, which is the argument that the shape is
ordinary rather than odd: `esp_vfs_uart_get_vfs()` exists so IDF's own console
code can hold the UART driver's ops table instead of routing to it by path. That
one lives in an `esp_private/` header, so it is internal — but "hand me the ops
table" is evidently the shape IDF reaches for when it needs the same thing.

### How espix carries both

[tools/esp_littlefs-attrs.patch](../tools/esp_littlefs-attrs.patch) holds both —
the three attribute accessors, modelled on the port's own mtime helpers and
taking the same lock, and `esp_littlefs_mount()` — and
[tools/patch-littlefs.py](../tools/patch-littlefs.py) applies them to the
downloaded copy from a CMake hook. That is the least clean of the options --
it mutates a tree the build system treats as read-only, and `managed_components/`
is gitignored so nothing records that it happened. It was chosen over vendoring
the component (owning a 92KB file) and over a git-source dependency (maintaining
a public fork) as the smallest thing that keeps the manifest honest. The patch
file exists so the change can go upstream without being reconstructed; when it
lands, the script, the patch and the hook all go.

One caveat found while doing it, now answered:
[littlefs#1076](https://github.com/littlefs-project/littlefs/issues/1076) asks
whether `lfs_setattr` on a currently-open file interacts badly with the
attributes `lfs_file_opencfg` rewrites on every sync and close -- which for this
port is every file, because of that mtime. Open and unanswered since February
2025. Tested on device: a mode set with `lfs_setattr` survives both an appending
and a truncating write to the same file, so an attribute the file config does
not list is left alone.

### `readdir()` reports no inode, and hides `.` and `..`

Two smaller gaps in the same file, both defensible for an embedded port and
both invisible until something wants them.

`esp_littlefs.c` sets `entry->d_ino = 0` for every entry, and neither
`lfs_stat()` nor `struct lfs_info` exposes the id LittleFS identifies a file by.
There is therefore no inode number to report, which is why espix has no
`ls -i` -- with no hard links either, the question it answers cannot arise.

The same function reads in a loop:

    do{ /* Read until we get a real object name */
        res = lfs_dir_read(efs->fs, &dir->d, &info);
    }while( res>0 && (strcmp(info.name, ".") == 0 || strcmp(info.name, "..") == 0));

LittleFS itself synthesises `.` and `..`; the port discards them. Nothing above
the VFS can see them, so `ls -a` shows dotfiles but not the directory entries,
which is GNU `ls`'s `-A` rather than its `-a`.
