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
