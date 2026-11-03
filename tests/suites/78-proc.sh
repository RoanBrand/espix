# /proc: kernel state as files, generated on read. R-P6.3.
#
# RESOURCES: none -- read-only, touches no shared state.

listing=$(dev_run 'ls /proc')
assert_contains "/proc lists meminfo" "meminfo" "$listing"
assert_contains "/proc lists cpuinfo" "cpuinfo" "$listing"
assert_contains "/proc lists uptime"  "uptime"  "$listing"
assert_contains "/proc lists version" "version" "$listing"

mem=$(dev_run 'cat /proc/meminfo')
assert_contains "meminfo names the total"     "MemTotal:" "$mem"
assert_contains "meminfo separates internal"  "InternalFree:" "$mem"
assert_contains "meminfo separates PSRAM"     "PsramTotal:" "$mem"

total=$(printf '%s\n' "$mem" | awk '/^MemTotal:/ { print $2; exit }')
if [ -n "$total" ] && [ "$total" -gt 0 ] 2>/dev/null; then
    espix_pass "the total is a positive number ($total kB)"
else
    espix_fail "the total is a positive number" "got: '$total'"
fi

cpu=$(dev_run 'cat /proc/cpuinfo')
assert_contains "cpuinfo names the model"   "model name" "$cpu"
assert_contains "cpuinfo reports the cores" "cores" "$cpu"

up=$(dev_run 'cat /proc/uptime')
case "$up" in
    [0-9]*.[0-9]*) espix_pass "uptime is decimal seconds" ;;
    *)             espix_fail "uptime is decimal seconds" "got: '$up'" ;;
esac

assert_contains "version names espix" "espix" "$(dev_run 'cat /proc/version')"

first=$(dev_run 'head -n 1 /proc/meminfo')
assert_contains "a second read sees the same file" "$first" \
    "$(dev_run 'cat /proc/meminfo')"

assert_status "a missing proc file is refused" 1 dev_status 'cat /proc/nosuch'

# The tree has a mount point: a real directory in the root, the way /dev has
# one, so the root listing shows it. R-P6.3.
assert_contains "the root lists the proc mount point" "proc" "$(dev_run 'ls /')"

# /proc/stat is Linux-shaped: aggregate first, then one line per core, then the
# summary. htop reads exactly existingCPUs+1 leading cpu lines.
stat=$(dev_run 'cat /proc/stat')
assert_contains "stat has the aggregate line" "cpu " "$stat"
assert_contains "stat has cpu0" "cpu0 " "$stat"
assert_contains "stat has cpu1" "cpu1 " "$stat"
assert_contains "stat has btime" "btime " "$stat"
case "$stat" in
    cpu\ *cpu0\ *) espix_pass "the cpu lines are in order" ;;
    *)             espix_fail "the cpu lines are in order" "got: $(printf '%s' "$stat" | head -n 3)" ;;
esac

# Linux's ten fields, including system.  htop uses that field for the red part
# of the meter; until it was sampled from mstatus.MPP it was always zero.
cpu_fields=$(printf '%s\n' "$stat" | awk '/^cpu / { print NF; exit }')
if [ "$cpu_fields" = 11 ]; then
    espix_pass "the aggregate cpu line has ten fields"
else
    espix_fail "the aggregate cpu line has ten fields" "got $cpu_fields: $(printf '%s\n' "$stat" | head -n 1)"
fi
system_ticks=$(printf '%s\n' "$stat" | awk '/^cpu / { print $4; exit }')
case "$system_ticks" in
    ''|*[!0-9]*) espix_fail "stat reports system time" "got: '$system_ticks'" ;;
    *)           espix_pass "stat reports system time ($system_ticks ticks)" ;;
esac

# A process's stat, comm and directory ownership, from the shell's own pid.
pid=$(dev_run 'echo $$')
case "$pid" in
    ''|*[!0-9]*) espix_fail "the shell reports a pid" "got: '$pid'" ;;
    *)
        pstat=$(dev_run "cat /proc/$pid/stat")
        case "$pstat" in
            "$pid "*) espix_pass "/proc/<pid>/stat starts with its pid" ;;
            *)         espix_fail "/proc/<pid>/stat starts with its pid" "got: '$pstat'" ;;
        esac
        pcomm=$(dev_run "cat /proc/$pid/comm")
        case "$pcomm" in
            '') espix_fail "/proc/<pid>/comm names it" "got: ''" ;;
            *)  espix_pass "/proc/<pid>/comm names it ($pcomm)" ;;
        esac
        ;;
esac

# The CPU directories are what make htop draw two meters; a missing /sys leaves
# it with one, whatever sysconf() says.
cpus=$(dev_run 'ls /sys/devices/system/cpu')
assert_contains "sysfs has cpu0" "cpu0" "$cpus"
assert_contains "sysfs has cpu1" "cpu1" "$cpus"
freq=$(dev_run 'cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq')
case "$freq" in
    ''|*[!0-9]*) espix_fail "cpufreq is a number" "got: '$freq'" ;;
    *)           espix_pass "cpufreq is a number ($freq kHz)" ;;
esac

load=$(dev_run 'cat /proc/loadavg')
case "$load" in
    [0-9]*.[0-9]*\ [0-9]*.[0-9]*\ [0-9]*.[0-9]*) espix_pass "loadavg has three decimal fields" ;;
    *) espix_fail "loadavg has three decimal fields" "got: '$load'" ;;
esac
