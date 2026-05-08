# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`smartmet-qdless` is an interactive UTF-8 terminal viewer for SmartMet
querydata (.sqd) files. It renders gridded weather data as a quadrant-block
raster, with coastline overlay, palette-driven colour fills (using the FMI
wms-conf colour ramps), animation, mouse-driven panning / zoom,
click-to-probe time-series, place search, wind arrows, lat/lon graticule,
PNG export and cross-section views across pressure / height levels.

The project was split out of `smartmet-qdtools` so it can take a dependency
on `smartmet-library-grid-files` (for GRIB / NetCDF input) without pulling
grid-files into qdtools.

## Build commands

```bash
make -j all          # Build the qdless binary
make test            # Run test suite
make format          # clang-format all source
make clean           # Clean build artifacts
make rpm             # Build RPM package
make install         # Install to $PREFIX/bin (default /usr/bin)
```

## Build architecture

- `main/qdless.cpp` — entry point (boost::program_options arg parsing)
- `source/Qdless*.cpp` — implementation
- `include/Qdless*.h` — headers
- `palettes/*.json` — pre-baked colour palettes (run scripts/wmsconf2palette.py to regenerate from wms-conf)
- `cnf/qdless.conf` — parameter-name → palette-name mapping (JSON)
- `data/cities1000.tsv` — GeoNames cities for the place-search popup
- `scripts/wmsconf2palette.py` — manual conversion from wms-conf to palette JSON

The Makefile builds a single binary; there is no shared library output.

## Key dependencies

- SmartMet libraries: `newbase` (querydata), `macgyver`, `smarttools`,
  `gis`, `calculator`, `imagine` (PNG export only)
- ncurses (`ncursesw`) — keyboard / mouse input only; map and popups are
  rendered via raw ANSI escape sequences for full opacity control
- jsoncpp — palette and config file parsing
- netcdf-cxx4 — gshhg-gmt-nc4 binned-NetCDF coastline reader
- gshhg-gmt-nc4 (runtime) — coastline / border / river data
- Boost (program_options, regex, iostreams, thread)

## Code conventions

- `kFloatMissing` (newbase) marks missing values in querydata
- Sentinel detection: `Palette::lookup` treats `|value| > 1e10` as missing
- Out-of-palette-range values render as `transparent` Rgb (terminal default
  bg), not clamped to the nearest band — e.g. precipitation < 0.1 mm shows
  no colour
- Popups bypass ncurses for the body (raw ANSI), use ncurses only for the
  status / timeline windows and `wgetch` for input
- Coordinate convention: `NFmiArea` uses image-coords (Y=0 at top = north,
  Y=Height = south)
- Viewport (`uMin..uMax, vMin..vMax`) is a sub-rectangle of `NFmiArea` in
  `[0..1]` so it works for any SmartMet projection

## Testing notes

Most features are interactive (ncurses event loop). For non-tty contexts
use `qdless --dump <file>` which renders one frame to stdout and exits.
The header line shows the resolved palette, coast polyline counts, data
range and time, useful for sanity-checking from scripts / CI.
