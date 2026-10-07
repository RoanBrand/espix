#!/usr/bin/env python3
"""Print release notes covering every board in a merged espix-ota.json.

    tools/release-notes.py <manifest> <version>

One set of notes for a release that serves several boards, so the body does not
describe whichever target happened to be published first."""

import json
import sys


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: release-notes.py <manifest> <version>\n")
        return 2
    manifest, ver = argv[1], argv[2]
    with open(manifest) as f:
        boards = json.load(f)["boards"]

    entries = []
    for board, entry in sorted(boards.items()):
        entries.append((board.split("-")[0], entry.get("chip", "?")))

    out = []
    out.append("espix %s" % ver)
    out.append("")
    out.append("Flashing a board for the first time")
    out.append("-----------------------------------")
    out.append("")
    out.append("Each board has its own image. Write the one for your chip at offset 0:")
    out.append("")
    for model, chip in entries:
        out.append("    esptool.py --chip %s -p <port> write-flash 0x0 espix-%s-minimal.bin"
                   % (chip, model))
    out.append("")
    out.append("Run that in the directory you downloaded the images into, and take the")
    out.append("-full image instead if you want the stock applications. Offset 0 is")
    out.append("right for every board: the image carries its own bootloader at the")
    out.append("offset that chip expects.")
    out.append("")
    out.append("None of these images writes NVS, so a board that has run espix before")
    out.append("keeps its saved network settings. To have it come up as new, erase")
    out.append("first -- which is also what a board out of the box looks like:")
    out.append("")
    for model, chip in entries:
        out.append("    esptool.py --chip %s -p <port> erase-flash" % chip)
    out.append("")
    out.append("then write the image as above.")
    out.append("")
    out.append("That is the whole system, and it makes its own filesystem on first boot.")
    out.append("It has no stock apps in /bin; take the -full image instead if you want")
    out.append("those. The full image holds only the stock applications -- a development")
    out.append("tree's test app and local configuration are never packaged. Flashing it")
    out.append("replaces whatever is on the device.")
    out.append("")
    out.append("Updating an espix already on the network")
    out.append("----------------------------------------")
    out.append("")
    out.append("    sudo upgrade")
    out.append("")
    out.append("It reads espix-ota.json from this release and installs its own board's")
    out.append("entry, so the same command works on every board.")
    out.append("")
    out.append("Assets")
    out.append("------")
    out.append("")
    for model, chip in entries:
        out.append("  espix-%s-minimal.bin   bootloader, partition table, kernel, loader" % model)
        out.append("  espix-%s-full.bin      the same plus the stock apps; reflashes everything" % model)
        out.append("  espix-%s-ota.bin       the kernel, for remote updating" % model)
    out.append("  espix-ota.json         the manifest every board reads")
    out.append("")
    sys.stdout.write("\n".join(out))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
