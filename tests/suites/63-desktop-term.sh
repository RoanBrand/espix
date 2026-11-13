# The desktop's own terminal, typed at over VNC as a person at the board would: it sends the keys
# an app asks for -- ESC O under keypad mode (DECCKM, which ncurses' keypad() sets), ESC [ outside
# it, and xterm's F-keys -- so a curses app works on the device's screen as over SSH.
#
# RESOURCES: exclusive -- it opens a window on the one screen and types into it.
#
# With the app suites, as 64-gfx, because it starts VNC: the server keeps an eventfd for the boot
# once started (docs/ROADMAP.md), one of the S31's 16, and a -j4 run then runs out.

if [ "${ESPIX_APPS:-0}" != 1 ]; then
    espix_skip "the desktop suites need --apps (or 'make test-apps')"
    return 0
fi
if ! dev_testapp_present; then
    espix_skip "test app not built -- run 'make test-app'"
    return 0
fi

# Something to type at: VNC, started for the suite if it is not running, and stopped again after.
term_vnc_was=$(dev_run vnc)
case $term_vnc_was in *stopped*) dev_run "vnc start" >/dev/null ;; esac

APP="/home/$ESPIX_USER/testapp"
out=$("${ESPIX_PYTHON:-python3}" "$ESPIX_ROOT/tests/lib/vnc_keys.py" "$ESPIX_HOST" \
    "type:$APP keyburst app /tmp/kb-up" "key:0xFF52" \
    "type:$APP keyburst app /tmp/kb-f1" "key:0xFFBE" \
    "type:$APP keyburst /tmp/kb-plain" "key:0xFF52" 2>&1)
assert_contains "the desktop terminal sends Up as ESC O A to an app in keypad mode" \
    "first 1 1b, then poll 1, rest 4f 41" "$(dev_run 'cat /tmp/kb-up')$out"
assert_contains "and F1 as ESC O P" "first 1 1b, then poll 1, rest 4f 50" "$(dev_run 'cat /tmp/kb-f1')"
assert_contains "and Up as ESC [ A to one that did not ask" \
    "first 1 1b, then poll 1, rest 5b 41" "$(dev_run 'cat /tmp/kb-plain')"
dev_run "rm /tmp/kb-up /tmp/kb-f1 /tmp/kb-plain" >/dev/null 2>&1

case $term_vnc_was in *stopped*) dev_run "vnc stop" >/dev/null ;; esac
