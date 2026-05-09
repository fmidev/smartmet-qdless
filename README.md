# smartmet-qdless

Part of [SmartMet Server](https://github.com/fmidev/smartmet-server). See the [SmartMet Server documentation](https://github.com/fmidev/smartmet-server) for an overview of the ecosystem.

![qdless rendering 2-m temperature over Scandinavia](docs/images/qdless.png)

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
- Native projection rendering for QueryData (polar stereographic,
  Lambert, rotated lat/lon, …); GRIB / NetCDF render in lat/lon
- 80+ pre-baked palettes from `wms-conf`, plus a built-in fallback ramp
- Automatic palette + unit detection: `K → °C`, `Pa → hPa`, fraction → %,
  with name-based disambiguation (sea-surface temperature ≠ atmospheric
  temperature, wind-speed ≠ ocean-current, etc.)
- Coastlines, borders and rivers from `gshhg-gmt-nc4`, with area /
  roundness filters that drop fractal noise (Saimaa) while keeping real
  lakes (Vänern). Drawn by default as a thin braille overlay (~1/4 cell
  wide) so the gridded data shows through; cycle to the older thick
  half-cell rasterisation or off with `c` / `b`
- Multi-panel layouts (`F2` cycles single → side-by-side → 2x2); each
  panel has its own parameter, level, and palette while sharing the
  viewport, time, marker, and overlay toggles. `Tab`, digit keys `1`–`4`,
  and mouse clicks switch the active panel; parameter / level / legend /
  probe / cross-section / PNG export all act on it
- Lat/lon graticule, wind arrows, legend popup, help popup
- City overlay (top-N most-populous in view, with names) sourced from
  GeoNames `cities1000`
- Place search with auto-marker and time-series probe at the picked
  location
- Click-to-probe braille-sparkline time series at any point on the map.
  Popup auto-scales with terminal size, so the chart keeps a similar
  physical size when the font shrinks
- Valid time and model-run time both shown — labelled "analysis" to
  match GRIB terminology; omitted when the file has no reference time
- Animation (play/pause + speed control)
- PNG export
- Cross-section view across pressure / height (model) levels
- Mouse drag-pan and double-click zoom

## Keyboard shortcuts

The full list is in the in-app help popup (`?`). Quick reference:

| Key | Action |
| --- | --- |
| `q` / `Esc` | Quit |
| `p` | Parameter menu |
| `L` (Shift+L) | Level menu |
| `←` / `→` | Previous / next time |
| `Home` / `End` | First / last time |
| `Space` | Play / pause animation |
| `↑` / `↓` | Animation speed up / down |
| `+` / `-` | Zoom in / out (centre) |
| dbl-click L / R | Zoom in / out at cursor |
| `0` | Reset view |
| `h` `j` `k` `l` or Shift+arrow or drag | Pan |
| click | Time-series probe at point |
| `g` | Legend popup |
| `c` | Coastlines: braille → thick → off |
| `b` | Borders: braille → thick → off |
| `n` | Toggle lat/lon graticule |
| `w` | Toggle wind arrows |
| `i` | Toggle city overlay |
| `PgDn` / `PgUp` | Cities: denser / sparser (5 → 500) |
| `/` | Place search (auto-opens probe at the pick) |
| `x` | Cross-section (click two endpoints) |
| `e` | Export PNG (active panel) |
| `F2` | Cycle panel layout: single → side-by-side → 2x2 |
| `Tab` / `Shift+Tab` | Next / previous active panel |
| `1`–`4` | Activate panel by number |
| click | Activate the panel under the cursor |
| `?` | This help |

PgUp / PgDn were chosen for the city density step because they're in the
same physical position on every common keyboard layout (US, AZERTY,
QWERTZ, Finnish) and need no modifier.

## Gallery

| | |
| --- | --- |
| ![global temperature](docs/images/global.png) <br/>Global temperature, ECMWF | ![Himalaya zoom](docs/images/zoom_himalaya.png) <br/>Zoom into the Himalaya |
| ![graticule overlay](docs/images/graticule.png) <br/>Lat/lon graticule on a stereographic grid | ![wind arrows](docs/images/wind.png) <br/>Wind arrows over a temperature field |
| ![GRIB with K→°C](docs/images/grib_celsius.png) <br/>GRIB temperature with K → °C auto-conversion and a click-to-probe sparkline | ![sea-temperature palette](docs/images/temperature_sea_own_legend.png) <br/>Sea-surface temperature using the dedicated `seatemperature` palette |
| ![parameter menu](docs/images/params.png) <br/>Parameter selection menu | ![place search](docs/images/search.png) <br/>Place search via GeoNames |
| ![legend popup](docs/images/legend.png) <br/>Legend popup | ![help popup](docs/images/help.png) <br/>Help popup |
| ![pressure-level cross-section](docs/images/pressurelevel_cross_section.png) <br/>Cross-section across pressure levels | ![model-level cross-section](docs/images/model_level_cross_section.png) <br/>Cross-section across model (hybrid) levels |
| ![timeseries probe](docs/images/timeseries.png) <br/>Click-to-probe braille time series | |

The screenshots above were taken before the braille coastline overlay
landed, so they show the older half-cell "thick" style. The current
default renders coastlines and political borders much thinner than what
you see in these images; press `c` or `b` to cycle back to the thick
style if you prefer the look.

## Build

```bash
make -j all          # build the qdless binary
make test            # run the test suite
make rpm             # build RPM
make install         # install to $PREFIX/bin (default /usr/bin)
```

## Usage

```bash
qdless data.sqd                              # interactive viewer
qdless --param Temperature data.grib2
qdless --palette seatemperature sst.nc
qdless --dump data.sqd                       # render one frame to stdout and exit

# Multi-panel: -p takes a comma-separated list. 1 -> single, 2 -> side,
# 3 or 4 -> 2x2; >4 is rejected. With three parameters the 4th panel
# clones the first.
qdless -p Temperature,Pressure data.sqd      # side-by-side
qdless -p T,RH,P,W data.sqd                  # 2x2 quad
qdless --layout side data.sqd                # explicit override
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
