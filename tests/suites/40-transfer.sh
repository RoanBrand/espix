# scp and sftp: the transports, and the refusals that must survive them.
#
# PARALLEL_SAFE=no -- transfers files to fixed names.

TMP=$(espix_mktemp_dir)

# A round trip has to be byte-exact, not merely successful.
dd if=/dev/urandom of="$TMP/blob.bin" bs=4096 count=1 2>/dev/null
dev_push "$TMP/blob.bin" "/home/$ESPIX_USER/blob.bin" >/dev/null
dev_pull "/home/$ESPIX_USER/blob.bin" "$TMP/back.bin" >/dev/null
if cmp -s "$TMP/blob.bin" "$TMP/back.bin"; then
    espix_pass "4KB scp round-trip is byte-identical"
else
    espix_fail "4KB scp round-trip is byte-identical" "the copy differs from the original"
fi

# scp with no remote path lands in the home directory, which is the commonest
# invocation and the one that breaks if /home/<user> does not exist.
echo "no-path-marker" > "$TMP/nopath.txt"
dev_push "$TMP/nopath.txt" "" >/dev/null
assert_eq "scp with no remote path lands in the home" "no-path-marker" \
    "$(dev_run "cat /home/$ESPIX_USER/nopath.txt")"

# sftp is checked against the same rules as the shell.
printf 'get /etc/passwd %s/stolen\nquit\n' "$TMP" > "$TMP/batch"
sftp_out=$(dev_sftp "$TMP/batch")
assert_contains "sftp refuses a root-only file" "Permission denied" "$sftp_out"
if [ -f "$TMP/stolen" ]; then
    espix_fail "sftp wrote no local file for a refused fetch" \
               "$TMP/stolen exists -- the refusal leaked content"
else
    espix_pass "sftp wrote no local file for a refused fetch"
fi

printf 'get /home/%s/nopath.txt %s/allowed.txt\nquit\n' "$ESPIX_USER" "$TMP" > "$TMP/batch2"
dev_sftp "$TMP/batch2" >/dev/null
assert_eq "sftp fetches a file the user owns" "no-path-marker" \
    "$(cat "$TMP/allowed.txt" 2>/dev/null)"

dev_run "rm /home/$ESPIX_USER/blob.bin" >/dev/null 2>&1
dev_run "rm /home/$ESPIX_USER/nopath.txt" >/dev/null 2>&1
rm -rf "$TMP"
