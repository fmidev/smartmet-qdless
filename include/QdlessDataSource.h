#pragma once

#include "QdlessPalette.h"

#include <newbase/NFmiMetTime.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Qdless
{
// Lat/lon bounding box for the data extent.
struct LatLonBox
{
  double minLat = -90;
  double maxLat = 90;
  double minLon = -180;
  double maxLon = 180;
};

// What kind of UI the source wants. Sources self-report via
// `category()`; the App branches on this single tag instead of
// scattering isRawImage() / dynamic_cast<ShapeSource*> checks
// across the constructor, key handler, status bar, help popup, etc.
//
//   Gridded  — value-per-cell + palette → colour. The original
//              data path: QueryData, GRIB, NetCDF, ODIM, GeoTIFF.
//              Time-series probe, parameter / level menus, palette
//              legend, cross-section all apply.
//   Image    — naked PNG/WebP/JPEG/GIF/BMP. No projection, no
//              scalar value; pixels go straight from the source's
//              pixelAtUV() to the renderer. Geographic overlays
//              suppressed.
//   Vector   — shapefile or PostGIS layer rasterised to feature ids
//              with outline polylines on top. Click → attributes,
//              [A] table, [O] outlines, [R] palette cycle. No time
//              animation, no cross-section.
enum class SourceCategory
{
  Gridded,
  Image,
  Vector,
};

// Format-agnostic interface to a gridded weather file (.sqd / .grib /
// .grib2 / .nc). Concrete backends wrap NFmiFastQueryInfo (newbase) or
// SmartMet::GRID::GridFile (grid-files).
class DataSource
{
 public:
  virtual ~DataSource() = default;

  // Auto-detects the format and returns a DataSource. Throws on error.
  static std::unique_ptr<DataSource> open(const std::string& filename);

  // Return a fresh, independently-iterable view of the same underlying
  // data, safe to use concurrently with the original for READ-ONLY
  // queries. Used by background threads (e.g. the phenomenon detector)
  // that need to sample the data without blocking the main thread.
  //
  // The default returns nullptr — callers must then either fall back
  // to running the analysis synchronously, or simply skip the
  // background work. QueryDataSource implements this by sharing the
  // underlying NFmiQueryData (a shared_ptr) and giving the clone its
  // own NFmiFastQueryInfo iterator. Other sources can opt in later.
  virtual std::unique_ptr<DataSource> cloneForRead() const { return nullptr; }

  // Parameter access — IDs use newbase / FMI numeric enums.
  virtual std::vector<int> paramIds() const = 0;
  virtual std::string paramShortName(int paramId) const = 0;
  virtual std::string paramLongName(int paramId) const = 0;
  // Unit string as given by the underlying file (e.g. "K", "m/s", "%").
  // Empty if unknown.
  virtual std::string paramUnits(int paramId) const = 0;
  virtual int currentParamId() const = 0;
  virtual bool selectParamId(int paramId) = 0;

  // Time axis.
  virtual std::size_t timeCount() const = 0;
  virtual std::size_t currentTimeIndex() const = 0;
  virtual void selectTimeIndex(std::size_t i) = 0;
  virtual NFmiMetTime currentValidTime() const = 0;

  // Model run / analysis / reference time — when the forecast was issued.
  // QueryData calls this "origin time"; GRIB calls it "analysis time" or
  // "reference time". Returns an invalid (year=0) NFmiMetTime if unknown.
  virtual NFmiMetTime originTime() const { return NFmiMetTime(0, 0, 0, 0, 0); }

  // Level axis. The flat methods below (levelCount/levelValueAt/levelLabel)
  // are always the *active group only* — see LevelGroup. GRIB / NetCDF
  // files can carry several level types in one file (pressure surfaces
  // alongside hybrid surfaces, height-above-ground, ...); mixing them on
  // one cross-section axis is meaningless, so the source segregates them
  // into typed groups and only one group is "live" at a time.
  struct LevelGroup
  {
    int typeId = 0;             // FmiLevelType numeric value (0 = unknown)
    std::string typeName;       // "Pressure (hPa)", "Hybrid", "Height (m)", ...
    std::vector<float> values;  // sorted in natural order for this type
    bool ascendsWithValue = false;
  };
  // All groups available for `paramId`. Default impl synthesises one
  // group from the existing flat list so backends without multi-type
  // levels keep working unchanged.
  virtual std::vector<LevelGroup> levelGroupsForParam(int paramId) const;
  // Switch which group is active for `paramId`. Default: no-op (single
  // group). After this, levelCount / levelValueAt / levelLabel return
  // values for the active group, and the source restores its own memory
  // of the last-used level within that group.
  virtual void selectLevelGroup(int /*paramId*/, int /*groupIdx*/) {}
  virtual int currentLevelGroupIndex(int /*paramId*/) const { return 0; }

  // Human-readable name for an FmiLevelType numeric value. Used by the
  // level picker's section headers and the metadata popup.
  static std::string levelTypeName(int typeId);
  // Type-aware text for a level value (e.g. "850 hPa", "100 m",
  // "FL150", "Surface"). Shared helper for backends that know the type.
  static std::string formatLevelByType(int typeId, float value);

  // Geometric height (metres above sea level) for a level value of a known
  // FmiLevelType, or kFloatMissing when the type carries no derivable height.
  // Pressure (hPa) is mapped through the ISA hypsometric formula; height /
  // altitude levels are already metres and pass through. Hybrid, depth,
  // ground and unknown types return missing — they have no self-contained
  // vertical coordinate (hybrid needs geopotential we don't co-locate).
  // Lets backends that only know their level *type* (e.g. the masala GRIB
  // cube, where each level is a separate file) still expose a height axis
  // for the 3D / cross-section views without a height field in the data.
  static float levelToHeightMeters(int typeId, float levelValue);

  virtual std::size_t levelCount() const = 0;
  virtual std::size_t currentLevelIndex() const = 0;
  virtual void selectLevelIndex(std::size_t i) = 0;
  virtual float levelValueAt(std::size_t i) const = 0;

  // Does the level value increase with altitude? Pressure decreases
  // (1000 hPa near ground, 100 hPa at altitude) — default false. Height
  // levels (m above sea), CAPPI heights, and radar elevation angles
  // increase — those backends override to true so the cross-section
  // popup orients the Y-axis with ground at the bottom.
  virtual bool levelsAscendWithValue() const { return false; }

  // Human-readable label for level `i`, used by the [L] popup and the
  // status line. Default formats `levelValueAt(i)` as a number; sources
  // with synthetic levels (e.g. PVOL "MAX" composite) override to give
  // those entries a string tag instead of a meaningless numeric value.
  virtual std::string levelLabel(std::size_t i) const;

  // Cross-section axis hints. Default sources put one row per level on
  // the Y-axis (suits pressure/hybrid data). Sources that know how to
  // sample at arbitrary heights (PVOL polar volumes) override
  // hasNativeHeight() to true so the cross-section becomes a true RHI:
  // Y-axis is km, sampled via interpolatedValueAtHeight() per pixel.
  virtual bool hasNativeHeight() const { return false; }

  // Y-axis span for the height-mode cross-section, in kilometres above
  // the source's reference (radar antenna for PVOL). Only consulted
  // when hasNativeHeight() is true. Default covers troposphere.
  virtual std::pair<double, double> heightRangeKm() const { return {0.0, 12.0}; }

  // Sample the active (param, time) slice at a 3D point. Default falls
  // back to interpolatedValue() ignoring the height. PVOL inverts the
  // 4/3-Earth beam-height formula to find which sweep covers (lat,lon,h)
  // and reads the corresponding (ray, bin).
  virtual float interpolatedValueAtHeight(double lat, double lon,
                                          double /*heightKm*/) const
  {
    return interpolatedValue(lat, lon);
  }

  // Batched column profile for the active (param, time): return the
  // height (in metres above the reference) and the param value for
  // every level at one (lat, lon). Cheap renderer pre-sample for
  // cross-section curtains — calling interpolatedValueAtHeight per
  // output pixel is O(nLevels) per call, but with this the renderer
  // only pays that cost once per column instead of once per pixel.
  // Default: a single (height=0, value=interpolatedValue) tuple, which
  // is right for sources that have no vertical axis. Volumetric sources
  // (QD with GeomHeight, PVOL) override.
  struct ColumnProfile
  {
    std::vector<float> heightsM;  // length nLevels; metres above reference
    std::vector<float> values;    // length nLevels; active-param value
  };
  virtual ColumnProfile sampleColumnProfile(double lat, double lon) const
  {
    ColumnProfile p;
    p.heightsM.push_back(0.0F);
    p.values.push_back(interpolatedValue(lat, lon));
    return p;
  }

  // One emission of the 3D point-cloud sampler: an active-param value at a
  // geographic point and its geometric height (metres above the source's
  // reference). Shared by every backend that feeds App::draw3DQueryData.
  struct VolumeSample
  {
    double lat;
    double lon;
    double heightMeters;
    float value;
  };

  // True when the source has a renderable 3D volume — multiple levels with a
  // usable height axis — so the App offers the point-cloud / curtain views.
  // Default false; QueryDataSource (height field) and MasalaSource (pressure /
  // height level axis) override. Distinct from hasNativeHeight() only in that
  // a backend could in principle support the curtain without a full volume;
  // in practice the two coincide for the gridded backends.
  virtual bool isVolumetric() const { return false; }

  // Emit (lat, lon, heightMeters, value) for the active (param, time) over the
  // whole volume, for the 3D point cloud. Returns false (no emissions) when the
  // source has no volume. The default walks a coarse lat/lon lattice over
  // boundingBox(), sampling sampleColumnProfile() at each node — correct for
  // any source that implements a height-aware column (MasalaSource). Backends
  // with a native grid (QueryDataSource) override to splat the real cells.
  virtual bool sampleVolume(const std::function<void(const VolumeSample&)>& cb) const;

  // Sample the currently-selected (param, time, level) slice at a given
  // lat/lon. Returns kFloatMissing or non-finite for missing / out-of-grid.
  virtual float interpolatedValue(double lat, double lon) const = 0;

  // Sample the active slice directly at viewport position (u, v) ∈ [0, 1]²
  // (v=0 = top). The renderer calls this once per terminal sub-cell every
  // frame — it is the hot path. The default routes through uvToLatLon +
  // interpolatedValue, which for a projected grid does a forward grid→lat/lon
  // projection here and then an inverse lat/lon→grid projection inside
  // interpolatedValue: a wasteful round-trip that is expensive on rotated-
  // latlon / Lambert grids. Projection-backed sources override this to map
  // (u, v) straight to grid coordinates and bilinear-sample a cached value
  // grid, skipping both projections and the per-pixel decode.
  virtual float sampleValueAtUV(double u, double v) const
  {
    double lat = 0;
    double lon = 0;
    uvToLatLon(u, v, lat, lon);
    return interpolatedValue(lat, lon);
  }

  // Lat/lon bounding box of the data extent (rectangle covering all grid
  // points; exact for lat/lon grids, approximate for projected grids).
  virtual LatLonBox boundingBox() const = 0;

  // Backend-specific extra metadata for the metadata popup ('M'). Returns
  // a list of (label, value) rows, e.g. ("Format", "GRIB2"), ("Grid",
  // "LatLon 2879x1441"), ("Producer", "ECMWF/IFS"). Default is empty;
  // overridden by QueryDataSource and GridFilesSource.
  virtual std::vector<std::pair<std::string, std::string>> extraMetadata() const { return {}; }

  // Map a viewport position (u, v) ∈ [0, 1]² to a geographic lat/lon, and
  // back. The viewport rectangle is the source's "natural" rendering
  // surface: native projection XY for projected sources (QueryData with
  // an NFmiArea, GRIB/NetCDF with a known projection), and lat/lon bbox
  // for unprojected sources. Convention: v=0 is the top (north / max-lat)
  // edge to match image coordinates. Default implementations interpolate
  // over `boundingBox()` so unprojected backends work without overrides.
  virtual void uvToLatLon(double u, double v, double& lat, double& lon) const;
  virtual void latLonToUV(double lat, double lon, double& u, double& v) const;

  // Batched latLonToUV: project many points at once. `lats`/`lons` are
  // parallel inputs; `us`/`vs` are resized and filled in parallel. The
  // default just loops latLonToUV, but projected backends override it —
  // GridFilesSource builds ONE PROJ/GDAL coordinate transform for the whole
  // batch instead of paying the (dominant) per-point transform setup, which
  // is what made projecting a full coastline take seconds on rotated-latlon
  // / Lambert grids. Used by the coastline projection cache.
  virtual void latLonToUVBatch(const std::vector<float>& lats,
                               const std::vector<float>& lons,
                               std::vector<float>& us,
                               std::vector<float>& vs) const;

  // Stable string identifying the grid (projection + dimensions + extent).
  // Used by MultiFileSource to verify all members of a multi-file batch
  // share one grid. Default is a bounding-box-derived string; backends
  // SHOULD override with a stronger fingerprint (projection WKT/proj4 +
  // pixel dimensions + origin) so two unrelated grids that happen to span
  // the same lat/lon bbox don't compare equal. Equality is exact-string;
  // backends should format with stable precision.
  virtual std::string gridSignature() const;

  // What kind of UI this source wants — the App branches on this
  // tag rather than guessing from method behaviour. Default is the
  // gridded-data path; ImageSource and ShapeSource override.
  virtual SourceCategory category() const { return SourceCategory::Gridded; }
  // Convenience predicates over `category()` so call sites read
  // naturally (`if (src.isImage())`) without each having to spell
  // out the enum compare.
  bool isImage() const { return category() == SourceCategory::Image; }
  bool isVector() const { return category() == SourceCategory::Vector; }
  bool isGridded() const { return category() == SourceCategory::Gridded; }
  // Sample the source at viewport (u, v) ∈ [0, 1]² (v=0 is top).
  // Only meaningful when category() == Image; the gridded path
  // goes through interpolatedValue + palette instead. Returns a
  // transparent Rgb for out-of-image samples so zoomed-out
  // viewports show the terminal default outside the image bounds.
  virtual Rgb pixelAtUV(double /*u*/, double /*v*/) const { return Rgb{0, 0, 0, true}; }

  // Hint that the renderer is about to sample from `bbox` at
  // approximately `cellsX × cellsY` resolution. Sources that
  // pre-rasterise at construction (currently only ShapeSource) can
  // use this to refine the rasterisation when the viewport shows a
  // small enough portion that the base grid undersamples it.
  // Default no-op for sources that already serve interpolatedValue
  // at full source resolution.
  virtual void prepareViewport(const LatLonBox& /*bbox*/, int /*cellsX*/,
                               int /*cellsY*/) const
  {
  }
};
}  // namespace Qdless
