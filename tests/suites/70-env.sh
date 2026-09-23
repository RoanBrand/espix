# Session variables, expansion, and what a process inherits.
#
# The assertion that earns its place here is the isolation one. espix owns
# command history per *user*, so two sessions as the same account share one
# list -- and because only the lookup was locked, that was a double free which
# surfaced as four unrelated panics, none of them in history.c. The environment
# is the next thing shaped like that, so this suite proves per-session ownership
# rather than assuming it.
#
# RESOURCES: none -- everything here is per session, which is the property
# under test. Two of these running side by side is a feature.

if [ -z "${DEV_PROMPT:-}" ]; then
    espix_skip "no session"
    return 0
fi

# ---------------------------------------------------------------------------
# Expansion
# ---------------------------------------------------------------------------

dev_run 'export FOO=bar' >/dev/null
assert_eq "\$VAR expands"        "bar" "$(dev_run 'echo $FOO')"
assert_eq "\${VAR} expands"      "bar" "$(dev_run 'echo ${FOO}')"
assert_eq "it expands mid-word"  "pre-bar-post" "$(dev_run 'echo pre-$FOO-post')"
assert_eq "an unset name expands to nothing" "." "$(dev_run 'echo $NOPE.')"

# A value with a space in it. Note the quoting: esp_console_split_argv() honours
# a quote that *starts* an argument, so `export "SP=a b"` works and
# `export SP="a b"` does not -- the latter sets SP to the two characters `"a`.
# Worth asserting because it is the form that looks right and is not.
dev_run 'export "SPACED=a b"' >/dev/null
assert_eq "a quoted assignment keeps the space" "a b" \
    "$(dev_run 'echo $SPACED')"
dev_run 'export SPACED2="a b"' >/dev/null 2>&1
assert_eq "an unquoted one does not, and splits at the space" '"a' \
    "$(dev_run 'echo $SPACED2')"

# ---------------------------------------------------------------------------
# $? -- tracked by the session since before there was any way to read it
# ---------------------------------------------------------------------------

dev_run 'definitelynotacommand' >/dev/null 2>&1
assert_eq "\$? reports a failed command" "127" "$(dev_run 'echo $?')"
dev_run 'uptime' >/dev/null
assert_eq "\$? reports success"          "0"   "$(dev_run 'echo $?')"

# ---------------------------------------------------------------------------
# The defaults a login starts with
# ---------------------------------------------------------------------------

assert_eq "HOME is the account's home" "/home/$ESPIX_USER" "$(dev_run 'echo $HOME')"
assert_eq "USER is the login name"     "$ESPIX_USER"       "$(dev_run 'echo $USER')"
assert_eq "PATH defaults to /bin"      "/bin"              "$(dev_run 'echo $PATH')"
assert_contains "TERM is set for an SSH session" "xterm"   "$(dev_run 'echo $TERM')"

# TZ is not a session variable: it belongs to the machine, lives in the system
# environment where tzset() reads it, and `env` shows it because a process
# inherits it.
assert_contains "the system environment reaches env" "TZ=" "$(dev_run 'env')"

# ---------------------------------------------------------------------------
# export vs set: the distinction only a spawned process can see
# ---------------------------------------------------------------------------

if ! dev_testapp_present; then
    espix_skip "test app not built -- the inheritance checks need it"
else
    APP="/home/$ESPIX_USER/testapp"

    dev_run 'PRIVATE=hidden' >/dev/null
    assert_eq "a bare assignment is visible to the shell" "hidden" \
        "$(dev_run 'echo $PRIVATE')"
    assert_contains "and not to a program" "PRIVATE=(unset)" \
        "$(dev_run "$APP env get PRIVATE")"

    dev_run 'export PRIVATE' >/dev/null
    assert_contains "until it is exported" "PRIVATE=hidden" \
        "$(dev_run "$APP env get PRIVATE")"

    # The app reaches espix's getenv, not newlib's: this name exists only in
    # the session's table, so newlib's global would answer "unset".
    dev_run 'export ONLYHERE=proof' >/dev/null
    assert_contains "a program's getenv is espix's, not newlib's" \
        "ONLYHERE=proof" "$(dev_run "$APP env get ONLYHERE")"

    assert_contains "a program inherits HOME" "HOME=/home/$ESPIX_USER" \
        "$(dev_run "$APP env get HOME")"

    # The one that needs argc to prove: an expansion holding a space arrives as
    # ONE argument, not two. espix does not re-split after expanding -- that is
    # where shells get their sharp edges, and there is no quoting here to defend
    # it with. argc 4 rather than 5 is the whole assertion.
    assert_contains "an expansion is one argument, not re-split" "argc 4" \
        "$(dev_run "$APP argv \$SPACED tail")"
    assert_contains "and the space survived into it" "argv[2] a b" \
        "$(dev_run "$APP argv \$SPACED tail")"

    # Its copy is its own. This is the history bug's lesson applied one level
    # down: a process changing its environment must not reach back.
    dev_run "$APP env set ONLYHERE=changed" >/dev/null
    assert_eq "a program's setenv does not reach the shell" "proof" \
        "$(dev_run 'echo $ONLYHERE')"

    # One-shot assignments.
    assert_contains "FOO=bar cmd reaches the program" "ONESHOT=yes" \
        "$(dev_run "ONESHOT=yes $APP env get ONESHOT")"
    assert_eq "and does not survive the command" "" "$(dev_run 'echo $ONESHOT')"

    dev_run 'export OVER=session' >/dev/null
    assert_contains "a one-shot beats the session's value for that command" \
        "OVER=oneshot" "$(dev_run "OVER=oneshot $APP env get OVER")"
    assert_eq "and the session's value survives it" "session" \
        "$(dev_run 'echo $OVER')"

    # unset removes the session's override. It cannot remove a system variable
    # -- the machine's environment is not a login's to edit -- so the program
    # falls back to the machine's answer, which is the intended behaviour and
    # not a gap.
    assert_contains "a program sees the system TZ" "TZ=" \
        "$(dev_run "$APP env get TZ")"
    dev_run 'export TZ=Mars/Olympus' >/dev/null
    assert_contains "a session can override it for its programs" \
        "TZ=Mars/Olympus" "$(dev_run "$APP env get TZ")"
    dev_run 'unset TZ' >/dev/null
    assert_not_contains "and unset drops back to the system value, not to nothing" \
        "Mars/Olympus" "$(dev_run "$APP env get TZ")"
fi

# ---------------------------------------------------------------------------
# PATH is walked, not assumed
# ---------------------------------------------------------------------------

if dev_status 'ls /bin/hello'; then
    assert_contains "a bare name is found on the default PATH" "hello from" \
        "$(dev_run 'hello')"
else
    espix_skip "a bare name is found on the default PATH: /bin/hello not in this rootfs (make apps && make fs)"
fi
assert_contains "an empty PATH finds nothing" "not found" \
    "$(dev_run 'PATH= hello' 2>&1)"
assert_contains "a PATH without it finds nothing" "not found" \
    "$(dev_run 'PATH=/nowhere hello' 2>&1)"
if dev_status 'ls /bin/hello'; then
    assert_contains "and a later entry is still searched" "hello from" \
        "$(dev_run 'PATH=/nowhere:/bin hello')"
else
    espix_skip "and a later entry is still searched: /bin/hello not in this rootfs"
fi

# ---------------------------------------------------------------------------
# The limits are refusals, not truncations
# ---------------------------------------------------------------------------

assert_contains "a name that is not an identifier is refused" "not a valid" \
    "$(dev_run 'export 9bad=x' 2>&1)"

LONG=$(printf 'x%.0s' $(seq 1 200))
assert_contains "an over-long value is refused, not truncated" "longer than" \
    "$(dev_run "export BIG=$LONG" 2>&1)"

i=0
while [ "$i" -lt 26 ]; do
    dev_run "export FILL$i=$i" >/dev/null 2>&1
    i=$((i + 1))
done
assert_contains "a full table is refused with the limit named" "is the limit" \
    "$(dev_run 'export LAST=x' 2>&1)"
