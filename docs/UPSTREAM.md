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
| driving `mbedtls_sha256_*` directly, software, no allocation | **919 ms** |

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

espix works around it in `components/espix_auth/auth.c`: absorb
ipad and opad once, then `psa_hash_clone()` those states per iteration. Same construction and
byte-identical output — checked three ways: against RFC 7914 §11's first vector
at init, against `mbedtls_pkcs5_pbkdf2_hmac_ext()` directly, and by every
existing `/etc/passwd` record still verifying — for 45% less time and no extra
memory. It is ~90 lines of hand-driven HMAC that nobody should have to write.

Worth fixing upstream because the API shape already supports it:
`psa_mac_sign_setup()`/`update()`/`sign_finish()` exist, and the loop could hold
one operation across iterations. Mbed-TLS issue
[#7801](https://github.com/Mbed-TLS/mbedtls/issues/7801) is already cited in a
comment two lines above this loop, for a different quirk in the same function.

### `psa_hash_clone()` allocates, and the API does not read that way

A second defect in the same driver, found while trying to remove the first, and
worth reporting separately because it survives any fix to PBKDF2.
`psa_crypto_driver_esp_sha.c`:

```c
psa_status_t esp_sha_hash_setup(...) { ... heap_caps_malloc(..., MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL); }  /* :161 */
psa_status_t esp_sha_hash_clone(...) { ... heap_caps_malloc(ctx_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL); }  /* :386 */
```

Cloning a hash operation allocates a DMA-capable **internal**-RAM context and
frees it at finish. A cloned hash state is a fixed-size struct; the equivalent
`mbedtls_sha256_clone()` is a plain assignment. On a part where internal
DMA-capable RAM is the binding constraint — espix fits eight SSH sessions in
~145K and counts every kilobyte — an allocation hidden behind a copy is
expensive twice over: the time, and the fragmentation of forty thousand
transient allocations per password check.

**The last row of the table is what to take from this.** Bypassing PSA entirely
and cloning a raw `mbedtls_sha256_context` — no allocator, no dispatch, no
hardware lock — reached only 919 ms, against 1116 ms through PSA. So the
allocation is worth about 200 ms of the total and the remaining ~900 ms is the
software compression itself, at roughly 23 microseconds per 64-byte block.

Which is the real indictment: the SHA accelerator does a block in low
single-digit microseconds, so a driver whose per-operation overhead did not
swamp it would put this whole derivation near **100 ms**. The hardware is there
and unreachable at this granularity. espix kept the PSA version — 1.2x is not
worth a dependency on a private header — so this stays a report rather than a
workaround.

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

### FatFs cannot be mounted without registering a path

`esp_vfs_fat_*_mount()` ends in `esp_vfs_register_fs()`, so an IDF FatFs mount is
always *also* a prefix in the namespace. For a VFS that owns the namespace itself
— which is where a permission check belongs, the same place Linux and NuttX put
theirs — that is the whole problem: the prefix outranks the owner's fallback, and
those paths reach the filesystem with no check applied.

There is no way around it from outside the component. The ops tables
(`s_vfs_fat`, `s_vfs_fat_dir`) are file-scope static, and the context they take is
built *inside* `esp_vfs_fat_register()`, whose only other job is the registration,
so the two cannot be separated by a caller either.

The split is small, and this is what espix carries in
`tools/esp_vfs_fat-ctx.patch`:

- `fat_ctx_create()`, lifted out of `esp_vfs_fat_register()`, which now calls it —
  one implementation with two callers rather than a copy that can drift;
- `esp_vfs_fat_ctx_create()` / `esp_vfs_fat_ctx_free()`: build and free a context
  that is registered nowhere;
- `esp_vfs_fat_get_ops()`: hand out the tables.

Paths handed to those ops are already relative to the mount point, exactly as
`esp_vfs` would have made them. (The declarations went into
`vfs/vfs_fat_internal.h` because `esp_vfs_fat.h` cannot name `esp_vfs_fs_ops_t`
without making fatfs depend on `vfs` publicly — a decision upstream should make
rather than inherit from a patch.)

espix applies this with `tools/patch-fatfs.py` from the build, which is a worse
arrangement than `managed_components/`: fatfs is a **built-in** component and the
registry publishes no `espressif/fatfs` that could override it. See
[USB-HOST.md](USB-HOST.md#stage-2--mounting) for the cost of carrying it.

### A partition view cannot be larger than 4GB

`esp_blockdev_generic_partition_get()` takes the partition's offset and size as
`size_t`, and stores the offset as one:

    esp_err_t esp_blockdev_generic_partition_get(esp_blockdev_handle_t parent,
                                                 size_t start, size_t size,
                                                 esp_blockdev_handle_t *out);
    typedef struct { esp_blockdev_t dev; esp_blockdev_handle_t parent;
                     size_t start_offset; } esp_blockdev_generic_partition_t;

`size_t` is 32 bits on every ESP32 target, so a partition larger than 4GB — one
FAT32 volume on an ordinary USB stick, which is the case that finds this — cannot
be expressed. It does not fail, either: the size truncates, the volume mounts as
the low 32 bits of itself, and the failure surfaces much later as a read past a
boundary nobody asked for. espix carries `components/espix_fs/part.c`, the same
implementation with `uint64_t` offsets — about eighty lines, and a pity to have
twice.

**The same truncation, one file over, in the SDMMC block device.**
`components/sdmmc/sdmmc_blockdev.c` (v6.0.2) narrows the address the same way,
before converting it to sectors:

    size_t start_sector_num = (size_t) addr / sector_size;
    size_t last_byte_addr   = (size_t) (addr + data_len - 1);

Media past 4GB therefore wraps modulo 4GB, silently — the operation targets a
valid sector and returns success — and **reads are affected as well as writes**,
so data above the boundary comes from the wrong place without an error anywhere.

espix is not on that path: the SDMMC block device is for SD cards, and espix's
storage is USB MSC, where `msc_bdl_read()`/`msc_bdl_write()` are `uint64_t`
throughout and pick `READ(16)` above `UINT32_MAX` (see `tools/patch-msc.py`).
The point of recording it is the audit, not the fix: this is the **third**
instance of the family after `esp_blockdev_generic_partition_get()` above, and
the rule it establishes is that no byte address in a lower block-device layer is
safe to assume 64-bit without reading it.

Reported upstream as [esp-idf#18875](https://github.com/espressif/esp-idf/issues/18875)
(IDFGH-18017), fixed on main and **not** backported to the v6.0 release line, so
a v6.0-based project has to check its exact revision rather than the issue's
state. The measurement is from a third-party port's own write-up —
`doc/CAVEATS.md` in `huming2207/esp_lwext4` — which pinned the alias exactly: an
inode bitmap for block group 227 landing on block group 3's block bitmap, at
which point the allocator had marked that group's own metadata as free.

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

## `espressif/esp_ext_part_tables`

### A `0x00` type byte ends the table, and on real media it does not

`esp_mbr_parse()` stops at the first partition entry whose type byte is zero:

    if (partition->type == 0x00) {
        break; // No more partitions, exit the loop (MBR partition table cannot
               // have holes in it)
    }

That is true of an *empty* entry and false of a typed one. A hybrid ISO image —
the layout that `dd`-ing an Arch, CachyOS or Ubuntu installer onto a stick
produces — uses `0x00` for its large filesystem entry, with a real LBA and sector
count, so every entry after it is dropped. Measured on a CachyOS 202604
installer: entry 1 is 2.8G of ISO 9660 typed `0x00`, entry 2 is 23M of EFI FAT
typed `0xEF`. The library reports one partition and no partitions respectively —
neither the ISO nor the FAT partition that is the only mountable thing on the
stick. Nothing sets `ESP_EXT_PART_LIST_FLAG_LOSSY` either, so a caller cannot even
tell that something was left out.

The rule that would be right is "empty" in the sense the comment means it: no
start *and* no size. A `0x00`-typed entry that names sectors is an unnameable type
— worth inserting, or at the very least worth flagging.

espix walks the four entries itself now and calls
`esp_mbr_parse_default_supported_partition_types()` for the type table, which is
the part of the parser that assumes nothing about the walk; see
[USB-HOST.md](USB-HOST.md#what-it-reports-and-what-it-cannot).

### `0xEF` is not in the type table

The EFI System Partition — `0xEF`, "EFI (FAT-12/16/32)" to `fdisk` — is FAT, so
FatFs can mount it, and every Arch, CachyOS and Windows installer writes one. It
maps to `ESP_EXT_PART_TYPE_NONE`, so a caller that trusts the table cannot name
it, let alone mount it. espix adds the mapping itself; it belongs beside `0x01`,
`0x04`, `0x06` and `0x0E`.

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

## `espressif/usb` and `espressif/usb_host_msc` (the USB host stack)

### A client's event callback must not wait on that client's own transfers

The documented pattern for a class driver is to call `msc_host_install_device()`
from the MSC event callback, and Espressif's own example does exactly that. It
does not work. The callback is invoked from `usb_host_client_handle_events()`
(`usb_host.c:1319`), and a client's transfer completions are delivered only from
inside that same function: its endpoint list is serviced there
(`_handle_pending_ep()`, `usb_host.c:1296`) and that is where
`urb->transfer.callback()` runs (`usb_host.c:1136`). So an install started in the
callback waits on a semaphore that only the call stack it is blocking can give.

Nothing fails fast and nothing is logged. `msc_bulk_transfer()` waits the
transfer's full `timeout_ms` — 5000 ms, set by the driver (`msc_host.c:703`) — and
`msc_wait_for_ready_state()` retries `5000 / 100 = 50` times (`msc_host.c:297`,
`:551`), each retry another SCSI command of three transfers. Measured on an
ESP32-S3 with a Samsung PSSD T9: **ten minutes to fail**, no log line until it
does, the `USB MSC` task blocked throughout, and therefore the *next* device never
reported either. From outside it looks exactly like a driver that ignored the
device — which is how espix spent a day on it.

**Workaround:** do the work somewhere else. espix queues the address from the
callback and a `usb:work` task performs the install; the same SSD then attaches in
**1.0 s**. Everything else that blocks on transfers follows the same rule: espix's
`usbscan` and `usbprobe` run from a session task and never from `usb:host`,
because `usb:host` is the task that drives the library whose completions the
install is waiting for — doing it there moves the deadlock rather than fixing it.

### `msc_host_install_device()` asserts on a device with no bulk-only interface

`extract_config_from_descriptor()` looks for the class-8 / subclass-6 /
protocol-0x50 interface and then does `assert(ifc_desc)` (`msc_host.c:228`). A
public API that aborts the board is a sharp edge: pointing a diagnostic at a hub
resets the device instead of answering "not storage".

**Workaround:** check the interface list first and never call the driver for a
device that has none — `addr_msc_interface()` in `espix_usb/host.c` does, and
`usbprobe` reports it in words. The driver could return
`ESP_ERR_NOT_SUPPORTED` like the rest of its error paths do.

### `USB_W_VALUE_DT_INTERFACE` used where `USB_B_DESCRIPTOR_TYPE_INTERFACE` is meant

`next_interface_desc()` filters descriptor walks by `USB_W_VALUE_DT_INTERFACE`
(`msc_host.c:108`) — the *wValue* encoding used in control requests, not the
`bDescriptorType` field it is compared against. It works only because both
constants happen to be `0x04` (`usb_types_ch9.h:45` and `:146`). Either changing
would silently stop the class driver seeing any interface at all, with the symptom
of the first entry above: a device that enumerates and is then ignored.

**Workaround:** none needed today; named here so the coincidence is not
rediscovered as a mystery.

### The external-hub driver asserts when a device is released twice

`device_release()` asserts that the device it is handed is actually on its way
out (`ext_hub.c:508`):

    static void device_release(ext_hub_dev_t *ext_hub_dev)
    {
        EXT_HUB_ENTER_CRITICAL();
        assert(ext_hub_dev->dynamic.flags.waiting_release); // Sanity check
        ext_hub_dev->dynamic.flags.waiting_release = 0;
        ext_hub_dev->dynamic.flags.waiting_free = 1;

On this board it arrives as, from the core dump:

    Panic reason: assert failed: device_release ext_hub.c:508 (ext_hub_dev->dynamic.flags.waiting_release)
    ext_hub_process ()            ext_hub.c:1535
    hub_process ()                hub.c:1345
    usb_host_lib_handle_events () usb_host.c:929

`waiting_release` is the driver's "this device is being torn down" state. It is
set in three places — `ext_hub.c:405`, the *device gone* path at `:1431`/`:1439`,
and `:1469`, a loop over the pending and active hub lists — and cleared only at
`:509`. The *device gone* path guards itself against re-entry (`:1420`); the loop
at `:1469` sets the flag unconditionally and adds `DEV_ACTION_RELEASE` when the
stage is idle, so a second release of the same device reaches the assert there.

Seen once, on the first attach of a newly formatted stick with an external hub
attached (`espressif/usb` 1.5.0, ESP-IDF v6.1, esp32s3). It is the *attach* path,
not removal: a clean unplug and replug of the same stick did not reproduce it.
The record said removal for a while, which is worth knowing before anyone goes
looking for a teardown race. It is in the library's own event loop — a caller's
only frame in the stack is the `usb_host_lib_handle_events()` call that drives it
— so nothing in a caller can prevent it, and the cost is the whole board. Checking
the flag where it is set, or tolerating a second release, would make this a log
line instead.

**Workaround:** none. Hubs cannot be turned off here — this board cannot power a
device from the OTG socket, so the hub is how anything is plugged in at all.

### A failed SCSI write discards its sense data

`scsi_cmd_write10()` fetches the sense data when a write fails — and throws it
away (`msc_scsi_bot.c:366`):

    esp_err_t ret = bot_execute_command(device, &cbw.base, (void *)data, num_sectors * sector_size);
    if (unlikely(ret != ESP_OK)) {
        MSC_RETURN_ON_ERROR( scsi_cmd_sense(device, NULL));
    }
    return ret;

`scsi_cmd_sense(device, NULL)` runs the REQUEST SENSE transfer and drops the
response, so the one thing that says *why* — write-protected, a UNIT ATTENTION
left over from partitioning the medium on another host, a medium error — reaches
neither a caller nor a log. The BDL path adds nothing of its own either:
`msc_bdl_write()` returns the error without logging anything, where the older
`diskio_usb.c` path at least printed `scsi_cmd_write10 failed (%d)`.

Those causes are indistinguishable without it, and a caller sees one error code
for all of them. Passing the sense response out, or logging it, would cost
nothing.

**Workaround:** none — which is why a write failure here has to be diagnosed from
the errno the layer above reports. See
[GOTCHAS.md](GOTCHAS.md#a-write-that-succeeded-has-not-been-written-yet).

### A device pulled mid-transfer takes the heap with it

`espix_usb` releases the block device on detach, which is what its design says it
should do: the slot owns the handle and gives it back. The problem is *when*. An
MSC transfer already in flight cannot be recalled, and it holds a pointer to that
block device, so it finishes against freed memory — and a transfer whose buffer
and length are no longer meaningful does not fail, it writes somewhere.

Measured, pulling a second or two into `cp /dev/factory /mnt/sd1/…`:

    pc 0x403840a2 <tlsf_free+614>
    #0  remove_free_block (tlsf_control_functions.h:374)
    #1  block_remove
    #2  block_merge_next
    #3  tlsf_free (tlsf.c:633)
    #4  multi_heap_free_impl
    #5  heap_caps_free
    #10 espix_shell_session_run (session.c:607)   ← the console task, a bystander

Heap corruption, caught by `free()` walking a list that had stopped making sense,
in a task that had nothing to do with the stick. It is repeatable once seen: pull
*while a write is in progress* and the board is gone. Pull when nothing is being
written and everything above this works — including reads on the dead mount
answering `ENOSYS` and `df` declining to report on it, both measured.

**What would fix it, best first:**

1. **A way to quiesce.** An `msc_host_*` call that stops new transfers and waits
   for the in-flight one before the caller releases the handle. That is the ask,
   and it is the only version that is correct rather than lucky.
2. **Reference counting on the handle**, so a transfer finishing after the release
   has nothing to write through.
3. **What espix could do alone:** `on_disconnected()` could delay
   `msc_host_uninstall_device()` instead of calling it immediately, on the theory
   that a few hundred milliseconds outlasts any transfer. A guess dressed as a
   fix, which is why it is third.

Until one of those exists this belongs with the known ways to lose the board,
which is where docs/KNOWN-ISSUES.md points from.

## `espressif/esp_tinyusb` (TinyUSB NCM)

### `CFG_TUD_NCM_IN_NTB_N = 2` silently truncates a transfer

`CONFIG_TINYUSB_NCM_IN_NTB_BUFFS_COUNT` accepts 1-6 and defaults to 3. At **2**,
device-to-host bulk transfer breaks: `scp` of a 4 MB file from the device
delivered exactly 2,097,152 bytes and stopped, at 46 KB/s against 818 KB/s at
the default. Three runs, identical to the byte and the kilobyte.

Nothing is logged. `esp_tinyusb`'s own warning for a refused send --
`"Packet cannot be accepted on USB interface, dropping"` in `tinyusb_net.c` --
never fires, so this is not the back-pressure path the Kconfig help describes
when it says a low count causes `tud_network_can_xmit: request blocked`. It is
also not the *other* direction: `OUT` at 2 with `IN` at 3 measures identically
to the default, in both directions, which is the configuration espix now ships.

Exactly half the file, at a count of 2, suggests the xmit ring's free-list
accounting rather than throughput -- but that is inference, and the measurement
is the part worth reporting.

Not filed upstream yet. What a report needs first is the same test on a stock
`esp_tinyusb` example rather than through espix's netif, to rule this side out.

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

## `gkostka/lwext4` (the ext2/3/4 core, through `huming2207/esp_lwext4`)

### No volume made by e2fsprogs 1.47 or later can be mounted

`mke2fs` enables `metadata_csum_seed` by default these days, which sets
`EXT4_FEATURE_INCOMPAT_CSUM_SEED` (`0x2000`) in the superblock. On such a volume
every metadata checksum is seeded from the superblock's `s_checksum_seed` rather
than from the filesystem UUID — and the core seeded from the UUID, inline, in
eight places: `ext4_balloc.c`, `ext4_dir.c`, `ext4_dir_idx.c`, `ext4_extent.c`,
`ext4_fs.c`, `ext4_ialloc.c` and `ext4_xattr.c`, with `ext4_types.h` calling the
bit `BG_USE_META_CSUM`, which is not what it means.

`ext4_fs_check_features()` then finds an incompatible bit outside
`CONFIG_SUPPORTED_FINCOM` and answers `ENOTSUP`, so the mount fails with no hint
as to which bit or why. The refusal is correct — the feature is not implemented —
but the effect is that **no ext4 volume a current Linux creates can be read at
all**, which is a large gap for a filesystem espix advertises on the strength of
reading Linux-formatted sticks.

The seed is not something that only matters when writing, which is what makes
tolerating the bit useless rather than merely risky: verification runs on *reads*.
`ext4_balloc_verify_bitmap_csum()` checks the block and inode bitmaps as they are
read, `ext4_dir_csum_verify()` checks every directory block, the htree code checks
its nodes, and the extent tree's tail carries its own checksum. All of them
compare against what the filesystem stored, so a wrong seed fails the first bitmap
and reads as a broken device rather than an unsupported filesystem.

**How it was found**, since the code alone could not say: the mount answered
`ENOTSUP` and nothing else, and espix's userland cannot read a device to go looking
— dev.c refuses to open a block node on purpose. The driver now reads the
superblock on that failure and logs its fields, its feature words and whether the
checksum it computes matches the one stored: `log_why_not_mounted()` in
`components/espix_fs/ext.c`. That turned "cannot mount" into `incompat 000022c2,
unsupported 00002000` in one line. Everything else in it was fine — `compat
0000103c`, `ro_compat 0000046b`, magic `0xEF53`, block size 4096, and the
superblock checksum matched exactly, which is also what ruled out a bad read.

**How espix carries it.** `tools/lwext4-csum-seed.patch` is the change as a plain
diff: one helper, `ext4_sb_csum_seed()`, returning `s_checksum_seed` when the
feature is set and `crc32c(~0, uuid, 16)` otherwise — which is exactly the
kernel's `s_csum_seed` — with the eight sites asking for it and the bit joining
the supported set. `tools/patch-lwext4.py` applies it to the copy the component
manager fetches, from a CMake hook beside the other patch scripts.

Two details worth having if this is ever redone by hand. The core's
`struct ext4_sblock` sits inside a `#pragma pack(push, 1)` and mirrors the disk
layout, so `s_checksum_seed` is at 0x270 and was named by taking two words out of
the padding in front of `checksum`, which leaves the struct 1024 bytes. And
`ext4_bg_crc16()` is deliberately untouched: that is the legacy `GDT_CSUM` path,
and the kernel seeds it from the UUID as well.

**Upstream** it belongs to `gkostka/lwext4`; the port would pick it up by bumping
its submodule. When it lands, delete `tools/patch-lwext4.py`,
`tools/lwext4-csum-seed.patch` and the `execute_process()` block in the top-level
`CMakeLists.txt`, and update the pin in `main/idf_component.yml`.

## ESP-IDF (newlib)

### `off_t` is 32 bits, and no Kconfig changes it

`st_size` is an `off_t`, and on this target `off_t` is a 32-bit `long`. Nothing
in IDF 6.1 offers to widen it: there is no `CONFIG_LIBC_FS_*` for it, and the only
size knob nearby — `sys/select.h`'s `FD_SETSIZE` — is about `select()`, not file
sizes.

Both symptoms were measured. `/dev/sda4`, a 23 GiB partition, lists as `0`, which
is exactly its low 32 bits. And `df -h` printed a 6.0 GiB volume as `2.0G`, since
2,133,860,352 is the low 32 bits of 6,428,827,648 — that one was espix's own bug,
a cast through `off_t` in the shared size formatter, and is fixed; the first is the
ceiling itself. No file over 4 GiB can be reported or seeked correctly, whatever
espix does above it.

**The same 32-bit narrowing, twice more.** Reaching a 3.1 TB volume found the
other two places a size passed through a type narrower than the value:

- `df`'s plain `1K-blocks` column formatted each figure through `unsigned`, so a
  3.1 TB volume — 3,371,628,178 1K-blocks — both wrapped and could subtract into
  a negative that printed as a huge number. It is `%llu` on a `uint64_t` now, and
  the available figure is derived in 64-bit rather than from two truncated ones.
- `df -h`'s rootfs row still cast through `off_t` before calling the formatter,
  which is the 6.0G/2.0G bug surviving in the one row the earlier fix did not
  cover: the volume rows beneath it passed a `uint64_t` and were right.

The rule that keeps it fixed: `espix_cmd_size()` takes a `uint64_t` and every
caller hands it one. A size carried in a 32-bit type anywhere between the driver
and the formatter is the bug, and it is silent.

There is a fourth, which reaching T also exposed and which was **not** the 32-bit
ceiling: the formatter's unit table stopped at `G`. A 3.1 TB volume printed as
`3214G` in `df`, `lsblk` and `ls` alike, because all three share that one
function. Two rounding faults came out with it — the tenths were rounded *up*
rather than to the nearest, and the whole unit was rounded a second time, so
3.638 TiB printed as `4T` before it printed as `3.7T`. Corrected against
coreutils: round the tenths to nearest, truncate the unit, so `3.6T`.

A patch in the shape of the other two — a build-time define on the newlib headers
— would fix the ceiling. It is also the one patch that would touch every `struct
stat` in the image, so it wants a deliberate decision rather than riding along
with something else. docs/ROADMAP.md is where that decision is written down.
Until then the four-byte-`off_t` cases above are the ones to watch for, and
`st_size` — so `ls -l`'s own size column — is the remaining one that is still
truncated: a file over 4 GiB still lists as its low 32 bits.

## `FF_USE_LABEL` is a bare symbol, and exFAT is what compiles the line using it

`ffconf.h` writes `#define FF_USE_LABEL CONFIG_FATFS_USE_LABEL`, and ESP-IDF's
generated `sdkconfig.h` defines only the symbols set to `y` -- so with
`CONFIG_FATFS_USE_LABEL=n` that value is an undeclared identifier. Harmless
until something evaluates it in *C* rather than in `#if`: `ff.c:2357` has
`if (FF_USE_LABEL && vol)`, inside a block guarded by
`#if FF_FS_MINIMIZE <= 1 || FF_FS_RPATH >= 2 || FF_USE_LABEL || FF_FS_EXFAT`.
Enabling exFAT compiles that block, and the build fails with
`'CONFIG_FATFS_USE_LABEL' undeclared (first use in this function)`.

espix's patch adds a `#ifndef CONFIG_FATFS_USE_LABEL` fallback ahead of the
define; `tools/patch-fatfs.py` carries the same note. Upstream, the fix is
either a `default y` on the option or that guard.

## A failed `readdir` and the end of a directory are the same NULL

FatFs reports a directory read failure honestly. `dir_read` propagates the
`FR_DISK_ERR` out of `move_window(fs, dp->sect)` (`ff.c:2349-2350`), and IDF
carries it all the way:

- `vfs_fat_readdir_r` sets `*out_dirent = NULL` and returns the errno
  (`vfs_fat.c:1153-1157`);
- `vfs_fat_readdir` sets `errno` and returns `NULL` (`vfs_fat.c:1131-1134`).

The POSIX `readdir` contract is what loses it: `NULL` means end-of-directory,
and a caller that does not read `errno` cannot tell the two apart. espix's `ls`
did not — its loop was `while ((ent = readdir(dir)) != NULL)` and the file held
no `errno` reference at all — so a walk that failed part-way printed a short
listing and a count of what it had managed to read. On a 3.1TB exFAT volume that
read `13 entries` for a directory holding 19, which is not a truncated listing
to anyone reading it: it is a directory that holds 13 things.

**espix's workaround:** clear `errno` immediately before *every* `readdir` --
after all other work in the iteration, with nothing between the reset and the
call. Two placements that look right do not work, and both were tried on
hardware rather than reasoned about:

- clearing it once outside the loop. Anything before the first `readdir` can set
  it, and `opendir` does.
- clearing it after the `NULL` check. The per-entry `stat()` below sets `errno`
  in the ordinary course of a *successful* listing -- a lookup that misses a probe
  and then answers from the rule leaves `ENOENT` behind -- so by the next
  iteration's end-of-directory the reset has long been overwritten. Measured:
  `ls -l /` reported `stopped after 8 entries: No data` on a walk that had not
  failed at all.

`ENOSYS` is excluded from the report, because that is what a lower filesystem
with no `readdir` at all answers (see `vfs_readdir`) and it is a build fact rather
than a read failure.

Upstream, there is no fix at this level — `readdir` cannot distinguish the two
and never will. What a POSIX implementation can do is what espix does, and what
every other `ls` does: consult `errno`. The trap worth knowing is that the
consultation has to be immediate.

This is worth knowing beyond espix. It hit here because a directory walk over
USB can fail a sector read, and FatFs is honest about it; any `readdir` loop that
ignores `errno` will silently under-report the same way.

Two halves of the same feature were widened separately, and only one of them
reaches the driver.

FatFs's side is right. `ff.h:74` makes `LBA_t` a `QWORD` when `FF_LBA64` is set,
`ff_disk_read()`/`ff_disk_write()` are declared with it (`diskio.c:95,99`, via
`#define disk_read ff_disk_read` at `ffconf.h:403`), and every sector FatFs
computes internally is 64-bit.

The dispatch under it is not. `diskio_impl.h` declares the registered drivers'
function pointers with a 32-bit sector:

    typedef struct {
        DSTATUS (*init) (unsigned char pdrv);
        DSTATUS (*status) (unsigned char pdrv);
        DRESULT (*read) (unsigned char pdrv, unsigned char* buff, uint32_t sector, UINT count);
        DRESULT (*write)(unsigned char pdrv, const unsigned char* buff, uint32_t sector, UINT count);
        DRESULT (*ioctl)(unsigned char pdrv, unsigned char cmd, void* buff);
    } ff_diskio_impl_t;

`ff_disk_read()` then calls `s_impls[pdrv]->read(pdrv, buff, sector, count)`,
where `sector` is the 64-bit `LBA_t` it was handed. The conversion to the
pointer's `uint32_t` parameter happens at the call site, implicitly, and the
compiler says nothing. Sector `0x100000000` becomes `0`: the read succeeds, from
somewhere else on the medium entirely.

So `FF_LBA64` widens FatFs's arithmetic and nothing else. A volume larger than
2^32 sectors — 2TiB at a 512-byte sector, which is the size that motivates exFAT
in the first place — mounts, reads at low LBAs, and silently returns wrong data
above the boundary. It is the same class of failure as the `size_t` partition
offset in `esp_blockdev_generic_partition_get()` above, one layer further down.

`FF_LBA64` also cannot be enabled alone: `ff.h:81` fails the build with
`#error exFAT needs to be enabled when enable 64-bit LBA`, so every route to this
goes through `FF_FS_EXFAT`.

**espix's workaround.** `tools/patch-fatfs.py` gives the struct a
`DISKIO_SECT`, which is `LBA_t` when `FF_LBA64` is set and `DWORD` when it is
not, and declares every backend that assigns into it with the same type. The
mismatch becomes a build error instead of a wrong read, which is how the other
three backends surfaced rather than being guessed at. `diskio_bdl.c` also widens
its `GET_SECTOR_COUNT` buffer write, since `ff.c:5954` passes an `LBA_t*` there
and a 32-bit store left the upper half uninitialised.

Deliberately *not* widened: `diskio_sdmmc.c`, `diskio_wl.c` and
`diskio_rawflash.c` still narrow to 32 bits at `sdmmc_read_sectors()`,
`wl_read()` and `esp_partition_read()`. Those APIs take 32-bit sector numbers
themselves, so widening the parameter would only move the truncation one call
further in; espix registers none of them.

Upstream, the fix is to declare the struct's sector parameter as `LBA_t` — and,
with it, to widen every backend's error of omission into the build error espix's
patch makes it.
