#include "QdlessQueryDataSource.h"

#include <newbase/NFmiArea.h>
#include <newbase/NFmiEnumConverter.h>
#include <newbase/NFmiFastQueryInfo.h>
#include <newbase/NFmiPoint.h>
#include <newbase/NFmiQueryData.h>

#include <algorithm>
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

float QueryDataSource::interpolatedValue(double lat, double lon) const
{
  return itsInfo->InterpolatedValue(NFmiPoint(lon, lat));
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
}  // namespace Qdless
