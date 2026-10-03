# The descriptor pool, measured alone.
#
# RESOURCES: exclusive -- fdprobe reports the *system-wide* free descriptors
# (IDF's MAX_FDS pool, shared with sockets). The check is that a process which
# exits holding half of them gives that half back, so a second probe must see
# the same number. Beside three other suites that number moves for reasons that
# have nothing to do with reclaiming, which is how this came to fail twice in
# six runs and pass alone every time. It waits for the quiet phase instead.

if ! dev_testapp_present; then
    espix_skip "test app not built -- run 'make test-app'"
    return 0
fi

APP="/home/$ESPIX_USER/testapp"

# --- a process gives its files back when it ends ---------------------------
#
# The descriptor an app holds is IDF's, not espix's, and it comes from a fixed
# MAX_FDS pool shared with sockets. A process killed with files open, whose
# descriptors nothing released, could exhaust that pool and stop the whole
# system opening a file -- which is what closing the layer below directly did,
# while both that close and espix's own slot reported success.
#
# The probe measures the capacity itself and holds only half of it, so it can
# never fill the table (a full table cannot load the next app's binary). Two
# runs must therefore report the same capacity.

FDFILE=/tmp/espix-fds-$ESPIX_WORKER.txt
dev_run "rm $FDFILE" >/dev/null 2>&1

first=$(dev_run "$APP fdprobe $FDFILE 8")
n1=$(printf '%s' "$first" | awk '/^capacity/ {print $2; exit}')
second=$(dev_run "$APP fdprobe $FDFILE 0")
n2=$(printf '%s' "$second" | awk '/^capacity/ {print $2; exit}')

if [ -z "$n1" ] || [ "$n1" -lt 8 ] 2>/dev/null; then
    espix_fail "the descriptor pool reports its capacity" "got '$n1' from: $first"
else
    assert_eq "a process that ends holding files gives them back" "$n1" "$n2"
    assert_contains "and the device is still up" "espix" "$(dev_run uname)"
fi

# And the reaper has to close every IDF entry a dup left on one key, not only
# the first: a process that exits holding three duplicates must cost the pool
# nothing. Same capacity, measured the same way as the pair above -- which is
# why it is here, with nothing else holding descriptors.
DUPFILE=/tmp/espix-dup-quiet.txt
dev_run "rm $DUPFILE" >/dev/null 2>&1
if [ -n "$n1" ] && [ "$n1" -ge 8 ] 2>/dev/null; then
    out=$(dev_run "$APP dup $DUPFILE hold" 2>&1)
    assert_contains "a process can end holding duplicated descriptors" \
        "dupped 3" "$out"
    n3=$(dev_run "$APP fdprobe $DUPFILE 0" | awk '/^capacity/ {print $2; exit}')
    assert_eq "and every duplicate is given back" "$n1" "$n3"
fi
dev_run "rm $DUPFILE" >/dev/null 2>&1
