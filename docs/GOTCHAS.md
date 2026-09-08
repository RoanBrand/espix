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
SSH sessions encrypting while another task read 4MB out of a flash partition.

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
