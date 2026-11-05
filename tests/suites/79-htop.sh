# htop: the staged app binary, and its version. The interactive screen is not
# driven here -- that needs a real terminal and a keypress -- but this catches a
# build or staging regression that would make 'htop' unrunnable.
#
# RESOURCES: none -- a version string and no shared state.

ver=$(dev_run 'htop --version')
case "$ver" in
    "htop "*)
        espix_pass "htop is staged and reports a version ($ver)"
        ;;
    *)
        espix_skip "htop is not staged for this target (apps/htop/targets)"
        ;;
esac
