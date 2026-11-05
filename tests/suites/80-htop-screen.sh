# htop on a pty: one frame, then exit.  The version test only proves the file
# exists; this proves ncurses, terminfo, /dev/tty input and /proc all work
# together far enough to paint the screen.
#
# RESOURCES: none -- htop reads /proc, then exits.

ver=$(dev_run 'htop --version')
case "$ver" in
    "htop "*) ;;
    *) espix_skip "htop is not staged for this target (apps/htop/targets)"; return 0 ;;
esac

# The session already runs ssh -tt, so this command has a pty.  TERM is
# explicit because the test harness itself uses a dumb terminal.
screen=$(dev_run 'TERM=xterm-256color htop -n 1 -d 10')
case "$screen" in
    *"Tasks:"*) espix_pass "htop paints a frame on a pty" ;;
    *)          espix_fail "htop paints a frame on a pty" "no Tasks: in output" ;;
esac
case "$screen" in
    *"PID"*) espix_pass "htop has a process table" ;;
    *)       espix_fail "htop has a process table" "no PID header" ;;
esac
