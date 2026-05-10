#pragma once

#include "QdlessCoastline.h"
#include "QdlessDataSource.h"
#include "QdlessPalette.h"

#include <memory>
#include <string>
#include <vector>

class NFmiArea;
class OGRLayer;

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
  // Construct from an already-opened OGRLayer. The caller (App in
  // PostGIS mode) owns the GDALDataset that hosts `layer` and must
  // keep it alive for the duration of this constructor; afterwards
  // ShapeSource has copied everything it needs and the layer can be
  // discarded. `displayName` shows up as the source's "filename" in
  // status messages and metadata (e.g. "public.itameri_alueet").
  ShapeSource(OGRLayer* layer, const std::string& displayName);
  ShapeSource(OGRLayer* layer, const std::string& displayName, Options opts);
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

  // Polygon (and polyline) boundaries in lat/lon. App keeps these in
  // a dedicated `itsShapeOutlines` slot so the regular [B] cycling
  // for GSHHS political borders is unaffected; outline styling is on
  // its own [O] toggle.
  const std::vector<Polyline>& outlines() const { return itsOutlines; }
  // Recommended palette for this shapefile (flat fill, or rainbow
  // over the burn-id range when the constructor was passed
  // `Options::rainbow`). Used by the App when no --palette override
  // was supplied.
  Palette recommendedPalette() const;
  // Rainbow palette restricted to burn ids that actually appear in
  // the rasterised grid; each band labelled by the feature's
  // NAME/NIMI/first text field (fallback "#N"). Drives the [G]Legend
  // popup so the user sees one swatch per visible polygon, not one
  // per registered burn id (which would include all the
  // sub-pixel-sized children of a MultiPolygon).
  Palette rainbowPalette() const;
  // Number of original OGR features in the shapefile (for the
  // metadata popup). Distinct from burn-value count: a shapefile
  // with one MultiPolygon feature of N sub-polygons has 1 feature
  // but N burn ids.
  int featureCount() const { return itsFeatureCount; }
  // Number of distinct burn ids in the rasterised grid (1..N). Used
  // by the App when sizing the rainbow palette so each sub-polygon
  // of a MultiPolygon gets its own hue.
  int burnIdCount() const { return itsBurnIdCount; }
  // Attribute (label, value) pairs of the feature whose burn id
  // contains the given lat/lon. Empty if the click landed outside
  // every polygon, or if the shapefile has no .dbf fields.
  std::vector<std::pair<std::string, std::string>> attributesAt(double lat,
                                                                double lon) const;
  // Per-feature accessors used by the [A] attributes-table popup.
  // Indices are 0..featureCount()-1.
  const std::vector<std::pair<std::string, std::string>>& featureAttributes(int idx) const;
  // Bounding-box centre (lat, lon) of feature `idx`. Cheap proxy for
  // a true centroid; good enough for dropping the click marker on a
  // user-picked row.
  std::pair<double, double> featureCentroid(int idx) const;
  // Field names from the .dbf — used as table column headers.
  const std::vector<std::string>& fieldNames() const { return itsFieldNames; }

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
  // For each burn id i ∈ [1..itsBurnIdCount], itsBurnToFeature[i-1]
  // is the index of the original OGR feature it came from. Multiple
  // burn ids can map to the same feature when that feature is a
  // MultiPolygon — every leaf polygon gets its own burn value so the
  // rainbow palette can colour them separately, but they all share
  // the same .dbf attribute row.
  std::vector<int> itsBurnToFeature;
  // Per-feature .dbf attributes captured at construction (the OGR
  // features themselves are destroyed before we exit the ctor).
  std::vector<std::vector<std::pair<std::string, std::string>>> itsAttributes;
  // Per-feature bounding-box centre (lat, lon) — used as the marker
  // position when the user picks a feature from the [A] table.
  std::vector<std::pair<double, double>> itsCentroids;
  // Geometric details for the metadata popup.
  std::string itsGeometryType;
  std::string itsLayerName;
  std::vector<std::string> itsFieldNames;
  int itsFeatureCount = 0;
  int itsBurnIdCount = 0;
  // GeoTransform of itsGrid: x = origX + col*pixelW, y = origY + row*pixelH.
  // Used by interpolatedValue to map (lat,lon) → grid cell directly.
  double itsOriginX = 0;
  double itsOriginY = 0;
  double itsPixelW = 0;
  double itsPixelH = 0;

  // Heavy lifting (reproject features, rasterise, capture
  // attributes/centroids/outlines) shared by both ctors. The caller
  // owns the OGRDataset that hosts `layer`.
  void init(OGRLayer* layer);
};
}  // namespace Qdless
