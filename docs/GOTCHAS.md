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

## How to add to this

One heading per gotcha, with what it broke and where the claim comes from. If it
is only true on some parts, say which. If it was measured rather than
documented, say that too.
