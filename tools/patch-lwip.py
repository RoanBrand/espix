#!/usr/bin/env python3
"""
Give lwIP's struct netif two fewer client-data slots, so it matches the layout
IDF's prebuilt Thread border-router library was compiled against.

Why
---
components/openthread/lib/<target>/libopenthread_br.a is C compiled against
struct netif, and reads and writes its fields at fixed offsets -- the library
records them as esp_openthread_check_netif_*_offset(). Everything after
client_data[] moves with the array's length, and the library expects four
entries. espix had six: DHCP, ACD, IGMP and MLD6 that lwIP reserves, one that
esp_netif allocates (it must: WireGuard's wg0 is a plain lwIP netif, and without
its own slot esp_netif reads every netif's `state` as an esp_netif), and one that
lwIP's bridge allocates. docs/THREAD.md, section 10, has what the mismatch broke.

Two of those six hold nothing that needs a slot:

1. ACD's. lwip/netif.h reserves LWIP_NETIF_CLIENT_DATA_INDEX_ACD whenever
   LWIP_ACD is on, but nothing reads or writes it -- acd.c keeps its state in
   netif->acd_list. The patch stops reserving it.
2. The bridge's. bridgeif.c parks each port's bridgeif_port_t in the port
   netif's client data, only so bridgeif_input() can find it again. The patch
   keeps a list of bridges instead and finds the port by its netif: a bridge has
   at most BRIDGEIF_MAX_PORTS ports and is never freed (bridgeif has no deinit).

That leaves DHCP, IGMP, MLD6 and esp_netif's -- four -- with ACD, br0 and wg0
all working. tools/lwip-netif-client-data.patch is the same change as a diff.

Where this writes
-----------------
Both files are inside the IDF installation's lwIP, as tools/patch-fatfs.py
patches FatFs, and the CMake hook re-applies it on every configure.

Failure policy
--------------
Loud, never silent: a missing file or a moved anchor stops the build, and
running twice is a no-op.
"""

import argparse
import sys
from pathlib import Path

MARK = "tools/patch-lwip.py"

NETIF_H = "components/lwip/lwip/src/include/lwip/netif.h"
BRIDGEIF_C = "components/lwip/lwip/src/netif/bridgeif.c"

NETIF_EDITS = [
    (
        "#if LWIP_ACD\n"
        "   LWIP_NETIF_CLIENT_DATA_INDEX_ACD,\n"
        "#endif\n",
        "   /* No slot for ACD: acd.c keeps its state in acd_list (tools/patch-lwip.py). */\n",
    ),
]

BRIDGEIF_EDITS = [
    (
        "  void             *fdbd;\n"
        "} bridgeif_private_t;\n",
        "  void             *fdbd;\n"
        "  struct bridgeif_private_s *next;\n"
        "} bridgeif_private_t;\n",
    ),
    (
        "/* netif data index to get the bridge on input */\n"
        "static u8_t bridgeif_netif_client_id = 0xff;\n",
        "/* Every bridge, so a port finds its own without a client-data slot\n"
        " * (tools/patch-lwip.py). Bridges are never freed. */\n"
        "static bridgeif_private_t *bridgeif_list;\n"
        "\n"
        "static bridgeif_port_t *\n"
        "bridgeif_port_of(struct netif *portif)\n"
        "{\n"
        "  bridgeif_private_t *br;\n"
        "  u8_t i;\n"
        "  for (br = bridgeif_list; br != NULL; br = br->next) {\n"
        "    for (i = 0; i < br->num_ports; i++) {\n"
        "      if (br->ports[i].port_netif == portif) {\n"
        "        return &br->ports[i];\n"
        "      }\n"
        "    }\n"
        "  }\n"
        "  return NULL;\n"
        "}\n",
    ),
    (
        "  port = (bridgeif_port_t *)netif_get_client_data(netif, bridgeif_netif_client_id);\n",
        "  port = bridgeif_port_of(netif);\n",
    ),
    (
        "  if (bridgeif_netif_client_id == 0xFF) {\n"
        "    bridgeif_netif_client_id = netif_alloc_client_data_id();\n"
        "  }\n"
        "\n",
        "",
    ),
    (
        "    mem_free(br);\n"
        "    return ERR_MEM;\n"
        "  }\n",
        "    mem_free(br);\n"
        "    return ERR_MEM;\n"
        "  }\n"
        "  br->next = bridgeif_list;\n"
        "  bridgeif_list = br;\n",
    ),
    (
        "  /* store pointer to bridge in netif */\n"
        "  netif_set_client_data(portif, bridgeif_netif_client_id, port);\n",
        "",
    ),
]


def patch(path: Path, edits) -> bool:
    try:
        text = path.read_text()
    except OSError as exc:
        raise SystemExit(f"patch-lwip: cannot read {path}: {exc}")

    if MARK in text:
        return False

    for old, new in edits:
        if text.count(old) != 1:
            raise SystemExit(f"patch-lwip: anchor not found exactly once in {path}:\n{old}")
        text = text.replace(old, new, 1)

    try:
        path.write_text(text)
    except OSError as exc:
        raise SystemExit(f"patch-lwip: cannot write {path}: {exc}")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf-path", required=True, type=Path)
    args = parser.parse_args()

    for rel, edits in ((NETIF_H, NETIF_EDITS), (BRIDGEIF_C, BRIDGEIF_EDITS)):
        path = args.idf_path / rel
        if patch(path, edits):
            print(f"patch-lwip: patched {path}")
        else:
            print(f"patch-lwip: already patched ({path})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
