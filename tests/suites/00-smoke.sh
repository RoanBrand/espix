# Does the device work at all, and is it the build we think it is?
#
# RESOURCES: none -- read-only, touches no shared state.

assert_eq "whoami is the login user" "$ESPIX_USER" "$(dev_run 'whoami')"
assert_contains "id reports a uid and gid" "uid=" "$(dev_run 'id')"
assert_eq "pwd is the home directory" "/home/$ESPIX_USER" "$(dev_run 'pwd')"

uname_out=$(dev_run 'uname -a')
assert_contains "uname says espix" "espix" "$uname_out"

# The prompt sigil is the identity check: '#' means root, '$' means not, and
# espix only earned that distinction once read and write were really enforced.
assert_contains "an ordinary account gets a \$ prompt" "$ESPIX_USER:" "$DEV_PROMPT"
case "$DEV_PROMPT" in
    *'$') espix_pass "the sigil is \$, not # -- this session is not root" ;;
    *)    espix_fail "the sigil is \$, not # -- this session is not root" \
                     "prompt was: $DEV_PROMPT" ;;
esac

motd_out=$(dev_run 'motd')
assert_contains "motd reports an espix version" "espix 0." "$motd_out"
assert_contains "motd reports the IDF version"  "ESP-IDF"  "$motd_out"

# The rootfs the device builds for itself. These exist because espix creates
# them, not because an image shipped them -- fsroot/ is empty of all of this.
for f in /etc/passwd /etc/group /etc/sudoers /etc/hostname; do
    assert_contains "$f exists" "$f" "$(dev_run "ls -l $f")"
done
assert_contains "/tmp is sticky and world-writable" "drwxrwxrwt" "$(dev_run 'ls -l /')"

# Exit status has to come from its own connection: the shell has no $?.
assert_status "a good command exits 0" 0 dev_status 'uptime'
assert_status "an unknown command exits 127" 127 dev_status 'definitelynotacommand'

# An over-long exec command is answered inside the channel, not refused
# (R-P7.10). Refusing made the client say only "exec request failed on channel
# 0", which it cannot tell from a login failure; accepting, printing the reason
# and exiting 130 puts it where the user is looking.
long_cmd=$(printf 'x%.0s' $(seq 1 300))
assert_contains "an over-long exec names the limit" "limit is" \
    "$(dev_once "$long_cmd" 2>&1)"
assert_status "and it exits 130" 130 dev_status "$long_cmd"

# Redirection attached to the operator (R-P2.13). The separated form always
# worked; an attached 2> used to become an argument, so the command was handed
# a file by that name. dev_once rather than dev_status: the status of a
# redirected command comes back through the one-shot exec reliably.
assert_contains "stdout redirects without a space" "hi"     "$(dev_once 'echo hi >/tmp/smoke-redir; cat /tmp/smoke-redir')"
assert_contains "stderr redirects without a space" "st=1"     "$(dev_once 'ls /nonexistent 2>/dev/null; echo st=$?')"
assert_contains "stdin redirects without a space" "$(dev_once 'cat /etc/hostname')"     "$(dev_once 'cat </etc/hostname')"
assert_contains "an unsupported descriptor is refused" "descriptors 0, 1 and 2"     "$(dev_once 'echo x 3>/tmp/smoke-redir')"
