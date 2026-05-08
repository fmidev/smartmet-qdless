# smartmet-qdless

Part of [SmartMet Server](https://github.com/fmidev/smartmet-server). See the [SmartMet Server documentation](https://github.com/fmidev/smartmet-server) for an overview of the ecosystem.

## Overview

`qdless` is an interactive UTF-8 terminal viewer for gridded weather data.
It renders a 2-D field as a quadrant-block raster directly in the terminal,
with coastline overlay, palette-driven colour fills (using the FMI wms-conf
colour ramps), animation, mouse-driven panning / zoom, click-to-probe time
series, place search, wind arrows, lat/lon graticule, PNG export and
cross-section views across pressure / height levels.

Despite the `qd` prefix (a long-standing SmartMet convention), `qdless`
reads three formats through a unified
[smartmet-library-grid-files](https://github.com/fmidev/smartmet-library-grid-files)
back-end:

- **QueryData** (`.sqd`) — FMI's native gridded format. See the
  [QueryData file format and usage guide](https://github.com/fmidev/smartmet-library-newbase/blob/master/docs/querydata.md)
  in `smartmet-library-newbase`, and the
  [smartmet-qdtools](https://github.com/fmidev/smartmet-qdtools)
  command-line toolkit.
- **GRIB1 / GRIB2**
- **NetCDF**

For QueryData input, `qdless` uses the native newbase path directly
(faster, projection-aware); GRIB and NetCDF go through the grid-files
adapter.

## Features

- Quadrant-block (¼-cell) raster with 24-bit truecolor or xterm-256
  fallback
- 80+ pre-baked palettes from `wms-conf`, plus a built-in fallback ramp
- Automatic palette + unit detection: `K → °C`, `Pa → hPa`, fraction → %,
  with name-based disambiguation (sea-surface temperature ≠ atmospheric
  temperature, wind-speed ≠ ocean-current, etc.)
- Coastlines, borders and rivers from `gshhg-gmt-nc4`, with area /
  roundness filters that drop fractal noise (Saimaa) while keeping real
  lakes (Vänern)
- Lat/lon graticule, wind arrows, legend popup, help popup
- Click-to-probe braille-sparkline time series at a chosen point
- Place search via GeoNames `cities1000`
- Animation (play/pause + speed control)
- PNG export
- Cross-section view across pressure / height levels
- Mouse drag-pan and double-click zoom

## Build

```bash
make -j all          # build the qdless binary
make test            # run the test suite
make rpm             # build RPM
make install         # install to $PREFIX/bin (default /usr/bin)
```

## Usage

```bash
qdless data.sqd                        # interactive viewer
qdless --param Temperature data.grib2
qdless --palette seatemperature sst.nc
qdless --dump data.sqd                 # render one frame to stdout and exit
```

In `--dump` mode the first stdout line is a header summarising the
resolved palette, value range, time and coastline counts — useful for
sanity-checking from scripts and CI.

Run `qdless --help` for the full option list.

## Dependencies

- SmartMet libraries: `newbase`, `macgyver`, `smarttools`, `gis`,
  `calculator`, `imagine`, `grid-files`
- `ncursesw` (input only — the map and popups are rendered with raw ANSI
  for full opacity control)
- `jsoncpp`, `netcdf-cxx4`
- `gshhg-gmt-nc4` (runtime, for coastlines / borders / rivers)
- Boost (program_options, regex, iostreams, thread)

## License

MIT — see [LICENSE](LICENSE)

## Contributing

Bug reports and pull requests are welcome on [GitHub](../../issues).
