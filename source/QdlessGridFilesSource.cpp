#include "QdlessGridFilesSource.h"

#include <grid-files/grid/GridFile.h>
#include <grid-files/grid/Message.h>
#include <grid-files/grid/Typedefs.h>
#include <grid-files/common/Coordinate.h>
#include <grid-files/identification/GridDef.h>

#include <newbase/NFmiEnumConverter.h>

using namespace SmartMet;

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <set>
#include <stdexcept>

namespace Qdless
{
namespace
{
// Convert FMI parameter ID to newbase enum value via grid-files' GridDef.
// Currently we just use the FMI ID directly — the SmartMet convention has
// FMI param IDs aligned with newbase enums for the params we care about.
int fmiToNewbase(int fmiId) { return fmiId; }

// Parse "20260508T030000" → NFmiMetTime.
NFmiMetTime parseForecastTime(const char* s)
{
  // Format: YYYYMMDDTHHMMSS or yyyy-mm-dd hh:mm:ss
  if (s == nullptr || std::string(s).empty()) return NFmiMetTime();
  int yy = 0;
  int mm = 0;
  int dd = 0;
  int h = 0;
  int mi = 0;
  int se = 0;
  if (std::sscanf(s, "%4d%2d%2dT%2d%2d%2d", &yy, &mm, &dd, &h, &mi, &se) == 6 ||
      std::sscanf(s, "%4d-%2d-%2d %2d:%2d:%2d", &yy, &mm, &dd, &h, &mi, &se) == 6)
  {
    return NFmiMetTime(static_cast<short>(yy), static_cast<short>(mm), static_cast<short>(dd),
                       static_cast<short>(h), static_cast<short>(mi), static_cast<short>(se));
  }
  return NFmiMetTime();
}
}  // namespace

void GridFilesSource::ensureGridDef()
{
  static bool initialised = false;
  if (initialised) return;
  // Search common locations for grid-files.conf. The library's parameter
  // mapping CSVs live in the same dir; the conf references them via
  // "%(DIR)/...".
  const std::vector<std::filesystem::path> candidates = {
      "/usr/share/smartmet/library/grid-files/grid-files.conf",
      "/usr/share/smartmet/grid-files/grid-files.conf",
      "/usr/share/smartmet/test/grid/library/grid-files.conf",
  };
  for (const auto& p : candidates)
  {
    if (std::filesystem::exists(p))
    {
      try
      {
        SmartMet::Identification::gridDef.init(p.string().c_str());
        initialised = true;
        return;
      }
      catch (const std::exception&)
      {
        // try next
      }
    }
  }
  // Couldn't init: still mark as attempted. Reads will fail with a clear
  // error from grid-files itself.
  initialised = true;
}

GridFilesSource::GridFilesSource(const std::string& filename)
{
  ensureGridDef();
  itsFile = std::make_unique<SmartMet::GRID::GridFile>();
  itsFile->read(filename);
  if (itsFile->getNumberOfMessages() == 0)
    throw std::runtime_error("no messages in file: " + filename);
  indexMessages();

  if (!itsParamIds.empty()) itsCurrentParam = itsParamIds.front();
}

GridFilesSource::~GridFilesSource() = default;

void GridFilesSource::indexMessages()
{
  // Resolve each message's parameter via its NAME (preferring the explicit
  // newbase / FMI / GRIB name from the file) rather than blindly trusting
  // grid-files' numeric FMI mapping, which depends on a CSV table that may
  // not cover every (discipline, category, number) GRIB combination — when
  // the table doesn't match, the numeric ID can collide with a totally
  // unrelated FMI parameter (e.g. Temperature ↦ RadiationOutSW2).
  NFmiEnumConverter conv;
  auto resolveId = [&](SmartMet::GRID::Message* m) -> int {
    auto tryName = [&](const char* s) -> int {
      if (s == nullptr || *s == '\0') return kFmiBadParameter;
      const int id = conv.ToEnum(s);
      return id;
    };
    // Prefer the GRIB-native parameter name straight from the file (the
    // parameterName / shortName field) — that's what tools like grib_dump
    // report and what the file producer intended. Fall through to FMI/
    // newbase mappings only if that doesn't resolve, since those depend on
    // grid-files' CSV tables which can mis-map unconfigured (discipline,
    // category, number) tuples to unrelated FMI parameters.
    int id = tryName(m->getGribParameterName());
    if (id == kFmiBadParameter) id = tryName(m->getNewbaseParameterName());
    if (id == kFmiBadParameter) id = tryName(m->getFmiParameterName());
    if (id == kFmiBadParameter)
    {
      // Last-resort fallback: numeric FMI ID. Likely wrong but at least
      // makes the parameter pickable.
      id = static_cast<int>(m->getFmiParameterId());
    }
    return id;
  };
  auto resolveUnits = [](SmartMet::GRID::Message* m) -> std::string {
    if (const char* u = m->getFmiParameterUnits(); u != nullptr) return u;
    return {};
  };

  std::map<int, std::string> idToUnits;  // remember units for each id
  std::map<int, std::string> idToNative;  // first non-empty native name
  std::set<int> paramSet;
  std::set<long> timeSet;  // unix timestamps
  std::set<float> levelSet;

  auto firstNonEmpty = [](std::initializer_list<const char*> ss) -> std::string {
    for (const char* s : ss)
      if (s != nullptr && *s != '\0') return s;
    return {};
  };

  const std::size_t n = itsFile->getNumberOfMessages();
  for (std::size_t i = 0; i < n; ++i)
  {
    auto* m = itsFile->getMessageByIndex(i);
    const int id = resolveId(m);
    paramSet.insert(id);
    if (idToUnits.find(id) == idToUnits.end()) idToUnits[id] = resolveUnits(m);
    if (idToNative.find(id) == idToNative.end())
      idToNative[id] = firstNonEmpty(
          {m->getNewbaseParameterName(), m->getFmiParameterName(),
           m->getNetCdfParameterName(), m->getGribParameterName()});
    timeSet.insert(static_cast<long>(m->getForecastTimeT()));
    levelSet.insert(static_cast<float>(m->getGridParameterLevel()));
  }
  itsParamIds.assign(paramSet.begin(), paramSet.end());
  itsParamUnits.clear();
  itsParamNativeNames.clear();
  itsParamUnits.reserve(itsParamIds.size());
  itsParamNativeNames.reserve(itsParamIds.size());
  for (int id : itsParamIds)
  {
    itsParamUnits.push_back(idToUnits[id]);
    itsParamNativeNames.push_back(idToNative[id]);
  }
  itsLevels.assign(levelSet.begin(), levelSet.end());
  itsTimesT.assign(timeSet.begin(), timeSet.end());
  itsTimes.clear();
  itsTimes.reserve(itsTimesT.size());
  for (long ts : itsTimesT)
  {
    std::tm utc{};
    gmtime_r(&ts, &utc);
    itsTimes.emplace_back(static_cast<short>(utc.tm_year + 1900),
                          static_cast<short>(utc.tm_mon + 1),
                          static_cast<short>(utc.tm_mday),
                          static_cast<short>(utc.tm_hour),
                          static_cast<short>(utc.tm_min),
                          static_cast<short>(utc.tm_sec));
  }

  // Build the lookup index — same name-resolution as above so the index
  // matches itsParamIds.
  for (std::size_t i = 0; i < n; ++i)
  {
    auto* m = itsFile->getMessageByIndex(i);
    const int pId = resolveId(m);
    const long ts = static_cast<long>(m->getForecastTimeT());
    const float lv = static_cast<float>(m->getGridParameterLevel());

    // Match against the raw time_t we stored — NFmiMetTime can snap to
    // an internal time-step resolution and break round-trip equality.
    auto tIt = std::find(itsTimesT.begin(), itsTimesT.end(), ts);
    auto lIt = std::find(itsLevels.begin(), itsLevels.end(), lv);
    if (tIt == itsTimesT.end() || lIt == itsLevels.end()) continue;
    const std::size_t tIdx = static_cast<std::size_t>(tIt - itsTimesT.begin());
    const std::size_t lIdx = static_cast<std::size_t>(lIt - itsLevels.begin());
    itsIndex[std::make_tuple(pId, tIdx, lIdx)] = i;
  }
}

SmartMet::GRID::Message* GridFilesSource::currentMessage() const
{
  auto it = itsIndex.find(std::make_tuple(itsCurrentParam, itsCurrentTime, itsCurrentLevel));
  if (it == itsIndex.end()) return nullptr;
  return itsFile->getMessageByIndex(it->second);
}

std::vector<int> GridFilesSource::paramIds() const { return itsParamIds; }

std::string GridFilesSource::paramShortName(int paramId) const
{
  // Prefer grid-files' FmiParameterDef.mParameterName — it's more accurate
  // than newbase's enum (which can have name collisions for IDs not in the
  // newbase table, e.g. ID 153 = "T-K" in grid-files but "RadiationOutSW2"
  // in newbase).
  using SmartMet::Identification::FmiParameterDef;
  FmiParameterDef def;
  if (SmartMet::Identification::gridDef.getFmiParameterDefById(paramId, def) &&
      !def.mParameterName.empty())
    return def.mParameterName;
  NFmiEnumConverter conv;
  std::string name = conv.ToString(paramId);
  if (!name.empty()) return name;
  // Fall back to the native name we cached from the file (NetCDF variable
  // name / GRIB shortName), useful for parameters that grid-files / newbase
  // don't have a config table entry for.
  auto it = std::find(itsParamIds.begin(), itsParamIds.end(), paramId);
  if (it != itsParamIds.end())
  {
    const std::size_t idx = static_cast<std::size_t>(it - itsParamIds.begin());
    if (idx < itsParamNativeNames.size() && !itsParamNativeNames[idx].empty())
      return itsParamNativeNames[idx];
  }
  return std::to_string(paramId);
}

std::string GridFilesSource::paramLongName(int paramId) const
{
  using SmartMet::Identification::FmiParameterDef;
  FmiParameterDef def;
  if (SmartMet::Identification::gridDef.getFmiParameterDefById(paramId, def))
    return def.mParameterDescription.empty() ? def.mParameterName : def.mParameterDescription;
  return paramShortName(paramId);
}

std::string GridFilesSource::paramUnits(int paramId) const
{
  auto it = std::find(itsParamIds.begin(), itsParamIds.end(), paramId);
  if (it == itsParamIds.end()) return {};
  const std::size_t idx = static_cast<std::size_t>(it - itsParamIds.begin());
  return idx < itsParamUnits.size() ? itsParamUnits[idx] : std::string{};
}

int GridFilesSource::currentParamId() const { return itsCurrentParam; }

bool GridFilesSource::selectParamId(int paramId)
{
  if (std::find(itsParamIds.begin(), itsParamIds.end(), paramId) == itsParamIds.end())
    return false;
  itsCurrentParam = paramId;
  return true;
}

std::size_t GridFilesSource::timeCount() const { return itsTimes.size(); }
std::size_t GridFilesSource::currentTimeIndex() const { return itsCurrentTime; }
void GridFilesSource::selectTimeIndex(std::size_t i)
{
  if (i < itsTimes.size()) itsCurrentTime = i;
}
NFmiMetTime GridFilesSource::currentValidTime() const
{
  return itsTimes.empty() ? NFmiMetTime() : itsTimes[itsCurrentTime];
}

std::size_t GridFilesSource::levelCount() const { return itsLevels.size(); }
std::size_t GridFilesSource::currentLevelIndex() const { return itsCurrentLevel; }
void GridFilesSource::selectLevelIndex(std::size_t i)
{
  if (i < itsLevels.size()) itsCurrentLevel = i;
}
float GridFilesSource::levelValueAt(std::size_t i) const
{
  return i < itsLevels.size() ? itsLevels[i] : 0.0F;
}

float GridFilesSource::interpolatedValue(double lat, double lon) const
{
  auto* msg = currentMessage();
  if (msg == nullptr) return std::numeric_limits<float>::quiet_NaN();
  // 1 = Linear (bilinear) per T::AreaInterpolationMethod.
  return static_cast<float>(msg->getGridValueByLatLonCoordinate(lat, lon, 1));
}

LatLonBox GridFilesSource::boundingBox() const
{
  LatLonBox b;
  // Use the first message's geometry; assume all messages share a grid.
  if (itsFile->getNumberOfMessages() == 0) return b;
  auto* msg = itsFile->getMessageByIndex(0);
  T::Coordinate tl;
  T::Coordinate tr;
  T::Coordinate bl;
  T::Coordinate br;
  if (!msg->getGridLatLonArea(tl, tr, bl, br)) return b;
  b.minLat = std::min({tl.y(), tr.y(), bl.y(), br.y()});
  b.maxLat = std::max({tl.y(), tr.y(), bl.y(), br.y()});
  b.minLon = std::min({tl.x(), tr.x(), bl.x(), br.x()});
  b.maxLon = std::max({tl.x(), tr.x(), bl.x(), br.x()});
  return b;
}
}  // namespace Qdless
