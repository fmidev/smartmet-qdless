%define BINNAME qdless
%define RPMNAME smartmet-%{BINNAME}
Summary: Interactive UTF-8 terminal viewer for SmartMet querydata
Name: %{RPMNAME}
Version: 26.5.9
Release: 15%{?dist}.fmi
License: MIT
Group: Development/Tools
URL: https://github.com/fmidev/smartmet-qdless
Source0: %{name}.tar.gz
BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-buildroot-%(%{__id_u} -n)

# https://fedoraproject.org/wiki/Changes/Broken_RPATH_will_fail_rpmbuild
%global __brp_check_rpaths %{nil}

%if 0%{?rhel} && 0%{rhel} < 9
%define smartmet_boost boost169
%else
%define smartmet_boost boost
%endif

%define smartmet_fmt_min 12.0.0
%define smartmet_fmt_max 13.0.0
%define smartmet_fmt fmt-libs >= %{smartmet_fmt_min}, fmt-libs < %{smartmet_fmt_max}
%define smartmet_fmt_devel fmt-devel >= %{smartmet_fmt_min}, fmt-devel < %{smartmet_fmt_max}

BuildRequires: %{smartmet_boost}-devel
BuildRequires: bzip2-devel
BuildRequires: %{smartmet_fmt_devel}
BuildRequires: gcc-c++
BuildRequires: gdal312-devel
BuildRequires: hdf5-devel >= 1.8.12
BuildRequires: jsoncpp-devel
BuildRequires: libjpeg-devel
BuildRequires: libpng-devel
BuildRequires: make
BuildRequires: ncurses-devel
BuildRequires: netcdf-cxx4-devel
BuildRequires: netcdf-devel >= 4.3.3.1
BuildRequires: rpm-build
BuildRequires: smartmet-library-calculator-devel >= 26.4.13
BuildRequires: smartmet-library-gis-devel >= 26.4.13
BuildRequires: smartmet-library-grid-files-devel >= 26.4.22
BuildRequires: smartmet-library-imagine-devel >= 26.4.13
BuildRequires: smartmet-library-macgyver-devel >= 26.4.13
BuildRequires: smartmet-library-newbase-devel >= 26.2.4
BuildRequires: smartmet-library-smarttools-devel >= 26.4.13
BuildRequires: smartmet-timezones
BuildRequires: zlib-devel

Requires: %{smartmet_boost}-filesystem
Requires: %{smartmet_boost}-iostreams
Requires: %{smartmet_boost}-program-options
Requires: %{smartmet_boost}-regex
Requires: %{smartmet_boost}-system
Requires: %{smartmet_boost}-thread
Requires: bzip2-libs
Requires: %{smartmet_fmt}
Requires: gdal312-libs
Requires: glibc
Requires: gshhg-gmt-nc4
Requires: hdf5 >= 1.8.12
Requires: jsoncpp
Requires: libgcc
Requires: libjpeg
Requires: libpng
Requires: libstdc++
Requires: ncurses-libs
Requires: netcdf >= 4.3.3.1
Requires: netcdf-cxx4
Requires: smartmet-library-calculator >= 26.4.13
Requires: smartmet-library-gis >= 26.4.13
Requires: smartmet-library-grid-files >= 26.4.22
Requires: smartmet-library-imagine >= 26.4.13
Requires: smartmet-library-macgyver >= 26.4.13
Requires: smartmet-library-newbase >= 26.2.4
Requires: smartmet-library-smarttools >= 26.4.13
Requires: smartmet-timezones >= 24.5.27
Requires: zlib

Provides: qdless = %{version}

%description
qdless is an interactive UTF-8 terminal viewer for SmartMet querydata
(.sqd) files. It renders gridded weather data as quadrant-block raster in
the terminal, with coastline / political-border overlay (gshhg-gmt-nc4),
palette-driven colouring (FMI wms-conf colour ramps baked at build time),
animation, mouse-driven panning and zoom, click-to-probe time-series with
a braille-sparkline graph, place search via GeoNames, lat/lon graticule,
wind arrows for u/v components, PNG export, and cross-section views
across pressure / height levels.

%prep
%setup -q -n %{RPMNAME}

%build
make %{_smp_mflags}

%install
%makeinstall

%clean

%files
%defattr(0775,root,root,0775)
%{_bindir}/qdless
%defattr(0664,root,root,0775)
%{_datadir}/smartmet/qdless/qdless.conf
%{_datadir}/smartmet/qdless/palettes/*.json
%{_datadir}/smartmet/qdless/cities1000.tsv

%changelog
* Sat May  9 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.9-15.fmi
- Time-series probe popup: in-popup time animation. Space toggles
  play/pause, Up/Down adjust the per-frame delay (same scaling and
  same shared `itsAnimationDelayMs` as the outside-popup animation,
  so speed persists on either side). Each frame the marker advances
  one step (wrapping at the end) and onTimeChange is invoked so the
  underlying map ticks in lockstep — and because the viewport-stats
  cache is keyed on (param, level, viewport), animating with stats
  visible is immediate after the first scan.
- Probe info row: rename "now" → "value" (the marker can be anywhere
  in the forecast — calling it "now" implied real-time). Numeric
  readings are coloured to match their chart series — the value /
  point reading in green, viewport mean in teal, min and max in
  grey when stats are visible — and the parameter's units are
  appended where the source provides them (GRIB/NetCDF; QueryData
  has no explicit units so it's omitted there).
- First press of `s` flashes a "Computing viewport stats…" line in
  the info row before invoking the (potentially slow) scan, so the
  popup doesn't look frozen on a multi-second computation.

* Sat May  9 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.9-14.fmi
- Fix blank rendering of curvilinear NetCDF files (e.g. NEMO ocean SST):
  grid-files' getGridLatLonCoordinatesByGridPosition / getGridPointBy-
  LatLonCoordinates fill the (lat, lon) out-params in different orders
  per backend — GRIB is (lat, lon), NetCDF is actually (lon, lat) —
  causing the new uvToLatLon override to query SST at axis-swapped
  coordinates and return only the all-neighbours-missing sentinel.
  Detect the convention at startup by comparing the (0,0) result to the
  bottom-left corner reported by getGridLatLonArea, and unswap inside
  thin wrappers so the rest of the code stays format-agnostic.
  getGridValueByLatLonCoordinate is consistent across backends and is
  left untouched.

* Sat May  9 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.9-13.fmi
- Bracket each redraw in DEC mode 2026 (synchronized output) so the
  timeline header, map, and any persistent overlay (cross-section,
  marker) commit to the screen as one composed frame. Terminals that
  don't implement 2026 ignore the private-mode set/reset, so the
  sequence is safe to emit unconditionally. Removes the brief flash
  between ncurses-managed UI elements and the raw-ANSI map.

* Sat May  9 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.9-12.fmi
- Metadata popup ('M'): wrap long values across continuation rows
  instead of truncating with an ellipsis. The parameter listing on a
  many-parameter querydata (e.g. the 10-param `pal_skandinavia_pinta`)
  now lays out across multiple rows broken at ", " boundaries; long
  filenames or projection strings wrap at any whitespace as a fallback.
  The popup width is capped at 100 columns; lines longer than that
  pick up extra rows so no data is hidden.

* Sat May  9 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.9-11.fmi
- Time-series probe: render the point-series trace in bright green
  instead of white when the viewport-stats overlay is visible. Stats
  curves stay grey (envelope) and teal (mean), and the user's chosen-
  coordinate signal now reads as the primary line at a glance.

* Sat May  9 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.9-10.fmi
- Time-series probe: press `s` inside the popup to overlay viewport
  min/mean/max curves on top of the point series. Stats are computed
  once across the visible cells (256x128 samples per time step) and
  cached at the App level keyed on (param, level, viewport), so
  re-toggling, re-probing at a new coordinate, or animating across
  time within the same view reuses the result without rescanning.
  The chart's Y axis auto-expands to fit the stats range, and the
  info line switches from point-series min/max to viewport min/mean/
  max. Cache invalidates when the viewport, parameter, or level
  changes.

* Sat May  9 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.9-9.fmi
- New `M` key opens a metadata popup listing file path, format (GRIB1/2,
  NetCDF, QueryData), grid type and dimensions, lat/lon extent, reference
  time, time/level/parameter counts, and the parameter listing with
  units. Grid-files format is detected from the file's magic bytes
  because grid-files' own `GridFile::getFileType()` reports Unknown for
  the GRIB and NetCDF inputs we read.
- `boundingBox()` now walks the grid perimeter as a single closed loop
  with a continuous longitude-unwrap state spanning all four edges, so
  the lon range comes out normalised to [-180, 180] for global grids
  that wrap the antimeridian (the per-edge reset in 26.5.9-8 mistracked
  one corner and reported e.g. `179.75..539.75` instead of
  `-180..179.75`). Only affects the metadata display; the rendering
  pipeline already bypasses bbox via the `uvToLatLon` override.

* Sat May  9 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.9-8.fmi
- Fix GRIB rendering for global lat/lon grids and parameters whose
  newbase / grid-files FMI ID namespaces disagree. `boundingBox()`
  collapsed to a 0.25° sliver on grids that wrap the antimeridian
  (corner longitudes wrap to [-180,180], so min/max-of-corners loses
  the actual span); the renderer therefore sampled a single column and
  stretched it horizontally. `paramShortName()` looked the file's id up
  in grid-files' FmiParameterDef table, which uses different numeric
  ids than newbase (id 13 = Humidity in newbase but GROWDEV-D in
  grid-files), so RH-PRCNT GRIBs were labelled "GROWDEV-D" and didn't
  match the humidity palette. Fixed by routing (u,v) through the grid's
  native (i,j) coordinates via `getGridLatLonCoordinatesByGridPosition`
  / `getGridPointByLatLonCoordinates` (mirroring how QueryDataSource
  uses NFmiArea), and by displaying the file's own newbase parameter
  name instead of the cross-namespace id lookup.

* Sat May  9 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.9-7.fmi
- New `t` key cycles the cell render style: sextants → triangles →
  squares. Sextants (2x3 sub-pixel grid, 64 glyphs from Symbols for
  Legacy Computing U+1FB00) is the new default — 1.5x vertical
  resolution and near-square sub-pixels in a typical 1:2 terminal
  cell, so colour clustering is more spatially meaningful and diagonal
  boundaries land in 64 places per cell instead of 16. Triangles is
  the previous quadrant-block rendering with 1/12-cell corner-triangle
  bevels (U+1FB57, U+1FB62, U+1FB3C, U+1FB47) substituted on 3:1 cells
  to soften staircase boundaries. Squares is the original 16-glyph
  quadrant rendering and the universal-font fallback (Block Elements
  U+2580..259F, Unicode 1.1, ships with every monospace font); both
  sextants and triangles need a font that includes the U+1FB00 block.

* Sat May  9 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.9-6.fmi
- Coastline (`c`) and border (`b`) keys cycle braille → thick → off
  instead of toggling on/off. Braille is the default; thick is the old
  half-cell quadrant rasterisation. CLI --no-coastline / --no-borders
  start in the off state.

* Sat May  9 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.9-5.fmi
- Coastlines and political borders are now drawn as a braille overlay on
  top of the rendered quadrant blocks (2x4 sub-cell resolution instead of
  2x2), giving lines that are roughly 1/2 to 1/4 of a cell wide instead
  of half a cell. Background of cells the line passes through is sampled
  from the underlying data so the line reads as a thin trail across the
  data rather than a solid colour block.

* Sat May  9 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.9-4.fmi
- Panel separators use one-eighth-block glyphs (▏ U+258F vertical, ▔
  U+2594 horizontal) instead of the heavier ─/│ box-drawing characters.
  The lines are anchored to cell edges so the Quad cross meets cleanly
  when the vertical pass overwrites the horizontal at the intersection.

* Sat May  9 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.9-3.fmi
- -p accepts a comma-separated list of parameters and picks the layout
  from the count: 1 -> single, 2 -> side-by-side, 3 or 4 -> 2x2; >4 is an
  error. With three parameters the 4th panel clones the first. New
  --layout single|side|quad option overrides the count-derived layout
  (must hold at least as many panels as parameters).

* Sat May  9 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.9-2.fmi
- Multi-panel layouts. F2 cycles single → side-by-side → 2x2; each panel
  has its own parameter, level, and palette while sharing the viewport,
  time, marker, and overlay toggles. Tab / Shift+Tab / digit keys 1–4 /
  mouse click switch the active panel; parameter / level / legend / probe
  / cross-section / PNG export operate on it. Per-panel labels at top-left
  of each panel show "[N] paramName" with the active panel highlighted.

* Sat May  9 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.9-1.fmi
- Timeseries probe: a click outside the chart on the map area now picks a
  new probe location instead of closing the popup; the popup loops at the
  new lat/lon. Keyboard still closes. Off-map clicks are ignored. Also
  handles BUTTON1_CLICKED so clicks register on terminals that deliver an
  atomic click event.

* Fri May  8 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.8-1.fmi
- Initial release. Split out from smartmet-qdtools so the project can
  depend on smartmet-library-grid-files for GRIB / NetCDF input without
  pulling that into qdtools.
