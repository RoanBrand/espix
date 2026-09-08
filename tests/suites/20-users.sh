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
