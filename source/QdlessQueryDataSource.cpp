#include "QdlessQueryDataSource.h"

#include <newbase/NFmiArea.h>
#include <newbase/NFmiEnumConverter.h>
#include <newbase/NFmiFastQueryInfo.h>
#include <newbase/NFmiGlobals.h>
#include <newbase/NFmiParameterName.h>
#include <newbase/NFmiPoint.h>
#include <newbase/NFmiProducer.h>
#include <newbase/NFmiQueryData.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Qdless
{
QueryDataSource::QueryDataSource(const std::string& filename)
{
  itsData = std::make_unique<NFmiQueryData>(filename);
  itsInfo = std::make_unique<NFmiFastQueryInfo>(itsData.get());
  if (!itsInfo->IsGrid())
    throw std::runtime_error("not gridded data: " + filename);

  // Cache parameter IDs in file order.
  itsInfo->FirstParam();
  do
  {
    itsParamIds.push_back(itsInfo->Param().GetParamIdent());
  } while (itsInfo->NextParam(true));
  itsInfo->FirstParam();
}

QueryDataSource::~QueryDataSource() = default;

std::vector<int> QueryDataSource::paramIds() const { return itsParamIds; }

std::string QueryDataSource::paramShortName(int paramId) const
{
  NFmiEnumConverter conv;
  return conv.ToString(paramId);
}

std::string QueryDataSource::paramLongName(int paramId) const
{
  // Save current; iterate to find matching param; restore.
  const int saved = itsInfo->Param().GetParamIdent();
  std::string name;
  if (itsInfo->Param(static_cast<FmiParameterName>(paramId)))
    name = itsInfo->Param().GetParamName().CharPtr();
  itsInfo->Param(static_cast<FmiParameterName>(saved));
  return name;
}

std::string QueryDataSource::paramUnits(int /*paramId*/) const
{
  // QueryData does not record explicit unit strings; rely on the fact that
  // SmartMet QueryData traditionally stores parameters in their canonical
  // units (Celsius for Temperature, m/s for wind, mm for precipitation, …).
  return {};
}

int QueryDataSource::currentParamId() const { return itsInfo->Param().GetParamIdent(); }

bool QueryDataSource::selectParamId(int paramId)
{
  return itsInfo->Param(static_cast<FmiParameterName>(paramId));
}

std::size_t QueryDataSource::timeCount() const { return itsInfo->SizeTimes(); }

std::size_t QueryDataSource::currentTimeIndex() const
{
  return static_cast<std::size_t>(itsInfo->TimeIndex());
}

void QueryDataSource::selectTimeIndex(std::size_t i)
{
  itsInfo->TimeIndex(static_cast<unsigned long>(i));
}

NFmiMetTime QueryDataSource::currentValidTime() const { return itsInfo->ValidTime(); }

NFmiMetTime QueryDataSource::originTime() const { return itsInfo->OriginTime(); }

std::size_t QueryDataSource::levelCount() const { return itsInfo->SizeLevels(); }

std::size_t QueryDataSource::currentLevelIndex() const
{
  return static_cast<std::size_t>(itsInfo->LevelIndex());
}

void QueryDataSource::selectLevelIndex(std::size_t i)
{
  itsInfo->LevelIndex(static_cast<unsigned long>(i));
}

float QueryDataSource::levelValueAt(std::size_t i) const
{
  const auto save = itsInfo->LevelIndex();
  itsInfo->LevelIndex(static_cast<unsigned long>(i));
  const float v = itsInfo->Level()->LevelValue();
  itsInfo->LevelIndex(save);
  return v;
}

std::string QueryDataSource::levelLabel(std::size_t i) const
{
  const auto save = itsInfo->LevelIndex();
  itsInfo->LevelIndex(static_cast<unsigned long>(i));
  const int t = static_cast<int>(itsInfo->Level()->LevelType());
  const float v = itsInfo->Level()->LevelValue();
  itsInfo->LevelIndex(save);
  return DataSource::formatLevelByType(t, v);
}

bool QueryDataSource::levelsAscendWithValue() const
{
  // QueryData carries a single level type per file; ask the first level
  // about it. Pressure / depth descend; everything else ascends.
  if (itsInfo->SizeLevels() == 0)
    return false;
  const auto save = itsInfo->LevelIndex();
  itsInfo->LevelIndex(0);
  const int t = static_cast<int>(itsInfo->Level()->LevelType());
  itsInfo->LevelIndex(save);
  return t != 100 && t != 160;
}

std::vector<DataSource::LevelGroup> QueryDataSource::levelGroupsForParam(int /*paramId*/) const
{
  // QueryData files carry exactly one level type. Report it as a single
  // group so the App's grouped picker still shows the type name in its
  // status line.
  const std::size_t n = itsInfo->SizeLevels();
  if (n == 0)
    return {};
  std::vector<float> values(n);
  const auto save = itsInfo->LevelIndex();
  int typeId = 0;
  for (std::size_t i = 0; i < n; ++i)
  {
    itsInfo->LevelIndex(static_cast<unsigned long>(i));
    values[i] = itsInfo->Level()->LevelValue();
    if (i == 0)
      typeId = static_cast<int>(itsInfo->Level()->LevelType());
  }
  itsInfo->LevelIndex(save);
  LevelGroup g;
  g.typeId = typeId;
  g.typeName = DataSource::levelTypeName(typeId);
  g.values = std::move(values);
  g.ascendsWithValue = (typeId != 100 && typeId != 160);
  return {g};
}

float QueryDataSource::interpolatedValue(double lat, double lon) const
{
  return itsInfo->InterpolatedValue(NFmiPoint(lon, lat));
}

void QueryDataSource::uvToLatLon(double u, double v, double& lat, double& lon) const
{
  // Render in the file's native projection: u,v are fractions of the
  // NFmiArea's image-coordinate rectangle (XY image coords have y=0 at
  // top = north for normal orientations). Falls back to bbox interpolation
  // if the area is missing for any reason.
  const auto* area = itsInfo->Area();
  if (area == nullptr)
  {
    DataSource::uvToLatLon(u, v, lat, lon);
    return;
  }
  const NFmiPoint xy(u * area->Width(), v * area->Height());
  const NFmiPoint world = area->XYToWorldXY(xy);
  const NFmiPoint ll = area->WorldXYToLatLon(world);
  lat = ll.Y();
  lon = ll.X();
}

void QueryDataSource::latLonToUV(double lat, double lon, double& u, double& v) const
{
  const auto* area = itsInfo->Area();
  if (area == nullptr)
  {
    DataSource::latLonToUV(lat, lon, u, v);
    return;
  }
  const NFmiPoint world = area->LatLonToWorldXY(NFmiPoint(lon, lat));
  const NFmiPoint xy = area->WorldXYToXY(world);
  const double w = area->Width();
  const double h = area->Height();
  u = w > 0 ? xy.X() / w : 0.0;
  v = h > 0 ? xy.Y() / h : 0.0;
}

LatLonBox QueryDataSource::boundingBox() const
{
  LatLonBox b;
  const auto* area = itsInfo->Area();
  if (area == nullptr) return b;

  // For non-rectilinear projections (polar stereographic, lambert, rotated
  // lat/lon, …) the lat/lon at the four corners can underestimate the
  // axis-aligned bbox: e.g. a polar-stereographic grid centered on the pole
  // covers latitude 90° at its midpoint, but never at the corners. Sample
  // along the perimeter at fine granularity to capture the true extent.
  b.minLat = std::numeric_limits<double>::infinity();
  b.maxLat = -std::numeric_limits<double>::infinity();
  b.minLon = std::numeric_limits<double>::infinity();
  b.maxLon = -std::numeric_limits<double>::infinity();

  const double w = area->Width();
  const double h = area->Height();

  auto addPoint = [&](double x, double y) {
    NFmiPoint world = area->XYToWorldXY(NFmiPoint(x, y));
    NFmiPoint ll = area->WorldXYToLatLon(world);
    b.minLat = std::min(b.minLat, ll.Y());
    b.maxLat = std::max(b.maxLat, ll.Y());
    b.minLon = std::min(b.minLon, ll.X());
    b.maxLon = std::max(b.maxLon, ll.X());
  };

  constexpr int kSamples = 60;
  for (int i = 0; i <= kSamples; ++i)
  {
    const double t = static_cast<double>(i) / kSamples;
    addPoint(0, t * h);          // left edge
    addPoint(w, t * h);          // right edge
    addPoint(t * w, 0);          // top edge
    addPoint(t * w, h);          // bottom edge
  }
  return b;
}

std::vector<std::pair<std::string, std::string>> QueryDataSource::extraMetadata() const
{
  std::vector<std::pair<std::string, std::string>> rows;
  rows.emplace_back("Format", "QueryData");

  if (auto* prod = itsInfo->Producer(); prod != nullptr)
  {
    const std::string name = prod->GetName().CharPtr();
    const auto id = prod->GetIdent();
    if (!name.empty() || id != 0)
    {
      std::string val = name.empty() ? std::string{} : name;
      if (id != 0)
      {
        if (!val.empty()) val += ' ';
        val += '(';
        val += std::to_string(id);
        val += ')';
      }
      rows.emplace_back("Producer", val);
    }
  }

  if (const auto* area = itsInfo->Area(); area != nullptr)
  {
    rows.emplace_back("Grid", area->ClassName());
    rows.emplace_back("Grid size", std::to_string(itsInfo->GridXNumber()) + "x" +
                                       std::to_string(itsInfo->GridYNumber()));
    // Area::AreaStr() returns the SmartMet projection descriptor
    // (e.g. "stereographic,20,90,60:6,51.3,49,70.2"). Long-ish but the
    // popup wraps gracefully.
    const std::string proj = area->AreaStr();
    if (!proj.empty()) rows.emplace_back("Projection", proj);
  }
  return rows;
}

std::string QueryDataSource::gridSignature() const
{
  const auto* area = itsInfo->Area();
  if (area == nullptr) return DataSource::gridSignature();
  return std::string("qd:") + area->ProjStr() + "|" +
         std::to_string(itsInfo->GridXNumber()) + "x" +
         std::to_string(itsInfo->GridYNumber());
}

namespace
{
// Returns true if the file's param list contains GeomHeight (preferred) or
// GeopHeight, with the same level set as everything else. Caller must
// restore the param index afterwards. The probe leaves itsInfo pointing
// at the first available height param, with a `useGeop` flag telling the
// caller it needs to divide by g.
bool selectHeightParam(NFmiFastQueryInfo& info, bool& useGeop)
{
  if (info.Param(kFmiGeomHeight))
  {
    useGeop = false;
    return true;
  }
  if (info.Param(kFmiGeopHeight))
  {
    useGeop = true;
    return true;
  }
  return false;
}
}  // namespace

bool QueryDataSource::isVolumetric() const
{
  if (itsInfo->SizeLevels() < 2) return false;
  const auto savedParam = itsInfo->ParamIndex();
  bool useGeop = false;
  const bool ok = selectHeightParam(*itsInfo, useGeop);
  itsInfo->ParamIndex(savedParam);
  return ok;
}

bool QueryDataSource::sampleVolume(const std::function<void(const VolumeSample&)>& cb) const
{
  const auto nLevels = itsInfo->SizeLevels();
  const auto nLoc = itsInfo->SizeLocations();
  if (nLevels < 2 || nLoc == 0) return false;

  const auto savedParam = itsInfo->ParamIndex();
  const auto savedLevel = itsInfo->LevelIndex();
  const auto savedLoc = itsInfo->LocationIndex();

  bool useGeop = false;
  if (!selectHeightParam(*itsInfo, useGeop))
  {
    itsInfo->ParamIndex(savedParam);
    return false;
  }
  const auto heightParamIndex = itsInfo->ParamIndex();
  const double heightScale = useGeop ? (1.0 / 9.80665) : 1.0;

  // Lat/lon per grid cell is invariant across level/param/time, so cache
  // it once. 65 levels × 316·356 ≈ 7.3M emissions; we don't want to do
  // a NFmiArea projection that many times.
  std::vector<std::pair<float, float>> latlons(nLoc);
  itsInfo->FirstLocation();
  for (std::size_t i = 0; i < nLoc; ++i)
  {
    const NFmiPoint ll = itsInfo->LatLon();
    latlons[i] = {static_cast<float>(ll.Y()), static_cast<float>(ll.X())};
    if (!itsInfo->NextLocation()) break;
  }

  std::vector<float> heights(nLoc);
  std::vector<float> values(nLoc);

  for (std::size_t li = 0; li < nLevels; ++li)
  {
    // Heights at this level.
    itsInfo->ParamIndex(heightParamIndex);
    itsInfo->LevelIndex(static_cast<unsigned long>(li));
    itsInfo->FirstLocation();
    for (std::size_t i = 0; i < nLoc; ++i)
    {
      heights[i] = itsInfo->FloatValue();
      if (!itsInfo->NextLocation()) break;
    }
    // Active-param values at this level.
    itsInfo->ParamIndex(savedParam);
    itsInfo->LevelIndex(static_cast<unsigned long>(li));
    itsInfo->FirstLocation();
    for (std::size_t i = 0; i < nLoc; ++i)
    {
      values[i] = itsInfo->FloatValue();
      if (!itsInfo->NextLocation()) break;
    }
    for (std::size_t i = 0; i < nLoc; ++i)
    {
      const float h = heights[i];
      const float v = values[i];
      if (h == kFloatMissing || v == kFloatMissing) continue;
      if (!std::isfinite(h) || !std::isfinite(v)) continue;
      cb(VolumeSample{static_cast<double>(latlons[i].first),
                      static_cast<double>(latlons[i].second),
                      static_cast<double>(h) * heightScale, v});
    }
  }

  itsInfo->ParamIndex(savedParam);
  itsInfo->LevelIndex(savedLevel);
  itsInfo->LocationIndex(savedLoc);
  return true;
}

bool QueryDataSource::isSurfaceStack() const
{
  // Need at least the bottom two cloud layers plus precip + fog to make
  // the synthetic stack visually meaningful. HighCloudCover is optional.
  const auto savedParam = itsInfo->ParamIndex();
  auto has = [&](FmiParameterName p) { return itsInfo->Param(p); };
  const bool ok =
      has(kFmiPrecipitation1h) && has(kFmiFogIntensity) &&
      has(kFmiLowCloudCover) && has(kFmiMediumCloudCover);
  itsInfo->ParamIndex(savedParam);
  return ok;
}

bool QueryDataSource::sampleSlab(
    int paramId, const std::function<void(double, double, float)>& cb) const
{
  const auto savedParam = itsInfo->ParamIndex();
  const auto savedLoc = itsInfo->LocationIndex();

  if (!itsInfo->Param(static_cast<FmiParameterName>(paramId)))
  {
    itsInfo->ParamIndex(savedParam);
    return false;
  }

  itsInfo->FirstLocation();
  const auto nLoc = itsInfo->SizeLocations();
  for (std::size_t i = 0; i < nLoc; ++i)
  {
    const float v = itsInfo->FloatValue();
    if (v != kFloatMissing && std::isfinite(v))
    {
      const NFmiPoint ll = itsInfo->LatLon();
      cb(ll.Y(), ll.X(), v);
    }
    if (!itsInfo->NextLocation()) break;
  }

  itsInfo->ParamIndex(savedParam);
  itsInfo->LocationIndex(savedLoc);
  return true;
}
}  // namespace Qdless
