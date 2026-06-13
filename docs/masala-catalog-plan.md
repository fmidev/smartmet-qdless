# Browsing the `/masala` data store with qdless

This document analyses the on-disk layout of the `/masala` forecast archive
(captured in the `masala` directory listing — 1.3 M lines, ~14 600 directories,
~1.2 M data files) and proposes how `qdless` can browse, animate and render it
in 3D. It deliberately **bypasses** the Redis content-server route discussed in
`redis-catalog-plan.md`: everything here is driven from the filesystem alone,
by parsing directory and file names — no data files are opened during indexing.

---

## 1. What is on disk

### 1.1 Top level

```
/masala/
  data/forecast/{processed,raw}/...     # legacy ERA5 fractiles, npy cloud archives — ignore
  datasets/<producer-id>/...            # THE archive we want to browse
```

`datasets/` holds ~50 numeric **producer ids** (`10`, `100`, `131`, `4`, `52` …)
plus a handful of named ones (`aila`, `bris`, `vire`, `virenwc`,
`meps-biascorrection`, `mnwc-biascorrection`, …). The plain number is the FMI
**radon producer id**; its human name lives in an external table (see §5).

### 1.2 The canonical "exploded" layout (the bulk of the data)

```
datasets/<producer>/<reftime>/<geometry>/<leadtime>/<param-file>
        131         /202606121200/ECEUR0100 /3        /T-K_hybrid_100_rll_661_576_0_000.grib2
```

| Path segment | Meaning | Example |
|---|---|---|
| `<producer>` | radon producer id (model) | `131` |
| `<reftime>` | origin/analysis time `YYYYMMDDHHMM` | `202606121200` |
| `<geometry>` | named grid = model × area × resolution | `ECEUR0100` |
| `<leadtime>` | **forecast length in hours** (1/3/6 h steps) | `0,1,2,…,144,150,…,240` |
| `<param-file>` | one parameter × one level × one timestep | see below |

The filename grammar (the SmartMet radon file-store convention) is:

```
<PARAM-UNIT>_<leveltype>_<level>_<proj>_<nx>_<ny>_<scanmode>_<fff>.<ext>
T-K        _hybrid    _100   _rll  _661 _576 _0        _000  .grib2
SAL-PSU    _depth     _12    _ll   _...                              .nc
VV-MS      _general   _0-6   _ll   _...                              .grib2
```

* **PARAM-UNIT** — FMI parameter name + unit suffix: `T-K` (temperature/K),
  `U-MS`/`V-MS` (wind m/s), `Q-KGKG` (spec. humidity), `P-PA`/`P-HPA`
  (pressure), `RH-PRCNT`, `Z-M2S2` (geopotential), `SAL-PSU` (salinity),
  `WVELU/V/W-MS` (ocean currents), `N-0TO1` (cloud cover), etc.
* **leveltype** — `hybrid` (444 k files, ~137 model levels), `height` (314 k),
  `pressure` (109 k), `depth` (67 k, ocean), `ground`/`meansea`/`top`/`entatm`
  (single-level surface fields), `general` (172 k — layer **ranges** like
  `0-6`, `72-73`: accumulations / flight-level slabs).
* **proj** — `ll` (latlon, 686 k), `rll` (rotated latlon, 274 k),
  `lcc` (Lambert conformal, 179 k), `tm` (transverse Mercator).
* **nx_ny** — grid dimensions; **scanmode**; **fff** — in-file forecast hour
  (usually `000` because the lead time is already the directory).

**Key consequence:** each file is exactly **one (param, level, timestep)** slice.
A single `<reftime>/<geometry>` therefore holds a dense, fully-enumerable cube
of `param × level × leadtime` — and the indexer can reconstruct that cube
**from filenames alone**, opening a file only when a slice is actually drawn.

### 1.3 The "monolithic" layout (files directly under `<reftime>`)

Some producers store whole forecasts in multi-message files; the dimensions live
*inside* the GRIB/NetCDF and must be enumerated by grid-files/newbase, not the name:

| Template | Producer family | Notes |
|---|---|---|
| `fc<reftime>+<fff>grib2_mbr000` | MEPS-type | one file per timestep, ensemble member in name |
| `fc-sfc-…+NNNh_mbr00N.grib2`, `fc-hyb-…`, `fc-postop-…` | MEPS ensemble | surface / hybrid split, members 0–~50 |
| `F6D…`, `F9D…`, `F<n>E<n>` | ECMWF dissemination | full multi-param/level/step GRIB |
| `gfs.t00z.pgrb2full.0p25.f024` | GFS | `.fNNN` = forecast hour |
| `ecmwf+024h.grib2`, `fc-fractile-…`, `fc-himan-…` | post-processed | |

### 1.4 Non-NWP rasters

* **GeoTIFF** (`.tif/.tiff`, ~3 k): radar nowcasts — `…_radar.fmippn.rate…det.tif`,
  `…fmippn.accrate.det.5min…`, `…composite_cappi_…rate_finradfast…`, `…DIW_…`.
  2D single fields in dense time series → pure animation targets.
* **`.npy`** under `data/forecast/.../512x512`: ML training cubes — out of scope.

### 1.5 Geometry-name inventory (reveals the model and the axes to expect)

| Geometry | Model / area | 3D axis present |
|---|---|---|
| `ECEUR0100`, `ECGLO0100`, `ECEUR0200` | ECMWF 0.1°/0.2° Eur/Glob | hybrid + pressure |
| `ICONGLO0125`, `ICONEUR00625` | DWD ICON | hybrid + pressure |
| `GFS0250` | NCEP GFS 0.25° | pressure |
| `MEPS2500D`, `MEPS1500D` | MetCoOp MEPS 2.5/1.5 km | hybrid |
| `MEPS2500D_ICING3D` | MEPS icing | `general` slabs (3D icing) |
| `NEMO801738_UV`, `COPERNICUSNEMO` | NEMO ocean | **depth** |
| `WAMBALMFC`, `WAMEC`, `WAMHKI`, `WAM_BALMFC_ARCH` | WAM wave | mostly 2D spectral |
| `SILAMAQEUR`, `SILAMAQFF` | SILAM air quality | height/levels |
| `LAPSLAMBERT2500`, `LAPS3000` | LAPS analysis | surface |
| `ILMASTO_INTERP_1KM`, `MES070120`, `VANADIS`, `ENFUSORHKI`, `TOPLINK` | FMI products | mostly 2D |

So "is there a vertical axis?" is answered cheaply: **count the distinct
`leveltype`/`level` values seen in the filenames** of a `<reftime>/<geometry>`.

---

## 2. Indexing model (filesystem-only, no Redis)

A new component — call it **`QdlessCatalog`** — turns the tree into a navigable
multi-dimensional index. It never opens a data file during scan.

```
Catalog
└── Producer(id, name?)                       # name resolved via §5
    └── ReferenceTime(YYYYMMDDHHMM)
        └── Geometry(name, proj, nx, ny)       # "(monolithic)" pseudo-geometry too
            └── Parameter(name, unit)
                ├── leveltype, [level values]  # the Z axis (may be a single surface)
                └── [leadtime hours]           # the T axis  → file path per (level,time)
```

Two parsers feed it:

1. **Exploded parser** — regex over the path + filename grammar in §1.2. One
   `readdir` per leaf gives the full `(param,level,time)` enumeration for free.
   This covers ~99 % of files.
2. **Monolithic parser** — for files sitting directly under `<reftime>` (§1.3):
   group by template, treat each file as a source whose internal
   `(param,level,time,member)` axes are read lazily via grid-files on first open.

### Runtime access pattern — lazy, one `readdir` per step, never `stat`

The catalog **never walks the tree** and **never calls `stat`**. A recursive
`ls -lR /masala` takes ~90 s — but that cost is `stat()` on ~1.2 M files plus
recursion through ~14 600 directories, and the browser needs neither. Every
metadata field lives in the directory and file *names*, so a single
`readdir`/`getdents` of the one directory the user just opened is enough:

| Browser column opened | Directory read (one) | ~entries |
|---|---|---|
| Producers | `datasets/` | ~50 |
| Reference times | `datasets/<producer>/` | tens–hundreds |
| Geometries | `datasets/<producer>/<reftime>/` | a few |
| Lead times | `…/<geometry>/` | tens–~130 |
| Param/level slices | `…/<geometry>/<leadtime>/` | up to ~1000 |

Descending the full path is **5 directory reads total**, each touching a single
directory — interactive latency, not a tree scan. `d_type` from `getdents`
distinguishes dir-from-file for free, so no per-entry `stat` is required. The Z
(level set) and T (lead-time set) extents are read from the same leaf and
geometry `readdir`s already being done. Listings are made **only when a node is
expanded**; an optional in-memory (or mtime-keyed on-disk) cache makes revisits
instant and the next column can be prefetched in the background.

The captured `ls -lR` dump is a **dev-only offline fixture** for unit-testing the
name parser — it is *not* the runtime mechanism, and the analysis in this
document was produced from it only because the live mount was not at hand.

The **ensemble member** (`mbrNNN`, up to ~50 + control) becomes a fourth axis,
surfaced as a member selector where present.

---

## 3. Browser UI in qdless

Add a **Miller-column / breadcrumb navigator** popup (reusing the raw-ANSI popup
machinery already used for place-search), opened with a key (e.g. `B` for
*browse*) and given a root path (`--catalog /masala/datasets`):

```
Producer        Reference time     Geometry        Parameter
────────────    ───────────────    ────────────    ──────────────
 131 ECMWF  ▸    2026-06-12 12Z ▸    ECEUR0100  ▸    T   Temperature ▸
 4   MEPS        2026-06-12 06Z      ECGLO0100       U   Wind U
 52  GFS         2026-06-12 00Z      (monolithic)    Q   Spec.humidity
 …                                                   RH  Rel.humidity
                                                     N   Cloud cover
```

* Columns are populated lazily from the catalog as the user moves right.
* Newest reference time first; lead-time and level ranges shown as a footer
  hint (`levels: hybrid 1–137 · steps: +0…+66 h /1h, +66…+240 /3h`).
* Selecting a parameter hands the viewer **two wired axes**: T (lead times) and
  Z (level values) — both already first-class concepts in qdless (animation +
  cross-section). A member axis appears when present.
* A free-text filter row (like the city search) narrows producers/params.

---

## 4. Rendering: animation and 3D

qdless already has the two hard pieces — **time animation** and **cross-section
across pressure/height levels**. The catalog just feeds them the right file set,
loading each `(level,time)` slice on demand.

### 4.1 Time animation (T axis)
For the chosen `(producer, reftime, geometry, param, level)`, the catalog yields
one file per lead time, already sorted. These become animation frames directly —
no change to the existing animator beyond "frames come from N files instead of N
sub-times of one file". Radar GeoTIFF series animate the same way.

### 4.2 The vertical axis (Z) — three escalating modes

1. **Level scrub** — reuse the time-slider UI for the Z axis: scrub through
   hybrid 1…137 / pressure 1000→10 hPa / ocean depth at a fixed lead time.
   Cheapest win; one file per step, already supported by the cross-section loader.

2. **Cross-section / transect** (already in qdless) — click-drag a line; build the
   vertical slice by reading the per-level files along it. The exploded layout is
   ideal: exactly the files needed, nothing more.

3. **Vertical profile probe** — click a point → value-vs-level plot (a
   skew-T-like column): temperature, wind, humidity through the column. Natural
   for hybrid/pressure data and cheap (one column of files).

### 4.3 True 3D / pseudo-volume (when a Z axis exists)

Leverage the existing **Globe** tilted-projection work:

* **Iso-surface as a 2D field** — derive and render a derived surface height:
  freezing-level height (0 °C isotherm), height of a chosen RH/cloud threshold,
  tropopause. One scan up the column per grid point; output is a normal 2D
  shaded field qdless can already draw and animate over time.
* **Tilted volume slab** — render stacked level rasters with depth shading /
  parallax in the quadrant-block raster, viewing angle borrowed from Globe.
  Animate the level or the view angle.
* **3D-icing / `general` slabs** — `MEPS2500D_ICING3D`'s `general_a-b` ranges map
  to flight-level layers; stack them as a vertical icing volume.

### 4.4 Ensembles
Member axis → either step through members, show a postage-stamp grid of all
members for one (param,level,time), or compute mean/spread on the fly.

---

## 5. Resolving producer id → name

The numeric ids are radon producer ids; the listing alone cannot name them, but:

* The **geometry directory name** usually identifies the model (`ECEUR0100`→ECMWF,
  `MEPS2500D`→MEPS, `GFS0250`→GFS, `ICON*`→ICON, `NEMO*`/`WAM*`→ocean/wave).
* Ship an editable **`cnf/producers.json`** (`id → {name, long_name}`), seeded
  from inferred geometry names, overridable by the user. Unknown ids render as
  `model <id>`. Optionally hydrate it from a radon `producer` dump if reachable —
  but that is an *enhancement*, not a dependency, keeping the browser
  filesystem-only as requested.

---

## 6. Suggested build order

1. **Filename/path parser + `QdlessCatalog`** (exploded layout) with unit tests
   over a captured `ls -lR` sample → `qdless --catalog <root> --dump` prints the
   resolved cube dimensions for one selection (headless-verifiable, per the
   project's test-against-real-files rule).
2. **Browser popup** (Miller columns) wired to open the existing viewer with the
   T axis = lead times.
3. **Z-axis scrub + vertical profile probe** (reuse cross-section loader).
4. **Monolithic-file backend** (MEPS/ECMWF/GFS multi-message) via grid-files.
5. **Iso-surface / tilted-volume 3D** building on Globe; **ensemble** axis.
6. **GeoTIFF radar** animation; **`producers.json`** name resolution.

The crucial enabling insight is in §1.2: because each exploded file is a single
`(param, level, time)` slice with the geometry and axes encoded in its name, the
entire navigable hypercube is reconstructable from filenames, and qdless's
existing animation + cross-section engines already cover the two axes that matter.

---

## 7. Implementation status (2026-06-13)

Built and verified against the `test/data/masala` sample:

* **`QdlessCatalog`** (`include/QdlessCatalog.h`, `source/QdlessCatalog.cpp`) —
  filename parser (anchored on the leveltype keyword, so it tolerates params
  containing `_` and the extra trailing fields some producers append, e.g.
  `ATMICEG-GH-3CM_height_3500_lcc_949_1069_0_000_3_7`), `MasalaCube` builder,
  and lazy one-`readdir`-per-node navigation helpers.
* **`MasalaSource`** (`QdlessMasalaSource.*`) — a `DataSource` over one cube.
  Param/level/time axes come from filenames; the slice for the current
  `(param, level, time)` is read on demand by opening that one file through
  `DataSource::open` and delegating spatial queries to it (LRU-cached, 16
  files). Param ids resolve to real newbase ids from a representative file so
  `-p` and palette lookup work; the **filename token** ("T-K") is the display
  name (matches the catalog and resolves the right palette).
* **CLI** `--catalog <path>` — opens a cube directly (works headless with
  `--dump`) or, above a cube, defers to the interactive Miller-column picker
  (`producer → reftime → geometry`), reusing `popupMenu`. `[D]` re-opens it.
* **`-p` by display name** — `initFromSource` now matches `-p` against the
  source's display short names too, so native GRIB/radon names ("T-K", "U-MS")
  work, not just newbase enum names.

Verified headless: time animation (CAPE across 125 lead times, valid time =
reftime + lead, honouring the 1h→3h→6h step changes), level scrub (T-K hybrid
1–137, physically correct lapse rate), and the globe view (auto-centres on the
data). `rll` and `lcc` now render correctly (see §8).

### §8 — the rotated-ll / Lambert rendering fix (prerequisite)

Most of `/masala` (MEPS/ECMWF/HARMONIE) is `rotated_ll` or `lambert`, and these
rendered **blank** in qdless — a pre-existing bug, not masala-specific. Root
cause: `GridFilesSource::readGridLatLon` used grid-files'
`getGridLatLonCoordinatesByGridPosition` (the *double*-position accessor), which
returns `(0,0)` for every position on rotated/Lambert grids in the linked
grid-files build, collapsing `uvToLatLon` to the equator so every sample missed.
Fix: use the **integer** `getGridLatLonCoordinatesByGridPoint` (rounded grid
point) — correct for all projections, no regression to `regular_ll`. This fixes
single-file rendering of those grids too.

### Known gaps / future work

* **Lambert is slow** (~150 s for a 949×1069 grid): grid-files' geo→value
  lookup for Lambert appears O(grid) per call, uncached. `rll`/`regular_ll`
  are fast. Needs a cached value/coordinate vector in `GridFilesSource`.
* **NetCDF** (wave `.nc`) still renders blank: `getGridDimensions()` returns
  0×0, so the grid geometry path is never taken — a separate NetCDF issue.
* **True 3D point cloud** (`[3]`/`v`) is gated to PVOL / QueryData-with-height;
  `MasalaSource` exposes hybrid *level numbers*, not metre heights, so it uses
  the level-based cross-section path instead. A vertical-profile / iso-surface
  mode would need geopotential height (a `Z-M2S2` param or hypsometric
  estimate).
* **Monolithic files** (MEPS `fc-…_mbr000`, ECMWF `F6D…`, GFS `gfs.t…fNNN`),
  the **ensemble** member axis, **GeoTIFF radar** series, and a
  `producers.json` id→name table remain as outlined in §6.
