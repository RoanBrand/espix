# How many connections the device really accepts, and what it costs.
#
# This exists because the parallel runner spends the session limit: one session
# per worker, plus whatever each opens for a transfer or an exit status. Raising
# CONFIG_ESPIX_SSH_MAX_SESSIONS from 4 to 8 is only useful if the device can
# actually carry eight, and only *true* if the build picked the value up.
#
# That second part is the reason this suite reads the limit from the device
# instead of from a file. sdkconfig is not tracked, and an existing one wins
# over both the Kconfig default and sdkconfig.defaults -- so a checkout can
# quietly keep four while every file in the tree says eight, and the only
# symptom would be a parallel run that is mysteriously slow and full of
# retries.
#
# The limit is found by filling it, not by being told: connections are opened
# one at a time until the count stops going up. What the device *says* is then
# checked against what it *did*, which is the pair that catches an off-by-one
# in the accounting -- the accept loop used to check the limit and increment the
# counter in two different places, so it could be overshot by anything that
# connected fast enough.
#
# RESOURCES: exclusive -- it fills every session slot on the device.

SESS_CAP=20                 # never open more than this, whatever it says
SESS_BATCH=4                # how many to bring up at once
SESS_SETTLE=14              # seconds to let a batch finish logging in

if [ -z "${DEV_PROMPT:-}" ]; then
    espix_skip "no session"
    return 0
fi

sess_count() { dev_run 'ps' | grep -c 'sshd:conn'; }

# Is the session this suite counts through still there?
#
# It matters that this is asked rather than inferred. When the session goes,
# dev_run returns the dead-session sentinel, `grep -c` counts zero occurrences
# of sshd:conn in it, and every number below becomes 0 -- so the suite reports
# "the limit is 0" and "no memory came back", which are four wrong findings
# dressed as measurements. Losing the session while connections are being opened
# is itself worth knowing about, so it is said once, plainly, and the suite
# stops.
sess_alive() {
    [ "$(dev_run 'whoami')" = "$ESPIX_USER" ]
}

sess_free_line() { dev_run 'free' | sed -n 's/^internal *//p'; }

sess_pids=""
sess_hold() {
    # ssh itself, backgrounded -- never a shell function. Backgrounding a
    # function gives back a subshell pid, and killing that leaves the ssh
    # underneath alive holding a session slot until the device reboots. This
    # project has lost a day to precisely that; see portable.sh.
    env SSH_ASKPASS="$DEV_ASKPASS" SSH_ASKPASS_REQUIRE=force DISPLAY=:0 \
        ssh $DEV_SSH_OPTS "$ESPIX_USER@$ESPIX_HOST" 'top -b -n 200' \
        >/dev/null 2>&1 &
    sess_pids="$sess_pids $!"
}

sess_release() {
    local p
    for p in $sess_pids; do
        kill "$p" 2>/dev/null
        # Waited for one at a time, and never with a bare `wait`: this suite is
        # sourced into a shell that has session.py running in the background,
        # and `wait` with no argument waits for *that* too -- which never
        # returns. It hung a run for half an hour before it was noticed.
        wait "$p" 2>/dev/null
    done
    sess_pids=""
}

# Wait until the count stops rising, or the time is up.
#
# A fixed sleep per connection is not enough, and the reason is the thing being
# measured: a login costs a key exchange plus PBKDF2 at 20 000 iterations,
# comfortably a second and sometimes four with several arriving together. A
# four-second sleep read "the device refused that one" when the device was
# merely still doing the arithmetic, and the suite reported the limit as two.
sess_settle() {
    local i=0 now last=-1
    while [ "$i" -lt "$SESS_SETTLE" ]; do
        now=$(sess_count)
        case "$now" in ''|*[!0-9]*) now=-1 ;; esac
        [ "$now" -ge 0 ] && [ "$now" = "$last" ] && return 0
        last=$now
        sleep 1
        i=$((i + 1))
    done
    return 0
}

# ---------------------------------------------------------------------------
# Idle first, so the cost below is a difference and not an absolute.
# ---------------------------------------------------------------------------

free_idle=$(sess_free_line)
base=$(sess_count)
case "${base:-0}" in ''|*[!0-9]*) base=0 ;; esac

# This suite runs alone, but "alone" is two: its own session, and the runner's
# health monitor, which holds one for the whole run. Everything below is
# relative to `base` rather than to one, so the exact number does not matter --
# only that it is small enough to leave room to fill the rest.
if [ "$base" -lt 1 ] || [ "$base" -gt 3 ]; then
    espix_skip "$base connections already open; cannot measure the limit cleanly"
    return 0
fi
espix_pass "idle: $base connection(s) held by the harness, internal heap $free_idle"

# ---------------------------------------------------------------------------
# Fill it. One at a time, counting through the session we already have, so
# finding the limit costs no extra connections of its own.
# ---------------------------------------------------------------------------

# In batches, not one at a time. One at a time is the obvious way and it costs
# four minutes: a login is seconds, and the count has to be polled after each.
# A batch also does something the drip-feed cannot -- it opens several
# connections *simultaneously*, which is exactly the window the accept loop used
# to admit connections against a stale count, so this is the shape of load that
# the sessions_take() fix exists for.
held=0
full=""
while [ "$held" -lt "$SESS_CAP" ]; do
    i=0
    while [ "$i" -lt "$SESS_BATCH" ] && [ "$held" -lt "$SESS_CAP" ]; do
        sess_hold
        held=$(( held + 1 ))
        i=$(( i + 1 ))
    done
    sess_settle
    if ! sess_alive; then
        espix_fail "the session survives other connections being opened" \
                   "it was dropped while opening $held connections" \
                   "nothing below can be measured through a session that is gone"
        sess_release
        return 0
    fi
    if _dev_refused_for_capacity; then
        full=yes
        break
    fi
done

if [ -z "$full" ]; then
    espix_fail "the device refuses a connection once it is full" \
               "opened $held without ever being refused, which is past the cap"
    sess_release
    return 0
fi

# Whatever was opened past the limit was refused and has already exited, so the
# count is the answer rather than the number asked for.
sess_settle
last=$(sess_count)
case "$last" in ''|*[!0-9]*) last=0 ;; esac
espix_pass "the device fills up and then refuses ($last connections)"

# ---------------------------------------------------------------------------
# What it did, against what it says. RFC 4253 4.2 lets a server send lines
# before its version string; espix uses that to say why it hung up, and the
# count in the message is the configured limit rather than a guess.
# ---------------------------------------------------------------------------

banner=$(dev_banner)
assert_contains "a refused connection is told why" "too many connections" "$banner"

claimed=$(printf '%s' "$banner" | sed -n 's/.*of \([0-9][0-9]*\) in use.*/\1/p')
if [ -n "$claimed" ]; then
    assert_eq "the limit it claims is the limit it enforces" "$claimed" "$last"
else
    espix_fail "the limit it claims is the limit it enforces" \
               "no '(N of M in use)' in: $banner"
    claimed=$last
fi

# The floor the parallel runner needs. A stale sdkconfig fails here rather than
# turning up later as a slow run nobody can explain.
if [ "${claimed:-0}" -ge 8 ]; then
    espix_pass "the build allows at least 8 sessions (allows $claimed)"
else
    espix_fail "the build allows at least 8 sessions" \
               "it allows $claimed" \
               "sdkconfig wins over Kconfig defaults; delete it and rebuild"
fi

free_full=$(sess_free_line)
espix_pass "at $last connections, internal heap $free_full"

# Not a floor on the number, which differs per board, but on there being any
# left: a device with nothing spare has no room for the WiFi stack's own
# spikes, and the next allocation failure would look like anything but this.
free_k=$(printf '%s' "$free_full" | awk '{print $3+0}')
if [ "${free_k:-0}" -ge 20 ]; then
    espix_pass "and at least 20K of internal heap is still free (${free_k}K)"
else
    espix_fail "and at least 20K of internal heap is still free" \
               "only ${free_k}K left with $last connections open" \
               "lower CONFIG_ESPIX_SSH_MAX_SESSIONS for this board"
fi

# ---------------------------------------------------------------------------
# And it all comes back. A limit that is never reached hides a leak; a limit
# that is reached and not released turns into one connection slot fewer per
# run until the device answers nobody.
# ---------------------------------------------------------------------------

sess_release

# Wait for the count to come back rather than sleeping a guess at it.
#
# A fixed ten seconds was the guess, and it was wrong often enough to matter:
# close_gracefully() drains each connection until its peer hangs up, bounded at
# five seconds *each*, and eight of them closing together do not all finish
# inside one bound. The assertion is that the slots come back, not that they
# come back within some particular second, so it waits and then says how long
# it took.
released_after=0
i=0
while [ "$i" -lt 30 ]; do
    sleep 1
    i=$((i + 1))
    back=$(sess_count)
    case "$back" in ''|*[!0-9]*) continue ;; esac
    if [ "$back" -le "$base" ]; then
        released_after=$i
        break
    fi
done

back=$(sess_count)
if [ "$back" -le "$base" ]; then
    espix_pass "closing them frees the slots (back to $back after ${released_after}s)"
else
    espix_fail "closing them frees the slots" "expected: $base" "actual:   $back"
fi

free_after=$(sess_free_line)
after_k=$(printf '%s' "$free_after" | awk '{print $3+0}')
idle_k=$(printf '%s' "$free_idle" | awk '{print $3+0}')
if [ "${after_k:-0}" -ge $(( ${idle_k:-0} - 8 )) ]; then
    espix_pass "and the memory with them (${idle_k}K -> ${after_k}K free)"
else
    espix_fail "and the memory with them" \
               "was ${idle_k}K free before, ${after_k}K after" \
               "$(( idle_k - after_k ))K did not come back"
fi
