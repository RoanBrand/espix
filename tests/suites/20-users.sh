# Identity: who you are, what that lets you do, and what it does not.
#
# RESOURCES: none -- `sudo <cmd>` raises privilege for that one command and
# nothing outlives it, so a second session running beside this one is
# unaffected. It was marked unsafe on the assumption that it was not.

assert_contains "id names the user"  "($ESPIX_USER)" "$(dev_run 'id')"
assert_contains "groups lists a group" "$ESPIX_USER" "$(dev_run 'groups')"

# sudo is gated by /etc/sudoers, which ships seeded with %sudo the way Debian
# does it.
assert_contains "sudo reaches root"  "uid=0" "$(dev_run 'sudo id')"
assert_contains "sudo can read a root-only file" "root:" "$(dev_run 'sudo cat /etc/passwd')"

# root is a real account, and it is locked: reachable through sudo, never by
# logging in.
assert_contains "root is locked in /etc/passwd" "root:!:" "$(dev_run 'sudo cat /etc/passwd')"

# Changing another account's password is root's alone.
assert_contains "passwd refuses another account without root" "only root" \
    "$(dev_run 'passwd root somepassword')"

# The same identity, asked from the app side. geteuid() and getegid() are published
# to apps because their absence is not a failed call but a failed *load*: testapp
# died with `undefined symbol: geteuid` once and would have taken every suite using
# it along. The shell's `id` above is the other side of this comparison.
if ! dev_testapp_present; then
    espix_skip "test app not built -- run 'make test-app'"
else
    app_out=$(dev_run "/home/$ESPIX_USER/testapp id")
    shell_uid=$(dev_run 'id' | sed -n 's/^uid=\([0-9]*\).*/\1/p')
    app_uid=$(printf '%s\n' "$app_out" | sed -n 's/^uid=\([0-9]*\).*/\1/p')

    if [ -z "$shell_uid" ] || [ -z "$app_uid" ]; then
        espix_fail "an app is told its own uid" \
                   "shell gave '$shell_uid', the app gave '$app_uid'"
    else
        assert_eq "an app's uid is the shell's" "$shell_uid" "$app_uid"
    fi
fi
