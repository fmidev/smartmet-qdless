#!/usr/bin/env python3
"""Trim and resample CMU mocap BVH files for use in qdless.

The CMU recordings (https://github.com/una-dinosauria/cmu-mocap, CC0) are
captured at 120 fps and typically run several seconds each, with the
subject standing still at the start and end. For an exit effect we don't
need either — we want one clean piece of motion that will loop or play
once at terminal redraw rate (~30-60 fps).

The per-motion table below gives:
  source_id   the BVH file under /tmp/cmu_raw/ (basename without .bvh)
  outname     short name used in qdless's data/cmu/<outname>.bvh
  start       first frame to keep at 120 fps (or None for the file's start)
  end         last frame to keep, exclusive (or None for the file's end)
  stride      sampling step — 2 means 120→60 fps, 4 means 30 fps

Picks for start/end come from CMU's text index plus a manual scan of how
long the subject takes to settle into the motion. Most clips have ~0.5 s
of breathing-room padding at each end that we want to drop.
"""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path

# (source_id, outname, start_frame, end_frame, stride)
MOTIONS = [
    ("35_01",  "walk",       60,  300, 2),  # subject 35 canonical walk
    ("143_01", "run",        10,   90, 2),  # short, mostly running already
    ("120_08", "sneak",     200,  600, 2),  # Mickey sneaky walk mid-clip
    ("143_37", "ladder",     30,  480, 2),  # climb up and down
    ("118_01", "jump",      100,  400, 2),  # one anticipation→jump→land
    ("88_07",  "cartwheel",  10,  150, 2),  # whole clip is the move
    ("113_15", "sit",        80,  700, 2),  # sit + get up sequence
    ("141_16", "wave",       30,  270, 2),  # wave hello
    ("143_23", "punch",      50,  500, 2),  # ~3.7 s of punches
    ("143_24", "kick",       50,  600, 2),  # ~4.5 s of kicks
    ("60_01",  "salsa",     200,  800, 2),  # ~5 s of clean salsa
    ("141_11", "throw",      40,  500, 2),  # throw + catch
]


def parse_bvh(path: Path) -> tuple[list[str], int, float, list[str]]:
    """Return (header_lines, frame_count, frame_time, frame_lines).

    `header_lines` is everything up to and INCLUDING the MOTION marker —
    so the output BVH can just glue updated 'Frames:' + 'Frame Time:'
    lines after it. `frame_lines` is the per-frame channel data."""
    with open(path) as f:
        text = f.read()
    m = re.search(r"^MOTION\s*$", text, re.MULTILINE)
    if not m:
        raise RuntimeError(f"no MOTION section in {path}")
    header = text[: m.end()].splitlines(keepends=False)
    rest = text[m.end():].splitlines(keepends=False)
    # rest[0] is blank, rest[1] is 'Frames: N', rest[2] is 'Frame Time: ...'
    # then frame data
    i = 0
    while i < len(rest) and not rest[i].strip().startswith("Frames:"):
        i += 1
    frames_line = rest[i]
    frame_time_line = rest[i + 1]
    frames = int(frames_line.split(":")[1].strip())
    frame_time = float(frame_time_line.split(":")[1].strip())
    frame_lines = rest[i + 2 : i + 2 + frames]
    return header, frames, frame_time, frame_lines


def write_bvh(path: Path, header: list[str], frame_time: float,
              frame_lines: list[str]) -> None:
    with open(path, "w") as f:
        f.write("\n".join(header))
        f.write("\n")
        f.write(f"Frames: {len(frame_lines)}\n")
        f.write(f"Frame Time: {frame_time:.7f}\n")
        for line in frame_lines:
            f.write(line)
            f.write("\n")


def emit(src_id: str, outname: str, start: int, end: int, stride: int,
         src_root: Path, out_root: Path) -> tuple[int, int]:
    src_path = src_root / f"{src_id}.bvh"
    if not src_path.exists():
        print(f"  skip {outname}: missing {src_path}", file=sys.stderr)
        return (0, 0)
    header, n_total, ft, frames = parse_bvh(src_path)
    a = max(0, start)
    b = min(n_total, end)
    if a >= b:
        print(f"  skip {outname}: empty window {a}..{b}", file=sys.stderr)
        return (0, 0)
    sub = frames[a:b:stride]
    out_path = out_root / f"{outname}.bvh"
    write_bvh(out_path, header, ft * stride, sub)
    return (len(sub), out_path.stat().st_size)


def main():
    src_root = Path("/tmp/cmu_raw")
    out_root = Path(__file__).parent.parent / "data" / "cmu"
    out_root.mkdir(parents=True, exist_ok=True)
    # Clear any stale outputs.
    for p in out_root.glob("*.bvh"):
        p.unlink()
    total_bytes = 0
    print(f"{'motion':10s} {'frames':>7s}  {'bytes':>9s}")
    for src, name, a, b, s in MOTIONS:
        n, sz = emit(src, name, a, b, s, src_root, out_root)
        total_bytes += sz
        print(f"{name:10s} {n:>7d}  {sz:>9d}")
    print(f"{'TOTAL':10s} {'':>7s}  {total_bytes:>9d}")


if __name__ == "__main__":
    main()
