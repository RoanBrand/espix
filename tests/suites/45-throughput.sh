# Transfer rates, and a floor under them.
#
# espix had no throughput measurement at all, which is how a process's stdin
# came to run at 5 KB/s without anyone noticing. The numbers are printed every
# run; the floors are set at roughly half of what was measured on a quiet
# network, because a tight floor flaps on WiFi variance and a benchmark that
# cries wolf gets switched off.
#
# Nothing here writes to littlefs. Uploads go to /dev/null and downloads come
# from /dev/factory, so the suite can run as often as it likes without wearing
# flash -- littlefs's cost is in its writes.
#
# RESOURCES: exclusive -- these are measurements. They run alone, after the
# pool has drained and the device has settled, so the floors below mean the
# same thing in a parallel run as in a serial one.

if ! dev_testapp_present; then
    espix_skip "test app not built -- run 'make test-app'"
    return 0
fi

TMP=$(espix_mktemp_dir)
SMALL_KB=512
BIG_KB=2048
dd if=/dev/urandom of="$TMP/small" bs=1024 count=$SMALL_KB 2>/dev/null
dd if=/dev/urandom of="$TMP/big"   bs=1024 count=$BIG_KB   2>/dev/null

now_ms() { "$ESPIX_PYTHON" -c 'import time;print(int(time.time()*1000))'; }

# Two sizes, and the rate taken from the difference.
#
# A single transfer folds in the SSH handshake -- PBKDF2 at 20 000 iterations,
# comfortably a second -- which at these rates is a large slice of the answer.
# It is identical in both runs and cancels.

# The smallest difference between the two transfers that can mean anything.
#
# Below this the differential is noise and the rate it produces is arithmetic,
# not measurement. 1536KB at the 200 KB/s floor is 7.7 seconds, so even a
# transfer ten times faster than anything this device has managed leaves 770ms
# between the two -- a difference under 100ms means a transfer did not happen.
RATE_MIN_DT_MS=100

# Answers -1, never a number, when the measurement collapsed.
#
# It used to answer a number regardless, guarded only against dt <= 0, and a run
# reported `scp upload: 307200 KB/s (floor 200)` -- 300 MB/s over WiFi, from the
# 2MB upload finishing 5ms after the 512KB one. Both had failed: dev_push sends
# its errors to /dev/null, so a broken upload path costs no time, collapses the
# difference, and divides its way to an enormous rate that clears the floor.
#
# Which is the exact failure this suite exists to catch, passing. A check that
# goes green because its measurement broke is worse than no check -- the same
# lesson _dev_parse_uptime learned by inventing reboots out of a failed parse.
#
# The guard below catches the two transfers finishing too CLOSE. It cannot catch
# the mirror image, which is the small leg failing fast: that stretches the
# difference rather than collapsing it, and the rate inflates through the floor
# instead of through the ceiling. A run reported `ssh stdin: 1575 KB/s` -- seven
# times the honest figure -- from the 512K leg dying when the board panicked,
# while the assertion on that same leg's byte count was failing two lines above.
#
# So no arithmetic here can tell a good measurement from a bad one. Each caller
# has to establish that BOTH legs delivered what they were asked for, and skip
# the rate rather than compute one when they did not.
rate_kbs() {   # <ms for small> <ms for big> <small KB> <big KB>
    local dt=$(( $2 - $1 ))
    [ "$dt" -ge "$RATE_MIN_DT_MS" ] || { echo -1; return; }
    echo $(( ($4 - $3) * 1000 / dt ))
}

report() {     # <what> <rate> <floor>
    if [ "$2" -lt 0 ]; then
        espix_fail "$1: the measurement collapsed" \
                   "the two transfers finished within ${RATE_MIN_DT_MS}ms of each other" \
                   "which means one of them did not transfer -- not that it was fast"
    elif [ "$2" -ge "$3" ]; then
        espix_pass "$1: $2 KB/s (floor $3)"
    else
        espix_fail "$1: $2 KB/s is below the $3 KB/s floor" \
                   "a quiet network measured roughly twice this"
    fi
}

# ---------------------------------------------------------------------------
# scp, both directions. /dev/null and the firmware node keep littlefs out of it.
# ---------------------------------------------------------------------------

# Whether the destination even accepts a write, before timing writes to it.
#
# It does not, for an ordinary account, and that is why this measurement was
# reporting 307200 KB/s: every upload failed with "dest open /dev/null:
# Permission denied", both failures cost the same handshake, and the difference
# they were derived from was 5ms of noise. See docs/KNOWN-ISSUES.md.
#
# The error used to go to /dev/null on *this* side too, which is how a check
# whose whole purpose is noticing a broken transfer path managed not to.
if dev_push "$TMP/small" /dev/null >/dev/null 2>&1; then
    t0=$(now_ms); dev_push "$TMP/small" /dev/null >/dev/null 2>&1; rc_us=$?; t_us=$(( $(now_ms) - t0 ))
    t0=$(now_ms); dev_push "$TMP/big"   /dev/null >/dev/null 2>&1; rc_ub=$?; t_ub=$(( $(now_ms) - t0 ))

    # scp's own exit status is the only evidence available here: the destination
    # is /dev/null, so there is no byte count to compare against afterwards.
    if [ "$rc_us" -eq 0 ] && [ "$rc_ub" -eq 0 ]; then
        report "scp upload" "$(rate_kbs 0 $(( t_ub - t_us )) 0 $(( BIG_KB - SMALL_KB )))" 200
    else
        espix_skip "scp upload: not measured -- a transfer failed (small rc $rc_us, big rc $rc_ub)"
    fi
else
    espix_skip "scp upload: /dev/null refuses a write from this account (KNOWN-ISSUES)"
fi

t0=$(now_ms); dev_pull /etc/hostname  "$TMP/tiny" >/dev/null 2>&1; t_dt=$(( $(now_ms) - t0 ))
# A firmware-sized read that never touches littlefs. Which node exists depends
# on the table: the A/B build has ota0 (and no factory), the factory-only build
# has factory. Take whichever this image exposes -- and take its size from the
# device, because the slot is 4 MiB on the factory table and 0x1F0000 on the A/B
# one, so a hardcoded expectation would pin this test to a single layout.
fwdev=/dev/ota0
dev_run 'ls /dev/ota0' >/dev/null 2>&1 || fwdev=/dev/factory
fwsize=$(dev_run "ls -l $fwdev" 2>/dev/null |
             awk '{ for (i = 1; i <= NF; i++) if ($i ~ /^[0-9]+$/) { print $i; exit } }')
t0=$(now_ms); dev_pull "$fwdev" "$TMP/got" >/dev/null 2>&1; t_db=$(( $(now_ms) - t0 ))
tiny=$(wc -c < "$TMP/tiny" 2>/dev/null | tr -d ' ' || echo 0)
got=$(wc -c < "$TMP/got"  2>/dev/null | tr -d ' ' || echo 0)

# The reference leg is subtracted from the timed one, so a failure there is not
# neutral -- it shortens what gets subtracted and inflates the result. Its size
# is not asserted exactly because a hostname is allowed to change; that it
# arrived at all is the part this measurement depends on.
if [ "${tiny:-0}" -gt 0 ]; then
    espix_pass "the reference fetch arrived"
else
    espix_fail "the reference fetch arrived" "/etc/hostname fetched 0 bytes"
fi
assert_eq "the download arrived whole" "$fwsize" "$got"

if [ "${tiny:-0}" -gt 0 ] && [ -n "$fwsize" ] && [ "$got" = "$fwsize" ]; then
    report "scp download" "$(rate_kbs 0 $(( t_db - t_dt )) 0 $(( fwsize / 1024 )))" 180
else
    espix_skip "scp download: not measured -- a fetch did not arrive whole"
fi

# ---------------------------------------------------------------------------
# A process's stdin -- the one path scp does not touch.
#
# An sftp channel gets no stdin queue at all, so every scp number above would
# be unchanged with this path completely broken. It has been: 5 KB/s and
# truncating, from a 256-byte queue drained once per 50ms poll and a stream
# newlib was reading a byte at a time.
# ---------------------------------------------------------------------------

_ssh_in() { dev_ssh_raw "$@"; }

APP="/home/$ESPIX_USER/testapp"

t0=$(now_ms); out_s=$(_ssh_in "$APP sink" < "$TMP/small" 2>/dev/null); t_ss=$(( $(now_ms) - t0 ))
t0=$(now_ms); out_b=$(_ssh_in "$APP sink" < "$TMP/big"   2>/dev/null); t_sb=$(( $(now_ms) - t0 ))

# The count first: a transfer that ended early is a failure, not a fast result,
# and without this a truncated read reports as excellent throughput.
#
# Tested separately from the assertions rather than read back out of them:
# assert_contains reports a result, it does not return one, and the rate below
# must not be computed from a leg that did not deliver.
small_ok=0; case "$out_s" in *"sink: $(( SMALL_KB * 1024 )) bytes"*) small_ok=1 ;; esac
big_ok=0;   case "$out_b" in *"sink: $(( BIG_KB   * 1024 )) bytes"*) big_ok=1   ;; esac

assert_contains "stdin delivered every byte (512K)"  "sink: $(( SMALL_KB * 1024 )) bytes" "$out_s"
assert_contains "stdin delivered every byte (2048K)" "sink: $(( BIG_KB * 1024 )) bytes"   "$out_b"

if [ "$small_ok" = 1 ] && [ "$big_ok" = 1 ]; then
    report "ssh stdin" "$(rate_kbs 0 $(( t_sb - t_ss )) 0 $(( BIG_KB - SMALL_KB )))" 100
else
    espix_skip "ssh stdin: not measured -- the $([ "$small_ok" = 1 ] && echo 2048K || echo 512K) leg did not deliver its bytes"
fi

rm -rf "$TMP"
