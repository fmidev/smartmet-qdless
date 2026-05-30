#!/usr/bin/env python3
"""Convert Muybridge animated GIFs (downloaded from Wikimedia Commons) into
per-frame RGBA silhouette PNGs suitable for qdless's ImageSource loader.

Inputs:  full-colour or grayscale GIFs, dark or light background.
Outputs: data/muybridge/<motion>/frame_NN.png — each a tight crop of the
         silhouette in pure black on transparent. The terminal effect tints
         them at render time, so storing a single neutral colour keeps
         disk/repo size down and matches the rotoscope aesthetic.

Background detection uses the corner pixel as the canonical background and
thresholds the inverse-distance grayscale at a Otsu-derived level. Each
plate has its own row/column of subject pixels; the script auto-finds the
tightest box around the figure across the whole sequence so each frame is
positioned in a common (motion-relative) coordinate frame.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

from PIL import Image, ImageOps, ImageFilter
from collections import deque

SRC = {
    # Humans
    "man_walk":     "/tmp/mb_Muybridge_human_male_walking_animated.gif",
    "stairs":       "/tmp/mb_Muybridge_ascending_stairs_animated.gif",
    "woman_walk":   "/tmp/mb_Eadweard_Muybridge_1.gif",
    "disc_throw":   "/tmp/mb_Eadweard_Muybridge_2.gif",
    "leapfrog":     "/tmp/mb_Eadweard_Muybridge_Boys_playing_Leapfrog_(1883–86,_printed_1887)_animated.gif",
    "dance":        "/tmp/mb_Eadweard_Muybridge,_Animal_Locomotion,_Woman_Dancing_(fancy),_1887,_plate_187.gif",
    "somersault":   "/tmp/mb_Somersault_Muybridge.gif",
    "hammering":    "/tmp/mb_Nude_blacksmiths_hammering,_animated_from_Animal_locomotion,_Vol._II,_Plate_377_by_Eadweard_Muybridge.gif",
    "wrestling":    "/tmp/mb_Animal_locomotion._Plate_294_(Boston_Public_Library)_a.gif",
    "waltz":        "/tmp/mb_Man_and_woman_dancing_a_waltz_(1887).gif",
    # Animals — included as a nod to Sallie Gardner (the very first
    # rotoscoping subject) and to give the gallery breadth.
    "horse_gallop": "/tmp/mb_Muybridge_horse_gallop_animated_2.gif",
    "race_horse":   "/tmp/mb_Muybridge_race_horse_animated.gif",
    "horse_jump":   "/tmp/mb_Muybridge_horse_jumping_animated.gif",
    "horse_walk":   "/tmp/mb_Muybridge_horse_walking_animated.gif",
    "elephant":     "/tmp/mb_Elephant_walking.gif",
    "buffalo":      "/tmp/mb_Muybridge_Buffalo_galloping.gif",
}

# Target sprite height in pixels — chosen so a sprite spanning 12-20 cells
# of terminal output still has perceivable joint detail when nearest-sampled
# through ImageSource::pixelAtUV. Aspect is preserved.
TARGET_H = 64


def otsu(hist: list[int]) -> int:
    total = sum(hist)
    if total == 0:
        return 128
    sum_all = sum(i * h for i, h in enumerate(hist))
    w0 = 0
    sum0 = 0
    best_var = -1.0
    best_t = 128
    for t in range(256):
        w0 += hist[t]
        if w0 == 0:
            continue
        w1 = total - w0
        if w1 == 0:
            break
        sum0 += t * hist[t]
        m0 = sum0 / w0
        m1 = (sum_all - sum0) / w1
        var = w0 * w1 * (m0 - m1) ** 2
        if var > best_var:
            best_var = var
            best_t = t
    return best_t


def frames(gif_path: str) -> list[Image.Image]:
    out = []
    im = Image.open(gif_path)
    n = getattr(im, "n_frames", 1)
    for i in range(n):
        im.seek(i)
        out.append(im.convert("RGB"))
    return out


def to_silhouette(frame: Image.Image) -> Image.Image:
    """Return a grayscale 'L' image where 255 = subject, 0 = background."""
    gray = frame.convert("L")
    # Sample the background from the four corners — Muybridge's plates have
    # a uniform backdrop in either direction.
    px = gray.load()
    W, H = gray.size
    corners = [px[0, 0], px[W - 1, 0], px[0, H - 1], px[W - 1, H - 1]]
    bg = sorted(corners)[len(corners) // 2]  # median of corners
    # If bg is light, the subject is dark; invert so subject is always bright.
    if bg > 128:
        gray = ImageOps.invert(gray)
        bg = 255 - bg
    # Otsu threshold against the histogram, then sanity-clamp: don't let the
    # threshold dip below bg + a margin or above 220.
    hist = gray.histogram()
    t = otsu(hist)
    t = max(t, bg + 18)
    t = min(t, 220)
    mask = gray.point(lambda v: 255 if v >= t else 0, mode="L")
    # Small morphological cleanup: dilate-then-erode (close) to fill the
    # joint gaps left by halftone print, then a 1-px median to smooth edges.
    mask = mask.filter(ImageFilter.MaxFilter(3))
    mask = mask.filter(ImageFilter.MinFilter(3))
    mask = mask.filter(ImageFilter.MedianFilter(3))
    # Drop small connected components — Muybridge's plates have a metric
    # grid (a horizontal floor line plus ruler ticks). Those are a thin
    # 1-2-px horizontal stripe with a small bbox area. Anything below 1%
    # of the frame area is junk; figures are several percent.
    W, H = mask.size
    px = mask.load()
    visited = [[False] * H for _ in range(W)]
    min_area = max(40, int(W * H * 0.005))
    keep = Image.new("L", (W, H), 0)
    kp = keep.load()
    for sy in range(H):
        for sx in range(W):
            if px[sx, sy] == 0 or visited[sx][sy]:
                continue
            # BFS the component.
            comp = []
            q = deque([(sx, sy)])
            visited[sx][sy] = True
            while q:
                x, y = q.popleft()
                comp.append((x, y))
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    nx, ny = x + dx, y + dy
                    if 0 <= nx < W and 0 <= ny < H and not visited[nx][ny] and px[nx, ny] != 0:
                        visited[nx][ny] = True
                        q.append((nx, ny))
            if len(comp) < min_area:
                continue
            # Also drop very flat components — height-to-area ratio
            # detects horizontal grid stripes (wide but only 1-3 px tall).
            ys = [y for _, y in comp]
            xs = [x for x, _ in comp]
            ch = max(ys) - min(ys) + 1
            cw = max(xs) - min(xs) + 1
            if ch <= 4 and cw > 0.25 * W:
                continue
            for x, y in comp:
                kp[x, y] = 255
    return keep


def tight_bbox(masks: list[Image.Image]) -> tuple[int, int, int, int]:
    """Bounding box that contains the subject across every frame, padded."""
    boxes = [m.getbbox() for m in masks]
    boxes = [b for b in boxes if b is not None]
    if not boxes:
        raise RuntimeError("no subject pixels found")
    x0 = min(b[0] for b in boxes)
    y0 = min(b[1] for b in boxes)
    x1 = max(b[2] for b in boxes)
    y1 = max(b[3] for b in boxes)
    # Pad 2 px so the silhouette doesn't kiss the sprite edge.
    W, H = masks[0].size
    pad = 2
    return (max(0, x0 - pad), max(0, y0 - pad), min(W, x1 + pad), min(H, y1 + pad))


def emit(motion: str, src_path: str, out_root: Path) -> int:
    fs = frames(src_path)
    masks = [to_silhouette(f) for f in fs]
    bbox = tight_bbox(masks)
    bw, bh = bbox[2] - bbox[0], bbox[3] - bbox[1]
    # Scale so the *bounding box height* lands at TARGET_H.
    scale = TARGET_H / bh
    out_w = max(1, int(round(bw * scale)))
    out_h = TARGET_H

    out_dir = out_root / motion
    out_dir.mkdir(parents=True, exist_ok=True)
    # Clean any stale frames from a previous run.
    for p in out_dir.glob("frame_*.png"):
        p.unlink()

    for i, mask in enumerate(masks):
        crop = mask.crop(bbox).resize((out_w, out_h), Image.LANCZOS)
        # Build RGBA: solid black where mask is bright, transparent elsewhere.
        # Use the mask itself as alpha so the resampled antialiased edge
        # softens naturally — this composites smoothly over weather data.
        rgba = Image.new("RGBA", (out_w, out_h), (0, 0, 0, 0))
        rgba.putalpha(crop)
        rgba.save(out_dir / f"frame_{i:02d}.png", optimize=True)
    return len(masks)


def main():
    out_root = Path(__file__).parent.parent / "data" / "muybridge"
    out_root.mkdir(parents=True, exist_ok=True)
    summary = []
    for motion, src in SRC.items():
        if not os.path.exists(src):
            print(f"  skip {motion}: missing {src}", file=sys.stderr)
            continue
        n = emit(motion, src, out_root)
        summary.append((motion, n))
    print("Wrote sprites:")
    for m, n in summary:
        print(f"  {m:14s} {n:3d} frames")


if __name__ == "__main__":
    main()
