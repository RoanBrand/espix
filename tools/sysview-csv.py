#!/usr/bin/env python3
"""Turn a SystemView CSV export into the numbers a profiling question needs.

SystemView writes one row per event with Export Data -> SystemView Export
(*.csv). This reads that back and reduces the espix RFB markers, which are the
ones rfb.c emits in a PROFILE=sysview build:

    0 canvas   lock, damage/move take, copy-out to the staging buffer
    1 encode   update headers plus Hextile/Raw (contains 2 and 3/4)
    2 wire     a socket write that blocked
    3 copy     the update carried a CopyRect header
    4 pixels   the update carried pixel rectangles

and the per-update line 'rfb copy=.. rects=.. bytes=.. flushes=..' that goes
with them. The point is to answer 'where did the drag time go, and is it
waiting or computing' without reading the timeline by eye.

    tools/sysview-csv.py /tmp/heavy_window_dragging.csv

Nothing here needs the board or the ELF; the numbers are all in the export.
"""
import argparse
import csv
import statistics
import sys
from collections import defaultdict

MARKERS = {0: "canvas", 1: "encode", 2: "wire", 3: "copy", 4: "pixels"}


def parse_ts(s):
    """'0.378 260 000' -> 0.37826 seconds. SystemView groups the digits."""
    try:
        return float(s.strip().replace(" ", ""))
    except ValueError:
        return None


def parse_dur(s):
    """'Runs for 35.000 us, pass #1' -> 35.0 (us). or '12 ms' -> 12000.0."""
    if "for " not in s:
        return None
    rest = s.split("for ", 1)[1].strip()
    parts = rest.split()
    try:
        v = float(parts[0])
    except (ValueError, IndexError):
        return None
    return v * 1000.0 if len(parts) > 1 and parts[1].startswith("ms") else v


def parse_packet(row):
    """Pull the numbers out of the 'rfb copy=..' line, wherever it landed."""
    for field in row:
        if "rfb copy=" not in field:
            continue
        vals = {}
        for tok in field.split():
            if "=" in tok:
                k, v = tok.split("=", 1)
                try:
                    vals[k] = int(v)
                except ValueError:
                    pass
        if "bytes" in vals:
            return vals
    return None


def pct(v, q):
    if not v:
        return 0.0
    v = sorted(v)
    return v[min(len(v) - 1, int(q * len(v)))]


def ms(x):
    return x / 1000.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv", help="SystemView Export Data CSV")
    ap.add_argument("--tail", type=float, default=10.0,
                    help="ms; updates whose encode CPU exceeds this are the tail")
    args = ap.parse_args()

    marks = []
    packets = []
    rows = 0
    with open(args.csv, newline="", encoding="latin-1") as f:
        r = csv.reader(f)
        next(r, None)
        for row in r:
            if len(row) < 5:
                continue
            rows += 1
            ev = row[3]
            if "Start Marker" in ev and "0x" in ev:
                t = parse_ts(row[1])
                d = parse_dur(row[4])
                if t is not None and d is not None:
                    marks.append((t, int(ev.split("0x")[-1].strip(), 16), d))
            p = parse_packet(row)
            if p is not None:
                packets.append(p)

    if not marks:
        sys.exit("%s: no markers -- was a PROFILE=sysview build running?" % args.csv)

    dur = defaultdict(list)
    for _, mid, d in marks:
        dur[mid].append(d)

    # Group the markers into updates, one canvas pair per update.
    updates = []
    cur = None
    for t, mid, d in marks:
        if mid == 0:
            if cur:
                updates.append(cur)
            cur = {"t": t, "canvas": d, "encode": 0.0, "wire": 0.0,
                   "copy": 0, "pixels": 0}
        elif cur is not None:
            if mid == 1:
                cur["encode"] += d
            elif mid == 2:
                cur["wire"] += d
            elif mid == 3:
                cur["copy"] += 1
            elif mid == 4:
                cur["pixels"] += 1
    if cur:
        updates.append(cur)

    span = updates[-1]["t"] - updates[0]["t"] if len(updates) > 1 else 0.0
    print("trace: %s" % args.csv)
    print("rows %d   updates %d   burst %.2f s   %.1f updates/s"
          % (rows, len(updates), span, (len(updates) - 1) / span if span else 0))

    print("\nphase (per update, ms):")
    print("  %-12s %5s %9s %9s %9s %9s %9s %9s" %
          ("", "n", "mean", "median", "p90", "p99", "max", "total s"))
    series = {
        "canvas": [u["canvas"] for u in updates],
        "encode CPU": [u["encode"] - u["wire"] for u in updates],
        "wire": [u["wire"] for u in updates],
        "encode all": [u["encode"] for u in updates],
        "update": [u["canvas"] + u["encode"] for u in updates],
    }
    for name, v in series.items():
        print("  %-12s %5d %9.2f %9.2f %9.2f %9.2f %9.2f %9.3f" %
              (name, len(v), ms(statistics.mean(v)), ms(statistics.median(v)),
               ms(pct(v, .90)), ms(pct(v, .99)), ms(max(v)), sum(v) / 1e6))

    enc_cpu = series["encode CPU"]
    over = [x for x in enc_cpu if x / 1000.0 > args.tail]
    print("\nencode tail: %d/%d updates over %.0f ms (worst %.1f ms)"
          % (len(over), len(enc_cpu), args.tail, ms(max(enc_cpu))))

    wire = sum(u["wire"] for u in updates)
    total = sum(series["update"])
    if total:
        print("wire is %.1f%% of all update time" % (100.0 * wire / total))

    both = sum(1 for u in updates if u["copy"] and u["pixels"])
    copy_only = sum(1 for u in updates if u["copy"] and not u["pixels"])
    px_only = sum(1 for u in updates if u["pixels"] and not u["copy"])
    print("\npath: copy=%d (copy+pixels=%d, copy only=%d)  pixels only=%d"
          % (sum(1 for u in updates if u["copy"]), both, copy_only, px_only))

    if packets:
        b = [p["bytes"] for p in packets]
        fl = [p["flushes"] for p in packets]
        rc = [p["rects"] for p in packets]
        print("packets: bytes mean=%.0f median=%.0f p90=%.0f max=%d   "
              "rects mean=%.1f   flushes mean=%.2f max=%d"
              % (statistics.mean(b), statistics.median(b), pct(b, .9), max(b),
                 statistics.mean(rc), statistics.mean(fl), max(fl)))
        if "moves" in packets[0]:
            hist = {}
            for p in packets:
                hist[p["moves"]] = hist.get(p["moves"], 0) + 1
            print("moves/update: " + ", ".join(
                "%d x%d" % (k, hist[k]) for k in sorted(hist))
                + "   full frames: %d   client without copyrect: %d"
                % (sum(1 for p in packets if p.get("full")),
                   sum(1 for p in packets if p.get("cap") == 0)))
    else:
        print("packets: none logged (older build; the rfb line is what adds them)")

    print("\nheaviest updates (canvas / encode CPU / wire / copy+pixels, ms):")
    for u in sorted(updates, key=lambda u: -(u["encode"]))[:5]:
        print("  %7.2f  %7.2f  %7.2f   %d+%d"
              % (ms(u["canvas"]), ms(u["encode"] - u["wire"]), ms(u["wire"]),
                 u["copy"], u["pixels"]))


if __name__ == "__main__":
    main()
