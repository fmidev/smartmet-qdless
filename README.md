# smartmet-qdless

Part of [SmartMet Server](https://github.com/fmidev/smartmet-server). See the [SmartMet Server documentation](https://github.com/fmidev/smartmet-server) for an overview of the ecosystem.

`qdless` is an interactive terminal viewer for gridded weather data. It draws
fields as a 24-bit colour raster made of Unicode block characters, so it
works over plain SSH in any modern terminal. You can animate forecasts, probe
time series, cut cross-sections and fly through the data in 3D.

![qdless showing 2 m temperature over Scandinavia](docs/images/qdless.png)

**📖 Full documentation: [docs/user-guide.md](docs/user-guide.md)**, which
covers every feature, view and key with screenshots and animations.

## Highlights

| Animate forecasts (`Space`) | Probe any point (click) |
| --- | --- |
| ![forecast animation](docs/images/animation.webp) | ![time-series probe](docs/images/probe.webp) |
| **Cross-sections (`x`)** | **3D curtain through the jet stream (`v`)** |
| ![cross-section](docs/images/cross_section.webp) | ![animated 3D curtain](docs/images/curtain_combo.webp) |
| **The jet stream in 3D (`3`)** | **Peeling it by wind-speed threshold (`.`)** |
| ![jet stream body orbiting](docs/images/qd_3d_jet.webp) | ![threshold sweep from 15 to 40 m/s](docs/images/qd_3d_jet_threshold.webp) |
| **Radar volumes in 3D (`3`)** | **Globe with twilight shadow (`G`, `u`)** |
| ![radar volume in 3D](docs/images/pvol_3d.webp) | ![globe with the terminator](docs/images/globe_sun.webp) |

- **Formats**: QueryData, GRIB1/2, NetCDF, ODIM HDF5 radar volumes and
  composites, GeoTIFF, animated WebP and other images, ESRI shapefiles,
  PostGIS, and SmartMet/radon ("masala") file stores. Several files or a
  directory play as one animation.
- **2D map** in the file's native projection: palettes converted from FMI
  `wms-conf`, automatic unit conversion (K → °C, Pa → hPa), coastlines,
  borders, graticule, cities, wind arrows, place search, legend, metadata,
  and multi-panel layouts of up to four parameters.
- **Vertical structure**: cross-sections by height or by level, Hovmöller
  diagrams, and a 3D curtain that swings, rotates, orbits and tilts through
  the data. 3D point clouds of radar volumes and model fields, with
  thresholds and anomaly extrema.
- **Globe** view and a **twilight shadow** showing civil, nautical and
  astronomical twilight and night.
- **Output**: PNG export, and a headless `--dump` for scripts and CI.
  Terminals with Kitty or Sixel graphics get pixel output.

## Usage

```bash
qdless data.sqd                         # interactive viewer
qdless -p Temperature,Pressure data.sqd # two panels side by side
qdless --dir radar_frames/              # animate a directory of files
qdless --catalog /masala/datasets       # browse a SmartMet/radon file store
qdless --pg "host=db dbname=gis"        # browse PostGIS tables
qdless --dump data.sqd                  # render one frame to stdout and exit
```

Press `?` in the viewer for the keys that apply to the current view, and see
the [key reference](docs/user-guide.md#23-key-reference) and
[command-line reference](docs/user-guide.md#24-command-line-reference) in the
guide. `qdless --help` lists all options.

## Build

```bash
make -j all          # build the qdless binary
make test            # run the test suite
make rpm             # build RPM
make install         # install to $PREFIX/bin (default /usr/bin)
```

### macOS (Homebrew)

The fmidev tap ships a pre-built bottle for Apple Silicon. Install with:

```bash
brew tap fmidev/smartmet
brew install fmidev/smartmet/smartmet-qdless
```

The formula pulls in all SmartMet library deps (`macgyver`, `gis`,
`newbase`, `grid-files` and its transitives) plus `gshhg-gmt-nc4` for
coastlines, so a fresh tap install is enough to run `qdless` against
QueryData, GRIB, NetCDF, ODIM HDF, GeoTIFF, shapefiles and PostGIS.

Palettes, `qdless.conf`, and `cities1000.tsv` are installed under
`$(brew --prefix)/share/smartmet/qdless/` (baked into the binary via
`-DQDLESS_DATA_DIR=...`); the coastline NetCDFs live at
`$(brew --prefix)/share/gshhg-gmt-nc4/`. `--palette-dir`,
`--coastline-dir`, and the `QDLESS_GRID_FILES_CONF` env var override the
defaults at runtime.

For source-level development on macOS — non-brew sibling builds against
checked-out SmartMet libraries — there is a `Makefile.mac` carried in the
[fmidev/homebrew-smartmet](https://github.com/fmidev/homebrew-smartmet)
tap (`patches/qdless.Makefile.mac`) together with the small
cross-platform patch the bottle applies.

## Dependencies

- SmartMet libraries: `newbase`, `macgyver`, `smarttools`, `gis`,
  `calculator`, `grid-files`
- `ncursesw` (input only — the map and popups are rendered with raw ANSI
  for full opacity control)
- `jsoncpp`, `netcdf-cxx4`, `hdf5` (ODIM HDF reader)
- GDAL (vector + raster I/O for shapefiles, PostGIS, GeoTIFF, raw
  images); the PostgreSQL driver must be enabled in the GDAL build
  for `--pg`
- `libwebpdemux`, `libwebp` (animated WebP)
- `gshhg-gmt-nc4` (runtime, for coastlines / borders / rivers)
- Boost (program_options, regex, iostreams, thread)

## License

MIT — see [LICENSE](LICENSE)

## Contributing

Bug reports and pull requests are welcome on [GitHub](../../issues).
