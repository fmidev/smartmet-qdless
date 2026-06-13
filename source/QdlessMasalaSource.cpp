#include "QdlessMasalaSource.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <set>
#include <stdexcept>

namespace Qdless
{
namespace
{
int parseIntField(const std::string& s, std::size_t off, std::size_t len)
{
  if (off + len > s.size())
    return 0;
  return std::atoi(s.substr(off, len).c_str());
}
}  // namespace

MasalaSource::MasalaSource(MasalaCube cube) : itsCube(std::move(cube))
{
  if (itsCube.empty())
    throw std::runtime_error("masala: no renderable data in " + itsCube.dir());

  // Reference time → origin. Format YYYYMMDDHHMM.
  const std::string& rt = itsCube.reftime();
  if (rt.size() == 12)
  {
    const short yy = static_cast<short>(parseIntField(rt, 0, 4));
    const short mo = static_cast<short>(parseIntField(rt, 4, 2));
    const short dd = static_cast<short>(parseIntField(rt, 6, 2));
    const short hh = static_cast<short>(parseIntField(rt, 8, 2));
    const short mi = static_cast<short>(parseIntField(rt, 10, 2));
    if (yy > 1900 && mo >= 1 && mo <= 12 && dd >= 1 && dd <= 31)
    {
      itsOrigin = NFmiMetTime(yy, mo, dd, hh, mi);
      itsHasOrigin = true;
    }
  }

  // Resolve per-parameter metadata by opening one representative file each.
  // Real newbase ids are used where they resolve (so -p / palette lookup work
  // exactly as for a standalone GRIB); duplicates and unresolved names get a
  // unique synthetic id so every parameter stays pickable.
  std::set<int> used;
  itsMeta.reserve(itsCube.params().size());
  for (std::size_t i = 0; i < itsCube.params().size(); ++i)
  {
    const auto& p = itsCube.params()[i];
    ParamMeta m;
    m.shortName = p.name;
    m.longName = p.name;
    int id = 0;

    // Find any existing file for this parameter.
    std::string repr;
    for (const auto& ax : p.levels)
    {
      if (ax.levelStrs.empty())
        continue;
      for (int lead : itsCube.leadHours())
      {
        repr = itsCube.pathFor(p.name, ax.typeToken, ax.levelStrs.front(), lead);
        if (!repr.empty())
          break;
      }
      if (!repr.empty())
        break;
    }
    if (!repr.empty())
    {
      if (DataSource* d = openCached(repr))
      {
        const auto ids = d->paramIds();
        if (!ids.empty())
        {
          const int real = ids.front();
          // Keep the filename token ("T-K", "CAT-M23S") as the display name —
          // it's the radon-canonical name, matches the catalog browser, and
          // palettes resolve on it. Take only units and the newbase id from
          // the representative file.
          m.units = d->paramUnits(real);
          if (real != 0 && used.find(real) == used.end())
            id = real;
        }
      }
    }
    if (id == 0)
    {
      // Synthetic, guaranteed-unique handle (kept clear of the newbase id
      // range used by real parameters).
      id = 200000 + static_cast<int>(i);
      while (used.find(id) != used.end())
        ++id;
    }
    m.id = id;
    used.insert(id);
    itsMeta.push_back(std::move(m));
  }
}

MasalaSource::~MasalaSource() = default;

DataSource* MasalaSource::openCached(const std::string& path) const
{
  if (path.empty())
    return nullptr;
  if (auto it = itsCache.find(path); it != itsCache.end())
    return it->second.get();
  std::unique_ptr<DataSource> d;
  try
  {
    d = DataSource::open(path);
  }
  catch (const std::exception&)
  {
    return nullptr;
  }
  DataSource* raw = d.get();
  itsCache.emplace(path, std::move(d));
  itsCacheOrder.push_back(path);
  while (itsCacheOrder.size() > kCacheMax)
  {
    const std::string victim = itsCacheOrder.front();
    itsCacheOrder.pop_front();
    if (victim != path)
      itsCache.erase(victim);
  }
  return raw;
}

const MasalaCube::LevelAxis* MasalaSource::activeAxis() const
{
  const auto& p = curParam();
  if (p.levels.empty())
    return nullptr;
  int g = 0;
  if (auto it = itsActiveGroup.find(itsParamIdx); it != itsActiveGroup.end())
    g = it->second;
  if (g < 0 || g >= static_cast<int>(p.levels.size()))
    g = 0;
  return &p.levels[static_cast<std::size_t>(g)];
}

std::string MasalaSource::currentPath() const
{
  const auto* ax = activeAxis();
  if (ax == nullptr || ax->levelStrs.empty() || itsCube.leadHours().empty())
    return {};
  const std::size_t li = std::min(currentLevelIndex(), ax->levelStrs.size() - 1);
  const int lead = itsCube.leadHours()[std::min(itsTimeIdx, itsCube.leadHours().size() - 1)];
  return itsCube.pathFor(curParam().name, ax->typeToken, ax->levelStrs[li], lead);
}

DataSource* MasalaSource::currentDelegate() const
{
  return openCached(currentPath());
}

std::vector<int> MasalaSource::paramIds() const
{
  std::vector<int> ids;
  ids.reserve(itsMeta.size());
  for (const auto& m : itsMeta)
    ids.push_back(m.id);
  return ids;
}

std::string MasalaSource::paramShortName(int paramId) const
{
  for (const auto& m : itsMeta)
    if (m.id == paramId)
      return m.shortName;
  return std::to_string(paramId);
}

std::string MasalaSource::paramLongName(int paramId) const
{
  for (const auto& m : itsMeta)
    if (m.id == paramId)
      return m.longName;
  return paramShortName(paramId);
}

std::string MasalaSource::paramUnits(int paramId) const
{
  for (const auto& m : itsMeta)
    if (m.id == paramId)
      return m.units;
  return {};
}

int MasalaSource::currentParamId() const
{
  return itsParamIdx < itsMeta.size() ? itsMeta[itsParamIdx].id : 0;
}

bool MasalaSource::selectParamId(int paramId)
{
  for (std::size_t i = 0; i < itsMeta.size(); ++i)
  {
    if (itsMeta[i].id == paramId)
    {
      itsParamIdx = i;
      return true;
    }
  }
  return false;
}

std::size_t MasalaSource::timeCount() const
{
  return itsCube.leadHours().size();
}
std::size_t MasalaSource::currentTimeIndex() const
{
  return itsTimeIdx;
}
void MasalaSource::selectTimeIndex(std::size_t i)
{
  if (i < itsCube.leadHours().size())
    itsTimeIdx = i;
}

NFmiMetTime MasalaSource::originTime() const
{
  if (itsHasOrigin)
    return itsOrigin;
  if (DataSource* d = currentDelegate())
    return d->originTime();
  return NFmiMetTime(0, 0, 0, 0, 0);
}

NFmiMetTime MasalaSource::currentValidTime() const
{
  if (itsHasOrigin && !itsCube.leadHours().empty())
  {
    NFmiMetTime t = itsOrigin;
    t.ChangeByHours(itsCube.leadHours()[std::min(itsTimeIdx, itsCube.leadHours().size() - 1)]);
    return t;
  }
  if (DataSource* d = currentDelegate())
    return d->currentValidTime();
  return itsOrigin;
}

std::vector<DataSource::LevelGroup> MasalaSource::levelGroupsForParam(int paramId) const
{
  std::vector<LevelGroup> out;
  std::size_t idx = itsParamIdx;
  for (std::size_t i = 0; i < itsMeta.size(); ++i)
    if (itsMeta[i].id == paramId)
      idx = i;
  if (idx >= itsCube.params().size())
    return out;
  for (const auto& ax : itsCube.params()[idx].levels)
  {
    LevelGroup g;
    g.typeId = ax.typeId;
    g.typeName = ax.typeId != 0 ? DataSource::levelTypeName(ax.typeId)
                                : (ax.typeToken.empty() ? "Levels" : ax.typeToken);
    g.values.assign(ax.levelVals.begin(), ax.levelVals.end());
    g.ascendsWithValue = ax.ascends;
    out.push_back(std::move(g));
  }
  return out;
}

void MasalaSource::selectLevelGroup(int paramId, int groupIdx)
{
  for (std::size_t i = 0; i < itsMeta.size(); ++i)
    if (itsMeta[i].id == paramId)
      itsActiveGroup[i] = groupIdx;
}

int MasalaSource::currentLevelGroupIndex(int paramId) const
{
  for (std::size_t i = 0; i < itsMeta.size(); ++i)
    if (itsMeta[i].id == paramId)
      if (auto it = itsActiveGroup.find(i); it != itsActiveGroup.end())
        return it->second;
  return 0;
}

std::size_t MasalaSource::levelCount() const
{
  const auto* ax = activeAxis();
  return ax ? ax->levelVals.size() : 0;
}

std::size_t MasalaSource::currentLevelIndex() const
{
  int g = currentLevelGroupIndex(currentParamId());
  if (auto it = itsActiveLevel.find({itsParamIdx, g}); it != itsActiveLevel.end())
  {
    const auto* ax = activeAxis();
    if (ax && it->second < ax->levelVals.size())
      return it->second;
  }
  return 0;
}

void MasalaSource::selectLevelIndex(std::size_t i)
{
  int g = currentLevelGroupIndex(currentParamId());
  const auto* ax = activeAxis();
  if (ax && i < ax->levelVals.size())
    itsActiveLevel[{itsParamIdx, g}] = i;
}

float MasalaSource::levelValueAt(std::size_t i) const
{
  const auto* ax = activeAxis();
  return (ax && i < ax->levelVals.size()) ? static_cast<float>(ax->levelVals[i]) : 0.0F;
}

std::string MasalaSource::levelLabel(std::size_t i) const
{
  const auto* ax = activeAxis();
  if (ax == nullptr || i >= ax->levelStrs.size())
    return {};
  // Ranges and unknown types: show the raw token. Known types: unit-aware.
  if (ax->typeId == 0 || ax->levelStrs[i].find('-', 1) != std::string::npos)
    return ax->levelStrs[i];
  return DataSource::formatLevelByType(ax->typeId, static_cast<float>(ax->levelVals[i]));
}

bool MasalaSource::levelsAscendWithValue() const
{
  const auto* ax = activeAxis();
  return ax ? ax->ascends : false;
}

float MasalaSource::interpolatedValue(double lat, double lon) const
{
  if (DataSource* d = currentDelegate())
    return d->interpolatedValue(lat, lon);
  return std::numeric_limits<float>::quiet_NaN();
}

LatLonBox MasalaSource::boundingBox() const
{
  if (DataSource* d = currentDelegate())
    return d->boundingBox();
  return {};
}

void MasalaSource::uvToLatLon(double u, double v, double& lat, double& lon) const
{
  if (DataSource* d = currentDelegate())
  {
    d->uvToLatLon(u, v, lat, lon);
    return;
  }
  DataSource::uvToLatLon(u, v, lat, lon);
}

void MasalaSource::latLonToUV(double lat, double lon, double& u, double& v) const
{
  if (DataSource* d = currentDelegate())
  {
    d->latLonToUV(lat, lon, u, v);
    return;
  }
  DataSource::latLonToUV(lat, lon, u, v);
}

std::string MasalaSource::gridSignature() const
{
  if (DataSource* d = currentDelegate())
    return d->gridSignature();
  return "masala:" + itsCube.geometry();
}

std::vector<std::pair<std::string, std::string>> MasalaSource::extraMetadata() const
{
  std::vector<std::pair<std::string, std::string>> rows;
  if (!itsCube.producer().empty())
    rows.emplace_back("Producer", itsCube.producer());
  if (!itsCube.geometry().empty())
    rows.emplace_back("Geometry", itsCube.geometry());
  if (!itsCube.reftime().empty())
    rows.emplace_back("Reference time", itsCube.reftime());
  rows.emplace_back("Parameters", std::to_string(itsCube.params().size()));
  rows.emplace_back("Lead times", std::to_string(itsCube.leadHours().size()));
  if (const auto* ax = activeAxis())
    rows.emplace_back("Levels (" + ax->typeToken + ")", std::to_string(ax->levelVals.size()));
  rows.emplace_back("", "");
  if (DataSource* d = currentDelegate())
  {
    for (auto& r : d->extraMetadata())
      rows.push_back(std::move(r));
  }
  return rows;
}
}  // namespace Qdless
