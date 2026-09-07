# stdout and stderr are different streams.
#
# espix had one output path until recently: espix_printf() carried results and
# error messages alike, and `>` captured both. That is not merely untidy -- it
# cost a wrong conclusion during the test-suite work, when a failed ELF load's
# message vanished into a redirect and the loader was blamed for not naming a
# symbol it had named perfectly. That specific failure is the first check here.
#
# PARALLEL_SAFE=no -- writes files under /tmp with fixed names.

T=/tmp/espix-streams
dev_run "rm -r $T" >/dev/null 2>&1
dev_run "mkdir $T" >/dev/null 2>&1

# ---------------------------------------------------------------------------
# `>` captures output and leaves diagnostics alone.
# ---------------------------------------------------------------------------

out=$(dev_run "/bin/nosuchprogram > $T/log")
assert_contains "a diagnostic still reaches the terminal" "not found" "$out"
assert_eq "and the redirect did not swallow it" "" "$(dev_run "cat $T/log")"

# The control: ordinary output still goes to the file and not the terminal.
out=$(dev_run "echo hello-stdout > $T/out")
assert_eq "output goes to the file, not the terminal" "" "$out"
assert_eq "and the file has it" "hello-stdout" "$(dev_run "cat $T/out")"

# ---------------------------------------------------------------------------
# `2>` captures diagnostics and leaves output alone.
# ---------------------------------------------------------------------------

out=$(dev_run "/bin/nosuchprogram 2> $T/err")
assert_eq "2> takes the diagnostic off the terminal" "" "$out"
assert_contains "and puts it in the file" "not found" "$(dev_run "cat $T/err")"

out=$(dev_run "ls / 2> $T/err2")
assert_contains "2> leaves ordinary output alone" "etc" "$out"

# ---------------------------------------------------------------------------
# `2>&1` sends both to the same place, without closing one stream twice.
# ---------------------------------------------------------------------------

out=$(dev_run "/bin/nosuchprogram > $T/both 2>&1")
assert_eq "2>&1 takes the diagnostic off the terminal" "" "$out"
assert_contains "and merges it into the output file" "not found" \
    "$(dev_run "cat $T/both")"

# Still alive afterwards: a double fclose would have taken the session with it.
assert_eq "the session survived 2>&1" "$ESPIX_USER" "$(dev_run 'whoami')"

# ---------------------------------------------------------------------------
# Both streams reach the client when nothing is redirected.
# ---------------------------------------------------------------------------

both=$(dev_run "/bin/nosuchprogram" 2>&1)
assert_contains "unredirected, a diagnostic is still visible" "not found" "$both"

# ---------------------------------------------------------------------------
# Over SSH the two streams are genuinely separate on the wire.
#
# espix sends diagnostics as CHANNEL_EXTENDED_DATA (RFC 4254 5.2, type 1), so
# the *client* puts them on its own stderr. That is what this checks, and it is
# the only check that can tell the difference: if espix simply sent them as
# ordinary CHANNEL_DATA, everything else in this file would still pass and
# `ssh host cmd 2>/dev/null` would still show the error.
# ---------------------------------------------------------------------------

_ssh() {
    SSH_ASKPASS="$DEV_ASKPASS" SSH_ASKPASS_REQUIRE=force DISPLAY=:0 \
        ssh $DEV_SSH_OPTS "$ESPIX_USER@$ESPIX_HOST" "$@"
}

assert_eq "the client's 2>/dev/null swallows a diagnostic" "" \
    "$(_ssh /bin/nosuchprogram 2>/dev/null)"

case "$(_ssh /bin/nosuchprogram 2>&1)" in
    *"not found"*) espix_pass "and its 2>&1 shows it" ;;
    *)             espix_fail "and its 2>&1 shows it" "no message came through" ;;
esac

case "$(_ssh uname -a 2>/dev/null)" in
    *espix*) espix_pass "ordinary output still arrives on stdout" ;;
    *)       espix_fail "ordinary output still arrives on stdout" ;;
esac

# ---------------------------------------------------------------------------
# A loaded app's own streams.
#
# stdout and stderr are two separate funopen() streams over the session, so an
# app's fprintf(stderr, ...) has to land on the error side like a builtin's
# diagnostics do.
# ---------------------------------------------------------------------------

if dev_testapp_sync "$ESPIX_ROOT/fsroot/home/$ESPIX_USER/testapp"; then
    APP="/home/$ESPIX_USER/testapp"

    both=$(dev_run "$APP both")
    assert_contains "an app's stdout arrives"  "this-is-stdout" "$both"
    assert_contains "and so does its stderr"   "this-is-stderr" "$both"

    # A *foreground* app's streams are redirected, and the qualifier is the
    # whole of it: run_program() blocks in espix_proc_wait() for a foreground
    # process, so the redirect FILE outlives it. A backgrounded one does not,
    # and redirects_release() closes that FILE when the command returns.
    dev_run "$APP both > $T/app-out" >/dev/null
    assert_contains "a foreground app's stdout follows >" "this-is-stdout" \
        "$(dev_run "cat $T/app-out")"
    assert_not_contains "and its stderr does not go with it" "this-is-stderr" \
        "$(dev_run "cat $T/app-out")"

    dev_run "$APP both 2> $T/app-err" >/dev/null
    assert_contains "a foreground app's stderr follows 2>" "this-is-stderr" \
        "$(dev_run "cat $T/app-err")"
    assert_not_contains "and its stdout does not"          "this-is-stdout" \
        "$(dev_run "cat $T/app-err")"

    # Both to one file, and the app survives it -- the double-close trap again,
    # this time with the app's own two streams pointed at one shell-owned FILE.
    dev_run "$APP both > $T/app-both 2>&1" >/dev/null
    both=$(dev_run "cat $T/app-both")
    assert_contains "2>&1 merges an app's streams"  "this-is-stdout" "$both"
    assert_contains "and keeps both of them"        "this-is-stderr" "$both"
    assert_eq "the session survived an app's 2>&1" "$ESPIX_USER" "$(dev_run 'whoami')"

    # `2>&1` with no `>` at all, checked from the client, which is the only
    # place the difference shows. espix has no redirect FILE to share here, so
    # it has to send the app's diagnostics as ordinary CHANNEL_DATA -- if it
    # sent them as extended data instead, the client would put them on its own
    # stderr and `2>/dev/null` below would throw them away.
    merged=$(_ssh "$APP both 2>&1" 2>/dev/null)
    assert_contains "an app's 2>&1 merges onto stdout for the client" \
        "this-is-stdout" "$merged"
    assert_contains "and its stderr comes with it"  "this-is-stderr" "$merged"

    # The control: without 2>&1 the client's stderr is where the app's
    # diagnostics land, so the same redirect discards them.
    assert_not_contains "control: without 2>&1 they are separable again" \
        "this-is-stderr" "$(_ssh "$APP both" 2>/dev/null)"

    # A backgrounded app is the case that cannot be redirected. Deliberate, and
    # the reason is lifetime rather than oversight -- see KNOWN-ISSUES.
    dev_run "$APP both > $T/app-bg &" >/dev/null
    sleep 2
    assert_eq "a backgrounded app's output is not redirected (see KNOWN-ISSUES)" "" \
        "$(dev_run "cat $T/app-bg")"

    # ---------------------------------------------------------------------
    # Standard input.
    #
    # The check that the readfn is real: espix hands a process a stream over
    # the queue chan_poll_interrupt() fills from CHANNEL_DATA. Until that
    # existed the readfn was NULL and stdin returned EOF at once -- which is
    # indistinguishable from an empty file, so the byte count matters as much
    # as the text.
    # ---------------------------------------------------------------------

    stdin_src=$(espix_mktemp_dir)/in.txt
    printf 'alpha\nbeta\ngamma\n' > "$stdin_src"

    got=$(_ssh "$APP cat" < "$stdin_src")
    assert_contains "an app reads stdin over ssh"    "alpha" "$got"
    assert_contains "and reads all of it"            "gamma" "$got"
    assert_contains "and counts the bytes it read"   "cat: 17 bytes" "$got"

    # EOF has to arrive as EOF rather than as a hang: no input at all must end
    # the read, not block until the client gives up.
    assert_contains "empty stdin reads as end of input" "cat: 0 bytes" \
        "$(_ssh "$APP cat" < /dev/null)"

    rm -rf "$(dirname "$stdin_src")"
else
    espix_skip "test app not built -- run 'make test-app'"
fi

dev_run "rm -r $T" >/dev/null 2>&1
