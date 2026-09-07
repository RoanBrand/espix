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
# PARALLEL_SAFE=no -- saturates the link.

if ! dev_testapp_sync "$ESPIX_ROOT/fsroot/home/$ESPIX_USER/testapp"; then
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
rate_kbs() {   # <ms for small> <ms for big> <small KB> <big KB>
    local dt=$(( $2 - $1 ))
    [ "$dt" -gt 0 ] || { echo 0; return; }
    echo $(( ($4 - $3) * 1000 / dt ))
}

report() {     # <what> <rate> <floor>
    if [ "$2" -ge "$3" ]; then
        espix_pass "$1: $2 KB/s (floor $3)"
    else
        espix_fail "$1: $2 KB/s is below the $3 KB/s floor" \
                   "a quiet network measured roughly twice this"
    fi
}

# ---------------------------------------------------------------------------
# scp, both directions. /dev/null and /dev/factory keep littlefs out of it.
# ---------------------------------------------------------------------------

t0=$(now_ms); dev_push "$TMP/small" /dev/null >/dev/null 2>&1; t_us=$(( $(now_ms) - t0 ))
t0=$(now_ms); dev_push "$TMP/big"   /dev/null >/dev/null 2>&1; t_ub=$(( $(now_ms) - t0 ))
report "scp upload" "$(rate_kbs 0 $(( t_ub - t_us )) 0 $(( BIG_KB - SMALL_KB )))" 200

t0=$(now_ms); dev_pull /etc/hostname  "$TMP/tiny" >/dev/null 2>&1; t_dt=$(( $(now_ms) - t0 ))
t0=$(now_ms); dev_pull /dev/factory   "$TMP/got"  >/dev/null 2>&1; t_db=$(( $(now_ms) - t0 ))
got=$(wc -c < "$TMP/got" 2>/dev/null || echo 0)
assert_eq "the download arrived whole" "4194304" "$(printf '%s' "$got" | tr -d ' ')"
report "scp download" "$(rate_kbs 0 $(( t_db - t_dt )) 0 4096)" 180

# ---------------------------------------------------------------------------
# A process's stdin -- the one path scp does not touch.
#
# An sftp channel gets no stdin queue at all, so every scp number above would
# be unchanged with this path completely broken. It has been: 5 KB/s and
# truncating, from a 256-byte queue drained once per 50ms poll and a stream
# newlib was reading a byte at a time.
# ---------------------------------------------------------------------------

_ssh_in() {
    espix_timeout "$ESPIX_SSH_TIMEOUT" \
        env SSH_ASKPASS="$DEV_ASKPASS" SSH_ASKPASS_REQUIRE=force DISPLAY=:0 \
        ssh $DEV_SSH_OPTS "$ESPIX_USER@$ESPIX_HOST" "$@"
}

APP="/home/$ESPIX_USER/testapp"

t0=$(now_ms); out_s=$(_ssh_in "$APP sink" < "$TMP/small" 2>/dev/null); t_ss=$(( $(now_ms) - t0 ))
t0=$(now_ms); out_b=$(_ssh_in "$APP sink" < "$TMP/big"   2>/dev/null); t_sb=$(( $(now_ms) - t0 ))

# The count first: a transfer that ended early is a failure, not a fast result,
# and without this a truncated read reports as excellent throughput.
assert_contains "stdin delivered every byte (512K)"  "sink: $(( SMALL_KB * 1024 )) bytes" "$out_s"
assert_contains "stdin delivered every byte (2048K)" "sink: $(( BIG_KB * 1024 )) bytes"   "$out_b"

report "ssh stdin" "$(rate_kbs 0 $(( t_sb - t_ss )) 0 $(( BIG_KB - SMALL_KB )))" 100

rm -rf "$TMP"
