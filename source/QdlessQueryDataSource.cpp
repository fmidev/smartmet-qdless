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
  // Walk the four corners in case the projection is rotated/oblique so the
  // axis-aligned lat/lon bbox correctly encloses the grid.
  NFmiPoint corners[4] = {
      area->TopLeftLatLon(), area->TopRightLatLon(), area->BottomLeftLatLon(),
      area->BottomRightLatLon()};
  b.minLat = b.maxLat = corners[0].Y();
  b.minLon = b.maxLon = corners[0].X();
  for (int i = 1; i < 4; ++i)
  {
    b.minLat = std::min(b.minLat, corners[i].Y());
    b.maxLat = std::max(b.maxLat, corners[i].Y());
    b.minLon = std::min(b.minLon, corners[i].X());
    b.maxLon = std::max(b.maxLon, corners[i].X());
  }
  return b;
}
}  // namespace Qdless
