#pragma once

#include "QdlessPalette.h"

#include <newbase/NFmiMetTime.h>

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

  // Level axis.
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

  // Sample the currently-selected (param, time, level) slice at a given
  // lat/lon. Returns kFloatMissing or non-finite for missing / out-of-grid.
  virtual float interpolatedValue(double lat, double lon) const = 0;

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
