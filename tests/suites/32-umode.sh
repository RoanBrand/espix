# U-mode apps on the S31 (docs/USER-MODE.md): an app in its own address space
# reaches the kernel only through syscalls, and a fault ends only the app.
#
# RESOURCES: none -- the binaries carry the worker number.

if [ "$ESPIX_TARGET" != "esp32s31" ]; then
    espix_skip "user mode is S31-only"
    return 0
fi

UM_DIR=$(mktemp -d)
UAPP="/home/$ESPIX_USER/uapp-$ESPIX_WORKER"
UHELLO="/home/$ESPIX_USER/hello-u-$ESPIX_WORKER"

if ! "$ESPIX_ROOT/tools/espix-cc" -Os -o "$UM_DIR/uapp" "$ESPIX_ROOT/tests/uapp/uapp.c" >/dev/null 2>&1 ||
   ! "$ESPIX_ROOT/tools/espix-cc" -Os -o "$UM_DIR/hello-u" "$ESPIX_ROOT/apps/hello/main/hello.c" >/dev/null 2>&1; then
    espix_fail "the U-mode apps build" "tools/espix-cc failed"
    rm -rf "$UM_DIR"
    return 0
fi
dev_push "$UM_DIR/uapp" "$UAPP" >/dev/null
dev_push "$UM_DIR/hello-u" "$UHELLO" >/dev/null
dev_run "chmod +x $UAPP $UHELLO" >/dev/null

# The same hello.c as the M-mode app, linked against picolibc and libespix-u.
hello_out=$(dev_run "$UHELLO one two")
assert_contains "hello runs in U-mode"          "hello from an espix app" "$hello_out"
assert_contains "and its arguments arrive"      "argv[2] = two"           "$hello_out"

assert_contains "a U-mode app prints through write()" "hello from U-mode, argc 2" \
    "$(dev_run "$UAPP hello")"
assert_status "its exit status reaches the client" 42 dev_status "$UAPP exit 42"

# Each fault is SIGSEGV for the app alone; the next assertion is the board still answering.
assert_status   "a write to address 0 ends the app with SIGSEGV" 139 dev_status "$UAPP null"
assert_contains "and the fault is reported"  "Segmentation fault" "$(dev_run "$UAPP null")"
assert_status   "kernel memory is out of reach"                  139 dev_status "$UAPP kernel"
assert_status   "the app's own data does not execute"            139 dev_status "$UAPP exec-data"
assert_contains "the board is still up after the faults" "up " "$(dev_run uptime)"

# Preemption and the FPU in U-mode, one app and then two sharing a core.
assert_contains "floating point survives preemption" "uapp: spun" "$(dev_run "$UAPP spin 1")"
# Their own connections: dev_run's session is one shell, and two commands in it interleave.
dev_once "$UAPP spin 1" > "$UM_DIR/a" 2>&1 & um_a=$!
dev_once "$UAPP spin 1" > "$UM_DIR/b" 2>&1 & um_b=$!
wait "$um_a" "$um_b"
assert_contains "two U-mode apps run side by side (first)"  "uapp: spun" "$(cat "$UM_DIR/a")"
assert_contains "two U-mode apps run side by side (second)" "uapp: spun" "$(cat "$UM_DIR/b")"

# Syscalls into the kernel's files, clocks and identities.
assert_contains "every file syscall, through one file's life" "uapp: files ok" \
    "$(dev_run "$UAPP files /home/$ESPIX_USER")"
assert_contains "a U-mode app is refused a root-only file" "Permission denied" \
    "$(dev_run "$UAPP read /etc/passwd")"
assert_contains "and reads it under sudo" "uapp: esp:" "$(dev_run "sudo $UAPP read /etc/passwd")"
assert_contains "chdir and getcwd" "uapp: cwd /tmp" "$(dev_run "$UAPP cwd /tmp")"
assert_contains "the app knows who it is" "euid=1000" "$(dev_run "$UAPP ids")"
sleep_out=$(dev_run "$UAPP sleep 200")
slept=$(printf '%s\n' "$sleep_out" | sed -n 's/.*slept \([0-9]*\) ms.*/\1/p')
if [ -n "$slept" ] && [ "$slept" -ge 190 ] && [ "$slept" -le 400 ]; then
    espix_pass "a 200 ms sleep takes about 200 ms on the monotonic clock ($slept ms)"
else
    espix_fail "a 200 ms sleep takes about 200 ms on the monotonic clock" "$sleep_out"
fi
printf 'alpha\nbeta\n' > "$UM_DIR/in.txt"
assert_contains "stdin reaches a U-mode app, line by line" "beta" "$(dev_ssh_raw "$UAPP cat" < "$UM_DIR/in.txt")"
assert_eq "and the end of input ends its read" "" "$(dev_ssh_raw "$UAPP cat" < /dev/null)"
dev_run "$UAPP redirect /home/$ESPIX_USER/uapp-redir-$ESPIX_WORKER" >/dev/null
assert_contains "dup2 onto stdout redirects printf" "uapp: into the file" \
    "$(dev_run "cat /home/$ESPIX_USER/uapp-redir-$ESPIX_WORKER")"
# Memory is mapped as it is touched: a heap by brk, and a stack that grows to its guard.
heap_out=$(dev_run "$UAPP bigheap")
reserve_kb=$(printf '%s\n' "$heap_out" | sed -n 's/.*reserving cost \(-*[0-9]*\) KB.*/\1/p')
touch_kb=$(printf '%s\n' "$heap_out" | sed -n 's/.*touching 64 pages \(-*[0-9]*\) KB.*/\1/p')
if [ -n "$reserve_kb" ] && [ "$reserve_kb" -lt 100 ] && [ -n "$touch_kb" ] &&
   [ "$touch_kb" -ge 200 ] && [ "$touch_kb" -lt 600 ]; then
    espix_pass "4 MB of heap costs only the pages touched ($reserve_kb KB, then $touch_kb KB)"
else
    espix_fail "4 MB of heap costs only the pages touched" "$heap_out"
fi
shrink_out=$(dev_run "$UAPP brkshrink")
grew_kb=$(printf '%s\n' "$shrink_out" | sed -n 's/.*grew \(-*[0-9]*\) KB.*/\1/p')
back_kb=$(printf '%s\n' "$shrink_out" | sed -n 's/.*returned \(-*[0-9]*\) KB.*/\1/p')
if [ -n "$grew_kb" ] && [ "$grew_kb" -ge 1900 ] && [ -n "$back_kb" ] && [ "$back_kb" -ge $((grew_kb - 100)) ]; then
    espix_pass "shrinking the break returns its frames ($grew_kb KB grown, $back_kb KB back)"
else
    espix_fail "shrinking the break returns its frames" "$shrink_out"
fi
assert_contains "a megabyte of stack grows on demand" "uapp: deep stack ok" "$(dev_run "$UAPP deepstack")"
assert_status "the page below the stack is a guard" 139 dev_status "$UAPP guard"

# Syscalls from three apps at once: each runs as task code, never inside the trap (docs/USER-MODE.md).
for w in 1 2 3; do
    dev_once "$UAPP sysloop 4" > "$UM_DIR/loop$w" 2>&1 &
    eval "um_loop$w=\$!"
done
wait "$um_loop1" "$um_loop2" "$um_loop3"
assert_contains "three apps in syscall loops at once (first)"  "syscalls" "$(cat "$UM_DIR/loop1")"
assert_contains "three apps in syscall loops at once (second)" "syscalls" "$(cat "$UM_DIR/loop2")"
assert_contains "three apps in syscall loops at once (third)"  "syscalls" "$(cat "$UM_DIR/loop3")"

# A kill ends an app whether it is computing in U or asleep in a syscall.
um_kill() {
    local pid
    pid=$(dev_run "$UAPP $1 &" | sed -n 's/^\[\([0-9][0-9]*\)\].*/\1/p')
    sleep 1
    dev_run "kill -$2 $pid" >/dev/null
    sleep 1
    dev_run "ps" | awk -v p="$pid" '$1 == p { print $NF }'
}
assert_eq "SIGTERM ends an app asleep in a syscall" "143" "$(um_kill 'sleep 10000' TERM)"
assert_eq "SIGTERM ends an app computing in U"      "143" "$(um_kill 'spin 30' TERM)"
assert_eq "SIGKILL ends it with 137"                "137" "$(um_kill 'spin 30' KILL)"

# Threads (M3): one address space, futex-based locks, and the process ends as a whole.
assert_contains "threads share a counter, take turns and time out" \
    "counter 8000 of 8000, ping-pong done, timed wait timed out, tid is the pid" \
    "$(dev_run "$UAPP threads 2000")"
assert_contains "malloc and printf from four threads at once" "uapp: churn ok" "$(dev_run "$UAPP tchurn")"
assert_contains "a thread's stack grows" "deep stack in a thread ok" "$(dev_run "$UAPP tstack")"
assert_status "exit() in a thread ends the process" 5 dev_status "$UAPP texit 5"
out=$(dev_run "$UAPP tdetach; echo rc=\$?")
assert_contains "the main thread's pthread_exit leaves the process to its threads" \
    "the detached thread finished" "$out"
assert_contains "and it ends with 0 when the last one does" "rc=0" "$out"
assert_eq "SIGTERM ends threads asleep in the kernel" "143" "$(um_kill tsleep TERM)"
assert_eq "SIGKILL ends threads computing on both cores" "137" "$(um_kill tspin KILL)"
# A U-mode AMO here can lose an update, so the kernel does them; LR/SC is native.
amo_out=$(dev_run "$UAPP amo")
assert_contains "an AMO add from both cores loses nothing (${amo_out#uapp: })" "amoadd lost 0 " "$amo_out"
assert_contains "and an LR/SC add" "lr/sc lost 0 " "$amo_out"

# Signals (M4): a handler runs wherever the thread is, and its frame puts every register back.
async_out=$(dev_run "$UAPP sigasync 50")
assert_contains "a handler runs in a loop that never calls in (${async_out#uapp: })" \
    "50 of 50 handlers ran in a compute loop, 0 wrong results, 0 on another thread" "$async_out"
assert_contains "a blocked signal waits; SA_RESETHAND; sigsuspend" ": ok" "$(dev_run "$UAPP sigmask")"
fault_out=$(dev_run "$UAPP sigfault; echo rc=\$?")
assert_contains "a fault reaches its SIGSEGV handler, with the address" "caught SIGSEGV at 0x10" "$fault_out"
assert_contains "and one while SIGSEGV is blocked ends the app" "rc=139" "$fault_out"
assert_contains "SA_RESTART restarts a read a handler interrupted" "read 'hi', handler ran 1" \
    "$( (sleep 3; echo hi) | dev_ssh_raw "$UAPP sigread restart")"
assert_contains "and without it the read fails with EINTR" "read -1 errno 4, handler ran 1" \
    "$( (sleep 3; echo hi) | dev_ssh_raw "$UAPP sigread intr")"

# SIGSTOP stops a loop that never calls in, and the loop's own clock sees the pause.
PZ="/home/$ESPIX_USER/uapp-pz-$ESPIX_WORKER"
pid=$(dev_run "$UAPP pauses 5 $PZ &" | sed -n 's/^\[\([0-9][0-9]*\)\].*/\1/p')
sleep 1
dev_run "kill -STOP $pid" >/dev/null
sleep 1
assert_eq "kill -STOP stops a computing app" "T" "$(dev_run ps | awk -v p="$pid" '$1 == p { print $3 }')"
sleep 1
dev_run "kill -CONT $pid" >/dev/null
for i in 1 2 3 4 5 6 7 8; do
    [ "$(dev_run ps | awk -v p="$pid" '$1 == p { print $3 }')" = exit ] && break
    sleep 1
done
pause=$(dev_run "cat $PZ" | sed -n 's/.*longest pause \([0-9]*\) ms.*/\1/p')
if [ -n "$pause" ] && [ "$pause" -ge 1500 ]; then
    espix_pass "and it does not run until SIGCONT (paused $pause ms)"
else
    espix_fail "and it does not run until SIGCONT" "pause: '$pause'"
fi
# Ctrl-Z stops it the same way, as a job; fg runs it to its end.
z_out=$(dev_run "<<<SUSP 1>>> $UAPP pauses 4 $PZ; echo st=\$?")
assert_contains "Ctrl-Z stops a computing app, as a job" "st=146" "$z_out"
sleep 2
dev_run "fg" >/dev/null
pause=$(dev_run "cat $PZ" | sed -n 's/.*longest pause \([0-9]*\) ms.*/\1/p')
if [ -n "$pause" ] && [ "$pause" -ge 1500 ]; then
    espix_pass "and fg resumes it (paused $pause ms)"
else
    espix_fail "and fg resumes it" "pause: '$pause'"
fi
pid=$(dev_run "$UAPP pauses 5 $PZ &" | sed -n 's/^\[\([0-9][0-9]*\)\].*/\1/p')
sleep 1
dev_run "kill -STOP $pid" >/dev/null
sleep 1
dev_run "kill -9 $pid" >/dev/null
sleep 1
assert_eq "SIGKILL ends a stopped app" "137" "$(dev_run ps | awk -v p="$pid" '$1 == p { print $NF }')"

# Sockets and poll (M6): lwIP's, behind the process's descriptors; a blocking call waits where a signal can end it.
assert_contains "TCP over loopback, across pages and past the stack's buffers" \
    "tcp 20000 bytes intact, then end of stream 0, peer 127.0.0.1" "$(dev_run "$UAPP tcp")"
nb_out=$(dev_run "$UAPP nb")
assert_contains "a non-blocking accept and recv would block (EAGAIN)" "accept -1/No more processes" "$nb_out"
assert_contains "a non-blocking connect is in progress, and poll sees it and the accept" \
    "poll 2 (out 1, in 1), so_error 0" "$nb_out"
idle_ms=$(printf '%s' "$nb_out" | sed -n 's/.*idle poll 0 after \([0-9]*\) ms.*/\1/p')
if [ -n "$idle_ms" ] && [ "$idle_ms" -ge 300 ] && [ "$idle_ms" -lt 2000 ]; then
    espix_pass "poll times out with nothing to read ($idle_ms ms of 300)"
else
    espix_fail "poll times out with nothing to read" "got: $nb_out"
fi
assert_contains "a datagram over a page boundary, and its sender" \
    "udp sent 3000, got 3000 intact from 127.0.0.1" "$(dev_run "$UAPP udp")"
names_out=$(dev_run "$UAPP names")
assert_contains "IPv6 text both ways" "v6 2001:db8::8:800:200c:417a -> 2001:db8::8:800:200c:417a" "$names_out"
assert_contains "and a v4-mapped address" "v6 ::ffff:10.0.0.7 -> ::ffff:10.0.0.7" "$names_out"
assert_contains "getaddrinfo answers a number" "getaddrinfo port 1883 type 1" "$names_out"
assert_contains "and no socket type asked for is each of them" "any type: 2 results" "$names_out"
assert_contains "the user and group records, and the RNG" \
    "user $ESPIX_USER home /home/$ESPIX_USER, group $ESPIX_USER, random yes" "$names_out"
assert_contains "a handler ends a blocking recv with EINTR" "recv rc=-1 errno=4" "$(dev_run "$UAPP netintr recv")"
rr_out=$(dev_run "$UAPP netintr recvr")
rr_ms=$(printf '%s' "$rr_out" | sed -n 's/.*after \([0-9]*\) ms.*/\1/p')
if [ -n "$rr_ms" ] && [ "$rr_ms" -ge 1400 ] && printf '%s' "$rr_out" | grep -q "rc=4 errno=0"; then
    espix_pass "and SA_RESTART restarts it until the data comes ($rr_ms ms)"
else
    espix_fail "and SA_RESTART restarts it until the data comes" "got: $rr_out"
fi
assert_contains "poll is not restarted" "poll rc=-1 errno=4" "$(dev_run "$UAPP netintr poll")"
pid=$(dev_run "$UAPP acceptwait &" | sed -n 's/^\[\([0-9][0-9]*\)\].*/\1/p')
sleep 1
dev_run "kill -STOP $pid" >/dev/null
sleep 1
assert_eq "kill -STOP stops an app blocked in accept" "T" "$(dev_run ps | awk -v p="$pid" '$1 == p { print $3 }')"
dev_run "kill -CONT $pid" >/dev/null
sleep 1
st=$(dev_run ps | awk -v p="$pid" '$1 == p { print $3 }')
if [ -n "$st" ] && [ "$st" != T ] && [ "$st" != exit ]; then
    espix_pass "and SIGCONT puts it back in its wait ($st)"
else
    espix_fail "and SIGCONT puts it back in its wait" "state: '$st'"
fi
dev_run "kill -9 $pid" >/dev/null
assert_eq "SIGTERM ends an app blocked in accept" "143" "$(um_kill acceptwait TERM)"
assert_eq "and SIGKILL" "137" "$(um_kill acceptwait KILL)"
posix_out=$(dev_run "$UAPP posix")
assert_contains "getifaddrs lists the loopback" "if lo up loopback 127.0.0.1" "$posix_out"
assert_contains "and the address this suite reaches the board on" " $ESPIX_HOST" "$posix_out"
assert_contains "dlopen says why it cannot" "Dynamic loading not supported" "$posix_out"
assert_contains "syslog lands in the kernel log" "uapp[" "$(dev_run "dmesg | grep 'syslog from U-mode 42'")"
z_out=$(dev_run "<<<SUSP 1>>> $UAPP acceptwait; echo st=\$?")
assert_contains "Ctrl-Z stops an app blocked in accept, as a job" "st=146" "$z_out"
dev_run "kill %1" >/dev/null

bench_out=$(dev_run "$UAPP bench")
assert_contains "a null syscall is timed ($bench_out)" "uapp: a syscall takes" "$bench_out"
rm -rf "$UM_DIR"

dev_run "rm $UAPP $UHELLO /home/$ESPIX_USER/uapp-redir-$ESPIX_WORKER $PZ" >/dev/null
