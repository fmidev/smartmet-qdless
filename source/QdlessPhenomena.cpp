#include "QdlessPhenomena.h"

#include "QdlessDataSource.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <newbase/NFmiGlobals.h>  // kFloatMissing

namespace Qdless
{
namespace
{
std::string lowercased(const std::string& s)
{
  std::string out;
  out.reserve(s.size());
  for (char c : s)
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  return out;
}

bool contains(const std::string& haystack, const std::string& needle)
{
  return haystack.find(needle) != std::string::npos;
}

bool isPrecipitationParam(const std::string& name)
{
  const std::string n = lowercased(name);
  // FMI names: Precipitation1h, Precipitation3h, PrecipitationAmount,
  // TotalPrecipitation, PrecipitationRate, ConvectivePrecipitation.
  return contains(n, "precip");
}

bool isOlrOrCloudParam(const std::string& name)
{
  const std::string n = lowercased(name);
  return contains(n, "outgoing") || contains(n, "olr") ||
         contains(n, "longwave") || contains(n, "cloudcover") ||
         contains(n, "tcc") || contains(n, "totalcloud");
}

bool isPressureMslParam(const std::string& name)
{
  const std::string n = lowercased(name);
  // Match Pressure, PressureAtStationLevel, MeanSeaLevelPressure,
  // ReducedPressure, SurfacePressure — but NOT geopotential or
  // pressure-as-a-vertical-coordinate.
  if (!contains(n, "pressure")) return false;
  if (contains(n, "geopot") || contains(n, "level")) return contains(n, "stationlevel");
  return true;
}

bool isTemperatureParam(const std::string& name)
{
  const std::string n = lowercased(name);
  return contains(n, "temperature") || contains(n, "theta") ||
         contains(n, "potentialtemp");
}

bool isWindSpeedParam(const std::string& name)
{
  const std::string n = lowercased(name);
  return (contains(n, "wind") && contains(n, "speed")) ||
         contains(n, "windspeed") || contains(n, "windkt");
}

bool isGeopotentialParam(const std::string& name)
{
  const std::string n = lowercased(name);
  return contains(n, "geopot") || contains(n, "geoph") ||
         contains(n, "zh") || contains(n, "z500");
}

// Sample the current (time, level) of `src` onto a coarse global
// (-180..180, -90..90) lat/lon grid. The grid is dense enough for the
// detectors but sparse enough that the whole sweep runs in a few ms.
struct Grid
{
  static constexpr int W = 72;   // 5° longitude resolution
  static constexpr int H = 36;   // 5° latitude resolution
  std::array<float, W * H> v{};  // kFloatMissing where the source returned nothing
  static double lonOf(int ix) { return -180.0 + (ix + 0.5) * 360.0 / W; }
  static double latOf(int iy) { return -90.0 + (iy + 0.5) * 180.0 / H; }
  float at(int ix, int iy) const
  {
    return v[static_cast<std::size_t>(iy) * W + ix];
  }
};

Grid sampleGrid(DataSource& src)
{
  Grid g;
  for (int iy = 0; iy < Grid::H; ++iy)
    for (int ix = 0; ix < Grid::W; ++ix)
    {
      const float val = src.interpolatedValue(Grid::latOf(iy), Grid::lonOf(ix));
      g.v[static_cast<std::size_t>(iy) * Grid::W + ix] = val;
    }
  return g;
}

bool valid(float v) { return v != kFloatMissing && std::isfinite(v) && std::fabs(v) < 1e6F; }

// Detector 1: tropical convection / MJO ----------------------------------
PhenomenonHint detectTropicalConvection(const Grid& g, const std::string& units)
{
  // Tropical band: ±15° latitude.
  double sum = 0;
  int n = 0;
  float maxV = -std::numeric_limits<float>::infinity();
  int maxIx = -1, maxIy = -1;
  for (int iy = 0; iy < Grid::H; ++iy)
  {
    const double lat = Grid::latOf(iy);
    if (std::fabs(lat) > 15.0) continue;
    for (int ix = 0; ix < Grid::W; ++ix)
    {
      const float v = g.at(ix, iy);
      if (!valid(v)) continue;
      sum += v;
      ++n;
      if (v > maxV)
      {
        maxV = v;
        maxIx = ix;
        maxIy = iy;
      }
    }
  }
  if (n < 50 || maxIx < 0) return {};
  const double mean = sum / n;
  // Convection: max value at least 3× the mean and 5× the threshold for
  // "drizzle" (0.1 in the field's units). The 3× multiple weeds out
  // genuinely flat fields.
  if (maxV < std::max(0.5, mean * 3.0)) return {};

  PhenomenonHint h;
  const double lat = Grid::latOf(maxIy);
  const double lon = Grid::lonOf(maxIx);
  h.message =
      "Tropical convection peak " +
      std::to_string(static_cast<int>(maxV)) + units +
      " near " + std::to_string(static_cast<int>(std::fabs(lat))) +
      (lat < 0 ? "°S " : "°N ") +
      std::to_string(static_cast<int>(std::fabs(lon))) +
      (lon < 0 ? "°W" : "°E");
  h.suggestion = "→ X for cross-section along the equator, then H for Hovmöller";
  h.score = 80;
  h.anchorLat = lat;
  h.anchorLon = lon;
  h.hasAnchor = true;
  return h;
}

// Detector 2: cyclones / hurricanes --------------------------------------
PhenomenonHint detectCyclone(const Grid& g)
{
  // Scan for the deepest pressure minimum and measure the
  // surrounding (max - min) in hPa. Strong synoptic cyclone: > 15 hPa
  // drop in a ~10° radius. Hurricane in MSLP: > 30 hPa.
  float globalMin = std::numeric_limits<float>::infinity();
  int minIx = -1, minIy = -1;
  for (int iy = 0; iy < Grid::H; ++iy)
    for (int ix = 0; ix < Grid::W; ++ix)
    {
      const float v = g.at(ix, iy);
      if (!valid(v)) continue;
      // FMI stores pressure either in Pa (~100000) or hPa (~1000).
      // Normalise so 'globalMin' is always in hPa.
      const float p = (v > 5000.0F) ? v * 0.01F : v;
      if (p < globalMin)
      {
        globalMin = p;
        minIx = ix;
        minIy = iy;
      }
    }
  if (minIx < 0) return {};
  // Sample a ring 4 grid cells (~20°) around the minimum and take the
  // local max for the gradient strength.
  float ringMax = -std::numeric_limits<float>::infinity();
  for (int dy = -4; dy <= 4; ++dy)
    for (int dx = -4; dx <= 4; ++dx)
    {
      if (std::abs(dx) + std::abs(dy) < 3) continue;  // skip the centre
      const int ix = (minIx + dx + Grid::W) % Grid::W;
      const int iy = std::clamp(minIy + dy, 0, Grid::H - 1);
      const float v = g.at(ix, iy);
      if (!valid(v)) continue;
      const float p = (v > 5000.0F) ? v * 0.01F : v;
      if (p > ringMax) ringMax = p;
    }
  if (ringMax <= globalMin) return {};
  const float dropHPa = ringMax - globalMin;
  if (dropHPa < 8.0F) return {};
  const double lat = Grid::latOf(minIy);
  const double lon = Grid::lonOf(minIx);

  PhenomenonHint h;
  const char* sev = (dropHPa > 30.0F) ? "Hurricane-strength" :
                    (dropHPa > 15.0F) ? "Strong cyclone"     :
                                        "Cyclone";
  char buf[160];
  std::snprintf(buf, sizeof(buf),
                "%s low %d hPa near %d°%c %d°%c (Δ %d hPa)",
                sev, static_cast<int>(globalMin),
                static_cast<int>(std::fabs(lat)), lat < 0 ? 'S' : 'N',
                static_cast<int>(std::fabs(lon)), lon < 0 ? 'W' : 'E',
                static_cast<int>(dropHPa));
  h.message = buf;
  h.suggestion = "→ click the centre to time-series-probe; spacebar to animate";
  h.score = (dropHPa > 30.0F) ? 95 : (dropHPa > 15.0F) ? 85 : 70;
  h.anchorLat = lat;
  h.anchorLon = lon;
  h.hasAnchor = true;
  return h;
}

// Detector 3: front (sharp temperature gradient) -------------------------
PhenomenonHint detectFront(const Grid& g, const std::string& units)
{
  // Central-difference gradient magnitude per grid cell. Look at the
  // 99th-percentile cell; if it's well above the 95th, a sharp band
  // exists somewhere.
  std::vector<float> mag;
  mag.reserve(Grid::W * Grid::H);
  float topMag = 0;
  int topIx = -1, topIy = -1;
  for (int iy = 1; iy < Grid::H - 1; ++iy)
    for (int ix = 0; ix < Grid::W; ++ix)
    {
      const int ixp = (ix + 1) % Grid::W;
      const int ixm = (ix - 1 + Grid::W) % Grid::W;
      const float vL = g.at(ixm, iy);
      const float vR = g.at(ixp, iy);
      const float vU = g.at(ix, iy - 1);
      const float vD = g.at(ix, iy + 1);
      if (!valid(vL) || !valid(vR) || !valid(vU) || !valid(vD)) continue;
      const float dx = vR - vL;
      const float dy = vD - vU;
      const float m = std::sqrt(dx * dx + dy * dy);
      mag.push_back(m);
      if (m > topMag)
      {
        topMag = m;
        topIx = ix;
        topIy = iy;
      }
    }
  if (mag.size() < 100 || topIx < 0) return {};
  std::sort(mag.begin(), mag.end());
  const float p95 = mag[mag.size() * 95 / 100];
  const float p99 = mag[mag.size() * 99 / 100];
  // Need a clear gradient AND a clear top-of-distribution outlier.
  if (p95 < 5.0F || topMag < p99 * 1.4F) return {};
  const double lat = Grid::latOf(topIy);
  const double lon = Grid::lonOf(topIx);
  PhenomenonHint h;
  char buf[160];
  std::snprintf(buf, sizeof(buf),
                "Sharp temperature gradient (~%d%s per 10°) near %d°%c %d°%c",
                static_cast<int>(topMag), units.c_str(),
                static_cast<int>(std::fabs(lat)), lat < 0 ? 'S' : 'N',
                static_cast<int>(std::fabs(lon)), lon < 0 ? 'W' : 'E');
  h.message = buf;
  h.suggestion = "→ X for a cross-section across the front";
  h.score = 70;
  h.anchorLat = lat;
  h.anchorLon = lon;
  h.hasAnchor = true;
  return h;
}

// Detector 4: jet stream (upper-level wind speed) ------------------------
PhenomenonHint detectJet(DataSource& src, const Grid& g, const std::string& units)
{
  // Only meaningful if we're in the upper atmosphere — check the
  // currently-selected level. Pressure-level files typically expose
  // 100/150/200/250/300/400/500/... hPa via levelValueAt().
  const std::size_t li = src.currentLevelIndex();
  if (li >= src.levelCount()) return {};
  const float lv = src.levelValueAt(li);
  // For pressure-level files: jet criteria apply at ≤350 hPa. For files
  // that don't expose pressure on the level axis (e.g. surface wind),
  // we still look for unusually fast winds — the threshold is just
  // higher so the surface wind doesn't trigger by default.
  const bool upper = (lv > 0 && lv <= 400.0F);
  const float threshold = upper ? 40.0F : 60.0F;

  float topV = -std::numeric_limits<float>::infinity();
  int topIx = -1, topIy = -1;
  double sum = 0;
  int n = 0;
  for (int iy = 0; iy < Grid::H; ++iy)
    for (int ix = 0; ix < Grid::W; ++ix)
    {
      const float v = g.at(ix, iy);
      if (!valid(v)) continue;
      sum += v;
      ++n;
      if (v > topV)
      {
        topV = v;
        topIx = ix;
        topIy = iy;
      }
    }
  if (topIx < 0 || topV < threshold) return {};
  const double lat = Grid::latOf(topIy);
  const double lon = Grid::lonOf(topIx);
  PhenomenonHint h;
  char buf[160];
  std::snprintf(buf, sizeof(buf),
                "Jet stream %d %s at %d°%c %d°%c (level %d hPa)",
                static_cast<int>(topV), units.c_str(),
                static_cast<int>(std::fabs(lat)), lat < 0 ? 'S' : 'N',
                static_cast<int>(std::fabs(lon)), lon < 0 ? 'W' : 'E',
                static_cast<int>(lv));
  h.message = buf;
  h.suggestion = upper ? "→ L to browse other upper levels (200/300/500 hPa)"
                       : "→ L to switch to upper levels";
  h.score = upper ? 80 : 50;
  h.anchorLat = lat;
  h.anchorLon = lon;
  h.hasAnchor = true;
  return h;
}

// Detector 5/6: blocks and static field need multi-time sampling --------
struct TemporalStats
{
  bool ok = false;
  // Per-cell variance across time and the highest time-mean value.
  std::array<float, Grid::W * Grid::H> mean{};
  std::array<float, Grid::W * Grid::H> stdev{};
  float globalMean = 0;
  float globalStdev = 0;
};

TemporalStats sampleTemporal(DataSource& src)
{
  TemporalStats ts;
  const std::size_t nt = src.timeCount();
  if (nt < 3) return ts;
  const std::size_t saved = src.currentTimeIndex();
  // Cap at 8 time samples evenly spread — keeps the cost bounded on
  // long files (60-hour forecasts have 60+ steps).
  const std::size_t nSamples = std::min<std::size_t>(8, nt);
  std::vector<Grid> snaps;
  snaps.reserve(nSamples);
  for (std::size_t k = 0; k < nSamples; ++k)
  {
    const std::size_t ti = (k * (nt - 1)) / std::max<std::size_t>(1, nSamples - 1);
    src.selectTimeIndex(ti);
    snaps.push_back(sampleGrid(src));
  }
  src.selectTimeIndex(saved);

  double gSum = 0, gSqSum = 0;
  long gCount = 0;
  for (int i = 0; i < Grid::W * Grid::H; ++i)
  {
    double sm = 0;
    double sq = 0;
    int c = 0;
    for (const auto& sn : snaps)
    {
      const float v = sn.v[i];
      if (!valid(v)) continue;
      sm += v;
      sq += v * v;
      ++c;
    }
    if (c < 2)
    {
      ts.mean[i] = kFloatMissing;
      ts.stdev[i] = kFloatMissing;
      continue;
    }
    const double mu = sm / c;
    const double var = std::max(0.0, sq / c - mu * mu);
    ts.mean[i] = static_cast<float>(mu);
    ts.stdev[i] = static_cast<float>(std::sqrt(var));
    gSum += mu;
    gSqSum += mu * mu;
    ++gCount;
  }
  if (gCount > 1)
  {
    const double mu = gSum / gCount;
    ts.globalMean = static_cast<float>(mu);
    ts.globalStdev = static_cast<float>(std::sqrt(std::max(0.0, gSqSum / gCount - mu * mu)));
    ts.ok = true;
  }
  return ts;
}

PhenomenonHint detectBlock(const TemporalStats& ts, const std::string& paramName)
{
  if (!ts.ok) return {};
  // Look for a grid cell where the time-mean is high (top 10 %) AND
  // the time-stdev is low (bottom 25 %). That's a persistent ridge.
  std::vector<float> meanList;
  std::vector<float> stdList;
  meanList.reserve(Grid::W * Grid::H);
  stdList.reserve(Grid::W * Grid::H);
  for (int i = 0; i < Grid::W * Grid::H; ++i)
  {
    if (!valid(ts.mean[i]) || !valid(ts.stdev[i])) continue;
    meanList.push_back(ts.mean[i]);
    stdList.push_back(ts.stdev[i]);
  }
  if (meanList.size() < 100) return {};
  std::sort(meanList.begin(), meanList.end());
  std::sort(stdList.begin(), stdList.end());
  const float p90Mean = meanList[meanList.size() * 9 / 10];
  const float p25Std = stdList[stdList.size() / 4];

  int bestI = -1;
  float bestMean = 0;
  for (int i = 0; i < Grid::W * Grid::H; ++i)
  {
    if (!valid(ts.mean[i]) || !valid(ts.stdev[i])) continue;
    if (ts.mean[i] >= p90Mean && ts.stdev[i] <= p25Std && ts.mean[i] > bestMean)
    {
      bestI = i;
      bestMean = ts.mean[i];
    }
  }
  if (bestI < 0) return {};
  const int iy = bestI / Grid::W;
  const int ix = bestI % Grid::W;
  const double lat = Grid::latOf(iy);
  // Only meaningful for geopotential-like / pressure-like params —
  // for instance, a static temperature minimum at the South Pole is
  // not a "block".
  const std::string n = lowercased(paramName);
  if (!contains(n, "geopot") && !contains(n, "geoph") && !contains(n, "pressure")) return {};
  if (std::fabs(lat) < 20.0) return {};  // blocking is a mid-latitude pattern

  PhenomenonHint h;
  const double lon = Grid::lonOf(ix);
  char buf[160];
  std::snprintf(buf, sizeof(buf),
                "Persistent high-pressure ridge near %d°%c %d°%c (low temporal variance)",
                static_cast<int>(std::fabs(lat)), lat < 0 ? 'S' : 'N',
                static_cast<int>(std::fabs(lon)), lon < 0 ? 'W' : 'E');
  h.message = buf;
  h.suggestion = "→ spacebar to time-loop and see the block hold its position";
  h.score = 60;
  h.anchorLat = lat;
  h.anchorLon = lon;
  h.hasAnchor = true;
  return h;
}

PhenomenonHint detectStatic(const TemporalStats& ts)
{
  if (!ts.ok) return {};
  // If the global mean's stdev (across cells) is tiny AND the per-cell
  // temporal stdev is also tiny, the field is essentially flat in time
  // AND space — a Hovmöller will just be a single colour.
  if (ts.globalStdev > 1e-4F && ts.globalStdev / std::max(1e-6F, std::fabs(ts.globalMean)) > 0.01F)
    return {};
  // Tighter check on per-cell stdev: bottom 90 % all under 1 % of mean.
  std::vector<float> stdList;
  stdList.reserve(Grid::W * Grid::H);
  for (int i = 0; i < Grid::W * Grid::H; ++i)
    if (valid(ts.stdev[i])) stdList.push_back(ts.stdev[i]);
  if (stdList.size() < 100) return {};
  std::sort(stdList.begin(), stdList.end());
  const float p90 = stdList[stdList.size() * 9 / 10];
  if (p90 > std::fabs(ts.globalMean) * 0.005F) return {};

  PhenomenonHint h;
  h.message = "Field is essentially static across the loaded times";
  h.suggestion = "→ Hovmöller will be flat — try a longer time range or a different parameter";
  h.score = 30;  // low score so a real phenomenon wins
  return h;
}
}  // namespace

PhenomenonHint detectPhenomena(DataSource& src)
{
  const int paramId = src.currentParamId();
  if (paramId < 0) return {};
  const std::string name = src.paramShortName(paramId);
  if (name.empty()) return {};
  const Grid g = sampleGrid(src);

  std::vector<PhenomenonHint> hints;
  hints.reserve(8);

  if (isPrecipitationParam(name) || isOlrOrCloudParam(name))
  {
    const std::string units = isPrecipitationParam(name) ? "mm" : "";
    hints.push_back(detectTropicalConvection(g, units));
  }
  if (isPressureMslParam(name))
    hints.push_back(detectCyclone(g));
  if (isTemperatureParam(name))
    hints.push_back(detectFront(g, "K"));
  if (isWindSpeedParam(name))
    hints.push_back(detectJet(src, g, "m/s"));

  // The temporal detectors are cheap when timeCount() is small; they
  // self-skip on single-frame files.
  if (src.timeCount() >= 3 &&
      (isGeopotentialParam(name) || isPressureMslParam(name) || isTemperatureParam(name)))
  {
    const TemporalStats ts = sampleTemporal(src);
    if (isGeopotentialParam(name) || isPressureMslParam(name))
      hints.push_back(detectBlock(ts, name));
    hints.push_back(detectStatic(ts));
  }

  // Pick the highest-scoring detection above 0.
  PhenomenonHint best;
  for (const auto& h : hints)
    if (h.score > best.score) best = h;
  return best;
}
}  // namespace Qdless
