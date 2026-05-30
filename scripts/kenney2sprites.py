#!/usr/bin/env python3
"""Extract animation sequences from Kenney's "Platformer Art Deluxe" pack
(https://kenney.nl/assets/platformer-art-deluxe — CC0 / public domain).

Inputs:  kenney_platformer-art-deluxe.zip extracted to /tmp/kenney-deluxe-extract
Outputs: data/kenney/<motion>/frame_NN.png — each a tightly-cropped RGBA
         PNG with the original Kenney art (cartoon colours preserved).

The Kenney sprites are already alpha-clean PNGs from a 2014-era platformer
pack, so this script just:

  1) Picks 16 motion sequences across the player, alien and creature
     sprite sheets to give a varied gallery (walk / jump / climb / swim /
     fly / etc.);
  2) Tightly crops each frame to its non-transparent bounding box, with
     a 2-px padding so the silhouette doesn't kiss the sprite edge;
  3) For multi-frame cycles uses a UNION bounding box across the whole
     cycle so each frame stays positioned in a common reference frame
     and the figure doesn't visually scoot from frame to frame.

The original sprites are 50-100 px tall — we keep their native size
since they're already small enough for terminal rendering through
ImageSource.pixelAtUV.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

from PIL import Image

SRC_ROOT = "/tmp/kenney-deluxe-extract"

# Each motion is (output-name, [list of source files in cycle order]).
# Order in this table also dictates the gallery's 4×4 layout: row 0 =
# the human player; row 1 = a sci-fi alien; row 2 = small creatures
# (ground); row 3 = small creatures (air & water).
B = "Base pack"
X = "Extra animations and enemies"

MOTIONS: list[tuple[str, list[str]]] = [
    # Row 0: human player with the 11-frame walk cycle as the showpiece.
    ("walk", [f"{B}/Player/p1_walk/PNG/p1_walk{i:02d}.png" for i in range(1, 12)]),
    ("jump", [f"{B}/Player/p1_jump.png"]),
    ("duck", [f"{B}/Player/p1_duck.png"]),
    ("hurt", [f"{B}/Player/p1_hurt.png"]),
    # Row 1: alien with multiple 2-frame cycles.
    ("alien_walk",  [f"{X}/Alien sprites/alienBeige_walk1.png",
                     f"{X}/Alien sprites/alienBeige_walk2.png"]),
    ("alien_climb", [f"{X}/Alien sprites/alienBeige_climb1.png",
                     f"{X}/Alien sprites/alienBeige_climb2.png"]),
    ("alien_swim",  [f"{X}/Alien sprites/alienBeige_swim1.png",
                     f"{X}/Alien sprites/alienBeige_swim2.png"]),
    ("alien_jump",  [f"{X}/Alien sprites/alienBeige_jump.png"]),
    # Row 2: ground creatures.
    ("snail",  [f"{B}/Enemies/snailWalk1.png", f"{B}/Enemies/snailWalk2.png"]),
    ("slime",  [f"{B}/Enemies/slimeWalk1.png", f"{B}/Enemies/slimeWalk2.png"]),
    ("spider", [f"{X}/Enemy sprites/spider_walk1.png",
                f"{X}/Enemy sprites/spider_walk2.png"]),
    ("fish",   [f"{B}/Enemies/fishSwim1.png",  f"{B}/Enemies/fishSwim2.png"]),
    # Row 3: air creatures + bonus.
    ("fly", [f"{B}/Enemies/flyFly1.png", f"{B}/Enemies/flyFly2.png"]),
    ("bee", [f"{X}/Enemy sprites/bee.png",     f"{X}/Enemy sprites/bee_fly.png"]),
    ("bat", [f"{X}/Enemy sprites/bat.png",     f"{X}/Enemy sprites/bat_fly.png"]),
    # Three players walking together (1-frame from each) covers the "all
    # player skins" angle without needing 33 more frames.
    ("trio", [
        f"{B}/Player/p1_walk/PNG/p1_walk03.png",
        f"{B}/Player/p2_walk/PNG/p2_walk03.png",
        f"{B}/Player/p3_walk/PNG/p3_walk03.png",
    ]),
]


def tight_bbox_union(images: list[Image.Image]) -> tuple[int, int, int, int]:
    """Bounding box covering the non-transparent region across every
    frame, so the cycle's figure stays in a common reference frame."""
    boxes = [im.getbbox() for im in images]
    boxes = [b for b in boxes if b is not None]
    if not boxes:
        raise RuntimeError("no opaque pixels found")
    pad = 2
    W, H = images[0].size
    x0 = max(0, min(b[0] for b in boxes) - pad)
    y0 = max(0, min(b[1] for b in boxes) - pad)
    x1 = min(W, max(b[2] for b in boxes) + pad)
    y1 = min(H, max(b[3] for b in boxes) + pad)
    return (x0, y0, x1, y1)


def emit(motion: str, files: list[str], out_root: Path) -> int:
    images = []
    for rel in files:
        p = Path(SRC_ROOT) / rel
        if not p.exists():
            print(f"  skip {motion}: missing {rel}", file=sys.stderr)
            return 0
        images.append(Image.open(p).convert("RGBA"))
    # Frames in a sequence aren't always the same size in the Kenney
    # pack — running frames sway forward, hurt frames are squatter.
    # Resize everything to the largest frame's dimensions before taking
    # the union bbox so they share a coordinate system.
    max_w = max(im.width for im in images)
    max_h = max(im.height for im in images)
    aligned = []
    for im in images:
        if im.size == (max_w, max_h):
            aligned.append(im)
            continue
        # Centre-pad onto a max_w × max_h transparent canvas.
        canvas = Image.new("RGBA", (max_w, max_h), (0, 0, 0, 0))
        dx = (max_w - im.width) // 2
        dy = max_h - im.height  # bottom-align so feet share a baseline
        canvas.paste(im, (dx, dy), im)
        aligned.append(canvas)
    bbox = tight_bbox_union(aligned)

    out_dir = out_root / motion
    out_dir.mkdir(parents=True, exist_ok=True)
    for p in out_dir.glob("frame_*.png"):
        p.unlink()
    for i, im in enumerate(aligned):
        crop = im.crop(bbox)
        crop.save(out_dir / f"frame_{i:02d}.png", optimize=True)
    return len(aligned)


def main():
    out_root = Path(__file__).parent.parent / "data" / "kenney"
    out_root.mkdir(parents=True, exist_ok=True)
    summary = []
    for motion, files in MOTIONS:
        n = emit(motion, files, out_root)
        if n > 0:
            summary.append((motion, n))
    print("Wrote sprites:")
    for m, n in summary:
        print(f"  {m:14s} {n:3d} frames")


if __name__ == "__main__":
    main()
