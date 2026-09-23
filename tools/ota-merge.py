#!/usr/bin/env python3
"""Merge per-board espix OTA manifests into the one a release publishes.

    tools/ota-merge.py <out.json> <release-base-url> <in.json> ...

Each input is a single-board manifest as tools/ota-manifest.sh writes it. Every
board keeps its version and hashes; `url` is rewritten to the release base this
run publishes under, so a manifest left in another target's build directory from
an earlier version cannot point an update at the wrong tag.

One manifest can then serve every target: the device reads its own board's
sub-object out of it.
"""
import json
import sys


def main():
    if len(sys.argv) < 4:
        sys.stderr.write(__doc__)
        return 2

    out = sys.argv[1]
    base = sys.argv[2].rstrip("/")
    boards = {}

    for path in sys.argv[3:]:
        try:
            with open(path) as f:
                data = json.load(f)
        except (OSError, ValueError):
            continue
        for board, entry in (data.get("boards") or {}).items():
            model = board.split("-", 1)[0]
            entry = dict(entry)
            entry["url"] = "%s/espix-%s-ota.bin" % (base, model)
            boards[board] = entry

    if not boards:
        sys.stderr.write("ota-merge: no boards found in the given manifests\n")
        return 1

    with open(out, "w") as f:
        json.dump({"format": 1, "boards": boards}, f, indent=2)
        f.write("\n")

    print("ota-merge: %s (%d board(s))" % (out, len(boards)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
