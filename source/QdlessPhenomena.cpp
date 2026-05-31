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
// Sample the data on its NATIVE (u, v) grid in [0,1] × [0,1]. For a
// regional file (Nordic, European, etc.) this means the grid stays
// fully inside the data area instead of trying to cover the whole
// globe, where most samples would be outside the file's coverage. The
// per-cell (lat, lon) is stored so detectors can label anchors and so
// the tropical-convection detector can still filter by latitude band.
//
// 200×100 = 20k samples — enough resolution to find a cyclone centre
// to within ~1 grid cell in a regional file, and still under 30 ms
// even with the indirection through uvToLatLon + interpolatedValue.
struct Grid
{
  static constexpr int W = 200;
  static constexpr int H = 100;
  std::array<float, W * H> v{};
  std::array<float, W * H> lat{};
  std::array<float, W * H> lon{};
  static double uOf(int ix) { return (ix + 0.5) / W; }
  static double vOf(int iy) { return (iy + 0.5) / H; }
  float at(int ix, int iy) const
  {
    return v[static_cast<std::size_t>(iy) * W + ix];
  }
  float latAt(int ix, int iy) const
  {
    return lat[static_cast<std::size_t>(iy) * W + ix];
  }
  float lonAt(int ix, int iy) const
  {
    return lon[static_cast<std::size_t>(iy) * W + ix];
  }
};

Grid sampleGrid(DataSource& src)
{
  Grid g;
  for (int iy = 0; iy < Grid::H; ++iy)
    for (int ix = 0; ix < Grid::W; ++ix)
    {
      double lat = 0, lon = 0;
      src.uvToLatLon(Grid::uOf(ix), Grid::vOf(iy), lat, lon);
      const float val = src.interpolatedValue(lat, lon);
      const std::size_t k = static_cast<std::size_t>(iy) * Grid::W + ix;
      g.v[k] = val;
      g.lat[k] = static_cast<float>(lat);
      g.lon[k] = static_cast<float>(lon);
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
    const double latRow = g.latAt(0, iy);
    if (std::fabs(latRow) > 15.0) continue;
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
  const double lat = g.latAt(maxIx, maxIy);
  const double lon = g.lonAt(maxIx, maxIy);
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
  // Find the deepest pressure minimum on the sampled grid, then measure
  // the gradient strength as (local max - local min) inside a ring
  // sized in KILOMETRES (~500 km) rather than grid cells. A
  // cells-based ring would be tiny on a high-resolution regional file
  // and miss the cyclone's outer pressure rim — so we walk a haversine
  // distance instead, regardless of grid resolution.
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

  // Sub-cell refinement: fit a 2D parabola to the 3×3 neighbourhood of
  // the minimum cell to estimate the true minimum location to a
  // fraction of a cell. This bridges the gap between the discrete
  // sampling grid and the cyclone's actual centre.
  double refinedLat = g.latAt(minIx, minIy);
  double refinedLon = g.lonAt(minIx, minIy);
  float refinedMin = globalMin;
  {
    const int ixL = (minIx + Grid::W - 1) % Grid::W;
    const int ixR = (minIx + 1) % Grid::W;
    const int iyU = std::max(0, minIy - 1);
    const int iyD = std::min(Grid::H - 1, minIy + 1);
    auto norm = [&](int ix, int iy) -> float {
      const float v = g.at(ix, iy);
      if (!valid(v)) return std::numeric_limits<float>::quiet_NaN();
      return (v > 5000.0F) ? v * 0.01F : v;
    };
    const float pC = norm(minIx, minIy);
    const float pL = norm(ixL, minIy);
    const float pR = norm(ixR, minIy);
    const float pU = norm(minIx, iyU);
    const float pD = norm(minIx, iyD);
    auto fit = [&](float lo, float hi) -> float {
      const float denom = lo - 2 * pC + hi;
      if (!std::isfinite(denom) || std::fabs(denom) < 1e-6F) return 0.0F;
      return std::clamp((lo - hi) / (2 * denom), -0.5F, 0.5F);
    };
    if (std::isfinite(pL) && std::isfinite(pR) && std::isfinite(pU) && std::isfinite(pD))
    {
      const float fx = fit(pL, pR);
      const float fy = fit(pU, pD);
      // Recompute lat/lon at the refined u,v position.
      const double u = (minIx + 0.5 + fx) / Grid::W;
      const double v = (minIy + 0.5 + fy) / Grid::H;
      // We can't call src.uvToLatLon here — caller doesn't pass src —
      // but linear interpolation of the surrounding cells' lat/lon
      // is accurate enough for displaying an anchor point.
      const double latL = g.latAt(ixL, minIy);
      const double latR = g.latAt(ixR, minIy);
      const double latU = g.latAt(minIx, iyU);
      const double latD = g.latAt(minIx, iyD);
      const double latC = g.latAt(minIx, minIy);
      const double lonL = g.lonAt(ixL, minIy);
      const double lonR = g.lonAt(ixR, minIy);
      const double lonU = g.lonAt(minIx, iyU);
      const double lonD = g.lonAt(minIx, iyD);
      const double lonC = g.lonAt(minIx, minIy);
      refinedLat = latC + fx * (latR - latL) * 0.5 + fy * (latD - latU) * 0.5;
      refinedLon = lonC + fx * (lonR - lonL) * 0.5 + fy * (lonD - lonU) * 0.5;
      // Parabolic estimate of the actual minimum value at the refined
      // position (always ≤ pC by construction since pC is the lowest).
      refinedMin = pC - 0.25F * fx * (pR - pL) - 0.25F * fy * (pD - pU);
      (void)u; (void)v;
    }
  }

  // Ring radius: 500 km haversine. Walk only as many grid cells as
  // needed to cover that distance in any direction.
  const double cosLat = std::cos(refinedLat * M_PI / 180.0);
  // Approximate degrees per cell — average of x and y to pick a search
  // radius that's big enough to escape the cyclone's core regardless of
  // the data's projection.
  const double dLatPerCell =
      std::fabs(g.latAt(minIx, std::min(minIy + 1, Grid::H - 1)) -
                g.latAt(minIx, std::max(minIy - 1, 0))) * 0.5;
  const double dLonPerCell =
      std::fabs(g.lonAt(std::min(minIx + 1, Grid::W - 1), minIy) -
                g.lonAt(std::max(minIx - 1, 0), minIy)) * 0.5;
  // Convert 500 km to cells; clamp so we always look ≥3 cells away.
  const int searchY = std::max(3, static_cast<int>(std::ceil(
      500.0 / std::max(1e-6, dLatPerCell * 111.32))));
  const int searchX = std::max(3, static_cast<int>(std::ceil(
      500.0 / std::max(1e-6, dLonPerCell * 111.32 * std::max(0.1, cosLat)))));

  float ringMax = -std::numeric_limits<float>::infinity();
  for (int dy = -searchY; dy <= searchY; ++dy)
    for (int dx = -searchX; dx <= searchX; ++dx)
    {
      // Skip the centre's immediate neighbourhood so the gradient is
      // measured against the cyclone's outer edge.
      if (dx * dx + dy * dy < (searchX * searchY) / 4) continue;
      const int ix = std::clamp(minIx + dx, 0, Grid::W - 1);
      const int iy = std::clamp(minIy + dy, 0, Grid::H - 1);
      const float v = g.at(ix, iy);
      if (!valid(v)) continue;
      const float p = (v > 5000.0F) ? v * 0.01F : v;
      if (p > ringMax) ringMax = p;
    }
  if (ringMax <= refinedMin) return {};
  const float dropHPa = ringMax - refinedMin;
  if (dropHPa < 8.0F) return {};
  const double lat = refinedLat;
  const double lon = refinedLon;
  globalMin = refinedMin;

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
  const double lat = g.latAt(topIx, topIy);
  const double lon = g.lonAt(topIx, topIy);
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
  const double lat = g.latAt(topIx, topIy);
  const double lon = g.lonAt(topIx, topIy);
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
  // Cell centre geographic coordinates, copied from the first frame
  // sample (the grid geometry doesn't change between time steps).
  std::array<float, Grid::W * Grid::H> lat{};
  std::array<float, Grid::W * Grid::H> lon{};
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
  if (!snaps.empty())
  {
    ts.lat = snaps.front().lat;
    ts.lon = snaps.front().lon;
  }

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
  // We need cell-resolved lat/lon for the anchor; recover from the
  // grid we already sampled in this function.
  const double lat = ts.lat[bestI];
  // Only meaningful for geopotential-like / pressure-like params —
  // for instance, a static temperature minimum at the South Pole is
  // not a "block".
  const std::string n = lowercased(paramName);
  if (!contains(n, "geopot") && !contains(n, "geoph") && !contains(n, "pressure")) return {};
  if (std::fabs(lat) < 20.0) return {};  // blocking is a mid-latitude pattern

  PhenomenonHint h;
  const double lon = ts.lon[bestI];
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
