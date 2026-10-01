# Processes and the app ABI, through the test app.
#
# RESOURCES: none -- its /tmp paths carry the worker number, and the test app
# is staged once by the runner rather than by each suite that wants it.

if ! dev_testapp_present; then
    espix_skip "test app not built -- run 'make test-app'"
    return 0
fi
espix_pass "test app is present and current"

# Everything this suite makes on the device carries the worker number, so a
# second copy of it running beside this one is invisible to it.
APPTXT=/tmp/espix-app-$ESPIX_WORKER.txt

APP="/home/$ESPIX_USER/testapp"

# argv, which is the whole reason a loaded app is more than a script.
argv_out=$(dev_run "$APP argv one two")
assert_contains "argc counts the app's own name" "argc 4" "$argv_out"
assert_contains "argv[0] is the app path"        "argv[0] $APP" "$argv_out"
assert_contains "arguments arrive in order"      "argv[2] one"  "$argv_out"

# Exit status has to come from its own connection: the shell has no $?.
assert_status "an app's exit status reaches the client" 7 dev_status "$APP exit 7"
assert_status "and zero is zero"                        0 dev_status "$APP exit 0"

# The library exit path, which is a different path from the return above. The
# firmware's exit() ends at IDF's _exit() == abort(), so these used to reset the
# board rather than report a status. All of them are claimed by espix's symbol
# resolver now -- see components/espix_proc/abi_exit.c -- and the assertions
# that follow each one are also the evidence that the board survived it.
assert_status "exit() reaches the client" 5 dev_status "$APP exitcall 5"
assert_contains "and says so on the way out" "calling exit(5)" \
    "$(dev_run "$APP exitcall 5")"

assert_status "_Exit() reaches the client" 6 dev_status "$APP _Exit 6"

# 128 + SIGABRT.
assert_status "abort() reports 128 + SIGABRT" 134 dev_status "$APP abort"

# assert() is the one a symbol override could not have fixed on its own: a
# failing assert calls the *firmware's* __assert_func, which calls the
# firmware's abort at its own link time. __assert_func is claimed by the same
# table, so the wording is newlib's and the status is the same 134.
assert_status "a failing assert reports 128 + SIGABRT" 134 dev_status "$APP assert"
assert_contains "and prints newlib's own wording" "assertion" \
    "$(dev_run "$APP assert" 2>&1)"

# The whole point, and what the three above depend on being true afterwards.
assert_contains "the device is still up after all four" "espix" \
    "$(dev_run uname)"

# The file ABI: an app reaching the filesystem through libc, checked by espix.
dev_run "rm $APPTXT" >/dev/null 2>&1
assert_contains "an app can create a file" "ok" \
    "$(dev_run "$APP write $APPTXT hello-from-app")"
assert_contains "and read it back" "[hello-from-app]" \
    "$(dev_run "$APP read $APPTXT")"

# Permissions apply to apps exactly as to builtins -- checking in the shell
# alone would be a boundary you step around by running a program.
assert_contains "an app is refused /etc/passwd too" "EACCES" \
    "$(dev_run "$APP probe /etc/passwd")"
assert_contains "an app can read a world-readable file" "ok" \
    "$(dev_run "$APP probe /etc/hostname")"

# ps sees the processes that ran.
assert_contains "ps lists finished processes" "testapp" "$(dev_run 'ps')"

dev_run "rm $APPTXT" >/dev/null 2>&1

# ---------------------------------------------------------------------------
# Confinement: `confine <dir> <cmd>` -- the process may not name a path outside
# <dir>, and gets ENOENT rather than EACCES so it cannot map the filesystem by
# probing one path at a time.
#
# The controls matter as much as the checks. /etc/hostname is world-readable and
# is asserted readable above, so if the confined probe fails it is confinement
# doing it and not permissions -- otherwise this suite would pass just as well
# against a root that did nothing.
# ---------------------------------------------------------------------------

JAIL=/tmp/espix-jail-$ESPIX_WORKER
dev_run "rm $JAIL/inside.txt" >/dev/null 2>&1
dev_run "rm -r $JAIL"         >/dev/null 2>&1
dev_run "mkdir $JAIL"         >/dev/null 2>&1

assert_contains "a confined app reads a file inside its root" "ok" \
    "$(dev_run "confine $JAIL $APP write $JAIL/inside.txt hello")"

assert_contains "a confined app cannot see outside it" "ENOENT" \
    "$(dev_run "confine $JAIL $APP probe /etc/hostname")"

# The binary itself lives outside the root, which is not a loophole: it is read
# before the process exists, exactly where execve(2) draws the same line.
assert_contains "and the binary may live outside the root" "argc" \
    "$(dev_run "confine $JAIL $APP argv")"

# chmod does not enter the VFS -- abi_fs.c resolves its own paths and calls
# espix_fs_chmod() directly, because IDF's chmod is a stub. So the root has to
# be checked separately in espix_fs_admin_check(), and for exactly one commit it
# was not. This is that regression.
#
# The target has to be a file the app *could* chmod when unconfined, or the
# check proves nothing: an earlier version of this aimed at /etc/hostname, which
# is root-owned, so the confined chmod failed on permissions and would have
# passed just as happily with the root bypassed. Hence the control below -- the
# same call, the same file, differing only in the confinement.
OUTSIDE=/tmp/espix-outside-$ESPIX_WORKER.txt
dev_run "rm $OUTSIDE" >/dev/null 2>&1
dev_run "$APP write $OUTSIDE owned-by-the-app" >/dev/null 2>&1

assert_contains "control: unconfined, the app may chmod its own file" \
    "chmod $OUTSIDE ok" \
    "$(dev_run "$APP chmod $OUTSIDE 0644")"

assert_not_contains "chmod is not a way out of the root" \
    "chmod $OUTSIDE ok" \
    "$(dev_run "confine $JAIL $APP chmod $OUTSIDE 0600")"

dev_run "rm $OUTSIDE" >/dev/null 2>&1

# A root that is not there gives the process nothing at all, so say so rather
# than starting something that cannot reach anything.
assert_contains "confine refuses a directory that does not exist" "not a directory" \
    "$(dev_run "confine $JAIL/nope $APP argv" 2>&1)"
assert_contains "confine refuses a file"                          "not a directory" \
    "$(dev_run "confine $JAIL/inside.txt $APP argv" 2>&1)"

dev_run "rm $JAIL/inside.txt" >/dev/null 2>&1
dev_run "rm -r $JAIL"         >/dev/null 2>&1

# ---------------------------------------------------------------------------
# The app arena (R-P1.2). An app's malloc() now comes from PSRAM the process
# owns, and both a clean exit and a hard kill give all of it back. This is the
# Doom leak in miniature: two runs used to leave 571 KB of a 13.2 MB pool free,
# and only a reboot returned the rest.
#
# The measurement is PSRAM free, not the table: a slot whose accounting was
# zeroed would look identical after a kill however much memory it still held.
# ---------------------------------------------------------------------------

# PSRAM free in KB -- column 4 of the psram row of 'free'.
psram_free_kb() { dev_run free | awk '$1 == "psram" { print $4 }'; }

# The live arena bytes of the backgrounded test app, from the HEAP column of
# 'ps'; empty if it is not listed.
app_heap_bytes() { dev_run ps | awk '$2 == "app:testapp" { print $8; exit }'; }

app_running() {
    dev_run ps | sed -n '1,/^finished:/p' | grep -q "$1 app:testapp"
}

wait_app_gone() {
    local i
    for i in 1 2 3 4 5 6 7 8 9 10; do
        app_running "$1" || return 0
        sleep 1
    done
    return 1
}

assert_contains "ps has an arena column" "HEAP" "$(dev_run ps | head -1)"

base=$(psram_free_kb)
if [ -z "$base" ]; then
    espix_fail "free reports PSRAM" "no psram row"
    return 0
fi
espix_pass "free reports PSRAM free ($base KB)"

# --- a hard kill returns the arena -----------------------------------------

pid=$(dev_run "$APP hold 300 2000000 &" | sed -n 's/^\[\([0-9][0-9]*\)\].*/\1/p')
if [ -z "$pid" ]; then
    espix_fail "the hold app is backgrounded and reports its pid" "no [pid] line"
    return 0
fi

sleep 3
heap=$(app_heap_bytes)
if [ -n "$heap" ] && [ "$heap" -ge 1000000 ] 2>/dev/null; then
    espix_pass "ps shows the app's live arena (HEAP $heap bytes)"
else
    espix_fail "ps shows the app's live arena" "HEAP read '$heap'"
fi

held=$(psram_free_kb)
if [ -n "$held" ] && [ $((base - held)) -ge 1000 ]; then
    espix_pass "and PSRAM free fell with it ($base KB -> $held KB)"
else
    espix_fail "PSRAM free falls when an app holds memory" \
        "base ${base}K, holding ${held}K"
fi

dev_run "kill -9 $pid" >/dev/null
wait_app_gone "$pid" || espix_fail "the killed process leaves the table" \
    "pid $pid still running"

sleep 2
back=$(psram_free_kb)
if [ -n "$back" ] && [ $((base - back)) -le 1024 ]; then
    espix_pass "kill -9 returns the arena to PSRAM ($held KB -> $back KB, base $base KB)"
else
    espix_fail "kill -9 returns the arena to PSRAM" \
        "base ${base}K, holding ${held}K, after kill ${back}K"
fi

# --- a request too large for the first region makes a second ----------------

base2=$(psram_free_kb)
out=$(dev_run "$APP hold 0 524288 2097152")
assert_contains "an app holds blocks that need two arena regions" \
    "held 2 block(s)" "$out"

sleep 2
back2=$(psram_free_kb)
if [ -n "$back2" ] && [ $((base2 - back2)) -le 1024 ]; then
    espix_pass "both regions are returned on a clean exit (${base2}K -> ${back2}K)"
else
    espix_fail "both regions are returned on a clean exit" \
        "PSRAM free fell from ${base2}K to ${back2}K"
fi


# --- a free from a thread that has no espix slot ---------------------------
#
# The one arena path that cannot be reasoned about from the main task. The
# thread is not the process, so espix_proc_self() finds no slot and the region
# has to be found by address alone; without that walk this free would reach the
# global heap on a region pointer -- corruption, not a leak.

base3=$(psram_free_kb)
out=$(dev_run "$APP holdthread 1048576")
assert_contains "a free from the app's own thread finds the arena" \
    "freed in a thread" "$out"

sleep 1
back3=$(psram_free_kb)
if [ -n "$back3" ] && [ $((base3 - back3)) -le 1024 ]; then
    espix_pass "and the arena is still returned (${base3}K -> ${back3}K)"
else
    espix_fail "a thread free leaves the arena intact" \
        "PSRAM free ${base3}K -> ${back3}K"
fi

assert_contains "the device survives a thread free" "espix" \
    "$(dev_run uname)"

