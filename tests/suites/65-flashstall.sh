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

FS_BATCH=25         # commands per timed batch
FS_ERASERS=2        # concurrent erase loops during the busy batch
FS_CEILING=150      # busy batch may be at most this % of the quiet one

now_ms() { "$ESPIX_PYTHON" -c 'import time;print(int(time.time()*1000))'; }

# A batch of cheap commands over the session that is already open, timed as a
# whole. Two clock reads for the batch rather than two per command: python
# startup is tens of milliseconds and would swamp what is being measured.
timed_batch() {
    local i=0 t0
    t0=$(now_ms)
    while [ "$i" -lt "$FS_BATCH" ]; do
        dev_run 'uptime' >/dev/null 2>&1
        i=$((i + 1))
    done
    echo $(( $(now_ms) - t0 ))
}

# ---------------------------------------------------------------------------
# The control first: is the erase actually happening, and what does it cost?
#
# Without this the whole suite is vacuous -- if `coredump erase` were a no-op,
# the busy batch would match the quiet one and the test would pass by doing
# nothing. The comparison is against `uptime` over the same connection shape, so
# the login cost cancels and what is left is the erase.
# ---------------------------------------------------------------------------

t0=$(now_ms); dev_ssh_raw 'uptime'        >/dev/null 2>&1; t_noop=$(( $(now_ms) - t0 ))
t0=$(now_ms); dev_ssh_raw 'coredump erase' >/dev/null 2>&1; t_erase=$(( $(now_ms) - t0 ))
erase_ms=$(( t_erase - t_noop ))

if [ "$erase_ms" -ge 20 ]; then
    espix_pass "control: erasing 64KB of flash costs ${erase_ms}ms"
else
    espix_fail "control: erasing 64KB of flash costs measurable time" \
               "measured ${erase_ms}ms against a ${t_noop}ms no-op" \
               "if the erase is not happening, nothing below tests anything"
    return 0
fi

# ---------------------------------------------------------------------------
# Quiet, then the same batch with erases in flight.
#
# The comparison is inside one run, against the same board in the same minute,
# rather than against a number written down once -- WiFi latency moves too much
# between sessions for an absolute threshold to mean anything.
# ---------------------------------------------------------------------------

quiet_ms=$(timed_batch)

erase_loop() {
    while [ -f "$FS_FLAG" ]; do
        dev_ssh_raw 'coredump erase' >/dev/null 2>&1
    done
}

FS_FLAG=$(espix_mktemp_dir)/running
: > "$FS_FLAG"

fs_pids=""
i=0
while [ "$i" -lt "$FS_ERASERS" ]; do
    # Own connection, not the suite's session: a background subshell inherits
    # the session's descriptors, and two writers framing commands down one FIFO
    # pair interleave and wedge. dev_ssh_raw takes a connection of its own and
    # is budgeted against the device's session limit.
    erase_loop &
    fs_pids="$fs_pids $!"
    i=$((i + 1))
done

busy_ms=$(timed_batch)

rm -f "$FS_FLAG"
for p in $fs_pids; do wait "$p" 2>/dev/null; done
rmdir "$(dirname "$FS_FLAG")" 2>/dev/null

# ---------------------------------------------------------------------------

if [ "$quiet_ms" -le 0 ]; then
    espix_fail "the quiet batch took measurable time" "got ${quiet_ms}ms"
    return 0
fi

pct=$(( busy_ms * 100 / quiet_ms ))

if [ "$pct" -le "$FS_CEILING" ]; then
    espix_pass "flash erases do not stall the system (${quiet_ms}ms quiet, ${busy_ms}ms with $FS_ERASERS erasers -- ${pct}%)"
else
    espix_fail "flash erases do not stall the system" \
               "${quiet_ms}ms quiet, ${busy_ms}ms with $FS_ERASERS erasers -- ${pct}% of baseline, ceiling ${FS_CEILING}%" \
               "this is what CONFIG_SPIRAM_XIP_FROM_PSRAM buys; check it is still on" \
               "see docs/GOTCHAS.md, 'What a flash write actually stops'"
fi
