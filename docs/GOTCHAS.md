# Gotchas: ESP32 and ESP-IDF against the rest of the world

Things that are not wrong, not undocumented, and still cost this project a day
each — because they differ from what a POSIX habit, a vanilla FreeRTOS habit, or
the chip next to this one leads you to expect.

Not a description of the hardware. Every entry is here because an assumption
that holds everywhere else does not hold here, and the failure that follows
looks like something else entirely. Each carries a doc link, an ESP-IDF source
path, or a measurement on the board, because a gotcha without a citation is a
rumour.

espix targets the **ESP32-S3**. Other parts appear only where the same code
would behave differently on them.

Verified against ESP-IDF v6.1 and xtensa-esp-elf GCC 15.2 unless noted.

## "You cannot use X *while* Y" — the rules that do not fit in a signature

The constraints that bite are rarely "do not call this". They are about
combination and timing, and nothing in the prototype hints at them.

| You want | The catch | Where it says so |
|---|---|---|
| A DMA buffer in PSRAM | `MALLOC_CAP_DMA` **excludes** external RAM. You have to ask for `MALLOC_CAP_SPIRAM \| MALLOC_CAP_DMA` together — and only where `SOC_PSRAM_DMA_CAPABLE` is set, which is not everywhere. | `heap/include/esp_heap_caps.h` |
| `esp_cache_msync()` anywhere | **Not during any flash operation** — `esp_flash`, NVS, `esp_partition_*`, or a filesystem — unless `CONFIG_SPIRAM_XIP_FROM_PSRAM` is on. The sting is that the call is usually IDF's, not yours: see the crypto entry below. | [mm_sync](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/system/mm_sync.html) |
| `esp_cache_msync()` M2C (invalidate) | **Cannot** be combined with `ESP_CACHE_MSYNC_FLAG_UNALIGNED` — rejected outright with `ESP_ERR_INVALID_ARG`. Only the write-back direction takes unaligned, and only at the risk below. | `esp_mm/esp_cache_msync.c:134` |
| DMA descriptors anywhere convenient | They cannot live in PSRAM at all. `MALLOC_CAP_DMA_DESC_AHB` / `_AXI` exist for this. | [external-ram](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/external-ram.html) |
| A big task stack, in PSRAM | `xTaskCreate()` will never do it. `xTaskCreateStatic()` will, with `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM` — and then that task **may not touch WiFi or Bluetooth**, because it may not call ROM code. | `freertos/Kconfig:585` |
| An ISR that survives a flash write | `ESP_INTR_FLAG_IRAM` is not a promise the framework keeps for you. Every function it calls and every constant it reads must also be in IRAM/DRAM; one flash-resident callee turns a flash operation into a crash. | `esp_intr_alloc.h:42` |
| A lazily created mutex | `xSemaphoreCreateMutex()` allocates, so it cannot be called from a critical section — and the obvious `if (lock == NULL) create();` is itself the race it is meant to prevent. `xSemaphoreCreateMutexStatic()` with static storage, inside a critical section, is the version that works. | espix `espix_shell/history.c`, which learned this the hard way |
| USB host *and* USB-NCM on one board | `SOC_USB_OTG_PERIPH_NUM` is **1** on the S3 and the S31, and 2 only on the P4. The one peripheral is either a host or a device, so it is a build-time choice, not a runtime one — espix asks in `ESPIX_USB_ROLE`. | `soc/esp32s3/include/soc/soc_caps.h:338`, `soc/esp32p4/.../soc_caps.h:438`; espix [USB-HOST.md](USB-HOST.md) |
| Any device behind a hub | Nothing enumerates until `CONFIG_USB_HOST_HUBS_SUPPORTED=y` — it **defaults to `n`** — and the symptom is silence: the hub's port lights up if it has lights, the log says nothing, and the devices behind it may as well not be plugged in. `CONFIG_USB_HOST_HUB_MULTI_LEVEL` does default to `y`, which makes the trap look handled. | `usb/Kconfig:137` (esp `espressif/usb` 1.5.0) |
| A USB host transfer buffer in PSRAM | It has to be internal RAM on the S3: the USB-DWC DMA engine cannot reach external memory there, so `MALLOC_CAP_DMA` is the requirement and not the optimisation. The option that lifts it, `CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM`, is `depends on IDF_TARGET_ESP32P4 && SPIRAM` and does not exist on this target. | `usb/Kconfig:227` |
| Doing USB work *inside* a host-library client callback | The callback runs inside `usb_host_client_handle_events()`, and that function is the only thing that delivers that client's transfer completions (`usb_host.c:1136`, `:1296`). Anything that waits for a transfer from there waits for the loop it is blocking: `msc_host_install_device()` takes 5 s per transfer × 50 retries — ten minutes per device, no log line, driver task blocked, and every later device invisible. | `msc_host.c:297`, `:551`, `:703`; espix [UPSTREAM.md](UPSTREAM.md) |
| A hub *and* two storage devices | Every pipe is a host-controller channel: the root port 1, an open hub 2 (control + interrupt), a bulk-only device 3 (control + two bulk). `hcd_pipe_alloc()` returns `ESP_ERR_NOT_SUPPORTED` when the pool is empty — with the HCD logging `No more HCD channels available` — so the second drive is refused while the first works, and the *first* one to enumerate wins. One storage device at a time on the S3. | `hcd_dwc.c:2119`; espix [USB-HOST.md](USB-HOST.md) |

## Cache and DMA

### The alignment rule is address **and size**, and the size half is the one people miss

> An address region whose start address **and size** both meet the cache memory
> synchronization alignment requirement is defined as an *aligned address
> region*. … Cache memory synchronization to an unaligned address region may
> silently corrupt the memory.
> — [Memory Synchronization](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/system/mm_sync.html)

Aligning the base and letting the length fall where it may feels careful and is
not. A write-back rounds outwards to whole cache lines, so the tail of the last
line — which belongs to whoever the allocator put next — goes with it, and data
not yet written back "will be discarded". The symptom is a heap assertion in a
completely unrelated task, minutes later.

And the line size is a **query**, not a constant: `esp_cache_get_line_size_by_addr()`.
On the S3 it is configurable at 16, 32 or 64 bytes; espix builds at 32. Code
that hard-codes 64 is accidentally safe until someone changes the option.

### Using PSRAM for crypto buffers silently opts you into all of that

This is the one that is genuinely hard to see coming. IDF's AES and SHA drivers
call `esp_cache_msync(..., UNALIGNED)` on any buffer that happens to be in
external RAM — `esp_aes_process_dma()`, `esp_sha_dma_process()`. So you inherit
both the alignment rule and the no-flash-operations rule **without ever calling
the cache API**, just by allocating a packet buffer with `MALLOC_CAP_SPIRAM`.

espix hit it as `exccause 0x47 (CacheError)` inside `Cache_WriteBack_Addr`, with
SSH sessions encrypting while another task touched flash — about one parallel
test run in three, under both `esp_aes_process_dma()` and
`esp_sha_dma_process()`.

### What a flash write actually stops, and what XIP gives back

Worth being precise, because the usual summary is "ISRs need `IRAM_ATTR`", and
that undersells it. From
[spi_flash_concurrency](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/spi_flash/spi_flash_concurrency.html),
on the default configuration:

> caches are disabled during SPI1 operations (read/write) … all non-IRAM-safe
> interrupts will be disabled, and all other tasks are suspended

**All other tasks are suspended.** Saving a file stops the whole system for the
duration, not just the interrupt handlers — and cache *maintenance* running at
that moment does not merely stall, it faults.

**`CONFIG_SPIRAM_XIP_FROM_PSRAM` is the way out, and the name reads backwards.**
It does not execute anything *from flash*; it stops doing so. `.text` and
`.rodata` are copied into PSRAM at boot and fetched from there, so a flash write
no longer needs the cache off.

Two things not to overstate, because the popular summaries do:

- The doc says the cache will not be disabled "**in most cases**", not never.
- The named exception is **cache-mapped flash**, and the exact condition is
  worth having rather than the prose version, because it is sharper and nastier
  than "you cannot read a mapped region during an erase". From
  `spi_flash/spi_flash_os_func_app.c`, `spi1_start()`:

  ```c
  if (!(flags & ESP_FLASH_START_FLAG_NO_READ) || !flash_mmap_remain()) {
      ctx->current_op_type = OP_TYPE_MMAP_LOCK;   /* cache stays on */
  } else {
      cache_disable(NULL);                        /* cache goes off */
      ctx->current_op_type = OP_TYPE_CACHE_DIS;
  }
  ```

  A write or erase keeps the cache **only while no mmap region is live at all** —
  not "only while the region being written is mapped". `flash_mmap_remain()` reads
  `s_mmap_remain_count`, a single global incremented per mapping and decremented
  only by a matching `munmap`. So **one** mapping taken without
  `ESP_PARTITION_MMAP_BLOCKS_WRITE` — those release the mmap lock immediately and
  leave the count raised, `flash_mmap.c:293` and `:324` — stays counted for the
  rest of the boot and silently returns every later write to the cache-disable
  path. XIP is then buying nothing, the image is still in PSRAM, and the only
  symptom is that the crashes come back.

  Do not take the S3's counter for a no-op on the strength of the `#if` around
  it: the empty-macro branch in `flash_mmap.c` is guarded by
  `#else //!CONFIG_IDF_TARGET_ESP32`, so it is the **ESP32** that does no
  counting. The S3 counts.

  espix maps nothing — checked, and now asserted rather than checked once:
  `free` prints `flash mmap: none live` and `tests/suites/65-flashstall.sh`
  fails if it ever says otherwise. An app that mmaps a partition brings the
  whole hazard back with it.

Measured on an N16R8 rather than assumed: PSRAM total falls 8189K → 7114K —
1075K for the image, out of eight megabytes — internal RAM is unchanged, and scp
throughput went **up**, 562 → 612 KB/s uploading and 476 → 594 KB/s downloading,
because flash operations stop stalling the core. The alternatives are worse:
keeping DMA'd buffers in internal RAM caps how many sessions fit, and "just do
not overlap flash I/O with crypto" is not something an application with more
than one task can promise.

The boot cost is **88ms**, measured off the serial log's own timestamps:

```
I (364) esp_psram: Speed: 80MHz
I (381) mmu_psram: Read only data copied and mapped to SPIRAM      <- 17ms
I (452) mmu_psram: Instructions copied and mapped to SPIRAM        <- 71ms
```

against 836ms from reset to user code. Worth having the number, and worth
noticing what is next to it in the same log: `CONFIG_SPIRAM_MEMTEST` costs
**376ms** on this board, four times the copy, and has nothing to do with any of
this.

Note how the memory cost reads. Nothing "used" 1075K — the region is reserved *before*
the heap is created, so it never becomes heap at all. `free` reports the heap,
so the change shows up as the **total** falling rather than the used column
rising. That also makes it the easiest way to check the setting took: 8189K of
PSRAM means the image is in flash, 7114K means it is in PSRAM.

**Turning it off is not the same as un-setting it.** The behaviour is gated on
`MMAP_EXECUTABLES_FROM_FLASH`, which is

```c
#define MMAP_EXECUTABLES_FROM_FLASH \
    (!((CONFIG_SPIRAM_FETCH_INSTRUCTIONS && CONFIG_SPIRAM_RODATA) || CONFIG_APP_BUILD_TYPE_RAM))
```

— `spi_flash/include/esp_private/flash_mmap.h:17`. `CONFIG_SPIRAM_XIP_FROM_PSRAM`
is a convenience that *selects* those two, and Kconfig `select` does not
un-select: clearing the umbrella on its own leaves both children set and changes
nothing whatsoever. Worth knowing before spending a build cycle proving a
negative that is not one, which is how this was found.

### Reading a CacheError panic: "cache disabled" is a guess, not a reading

Two traps here, and between them they cost a day.

**`exccause 71` is not a real Xtensa cause.** The table in
`panic_arch.c` stops at 38, so 71 decodes as "Unknown" and gdb will not help.
It is a *pseudo*-cause: `core_dump_port.c` does
`s_exc_frame->exccause += XCHAL_EXCCAUSE_NUM` (64) for these, so

```
71 == PANIC_RSN_CACHEERR (7) + 64
```

Same arithmetic for the others in `xtensa/include/esp_private/panic_reason.h`.
Useful in a core dump, where `EXCVADDR` is 0 and there is nothing else to go on
— and useful as a discriminator, because a wild pointer gives you
LoadProhibited or StoreProhibited with a real faulting address instead.

**"CacheError" is seven different faults.** From
`esp_system/port/soc/esp32s3/cache_err_int.c`, `esp_cache_err_get_panic_info()`
walks the status bits and prints the first that is set:

| cause | typically means |
|---|---|
| Icache/Dcache **sync** parameter configuration error | a manual writeback/invalidate given an address or size the hardware rejects |
| Icache/Dcache **preload** parameter configuration error | same, for a preload |
| Write back error … dcache tries to **write back to flash** | a dirty line whose address maps to read-only flash |
| **MMU entry fault** | access through an unmapped MMU entry |
| Dbus write to cache **rejected** | a write to a region the Dbus will not take |
| *(no bit set)* | **"Cache disabled but cached memory region accessed"** |

That last one is the **fallback**, printed when none of the others matched — and
it is also the one everybody quotes, so it is easy to assume a cache error means
a disabled cache when it means one of six other things.

**And the string exists in exactly one place: the UART.** It is not in the core
dump. espcoredump's `PANIC_DETAILS` note is only ever written for watchdog
panics (`elf_add_wdt_panic_details`), so after a reboot all seven look identical
— faulting task, a PC, and `exccause 71`. Capture the console *before*
reproducing, or the reproduction is wasted: `tools/serlog.sh`, and run the suite
without `--port` so nothing competes for the device (macOS lets two readers open
the same `cu.*` and simply splits the bytes between them).

The same non-exclusivity bites a *flash read*: esptool drives the stub through
that driver, and a core-dump read through `cu.*` can fail at the last block with
`Corrupt data, expected 0x1000 bytes but received 0x9d`. Use the `tty.*` node,
which can only be held by one process, and slow it down while you are there:

```
ESPBAUD=115200 ./tools/idf.sh -p /dev/tty.usbserial-210 coredump-info -s /tmp/core.bin
```

`-s` keeps the bytes, so a failure to *decode* does not cost another read, and
`coredump-info -c /tmp/core.bin` then needs no serial port at all.

Better still, that is now one command: `make coredump` reads the whole partition
at 115200 and decodes from the file (`tools/coredump.sh`), and
`tools/port-holder.sh <port>` answers "who has this port" in one line — the check
every tool makes before opening one, since two readers on a `cu.*` device split
the byte stream between them.

### The task watchdog watches IDLE, and IDLE is not what you think

`CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0/1` subscribe the **idle tasks**, so
"task watchdog got triggered ... IDLE0" does not mean the idle task is broken.
It means something else held that core for the whole timeout (5s by default) and
IDLE, at priority 0, never became the highest-ready task. FreeRTOS is preemptive
but not fair: a runnable task at any priority above 0 starves IDLE on its core,
by design.

**It is not necessarily a reboot.** `CONFIG_ESP_TASK_WDT_PANIC` is off by
default, and off in espix: the ISR prints the offending tasks and a backtrace and
execution continues. Check that config before assuming a watchdog line explains a
reset — it usually does not.

**It is not cosmetic either**, and this is the part that is easy to wave away.
IDLE runs FreeRTOS's deferred cleanup: `prvCheckTasksWaitingTermination()` is
what actually frees the TCB and stack of a task that has called
`vTaskDelete()`. Starve IDLE and that memory is not reclaimed for as long as the
starvation lasts. Any design that creates and destroys tasks — espix makes one
per process and one per SSH connection — is leaning on the thing being starved.

**Linux does not do this, and the comparison is instructive.** There is no
idle-task watchdog there. The soft-lockup detector watches for *kernel* code
that fails to schedule for ~20s and the NMI hard-lockup detector for a CPU stuck
with interrupts off; a userspace process pinning a core at 100% for hours trips
neither, because the scheduler simply time-slices around it. The ESP-IDF
watchdog is closer to the soft-lockup detector, aimed at a system where a task
that never yields is usually a bug rather than a busy program.

**Counting triggers: `esp_task_wdt_isr_user_handler()`.** It is a *weak* symbol
that `task_wdt_isr()` calls between printing the running tasks and handling the
timeout. Defining it is **additive, not an override** — IDF's own reporting all
still happens, so there is no original behaviour to reimplement. It runs in ISR
context, and `task_wdt_isr()` is not itself in IRAM, so putting the hook in IRAM
buys nothing.

Prefer a counter to scraping the log. A log ring wraps; espix's is
`CONFIG_ESPIX_KLOG_LINES` entries and four parallel test workers churn it in well
under a minute, so a poller on a ten-second cadence can arrive after the evidence
is gone. espix counts in `espix_fault_wdt_count()` and reports it from `uptime`,
which is the line anything monitoring the machine already reads.

**What trips it in espix, and how that was established.** Not by arithmetic —
two derivations from login cost were wrong before the question was answered from
evidence. The watchdog ISR prints a backtrace to the **UART**, and with
`tools/serlog.sh` capturing it, three of four triggers in one session decoded to
the same stack:

```
pbkdf2_sha256 (espix_auth/auth.c) → hmac_half → psa_hash_finish
  → esp_sha_hash_abort → free() → multi_heap_free
```

PBKDF2 runs 20 000 iterations with no blocking call, and each one allocates and
frees internal DMA memory inside the PSA driver — see the next section, which
already counts that at 40 000 heap operations per password check. Eight
concurrent logins on two cores is enough to keep both above IDLE for five
seconds. So the trigger is a real workload saturating the machine, which is
exactly the case the watchdog cannot distinguish from a stall.

Two limits worth carrying: the fourth trigger decoded to the **wifi** task in
`pm_tbtt_process` → `esp_phy_enable`, the modem-sleep wake path, so not every
trigger is PBKDF2. And the ISR prints CPU 0's backtrace, which is not
necessarily the core that starved.

The general lesson is about the capture, not the crypto: the backtrace exists
only on the console. A watchdog trigger observed without `tools/serlog.sh`
running gives you a task name and nothing else, which is where this question sat
for two days.

### `psa_hash_clone()` allocates, and X25519 has a fast path you cannot reach

Two crypto-performance traps on ESP-IDF, both found by measuring a slow SSH
login rather than by reading anything.

**Cloning a PSA hash operation is not a struct copy.** It reads like one, and
`mbedtls_sha256_clone()` genuinely is one, but ESP-IDF's PSA SHA driver does
`heap_caps_malloc(ctx_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)` on every
clone (`psa_crypto_driver_esp_sha.c:386`) and frees it at finish.
`psa_hash_setup()` does the same at `:161`. Anything that hashes in a tight loop
is therefore allocating from the **scarcest pool on the part** at the loop rate:
PBKDF2 at 20000 iterations does it 40000 times per password check.

Measured cost of the allocation alone: ~200 ms of a 1116 ms derivation. Worth
knowing before assuming a hash loop is bounded by hashing.

Also worth knowing what is *not* the fix: turning `CONFIG_MBEDTLS_HARDWARE_SHA`
off measured slightly **faster** (1936 ms vs 2030 ms on the same workload),
because the per-operation overhead swamps what the accelerator saves. An
accelerator only helps if you can reach it at the right granularity.

**X25519 runs through mbedtls's generic ECP path**, at roughly 140 ms per
scalar multiplication on a 240 MHz S3 — and the S3 has no ECC accelerator at all
(`SOC_ECC_SUPPORTED` is absent; `MBEDTLS_HARDWARE_MPI` assists the big-integer
arithmetic and that is the extent of it). `CONFIG_MBEDTLS_ECP_FIXED_POINT_OPTIM`
helps only base-point multiplication — ECDSA's k·G, where it took a nistp256
signature from 593 ms to 319 ms — and does nothing for the variable-point half
of a key agreement.

mbedtls ships **Everest**, a much faster formally-verified Curve25519, and it is
already compiled into an IDF build (`libeverest.a` is in `build/`). But
`MBEDTLS_ECDH_VARIANT_EVEREST_ENABLED` is commented out in `crypto_config.h`,
IDF exposes no Kconfig for it, and there is no supported hook for adding an
mbedtls define — so reaching it means patching a vendored config, and it is
unverified whether PSA's `psa_raw_key_agreement()` would route to it even then.

### Flash auto-suspend is the tidier fix, and depends on your flash chip

`CONFIG_SPI_FLASH_AUTO_SUSPEND` is the option that makes the machine behave the
way a general-purpose computer does: the flash chip suspends an erase or program
to service a read, so the cache never has to be disabled at all — no XIP, no
megabyte in PSRAM, code stays where it is. In `spi_flash_os_func_app.c` it takes
a mutex and nothing else; the whole `cache_disable()` branch is compiled out.

It is off by default, and the Kconfig is unusually cautious about it — "READ
DOCS FIRST", "supported only for specific flash chips", "contact Espressif
Business support to check if the module has the flash that supports this
feature". The reason is that it needs the *flash chip*, not just the SoC, to
implement suspend/resume.

Two things decide whether it is available to you, and both are checkable in a
minute:

- **The SoC.** `CONFIG_SOC_SPI_MEM_SUPPORT_AUTO_SUSPEND` — set on the S3. It
  also requires `CONFIG_SPI_FLASH_ROM_IMPL` to be off.
- **The flash chip.** Read the manufacturer byte with
  `esptool flash-id`, then look at the matching `spi_flash_chip_*.c` in
  ESP-IDF: the driver either sets `SPI_FLASH_CHIP_CAP_SUSPEND` in its
  `get_caps` or it does not. Only the **GigaDevice** and **Winbond** drivers do.

This board reports manufacturer `0x68`, Boya, and
`spi_flash/spi_flash_chip_boya.c` says so in a comment before omitting the flag:

```c
// 32-bit-address flash is not supported
// flash-suspend is not supported
caps_flags |= SPI_FLASH_CHIP_CAP_UNIQUE_ID;
```

So it is unavailable here, and XIP from PSRAM is the fallback rather than the
first choice. On a Winbond or GigaDevice module it is worth evaluating first —
it costs no PSRAM and leaves the code in flash.

The threshold for "this goes through DMA" is low and differs by part — 256
bytes on the S3, 512 on the P4, 128 elsewhere
(`mbedtls/port/sha/core/include/esp_sha_internal.h`) — so "this buffer is too
small to be DMA'd" is not a safe assumption on any of them.

### The same code is not equally safe on the part next door

| | ESP32 | S2 | S3 | P4 | C5 | C6 |
|---|---|---|---|---|---|---|
| PSRAM usable as a DMA target | no | yes | yes | yes | yes | no |
| Crypto DMA realigns an unaligned external buffer for you | — | **no** | **no** | 16B | 16B | — |
| Internal RAM reached through L1 cache | no | no | no | **yes** | no | no |

From `components/soc/<target>/include/soc/soc_caps.h`. Two surprises in there:

- On the S2 and S3 there is **no** bounce-buffer path. `esp_sha_dma_process_ext()`
  exists but sits behind `SOC_GDMA_EXT_MEM_ENC_ALIGNMENT`, which those parts do
  not define — and even where it exists it checks the address only, never the
  size, and only with flash encryption on. The caller carries the rule alone.
- On the **P4**, internal RAM goes through L1 cache like everything else. The
  standard reassurance — "internal RAM is DMA-capable and cache-coherent, so it
  needs no sync" — is true on the S3 and false there. espix's own comments in
  `espix_ssh/ssh_server.c` say exactly that, and would need revisiting for a P4
  port.

## Things that look like POSIX or like FreeRTOS, and are not

### "IRAM" is internal RAM, and its free space is your heap

The naming invites the wrong mental model, and the wrong model leads to a plan
that cannot work.

There is **one** pool of internal SRAM — 512KB on the S3. Most of it is
**DIRAM**: dual-ported, reachable both as instruction memory (the IRAM bus) and
as data memory (the DRAM bus). "IRAM" is not a separate chip and not a separate
budget; it is internal SRAM addressed as code. `idf.py size` splits it in a way
that is easy to misread:

| | used | remaining | total |
|---|---|---|---|
| IRAM | 16,384 | **0** | 16,384 |
| DIRAM | 173,770 | 167,990 | 341,760 |

The `IRAM` row is a small dedicated slice holding vectors and IDF's IRAM-safe
code, and it is full. The `DIRAM` row is the dual-ported pool — and **its
"remaining" is the heap**. Every byte of code you move into internal RAM is a
byte `malloc()` will not have.

That is the trap: "put the hot code in IRAM" reads like spending spare capacity
and is actually spending the heap. On espix the heap is what decides how many
SSH sessions fit, at about 12K each — so the question "should the kernel run
from internal RAM?" is really "how many sessions is this worth?", and for 864KB
of `.text` against a 512KB pool the answer is that it does not fit at any price.

What internal RAM genuinely buys is the thing `IRAM_ATTR` exists for: code there
is not fetched through the flash cache, so it keeps running while the cache is
off. Once XIP from PSRAM is on, the cache does not go off, and that reason is
gone. Speed is the only motive left — and moving espix's code out of the flash
cache into PSRAM made scp *faster*, so PSRAM fetch was not the bottleneck.

### `IRAM_ATTR` in an app loaded from the filesystem does nothing

`IRAM_ATTR` is an instruction to the **linker**, at build time: put this
function in internal RAM. It works for code linked into the firmware.

Code loaded at runtime is never linked into the firmware. espix's ELF loader
reads the file and copies it into memory itself, through one allocator for every
section (`managed_components/espressif__elf_loader/src/esp_elf_adapter.c:33`):

```c
void *esp_elf_malloc(uint32_t n, bool exec)
{
#ifdef CONFIG_ELF_LOADER_LOAD_PSRAM
    caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
```

One `caps` for everything, and the `exec` argument ignored outright in that
branch.

`IRAM_ATTR` expands to `section(".iram1.N")` (`esp_attr.h:24`), and there are two
ways that ends badly, both silent. Measured on the shipped `apps/neopixel`,
which marks its WS2812 output routine `IRAM_ATTR` because the bit timing is
tight: the built ELF has **no `.iram1` section at all** — `readelf -S` shows only
`.text`, `.rodata`, `.data`, `.bss` — so the app's link folded those functions
into `.text`, and they load and run from PSRAM like everything else. Had the link
preserved the section instead, it would be worse: the loader captures sections by
name (`.text`, `.data`, `.rodata`, `.dram0.*`, `.bss`) and silently skips
anything else, so the code would not be loaded at all.

Either way the attribute compiles, links, loads, runs, and delivers nothing.

**What that costs is smaller than it looks, since XIP.** The usual reason IDF
code marks an ISR `IRAM_ATTR` is so it survives the cache being disabled during a
flash write — and with `CONFIG_SPIRAM_XIP_FROM_PSRAM` the cache is not disabled,
so a PSRAM-resident handler in an app keeps working for the same reason the
kernel's does. The remaining reason is **timing**: instruction fetch from PSRAM
is slower and jitterier than from internal SRAM, so a handler with a
sub-microsecond deadline — bit-banging WS2812, say — can still miss it where an
IRAM one would not.

This matters for espix specifically because two of the kinds of code it wants to
run use the attribute idiomatically: Arduino sketches with `attachInterrupt`
handlers, and IDF examples pasted in more or less unchanged. Both will build and
load; neither gets what it asked for. Honouring it would mean giving the loader
an IRAM section slot — the shape is already there, since `esp_elf_malloc()`
takes an `exec` flag and the non-PSRAM branch already uses `MALLOC_CAP_EXEC` —
plus an app link that preserves `.iram1.*`. See [ROADMAP.md](ROADMAP.md).

### `xTaskCreate()` takes a stack size in **bytes**

Vanilla FreeRTOS takes words. IDF takes bytes, and the header shouts it:
`freertos/include/freertos/task.h:575`, *"the stack size DEFINED IN BYTES"*.
Ported code is quietly given a quarter of the stack it asked for.

### A file descriptor from the VFS has to fit in a byte

`esp_vfs`'s `local_fd_t` is `uint8_t` on every target but Linux
(`vfs/private_include/esp_vfs_private.h`), so a filesystem fd is 0–255. espix's
device nodes sit at 240–255 for that reason, and refuse a lower-filesystem fd
that reaches into the range rather than letting it alias. A design that reserved
`0x1000` "to be obviously distinct" would have been truncated to 0 and aliased
the first open file.

### Raising the socket count moves existing descriptors

`LWIP_SOCKET_OFFSET` is `FD_SETSIZE - CONFIG_LWIP_MAX_SOCKETS`, so more sockets
means lwIP's descriptors start *lower*, in the range the VFS was using — not
that new ones appear on top. Harmless when everything is recompiled together,
and confusing the moment an fd number is stored or logged.

### The FreeRTOS run-time counter is 32-bit microseconds

`xPortGetRunTimeCounterValue()` is `(configRUN_TIME_COUNTER_TYPE)
esp_timer_get_time()`, and the type is 32 bits — so it wraps every **71.6
minutes**. Anything computing a lifetime CPU share has to be wrap-aware, or it
reports nonsense once an hour.

### `TaskHandle_t` is an address, and addresses get reused

A handle is the TCB's address, freed on `vTaskDelete()` and very often handed
straight to the next task created. Anything that remembers a handle across a
task's lifetime — a previous sample, a map, a cache — will eventually match a
dead task to a live one. espix's `top` reported 386491% CPU that way.

### ROM functions take no locks

`espcoredump` checksums with the ROM SHA — `ets_sha_enable()`, `ets_sha_init()`,
… `ets_sha_disable()` — on every target but the original ESP32
(`espcoredump/src/core_dump_sha.c`). Right for the panic path they were written
for; wrong for `esp_core_dump_image_check()`, which is a public API callable
from a task. It resets the SHA peripheral under whatever else is using it. The
general lesson: a `ets_`/ROM entry point is not a locked API, wherever you find
one being called at runtime.

### Every SSH command runs on the connection task's stack

`sshd:conn` is created with `CONFIG_ESPIX_SSH_TASK_STACK`, and the shell session
runs on that stack — so a command's frames and the task's are the same budget.
`sudo` makes it worse: it re-enters the shell inside the same task, paying the
line-parsing frames twice.

The console does not have this problem. `main` carries the interactive session
with 8700 bytes, while the same shell over SSH lives in whatever the Kconfig
says — which was 8192, and a `sudo mount` overflowed it: 7680 bytes used, 508
free, caught by the stack canary and reported by the core dump as a stack
overflow in `sshd:conn`.

That makes the stack cost of a command part of its contract, and `ps` the place
to read it: the STACK column is `usStackHighWaterMark`, the smallest free figure
the task has ever reached and not a current reading. A command that leaves a few
hundred bytes there will overflow on a slightly different path.

### The VFS fd table is sized by the socket count, and three other things it will not tell you

Four IDF behaviours that cost espix a bug each, all in the space where a stacking
VFS meets `esp_vfs`:

- **`MAX_FDS` is `FD_SETSIZE` is `MEMP_NUM_NETCONN` is
  `CONFIG_LWIP_MAX_SOCKETS`** (`esp_vfs.h:42`, `lwipopts.h:124`,
  `lwip/sockets.h:478`). The fd table is as wide as the socket budget, so every
  open file competes with every socket. A build with 20 sockets has twenty fds in
  total — stdio, sockets and files together.
- **A VFS registered without a path receives no paths at all.** It is stored with
  `path_prefix_len = LEN_PATH_PREFIX_IGNORED` (`vfs.c:431`), and
  `get_vfs_for_path()` skips those entries by name (`vfs.c:840`). Registering one
  that way is how espix stopped receiving `mkdir`: the rootfs mounted, and then
  not one directory could be created in it, which reads as a filesystem failure
  rather than a registration one.
- **`esp_vfs_unregister_fd()` only frees entries registered `permanent = true`**
  (`vfs.c:679`). A driver that allocates an entry per open must ask for permanent
  or nothing can ever free it — one table entry lost per open, and a table full
  after a boot's worth of them.
- **`local_fd` is opaque and stored verbatim** (`vfs.c:764`), and IDF hands it
  back on every call. It does *not* have to be a number IDF allocated, which is
  what lets a driver with several filesystems behind it hand out keys of its own
  instead of borrowing the lower filesystem's numbering.

Worth knowing on top of those: a path-based open costs **two** entries for one
file — IDF's own, for the number the driver returned (`vfs_calls.c:56`), and
whatever the driver allocated itself — and IDF's close releases only its own.

## Build and configuration

### `sdkconfig` wins, and it is usually not in version control

Changing a `default` in a `Kconfig`, or adding a line to `sdkconfig.defaults`,
does **not** change an existing `sdkconfig` — the existing value wins, silently.
A change can therefore be correct in every tracked file and absent from the
build, and the only symptom is behaviour that does not match the source.

espix's answer is not to trust the file: `tests/suites/55-sessions.sh` reads the
session limit back off the device, out of the refusal banner, and fails if it
disagrees with what the tree says.

### `idf.py` is often a shell function

Which makes it invisible to a `make` recipe's subshell, and `IDF_PATH` alone is
not enough to reconstruct it. espix shells out to `tools/idf.sh`, which finds an
SDK for itself.

### A DEBUG log level buries crash reports

`CONFIG_LOG_DEFAULT_LEVEL` at DEBUG does not merely allow debug output:
espcoredump's macros gate on `LOG_LOCAL_LEVEL` at compile time and write
straight to `esp_rom_printf`, bypassing the runtime level — so every crash
arrives under a page of core-dump tracing.

### A write that "succeeded" has not been written yet

`fwrite()` returns the length it was handed as soon as the bytes are in stdio's
buffer; the file is touched when that buffer fills or the stream is closed. A
program that checks `fwrite()` and not `fflush()`/`fclose()` therefore reports
success for a copy whose real write failed — and on a filesystem that refuses
writes, success for a file of zero bytes.

Found here by doing exactly that: `cp` into a freshly mounted FAT volume checked
`fwrite()` and ignored `fclose()`. The shell's `>` and `2>` had the same hole in
`redirects_release()`. Both check now and both name the errno — and that is how
the empty file turned out *not* to be this bug. Six of seven copies wrote their
fifteen bytes; one arrived as an empty file, and the `fclose()` that would have
reported a failure returned success. What is left is in
[KNOWN-ISSUES.md](KNOWN-ISSUES.md#filesystem), and
[UPSTREAM.md](UPSTREAM.md) explains why the reporting had to come first: the
layer below throws away the sense data that would have named the cause.

### A configure-time hook is not a build-time guarantee

`tools/patch-fatfs.py` runs from an `execute_process()` in the top-level
`CMakeLists.txt`, which by default runs **when CMake configures**, not when the
build runs. So a patched dependency can be reverted — a fresh IDF install, an
upgrade, or `git checkout` — and the next `make build` will not re-apply it: CMake
sees no reason to configure again, ninja compiles the reverted tree, and the
failure arrives as an undefined reference pointing at *espix's* code rather than at
the missing patch. That is not hypothetical here; it is exactly what the first
version of the fatfs hook did.

Telling CMake which files the hook owns is the fix, and it is one property:

    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${IDF_PATH}/components/fatfs/vfs/vfs_fat.c"
        "${IDF_PATH}/components/fatfs/vfs/vfs_fat_internal.h")

A patch *script* has the same shape of problem in the other direction: it must be
idempotent (this one checks for its own symbols and exits quietly when they are
there) and it must fail loudly when the thing it patches has moved, because a
patch that applies to the wrong version is worse than one that does not apply at
all. The IDF version and every anchor are checked, and a mismatch stops the build
with the reason.


## How to add to this

### Deviations get a marker, not just prose

espix looks like POSIX from the outside and is not, in places, and the cost of
finding that out one trap at a time is a debugging session. The live example is `fstat()`: `stat()` answers the owner from the ownership rule,
and `fstat()` still reports `st_uid` and `st_gid` as 0 -- so a program that opens a
file and asks who owns it is told root.

So a place that looks POSIX-shaped and is not gets a marker at the code site:

```c
/* ESPIX_NOT_POSIX: st_uid and st_gid are always 0; ownership comes from
 * espix_fs_owner(). See the POSIX surface table in docs/ROADMAP.md. */
```

`ESPIX_NOT_POSIX:` is spelled for `grep -rn`, the way `RESOURCES:` is in the test
suites, because the point is to be able to ask the question:

```
grep -rn ESPIX_NOT_POSIX components/
```

and get the whole list rather than the parts anyone happened to remember. It is
for **API-shaped** deviations — a function, a type, a field somebody will assume
means what POSIX means — not for a command's choice of column heading.

The prose stays here, saying what bites and how to recognise it; ROADMAP's table
says what to do about it and which layer would have to change. Both are needed:
one is how you stop being confused, the other is how it gets fixed.

One heading per gotcha, with what it broke and where the claim comes from. If it
is only true on some parts, say which. If it was measured rather than
documented, say that too.

## `vTaskDelete(other)` runs the victim's teardown somewhere else

Deleting another task does not tidy up "in the kernel". `prvDeleteTCB()` runs
**on the caller** when the victim is not currently running, and on **IDLE** when
it is (`FreeRTOS-Kernel/tasks.c`). Its `configDEINIT_TLS_BLOCK` step is
`_reclaim_reent()` -> `esp_cleanup_r()`, which fcloses every std stream in the
dead task's reent that differs from the global one. If those streams write to
anything that can block -- espix's do, they are `funopen()` objects over the SSH
channel -- then the *caller* blocks, or IDLE does, and a blocked IDLE takes the
board down because the scheduler assumes it can always fall back to it.

Three consequences worth knowing before deleting any task:

- **Put the global streams back first** if the task replaced its stdio.
  `espix_proc_detach_streams()` does this; a clean exit always did, which is why
  only force-kill was ever unsafe.
- **A mutex held at deletion is not just stuck, it is poison.** Only the owner
  may give a FreeRTOS mutex back, and `xQueueSemaphoreTake()` reaches into the
  recorded holder's TCB for priority inheritance -- freed memory. Bounding the
  wait does not help; it only swaps silent corruption for
  `vTaskPriorityDisinheritAfterTimeout` asserting. The answer is to never ask:
  mark the lock orphaned and test that before taking it.
- **A task blocked in lwIP leaves a dangling netconn.** It is waiting on
  `conn->op_completed`; delete it and close the socket, and `tcpip` signals a
  freed semaphore.

The general form: FreeRTOS owns none of what a process holds -- newlib owns the
FILEs, lwIP the netconn, espix the channel lock -- so `vTaskDelete()` is not a
SIGKILL. Terminate cooperatively at the syscall boundary and keep the delete for
a task that demonstrably holds nothing. See `espix_proc_stopping()`.

## The USB work task has a stack budget, and filesystem work runs on it

`usb:work` is created with `WORK_TASK_STACK` in components/espix_usb/host.c, and
it is where the attach hook fires -- deliberately, because the library's own task
cannot block on transfers that only it can deliver. Whatever the hook does runs
on that stack.

`blk`'s `/etc/fstab` applier mounts volumes from that hook, and FatFs is deep.
Measured with `ps`, reading the task's high-water mark:

| what the attach ran | peak use |
| --- | --- |
| the install alone, before /etc/fstab existed | ~740 bytes |
| the applier reading fstab and creating the file | ~3228 bytes |
| the same, with a mount in the path | ~4268 bytes |
| the same, matching an identity in /etc/fstab | ~4540 bytes |

At 4096 the middle row was already fatal once the device array sat on the stack.
The panic said `***ERROR*** A stack overflow in task usb:work has been detected.`
and the board reboot-looped for as long as a stick was attached, because the
attach that overflows happens again on every boot -- and the UART that would have
said so is the one cable that is usually somewhere else. The array is `static`
now (attaches are serialised by the host's attach lock, so one buffer is enough)
and the stack is 6144.

The rule: **anything added to the attach hook is spending `usb:work`'s stack.**
If it is deeper than a mount, give it a task of its own rather than a larger
number here.

## The boot path's stack is full, and `main` has 8192 bytes

A change that added one call to `vfs_open()` -- an `espix_fs_owner()` to capture a
file's owner for `fstat()` -- put the board in a boot loop:

    ***ERROR*** A stack overflow in task main has been detected.

At boot `main` mounts the rootfs, runs `espix_auth_init()` and opens files, and the
permission check inside every one of those opens already computes the owner it asks
for. Looking it up again drags `espix_fs_owner()` -> `stat()` -> the layer below on
top of a chain that was already deep, and there was nothing left.

The figures are recorded as they are, because they do not yet add up: `main` is
8192 bytes (`CONFIG_ESP_MAIN_TASK_STACK_SIZE`) and `ps` reports about 5500 free
after a clean boot -- some 2.7 KB used at rest, against a chain of roughly a
kilobyte that overflowed it. Whatever the rest of it is, the rule the incident does
support is the one above: **a call added anywhere the boot sequence reaches is a
stack decision**, and the value wanted is often already sitting in a frame above
you. `access_check()` had the owner in `may()` and threw it away; handing it out
costs no frames at all.

Two diagnostics worth keeping. `tools/serlog.sh` caught it -- the panic goes to the
UART and nowhere else. And the backtrace on the first panic named IDF's core-dump
writer rather than the overflow, because the dump itself faulted and re-entered; in
a boot loop, read the *first* fault, and treat the frames below `|<-CORRUPTED` as
gone rather than as the answer.
