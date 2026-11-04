#!/usr/bin/env python3
"""Find which draws of one pixel shader paint something wrong, while the game shows it and the view holds still.

Captures the frame as it is, then with the whole shader skipped; the pixels that differ are what the shader paints. Then
keeps only a range of its draws (only_range=<shader>:<first>-<last>, every other draw of it skipped), halving any range
that still paints a share of those pixels, until single draws are left. Keeping rather than skipping finds draws that
overlap, where skipping some leaves the rest painting the same place. Prints each culprit's line from the draw list,
and saves the base capture's vertex constants (vconst) beside it in the logs folder for comparing with a clean frame.

Usage: tools/find-slab-draws.py [shader, default 1CCAE300]. Needs ImageMagick. Uses metal-debug.txt and removes it after.
"""
import os
import re
import subprocess
import sys
import time

LOGS = os.path.expanduser("~/Developer/crackdown/work/Crackdown Data/logs")
CONTROL = os.path.join(LOGS, "metal-debug.txt")
SHADER = (sys.argv[1] if len(sys.argv) > 1 else "1CCAE300").upper()
MIN_SHARE = 0.03  # A range that paints less than this share of the shader's pixels is left alone.
MAX_STEPS = 80


def capture(words, name):
    with open(CONTROL, "w") as f:
        f.write(" ".join(words + [f"dump={name}"]))
    path = os.path.join(LOGS, f"live-{name}.png")
    started = time.time()
    while time.time() - started < 8:
        time.sleep(0.3)
        if os.path.exists(path) and os.path.getmtime(path) >= started:
            time.sleep(0.4)
            return path
    sys.exit(f"no capture for {name}: is the game running with the Metal renderer?")


def mean(*args):
    out = subprocess.run(["magick", *args, "-format", "%[fx:mean]", "info:"], capture_output=True, text=True).stdout
    return float(out or 0)


DIFF = "(abs(u.r-v.r)+abs(u.g-v.g)+abs(u.b-v.b))"


def painted_share(base, without):
    return mean(base, without, "-resize", "320x180!", "-fx", f"{DIFF}>0.3?1:0")


def kept_share(base, without, trial):
    # Of the pixels the shader paints, how many still look as they do with all of it when only this range is kept.
    return mean(base, without, trial, "-resize", "320x180!", "-fx",
                f"({DIFF}>0.3 && (abs(u[2].r-u.r)+abs(u[2].g-u.g)+abs(u[2].b-u.b))<0.15)?1:0")


def main():
    base = capture(["vconst"], "slab-base")
    lines = [l for l in open(os.path.join(LOGS, "live-slab-base.txt")) if re.match(rf"^\d+ D .*? ps {SHADER} ", l)]
    count = len(lines)
    without = capture([f"skip_ps={SHADER}"], "slab-without")
    painted = painted_share(base, without)
    print(f"{SHADER}: {count} draws, painting {painted * 100:.1f}% of the frame", flush=True)
    if not count or painted < 0.001:
        print("nothing to find: the shader paints nothing that changes")
        return
    found, pending, step = [], [(0, count - 1)], 0
    while pending and step < MAX_STEPS:
        first, last = pending.pop()
        step += 1
        trial = capture([f"only_range={SHADER}:{first}-{last}"], f"slab-{step}")
        share = kept_share(base, without, trial) / painted
        print(f"draws {first}-{last} alone: paint {share * 100:.0f}%", flush=True)
        if share < MIN_SHARE:
            continue
        if first == last:
            found.append((first, share))
            continue
        middle = (first + last) // 2
        pending += [(middle + 1, last), (first, middle)]
    print("culprits:", flush=True)
    for number, share in sorted(found):
        print(f"  #{number} paints {share * 100:.0f}% alone: {lines[number].strip()[:260]}", flush=True)
    if pending:
        print(f"stopped after {MAX_STEPS} captures with {len(pending)} ranges unsearched", flush=True)


try:
    main()
finally:
    with open(CONTROL, "w") as f:
        f.write("")
    time.sleep(1.5)
    if os.path.exists(CONTROL):
        os.remove(CONTROL)
