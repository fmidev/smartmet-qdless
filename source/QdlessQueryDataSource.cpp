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
#include <limits>
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

bool QueryDataSource::hasNativeHeight() const
{
  // Any volumetric QD has a height axis we can sample at arbitrary
  // altitudes via interpolatedValueAtHeight below. PVOL was the
  // original "native height" source; this just extends the contract to
  // NWP hybrid / pressure files with GeomHeight.
  return isVolumetric();
}

std::pair<double, double> QueryDataSource::heightRangeKm() const
{
  // Sample the height field at the centre of the data bbox to pick a
  // sensible range for the cross-section / curtain Y-axis. The same
  // (param, level, location) state is restored before returning so
  // callers don't see the probe.
  if (!isVolumetric())
    return DataSource::heightRangeKm();
  const auto savedParam = itsInfo->ParamIndex();
  const auto savedLevel = itsInfo->LevelIndex();
  bool useGeop = false;
  if (!selectHeightParam(*itsInfo, useGeop))
  {
    itsInfo->ParamIndex(savedParam);
    return DataSource::heightRangeKm();
  }
  const double scale = useGeop ? (1.0 / 9.80665) : 1.0;
  const auto bb = boundingBox();
  const NFmiPoint ll((bb.minLon + bb.maxLon) * 0.5, (bb.minLat + bb.maxLat) * 0.5);
  double lo = std::numeric_limits<double>::infinity();
  double hi = -std::numeric_limits<double>::infinity();
  const auto nLevels = itsInfo->SizeLevels();
  for (std::size_t i = 0; i < nLevels; ++i)
  {
    itsInfo->LevelIndex(static_cast<unsigned long>(i));
    const float h = itsInfo->InterpolatedValue(ll);
    if (h == kFloatMissing || !std::isfinite(h)) continue;
    const double m = static_cast<double>(h) * scale;
    lo = std::min(lo, m);
    hi = std::max(hi, m);
  }
  itsInfo->ParamIndex(savedParam);
  itsInfo->LevelIndex(savedLevel);
  if (!std::isfinite(lo) || !std::isfinite(hi) || hi <= lo)
    return DataSource::heightRangeKm();
  // Clamp the bottom to >=0 — GeomHeight occasionally dips slightly
  // negative for the lowest hybrid level (terrain offset).
  return {std::max(0.0, lo / 1000.0), hi / 1000.0};
}

QueryDataSource::ColumnProfile QueryDataSource::sampleColumnProfile(double lat, double lon) const
{
  ColumnProfile p;
  const auto nLevels = itsInfo->SizeLevels();
  if (nLevels == 0)
    return p;
  const auto savedParam = itsInfo->ParamIndex();
  const auto savedLevel = itsInfo->LevelIndex();
  bool useGeop = false;
  if (!selectHeightParam(*itsInfo, useGeop))
  {
    // No height field; fall back to a single (h=0, value).
    itsInfo->ParamIndex(savedParam);
    p.heightsM.push_back(0.0F);
    p.values.push_back(interpolatedValue(lat, lon));
    return p;
  }
  const auto heightParamIndex = itsInfo->ParamIndex();
  const double heightScale = useGeop ? (1.0 / 9.80665) : 1.0;
  const NFmiPoint ll(lon, lat);
  p.heightsM.resize(nLevels);
  p.values.resize(nLevels);
  // Heights pass.
  itsInfo->ParamIndex(heightParamIndex);
  for (std::size_t i = 0; i < nLevels; ++i)
  {
    itsInfo->LevelIndex(static_cast<unsigned long>(i));
    const float h = itsInfo->InterpolatedValue(ll);
    p.heightsM[i] =
        (h == kFloatMissing) ? kFloatMissing : static_cast<float>(h * heightScale);
  }
  // Active-param values pass.
  itsInfo->ParamIndex(savedParam);
  for (std::size_t i = 0; i < nLevels; ++i)
  {
    itsInfo->LevelIndex(static_cast<unsigned long>(i));
    p.values[i] = itsInfo->InterpolatedValue(ll);
  }
  itsInfo->LevelIndex(savedLevel);
  return p;
}

float QueryDataSource::interpolatedValueAtHeight(double lat, double lon, double heightKm) const
{
  const auto nLevels = itsInfo->SizeLevels();
  if (nLevels < 2)
    return interpolatedValue(lat, lon);

  const auto savedParam = itsInfo->ParamIndex();
  const auto savedLevel = itsInfo->LevelIndex();
  bool useGeop = false;
  if (!selectHeightParam(*itsInfo, useGeop))
  {
    itsInfo->ParamIndex(savedParam);
    return interpolatedValue(lat, lon);
  }
  const auto heightParamIndex = itsInfo->ParamIndex();
  const double heightScale = useGeop ? (1.0 / 9.80665) : 1.0;
  const double targetM = heightKm * 1000.0;
  const NFmiPoint ll(lon, lat);

  // Heights at every level for this (lat, lon).
  std::vector<float> heights(nLevels);
  itsInfo->ParamIndex(heightParamIndex);
  for (std::size_t i = 0; i < nLevels; ++i)
  {
    itsInfo->LevelIndex(static_cast<unsigned long>(i));
    const float h = itsInfo->InterpolatedValue(ll);
    heights[i] =
        (h == kFloatMissing) ? kFloatMissing : static_cast<float>(h * heightScale);
  }

  // Find the adjacent (level i, level i+1) pair that brackets targetM.
  // Hybrid level lists may ascend or descend with index — accept either.
  int loIdx = -1, hiIdx = -1;
  for (std::size_t i = 0; i + 1 < nLevels; ++i)
  {
    const float a = heights[i];
    const float b = heights[i + 1];
    if (a == kFloatMissing || b == kFloatMissing)
      continue;
    const float minv = std::min(a, b);
    const float maxv = std::max(a, b);
    if (targetM >= minv && targetM <= maxv)
    {
      loIdx = (a <= b) ? static_cast<int>(i) : static_cast<int>(i + 1);
      hiIdx = (a <= b) ? static_cast<int>(i + 1) : static_cast<int>(i);
      break;
    }
  }
  if (loIdx < 0)
  {
    itsInfo->ParamIndex(savedParam);
    itsInfo->LevelIndex(savedLevel);
    return kFloatMissing;  // outside the column's vertical extent
  }

  itsInfo->ParamIndex(savedParam);
  itsInfo->LevelIndex(static_cast<unsigned long>(loIdx));
  const float vLo = itsInfo->InterpolatedValue(ll);
  itsInfo->LevelIndex(static_cast<unsigned long>(hiIdx));
  const float vHi = itsInfo->InterpolatedValue(ll);
  itsInfo->LevelIndex(savedLevel);

  if (vLo == kFloatMissing || vHi == kFloatMissing)
    return kFloatMissing;
  const float hLo = heights[loIdx];
  const float hHi = heights[hiIdx];
  const float t = (hHi != hLo) ? static_cast<float>((targetM - hLo) / (hHi - hLo)) : 0.0F;
  return vLo + (vHi - vLo) * t;
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

bool QueryDataSource::sampleVolumeGrid(std::size_t& nx,
                                       std::size_t& ny,
                                       std::size_t& nz,
                                       std::vector<float>& values,
                                       std::vector<float>& heights,
                                       std::vector<float>& lats,
                                       std::vector<float>& lons,
                                       std::size_t targetMaxCells) const
{
  if (!itsInfo->IsGrid())
    return false;
  const auto nLevels = itsInfo->SizeLevels();
  const auto nLoc = itsInfo->SizeLocations();
  if (nLevels < 2 || nLoc == 0)
    return false;
  const std::size_t fullNx = itsInfo->GridXNumber();
  const std::size_t fullNy = itsInfo->GridYNumber();
  if (fullNx * fullNy != nLoc)
    return false;  // not a plain structured grid; bail rather than mis-index

  // Horizontal stride to keep the cell count under the caller's budget — the
  // merge-tree cost (and this read) scale with the cell count, so on a big
  // hybrid volume a stride of a few keeps the extrema view interactive.
  // Vertical resolution is preserved (levels carry the air-mass structure).
  std::size_t stride = 1;
  if (targetMaxCells > 0)
  {
    const double full = static_cast<double>(fullNx) * fullNy * nLevels;
    if (full > static_cast<double>(targetMaxCells))
      stride = static_cast<std::size_t>(std::ceil(std::sqrt(full / targetMaxCells)));
    if (stride < 1)
      stride = 1;
  }
  nx = (fullNx + stride - 1) / stride;
  ny = (fullNy + stride - 1) / stride;
  nz = nLevels;
  const std::size_t nLoc2 = nx * ny;

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

  constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

  // Source location index of strided cell (i2,j2): (j2*stride)*fullNx + i2*stride.
  auto srcLoc = [&](std::size_t i2, std::size_t j2)
  { return (j2 * stride) * fullNx + i2 * stride; };

  // Lat/lon per kept cell (level-invariant) — dest index j2*nx + i2.
  lats.assign(nLoc2, kNaN);
  lons.assign(nLoc2, kNaN);
  for (std::size_t j2 = 0; j2 < ny; ++j2)
    for (std::size_t i2 = 0; i2 < nx; ++i2)
    {
      itsInfo->LocationIndex(static_cast<unsigned long>(srcLoc(i2, j2)));
      const NFmiPoint ll = itsInfo->LatLon();
      lats[j2 * nx + i2] = static_cast<float>(ll.Y());
      lons[j2 * nx + i2] = static_cast<float>(ll.X());
    }

  values.assign(nLoc2 * nLevels, kNaN);
  heights.assign(nLoc2 * nLevels, kNaN);
  for (std::size_t li = 0; li < nLevels; ++li)
  {
    const std::size_t base = li * nLoc2;
    for (int pass = 0; pass < 2; ++pass)
    {
      itsInfo->ParamIndex(pass == 0 ? heightParamIndex : savedParam);
      itsInfo->LevelIndex(static_cast<unsigned long>(li));
      for (std::size_t j2 = 0; j2 < ny; ++j2)
        for (std::size_t i2 = 0; i2 < nx; ++i2)
        {
          itsInfo->LocationIndex(static_cast<unsigned long>(srcLoc(i2, j2)));
          const float val = itsInfo->FloatValue();
          if (val == kFloatMissing || !std::isfinite(val))
            continue;
          const std::size_t d = base + j2 * nx + i2;
          if (pass == 0)
            heights[d] = static_cast<float>(val * heightScale);
          else
            values[d] = val;
        }
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
