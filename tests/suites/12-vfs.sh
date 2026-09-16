# The contracts between espix's VFS and ESP-IDF's.
#
# RESOURCES: none of its own -- read-only, and the mount half skips loudly.
#
# Run it alone. Two harnesses at once is not a thing this suite tolerates: the
# copy loop below is the heaviest rootfs load in the set, and two of those
# interleaved on one device is a concurrency scenario nothing else here creates.
# It also wants the harness's own arguments -- --pass, not --password, and
# ESPIX_PYTHON exported -- which is worth knowing before a silent login failure:
#
#   ESPIX_SERLOG=/tmp/serlog.txt ESPIX_PYTHON=<idf venv python> \
#     ./tests/run.sh --suite vfs --host <addr> --user esp --pass espix \
#                    --port /dev/tty.usbserial-...
#
# Every fd bug in the Stage 2 work was one of these assumptions changing quietly:
# a path that stopped reaching espix's permission check, a lower filesystem
# reachable under a name of its own, an fd table that leaked an entry per open,
# two mounts whose fds collided because both count from zero. None of it shows up
# in an ordinary listing, so it is asserted here. docs/ROADMAP.md has why the
# assumptions exist and what the alternatives would cost.
#
# select() is deliberately absent: tests/suites/50-console.sh owns it, and a
# second, weaker check here would be noise rather than coverage.
#
# 1. Paths still reach espix, so the permission check still runs. espix's VFS is
#    the only thing registered at the root; this is the assertion that fails if a
#    second registration ever claims a prefix, or if the root registration loses
#    its path (a path-less one receives no paths at all -- that cost a boot loop
#    once, see docs/GOTCHAS.md).
assert_contains "an ordinary account cannot write into /etc" "denied" \
                "$(dev_run 'cp /etc/hostname /etc/vfs-probe 2>&1')"
assert_contains "and can still read it" "esp32" "$(dev_run 'cat /etc/hostname')"

# 2. The open/close loop. Four fds per copy -- source, destination, and the two
#    the read-back opens -- so twelve copies is nearly fifty opens: past the point
#    where a table sized by CONFIG_LWIP_MAX_SOCKETS ran out, and past the leak a
#    non-permanent espix fd entry used to be. A copy that fails says so, and a
#    copy that arrives empty is caught by the size check.
loop_msg=""
for i in 1 2 3 4 5 6 7 8 9 10 11 12; do
    copy_out=$(dev_run "cp /etc/hostname /home/$ESPIX_USER/vfs-loop-$i.txt 2>&1")
    if [ -n "$copy_out" ]; then
        loop_msg="copy $i: $copy_out"
        break
    fi
done
if [ -z "$loop_msg" ]; then
    espix_pass "twelve copies in a row, four fds each, none of them refused"
else
    espix_fail "twelve copies in a row, four fds each, none of them refused" \
               "$loop_msg"
fi

# The copy has to hold what the source holds: a lost write arrives as an empty
# file with every call reporting success, which is the bug cp's read-back exists
# to catch.
assert_not_contains "the last copy is not empty" " 0 " \
                    "$(dev_run "ls -l /home/$ESPIX_USER/vfs-loop-12.txt")"
assert_contains "and its contents match the source" "$(dev_run 'cat /etc/hostname')" \
                "$(dev_run "cat /home/$ESPIX_USER/vfs-loop-12.txt")"

dev_run "rm /home/$ESPIX_USER/vfs-loop-1.txt /home/$ESPIX_USER/vfs-loop-2.txt \
      /home/$ESPIX_USER/vfs-loop-3.txt /home/$ESPIX_USER/vfs-loop-4.txt \
      /home/$ESPIX_USER/vfs-loop-5.txt /home/$ESPIX_USER/vfs-loop-6.txt \
      /home/$ESPIX_USER/vfs-loop-7.txt /home/$ESPIX_USER/vfs-loop-8.txt \
      /home/$ESPIX_USER/vfs-loop-9.txt /home/$ESPIX_USER/vfs-loop-10.txt \
      /home/$ESPIX_USER/vfs-loop-11.txt /home/$ESPIX_USER/vfs-loop-12.txt" \
      >/dev/null 2>&1

# 3. A lower filesystem has no second name of its own. The stacked design exists
#    so that a filesystem is reachable only where it is mounted; the IDF patches
#    in tools/ are what keeps it that way, and a plausible prefix resolving is how
#    that would break.
assert_contains "a lower filesystem has no name of its own" "no such file" \
                "$(dev_run 'ls /sd1 2>&1')"

# 4. The one deliberate permission bypass stays where it is documented. Its
#    callers are the mode lookup, the ELF probe's stat, and espix_auth -- which
#    needs it to read /etc/passwd before anyone has authenticated. Anything
#    outside espix_fs and espix_auth is a security bug rather than a shortcut,
#    and docs/KNOWN-ISSUES.md says so.
priv_hits=$(grep -rn 'espix_fs_priv_begin();' "$ESPIX_ROOT/components" 2>/dev/null |
            grep -v '/espix_fs/' |
            grep -v '/espix_auth/' || true)
priv_count=$(printf '%s\n' "$priv_hits" | grep -c 'espix_fs_priv_begin' || true)
if [ "$priv_count" = "0" ]; then
    espix_pass "the permission bypass has no caller outside espix_fs and espix_auth"
else
    espix_fail "the permission bypass has no caller outside espix_fs and espix_auth" \
               "callers: $(printf '%s' "$priv_hits" | tr '\n' ' ')"
fi

# 5. Two mounts, one fd space. This is the shape of the original bug -- a file on
#    the stick and a file on the rootfs sharing one number, so a close of one
#    broke the other -- and it needs root to mount, which this suite does not
#    have. Run by hand: mount a FAT volume, then interleave reads and copies on
#    it and on the rootfs, and check that every one of them succeeds.
espix_skip "the cross-mount fd check needs root to mount; see the note above"

# 6. What an app is told about ownership. stat() answers from the ownership rule
#    and fstat() does not, so an app that opens a file and asks who owns it is
#    given a different answer than one that stats the path -- which the roadmap's
#    surface table lists as two rows. The app prints its own uid between them,
#    which is the comparison that matters and needs no uid known in advance: a
#    filesystem telling an app that a file it owns belongs to root is the bug.
if ! dev_testapp_present; then
    espix_skip "test app not built -- run 'make test-app'"
else
    app="/home/$ESPIX_USER/testapp"
    probe="/home/$ESPIX_USER/stat-probe.txt"
    dev_run "$app write $probe owner" >/dev/null 2>&1

    out=$(dev_run "$app stat $probe")
    stat_line=$(printf '%s\n' "$out" | sed -n '/^stat /p')
    assert_contains "an app can stat a file it owns" "uid=" "$out"

    # The contract, and the bug the surface table records: a filesystem telling an
    # app that a file it owns belongs to root. No uid is assumed -- only that it is
    # not 0, which is what being told root means.
    assert_not_contains "stat() does not report root for a file the app owns" \
                        "uid=0" "$stat_line"

    # fstat() is reported and not asserted: the table lists its st_uid as still 0,
    # and asserting that would lock in a behaviour the fix is undecided about.
    printf '%s\n' "$out" | sed -n 's/^fstat /testapp fstat /p'
fi
