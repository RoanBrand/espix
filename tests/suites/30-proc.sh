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


# --- the request list must never fail an ordinary allocation ---------------
#
# Each region is one fixed-size heap, so a request that will not fit beside an
# earlier block takes a region of its own. A fixed list therefore turns "the
# process is busy" into malloc() returning NULL with megabytes free -- which it
# did, at four entries: five 1 MB blocks failed the fifth. The list has no size
# now, so small allocations share a region, larger repeats get the regions they
# need, and all of it is still reclaimed at exit.

out=$(dev_run "$APP hold 0 1000 1000 1000 1000 1000 1000 1000 1000")
assert_contains "eight small allocations share one arena region" \
    "held 8 block(s), 8000 bytes" "$out"

out=$(dev_run "$APP hold 0 1000000 1000000 1000000 1000000 1000000")
assert_contains "five 1 MB allocations all succeed" \
    "held 5 block(s), 5000000 bytes" "$out"

base4=$(psram_free_kb)
out=$(dev_run "$APP hold 0 300000 300000 300000 300000 300000 300000 300000 300000 300000 300000")
assert_contains "many regions are tracked, with no fixed list to exhaust" \
    "held 10 block(s), 3000000 bytes" "$out"

sleep 2
back4=$(psram_free_kb)
if [ -n "$back4" ] && [ $((base4 - back4)) -le 1024 ]; then
    espix_pass "and the arena's regions still come back (${base4}K -> ${back4}K)"
else
    espix_fail "the arena's regions come back after many allocations" \
        "PSRAM free ${base4}K -> ${back4}K"
fi



# --- an app that never frees still gives everything back --------------------
#
# The case the arena exists for, and the one a test has to prove directly: the
# app deliberately exits holding three blocks it never frees. Before R-P1.2
# nothing recorded what an app took, so this memory was gone until a reboot.

base5=$(psram_free_kb)
out=$(dev_run "$APP leak 1048576 1048576 524288")
assert_contains "an app can exit without freeing what it holds" \
    "leaked 2621440 bytes" "$out"

sleep 2
back5=$(psram_free_kb)
if [ -n "$back5" ] && [ $((base5 - back5)) -le 1024 ]; then
    espix_pass "and the arena reclaims every byte (${base5}K -> ${back5}K)"
else
    espix_fail "the arena reclaims an app's leaked memory" \
        "PSRAM free ${base5}K -> ${back5}K"
fi



# --- memory an app's *thread* allocates belongs to the process too ----------
#
# R-P1.10. A thread is a different FreeRTOS task from the one that entered
# app_main(), so the allocator used to find no process for it and its malloc()
# went to the global heap -- unowned, and still there after the process ended.
# The command allocates in a thread, never frees, and returns.

base6=$(psram_free_kb)
out=$(dev_run "$APP leakthread 1048576")
assert_contains "a thread can allocate and never free it" \
    "thread done, process returning without freeing" "$out"

sleep 2
back6=$(psram_free_kb)
if [ -n "$back6" ] && [ $((base6 - back6)) -le 1024 ]; then
    espix_pass "and the process's arena reclaims what its thread took (${base6}K -> ${back6}K)"
else
    espix_fail "the arena reclaims a thread's allocation" \
        "PSRAM free ${base6}K -> ${back6}K"
fi



# --- threads can coordinate, and a thread can talk -------------------------
#
# Four threads each add to one counter under a mutex, so the total checks
# mutual exclusion rather than arithmetic. Before this, the mutex did not
# resolve at all -- an app calling pthread_mutex_lock failed to *load* -- and a
# thread's printf went to the UART instead of the session.

out=$(dev_run "$APP threaded 5000")
assert_contains "a thread's output reaches the session" \
    "thread running for 5000" "$out"
assert_contains "four threads share a counter under a mutex" \
    "counter 20000 after 4 thread(s) x 5000" "$out"
assert_contains "and the device is still up" "espix" "$(dev_run uname)"


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

# --- dup/dup2/fcntl(F_DUPFD): a second descriptor for one open file --------
#
# R-P1.8. A duplicate is another descriptor for the same open file: the offset
# is shared, closing one does not close the file, and the file goes when the
# last does. The command checks all of that itself and reopens at the end, so
# its exit status is the assertion and the summary line is the evidence.

DUPFILE=/tmp/espix-dup-$ESPIX_WORKER.txt
dev_run "rm $DUPFILE" >/dev/null 2>&1

assert_status "dup/dup2/F_DUPFD share one open file correctly" 0 \
    dev_status "$APP dup $DUPFILE"
assert_contains "and the run leaves no descriptor behind" "dup ok:" \
    "$(dev_run "$APP dup $DUPFILE")"

# The reaper has to close every IDF entry a dup left on one key, not only the
# first: a process that exits holding three duplicates must cost the pool
# nothing. Same capacity, measured the same way as the fdprobe pair above.
if [ -n "$n1" ] && [ "$n1" -ge 8 ] 2>/dev/null; then
    out=$(dev_run "$APP dup $DUPFILE hold" 2>&1)
    assert_contains "a process can end holding duplicated descriptors" \
        "dupped 3" "$out"
    n3=$(dev_run "$APP fdprobe $DUPFILE 0" | awk '/^capacity/ {print $2; exit}')
    assert_eq "and every duplicate is given back" "$n1" "$n3"
fi

dev_run "rm $DUPFILE" >/dev/null 2>&1
dev_run "rm $FDFILE" >/dev/null 2>&1

# --- parentage is recorded, even when there is nothing to be a parent ------
#
# The shell is a session task rather than a process, so an app it starts has no
# parent and shows '-' in the last column. The field behind it is what a child's
# exit is reported to (SIGCHLD), which needs something that is a process to
# spawn -- so the column is checked now and the notification is written and
# inert until then.

assert_contains "ps has a parent column" "PPID" "$(dev_run ps | head -1)"

pid=$(dev_run "$APP hold 60 1000 &" | sed -n 's/^\[\([0-9][0-9]*\)\].*/\1/p')
if [ -z "$pid" ]; then
    espix_fail "an app can be listed for its parent" "no [pid] line"
else
    sleep 1
    ppid_col=$(dev_run ps | awk -v p="$pid" '$1 == p { print $9; exit }')
    assert_eq "a shell-spawned app has no parent" "-" "$ppid_col"
    dev_run "kill -9 $pid" >/dev/null
fi


# --- a finished process leaves the table, and the log keeps the record ------
#
# R-P1.5. A finished process used to keep its slot, so history competed with
# concurrency inside a 12-slot table, and espix_proc_find() still answered for
# a process that was gone -- `kill` could signal it. Now the slot is released
# the moment the process finishes and the completed log is the only record:
# kill(2) answers ESRCH, and ps still shows what ran, from the log.

# The log is bounded: eight entries, however many processes have finished.
# Count only real finished rows -- column 3 is the state -- so a backgrounded
# app's output arriving in the same session cannot inflate the count.
hist=$(dev_run ps | sed -n '/^finished:/,$p' | awk '$3 ~ /^(exit|fault|kill)$/' | wc -l | tr -d ' ')
if [ "$hist" -ge 1 ] && [ "$hist" -le 8 ]; then
    espix_pass "ps history is bounded by the completed log ($hist entries)"
else
    espix_fail "ps history is bounded by the completed log" "read '$hist' rows"
fi

# A process that has finished is not signallable -- its slot is gone. The pid
# comes from the completed log rather than from racing a backgrounded app: the
# foreground run is reaped by the shell and stays listed in the log, so this is
# deterministic and costs three round trips instead of a poll loop.
dev_run "$APP exit 0" >/dev/null 2>&1
gone=$(dev_run ps | sed -n '/^finished:/,$p' | awk '$2 == "testapp" { p = $1 } END { print p }')
if [ -z "$gone" ]; then
    espix_fail "a finished process stops being signallable" "no testapp pid in the log"
else
    out=$(dev_run "kill -9 $gone" 2>&1)
    assert_contains "a finished process is not signallable" "no such process" "$out"
fi

assert_contains "and the device is still up" "espix" "$(dev_run uname)"

# --- a finished process is torn down by the reaper -------------------------
#
# R-P1.6. A process no longer deletes itself: it hands the slot to the reaper,
# which runs the teardown and deletes the task, so prvDeleteTCB -- the PSRAM
# stack free and the newlib reent reclaim -- happens on a normal stack instead
# of the idle task's small one. What it must not do is leave the task parked
# in the scheduler's list, so after a run nothing named app:testapp is a task.

dev_run "$APP exit 0" >/dev/null 2>&1
left=$(dev_run ps | sed -n '1,/^finished:/p' | grep -c 'app:testapp')
assert_eq "a finished process is deleted by the reaper" "0" "$left"
