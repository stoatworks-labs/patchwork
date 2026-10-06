#!/usr/bin/env python3
"""Render every parameter at both ends of its range and fail if any made no
difference.

**This is the only thing in the repo that catches a dead control.** A GLSL
uniform whose name does not match the C++ is silently ignored --
`glGetUniformLocation` returns -1 and `glUniform` on -1 is a documented no-op
-- so a slider can be stone dead while everything compiles, links, loads and
renders.

Each control is swept where it can act. The base context is a small-tiled
wall at one pixel per LED (10 x 7.5 tiles at 320x180) on the panning card,
because at the defaults' two-pixel LEDs and 60-LED tiles CI's raster holds
four tiles and a 2% fault rate has nothing to land on, and because a delay,
a lag or a hold of a still picture is the same picture. Rates are swept from
0 to a rate that is sure to fire. An option parameter reads back 0..1
whatever its count (the fleet's trap), so options are set here by element
index. A control whose ends differ by less than `--floor` (mean 8-bit
difference per channel) is reported as barely alive: vectrix's lesson.

The repeat's window is put mid-card (Repeat From 0.45, the sun and the
skyline): the card's top-left is a sky that varies only vertically, and a
scroll of a picture with no horizontal structure is the same picture.

Every render is 320x180 (CI's raster) and 160 frames at 60 fps.

Usage::

    tools/sweep.py [--build BUILD_DIR] [--verbose] [--jobs N] [--bare]

`--bare` drops every per-control context (keeping only the base), the
addendum's honesty check: what goes dead there is the list of controls that
only act with another one set, which AGENTS.md records.
"""

import argparse
import concurrent.futures
import pathlib
import re
import subprocess
import sys
import tempfile
import zlib

REPO = pathlib.Path(__file__).resolve().parent.parent

BASE = ["LED Pitch=1", "Tile W=32", "Tile H=24"]

# name -> (low setting, high setting, context settings)
SWEEP = {
    "LED Pitch": ("1", "4", []),
    "Fill": ("0", "1", ["LED Pitch=4"]),
    "Tile W": ("16", "40", ["Tile Spread=1"]),
    "Tile H": ("16", "40", ["Tile Spread=1"]),
    "Modules X": ("1", "4", ["Module Spread=1"]),
    "Modules Y": ("1", "4", ["Module Spread=1"]),
    "Scan": ("0", "4", ["Zebra=1"]),
    "Route": ("0", "2", ["Repeat Tiles=3", "Repeat Motion=0", "Repeat From=0.45"]),
    "Start Corner": ("0", "3", ["Repeat Tiles=3", "Repeat Motion=0", "Repeat From=0.45"]),
    "Tiles Per Port": ("0", "5", ["Repeat Tiles=3", "Repeat Motion=0", "Repeat From=0.5"]),
    "Tile Spread": ("0", "1", []),
    "Module Spread": ("0", "1", []),
    "Colour Spread": ("0", "1", []),
    "Batches": ("0", "2", ["Tile Spread=1", "Colour Spread=1"]),
    "Seams": ("0", "1", []),
    "Heat": ("0", "1", ["Heat Time=0"]),
    "Heat Time": ("0", "1", ["Heat=1"]),
    "Repeat Tiles": ("0", "3", []),
    "Repeat From": ("0", "0.5", ["Repeat Tiles=3"]),
    "Repeat Reach": ("0", "1", ["Repeat Tiles=2", "Repeat From=0.45"]),
    "Repeat Motion": ("0", "2", ["Repeat Tiles=3", "Repeat From=0.45"]),
    "Repeat Speed": ("0.5", "0.9", ["Repeat Tiles=3", "Repeat From=0.45"]),
    "Swapped Tiles": ("0", "1", []),
    "Flipped Tiles": ("0", "1", []),
    "Hop Delay": ("0", "1", []),
    "Lag Tiles": ("0", "1", []),
    "Lag Frames": ("1", "31", ["Lag Tiles=1"]),
    "Chain Break": ("0", "1", []),
    "Intermittent": ("0", "1", ["Chain Break=1"]),
    "Dropouts": ("0", "0.5", []),
    "Dropout Time": ("0", "1", ["Dropouts=0.5"]),
    "Lost Signal": ("0", "2", ["Chain Break=1"]),
    "Dead Tiles": ("0", "0.5", []),
    "Flicker Tiles": ("0", "1", []),
    "Flicker Rate": ("0", "1", ["Flicker Tiles=1"]),
    "PSU Limit": ("1", "0.2", []),
    "PSU Mode": ("0", "1", ["PSU Limit=0.2"]),
    "Dead Modules": ("0", "0.5", []),
    "Zebra": ("0", "1", []),
    "Zebra Width": ("1", "4", ["Zebra=1"]),
    "Zebra Mode": ("0", "1", ["Zebra=1"]),
    "Dead Rows": ("0", "1", []),
    "Colour Loss": ("0", "1", []),
    "Shifted": ("0", "1", []),
    "Dead LEDs": ("0", "1", []),
    "Stuck LEDs": ("0", "1", []),
    "Fault Seed": ("1", "2", []),
    "Mix": ("0", "1", []),
}

# The About block: a text line and browser buttons, with no pixel to sweep.
SKIP = {"About", "Project page", "Source on GitHub", "Support the work", "User guide"}


def read_png(path):
    """Enough of PNG for pwtest's own writer: 8-bit RGBA, filter 0 rows."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, width, height, idat = 8, 0, 0, b""
    while pos < len(data):
        length = int.from_bytes(data[pos:pos + 4], "big")
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width = int.from_bytes(body[0:4], "big")
            height = int.from_bytes(body[4:8], "big")
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    raw = zlib.decompress(idat)
    stride = width * 4
    out = bytearray()
    for y in range(height):
        start = y * (stride + 1)
        out += raw[start + 1:start + 1 + stride]
    return bytes(out)


def difference(a, b):
    if len(a) != len(b):
        return 255.0
    return sum(abs(x - y) for x, y in zip(a, b)) / len(a)


def render(pwtest, out, settings):
    args = [str(pwtest), "--out", str(out), "--size", "320x180", "--frames", "160", "--moving"]
    for setting in settings:
        args += ["--set", setting]
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"pwtest failed: {' '.join(args)}\n{result.stderr.strip()}")
    return read_png(out)


def parameters(pwtest):
    result = subprocess.run([str(pwtest), "--list"], capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"pwtest --list failed: {result.stderr.strip()}")
    names = []
    for line in result.stdout.splitlines()[1:]:
        parts = re.split(r"\s{2,}", line.strip())
        if len(parts) >= 3 and parts[0].isdigit():
            names.append(parts[1].strip())
    return names


def sweep_one(pwtest, scratch, name, bare):
    low, high, context = SWEEP[name]
    context = BASE + ([] if bare else context)
    tag = f"p{abs(hash(name))}"
    before = render(pwtest, scratch / f"{tag}-a.png", context + [f"{name}={low}"])
    after = render(pwtest, scratch / f"{tag}-b.png", context + [f"{name}={high}"])
    return name, difference(before, after)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=pathlib.Path, default=REPO / "build")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--bare", action="store_true")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--floor", type=float, default=0.05,
                        help="mean 8-bit difference below which a control is 'barely alive'")
    args = parser.parse_args()

    build = args.build if args.build.is_absolute() else REPO / args.build
    pwtest = build / "pwtest"
    if not pwtest.exists():
        print(f"{pwtest} not found", file=sys.stderr)
        return 1

    declared = parameters(pwtest)
    unknown = [n for n in declared if n not in SWEEP and n not in SKIP]
    if unknown:
        # A new parameter with no sweep is a hole, not a pass.
        print(f"no sweep defined for: {', '.join(unknown)}", file=sys.stderr)
        return 1
    unused = [n for n in SWEEP if n not in declared]
    if unused:
        print(f"the sweep names parameters the plugin does not have: {', '.join(unused)}", file=sys.stderr)
        return 1

    dead, weak = [], []
    with tempfile.TemporaryDirectory() as scratch:
        scratch = pathlib.Path(scratch)
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            results = list(pool.map(lambda n: sweep_one(pwtest, scratch, n, args.bare), [n for n in declared if n in SWEEP]))
    for name, delta in results:
        if delta == 0.0:
            dead.append(name)
            print(f"  DEAD {name:16s} both ends identical")
        elif delta < args.floor:
            weak.append(name)
            print(f"  WEAK {name:16s} mean delta {delta:.4f}")
        elif args.verbose:
            print(f"  ok   {name:16s} mean delta {delta:.3f}")

    print(f"{len(results)} parameters swept{' (bare: base context only)' if args.bare else ''}, "
          f"{len(dead)} dead, {len(weak)} barely alive")
    if dead or weak:
        if not args.bare:
            print("\nA parameter that changes nothing is usually a uniform name that does not match\n"
                  "the C++, or a setting nothing reads. Both are silent everywhere else.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
