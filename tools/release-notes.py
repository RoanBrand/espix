#!/usr/bin/env python3
"""Print release notes covering every board in a merged espix-ota.json.

    tools/release-notes.py <manifest> <version>

One set of notes for a release that serves several boards, so the body does not
describe whichever target happened to be published first. Deliberately short:
set the port once, then one line per board that flashes it and attaches the
monitor, and nothing else is said twice.

The blank line after each bold label is not cosmetic. Without it the indented
command is a lazy continuation of the paragraph rather than a code block, and
markdown then reads the port placeholder as an unknown HTML tag and eats it.
"""

import json
import sys


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: release-notes.py <manifest> <version>\n")
        return 2
    manifest, ver = argv[1], argv[2]
    with open(manifest) as f:
        boards = json.load(f)["boards"]

    out = ["espix %s" % ver, ""]
    out.append("Set the port once -- macOS /dev/cu.usbserial-*, Linux")
    out.append("/dev/ttyUSB* or /dev/ttyACM*, Windows COM3:")
    out.append("")
    out.append("    PORT=/dev/cu.usbserial-110")
    out.append("")

    for board, entry in sorted(boards.items()):
        model = board.split("-")[0]
        chip = entry.get("chip", "?")
        out.append("**%s**:" % model.upper())
        out.append("")
        out.append("    python -m esptool --chip %s -p \"$PORT\" -b 460800 "
                   "--after no-reset write-flash 0x0 espix-%s-full.bin && "
                   "python -m esp_idf_monitor -p \"$PORT\""
                   % (chip, model))
        out.append("")

    out.append("Assets")
    out.append("")
    for name, what in (
        ("espix-*-minimal.bin", "bootloader, partition table, kernel, loader"),
        ("espix-*-full.bin", "the same plus the stock apps; reflashes everything"),
        ("espix-*-ota.bin", "the kernel, for remote updating"),
        ("espix-ota.json", "the manifest every board reads"),
    ):
        out.append("    %-24s  %s" % (name, what))

    out.append("")
    out.append("Updating an espix already on the network: `sudo upgrade`")

    sys.stdout.write("\n".join(out) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
