#include "QdlessShapeSource.h"

#include <cpl_conv.h>
#include <gdal_alg.h>
#include <gdal_priv.h>
#include <gis/SpatialReference.h>
#include <ogr_api.h>
#include <ogrsf_frmts.h>

#include <newbase/NFmiArea.h>
#include <newbase/NFmiPoint.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <stdexcept>

namespace Qdless
{
namespace
{
// Build a polyline from an OGR LineString-like geometry (LineString
// itself, polygon ring, etc.). Reprojects on the fly through `ct`. If
// `ct` is null, points are assumed already in lat/lon. Empty geometries
// are skipped silently. v=0 is the top, lat/lon goes lat-up.
void appendOGRLineString(const OGRLineString* ls, OGRCoordinateTransformation* ct,
                         std::vector<Polyline>& out)
{
  if (ls == nullptr || ls->getNumPoints() < 2) return;
  Polyline p;
  p.lons.reserve(static_cast<std::size_t>(ls->getNumPoints()));
  p.lats.reserve(static_cast<std::size_t>(ls->getNumPoints()));
  std::vector<double> xs;
  std::vector<double> ys;
  xs.reserve(static_cast<std::size_t>(ls->getNumPoints()));
  ys.reserve(static_cast<std::size_t>(ls->getNumPoints()));
  for (int i = 0; i < ls->getNumPoints(); ++i)
  {
    xs.push_back(ls->getX(i));
    ys.push_back(ls->getY(i));
  }
  if (ct != nullptr)
  {
    // OGR's transform mutates an array of (x,y[,z]) in place. Pass
    // separate buffers (which is the per-axis API) so we don't
    // confuse axis order with newer GDALs that swap by default.
    if (!ct->Transform(static_cast<int>(xs.size()), xs.data(), ys.data())) return;
  }
  for (std::size_t i = 0; i < xs.size(); ++i)
  {
    p.lons.push_back(static_cast<float>(xs[i]));
    p.lats.push_back(static_cast<float>(ys[i]));
  }
  out.push_back(std::move(p));
}

void collectOutlines(const OGRGeometry* g, OGRCoordinateTransformation* ct,
                     std::vector<Polyline>& out)
{
  if (g == nullptr) return;
  switch (wkbFlatten(g->getGeometryType()))
  {
    case wkbLineString:
      appendOGRLineString(g->toLineString(), ct, out);
      break;
    case wkbPolygon:
    {
      const auto* poly = g->toPolygon();
      appendOGRLineString(poly->getExteriorRing(), ct, out);
      for (int i = 0; i < poly->getNumInteriorRings(); ++i)
        appendOGRLineString(poly->getInteriorRing(i), ct, out);
      break;
    }
    case wkbMultiLineString:
    case wkbMultiPolygon:
    case wkbGeometryCollection:
    {
      const auto* gc = g->toGeometryCollection();
      for (int i = 0; i < gc->getNumGeometries(); ++i)
        collectOutlines(gc->getGeometryRef(i), ct, out);
      break;
    }
    default:
      // Points / multipoints: nothing to outline.
      break;
  }
}
}  // namespace

ShapeSource::ShapeSource(const std::string& filename) : ShapeSource(filename, Options{}) {}

ShapeSource::ShapeSource(const std::string& filename, Options opts)
    : itsFilename(filename), itsOpts(std::move(opts))
{
  GDALAllRegister();
  // GDAL_OF_VECTOR avoids ambiguity with the raster driver when the
  // file extension would otherwise allow either (rare for .shp but a
  // good hygiene anyway).
  auto* ds = static_cast<GDALDataset*>(
      GDALOpenEx(filename.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr));
  if (!ds) throw std::runtime_error("OGR failed to open: " + filename);
  struct DsGuard
  {
    GDALDataset* p;
    ~DsGuard() { if (p) GDALClose(p); }
  } dsGuard{ds};

  if (ds->GetLayerCount() < 1)
    throw std::runtime_error("shapefile has no layers: " + filename);
  OGRLayer* layer = ds->GetLayer(0);
  itsLayerName = layer->GetName();

  // Geometry type name for the metadata popup.
  itsGeometryType = OGRGeometryTypeToName(layer->GetGeomType());

  // Field schema — first few names go into the metadata popup.
  if (auto* defn = layer->GetLayerDefn(); defn != nullptr)
  {
    for (int i = 0; i < defn->GetFieldCount() && itsFieldNames.size() < 8; ++i)
      itsFieldNames.emplace_back(defn->GetFieldDefn(i)->GetNameRef());
  }

  // Working coordinate system: WGS84 lat/lon. The shapefile may carry
  // any projection; we reproject features into WGS84 for both the
  // outline-polyline collection (which qdless's overlay code expects
  // in lat/lon) and the rasterisation (so the resulting grid is
  // axis-aligned in lat/lon).
  OGRSpatialReference dstSrs;
  dstSrs.SetWellKnownGeogCS("WGS84");
  // Force traditional lon,lat axis order regardless of GDAL's default
  // for newer versions; that's what NFmiArea + the rest of qdless
  // assume internally.
  dstSrs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  // OGR returns const OGRSpatialReference*; we need a mutable one only
  // to call SetAxisMappingStrategy. Clone the layer SRS to a writable
  // copy below.
  const OGRSpatialReference* srcSrs = layer->GetSpatialRef();
  std::unique_ptr<OGRCoordinateTransformation> ct;
  if (srcSrs != nullptr && !srcSrs->IsSame(&dstSrs))
  {
    auto* clone = srcSrs->Clone();
    clone->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    ct.reset(OGRCreateCoordinateTransformation(clone, &dstSrs));
    OGRSpatialReference::DestroySpatialReference(clone);
    if (!ct)
      throw std::runtime_error("ShapeSource: cannot transform from layer SRS to WGS84");
  }

  // Envelope in WGS84. layer->GetExtent gives source SRS; reproject
  // its corners through `ct` to get a usable lat/lon bbox.
  OGREnvelope env;
  if (layer->GetExtent(&env, /*force=*/TRUE) != OGRERR_NONE)
    throw std::runtime_error("ShapeSource: cannot compute layer extent");
  double xs[4] = {env.MinX, env.MaxX, env.MaxX, env.MinX};
  double ys[4] = {env.MinY, env.MinY, env.MaxY, env.MaxY};
  if (ct) ct->Transform(4, xs, ys);
  const double minLon = *std::min_element(xs, xs + 4);
  const double maxLon = *std::max_element(xs, xs + 4);
  const double minLat = *std::min_element(ys, ys + 4);
  const double maxLat = *std::max_element(ys, ys + 4);
  if (!(maxLon > minLon) || !(maxLat > minLat))
    throw std::runtime_error("ShapeSource: degenerate envelope");

  // Pick a reasonable rasterisation grid. Maintain envelope aspect so
  // pixels are roughly square in lat/lon (good enough for visual
  // sanity even though degrees of longitude vary with latitude).
  const double aspect = (maxLon - minLon) / (maxLat - minLat);
  const int M = std::max(64, itsOpts.rasterMax);
  if (aspect >= 1.0)
  {
    itsNx = static_cast<std::size_t>(M);
    itsNy = static_cast<std::size_t>(std::max(64, static_cast<int>(M / aspect)));
  }
  else
  {
    itsNy = static_cast<std::size_t>(M);
    itsNx = static_cast<std::size_t>(std::max(64, static_cast<int>(M * aspect)));
  }

  // GeoTransform: (originX, dx, 0, originY, 0, dy). Row 0 = top = max
  // latitude (pixelH negative).
  itsOriginX = minLon;
  itsOriginY = maxLat;
  itsPixelW = (maxLon - minLon) / static_cast<double>(itsNx);
  itsPixelH = (minLat - maxLat) / static_cast<double>(itsNy);

  // Build the NFmiArea so uvToLatLon / latLonToUV behave like the
  // other projection-aware backends.
  Fmi::SpatialReference sr(dstSrs);
  itsArea.reset(NFmiArea::CreateFromBBox(sr, NFmiPoint(minLon, minLat), NFmiPoint(maxLon, maxLat)));
  if (!itsArea) throw std::runtime_error("ShapeSource: failed to construct area");

  // ----- Rasterise -----
  // In-memory MEM raster of uint16 feature IDs. We rasterise feature
  // by feature with sequentially-incrementing burn values so each
  // polygon ends up tagged with a distinct id; the renderer then maps
  // id → colour via the palette.
  GDALDriver* memDrv = GetGDALDriverManager()->GetDriverByName("MEM");
  if (!memDrv) throw std::runtime_error("ShapeSource: MEM driver missing");
  auto* mem = memDrv->Create("", static_cast<int>(itsNx), static_cast<int>(itsNy), 1,
                             GDT_UInt16, nullptr);
  if (!mem) throw std::runtime_error("ShapeSource: MEM Create failed");
  struct MemGuard
  {
    GDALDataset* p;
    ~MemGuard() { if (p) GDALClose(p); }
  } memGuard{mem};
  double gt[6] = {itsOriginX, itsPixelW, 0.0, itsOriginY, 0.0, itsPixelH};
  mem->SetGeoTransform(gt);
  char* dstWkt = nullptr;
  dstSrs.exportToWkt(&dstWkt);
  if (dstWkt) { mem->SetProjection(dstWkt); CPLFree(dstWkt); }

  // Walk the layer once, building (geometry, burnValue) pairs and the
  // outline polylines. Reprojects each feature's geometry to WGS84.
  layer->ResetReading();
  std::vector<OGRGeometry*> ownedGeoms;  // reproject creates new OGRGeometry instances
  std::vector<OGRGeometryH> handles;
  std::vector<double> burn;
  ownedGeoms.reserve(static_cast<std::size_t>(layer->GetFeatureCount()));
  handles.reserve(ownedGeoms.capacity());
  burn.reserve(ownedGeoms.capacity());
  itsOutlines.reserve(ownedGeoms.capacity());

  while (auto* feat = layer->GetNextFeature())
  {
    struct FeatGuard
    {
      OGRFeature* f;
      ~FeatGuard() { if (f) OGRFeature::DestroyFeature(f); }
    } fg{feat};
    OGRGeometry* g = feat->GetGeometryRef();
    if (g == nullptr) continue;
    OGRGeometry* clone = g->clone();
    if (ct)
    {
      if (clone->transform(ct.get()) != OGRERR_NONE)
      {
        OGRGeometryFactory::destroyGeometry(clone);
        continue;
      }
    }
    collectOutlines(clone, /*ct already applied*/ nullptr, itsOutlines);
    // Burn value = feature index + 1 (0 is reserved for "outside").
    // Cycle modulo 65535 if a shapefile has more features than fit in
    // uint16; rainbow palette will repeat hues but flat fill doesn't
    // care.
    const double v = static_cast<double>((itsFeatureCount % 65535) + 1);
    ownedGeoms.push_back(clone);
    handles.push_back(static_cast<OGRGeometryH>(clone));
    burn.push_back(v);
    ++itsFeatureCount;
  }

  if (itsFeatureCount > 0)
  {
    int bandList[1] = {1};
    // ALL_TOUCHED ensures thin polygons / points / lines paint at
    // least one pixel per cell crossed instead of vanishing into
    // nothing on coarse rasterisation.
    char** rasterOpts = nullptr;
    rasterOpts = CSLSetNameValue(rasterOpts, "ALL_TOUCHED", "TRUE");
    CPLErr err = GDALRasterizeGeometries(
        mem, 1, bandList, static_cast<int>(handles.size()), handles.data(),
        nullptr, nullptr, burn.data(), rasterOpts, nullptr, nullptr);
    CSLDestroy(rasterOpts);
    if (err != CE_None)
    {
      for (auto* g : ownedGeoms) OGRGeometryFactory::destroyGeometry(g);
      throw std::runtime_error("ShapeSource: GDALRasterizeGeometries failed");
    }
  }

  for (auto* g : ownedGeoms) OGRGeometryFactory::destroyGeometry(g);

  // Read the burned raster back into our flat buffer.
  itsGrid.resize(itsNx * itsNy);
  CPLErr err = mem->GetRasterBand(1)->RasterIO(
      GF_Read, 0, 0, static_cast<int>(itsNx), static_cast<int>(itsNy),
      itsGrid.data(), static_cast<int>(itsNx), static_cast<int>(itsNy),
      GDT_UInt16, 0, 0);
  if (err != CE_None)
    throw std::runtime_error("ShapeSource: failed to read back rasterised grid");
}

ShapeSource::~ShapeSource() = default;

std::vector<int> ShapeSource::paramIds() const { return {1}; }
std::string ShapeSource::paramShortName(int /*paramId*/) const
{
  return itsLayerName.empty() ? std::string{"Shape"} : itsLayerName;
}
std::string ShapeSource::paramLongName(int paramId) const { return paramShortName(paramId); }
std::string ShapeSource::paramUnits(int /*paramId*/) const { return {}; }
int ShapeSource::currentParamId() const { return 1; }
bool ShapeSource::selectParamId(int paramId) { return paramId == 1; }

std::size_t ShapeSource::timeCount() const { return 1; }
std::size_t ShapeSource::currentTimeIndex() const { return 0; }
void ShapeSource::selectTimeIndex(std::size_t /*i*/) {}
NFmiMetTime ShapeSource::currentValidTime() const { return NFmiMetTime(); }
NFmiMetTime ShapeSource::originTime() const { return NFmiMetTime(); }

std::size_t ShapeSource::levelCount() const { return 1; }
std::size_t ShapeSource::currentLevelIndex() const { return 0; }
void ShapeSource::selectLevelIndex(std::size_t /*i*/) {}
float ShapeSource::levelValueAt(std::size_t /*i*/) const { return 0; }

float ShapeSource::interpolatedValue(double lat, double lon) const
{
  if (itsGrid.empty() || itsPixelW == 0 || itsPixelH == 0)
    return std::numeric_limits<float>::quiet_NaN();
  // Inverse of the GeoTransform (lon ≡ x, lat ≡ y).
  const double col = (lon - itsOriginX) / itsPixelW;
  const double row = (lat - itsOriginY) / itsPixelH;
  if (col < 0 || row < 0 || col >= static_cast<double>(itsNx) ||
      row >= static_cast<double>(itsNy))
    return 0.0F;  // outside layer extent → "no feature"
  const auto i = static_cast<std::size_t>(col);
  const auto j = static_cast<std::size_t>(row);
  return static_cast<float>(itsGrid[j * itsNx + i]);
}

LatLonBox ShapeSource::boundingBox() const
{
  LatLonBox b;
  b.minLat = itsOriginY + static_cast<double>(itsNy) * itsPixelH;
  b.maxLat = itsOriginY;
  b.minLon = itsOriginX;
  b.maxLon = itsOriginX + static_cast<double>(itsNx) * itsPixelW;
  return b;
}

void ShapeSource::uvToLatLon(double u, double v, double& lat, double& lon) const
{
  if (!itsArea)
  {
    DataSource::uvToLatLon(u, v, lat, lon);
    return;
  }
  const NFmiPoint xy(u * itsArea->Width(), v * itsArea->Height());
  const NFmiPoint world = itsArea->XYToWorldXY(xy);
  const NFmiPoint ll = itsArea->WorldXYToLatLon(world);
  lat = ll.Y();
  lon = ll.X();
}

void ShapeSource::latLonToUV(double lat, double lon, double& u, double& v) const
{
  if (!itsArea)
  {
    DataSource::latLonToUV(lat, lon, u, v);
    return;
  }
  const NFmiPoint world = itsArea->LatLonToWorldXY(NFmiPoint(lon, lat));
  const NFmiPoint xy = itsArea->WorldXYToXY(world);
  const double w = itsArea->Width();
  const double h = itsArea->Height();
  u = w > 0 ? xy.X() / w : 0.0;
  v = h > 0 ? xy.Y() / h : 0.0;
}

std::vector<std::pair<std::string, std::string>> ShapeSource::extraMetadata() const
{
  std::vector<std::pair<std::string, std::string>> rows;
  rows.emplace_back("Format", "Shapefile");
  if (!itsLayerName.empty()) rows.emplace_back("Layer", itsLayerName);
  if (!itsGeometryType.empty()) rows.emplace_back("Geometry", itsGeometryType);
  rows.emplace_back("Features", std::to_string(itsFeatureCount));
  rows.emplace_back("Outlines", std::to_string(itsOutlines.size()));
  rows.emplace_back("Raster", std::to_string(itsNx) + "x" + std::to_string(itsNy));
  if (!itsFieldNames.empty())
  {
    std::string joined;
    for (const auto& n : itsFieldNames)
    {
      if (!joined.empty()) joined += ", ";
      joined += n;
    }
    rows.emplace_back("Fields", joined);
  }
  return rows;
}

std::string ShapeSource::gridSignature() const
{
  // Shapefiles aren't aggregated into time series, so the signature
  // mostly exists for symmetry. Layer name + raster dimensions are
  // distinctive enough for the rare case of two compatible shapes.
  return std::string("shp:") + itsLayerName + "|" + std::to_string(itsNx) + "x" +
         std::to_string(itsNy);
}

Palette ShapeSource::recommendedPalette() const
{
  if (itsOpts.rainbow || !itsOpts.colorByField.empty())
    return Palette::rainbowCycle(std::max(1, itsFeatureCount));
  return Palette::flatFill(itsOpts.fillColor);
}
}  // namespace Qdless
