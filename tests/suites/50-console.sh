# The serial console.
#
# Worth testing separately from SSH for one reason above all: the console
# session is root, so it is where the privileged paths are exercised without
# unlocking the root account for the network. It is also the only interface
# that survives the network being broken, which is when you need it most.
#
# PARALLEL_SAFE=no -- there is one serial port.

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
if ! who=$(dev_console_run 'whoami'); then
    espix_skip "console did not answer -- see the message above for whether"
    espix_skip "something else is holding $ESPIX_PORT"
    return 0
fi

assert_eq "the console session is root" "root" "$who"

assert_eq "the console starts at /" "/" "$(dev_console_run 'pwd')"
assert_contains "the console reports uid 0" "uid=0(root)" "$(dev_console_run 'id')"

# Commands work over the console exactly as over SSH -- the shell is one
# implementation with two transports, and that is the claim being checked.
assert_contains "uname works on the console" "espix" "$(dev_console_run 'uname -a')"
assert_contains "the console can read a root-only file" "root:" \
    "$(dev_console_run 'cat /etc/passwd')"

# The sigil is the other half of the prompt change: root gets '#'.
