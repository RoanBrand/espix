# Does a flash erase stop the rest of the system?
#
# It used to, and that is not a small thing. ESP-IDF's default is that "caches
# are disabled during SPI1 operations (read/write) ... all non-IRAM-safe
# interrupts will be disabled, and all other tasks are suspended" -- so saving a
# file froze espix entirely for the length of the erase, and cache maintenance
# running at that moment did not stall but *faulted*, which is how four parallel
# suites came to panic the board with exccause 0x47 in Cache_WriteBack_Addr.
#
# CONFIG_SPIRAM_XIP_FROM_PSRAM fixed it by moving .text and .rodata into PSRAM
# at boot, so a flash write no longer needs the cache off. This suite is the
# only thing that would notice if that were ever turned back off: everything
# else about the system looks identical until something crashes.
#
# The erase generator is `coredump erase`, deliberately. It is a real
# esp_partition_erase_range() over the whole 64KB coredump partition
# (partitions/esp32s3-16mb.csv, 0x10000 at 0x410000 -- sixteen sectors),
# unconditional, and it touches littlefs not at all. Writing files to the
# filesystem would work too and would dirty it: LittleFS frees a block on delete
# without erasing it, so every later write to that block pays an erase, and
# there is no TRIM to undo it (see the throughput note in ARCHITECTURE.md). The
# coredump partition exists to be overwritten, so this borrows it.
#
# RESOURCES: exclusive -- it is a timing measurement, and it needs the device
# quiet to have a baseline worth comparing against.

if [ -z "${DEV_PROMPT:-}" ]; then
    espix_skip "no session"
    return 0
fi

# Never destroy a stored dump. The runner clears them between runs, so this is
# for the case where somebody is mid-investigation.
case "$(dev_run 'coredump')" in
    *"core dump: "*)
        espix_skip "a core dump is stored; not erasing it to run a timing test"
        return 0 ;;
esac

# What this can and cannot check, established by building it both ways.
#
# It cannot check the stall by timing. Two measurements were tried against a
# build with XIP genuinely disabled -- the step that turns a regression test
# into one known to test something -- and neither separates the builds:
#
#   a 64KB erase          506ms with XIP,  510ms without
#   a 25-command batch     99% with XIP,   102% without, against its own baseline
#
# An earlier run appeared to show 32ms against 400ms, and that was a measurement
# artefact rather than a finding: it opened a fresh connection per sample, so a
# 400ms quantity was being read out of a 3400ms login, and the same erase
# measured 192ms, -5ms and 401ms on consecutive tries. A negative time for an
# erase is the measurement saying it is not measuring. Over the already-open
# session the samples tighten to within 5% -- and say the two builds are the
# same.
#
# Read those numbers as "underpowered", not as "there is nothing to see". A
# 500ms stall inside a 25-command batch that takes several seconds over SSH is a
# few percent, which is inside the spread those two runs had -- the experiment
# could not have detected the thing it was looking for even if it were happening
# at full strength. That is a different statement from the one this comment used
# to make, and the difference matters: the original wording was later cited as
# evidence that XIP had made no difference.
#
# So the difference XIP makes is not visible in latency *at this load*. It is
# visible in whether the board crashes, and a suite cannot assert that.
#
# What it can check is two things that are observable directly. First, that the
# image really is in PSRAM: with .text and .rodata copied there at boot, the
# PSRAM heap starts about a megabyte smaller. Second -- and this is the one that
# actually decides whether a write disables the cache -- that no flash mapping
# is live. The first is a proxy for the second and does not imply it.
#
# Board-specific, deliberately, and it says so when it fails: on the N16R8 that
# is espix's target of record, `free` reports 8189K of PSRAM with the image in
# flash and 7114K with it in PSRAM. The S31's WROOM-3 is a 16 MB part, whose
# heap is 16384K in flash and about 14.6 MB with the image in PSRAM; the same
# 8 MB ceiling would read "not in PSRAM" for a board that is.

case "$ESPIX_TARGET" in
    esp32s31) FS_PSRAM_CEILING_K=15360 ;;
    *)        FS_PSRAM_CEILING_K=7800  ;;
esac
FS_SAMPLES=5
FS_BATCH=25
FS_ERASERS=2

now_ms() { "$ESPIX_PYTHON" -c 'import time;print(int(time.time()*1000))'; }
median_of() { printf '%s\n' "$@" | sort -n | sed -n "$(( ($# + 1) / 2 ))p"; }

# ---------------------------------------------------------------------------
# The assertion: is the image in PSRAM?
# ---------------------------------------------------------------------------

psram_k=$(dev_run 'free' | awk '/^psram/ {print $2}')
case "${psram_k:-}" in
    ''|*[!0-9]*)
        espix_fail "free reports a PSRAM total" "got '${psram_k:-}'"
        return 0 ;;
esac

if [ "$psram_k" -le "$FS_PSRAM_CEILING_K" ]; then
    espix_pass "the image is in PSRAM (${psram_k}K PSRAM heap)"
else
    espix_fail "the image is in PSRAM" \
               "PSRAM heap is ${psram_k}K, above the ${FS_PSRAM_CEILING_K}K ceiling" \
               "that is the size it has when .text and .rodata are still in flash" \
               "check CONFIG_SPIRAM_FETCH_INSTRUCTIONS and CONFIG_SPIRAM_RODATA --" \
               "note that clearing CONFIG_SPIRAM_XIP_FROM_PSRAM alone does nothing," \
               "because Kconfig select does not un-select what it selected" \
               "see docs/GOTCHAS.md, 'What a flash write actually stops'"
fi

# ---------------------------------------------------------------------------
# And the property the placement is only a proxy for.
#
# "The image is in PSRAM" does not by itself mean a flash write leaves the cache
# alone, and reading it as though it did is what let this go unmeasured for a
# week. IDF's actual condition is in spi_flash_os_func_app.c, spi1_start():
#
#     if (!(flags & NO_READ) || !flash_mmap_remain()) { keep the cache }
#     else                                            { cache_disable(NULL); }
#
# So a write or erase keeps the cache only while **no mmap region is live**, and
# s_mmap_remain_count is a plain global that only a matching munmap decrements.
# One mapping taken without ESP_PARTITION_MMAP_BLOCKS_WRITE stays counted for
# the rest of the boot and silently puts every later write back on the
# cache-disable path -- with the image still in PSRAM, this suite still green,
# and the panic back.
#
# `free` reports it because there is nowhere else to see it; see cmd_sys.c.
mmap_line=$(dev_run 'free' | sed -n 's/^flash mmap: //p')

case "${mmap_line:-}" in
    "none live"*)
        espix_pass "no flash mapping is live, so a write leaves the cache on" ;;
    "regions live"*)
        espix_fail "no flash mapping is live" \
                   "free says: flash mmap: $mmap_line" \
                   "a flash write will disable the cache while this is true, and" \
                   "esp_cache_msync() from SSH crypto faults when it does --" \
                   "exccause 71 in Cache_WriteBack_Addr, the panic XIP was meant" \
                   "to have closed. Find what mapped a partition and did not" \
                   "unmap it, or gave up BLOCKS_WRITE" ;;
    *)
        espix_skip "free does not report the flash mmap state (older firmware?)" ;;
esac

# ---------------------------------------------------------------------------
# The timings, reported and not asserted.
#
# Kept because they are what anyone will reach for first, and because having the
# numbers here saves them re-deriving that these do not discriminate. If a
# future change does make the stall measurable, this is where it will show.
# ---------------------------------------------------------------------------

i=0
noops=""
erases=""
while [ "$i" -lt "$FS_SAMPLES" ]; do
    t0=$(now_ms); dev_run 'uptime'         >/dev/null 2>&1; n=$(( $(now_ms) - t0 ))
    t0=$(now_ms); dev_run 'coredump erase' >/dev/null 2>&1; e=$(( $(now_ms) - t0 ))
    noops="$noops $n"
    erases="$erases $(( e - n ))"
    i=$((i + 1))
done

erase_ms=$(median_of $erases)

# One real assertion here after all: the erase has to be happening. If
# `coredump erase` were a no-op the numbers above would be meaningless, and so
# would any future attempt to time the stall with them.
if [ "$erase_ms" -ge 50 ]; then
    espix_pass "control: erasing 64KB of flash costs ${erase_ms}ms (samples$erases)"
else
    espix_fail "control: erasing 64KB of flash costs measurable time" \
               "median ${erase_ms}ms of$erases against round trips of$noops" \
               "if the erase is not happening, this suite measures nothing"
fi
