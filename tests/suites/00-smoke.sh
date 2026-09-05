# Does the device work at all, and is it the build we think it is?
#
# PARALLEL_SAFE=yes -- read-only, touches no shared state.

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
