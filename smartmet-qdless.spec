%define BINNAME qdless
%define RPMNAME smartmet-%{BINNAME}
Summary: Interactive UTF-8 terminal viewer for SmartMet querydata
Name: %{RPMNAME}
Version: 26.5.27
Release: 3%{?dist}.fmi
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
BuildRequires: libwebp13-devel
BuildRequires: make
BuildRequires: ncurses-devel
BuildRequires: netcdf-cxx4-devel
BuildRequires: netcdf-devel >= 4.3.3.1
BuildRequires: rpm-build
BuildRequires: smartmet-library-calculator-devel >= 26.4.13
BuildRequires: smartmet-library-gis-devel >= 26.4.13
BuildRequires: smartmet-library-grid-files-devel >= 26.4.22
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
Requires: libwebp
Requires: libstdc++
Requires: ncurses-libs
Requires: netcdf >= 4.3.3.1
Requires: netcdf-cxx4
Requires: smartmet-library-calculator >= 26.4.13
Requires: smartmet-library-gis >= 26.4.13
Requires: smartmet-library-grid-files >= 26.4.22
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
%{_datadir}/smartmet/qdless/foot.png

%changelog
* Wed May 27 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.27-3.fmi
- New exit strategy

* Wed May 27 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.27-2.fmi
- Improved exit handling

* Wed May 27 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.27-1.fmi
- Improved exit handling

* Fri May 22 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.22-2.fmi
- Extended the [3] 3D point-cloud view to QueryData sources. Two modes,
  picked automatically:
    * Volumetric: multi-level files that carry GeomHeight or GeopHeight
      alongside the active parameter (e.g. TotalCloudCover on 65 hybrid
      levels) — every (level, grid-cell) becomes a 3D point at its real
      height.
    * Surface stack: flat files that carry Precipitation1h, FogIntensity,
      LowCloudCover, MediumCloudCover (HighCloudCover optional) — each
      parameter is rendered at a canonical height (0 / 0.1 / 1.5 / 3.5
      / 8 km) with its own palette, giving a cartoon vertical
      cross-section of the weather.
  Coastlines are projected through a flat-Earth frame anchored at the
  data bbox centre. Threshold defaults to 50% cover for the cloud
  layers; precip and fog use fixed sensible thresholds. Vertical
  exaggeration defaults to 50× because NWP domains are two orders of
  magnitude wider than they are tall. New --3d flag boots straight
  into the 3D view (works with --dump too).

* Fri May 22 2026 Andris Pavēnis <andris.pavenis@fmi.fi> 26.5.22-1.fmi
- Fix building RPM packages

* Thu May 14 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.14-3.fmi
- 3D point-cloud view for ODIM PVOL polar volumes. Press [3] inside a
  PVOL file: the volume renders as a rotatable point cloud above a
  tilted coastline-and-range-rings ground plane, with z-buffer
  occlusion and the standard reflectivity palette. h/l yaw, j/k
  pitch, +/- zoom, ,/. dBZ threshold, PgUp/PgDn vertical exaggeration
  (default 8× because storms are ~30:1 wide:tall in true geometry),
  0 reset camera, [3] toggle back to 2D. HUD bottom-right shows the
  full camera state.

* Thu May 14 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.14-2.fmi
- ODIM-H5 polar volumes (/what/object=PVOL): each elevation sweep is
  exposed as a level and the data is rendered as a PPI on an
  azimuthal-equidistant grid centred on the radar. A synthetic "MAX"
  level at the top of the level list paints the column-max composite
  for a one-glance storm overview.
- Cross-section on PVOL becomes a true RHI by default: Y-axis in km
  above the radar, each pixel routed to the sweep whose beam passes
  through it under the 4/3-Earth refraction model. Press [Y] inside
  the cross-section to flip between height (km) and elevation angle.
- Probe popup (click on map) switches to a vertical-profile chart on
  single-time / multi-level sources: dBZ vs elangle at the picked
  point, arrows step elevations, map tracks the active sweep.
- Quick level cycling without opening the popup: `<`/`>` (or `,`/`.`)
  step the level down/up. Top status bar shows the active level
  whenever the source has more than one.
- Bottom status bar hides the [W]ind toggle for sources that don't
  carry WindUMS+WindVMS (e.g. radar files).
- Cross-section endpoint clicks now route through BUTTON1_RELEASED in
  addition to BUTTON1_CLICKED so terminals that don't synthesise a
  CLICKED event no longer fall through to the time-series probe.

* Thu May 14 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.14-1.fmi
- PNG-tree browser: --dir on a directory with subdirs (and no image
  files directly at the root) opens a leaf picker at startup instead of
  requiring the full leaf path on the command line. Searchable flat
  list by default; Tab toggles a column navigator. [D] reopens the
  picker; status bar shows [D]Browse in tree mode.
- Cross-section: hovering the mouse over the chart now highlights the
  corresponding (lat, lon) on the map with a yellow dot. The
  great-circle line and its two endpoints are drawn on the map while
  the cross-section is active, and the popup docks to the half of the
  screen opposite the line's midpoint so the line and dot stay visible.

* Mon May 11 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.11-3.fmi
- GSHHS resolution now scales with screen resolution, not just data
  resolution. Coastline::pickFile takes degrees-per-pixel and selects
  the coarsest GSHHS whose vertex spacing still resolves a distinct
  pixel; loadCoastlines feeds it the Braille sub-pixel grid
  (cellW*2 × cellH*4, the finest path) and the finer viewport axis so
  the polyline reads smooth in both directions. Fixes coastlines
  rendering polygonally on wide terminals at moderate spans, e.g. a
  ~13° Nordic view on a 280-sub-pixel-wide terminal now picks `i`
  (1 km) instead of `l` (5 km).

* Mon May 11 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.11-2.fmi
- `n` now cycles the graticule the same way `c` / `b` cycle coastlines
  and borders: Braille → Thick → Off. Braille is the new default — the
  meridian/parallel lines render as a thin dotted overlay on top of the
  rendered cells rather than half-cell-wide block characters punched
  into the data buffer. Lat/lon line tracing (round-trip filter,
  antimeridian guard, niceStep cadence) is shared between the two
  modes via App::traceGraticuleSegments. Status bar reflects the
  current mode on each press.

* Mon May 11 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.11-1.fmi
- Fix: graticule overlay no longer draws spurious lines hugging the
  viewport edges on projected sources (e.g. rotated lat/lon). Such
  projections cover a geographic bbox larger than the actual data
  region, and meridian/parallel samples near the bbox edges fall
  outside the grid; NFmiArea::LatLonToWorldXY then silently falls
  back to bbox interpolation, returning a (u, v) that doesn't
  correspond to the projection of the sampled lat/lon. The graticule
  drawer now round-trips each sample through uvToLatLon and skips
  any point whose round-trip diverges by more than 0.5 degrees, and
  refuses to draw segments whose endpoint jump exceeds half the
  viewport (catches antimeridian wraps the round-trip can't see).

* Sun May 10 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.10-23.fmi
- Drop the smartmet-library-imagine dependency. PNG export now
  goes through GDAL's PNG driver (CreateCopy from an in-memory
  MEM raster of three Byte bands). GDAL was already linked for
  shapefiles, PostGIS, GeoTIFF, raw images, and animated WebP, so
  the new path adds no transitive dependencies — just removes
  one. The PNG bytes-on-disk format and overlay rendering are
  unchanged from the user's side; only the encoder changes.
- README + RPM spec no longer list smartmet-library-imagine in
  the build- or runtime-requires.

* Sun May 10 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.10-22.fmi
- Zoom-aware re-rasterisation for shapefiles / PostGIS layers.
  ShapeSource now grows a `prepareViewport(bbox, cellsX, cellsY)`
  hook (added to the DataSource interface as a no-op default);
  the App calls it in sampleSlice with the visible lat/lon
  bbox plus the screen's cell count. When the user zooms in past
  the base raster's pixel size, ShapeSource builds a
  high-resolution refined raster covering the visible bbox plus
  a 25% margin so panning within the region doesn't trigger
  constant rebuilds. interpolatedValue checks the zoom raster
  first and falls back to the base for cells outside the
  refined bbox or at zoomed-out scales.
- ShapeSource no longer caches OGR geometries past construction.
  rasterise() re-walks the layer (PostGIS connection stays open;
  shapefile re-opens its file) on each call. Trade-off: more I/O
  per zoom step, no per-source memory tied up in geometry copies
  — important for huge PostGIS tables where the cache would have
  been untenable. Burn-id assignment is deterministic across
  walks (sequential by leaf-polygon iteration order) so the
  base and zoom rasters stay consistent and click-to-attribute
  keeps mapping the right feature.

* Sun May 10 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.10-21.fmi
- DataSource gains a SourceCategory enum (Gridded / Image /
  Vector) and a `category()` virtual; ImageSource and ShapeSource
  override; MultiFileSource forwards to its reference. Convenience
  predicates isImage() / isVector() / isGridded() let App branches
  read naturally. Replaces the scatter of `isRawImage()` and
  `dynamic_cast<ShapeSource*>()` checks across the constructor,
  loadPalette, status bar, help popup, and key handlers — every
  branch now switches on one self-reported tag.
- ImageSource: bilinear sampling in pixelAtUV. Sampling at the
  pixel-centre convention with weights that exclude transparent
  neighbours (so anti-aliased PNG edges don't leak colour past
  their alpha). Visible improvement when zooming in heavily on a
  raw image (no more blocky aliasing) AND when zooming out (no
  moiré from one screen cell sampling many source pixels at
  random offsets).

* Sun May 10 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.10-20.fmi
- Help popup ('?') is now context-aware. Pass a HelpContext flag
  set down from App; popupHelp builds the entry list dynamically
  and drops sections that don't apply (no time-series probe stuff
  on shapes; no panel-layout / Tab / 1-4 / "click activates panel"
  on shape or image mode; no wind / cross-section / coastline
  toggles where there's no projection; no Param/Level menus when
  the source has only one). Consecutive blank separators collapse
  so a heavily filtered context doesn't leave stacks of empty
  rows.
- Status bar in shapefile mode adds [D]Tables when the source was
  opened over PostGIS (--pg) so the layer-picker re-open
  shortcut is discoverable.
- Status bar drops [Space]Play in shape mode (no time animation).
- Time-axis bar is suppressed when timeCount() == 1. The label
  on the timeline still tells the user what they're looking at;
  a stuck slider would otherwise suggest a time axis exists when
  it doesn't.
- PostGIS layer picker filters out non-spatial tables (geometry
  type wkbNone). OGR's PG driver lists every table visible to
  the role, including pure attribute tables and metadata views;
  those have no geometry to render and shouldn't appear in the
  picker as a candidate layer.

* Sun May 10 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.10-19.fmi
- Fix: PostGIS layer picker (and any popup opened before the App
  draws the map for the first time) now appears immediately. UI
  ctor commits a clean blank screen to ncurses + the terminal at
  the end of init via clear() / wnoutrefresh(stdscr) / doupdate().
  Without this, the first popup ran before any ncurses doupdate()
  had happened, and the implicit refresh inside the popup's wgetch
  then committed ncurses' default empty state on top of the
  popup's raw-ANSI render — so the popup looked like it didn't
  appear until the user pressed a key, which prompted the popup
  loop to re-render after the refresh.

* Sun May 10 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.10-18.fmi
- popupSearch (attribute table, layer picker, place search) now
  scrolls when the selection moves past the visible window. Added
  PgUp / PgDn / Home / End handlers; the digit hotkeys 1..9 pick
  the visible row (relative to scroll), not the absolute index.
- popupLegend gains scrolling for long legends. ↑/↓ step a row,
  PgUp/PgDn step a screen, Home/End jump to the ends. Footer
  reports the visible range (e.g. "1-25 of 192"). Labelled
  rainbow legends (shapefiles) are sorted alphabetically;
  numeric (lo..hi) bands keep their intrinsic order.
- popupMetadata returns click coordinates when the user clicks
  outside the popup. App's vector-source click handler chains:
  the new click drops the marker on the next polygon and pops
  up its attributes, so users can hop between features by
  clicking around. Clicks inside the popup, or any keypress,
  still dismiss it as before.
- All popups force blocking input mode on entry (the App's
  animation loop leaves wgetch in non-blocking 250ms mode, which
  caused the popup body to redraw 4×/s and reportedly looked
  like the popup didn't appear until the user pressed something).

* Sun May 10 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.10-17.fmi
- README updated to cover the new input formats (ODIM HDF,
  GeoTIFF, raw images, animated WebP, shapefiles, PostGIS), the
  multi-file animation flow (--dir / positional list),
  shape-mode keys [A] / [O] / [R] and the PostGIS picker [D],
  the click-to-attributes behaviour for vector sources, and the
  expanded dependency list (GDAL with PostgreSQL driver,
  libwebpdemux, libhdf5).

* Sun May 10 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.10-16.fmi
- PostGIS browser: launch with `qdless --pg "<dsn>"` to open a
  PostgreSQL connection through OGR's PostgreSQL driver and pick
  a layer from a popup. `--schema <name>` filters the picker to
  one schema; `--table schema.name` skips the picker and opens
  that table directly. Auto-picks when only one layer matches.
  The connection stays open for the App's lifetime and [D]
  re-opens the picker without paying a libpq round-trip.
- ShapeSource gains a (OGRLayer*, displayName, Options) ctor; the
  filename ctor now delegates to a private init() helper that
  both paths share, so PostGIS layers go through the exact same
  rasterise / outline / attribute / palette pipeline as
  shapefiles do.
- App: post-itsSource init extracted into initFromSource() so the
  deferred PostGIS pick path (--pg without --table) can run it
  after the user picks a layer at startup.

* Sun May 10 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.10-15.fmi
- Shape rainbow palette now assigns one hue per *unique label*
  rather than per burn id. A feature that fans out into many
  sub-polygons (a MultiPolygon, or just several .dbf rows that
  share a NAME) was getting one hue per polygon, so e.g.
  "Öresund ja Bälten" appeared 7 times in the legend with 7
  different colours. Bands sharing a label now share the rgb
  drawn from a single golden-angle hue cycle, and popupLegend
  dedupes legend rows by label so each unique area is listed
  once.

* Sun May 10 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.10-14.fmi
- Attributes table column padding now counts UTF-8 codepoints
  rather than bytes. Finnish placenames (Ähtäri, Selkämeri) have
  2-byte chars whose .size() is double their visible width, so
  any row containing them threw the column edges off and the
  table looked unaligned. Header column widths are derived the
  same way so the bold header lines up under the values.
- popupSearch query row now reads "Search:" instead of just ">",
  so a stray keystroke that filters everything is obviously the
  search field doing its job rather than the popup glitching.

* Sun May 10 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.10-13.fmi
- Shape legend ([G]) now lists one swatch per visible polygon
  instead of one per registered burn id; sub-pixel children of a
  MultiPolygon (which never paint a cell) are filtered out.
- Each swatch is labelled by the feature's NAME / NIMI / first
  text field (case-insensitive, falls back to "#N"). The numeric
  range "14.5 .. 15.5" that the generic numeric formatter
  produced for integer feature IDs is replaced by that label.
- Palette::Band gains an optional `label` field; popupLegend uses
  it when set, falling back to the formatted lo..hi range for
  ordinary numeric palettes.
- Attributes table ([A]) now opens with a column-aligned layout
  and a bold header row showing the .dbf field names. Per-column
  widths are derived from header + value lengths, capped at 24
  chars so a runaway free-text field doesn't push the layout
  off-screen.
- popupSearch grows an optional `header` parameter for the column
  titles and shows "(no matches for "X")" in the body when the
  current query filters everything away — previously the body
  just blanked, which looked like the popup itself disappeared.

* Sun May 10 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.10-12.fmi
- Fix: rainbow palette sized by featureCount() instead of
  burnIdCount(), so a shapefile whose features fan out into more
  burn ids than features (any MultiPolygon) painted the
  higher-id sub-polygons transparent — they showed as terminal
  background, looking white on a default-light terminal. Flat
  fill was unaffected because every non-zero burn id maps to the
  same band there. Now sized correctly so every sub-polygon
  picks up its hue.
- New [A] keyboard shortcut: shapefile attributes table.
  popupSearch already gives scrollable + type-to-filter; build
  one row per feature as "#NN  field1=value1 | field2=value2 …"
  (case-insensitive substring filter against the whole row),
  drop the click marker on the picked feature's centroid, and
  pop up popupMetadata with that feature's full .dbf row. Listed
  in the help popup and the shape-mode status bar.

* Sun May 10 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.10-11.fmi
- Animated WebP support: ImageSource now tries libwebpdemux first
  on .webp inputs and, when the file carries more than one frame,
  decodes every frame eagerly into a per-frame Rgb buffer. Frame
  times are mtime + cumulative frame_duration_ms so cursor keys
  step naturally through the animation and Space plays it the
  same way it does for a multi-file batch. Single-frame WebPs
  keep going through GDAL (which qdless was already using and
  has support for any WebP feature we might want later, like ICC
  profile metadata).
- NFmiMetTime sub-minute precision fix: the seconds-bearing
  constructor goes through NearestMetTime, which hardcodes
  SetSec(0) at the end. Construct with sec=0 and re-apply the
  actual seconds after — otherwise an animation at 2-second
  frame intervals would collapse every frame to HH:MM:00. Same
  fix applied to ImageSource's filename-time parser, mtime
  parser, and the GeoTIFF filename-time parser, all of which
  previously lost seconds silently.

* Sun May 10 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.10-10.fmi
- Status bar in shapefile mode now lists [O]utlines and [R]ainbow
  next to the existing overlay toggles, and drops [P]aram /
  [L]evel / [X]Section since none of those have a meaningful
  scalar interpretation when the underlying value is a feature id.

* Sun May 10 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.10-9.fmi
- Shapefile burn ids now per leaf polygon. A feature that is a
  MultiPolygon (or GeometryCollection) used to burn all its
  sub-polygons under one id, so the rainbow palette gave one hue
  to a whole MultiPolygon feature. Walk the tree to leaves and
  give each polygon / line / point its own burn value, while
  keeping the .dbf attribute row keyed to the original feature so
  the click-popup still shows the correct attributes.
- Click → attribute popup (replaces the time-series probe for
  shapefile sources). RGB triplets / feature ids have no scalar
  interpretation, so the time-series chart is meaningless. The
  click now finds the polygon under the cursor and shows its full
  .dbf row in the existing metadata-popup widget.
- Shape outlines moved out of the GSHHS political-borders slot
  into their own [O] toggle. [B] cycles GSHHS borders again as it
  used to; [O] cycles shapefile outlines (Braille → Thick → None).
  The two layers can be styled and toggled independently.
- Default palette mode for shapefiles is now rainbow per burn id
  (was flat). [R] still toggles flat / rainbow.
- Metadata popup ('M') for a shapefile now shows the burn-id
  count separately from the feature count, so users can spot
  files where one feature is a MultiPolygon of N sub-polygons
  (or one big polygon dominates the rasterised area) without
  digging through the source.

* Sun May 10 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.10-8.fmi
- Shapefile outlines drawn in green (RGB 0,220,0) instead of the
  GSHHS-borders grey-90. Grey on the default mid-grey flat fill
  was nearly invisible (only 30 levels of contrast). Green is
  also distinct from coastlines (black) and political borders
  (grey-90), so the viewer can tell at a glance which lines come
  from which layer.
- New [R] keyboard shortcut: cycle the palette. For shapefiles
  this toggles between flat fill (one solid colour for every
  feature) and rainbow per feature ID (golden-angle hue rotation
  so adjacent feature IDs look maximally different) — useful for
  validating that polygon partitioning is correct independent of
  the outline visibility. The shortcut is a no-op with a status
  hint for non-shapefile sources; the help popup ('?') lists it.

* Sun May 10 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.10-7.fmi
- Shapefile support: new ShapeSource backend reads ESRI shapefiles
  through OGR, reprojects features to WGS84, and rasterises them
  once at construction (default 2000-pixel max dimension; uint16
  raster of feature IDs). The renderer's existing palette + overlay
  paths handle the rest:
    * interpolatedValue(lat, lon) returns the feature id under that
      cell, or 0 outside any polygon.
    * Default palette is a flat fill (mid grey above 0.5,
      transparent below) so the shape reads as a single solid
      colour without bias toward any feature. Palette::rainbowCycle
      is also available for distinguishing adjacent features (one
      hue per id, golden-angle rotation).
    * Polygon and polyline boundaries are extracted as Polylines
      and slotted into the GSHHS-borders overlay path, so the [B]
      Braille → Thick → None cycling toggles their style the same
      way it does for political borders.
  GSHHS coastline ('c'), graticule ('n'), cities ('i'), place
  search ('/'), and metadata popup ('M') keep working because the
  shapefile carries a real .prj (no image-mode gating). Time-series
  probe is still meaningful (feature id under cursor) so it is left
  enabled.
- Detection by ESRI shapefile magic (file code 9994 big-endian at
  offset 0). Routed automatically through DataSource::open.

* Sun May 10 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.10-6.fmi
- Image mode: clicking the map no longer opens the time-series
  probe. The chart plots a scalar value over time, and RGB triplets
  have no scalar interpretation — the popup was just an empty NaN
  graph. The keyboard / status-bar entries to the probe were
  already gated; this catches the mouse path too.

* Sun May 10 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.10-5.fmi
- Fix: multi-file image-mode rendered blank because MultiFileSource
  did not forward isRawImage() / pixelAtUV() to its sub-sources.
  The App's renderer therefore took the value/palette path on a
  source that returns NaN from interpolatedValue(), painting every
  cell as missing. Now the aggregator delegates isRawImage() to
  the reference and pixelAtUV() to the currently-selected file so
  the App's image-mode short-circuit fires and animates the batch.

* Sun May 10 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.10-4.fmi
- Filename time: match a 12-digit timestamp anywhere in the
  basename (was anchored to the start), so producers that put the
  date after a prefix (`HAV_202605081430_RR.png`) work the same as
  those with a leading timestamp.
- Filename time: construct NFmiMetTime with a 1-minute time step.
  The 60-minute default snapped sub-hourly stamps to the nearest
  hour, so a 15-minute radar batch (14:30, 14:45, 15:00, 15:15)
  collapsed into duplicate 15:00 slots in MultiFileSource.
- Image-mode UI: hide the parameter / level / legend / overlay /
  search / cross-section status-bar items when the source is a
  naked image, and intercept the corresponding key shortcuts so
  pressing them shows "Not available in image mode" instead of
  popping up an empty menu or silently toggling a flag with no
  visible effect.

* Sun May 10 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.10-3.fmi
- Fix: paletted PNGs (1 band of indices + a separate ColorTable;
  GCI_PaletteIndex) rendered as grayscale because the index byte
  was replicated to R/G/B. ImageSource now detects the palette
  and looks up each pixel's RGB(A) from the table; the alpha
  channel folds into Rgb::transparent below 128 the same way the
  RGBA branch does. Verified on FMI marine radar PNGs which use
  a 41-entry indexed palette.

* Sun May 10 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.10-2.fmi
- Raw image mode for PNG/WebP/JPEG/GIF/BMP. New ImageSource reads
  pixels straight through GDAL and exposes them via a new virtual
  pixelAtUV() on DataSource; the renderer short-circuits palette
  lookup when isRawImage() is true. App suppresses every geographic
  overlay (coastline, borders, graticule, cities, wind arrows) since
  naked images have no projection. Pan and zoom work for free
  through the existing viewport machinery.
- Animation across naked images: a leading YYYYMMDDhhmm in the
  basename feeds MultiFileSource the same way the GeoTIFF backend
  does, so `qdless --dir <path>` plays a directory of PNGs as a
  time-sorted series.
- Fix: OdimSource gridSignature now includes the (UL,LR) corner
  coords. Two ODIM files often share one projdef (a network-wide
  projection anchored on a producer's reference radar) but crop a
  different bbox; the previous projdef+dimensions signature would
  falsely merge them and the renderer would draw the wrong area
  for any time step but the reference. Verified against the
  qdtools test corpus: 13 of 18 files now correctly rejected when
  passed via --dir.

* Sun May 10 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.10-1.fmi
- ODIM HDF5 support: new OdimSource backend for 2D composites
  (object=IMAGE/COMP/CVOL). Reuses qdtools' Hdf5File reader and
  the h5toqd quantity → newbase parameter mapping. Applies
  gain*raw + offset on read, and maps both `nodata` and `undetect`
  to NaN so out-of-palette cells render transparent — radar dBZ
  can be legitimately negative, so painting clear-air as zero
  would mislead. Polar volumes (PVOL) are rejected up front.
- GeoTIFF support: new GeoTiffSource via GDAL directly. grid-files'
  built-in GeoTIFF reader only handles FMI's private GeometryId
  tag and does not parse the standard georef tags (33550, 33922,
  34735), so third-party GeoTIFFs went through it as Unknown
  projection. The new path reads ModelTiepoint + ModelPixelScale
  + WKT through GDAL and builds an NFmiArea from CreateFromBBox.
  When the file carries an FMI-style GDAL_METADATA TIFF tag
  (42112) — radar GeoTIFFs do — `Observation time`, `Quantity`,
  `Gain`, `Offset`, `Nodata`, `Undetect` are read from there in
  preference to filename parsing. Falls back to a leading
  YYYYMMDDhhmm in the basename, then to mtime.
- Multi-file mode: new MultiFileSource aggregator. Pass several
  files (`qdless f1 f2 …`) or `--dir <path>` to walk a directory.
  The most-recently-modified file is the canonical projection; any
  file whose grid signature does not match is skipped with a
  stderr warning naming both paths. Each backend exposes a
  `gridSignature()` (projection + dimensions + extent) for the
  comparison; default is bbox-based.
- Detection: HDF5 magic (0x89 H D F) now disambiguates ODIM vs
  NetCDF4 via a cheap probe of /what/object presence; TIFF magic
  (II*\\0 / MM\\0*) routes to the new GeoTIFF path. PROJ stderr
  noise from EPSG:3067 out-of-domain perimeter sampling is
  silenced via CPLPushErrorHandler around the affected calls.

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
