#include "QdlessGeoTiffSource.h"

#include <cpl_conv.h>
#include <cpl_error.h>
#include <gdal_priv.h>
#include <gis/SpatialReference.h>
#include <newbase/NFmiArea.h>
#include <newbase/NFmiEnumConverter.h>
#include <newbase/NFmiPoint.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <map>
#include <regex>
#include <stdexcept>

namespace Qdless
{
namespace
{
// Parse "YYYYMMDDhhmm" or "YYYYMMDDhhmmss" → NFmiMetTime (UTC). Returns an
// invalid (year=0) time on failure so callers can chain fallbacks.
NFmiMetTime parseUtcStamp(const std::string& s)
{
  if (s.size() < 12) return NFmiMetTime();
  try
  {
    short yy = static_cast<short>(std::stoi(s.substr(0, 4)));
    short mm = static_cast<short>(std::stoi(s.substr(4, 2)));
    short dd = static_cast<short>(std::stoi(s.substr(6, 2)));
    short h = static_cast<short>(std::stoi(s.substr(8, 2)));
    short mi = static_cast<short>(std::stoi(s.substr(10, 2)));
    short se = (s.size() >= 14) ? static_cast<short>(std::stoi(s.substr(12, 2))) : 0;
    // 1-minute time step — NFmiMetTime's 60-minute default snaps
    // sub-hourly timestamps to the nearest hour. NearestMetTime also
    // hardcodes SetSec(0); re-apply seconds afterwards.
    NFmiMetTime r(yy, mm, dd, h, mi, /*sec=*/0, /*timeStep=*/1);
    r.SetSec(se);
    return r;
  }
  catch (...)
  {
    return NFmiMetTime();
  }
}

NFmiMetTime parseTimeFromName(const std::string& filename)
{
  const std::string base = std::filesystem::path(filename).filename().string();
  // Match a 12- or 14-digit timestamp anywhere in the basename so files
  // named `<prefix>_<timestamp>_<suffix>.tif` work the same as those
  // with the timestamp at the start.
  static const std::regex re(R"((\d{12,14}))");
  std::smatch m;
  if (std::regex_search(base, m, re))
    return parseUtcStamp(m[1]);
  std::error_code ec;
  auto ftime = std::filesystem::last_write_time(filename, ec);
  if (ec) return NFmiMetTime();
  const auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
      ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
  const auto t = std::chrono::system_clock::to_time_t(sctp);
  std::tm utc{};
  gmtime_r(&t, &utc);
  return NFmiMetTime(static_cast<short>(utc.tm_year + 1900),
                     static_cast<short>(utc.tm_mon + 1),
                     static_cast<short>(utc.tm_mday),
                     static_cast<short>(utc.tm_hour),
                     static_cast<short>(utc.tm_min),
                     static_cast<short>(utc.tm_sec));
}

std::string extractLabel(const std::string& filename)
{
  std::string base = std::filesystem::path(filename).stem().string();
  static const std::regex re(R"(^\d{12,14}_(.*)$)");
  std::smatch m;
  if (std::regex_search(base, m, re)) return m[1];
  return base;
}

// Parse a GDAL_METADATA-style XML blob into (name → {value, unit}). Format:
//   <GDALMetadata><Item name="..." [unit="..."] [format="..."]>VALUE</Item>...</GDALMetadata>
// We don't bring in a full XML parser — the format is rigid and produced by
// a single GDAL writer, so a regex on Item elements is robust enough. The
// only quirk handled is unescaping the standard XML entities GDAL emits.
struct MetaItem
{
  std::string value;
  std::string unit;
};

std::string unescapeXml(std::string s)
{
  static const std::pair<std::string, std::string> kEntities[] = {
      {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}};
  for (const auto& [from, to] : kEntities)
  {
    std::string::size_type pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos)
    {
      s.replace(pos, from.size(), to);
      pos += to.size();
    }
  }
  return s;
}

std::map<std::string, MetaItem> parseGdalMetadata(const std::string& xml)
{
  std::map<std::string, MetaItem> out;
  if (xml.empty()) return out;
  // Match `<Item name="…"  unit="…">value</Item>` with optional unit.
  // Using a relaxed pattern: name attr is required; unit is captured if
  // present anywhere in the open tag; value is everything until </Item>.
  static const std::regex itemRe(
      R"(<Item\s+([^>]*?)>([^<]*)</Item>)", std::regex::ECMAScript);
  // Use a custom raw-string delimiter (`x`) so the regex's literal `"` and
  // `)` don't collide with the closing `)"` of an unadorned raw string.
  static const std::regex nameRe(R"x(name="([^"]*)")x");
  static const std::regex unitRe(R"x(unit="([^"]*)")x");
  auto begin = std::sregex_iterator(xml.begin(), xml.end(), itemRe);
  for (auto it = begin; it != std::sregex_iterator(); ++it)
  {
    const std::string attrs = (*it)[1].str();
    const std::string value = unescapeXml((*it)[2].str());
    std::smatch nm;
    if (!std::regex_search(attrs, nm, nameRe)) continue;
    MetaItem mi;
    mi.value = value;
    std::smatch um;
    if (std::regex_search(attrs, um, unitRe)) mi.unit = um[1].str();
    out[nm[1].str()] = std::move(mi);
  }
  return out;
}

// FMI's "Quantity" field is a long human label (e.g. "Precipitation
// accumulation"). Map it to a newbase ID when the match is unambiguous.
int quantityToParamId(const std::string& quantity)
{
  std::string q = quantity;
  std::transform(q.begin(), q.end(), q.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  if (q.find("precipitation accumulation") != std::string::npos ||
      q.find("precipitation amount") != std::string::npos ||
      q.find("rainfall accumulation") != std::string::npos)
    return kFmiPrecipitationAmount;
  if (q.find("precipitation rate") != std::string::npos ||
      q.find("rainfall rate") != std::string::npos)
    return kFmiPrecipitationRate;
  if (q.find("reflectivity") != std::string::npos) return kFmiReflectivity;
  if (q.find("echo top") != std::string::npos) return kFmiEchoTop;
  if (q.find("radial velocity") != std::string::npos) return kFmiRadialVelocity;
  return 0;
}

int labelToParamId(const std::string& label)
{
  if (label.find("rr1h") != std::string::npos || label.find("rr3h") != std::string::npos ||
      label.find("rr12h") != std::string::npos || label.find("rr24h") != std::string::npos ||
      label.find("rrate") != std::string::npos)
    return kFmiPrecipitationAmount;
  if (label.find("dbz") != std::string::npos || label.find("refl") != std::string::npos)
    return kFmiReflectivity;
  return 0;
}
}  // namespace

GeoTiffSource::GeoTiffSource(const std::string& filename) : itsFilename(filename)
{
  GDALAllRegister();
  auto* ds = static_cast<GDALDataset*>(GDALOpen(filename.c_str(), GA_ReadOnly));
  if (!ds) throw std::runtime_error("GDAL failed to open: " + filename);
  struct Guard
  {
    GDALDataset* p;
    ~Guard() { if (p) GDALClose(p); }
  } guard{ds};

  itsNx = static_cast<std::size_t>(ds->GetRasterXSize());
  itsNy = static_cast<std::size_t>(ds->GetRasterYSize());
  if (itsNx == 0 || itsNy == 0) throw std::runtime_error("empty raster: " + filename);

  double gt[6] = {};
  if (ds->GetGeoTransform(gt) != CE_None)
    throw std::runtime_error("no geotransform: " + filename);
  if (std::abs(gt[2]) > 1e-9 || std::abs(gt[4]) > 1e-9)
    throw std::runtime_error("rotated GeoTIFFs are not supported: " + filename);
  itsOriginX = gt[0];
  itsPixelW = gt[1];
  itsOriginY = gt[3];
  itsPixelH = gt[5];

  const OGRSpatialReference* osr = ds->GetSpatialRef();
  if (!osr) throw std::runtime_error("no projection in GeoTIFF: " + filename);
  char* wkt = nullptr;
  osr->exportToWkt(&wkt);
  if (wkt) { itsWkt = wkt; CPLFree(wkt); }
  Fmi::SpatialReference sr(*osr);

  const double x0 = itsOriginX;
  const double y0 = itsOriginY;
  const double x1 = itsOriginX + static_cast<double>(itsNx) * itsPixelW;
  const double y1 = itsOriginY + static_cast<double>(itsNy) * itsPixelH;
  const double minX = std::min(x0, x1);
  const double maxX = std::max(x0, x1);
  const double minY = std::min(y0, y1);
  const double maxY = std::max(y0, y1);
  // CreateFromBBox computes corner lat/lons internally; for projections
  // whose valid latitude range falls inside the raster bbox (TM35FIN over
  // a 7700×7700 km canvas reaches latitudes PROJ rejects) PROJ logs to
  // stderr. Silence that — we'll discover any genuine projection failure
  // through the null check below.
  CPLPushErrorHandler(CPLQuietErrorHandler);
  itsArea.reset(NFmiArea::CreateFromBBox(sr, NFmiPoint(minX, minY), NFmiPoint(maxX, maxY)));
  CPLPopErrorHandler();
  if (!itsArea) throw std::runtime_error("failed to construct projection for " + filename);

  // FMI radar GeoTIFFs (and many other producer outputs) carry an XML blob
  // in TIFF tag 42112 — `Observation time`, `Quantity`, `Gain`, `Offset`,
  // `Nodata`, `Undetect`. GDAL does NOT split this into individual metadata
  // items, so we read the raw key and parse it ourselves.
  std::map<std::string, MetaItem> meta;
  if (const char* blob = ds->GetMetadataItem("GDAL_METADATA"))
    meta = parseGdalMetadata(blob);

  // Reading order for valid time:
  //   1. <Item name="Observation time"> — preferred, authoritative
  //   2. Leading YYYYMMDDhhmm in basename
  //   3. mtime
  if (auto it = meta.find("Observation time"); it != meta.end())
    itsValidTime = parseUtcStamp(it->second.value);
  if (itsValidTime.GetYear() == 0)
    itsValidTime = parseTimeFromName(filename);

  itsLabel = extractLabel(filename);

  // Param naming: prefer the "Quantity" attribute (human-readable, with
  // an explicit unit), fall back to filename label, fall back to "Value".
  if (auto it = meta.find("Quantity"); it != meta.end())
  {
    itsParamName = it->second.value;
    itsParamUnits = it->second.unit;
    itsParamId = quantityToParamId(itsParamName);
  }
  else
  {
    itsParamId = labelToParamId(itsLabel);
    if (itsParamId == 0)
    {
      NFmiEnumConverter conv;
      int id = conv.ToEnum(itsLabel.c_str());
      if (id != 0) itsParamId = id;
    }
    if (itsParamId != 0)
    {
      NFmiEnumConverter conv;
      itsParamName = conv.ToString(itsParamId);
    }
    else
    {
      itsParamName = itsLabel.empty() ? std::string{"Value"} : itsLabel;
    }
  }

  // Gain / offset / nodata / undetect.
  auto fetchDouble = [&](const char* key, double& out, bool& has) {
    auto it = meta.find(key);
    if (it == meta.end()) return;
    try
    {
      out = std::stod(it->second.value);
      has = true;
    }
    catch (...)
    {
    }
  };
  bool hasGain = false;
  bool hasOffset = false;
  fetchDouble("Gain", itsGain, hasGain);
  fetchDouble("Offset", itsOffset, hasOffset);
  fetchDouble("Nodata", itsNodata, itsHasNodata);
  fetchDouble("Undetect", itsUndetect, itsHasUndetect);
  if (!hasGain) itsGain = 1.0;
  if (!hasOffset) itsOffset = 0.0;

  // Fall back to band-level GDAL nodata if metadata didn't supply one.
  if (!itsHasNodata)
  {
    int has = 0;
    double v = ds->GetRasterBand(1)->GetNoDataValue(&has);
    if (has)
    {
      itsNodata = v;
      itsHasNodata = true;
    }
  }

  // Optional descriptive fields for the metadata popup.
  if (auto it = meta.find("Temporal type"); it != meta.end())
    itsTemporalType = it->second.value;
  if (auto it = meta.find("Accumulation time"); it != meta.end())
    itsAccumulation = it->second.value + (it->second.unit.empty() ? "" : " " + it->second.unit);

  GDALRasterBand* band = ds->GetRasterBand(1);
  if (!band) throw std::runtime_error("no raster band: " + filename);

  std::vector<float> raw(itsNx * itsNy);
  CPLErr err = band->RasterIO(GF_Read, 0, 0, static_cast<int>(itsNx), static_cast<int>(itsNy),
                              raw.data(), static_cast<int>(itsNx), static_cast<int>(itsNy),
                              GDT_Float32, 0, 0);
  if (err != CE_None) throw std::runtime_error("RasterIO failed: " + filename);

  itsValues.resize(raw.size());
  const float nan = std::numeric_limits<float>::quiet_NaN();
  for (std::size_t i = 0; i < raw.size(); ++i)
  {
    const double v = raw[i];
    if ((itsHasNodata && v == itsNodata) || (itsHasUndetect && v == itsUndetect))
      itsValues[i] = nan;
    else
      itsValues[i] = static_cast<float>(itsGain * v + itsOffset);
  }
}

GeoTiffSource::~GeoTiffSource() = default;

std::vector<int> GeoTiffSource::paramIds() const
{
  return {itsParamId == 0 ? 1 : itsParamId};
}
std::string GeoTiffSource::paramShortName(int /*paramId*/) const { return itsParamName; }
std::string GeoTiffSource::paramLongName(int paramId) const { return paramShortName(paramId); }
std::string GeoTiffSource::paramUnits(int /*paramId*/) const { return itsParamUnits; }
int GeoTiffSource::currentParamId() const { return itsParamId == 0 ? 1 : itsParamId; }
bool GeoTiffSource::selectParamId(int paramId) { return paramId == currentParamId(); }

std::size_t GeoTiffSource::timeCount() const { return 1; }
std::size_t GeoTiffSource::currentTimeIndex() const { return 0; }
void GeoTiffSource::selectTimeIndex(std::size_t /*i*/) {}
NFmiMetTime GeoTiffSource::currentValidTime() const { return itsValidTime; }
NFmiMetTime GeoTiffSource::originTime() const { return itsValidTime; }

std::size_t GeoTiffSource::levelCount() const { return 1; }
std::size_t GeoTiffSource::currentLevelIndex() const { return 0; }
void GeoTiffSource::selectLevelIndex(std::size_t /*i*/) {}
float GeoTiffSource::levelValueAt(std::size_t /*i*/) const { return 0; }

float GeoTiffSource::interpolatedValue(double lat, double lon) const
{
  if (!itsArea || itsValues.empty())
    return std::numeric_limits<float>::quiet_NaN();
  const NFmiPoint world = itsArea->LatLonToWorldXY(NFmiPoint(lon, lat));
  if (itsPixelW == 0 || itsPixelH == 0)
    return std::numeric_limits<float>::quiet_NaN();
  const double col = (world.X() - itsOriginX) / itsPixelW;
  const double row = (world.Y() - itsOriginY) / itsPixelH;
  if (col < 0 || row < 0 || col >= static_cast<double>(itsNx) ||
      row >= static_cast<double>(itsNy))
    return std::numeric_limits<float>::quiet_NaN();
  const auto i = static_cast<std::size_t>(col);
  const auto j = static_cast<std::size_t>(row);
  return itsValues[j * itsNx + i];
}

void GeoTiffSource::uvToLatLon(double u, double v, double& lat, double& lon) const
{
  if (!itsArea)
  {
    DataSource::uvToLatLon(u, v, lat, lon);
    return;
  }
  // Suppress GDAL/PROJ stderr from points sampled outside the projection's
  // valid range during viewport/coastline math. We want to silently treat
  // those as out-of-grid rather than spam ERROR messages.
  CPLPushErrorHandler(CPLQuietErrorHandler);
  const NFmiPoint xy(u * itsArea->Width(), v * itsArea->Height());
  const NFmiPoint world = itsArea->XYToWorldXY(xy);
  const NFmiPoint ll = itsArea->WorldXYToLatLon(world);
  CPLPopErrorHandler();
  lat = ll.Y();
  lon = ll.X();
}

void GeoTiffSource::latLonToUV(double lat, double lon, double& u, double& v) const
{
  if (!itsArea)
  {
    DataSource::latLonToUV(lat, lon, u, v);
    return;
  }
  CPLPushErrorHandler(CPLQuietErrorHandler);
  const NFmiPoint world = itsArea->LatLonToWorldXY(NFmiPoint(lon, lat));
  const NFmiPoint xy = itsArea->WorldXYToXY(world);
  CPLPopErrorHandler();
  const double w = itsArea->Width();
  const double h = itsArea->Height();
  u = w > 0 ? xy.X() / w : 0.0;
  v = h > 0 ? xy.Y() / h : 0.0;
}

LatLonBox GeoTiffSource::boundingBox() const
{
  LatLonBox b;
  if (!itsArea) return b;
  b.minLat = std::numeric_limits<double>::infinity();
  b.maxLat = -std::numeric_limits<double>::infinity();
  b.minLon = std::numeric_limits<double>::infinity();
  b.maxLon = -std::numeric_limits<double>::infinity();
  const double w = itsArea->Width();
  const double h = itsArea->Height();
  // Edge sampling can land outside the projection's valid latitude range
  // (e.g. EPSG:3067 / TM35FIN sweeps a corner that PROJ rejects). Push a
  // quiet error handler around the whole loop instead of having PROJ spam
  // stderr for each rejected point.
  CPLPushErrorHandler(CPLQuietErrorHandler);
  auto add = [&](double x, double y) {
    NFmiPoint world = itsArea->XYToWorldXY(NFmiPoint(x, y));
    NFmiPoint ll = itsArea->WorldXYToLatLon(world);
    b.minLat = std::min(b.minLat, ll.Y());
    b.maxLat = std::max(b.maxLat, ll.Y());
    b.minLon = std::min(b.minLon, ll.X());
    b.maxLon = std::max(b.maxLon, ll.X());
  };
  constexpr int kSamples = 60;
  for (int i = 0; i <= kSamples; ++i)
  {
    const double t = static_cast<double>(i) / kSamples;
    add(0, t * h);
    add(w, t * h);
    add(t * w, 0);
    add(t * w, h);
  }
  CPLPopErrorHandler();
  return b;
}

std::vector<std::pair<std::string, std::string>> GeoTiffSource::extraMetadata() const
{
  std::vector<std::pair<std::string, std::string>> rows;
  rows.emplace_back("Format", "GeoTIFF");
  rows.emplace_back("Grid size", std::to_string(itsNx) + "x" + std::to_string(itsNy));
  if (!itsLabel.empty()) rows.emplace_back("Label", itsLabel);
  if (!itsParamUnits.empty()) rows.emplace_back("Unit", itsParamUnits);
  if (!itsAccumulation.empty()) rows.emplace_back("Accumulation", itsAccumulation);
  if (!itsTemporalType.empty()) rows.emplace_back("Temporal type", itsTemporalType);
  rows.emplace_back("Gain", std::to_string(itsGain));
  rows.emplace_back("Offset", std::to_string(itsOffset));
  if (itsHasNodata) rows.emplace_back("Nodata", std::to_string(itsNodata));
  if (itsHasUndetect) rows.emplace_back("Undetect", std::to_string(itsUndetect));
  if (!itsWkt.empty())
  {
    auto pos = itsWkt.find("PROJCS[\"");
    if (pos != std::string::npos)
    {
      auto end = itsWkt.find('"', pos + 8);
      if (end != std::string::npos)
        rows.emplace_back("Projection", itsWkt.substr(pos + 8, end - pos - 8));
    }
  }
  return rows;
}

std::string GeoTiffSource::gridSignature() const
{
  // WKT, dimensions, and the GeoTransform together pin down the grid:
  // two files only match when their projection and pixel grid agree
  // bit-for-bit. Within one producer's output that's a strict yes/no.
  char gt[160];
  std::snprintf(gt, sizeof(gt), "%g,%g,%g,%g", itsOriginX, itsOriginY, itsPixelW, itsPixelH);
  return std::string("tif:") + itsWkt + "|" + std::to_string(itsNx) + "x" +
         std::to_string(itsNy) + "|" + gt;
}
}  // namespace Qdless
