# The serial console.
#
# Worth testing separately from SSH for one reason above all: the console
# session is root, so it is where the privileged paths are exercised without
# unlocking the root account for the network. It is also the only interface
# that survives the network being broken, which is when you need it most.
#
# RESOURCES: console -- there is one serial port. Nothing else in the tree
# wants it, so this still runs in the pool beside the SSH suites.

if [ "$ESPIX_HAVE_SERIAL" != yes ]; then
    espix_skip "no serial port given (--port) or no pyserial"
    return 0
fi

# One probe decides whether the console is there at all, and the rest of the
# suite is skipped if it is not.
#
# Each dev_console_run spawns its own console.py, which syncs before it can do
# anything, so a console that cannot sync costs that wait *per assertion*. This
# suite used to spend around seven minutes failing five times over, which is
# both slow and a poor report: five failures that are one fact.
# Quiet the kernel log on the console first, and put it back at the end.
#
# Not tidiness: espix writes klog lines straight to the same UART the line
# editor is drawing on, so under load a log line lands in the middle of the
# echo and splits it -- `whoami` came back as `whoam` + a log line + `i`, and
# every assertion after it compared against rubble. Four suites running over
# SSH generate a connection message every few seconds, so this is the normal
# case now rather than an unlucky one.
#
# That interleaving is espix behaviour and arguably correct -- a Unix console
# does it too, which is why `dmesg -n` exists -- so it is recorded in
# KNOWN-ISSUES rather than worked around in the firmware. What this suite tests
# is the shell over serial, and it cannot test that through a channel something
# else is writing to.
# Quieted over SSH, not over the console, and it has to be that way round.
#
# The console cannot quiet its own log: setting the level takes a command, and
# typing a command is the thing the log breaks. The first attempt did it from
# the console and produced this --
#
#     root:/# dmesg -n w
#     root:/# dmesg -n wa
#     espix: sshchan: esp logged out
#
# -- the editor redrawing after every keystroke while klog lines landed between
# the redraws, so console.py saw a prompt mid-command and framed the answer
# wrong. Over SSH there is no such problem: `dmesg -n` is global, and sudo
# reaches root from the `esp` account.
CONSOLE_LEVEL_SAVED=$(dev_run 'sudo dmesg -n' |
                      sed -n 's/.*console level: \([0-9]*\).*/\1/p')
dev_run 'sudo dmesg -n warn' >/dev/null 2>&1

console_restore() {
    case "${CONSOLE_LEVEL_SAVED:-}" in
        ''|*[!0-9]*) dev_run 'sudo dmesg -n info' >/dev/null 2>&1 ;;
        *)           dev_run "sudo dmesg -n $CONSOLE_LEVEL_SAVED" >/dev/null 2>&1 ;;
    esac
}

# One probe decides whether the console is there, and it comes first among the
# console calls -- everything after may assume a console that answers.
if ! who=$(dev_console_run 'whoami'); then
    espix_skip "console did not answer -- see the message above for whether"
    espix_skip "something else is holding $ESPIX_PORT"
    console_restore
    return 0
fi

assert_eq "the console session is root" "root" "$who"

# The console has been seen to answer the probe and then go silent for the rest
# of the suite -- intermittently, in roughly two full runs out of three, while
# passing every time this suite is run on its own.
#
# What has been ruled out: it is not the firmware (two full runs on the *same*
# image, one green and one not), not a second reader (console.py checks, and
# lsof showed nothing), not an orphan of ours, and not the device dying -- SSH
# answers throughout and the health check reports no reboot. So the useful
# thing is not another guess, it is a report that says which of those it was
# next time. The device is reachable over SSH from here even when the cable is
# not talking, so ask it.
console_gone() {
    espix_fail "the console stopped answering after the first command" \
               "uptime:  $(dev_run 'uptime')" \
               "tasks:   $(dev_run 'ps' | sed -n '1,/^finished:/p' | tr -s ' \n' ' ')" \
               "dmesg:   $(dev_run 'dmesg' | tail -3 | tr '\n' '|')" \
               "holder:  $(lsof "$ESPIX_PORT" 2>/dev/null | tail -1)"
}

pwd_out=$(dev_console_run 'pwd')
case "$pwd_out" in
    */) assert_eq "the console starts at /" "/" "$pwd_out" ;;
    *"no console prompt"*|*"no prompt after"*)
        console_gone
        espix_skip "the rest of the console suite -- one fact, not four failures"
        console_restore
        return 0 ;;
    *)  assert_eq "the console starts at /" "/" "$pwd_out" ;;
esac
assert_contains "the console reports uid 0" "uid=0(root)" "$(dev_console_run 'id')"

# Commands work over the console exactly as over SSH -- the shell is one
# implementation with two transports, and that is the claim being checked.
assert_contains "uname works on the console" "espix" "$(dev_console_run 'uname -a')"
assert_contains "the console can read a root-only file" "root:" \
    "$(dev_console_run 'cat /etc/passwd')"

# The sigil is the other half of the prompt change: root gets '#'.

console_restore
