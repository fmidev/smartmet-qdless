#pragma once

#include "QdlessCoastline.h"
#include "QdlessDataSource.h"
#include "QdlessPalette.h"

#include <memory>
#include <string>
#include <vector>

class NFmiArea;

namespace Qdless
{
// DataSource for ESRI shapefiles read via OGR. The geometry is
// rasterised once at construction into a fixed-size buffer of feature
// indices (1..N for inside-a-polygon, 0 outside). The renderer's
// existing palette + overlay machinery then handles the rest:
//
//   - interpolatedValue(lat, lon) → feature id at that grid cell
//   - palette → flat fill (default) or cycling rainbow (--color-by *)
//   - polygon outlines exposed via outlines() so the App can hand
//     them to the same Braille / thick-stroke overlay path that
//     renders GSHHS coastlines and political borders
//
// Shapefiles have no temporal axis; one .shp == one timestep. Time and
// level are fixed at 0. The renderer's projection-aware overlays
// (graticule, GSHHS coast, place names) all keep working because the
// shapefile carries a real .prj — this is *not* image mode.
class ShapeSource : public DataSource
{
 public:
  struct Options
  {
    // Colour for flat fill (default = mid grey). Ignored when
    // colorByField is non-empty.
    Rgb fillColor = {120, 120, 120};
    // When set, colour each feature by the given attribute value:
    //   - numeric field: discretised over the value range
    //   - string field: hashed to one of the rainbow buckets
    // Empty → flat fill.
    std::string colorByField;
    // Force the rainbow palette regardless of colorByField. When
    // both are empty, flatFill is used.
    bool rainbow = false;
    // Rasterisation resolution. Larger = finer outlines under zoom
    // but more memory + slower startup. The default ~2k×2k uses
    // ~8 MB for a uint16 raster which is fine for any sane shapefile.
    int rasterMax = 2000;
  };

  explicit ShapeSource(const std::string& filename);
  ShapeSource(const std::string& filename, Options opts);
  ~ShapeSource() override;
  ShapeSource(const ShapeSource&) = delete;
  ShapeSource& operator=(const ShapeSource&) = delete;
  ShapeSource(ShapeSource&&) = delete;
  ShapeSource& operator=(ShapeSource&&) = delete;

  std::vector<int> paramIds() const override;
  std::string paramShortName(int paramId) const override;
  std::string paramLongName(int paramId) const override;
  std::string paramUnits(int paramId) const override;
  int currentParamId() const override;
  bool selectParamId(int paramId) override;

  std::size_t timeCount() const override;
  std::size_t currentTimeIndex() const override;
  void selectTimeIndex(std::size_t i) override;
  NFmiMetTime currentValidTime() const override;
  NFmiMetTime originTime() const override;

  std::size_t levelCount() const override;
  std::size_t currentLevelIndex() const override;
  void selectLevelIndex(std::size_t i) override;
  float levelValueAt(std::size_t i) const override;

  float interpolatedValue(double lat, double lon) const override;
  LatLonBox boundingBox() const override;
  void uvToLatLon(double u, double v, double& lat, double& lon) const override;
  void latLonToUV(double lat, double lon, double& u, double& v) const override;

  std::vector<std::pair<std::string, std::string>> extraMetadata() const override;
  std::string gridSignature() const override;

  // Polygon (and polyline) boundaries in lat/lon. The App swaps these
  // into the existing GSHHS-borders slot so the cycling-style
  // (Braille / Thick / None) keypress applies to them. Empty if the
  // shapefile carries only points.
  const std::vector<Polyline>& outlines() const { return itsOutlines; }
  // Recommended palette for this shapefile, given the constructor's
  // options (flat fill or rainbow over the feature count). Used by
  // the App when no --palette override was supplied.
  Palette recommendedPalette() const;
  // Number of features actually rasterised — used by App / metadata.
  int featureCount() const { return itsFeatureCount; }

 private:
  std::string itsFilename;
  Options itsOpts;
  std::unique_ptr<NFmiArea> itsArea;
  // uint16 grid of feature IDs (1..N) with 0 for "outside any
  // polygon". 65535 reserved as a sentinel if a shapefile somehow
  // exceeds 65k features (we cycle then).
  std::vector<std::uint16_t> itsGrid;
  std::size_t itsNx = 0;
  std::size_t itsNy = 0;
  std::vector<Polyline> itsOutlines;
  // Geometric details for the metadata popup.
  std::string itsGeometryType;
  std::string itsLayerName;
  std::vector<std::string> itsFieldNames;
  int itsFeatureCount = 0;
  // GeoTransform of itsGrid: x = origX + col*pixelW, y = origY + row*pixelH.
  // Used by interpolatedValue to map (lat,lon) → grid cell directly.
  double itsOriginX = 0;
  double itsOriginY = 0;
  double itsPixelW = 0;
  double itsPixelH = 0;
};
}  // namespace Qdless
