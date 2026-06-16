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

## Browsing the "masala" forecast archive (`--catalog`)

`--catalog` browses a SmartMet/radon file store laid out as
`<root>/<producer>/<reftime>/<geometry>/<leadtime>/<param-file>`, where each
leaf file holds one `(parameter, level, timestep)` slice and all metadata is
encoded in the path + filename (radon convention
`PARAM_LEVELTYPE_LEVEL_PROJ_NX_NY_SCAN_FFF.ext`).

```bash
./qdless --catalog /masala/datasets                              # interactive picker
./qdless --catalog /masala/datasets/131/202606130000/ECEUR0100   # open a cube directly
./qdless --catalog <cube-dir> -p T-K -l 60 --dump                # headless one-frame
```

- Pointed **above a cube**, it opens a lazy Miller-column picker (producer →
  reference time → geometry); pointed **at a cube** (a geometry dir with numeric
  leadtime subdirs, or a leaf of param files) it opens that cube. `[D]` re-opens
  the picker.
- A cube becomes a `(parameter × level × leadtime)` source: lead times drive the
  time animation, level types/values drive `L`/cross-section, valid time =
  reference time + lead. Slices are read on demand (one file per slice,
  LRU-cached) — navigation never walks or `stat`s the tree, only one `readdir`
  per node expanded.
- **3D views** (`[3]` point cloud, `[v]` curtain, height-mode 2D cross-section)
  work when the active level group is a real vertical axis — `pressure` or
  `height`/`altitude` levels. Heights come from the level *type* (ISA
  hypsometric formula for pressure, identity for height/altitude) since the
  per-slice files carry no height field; `hybrid`/`depth`/`ground` groups have
  no derivable height and so offer no 3D. The point cloud samples each level
  on a coarse lat/lon lattice via `DataSource::sampleVolume` →
  `MasalaSource::sampleColumnProfile`. Headless check: add `--3d` to a `--dump`.
- Implementation: `QdlessCatalog.{h,cpp}` (filename parser + `MasalaCube` index +
  nav helpers) and `QdlessMasalaSource.{h,cpp}` (a `DataSource` over one cube that
  delegates rendering to a per-file `GridFilesSource`). Full design and the
  rotated-ll/Lambert rendering fix it required are in
  `docs/masala-catalog-plan.md` (§7–8).
- **GRIB** renders best on `rll`/`regular_ll` (MEPS, ECMWF, GFS); `lcc` works but
  is slow. **NetCDF** (`.nc` wave/ocean — WAM, NEMO) renders via a GDAL fallback:
  grid-files is tried first, and when it can't georeference the grid
  (`geometryResolvable()` false, the "Geometry not found" case) `DataSource::open`
  hands the file to `GdalRasterSource` (which assumes WGS84 for a bare lon/lat
  geotransform). **Radar GeoTIFF nowcasts** (`.tif`, e.g. producer 110 `PPNFIN3`,
  laid out `<geometry>/<YYYYMMDD>/*.tif`) are recognised as a *raster cube*
  (`MasalaCatalog::isRasterCubeDir`) and opened as a time-animated
  `MultiFileSource` by `App::openCatalogRasterCube` — files are grouped by product
  (name after the leading timestamps), only the latest origin run is kept, and a
  multi-product dir prompts which product to open. `GdalRasterSource` reads the
  ODIM-style metadata these carry (`dataset1_data1_what_gain/offset/nodata`,
  `ForecastTimestamp`/`Timestamp`) so values come out scaled and times are correct.

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
