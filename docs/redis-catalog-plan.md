# Plan: browse the SmartMet grid catalog (Redis) and 3D hybrid/pressure views

Status: **planned, not started** (design only — no code written yet).
Last updated: 2026-06-13.

## Goal

Let qdless browse and visualize the gridded data that the SmartMet
**grid-engine content server** knows about, and add proper 3D views for
hybrid- and pressure-level data.

Key constraints (from the user):

- **Do not contact radon directly.** Read metadata from the **Redis content
  server** (the catalog that `radon2smartmet` populates from radon). The Redis
  instance may be remote (not localhost).
- **GRIB files are read directly off disk.** The machine running qdless has the
  data mounts available, plus the grid-engine / grid-files configuration files
  (geometry, parameter, projection definitions). No running grid-engine or
  DataServer is required — Redis is used purely as a catalog/index, and the
  bytes are read from the on-disk path the catalog reports.
- **Data spans many files.** Different forecast times, levels, and parameters
  may each live in different GRIB files. The single-file case is just the
  degenerate case.
- Data volume is ~3 TB. Full local access isn't available during development;
  **sshfs is the likely dev/test path, but it will be slow** — so lazy file
  opening and a file-handle cache are not optional niceties, they're required
  for usable animation/scrubbing.

## Reference code studied

- `~/hub/brainstorm/plugins/grid-gui/` — the web UI that does the equivalent
  browse+render. Cascading metadata queries: `getProducerInfoList` →
  `getGenerationInfoListByProducerId` → `getContentParamKeyListByGenerationId`
  → `getContentListByParameterAndGenerationId` (Plugin.cpp:3382/3425/3495/3581).
  Reads values via DataServer (`getGridData`, Plugin.cpp:700) — we will instead
  read straight off disk.
- `~/hub/grid-content/` — the content/data server library.
  - Catalog client interface: `ContentServer::ServiceInterface`
    (`src/contentServer/definition/ServiceInterface.h`).
  - Redis client: `ContentServer::RedisImplementation`
    (`src/contentServer/redis/RedisImplementation.h`) — `init(redisAddress,
    redisPort, tablePrefix, ...)`, uses hiredis, supports primary+secondary.
  - `FileInfo.mName` = on-disk path; `FileInfo.mProtocol`/`mServerType`
    distinguish filesystem vs S3/HTTP (`src/contentServer/definition/FileInfo.h`).
  - `ContentInfo` carries `mFileId`, `mMessageIndex`, `mFilePosition`,
    `mParameterKey`, `mFmiParameterLevelId`, `mParameterLevel`, `mForecastTime`,
    `mForecastType/Number`, `mGeometryId`
    (`src/contentServer/definition/ContentInfo.h`).
- `~/hub/tools-grid/src/fmi/radon2smartmet.cpp` — populates the catalog from
  radon (the producer side; we are the read side, like the `cs_get*` tools).
  Redis address/port/prefix and grid-files config come from
  `cfg/radon-to-smartmet.cfg`.
- `~/hub/grid-files/` — `GRID::GridFile` (`src/grid/GridFile.h`: `read()`,
  `getMessageByIndex()`) and `GRID::Message` (`src/grid/Message.h`:
  `getGridValueVector`, `getGridValueByPoint`, lat/lon coordinate accessors).

## What already exists in qdless (the good news)

This is **not** "teach qdless to read GRIB" — that already works.

- Clean `DataSource` abstraction (`include/QdlessDataSource.h`). The renderer/UI
  never touch newbase types; they call `interpolatedValue(lat,lon)`,
  `uvToLatLon()` / `latLonToUV()`, and parameter/level/time enumerators. A new
  backend needs **zero** changes to the renderer, palette, UI, or 3D code.
- A working `GridFilesSource` (`source/QdlessGridFilesSource.cpp`,
  `include/QdlessGridFilesSource.h`) that reads GRIB1/GRIB2/NetCDF directly via
  grid-files, inits `Identification::gridDef` from standard config locations
  (`QdlessGridFilesSource.cpp:50-80`), derives parameter names/units from the
  message, and builds **multi-level-type "level groups"** — the same parameter
  on pressure *and* hybrid *and* height surfaces
  (`QdlessGridFilesSource.cpp:99-217`). Geometry/projection comes from each
  message's own grid definition, so no reprojection/DataServer needed.
- Full volumetric/3D machinery already wired for querydata + ODIM volumes:
  `hasNativeHeight()`, `heightRangeKm()`, `interpolatedValueAtHeight()`,
  `sampleColumnProfile()`, `sampleVolumeGrid()` on `DataSource`; the `[4]` 3D
  curtain and discrete-level cross-sections in `QdlessApp`. Querydata
  implements the height methods at `QdlessQueryDataSource.cpp:338-484`.

The two real gaps: (1) no catalog browser to discover *which* files; (2)
`GridFilesSource` does not yet implement the height-aware methods, so GRIB
hybrid/pressure data can't drive the continuous 3D curtain (only discrete-level
cross-sections).

## Part A — `ContentServerSource`: catalog-backed multi-file GRIB source

A new `DataSource` backend using Redis as an index and reading files from disk.

- **Connection.** Link `grid-content` and instantiate
  `ContentServer::RedisImplementation`, calling `init(address, port, prefix)`.
  Address/port/**table-prefix** come from CLI flags (e.g. `--redis-host`,
  `--redis-port`, `--redis-prefix`) defaulting to the same
  `REDIS_CONTENT_SERVER_*` env vars the engine uses. Matching the table prefix
  is essential — wrong prefix = empty catalog.
- **Browse UX.** Cascading ncurses popup (reuse qdless's popup style) mirroring
  grid-gui: **producer → generation (analysis time) → parameter → geometry →
  level type**, then drop into the normal map view.
- **Indexing (multi-file).** From the filtered `ContentInfoList`, build the same
  `(paramId, levelTypeId, timeIdx, levelIdxInGroup) → (filePath, messageIndex)`
  map `GridFilesSource` builds for one file — except each tuple may resolve to a
  **different file**. This is the core multi-file requirement.
- **File-handle cache.** Small LRU of opened `GridFile` objects keyed by path;
  open lazily on first access. Critical for sshfs/slow-mount performance during
  animation and time/level scrubbing.
- **Reading.** Identical to `GridFilesSource`: `GridFile::read(path)` once, then
  `getMessageByIndex(idx)`; `uvToLatLon` off the message's grid definition.
- **Reuse strategy.** Factor the message-reading / level-group / `uvToLatLon`
  core out of `GridFilesSource` into a shared helper or common base, so
  `ContentServerSource` is mostly "build the index from Redis instead of from
  one file's message list." Avoid duplicating the ~800 lines that already work.
- **Caveat to handle.** Detect non-filesystem `FileInfo` protocols (S3/HTTP) and
  show a clear "not locally available" message instead of failing obscurely.
  Only filesystem-protocol files (and S3 only if fuse-mounted) are readable.

## Part B — 3D for hybrid & pressure levels

Net-new work. The `DataSource` 3D interface already exists; implement it for the
grid source by computing a height per level (mirrors
`QdlessQueryDataSource.cpp:338-484`):

- **Pressure levels.** If the generation has geopotential / geopotential-height
  on the same pressure levels, sample it for geometric height per level. Else
  fall back to the standard-atmosphere pressure→height relation.
- **Hybrid levels.** Need surface pressure + the model's hybrid **A/B
  coefficients** to get pressure per level, then height via geopotential or
  standard atmosphere. If a geopotential-height field exists on hybrid levels,
  prefer it. **Open question:** whether A/B coefficients + surface pressure come
  through the GRIB messages (grid-files exposes them) or need config — resolve
  this against a real hybrid GRIB sample.

Once `interpolatedValueAtHeight` works for the grid source, the existing `[4]`
curtain, `sampleColumnProfile`, and cross-sections light up with no renderer
changes.

## Open decisions

1. **Scope of first cut.** Recommend landing Part A (browse + 2D + discrete-level
   cross-sections) first (mostly wiring existing pieces), then Part B as a second
   pass. Confirm vs. wanting 3D in the same pass.
2. **Dependency weight.** Recommend linking `grid-content` (pulls spine +
   hiredis) — the robust route every other catalog client uses, and qdless was
   split out specifically to be allowed heavier grid deps. Alternative
   (hand-rolled hiredis re-implementing the key schema) is lighter but brittle;
   not recommended.
3. **Browse entry point.** Recommend an explicit flag (`--redis-host` / a
   `--catalog` flag) so plain `qdless file.sqd` is unaffected, rather than always
   probing the default Redis env.

## Data / config access needed (blocked until provided)

Per the "test against real files" rule, validate end-to-end against real data
before declaring done. ~3 TB total; full local access not available now —
**sshfs is the expected dev/test mount, accepted-slow**, which is exactly why
lazy-open + handle-cache are in the design.

Priority order of what to obtain:

1. A populated Redis content server to point at (host/port/**table-prefix**),
   read-only is fine, with its disk mounts visible (via sshfs if needed).
2. A real `grid-engine.conf` / `radon-to-smartmet.cfg` for the exact
   `REDIS_CONTENT_SERVER_*` conventions and table prefix, to set CLI-flag
   defaults and env-var names.
3. The deployed `grid-files.conf` + geometry/parameter CSVs and the value of
   `GRID_FILES_LIBRARY_CONFIG_FILE` on the target — qdless currently probes
   hardcoded `/usr/share/smartmet/...` paths; may need a `--grid-config` flag or
   to read the env var. Without this init, param-name/geometry resolution falls
   back to raw GRIB codes.
4. One or two sample hybrid-level GRIB files (for Part B — to settle the A/B
   coefficient question).

Nice-to-have: `producers.cfg` for sensible default producer ordering.

Offline fallback if a live Redis is awkward: a small dump (a producer list, one
generation's content list from `cs_get*`, plus a directory listing of one
referenced file) — enough to validate the key schema and path resolution.
