#!/usr/bin/env python3
"""Convert wms-conf isoband JSON+CSS pairs into qdless palette files.

Usage:
    wmsconf2palette.py <wms-conf-isobands-dir> <output-dir>

Reads each name.json + name.css pair in the input directory and writes
one name.json into the output directory in qdless palette format:

    {
      "name": "temperature",
      "isobands": [
        {"lo": -50.0, "hi": -48.0, "rgb": [198, 26, 155]},
        {"lo": -48.0, "hi": -46.0, "rgb": [230, 62, 200]},
        ...
      ]
    }

Open-ended ranges use null for the missing bound. The 999 sentinel
sometimes used for open upper ranges in wms-conf is normalised to null.

Run manually whenever the source palettes change; commit the output
into qdless/palettes/ for shipping with the RPM.
"""

import json
import re
import sys
from pathlib import Path

OPEN_UPPER_SENTINEL = 999  # wms-conf sometimes uses this for "+inf"

CSS_COMMENT = re.compile(r"/\*.*?\*/", re.DOTALL)
CSS_RULE = re.compile(
    r"\.([A-Za-z0-9_-]+)\s*\{([^}]*)\}",
    re.IGNORECASE,
)
FILL_RGB = re.compile(
    r"fill\s*:\s*rgba?\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)(?:\s*,\s*([0-9.]+))?\s*\)",
    re.IGNORECASE,
)
FILL_HEX = re.compile(r"fill\s*:\s*#([0-9a-fA-F]{3}|[0-9a-fA-F]{6})\b")


def _hex_to_rgb(hexstr: str) -> tuple[int, int, int]:
    if len(hexstr) == 3:
        return tuple(int(c * 2, 16) for c in hexstr)  # type: ignore[return-value]
    return (int(hexstr[0:2], 16), int(hexstr[2:4], 16), int(hexstr[4:6], 16))


def parse_css(path: Path) -> dict[str, tuple[int, int, int]]:
    text = CSS_COMMENT.sub("", path.read_text())
    out: dict[str, tuple[int, int, int]] = {}
    for m in CSS_RULE.finditer(text):
        name, body = m.group(1), m.group(2)
        rgb_match = FILL_RGB.search(body)
        if rgb_match:
            alpha_str = rgb_match.group(4)
            if alpha_str is not None and float(alpha_str) == 0:
                continue  # fully transparent: treat as "no rule"
            out[name] = (int(rgb_match.group(1)), int(rgb_match.group(2)), int(rgb_match.group(3)))
            continue
        hex_match = FILL_HEX.search(body)
        if hex_match:
            out[name] = _hex_to_rgb(hex_match.group(1))
    return out


def _resolve_class(class_str: str, css_map: dict[str, tuple[int, int, int]]):
    for token in class_str.split():
        if token in css_map:
            return css_map[token]
    return None


def build_isobands(json_path: Path, css_map: dict[str, tuple[int, int, int]]):
    entries = json.loads(json_path.read_text())
    bands = []
    missing_classes = []
    for entry in entries:
        cls = entry.get("attributes", {}).get("class")
        if cls is None:
            continue
        rgb = _resolve_class(cls, css_map)
        if rgb is None:
            missing_classes.append(cls)
            continue
        lo = entry.get("lolimit")
        hi = entry.get("hilimit")
        if hi == OPEN_UPPER_SENTINEL:
            hi = None
        bands.append({"lo": lo, "hi": hi, "rgb": list(rgb)})
    bands.sort(key=lambda b: (b["lo"] if b["lo"] is not None else float("-inf")))
    return bands, missing_classes


def convert_pair(json_path: Path, css_path: Path, out_dir: Path) -> bool:
    name = json_path.stem
    css_map = parse_css(css_path)
    bands, missing = build_isobands(json_path, css_map)
    if not bands:
        print(f"  skipped {name}: no usable bands", file=sys.stderr)
        return False
    if missing:
        print(f"  {name}: {len(missing)} class(es) without CSS rule, skipped", file=sys.stderr)
    out = {"name": name, "isobands": bands}
    out_path = out_dir / f"{name}.json"
    out_path.write_text(json.dumps(out, indent=2) + "\n")
    return True


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    src = Path(argv[1])
    dst = Path(argv[2])
    if not src.is_dir():
        print(f"not a directory: {src}", file=sys.stderr)
        return 1
    dst.mkdir(parents=True, exist_ok=True)

    pairs = []
    for json_path in sorted(src.glob("*.json")):
        css_path = json_path.with_suffix(".css")
        if css_path.exists():
            pairs.append((json_path, css_path))
        else:
            print(f"  skipped {json_path.name}: no matching .css", file=sys.stderr)

    print(f"Converting {len(pairs)} palette(s) from {src} -> {dst}")
    written = 0
    for json_path, css_path in pairs:
        if convert_pair(json_path, css_path, dst):
            written += 1
    print(f"Wrote {written} palette(s).")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
