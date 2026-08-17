#!/usr/bin/env python3
"""Compare the feedrate actually in force at each extruding move, across three files.

The G1 X/Y/E moves are identical in all three (verified by hash), so walking them in
parallel gives a true point-by-point comparison: at this exact point in the print, what
feedrate did each tool command?
"""
import re
import sys

F_RE = re.compile(r'(?:^|\s)F([0-9.]+)')
XY_RE = re.compile(r'(?:^|\s)[XY][-0-9.]')
E_RE = re.compile(r'(?:^|\s)E([-0-9.]+)')


def trace(path):
    """Active feedrate at each extruding move, in order."""
    out = []
    f = 0.0
    with open(path, 'r', errors='replace') as fh:
        for line in fh:
            cmd = line.split(';', 1)[0].strip()
            if not (cmd.startswith('G1') or cmd.startswith('G0')):
                continue
            m = F_RE.search(cmd)
            if m:
                f = float(m.group(1))
            e = E_RE.search(cmd)
            if e and float(e.group(1)) > 0 and XY_RE.search(cmd):
                out.append(f)
    return out


def summarise(name, ratios):
    inc = sum(1 for r in ratios if r > 1.0001)
    dec = sum(1 for r in ratios if r < 0.9999)
    same = len(ratios) - inc - dec
    changed = [r for r in ratios if r < 0.9999]
    print(f"  {name}")
    print(f"    moves            : {len(ratios)}")
    print(f"    SLOWED           : {dec} ({100*dec/len(ratios):.1f}%)")
    print(f"    unchanged        : {same} ({100*same/len(ratios):.1f}%)")
    print(f"    SPED UP          : {inc} ({100*inc/len(ratios):.1f}%)")
    if changed:
        changed.sort()
        print(f"    when slowed, to  : median {100*changed[len(changed)//2]:.0f}% "
              f"of slicer speed, min {100*changed[0]:.0f}%")


def main():
    raw, legacy, mine = sys.argv[1], sys.argv[2], sys.argv[3]
    r, l, m = trace(raw), trace(legacy), trace(mine)
    n = min(len(r), len(l), len(m))
    print(f"aligned moves: raw={len(r)} legacy={len(l)} mine={len(m)} -> comparing {n}\n")

    lr = [l[i] / r[i] for i in range(n) if r[i] > 0]
    mr = [m[i] / r[i] for i in range(n) if r[i] > 0]
    summarise("LEGACY vs raw", lr)
    print()
    summarise("MINE vs raw", mr)

    # Where do they disagree, and by how much?
    print("\n  mine vs legacy, move by move")
    diff = [m[i] / l[i] for i in range(n) if l[i] > 0]
    faster = sum(1 for d in diff if d > 1.0001)
    slower = sum(1 for d in diff if d < 0.9999)
    print(f"    mine FASTER than legacy : {faster} ({100*faster/len(diff):.1f}%)")
    print(f"    mine SLOWER than legacy : {slower} ({100*slower/len(diff):.1f}%)")
    print(f"    same                    : {len(diff)-faster-slower}")

    # Phase check: does shifting one trace against the other improve agreement?
    # If the difference were purely a timing/lag effect, some non-zero shift would fit
    # noticeably better than zero.
    print("\n  phase check (mean |mine-legacy| after shifting mine by k moves)")
    best, bestk = None, 0
    for k in (-400, -200, -100, -50, -20, 0, 20, 50, 100, 200, 400):
        acc, cnt = 0.0, 0
        for i in range(n):
            j = i + k
            if 0 <= j < n and l[i] > 0:
                acc += abs(m[j] - l[i])
                cnt += 1
        if cnt:
            err = acc / cnt
            mark = ""
            if best is None or err < best:
                best, bestk = err, k
            print(f"    shift {k:+5d} : {err:8.1f} mm/min{mark}")
    print(f"    -> best shift is {bestk:+d} moves")


if __name__ == '__main__':
    main()
