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
#include <functional>
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
  init(ds->GetLayer(0));
}

ShapeSource::ShapeSource(OGRLayer* layer, const std::string& displayName)
    : ShapeSource(layer, displayName, Options{})
{
}

ShapeSource::ShapeSource(OGRLayer* layer, const std::string& displayName, Options opts)
    : itsFilename(displayName), itsOpts(std::move(opts))
{
  GDALAllRegister();  // idempotent — guards against PG-only callers
  if (layer == nullptr) throw std::runtime_error("ShapeSource: null layer");
  itsLayerNonOwned = layer;
  init(layer);
}

void ShapeSource::init(OGRLayer* layer)
{
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
  // Walk every feature; for each, walk its geometry tree down to leaf
  // Polygons (or single LineStrings) and assign a distinct burn id to
  // each one. This way a shapefile containing a single MultiPolygon
  // feature still gets one hue per sub-polygon under the rainbow
  // palette. Per-feature .dbf attributes are captured into
  // itsAttributes; itsBurnToFeature lets a click look back up the
  // original feature row.
  layer->ResetReading();
  // Pass 1 (this loop): collect every metadata side-effect — burn-id
  // map, .dbf attributes, centroid, outline polylines — and discard
  // the geometries when done. Subsequent rasterise() calls re-walk
  // the layer to refetch geometries on demand. The trade-off vs
  // caching is more I/O on each zoom step but no per-source memory
  // tied up in OGRGeometry copies; for huge PostGIS tables the
  // caching alternative was untenable.

  std::function<void(OGRGeometry*, int)> registerLeaves =
      [&](OGRGeometry* geom, int featureIdx)
  {
    if (geom == nullptr) return;
    const auto t = wkbFlatten(geom->getGeometryType());
    if (t == wkbPolygon || t == wkbLineString || t == wkbPoint)
    {
      itsBurnToFeature.push_back(featureIdx);
      ++itsBurnIdCount;
    }
    else if (auto* gc = geom->toGeometryCollection(); gc != nullptr)
    {
      for (int i = 0; i < gc->getNumGeometries(); ++i)
        registerLeaves(gc->getGeometryRef(i), featureIdx);
    }
  };

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

    // Capture .dbf attributes verbatim (no type conversion) so the
    // click-popup shows the file's own representation.
    std::vector<std::pair<std::string, std::string>> attrs;
    if (auto* defn = layer->GetLayerDefn())
    {
      attrs.reserve(static_cast<std::size_t>(defn->GetFieldCount()));
      for (int i = 0; i < defn->GetFieldCount(); ++i)
      {
        const char* name = defn->GetFieldDefn(i)->GetNameRef();
        const char* val = feat->GetFieldAsString(i);
        attrs.emplace_back(name ? name : "", val ? val : "");
      }
    }
    itsAttributes.push_back(std::move(attrs));
    // Capture the feature's bbox centre (already in WGS84 after the
    // optional reproject) as the centroid for marker placement. True
    // centroid via OGR_G_Centroid would be accurate but expensive on
    // a 50k-vertex MultiPolygon; bbox-centre is fine for dropping a
    // pin in the right neighbourhood.
    OGREnvelope env;
    clone->getEnvelope(&env);
    itsCentroids.emplace_back((env.MinY + env.MaxY) / 2.0,
                              (env.MinX + env.MaxX) / 2.0);
    const int featureIdx = itsFeatureCount++;
    registerLeaves(clone, featureIdx);
    OGRGeometryFactory::destroyGeometry(clone);
  }

  // Build the base raster covering the entire envelope. The same
  // helper is reused by prepareViewport() for zoom-time refinement.
  itsGrid = rasterise(itsNx, itsNy, minLon, minLat, maxLon, maxLat);
}

ShapeSource::~ShapeSource() = default;

namespace
{
// Re-walk an OGR layer, optionally reproject every feature into
// `dst`, and append (geom, burn) pairs in a deterministic order
// matching ShapeSource::init's first walk. Caller owns the cloned
// geometries — destroyed via OGRGeometryFactory::destroyGeometry
// after rasterisation.
void collectLeavesForRaster(OGRLayer* layer, OGRSpatialReference* dst,
                            std::vector<OGRGeometry*>& geoms,
                            std::vector<double>& burnValues)
{
  if (layer == nullptr) return;
  std::unique_ptr<OGRCoordinateTransformation> ct;
  if (auto* src = layer->GetSpatialRef(); src != nullptr && !src->IsSame(dst))
  {
    auto* clone = src->Clone();
    clone->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    ct.reset(OGRCreateCoordinateTransformation(clone, dst));
    OGRSpatialReference::DestroySpatialReference(clone);
  }
  layer->ResetReading();
  int nextBurn = 1;

  std::function<void(OGRGeometry*)> registerLeaves =
      [&](OGRGeometry* geom)
  {
    if (geom == nullptr) return;
    const auto t = wkbFlatten(geom->getGeometryType());
    if (t == wkbPolygon || t == wkbLineString || t == wkbPoint)
    {
      const double v = static_cast<double>(((nextBurn - 1) % 65535) + 1);
      geoms.push_back(geom->clone());
      burnValues.push_back(v);
      ++nextBurn;
    }
    else if (auto* gc = geom->toGeometryCollection(); gc != nullptr)
    {
      for (int i = 0; i < gc->getNumGeometries(); ++i)
        registerLeaves(gc->getGeometryRef(i));
    }
  };

  while (auto* feat = layer->GetNextFeature())
  {
    OGRGeometry* g = feat->GetGeometryRef();
    if (g != nullptr)
    {
      OGRGeometry* clone = g->clone();
      if (ct && clone->transform(ct.get()) != OGRERR_NONE)
      {
        OGRGeometryFactory::destroyGeometry(clone);
        OGRFeature::DestroyFeature(feat);
        continue;
      }
      registerLeaves(clone);
      OGRGeometryFactory::destroyGeometry(clone);
    }
    OGRFeature::DestroyFeature(feat);
  }
}
}  // namespace

std::vector<std::uint16_t> ShapeSource::rasterise(std::size_t nx, std::size_t ny,
                                                  double minX, double minY,
                                                  double maxX, double maxY) const
{
  std::vector<std::uint16_t> out(nx * ny, 0);

  // Re-walk the source on each rasterise. The OGRLayer pointer is
  // valid for the App's lifetime (PostGIS) or we re-open the file
  // (shapefile path). For huge PostGIS tables this means each zoom
  // step costs one query — acceptable for an interactive viewer.
  GDALDataset* tempDs = nullptr;
  OGRLayer* layer = itsLayerNonOwned;
  if (layer == nullptr)
  {
    tempDs = static_cast<GDALDataset*>(GDALOpenEx(itsFilename.c_str(),
                                                  GDAL_OF_VECTOR | GDAL_OF_READONLY,
                                                  nullptr, nullptr, nullptr));
    if (tempDs == nullptr) return out;
    if (tempDs->GetLayerCount() < 1)
    {
      GDALClose(tempDs);
      return out;
    }
    layer = tempDs->GetLayer(0);
  }
  struct DsGuard
  {
    GDALDataset* p;
    ~DsGuard() { if (p) GDALClose(p); }
  } dsGuard{tempDs};

  OGRSpatialReference dst;
  dst.SetWellKnownGeogCS("WGS84");
  dst.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  std::vector<OGRGeometry*> geoms;
  std::vector<double> burnValues;
  collectLeavesForRaster(layer, &dst, geoms, burnValues);
  if (geoms.empty()) return out;

  GDALDriver* memDrv = GetGDALDriverManager()->GetDriverByName("MEM");
  if (memDrv == nullptr)
  {
    for (auto* g : geoms) OGRGeometryFactory::destroyGeometry(g);
    throw std::runtime_error("ShapeSource::rasterise: MEM missing");
  }
  auto* mem = memDrv->Create("", static_cast<int>(nx), static_cast<int>(ny), 1,
                             GDT_UInt16, nullptr);
  if (mem == nullptr)
  {
    for (auto* g : geoms) OGRGeometryFactory::destroyGeometry(g);
    throw std::runtime_error("ShapeSource::rasterise: MEM Create failed");
  }
  struct MemGuard
  {
    GDALDataset* p;
    ~MemGuard() { if (p) GDALClose(p); }
  } memGuard{mem};
  const double pxW = (maxX - minX) / static_cast<double>(nx);
  const double pxH = (minY - maxY) / static_cast<double>(ny);
  double gt[6] = {minX, pxW, 0.0, maxY, 0.0, pxH};
  mem->SetGeoTransform(gt);

  std::vector<OGRGeometryH> handles;
  handles.reserve(geoms.size());
  for (auto* g : geoms) handles.push_back(static_cast<OGRGeometryH>(g));

  int bandList[1] = {1};
  char** rasterOpts = nullptr;
  rasterOpts = CSLSetNameValue(rasterOpts, "ALL_TOUCHED", "TRUE");
  CPLErr err = GDALRasterizeGeometries(
      mem, 1, bandList, static_cast<int>(handles.size()), handles.data(),
      nullptr, nullptr, burnValues.data(), rasterOpts, nullptr, nullptr);
  CSLDestroy(rasterOpts);
  for (auto* g : geoms) OGRGeometryFactory::destroyGeometry(g);
  if (err != CE_None)
    throw std::runtime_error("ShapeSource::rasterise: GDALRasterizeGeometries failed");

  err = mem->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, static_cast<int>(nx),
                                        static_cast<int>(ny), out.data(),
                                        static_cast<int>(nx), static_cast<int>(ny),
                                        GDT_UInt16, 0, 0);
  if (err != CE_None)
    throw std::runtime_error("ShapeSource::rasterise: read-back failed");
  return out;
}


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
  // Try the zoom raster first when populated — it covers a smaller
  // window at higher resolution. Falls back to the base raster for
  // anything outside the zoom bbox or when no zoom is active.
  if (itsZoomNx > 0 && itsZoomPixelW != 0 && itsZoomPixelH != 0)
  {
    const double col = (lon - itsZoomOriginX) / itsZoomPixelW;
    const double row = (lat - itsZoomOriginY) / itsZoomPixelH;
    if (col >= 0 && row >= 0 && col < static_cast<double>(itsZoomNx) &&
        row < static_cast<double>(itsZoomNy))
    {
      const auto i = static_cast<std::size_t>(col);
      const auto j = static_cast<std::size_t>(row);
      return static_cast<float>(itsZoomGrid[j * itsZoomNx + i]);
    }
  }
  if (itsGrid.empty() || itsPixelW == 0 || itsPixelH == 0)
    return std::numeric_limits<float>::quiet_NaN();
  const double col = (lon - itsOriginX) / itsPixelW;
  const double row = (lat - itsOriginY) / itsPixelH;
  if (col < 0 || row < 0 || col >= static_cast<double>(itsNx) ||
      row >= static_cast<double>(itsNy))
    return 0.0F;
  const auto i = static_cast<std::size_t>(col);
  const auto j = static_cast<std::size_t>(row);
  return static_cast<float>(itsGrid[j * itsNx + i]);
}

void ShapeSource::prepareViewport(const LatLonBox& bbox, int cellsX, int cellsY) const
{
  if (itsBurnIdCount == 0) return;
  if (cellsX <= 0 || cellsY <= 0) return;

  // What's the visible bbox in WGS84? The viewport's lat/lon span
  // tells us the source-pixel size we'd need to match the screen.
  const double lonSpan = bbox.maxLon - bbox.minLon;
  const double latSpan = bbox.maxLat - bbox.minLat;
  if (lonSpan <= 0 || latSpan <= 0) return;

  // Refinement threshold: if the base-raster pixel size is finer
  // than what one screen cell covers, the existing grid already has
  // enough detail and a zoom raster wouldn't improve the look.
  // Compare in degrees of longitude (the dominant axis for our
  // typical mid-latitude shapefiles).
  const double basePxLon = std::abs(itsPixelW);
  const double screenPxLon = lonSpan / static_cast<double>(cellsX);
  if (basePxLon <= screenPxLon)
  {
    // Base resolution already adequate — drop any stale zoom raster
    // so we don't keep memory tied up.
    itsZoomGrid.clear();
    itsZoomNx = itsZoomNy = 0;
    return;
  }

  // Pick a target zoom resolution that gives at least screen scale
  // plus a 2× headroom so a small pan keeps us on the cached
  // raster. Don't exceed itsOpts.rasterMax in either dimension —
  // extreme zoom-ins would otherwise allocate huge buffers.
  const int desiredX = std::min(static_cast<int>(itsOpts.rasterMax),
                                std::max(cellsX * 2, 256));
  const int desiredY = std::min(static_cast<int>(itsOpts.rasterMax),
                                std::max(cellsY * 2, 256));

  // Expand the viewport bbox by 25% on each side so panning within
  // a small region doesn't trigger constant re-rasterisation. The
  // expansion is clamped to the source's own bbox.
  const double padLon = lonSpan * 0.25;
  const double padLat = latSpan * 0.25;
  double minX = bbox.minLon - padLon;
  double maxX = bbox.maxLon + padLon;
  double minY = bbox.minLat - padLat;
  double maxY = bbox.maxLat + padLat;
  // Clamp to the source extent so we never rasterise empty space
  // beyond what the geometries actually cover.
  const double srcMinX = itsOriginX;
  const double srcMaxX = itsOriginX + static_cast<double>(itsNx) * itsPixelW;
  const double srcMinY = itsOriginY + static_cast<double>(itsNy) * itsPixelH;
  const double srcMaxY = itsOriginY;
  minX = std::max(minX, srcMinX);
  maxX = std::min(maxX, srcMaxX);
  minY = std::max(minY, srcMinY);
  maxY = std::min(maxY, srcMaxY);
  if (maxX <= minX || maxY <= minY) return;

  // Skip the work if the existing zoom raster already covers the
  // requested bbox at sufficient resolution. "Covers" = the new
  // bbox is entirely inside the cached one and the cached
  // pixel-per-degree is at least as fine.
  if (itsZoomNx > 0)
  {
    const double zMinX = itsZoomOriginX;
    const double zMaxX = itsZoomOriginX + static_cast<double>(itsZoomNx) * itsZoomPixelW;
    const double zMinY = itsZoomOriginY + static_cast<double>(itsZoomNy) * itsZoomPixelH;
    const double zMaxY = itsZoomOriginY;
    if (bbox.minLon >= zMinX && bbox.maxLon <= zMaxX && bbox.minLat >= zMinY &&
        bbox.maxLat <= zMaxY && std::abs(itsZoomPixelW) <= screenPxLon * 1.5)
      return;
  }

  itsZoomGrid = rasterise(static_cast<std::size_t>(desiredX),
                          static_cast<std::size_t>(desiredY), minX, minY, maxX, maxY);
  itsZoomNx = static_cast<std::size_t>(desiredX);
  itsZoomNy = static_cast<std::size_t>(desiredY);
  itsZoomOriginX = minX;
  itsZoomOriginY = maxY;
  itsZoomPixelW = (maxX - minX) / static_cast<double>(itsZoomNx);
  itsZoomPixelH = (minY - maxY) / static_cast<double>(itsZoomNy);
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
  rows.emplace_back("Polygons", std::to_string(itsBurnIdCount));
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
  if (itsOpts.rainbow || !itsOpts.colorByField.empty()) return rainbowPalette();
  return Palette::flatFill(itsOpts.fillColor);
}

Palette ShapeSource::rainbowPalette() const
{
  // 1. Find the burn ids that ended up with at least one cell in the
  //    rasterised grid. Sub-pixel polygons (often the trailing
  //    children of a MultiPolygon) won't appear and shouldn't be in
  //    the legend.
  std::vector<bool> seen(static_cast<std::size_t>(itsBurnIdCount + 1), false);
  for (auto v : itsGrid)
    if (v != 0 && v <= itsBurnIdCount) seen[v] = true;

  std::vector<int> usedIds;
  usedIds.reserve(static_cast<std::size_t>(itsBurnIdCount));
  for (int i = 1; i <= itsBurnIdCount; ++i)
    if (seen[static_cast<std::size_t>(i)]) usedIds.push_back(i);

  // 2. Build a band per used id with the same golden-angle hue cycle
  //    rainbowCycle uses, plus a label from the feature's
  //    NAME/NIMI/first text field. The hue index is the position
  //    within usedIds, not the raw burn id, so adjacent visible
  //    polygons get adjacent (= maximally distinct) hues even when
  //    several invisible burn ids fall between them.
  Palette p;
  // Hand-roll because we need labels + non-contiguous integer values.
  // Replicates Palette::rainbowCycle's HSV → RGB conversion.
  auto hsvToRgb = [](float hueTurns, float s, float v) -> Rgb {
    const float h = hueTurns - std::floor(hueTurns);
    const int i = static_cast<int>(h * 6.0F);
    const float f = h * 6.0F - i;
    const float p1 = v * (1 - s);
    const float q = v * (1 - f * s);
    const float t = v * (1 - (1 - f) * s);
    float r = 0;
    float g = 0;
    float b = 0;
    switch (i % 6)
    {
      case 0: r = v; g = t; b = p1; break;
      case 1: r = q; g = v; b = p1; break;
      case 2: r = p1; g = v; b = t; break;
      case 3: r = p1; g = q; b = v; break;
      case 4: r = t; g = p1; b = v; break;
      default: r = v; g = p1; b = q; break;
    }
    return Rgb{static_cast<std::uint8_t>(r * 255),
               static_cast<std::uint8_t>(g * 255),
               static_cast<std::uint8_t>(b * 255)};
  };
  constexpr float kGolden = 0.61803398875F;

  // Field-name preference for the label: NIMI / NAME (case-insensitive),
  // then any other text-looking field. Computed once.
  auto pickField = [&](const std::vector<std::pair<std::string, std::string>>& attrs) -> std::string {
    auto eqIcase = [](const std::string& a, const char* b) {
      if (a.size() != std::strlen(b)) return false;
      for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
          return false;
      return true;
    };
    for (const char* want : {"nimi", "name", "label", "title"})
      for (const auto& [k, v] : attrs)
        if (eqIcase(k, want) && !v.empty()) return v;
    // Fallback: first non-numeric, non-empty value (avoids returning
    // the area in km² as a "name").
    for (const auto& [k, v] : attrs)
    {
      if (v.empty()) continue;
      bool numeric = true;
      for (char c : v)
        if (!(std::isdigit(static_cast<unsigned char>(c)) || c == '.' || c == ',' ||
              c == '-' || c == ' '))
        {
          numeric = false;
          break;
        }
      if (!numeric) return v;
    }
    return {};
  };

  // 3. Resolve a label (NAME/NIMI/…) for each used burn id. Multiple
  //    burn ids can map to the same feature (a MultiPolygon's
  //    children) or to different features that share a name in the
  //    .dbf — both should render in the same hue and dedupe to a
  //    single legend entry.
  std::vector<std::string> labelFor(usedIds.size());
  for (std::size_t i = 0; i < usedIds.size(); ++i)
  {
    const int id = usedIds[i];
    const int featureIdx = (id - 1) < static_cast<int>(itsBurnToFeature.size())
                               ? itsBurnToFeature[static_cast<std::size_t>(id - 1)]
                               : -1;
    std::string label;
    if (featureIdx >= 0 && static_cast<std::size_t>(featureIdx) < itsAttributes.size())
      label = pickField(itsAttributes[static_cast<std::size_t>(featureIdx)]);
    if (label.empty()) label = "#" + std::to_string(id);
    labelFor[i] = std::move(label);
  }

  // 4. Assign one hue per UNIQUE label. Hues cycle by golden angle in
  //    label-encounter order, so adjacent unique labels look maximally
  //    different. Burn ids that share a label all reuse the same hue.
  std::map<std::string, Rgb> hueByLabel;
  std::size_t nextHueIdx = 0;
  for (const auto& lab : labelFor)
  {
    if (hueByLabel.count(lab) != 0U) continue;
    const float hue = std::fmod(0.05F + static_cast<float>(nextHueIdx) * kGolden, 1.0F);
    hueByLabel[lab] = hsvToRgb(hue, 0.7F, 0.9F);
    ++nextHueIdx;
  }

  // 5. Build one band per burn id (Palette::lookup needs them all,
  //    one per integer value). Bands sharing a label share the rgb
  //    from hueByLabel; popupLegend dedupes by label so the user
  //    sees one row per unique area.
  p = Palette{};
  p.setName("shape-rainbow");
  for (std::size_t i = 0; i < usedIds.size(); ++i)
  {
    const int id = usedIds[i];
    Palette::Band band;
    band.lo = static_cast<float>(id) - 0.5F;
    band.hi = static_cast<float>(id) + 0.5F;
    band.rgb = hueByLabel[labelFor[i]];
    band.label = labelFor[i];
    p.bands().push_back(std::move(band));
  }
  return p;
}

const std::vector<std::pair<std::string, std::string>>& ShapeSource::featureAttributes(
    int idx) const
{
  static const std::vector<std::pair<std::string, std::string>> kEmpty;
  if (idx < 0 || static_cast<std::size_t>(idx) >= itsAttributes.size()) return kEmpty;
  return itsAttributes[static_cast<std::size_t>(idx)];
}

std::pair<double, double> ShapeSource::featureCentroid(int idx) const
{
  if (idx < 0 || static_cast<std::size_t>(idx) >= itsCentroids.size()) return {0.0, 0.0};
  return itsCentroids[static_cast<std::size_t>(idx)];
}

std::vector<std::pair<std::string, std::string>> ShapeSource::attributesAt(double lat,
                                                                           double lon) const
{
  if (itsGrid.empty() || itsPixelW == 0 || itsPixelH == 0) return {};
  const double col = (lon - itsOriginX) / itsPixelW;
  const double row = (lat - itsOriginY) / itsPixelH;
  if (col < 0 || row < 0 || col >= static_cast<double>(itsNx) ||
      row >= static_cast<double>(itsNy))
    return {};
  const auto i = static_cast<std::size_t>(col);
  const auto j = static_cast<std::size_t>(row);
  const auto burn = itsGrid[j * itsNx + i];
  if (burn == 0) return {};
  // burn ids are 1-based and may have wrapped past 65535; the
  // lookup table is the authoritative mapping back to the original
  // feature index.
  const std::size_t idx = (static_cast<std::size_t>(burn) - 1) % itsBurnToFeature.size();
  const int featureIdx = itsBurnToFeature[idx];
  if (featureIdx < 0 || static_cast<std::size_t>(featureIdx) >= itsAttributes.size())
    return {};
  return itsAttributes[static_cast<std::size_t>(featureIdx)];
}
}  // namespace Qdless
