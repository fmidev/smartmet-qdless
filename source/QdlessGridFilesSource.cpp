#include "QdlessGridFilesSource.h"

#include <grid-files/common/Coordinate.h>
#include <grid-files/grid/GridFile.h>
#include <grid-files/grid/Message.h>
#include <grid-files/grid/Typedefs.h>
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
// Parse "20260508T030000" → NFmiMetTime.
NFmiMetTime parseForecastTime(const char* s)
{
  // Format: YYYYMMDDTHHMMSS or yyyy-mm-dd hh:mm:ss
  if (s == nullptr || std::string(s).empty())
    return NFmiMetTime();
  int yy = 0;
  int mm = 0;
  int dd = 0;
  int h = 0;
  int mi = 0;
  int se = 0;
  if (std::sscanf(s, "%4d%2d%2dT%2d%2d%2d", &yy, &mm, &dd, &h, &mi, &se) == 6 ||
      std::sscanf(s, "%4d-%2d-%2d %2d:%2d:%2d", &yy, &mm, &dd, &h, &mi, &se) == 6)
  {
    return NFmiMetTime(static_cast<short>(yy),
                       static_cast<short>(mm),
                       static_cast<short>(dd),
                       static_cast<short>(h),
                       static_cast<short>(mi),
                       static_cast<short>(se));
  }
  return NFmiMetTime();
}
}  // namespace

void GridFilesSource::ensureGridDef()
{
  static bool initialised = false;
  if (initialised)
    return;
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

  if (!itsParamIds.empty())
    itsCurrentParam = itsParamIds.front();
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
  auto resolveId = [&](SmartMet::GRID::Message* m) -> int
  {
    auto tryName = [&](const char* s) -> int
    {
      if (s == nullptr || *s == '\0')
        return kFmiBadParameter;
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
    if (id == kFmiBadParameter)
      id = tryName(m->getNewbaseParameterName());
    if (id == kFmiBadParameter)
      id = tryName(m->getFmiParameterName());
    if (id == kFmiBadParameter)
    {
      // Last-resort fallback: numeric FMI ID. Likely wrong but at least
      // makes the parameter pickable.
      id = static_cast<int>(m->getFmiParameterId());
    }
    return id;
  };
  auto resolveUnits = [](SmartMet::GRID::Message* m) -> std::string
  {
    if (const char* u = m->getFmiParameterUnits(); u != nullptr)
      return u;
    return {};
  };

  std::map<int, std::string> idToUnits;   // remember units for each id
  std::map<int, std::string> idToNative;  // first non-empty native name
  std::set<int> paramSet;
  std::set<long> timeSet;  // unix timestamps
  std::set<float> levelSet;

  auto firstNonEmpty = [](std::initializer_list<const char*> ss) -> std::string
  {
    for (const char* s : ss)
      if (s != nullptr && *s != '\0')
        return s;
    return {};
  };

  const std::size_t n = itsFile->getNumberOfMessages();
  for (std::size_t i = 0; i < n; ++i)
  {
    auto* m = itsFile->getMessageByIndex(i);
    const int id = resolveId(m);
    paramSet.insert(id);
    if (idToUnits.find(id) == idToUnits.end())
      idToUnits[id] = resolveUnits(m);
    if (idToNative.find(id) == idToNative.end())
      idToNative[id] = firstNonEmpty({m->getNewbaseParameterName(),
                                      m->getFmiParameterName(),
                                      m->getNetCdfParameterName(),
                                      m->getGribParameterName()});
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
    if (tIt == itsTimesT.end() || lIt == itsLevels.end())
      continue;
    const std::size_t tIdx = static_cast<std::size_t>(tIt - itsTimesT.begin());
    const std::size_t lIdx = static_cast<std::size_t>(lIt - itsLevels.begin());
    itsIndex[std::make_tuple(pId, tIdx, lIdx)] = i;
  }
}

SmartMet::GRID::Message* GridFilesSource::currentMessage() const
{
  auto it = itsIndex.find(std::make_tuple(itsCurrentParam, itsCurrentTime, itsCurrentLevel));
  if (it == itsIndex.end())
    return nullptr;
  return itsFile->getMessageByIndex(it->second);
}

std::vector<int> GridFilesSource::paramIds() const
{
  return itsParamIds;
}

std::string GridFilesSource::paramShortName(int paramId) const
{
  // The id we store in itsParamIds is whatever NFmiEnumConverter resolved
  // from the file's native name — i.e. it lives in the *newbase* ID
  // namespace. grid-files' FmiParameterDef table uses a *different* numeric
  // namespace (e.g. ID 13 is Humidity in newbase but "GROWDEV-D" in
  // grid-files; ID 163 is CurrentSpeed in newbase but RH-PRCNT in
  // grid-files). Looking up the id in grid-files' table mis-renders any
  // parameter whose two namespaces disagree, so display the textual name
  // we cached from the file at index time and rely on newbase as the
  // single source of truth for the id.
  auto it = std::find(itsParamIds.begin(), itsParamIds.end(), paramId);
  if (it != itsParamIds.end())
  {
    const std::size_t idx = static_cast<std::size_t>(it - itsParamIds.begin());
    if (idx < itsParamNativeNames.size() && !itsParamNativeNames[idx].empty())
      return itsParamNativeNames[idx];
  }
  NFmiEnumConverter conv;
  std::string name = conv.ToString(paramId);
  if (!name.empty())
    return name;
  return std::to_string(paramId);
}

std::string GridFilesSource::paramLongName(int paramId) const
{
  // No reliable cross-namespace long name — paramShortName already returns
  // the file's own textual name, which is the most informative thing we
  // have. (The grid-files FmiParameterDef.mParameterDescription would be
  // wrong here for the same reason paramShortName must avoid that lookup.)
  return paramShortName(paramId);
}

std::string GridFilesSource::paramUnits(int paramId) const
{
  auto it = std::find(itsParamIds.begin(), itsParamIds.end(), paramId);
  if (it == itsParamIds.end())
    return {};
  const std::size_t idx = static_cast<std::size_t>(it - itsParamIds.begin());
  return idx < itsParamUnits.size() ? itsParamUnits[idx] : std::string{};
}

int GridFilesSource::currentParamId() const
{
  return itsCurrentParam;
}

bool GridFilesSource::selectParamId(int paramId)
{
  if (std::find(itsParamIds.begin(), itsParamIds.end(), paramId) == itsParamIds.end())
    return false;
  itsCurrentParam = paramId;
  return true;
}

std::size_t GridFilesSource::timeCount() const
{
  return itsTimes.size();
}
std::size_t GridFilesSource::currentTimeIndex() const
{
  return itsCurrentTime;
}
void GridFilesSource::selectTimeIndex(std::size_t i)
{
  if (i < itsTimes.size())
    itsCurrentTime = i;
}
NFmiMetTime GridFilesSource::currentValidTime() const
{
  return itsTimes.empty() ? NFmiMetTime() : itsTimes[itsCurrentTime];
}

NFmiMetTime GridFilesSource::originTime() const
{
  // GRIB / NetCDF call this the "reference time" or "analysis time" — the
  // moment the forecast was issued. All messages in one file share it; ask
  // any message we have indexed.
  if (itsFile == nullptr || itsFile->getNumberOfMessages() == 0)
    return NFmiMetTime();
  auto* msg = itsFile->getMessageByIndex(0);
  if (msg == nullptr)
    return NFmiMetTime();
  return parseForecastTime(msg->getReferenceTime().c_str());
}

std::size_t GridFilesSource::levelCount() const
{
  return itsLevels.size();
}
std::size_t GridFilesSource::currentLevelIndex() const
{
  return itsCurrentLevel;
}
void GridFilesSource::selectLevelIndex(std::size_t i)
{
  if (i < itsLevels.size())
    itsCurrentLevel = i;
}
float GridFilesSource::levelValueAt(std::size_t i) const
{
  return i < itsLevels.size() ? itsLevels[i] : 0.0F;
}

float GridFilesSource::interpolatedValue(double lat, double lon) const
{
  auto* msg = currentMessage();
  if (msg == nullptr)
    return std::numeric_limits<float>::quiet_NaN();
  // 1 = Linear (bilinear) per T::AreaInterpolationMethod.
  // Note: unlike getGridLatLonCoordinatesByGridPosition / getGridPointBy-
  // LatLonCoordinates (whose argument ordering varies per backend — see
  // itsCoordsSwapped), getGridValueByLatLonCoordinate consistently takes
  // (lat, lon), so no swap-aware wrapper is needed here.
  return static_cast<float>(msg->getGridValueByLatLonCoordinate(lat, lon, 1));
}

bool GridFilesSource::ensureGridGeometry() const
{
  if (itsGeometryCached)
    return itsGeometryValid;
  itsGeometryCached = true;
  itsGeometryValid = false;
  if (itsFile == nullptr || itsFile->getNumberOfMessages() == 0)
    return false;
  auto* msg = itsFile->getMessageByIndex(0);
  if (msg == nullptr)
    return false;
  try
  {
    T::Dimensions d = msg->getGridDimensions();
    if (d.getDimensions() != 2)
      return false;
    itsNx = d.nx();
    itsNy = d.ny();
    if (itsNx < 2 || itsNy < 2)
      return false;

    // Detect the lat/lon argument-ordering of getGridLatLonCoordinates-
    // ByGridPosition by comparing its (0, 0) output to the bottom-left
    // corner reported by getGridLatLonArea (which uses (x=lon, y=lat) and
    // is reliable). Different grid-files backends fill the (lat, lon) out-
    // params in different orders — GRIB returns (lat, lon) per the docs,
    // NetCDF actually returns (lon, lat).
    itsCoordsSwapped = false;
    T::Coordinate tl;
    T::Coordinate tr;
    T::Coordinate bl;
    T::Coordinate br;
    if (msg->getGridLatLonArea(tl, tr, bl, br))
    {
      double a = 0;
      double b = 0;
      if (msg->getGridLatLonCoordinatesByGridPosition(0, 0, a, b))
      {
        const double dDirect = std::abs(a - bl.y()) + std::abs(b - bl.x());
        const double dSwapped = std::abs(a - bl.x()) + std::abs(b - bl.y());
        itsCoordsSwapped = (dSwapped < dDirect);
      }
    }

    double lat0 = 0;
    double lon0 = 0;
    double lat1 = 0;
    double lon1 = 0;
    if (!readGridLatLon(msg, 0, 0, lat0, lon0))
      return false;
    if (!readGridLatLon(msg, 0, itsNy - 1, lat1, lon1))
      return false;
    itsScanFromNorth = (lat0 > lat1);
    itsGeometryValid = true;
    return true;
  }
  catch (const std::exception&)
  {
    return false;
  }
}

bool GridFilesSource::readGridLatLon(SmartMet::GRID::Message* msg, double gi, double gj,
                                     double& lat, double& lon) const
{
  double a = 0;
  double b = 0;
  if (!msg->getGridLatLonCoordinatesByGridPosition(gi, gj, a, b))
    return false;
  if (itsCoordsSwapped)
  {
    lat = b;
    lon = a;
  }
  else
  {
    lat = a;
    lon = b;
  }
  return true;
}

bool GridFilesSource::lookupGridPoint(SmartMet::GRID::Message* msg, double lat, double lon,
                                      double& gi, double& gj) const
{
  if (itsCoordsSwapped)
    return msg->getGridPointByLatLonCoordinates(lon, lat, gi, gj);
  return msg->getGridPointByLatLonCoordinates(lat, lon, gi, gj);
}

void GridFilesSource::uvToLatLon(double u, double v, double& lat, double& lon) const
{
  if (!ensureGridGeometry())
  {
    DataSource::uvToLatLon(u, v, lat, lon);
    return;
  }
  auto* msg = itsFile->getMessageByIndex(0);
  // Convention: v=0 is the top of the screen = max latitude. Map v to
  // grid_j so that v=0 always lands on the northern edge regardless of
  // scan direction.
  const double gi = u * (itsNx - 1);
  const double gj = itsScanFromNorth ? v * (itsNy - 1) : (1.0 - v) * (itsNy - 1);
  try
  {
    if (readGridLatLon(msg, gi, gj, lat, lon))
      return;
  }
  catch (const std::exception&)
  {
  }
  DataSource::uvToLatLon(u, v, lat, lon);
}

void GridFilesSource::latLonToUV(double lat, double lon, double& u, double& v) const
{
  if (!ensureGridGeometry())
  {
    DataSource::latLonToUV(lat, lon, u, v);
    return;
  }
  auto* msg = itsFile->getMessageByIndex(0);
  double gi = 0;
  double gj = 0;
  try
  {
    if (!lookupGridPoint(msg, lat, lon, gi, gj))
    {
      DataSource::latLonToUV(lat, lon, u, v);
      return;
    }
  }
  catch (const std::exception&)
  {
    DataSource::latLonToUV(lat, lon, u, v);
    return;
  }
  u = gi / (itsNx - 1);
  v = itsScanFromNorth ? gj / (itsNy - 1) : 1.0 - gj / (itsNy - 1);
}

namespace
{
const char* gridProjectionName(T::GridProjection p)
{
  using GP = T::GridProjectionValue;
  switch (p)
  {
    case GP::LatLon: return "LatLon";
    case GP::RotatedLatLon: return "RotatedLatLon";
    case GP::StretchedLatLon: return "StretchedLatLon";
    case GP::StretchedRotatedLatLon: return "StretchedRotatedLatLon";
    case GP::VariableResolutionLatLon: return "VariableResolutionLatLon";
    case GP::VariableResolutionRotatedLatLon: return "VariableResolutionRotatedLatLon";
    case GP::Mercator: return "Mercator";
    case GP::TransverseMercator: return "TransverseMercator";
    case GP::PolarStereographic: return "PolarStereographic";
    case GP::LambertConformal: return "LambertConformal";
    case GP::ObliqueLambertConformal: return "ObliqueLambertConformal";
    case GP::Albers: return "Albers";
    case GP::Gaussian: return "Gaussian";
    case GP::RotatedGaussian: return "RotatedGaussian";
    case GP::SpaceView: return "SpaceView";
    case GP::IrregularLatLon: return "IrregularLatLon";
    case GP::LambertAzimuthalEqualArea: return "LambertAzimuthalEqualArea";
    default: return "Unknown";
  }
}
}  // namespace

std::vector<std::pair<std::string, std::string>> GridFilesSource::extraMetadata() const
{
  std::vector<std::pair<std::string, std::string>> rows;
  if (itsFile == nullptr || itsFile->getNumberOfMessages() == 0) return rows;
  // grid-files' GridFile::getFileType() returns Unknown for the formats we
  // care about, so re-detect from the filename + magic bytes. Reads the
  // first 4 bytes; cheap and exact.
  const std::string& path = itsFile->getFileName();
  std::ifstream in(path, std::ios::binary);
  unsigned char hdr[4] = {};
  in.read(reinterpret_cast<char*>(hdr), sizeof(hdr));
  std::string format = "grid-files";
  if (hdr[0] == 'G' && hdr[1] == 'R' && hdr[2] == 'I' && hdr[3] == 'B')
  {
    format = "GRIB";
    // The edition byte is at offset 7 in both GRIB1 and GRIB2 headers.
    in.seekg(7, std::ios::beg);
    char edition = 0;
    if (in.read(&edition, 1))
    {
      if (edition == 1) format = "GRIB1";
      else if (edition == 2) format = "GRIB2";
    }
  }
  else if (hdr[0] == 'C' && hdr[1] == 'D' && hdr[2] == 'F') format = "NetCDF3";
  else if (hdr[0] == 0x89 && hdr[1] == 'H' && hdr[2] == 'D' && hdr[3] == 'F') format = "NetCDF4";
  rows.emplace_back("Format", format);
  rows.emplace_back("Messages", std::to_string(itsFile->getNumberOfMessages()));
  auto* msg = itsFile->getMessageByIndex(0);
  if (msg != nullptr)
  {
    try
    {
      rows.emplace_back("Grid", gridProjectionName(msg->getGridProjection()));
    }
    catch (const std::exception&)
    {
    }
    if (ensureGridGeometry())
      rows.emplace_back("Grid size", std::to_string(itsNx) + "x" + std::to_string(itsNy));
  }
  return rows;
}

std::string GridFilesSource::gridSignature() const
{
  if (itsFile == nullptr || itsFile->getNumberOfMessages() == 0)
    return DataSource::gridSignature();
  auto* msg = itsFile->getMessageByIndex(0);
  if (msg == nullptr) return DataSource::gridSignature();
  std::string proj;
  try
  {
    proj = msg->getWKT();
  }
  catch (const std::exception&)
  {
  }
  if (proj.empty()) proj = gridProjectionName(msg->getGridProjection());
  ensureGridGeometry();
  return std::string("grid:") + proj + "|" + std::to_string(itsNx) + "x" + std::to_string(itsNy);
}

LatLonBox GridFilesSource::boundingBox() const
{
  LatLonBox b;
  if (!ensureGridGeometry())
  {
    // Fall back to the corner-only bbox, which is fine for grids whose
    // longitudes don't wrap. For wrapping global grids this reduces to a
    // sliver — the override of uvToLatLon / latLonToUV above means callers
    // shouldn't see that bbox in the rendering pipeline anyway.
    if (itsFile == nullptr || itsFile->getNumberOfMessages() == 0)
      return b;
    auto* msg = itsFile->getMessageByIndex(0);
    T::Coordinate tl;
    T::Coordinate tr;
    T::Coordinate bl;
    T::Coordinate br;
    if (!msg->getGridLatLonArea(tl, tr, bl, br))
      return b;
    b.minLat = std::min({tl.y(), tr.y(), bl.y(), br.y()});
    b.maxLat = std::max({tl.y(), tr.y(), bl.y(), br.y()});
    b.minLon = std::min({tl.x(), tr.x(), bl.x(), br.x()});
    b.maxLon = std::max({tl.x(), tr.x(), bl.x(), br.x()});
    return b;
  }
  auto* msg = itsFile->getMessageByIndex(0);

  // Walk the perimeter as one continuous closed loop, sampling lat/lon at
  // each step. A single running longitude-unwrap state spans all four
  // edges so that consecutive points always land adjacent on the grid:
  // grid-files wraps lon output to [-180, 180], so a global grid that
  // starts at the antimeridian gives e.g. 180 → -179.875 across the
  // bottom row, looking like a 0.25° span when it's really 359.875°.
  // Detecting >180° jumps and adding ±360 reconstructs the contiguous
  // sequence we can min/max. Per-run reset would break the corner
  // transitions (top row ends near 180°, right edge starts near -180°)
  // and miss the wrap.
  double minLat = std::numeric_limits<double>::infinity();
  double maxLat = -std::numeric_limits<double>::infinity();
  double minLon = std::numeric_limits<double>::infinity();
  double maxLon = -std::numeric_limits<double>::infinity();
  double prevLon = 0;
  bool havePrev = false;

  constexpr int kSamples = 60;
  auto sample = [&](double gi, double gj)
  {
    double lat = 0;
    double lon = 0;
    try
    {
      if (!readGridLatLon(msg, gi, gj, lat, lon)) return;
    }
    catch (const std::exception&)
    {
      return;
    }
    minLat = std::min(minLat, lat);
    maxLat = std::max(maxLat, lat);
    if (havePrev)
    {
      while (lon - prevLon > 180.0)
        lon -= 360.0;
      while (prevLon - lon > 180.0)
        lon += 360.0;
    }
    prevLon = lon;
    havePrev = true;
    minLon = std::min(minLon, lon);
    maxLon = std::max(maxLon, lon);
  };
  const double lastI = static_cast<double>(itsNx - 1);
  const double lastJ = static_cast<double>(itsNy - 1);
  // Closed perimeter in CW order: left edge ↓, bottom edge →, right edge ↑,
  // top edge ←. Repeat the start corner so we exit unwrap-consistent.
  for (int i = 0; i <= kSamples; ++i)
    sample(0.0, static_cast<double>(i) / kSamples * lastJ);
  for (int i = 1; i <= kSamples; ++i)
    sample(static_cast<double>(i) / kSamples * lastI, lastJ);
  for (int i = 1; i <= kSamples; ++i)
    sample(lastI, lastJ - static_cast<double>(i) / kSamples * lastJ);
  for (int i = 1; i <= kSamples; ++i)
    sample(lastI - static_cast<double>(i) / kSamples * lastI, 0.0);

  if (!std::isfinite(minLat) || !std::isfinite(minLon))
    return b;
  // Shift the unwrapped lon range so that it sits in or close to [-180,
  // 180]. Callers (and grid-files itself) accept arbitrary lon values
  // since `getGridPointByLatLonCoordinates` handles both wraps, but a
  // canonical range keeps overlay code (place names, coastlines) happy.
  while (minLon >= 180.0)
  {
    minLon -= 360.0;
    maxLon -= 360.0;
  }
  while (minLon < -180.0)
  {
    minLon += 360.0;
    maxLon += 360.0;
  }
  b.minLat = minLat;
  b.maxLat = maxLat;
  b.minLon = minLon;
  b.maxLon = maxLon;
  return b;
}
}  // namespace Qdless
