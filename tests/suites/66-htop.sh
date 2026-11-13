# htop on a pty, the way a person uses it: every key lands once pressed, a screen
# stays open until it is closed, and launching and quitting it again and again
# leaves the board as it was.
#
# RESOURCES: exclusive -- the screens are timed in seconds.

if [ "${ESPIX_APPS:-0}" != 1 ]; then
    espix_skip "app suites need --apps (or 'make test-apps')"
    return 0
fi

ver=$(dev_run 'htop --version')
case "$ver" in
    "htop "*) espix_pass "htop is installed and reports a version ($ver)" ;;
    *)        espix_skip "htop is not built for this target (apps/htop/targets)"; return 0 ;;
esac

# The keys as an xterm in keypad mode sends them, each in one write.
F2=1b4f51; F10=1b5b32317e; DOWN=1b4f42; Q=71
HTOP='TERM=xterm-256color htop -d 5'

out=$(dev_keys "$HTOP" "expect:Tasks:" "expect:PID" "send:$Q")
assert_contains "htop paints its process table" "PID" "$out"
assert_not_contains "and q quits it" "KEYS-TIMEOUT" "$out"

# In colour: ncurses read xterm-256color's colours and pairs, so the theme draws with ESC [ 3x m, not only bold.
raw=$(dev_keys "$HTOP" --raw "expect:PID" "send:$Q")
colours=$(printf '%s' "$raw" | grep -o $'\e\\[3[0-7]m' | wc -l | tr -d ' ')
if [ "${colours:-0}" -gt 0 ]; then
    espix_pass "and draws it in colour ($colours colour changes)"
else
    espix_fail "and draws it in colour" "no ESC [ 3x m in what htop wrote"
fi

# Setup must stay open past several refreshes, and take a key there: Down moves it to the header layout.
out=$(dev_keys "$HTOP" "expect:PID" "send:$F2" "expect:Display options" "sleep:3" "mark" \
               "send:$DOWN" "expect:Header Layout" "send:$F10" "sleep:1" "send:$Q")
assert_contains "F2's Setup is still open three seconds on, and Down moves within it" "Header Layout" "$out"
assert_not_contains "and F10 then q leave htop" "KEYS-TIMEOUT" "$out"

# Three arrows and q in one write: each is a key, none is lost, and q still quits.
out=$(dev_keys "$HTOP" "expect:PID" "send:$DOWN$DOWN$DOWN$Q")
assert_not_contains "a burst of keys lands whole: three arrows, then q quits" "KEYS-TIMEOUT" "$out"

# Twenty launches and quits in one session, then the board's health.
before=$(dev_uptime_minutes)
steps=()
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
    steps+=("type:$HTOP" "expect:PID" "send:$Q" "prompt")
done
out=$(dev_keys "true" prompt "${steps[@]}")
assert_not_contains "htop launched and quit twenty times" "KEYS-TIMEOUT" "$out"
after=$(dev_uptime_minutes)
if [ -n "$before" ] && [ -n "$after" ] && [ "$after" -ge "$before" ]; then
    espix_pass "and the board did not reset ($before -> $after min up)"
else
    espix_fail "and the board did not reset" "uptime went ${before}min -> ${after}min"
fi
