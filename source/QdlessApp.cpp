#include "QdlessApp.h"

#include "QdlessUI.h"

#include <newbase/NFmiEnumConverter.h>
#include <newbase/NFmiGlobals.h>
#include <newbase/NFmiPoint.h>

#include <imagine/NFmiColorTools.h>
#include <imagine/NFmiImage.h>

#include <ncurses.h>

#include <boost/dll/runtime_symbol_info.hpp>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <json/json.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <sys/ioctl.h>
#include <unistd.h>

namespace Qdless
{
namespace
{
struct TerminalSize
{
  int cols;
  int rows;
};

TerminalSize terminalSize()
{
  // NOLINTNEXTLINE(misc-include-cleaner)
  struct winsize ws{};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0)
    return {ws.ws_col, ws.ws_row};
  return {80, 24};
}

// Built-in fallback when qdless.conf isn't installed. Mirrors cnf/qdless.conf;
// keep them in sync. Lookup is case-insensitive.
const std::vector<std::pair<const char*, const char*>>& builtinPaletteMap()
{
  static const std::vector<std::pair<const char*, const char*>> kMap = {
      {"T", "temperature"},
      {"Temperature", "temperature"},
      {"T-K", "temperature"},
      {"T2m", "temperature"},
      {"TemperatureF", "temperature"},
      {"DewPoint", "temperature"},
      {"TD", "temperature"},
      {"GroundTemperature", "temperature"},
      {"TSEA", "seatemperature"},
      {"TSEA-C", "seatemperature"},
      {"TemperatureSea", "seatemperature"},
      {"SeaSurfaceTemperature", "seatemperature"},
      {"SST", "seatemperature"},
      {"sea_surface_temperature", "seatemperature"},
      {"bulk_sea_surface_temperature", "seatemperature"},
      {"Pressure", "pressure"},
      {"MeanSeaLevelPressure", "pressure"},
      {"MSLPressure", "pressure"},
      {"MSL", "pressure"},
      {"P", "pressure"},
      {"Humidity", "humidity"},
      {"RelativeHumidity", "humidity"},
      {"RH", "humidity"},
      {"Precipitation1h", "precipitation"},
      {"Precipitation", "precipitation"},
      {"RR-1H", "precipitation"},
      {"RR-KGM2", "precipitation"},
      {"PrecipitationAmount", "precipitation"},
      {"PrecipitationForm", "precipitation_form"},
      {"PotentialPrecipitationForm", "precipitation_form"},
      {"PrecipitationType", "precipitation_type"},
      {"PotentialPrecipitationType", "precipitation_type"},
      {"WindSpeedMS", "wind"},
      {"TotalWindMS", "wind"},
      {"WS", "wind"},
      {"WindSpeed", "wind"},
      {"FF", "wind"},
      {"WindUMS", "wind_component"},
      {"WindVMS", "wind_component"},
      {"WindGust", "windgust"},
      {"WG", "windgust"},
      {"HourlyMaximumGust", "windgust"},
      {"TotalCloudCover", "totalcloudcover_color"},
      {"MiddleAndLowCloudCover", "totalcloudcover_color"},
      {"LowCloudCover", "totalcloudcover_color"},
      {"MediumCloudCover", "totalcloudcover_color"},
      {"HighCloudCover", "totalcloudcover_color"},
      {"FogIntensity", "fog_intensity"},
      {"SnowDepth", "snowdepth"},
      {"SD", "snowdepth"},
      {"ProbabilityOfPrecipitation", "probability"},
      {"PoP", "probability"},
      {"ProbabilityThunderstorm", "probability"},
      {"ThermalSum", "thermalsum"},
      {"CurrentSpeed", "currentspeed"},
      {"WaterCurrentSpeed", "currentspeed"},
  };
  return kMap;
}

std::string lowercased(const std::string& s)
{
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(), ::tolower);
  return out;
}

// Stream `n` space chars (no-op if n <= 0).
void padSpaces(std::ostream& os, int n)
{
  if (n > 0) os << std::string(static_cast<std::size_t>(n), ' ');
}

// "Nice" tick step for an axis spanning `range`: one of {1,2,5}*10^k.
double niceStep(double range, int maxTicks)
{
  if (range <= 0 || maxTicks < 1) return 1.0;
  const double rough = range / maxTicks;
  const double exponent = std::floor(std::log10(rough));
  const double power = std::pow(10.0, exponent);
  const double fraction = rough / power;
  double nf = 1.0;
  if (fraction < 1.5)
    nf = 1.0;
  else if (fraction < 3.5)
    nf = 2.0;
  else if (fraction < 7.5)
    nf = 5.0;
  else
    nf = 10.0;
  return nf * power;
}

// Lower-case copy.
std::string toLower(std::string s)
{
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

// Returns true if any of `needles` (already lowercase) appears in `text`
// (compared case-insensitively).
bool nameContains(const std::string& text, std::initializer_list<const char*> needles)
{
  const std::string lower = toLower(text);
  return std::any_of(needles.begin(), needles.end(),
                     [&](const char* n) { return lower.find(n) != std::string::npos; });
}

// Output of the unit-and-name auto-detection: the value transform that
// brings the data into the palette's expected unit system, plus the
// proposed palette name. Empty palette = no automatic suggestion.
struct UnitGuess
{
  float scale = 1.0F;
  float offset = 0.0F;
  std::string palette;
};

// Map (parameter name + unit string) → suggested palette and value transform.
// Detection is conservative: we only propose a palette when the unit AND
// the name agree on the parameter's nature, so e.g. a m/s ocean current
// doesn't accidentally pick up the atmospheric wind palette.
UnitGuess guessFromUnits(const std::string& shortName, const std::string& longName,
                         const std::string& units)
{
  UnitGuess g;
  const std::string both = shortName + " " + longName;

  // Sea-temperature names take priority over the generic temperature
  // palette so the realistic −2…+25 °C ramp is used instead of the
  // atmospheric −50…+50 °C one.
  auto isSeaTempName = [&] {
    return nameContains(both, {"sea_surface_temperature", "sea surface temperature",
                               "sea_temperature", "sea temperature", "temperaturesea"}) ||
           shortName == "TSEA" || shortName == "TSEA-C" || shortName == "SST";
  };

  // Temperature: K → °C
  if (units == "K" || units == "Kelvin" || units == "kelvin")
  {
    if (isSeaTempName())
    {
      g.offset = -273.15F;
      g.palette = "seatemperature";
    }
    else if (nameContains(both, {"temperature", "dewpoint", "dew point"}) || shortName == "T" ||
             shortName == "T-K" || shortName == "TD" || shortName == "TD-K" ||
             shortName == "T2m")
    {
      g.offset = -273.15F;
      g.palette = "temperature";
    }
  }
  // Pressure: Pa → hPa (palette is in hPa)
  else if (units == "Pa" || units == "pascal" || units == "Pascal")
  {
    if (nameContains(both, {"pressure", "msl"}))
    {
      g.scale = 0.01F;
      g.palette = "pressure";
    }
  }
  // Wind speed: m/s
  else if (units == "m/s" || units == "m s-1" || units == "m s**-1" ||
           units == "ms-1" || units == "ms**-1")
  {
    if (nameContains(both, {"gust"}))
      g.palette = "windgust";
    else if (shortName == "WindUMS" || shortName == "WindVMS" ||
             nameContains(both, {"u-component of wind", "v-component of wind",
                                 "u component of wind", "v component of wind"}))
      g.palette = "wind_component";
    else if (nameContains(both, {"wind"}) || shortName == "WS" || shortName == "FF")
      g.palette = "wind";
  }
  // Percent: humidity / cloud cover / probability — name disambiguates.
  else if (units == "%" || units == "percent")
  {
    if (nameContains(both, {"humidity"}))
      g.palette = "humidity";
    else if (nameContains(both, {"cloud"}))
      g.palette = "totalcloudcover_color";
    else if (nameContains(both, {"probability", "pop", "thunderstorm"}))
      g.palette = "probability";
  }
  // Fraction (0..1): scale by 100 to become percent.
  else if (units == "1" || units == "0-1" || units == "fraction" || units == "(0 - 1)")
  {
    if (nameContains(both, {"humidity"}))
    {
      g.scale = 100.0F;
      g.palette = "humidity";
    }
    else if (nameContains(both, {"cloud"}))
    {
      g.scale = 100.0F;
      g.palette = "totalcloudcover_color";
    }
    else if (nameContains(both, {"probability"}))
    {
      g.scale = 100.0F;
      g.palette = "probability";
    }
  }
  // Precipitation: mm, mm/h, kg/m² (1 kg/m² ≈ 1 mm rain)
  else if (units == "mm" || units == "mm/h" || units == "mm/hr" || units == "kg/m^2" ||
           units == "kg m-2" || units == "kg m**-2" || units == "kg m^-2")
  {
    if (nameContains(both, {"precipitation", "rain", "snow"}) || shortName == "RR-1H" ||
        shortName == "RR-KGM2")
      g.palette = "precipitation";
  }

  // Name-only fallback: when the file's units string is missing or the
  // unit didn't match any branch above, but the parameter name strongly
  // suggests a known type, propose the palette anyway with no transform.
  // The user can override with --palette.
  if (g.palette.empty())
  {
    if (isSeaTempName())
      g.palette = "seatemperature";
    else if (nameContains(both, {"temperature", "dewpoint", "dew_point"}))
      g.palette = "temperature";
    else if (nameContains(both, {"gust"}))
      g.palette = "windgust";
    else if (nameContains(both, {"wind"}) &&
             !nameContains(both, {"u-component", "v-component", "u_component", "v_component",
                                  "windums", "windvms"}))
      g.palette = "wind";
    else if (nameContains(both, {"humidity"}))
      g.palette = "humidity";
    else if (nameContains(both, {"cloud_cover", "cloudcover", "cloudiness"}))
      g.palette = "totalcloudcover_color";
    else if (nameContains(both, {"precipitation", "rainfall", "snowfall"}))
      g.palette = "precipitation";
    else if (nameContains(both, {"pressure"}))
      g.palette = "pressure";
  }

  return g;
}

std::string paletteForParam(const std::string& cfgPath, const std::string& paramName)
{
  // Try the user-configurable JSON map first.
  std::ifstream in(cfgPath);
  if (in)
  {
    Json::CharReaderBuilder builder;
    Json::Value root;
    std::string errs;
    if (Json::parseFromStream(builder, in, &root, &errs))
    {
      const auto& palettes = root["palettes"];
      if (palettes.isObject())
      {
        std::string needle = lowercased(paramName);
        for (const auto& key : palettes.getMemberNames())
        {
          if (lowercased(key) == needle) return palettes[key].asString();
        }
      }
    }
  }
  // Fall back to the built-in defaults so it works without an installed conf.
  std::string needle = lowercased(paramName);
  for (const auto& [param, palette] : builtinPaletteMap())
    if (lowercased(param) == needle) return palette;
  return {};
}
}  // namespace

void Viewport::reset()
{
  uMin = 0;
  uMax = 1;
  vMin = 0;
  vMax = 1;
}

void Viewport::clamp()
{
  // Allow the viewport to extend beyond the data bounding box (negative or
  // > 1 fractions). This is essential for projected data — e.g. polar
  // stereographic grids whose lat/lon bbox underestimates the actual
  // coverage near the pole. Out-of-bbox samples render as missing
  // (transparent), letting the user zoom out to see the whole data.
  // We only clamp the span itself: keep it positive and bounded.
  constexpr float kMinSpan = 1e-4F;  // prevent zooming in past 0.01%
  constexpr float kMaxSpan = 3.0F;   // cap zoom-out at 3× bbox
  auto bound = [&](float& lo, float& hi) {
    float span = hi - lo;
    if (span < kMinSpan)
    {
      const float c = (lo + hi) * 0.5F;
      lo = c - kMinSpan * 0.5F;
      hi = c + kMinSpan * 0.5F;
    }
    else if (span > kMaxSpan)
    {
      const float c = (lo + hi) * 0.5F;
      lo = c - kMaxSpan * 0.5F;
      hi = c + kMaxSpan * 0.5F;
    }
  };
  bound(uMin, uMax);
  bound(vMin, vMax);
}

void Viewport::zoom(float factor)
{
  zoomAt(factor, (uMin + uMax) * 0.5F, (vMin + vMax) * 0.5F);
}

void Viewport::zoomAt(float factor, float anchorU, float anchorV)
{
  // After zoom, the anchor (in area coords) stays at the same fractional
  // position within the viewport — i.e. the point under the cursor doesn't
  // move on screen.
  const float oldSpanU = uMax - uMin;
  const float oldSpanV = vMax - vMin;
  const float relU = (oldSpanU > 0) ? (anchorU - uMin) / oldSpanU : 0.5F;
  const float relV = (oldSpanV > 0) ? (anchorV - vMin) / oldSpanV : 0.5F;
  const float newSpanU = oldSpanU * factor;
  const float newSpanV = oldSpanV * factor;
  uMin = anchorU - relU * newSpanU;
  uMax = uMin + newSpanU;
  vMin = anchorV - relV * newSpanV;
  vMax = vMin + newSpanV;
  clamp();
}

void Viewport::pan(float duFrac, float dvFrac)
{
  float spanU = uMax - uMin;
  float spanV = vMax - vMin;
  uMin += spanU * duFrac;
  uMax += spanU * duFrac;
  vMin += spanV * dvFrac;
  vMax += spanV * dvFrac;
  clamp();
}

App::App(Options opts) : itsOpts(std::move(opts))
{
  itsSource = DataSource::open(itsOpts.filename);
  buildIndices();

  // Apply parameter override.
  if (!itsOpts.parameterOverride.empty())
  {
    NFmiEnumConverter conv;
    int id = conv.ToEnum(itsOpts.parameterOverride);
    if (id == kFmiBadParameter || !itsSource->selectParamId(id))
      throw std::runtime_error("qdless: parameter not found: " + itsOpts.parameterOverride);
    auto it = std::find(itsParamIds.begin(), itsParamIds.end(), id);
    if (it != itsParamIds.end()) itsParamIndex = static_cast<int>(it - itsParamIds.begin());
  }

  auto resolveIndex = [](int requested, std::size_t size) -> std::size_t {
    if (size == 0) return 0;
    if (requested < 0) return size - 1;
    if (static_cast<std::size_t>(requested) >= size)
      throw std::runtime_error("qdless: index " + std::to_string(requested) +
                               " out of range (size " + std::to_string(size) + ")");
    return static_cast<std::size_t>(requested);
  };
  itsSource->selectTimeIndex(resolveIndex(itsOpts.timeIndex, itsSource->timeCount()));
  itsSource->selectLevelIndex(resolveIndex(itsOpts.levelIndex, itsSource->levelCount()));

  loadPalette();
  loadCoastlines();
}

App::~App() = default;

float App::transform(float v) const
{
  // Same sentinel detection as Palette::lookup; keep them in sync.
  if (v == kFloatMissing || !std::isfinite(v) || std::abs(v) > 1e6F) return v;
  return v * itsValueScale + itsValueOffset;
}

void App::buildIndices()
{
  itsParamIds = itsSource->paramIds();
  itsParamIndex = 0;
}

void App::loadPalette()
{
  const int id = itsSource->currentParamId();
  const std::string shortName = itsSource->paramShortName(id);
  const std::string longName = itsSource->paramLongName(id);
  const std::string units = itsSource->paramUnits(id);

  // Auto-detect units. Result has a value transform (scale/offset) to bring
  // the data into the palette's canonical unit system, plus a palette name
  // suggestion (used only as a last-resort fallback).
  const UnitGuess guess = guessFromUnits(shortName, longName, units);
  itsValueScale = guess.scale;
  itsValueOffset = guess.offset;
  if (guess.scale != 1.0F || guess.offset != 0.0F)
  {
    if (guess.scale == 1.0F)
      itsLastMessage = fmt::format("Auto-shift: {} + {:g}", units, guess.offset);
    else
      itsLastMessage = fmt::format("Auto-shift: {} × {:g}", units, guess.scale);
  }

  std::string paletteName = itsOpts.paletteOverride;
  if (paletteName.empty()) paletteName = paletteForParam(itsOpts.configFile, shortName);
  if (paletteName.empty()) paletteName = paletteForParam(itsOpts.configFile, longName);
  if (paletteName.empty()) paletteName = guess.palette;

  if (!paletteName.empty())
  {
    // Try several palette locations so the tool works without `make install`
    // and regardless of cwd:
    //   1. --palette-dir (default /usr/share/smartmet/qdless/palettes)
    //   2. palettes/ next to the binary (build-tree layout)
    //   3. ../share/smartmet/qdless/palettes/ relative to binary (install)
    //   4. $HOME/.config/qdless/palettes/
    std::vector<std::filesystem::path> candidates{
        std::filesystem::path(itsOpts.paletteDir) / (paletteName + ".json"),
    };
    try
    {
      // Cross-platform exe path: Linux /proc, macOS _NSGetExecutablePath, etc.
      const std::filesystem::path exeDir =
          boost::dll::program_location().parent_path().string();
      candidates.push_back(exeDir / "palettes" / (paletteName + ".json"));
      candidates.push_back(exeDir / ".." / "share" / "smartmet" / "qdless" / "palettes" /
                           (paletteName + ".json"));
    }
    catch (const std::exception&)
    {
      // exe path not resolvable (no /proc on Linux, dyld lookup failed, …)
    }
    if (const char* home = std::getenv("HOME"))
      candidates.push_back(std::filesystem::path(home) / ".config" / "qdless" / "palettes" /
                           (paletteName + ".json"));

    for (const auto& path : candidates)
    {
      try
      {
        itsPalette = Palette::loadFromFile(path.string());
        return;
      }
      catch (const std::exception&)
      {
        // try next candidate
      }
    }
  }
  // Derive a built-in ramp by sampling the current slice on a coarse lattice
  // in the source's native (u, v) coordinates. Cheap (~O(40*40) calls).
  float lo = std::numeric_limits<float>::infinity();
  float hi = -std::numeric_limits<float>::infinity();
  constexpr int N = 40;
  for (int j = 0; j < N; ++j)
  {
    for (int i = 0; i < N; ++i)
    {
      double lat = 0;
      double lon = 0;
      itsSource->uvToLatLon((i + 0.5) / N, (j + 0.5) / N, lat, lon);
      const float v = transform(itsSource->interpolatedValue(lat, lon));
      if (v == kFloatMissing || !std::isfinite(v) || std::abs(v) > 1e6F) continue;
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
  }
  itsPalette = Palette::builtinRamp(lo, hi);
}

void App::loadCoastlines()
{
  // Estimate the visible lat/lon extent for picking GSHHS resolution by
  // sampling the four viewport corners through the projection. Works for
  // both lat/lon and projected backends.
  double minLat = 90;
  double maxLat = -90;
  double minLon = 180;
  double maxLon = -180;
  for (double u : {itsViewport.uMin, itsViewport.uMax})
    for (double v : {itsViewport.vMin, itsViewport.vMax})
    {
      double lat = 0;
      double lon = 0;
      itsSource->uvToLatLon(u, v, lat, lon);
      minLat = std::min(minLat, lat);
      maxLat = std::max(maxLat, lat);
      minLon = std::min(minLon, lon);
      maxLon = std::max(maxLon, lon);
    }
  const auto span = static_cast<float>(std::max(maxLon - minLon, maxLat - minLat));

  if (!itsOpts.noCoastline)
  {
    auto path = Coastline::pickFile(itsOpts.coastlineDir, "GSHHS", span);
    if (!path.empty() && path != itsCoastlinePath)
    {
      itsCoastlines = Coastline::read(path, itsOpts.minLakeAreaKm2, itsOpts.minLakeRoundness,
                                      itsOpts.minIslandAreaKm2);
      itsCoastlinePath = path;
    }
  }
  if (!itsOpts.noBorders)
  {
    auto path = Coastline::pickFile(itsOpts.coastlineDir, "border", span);
    if (!path.empty() && path != itsBorderPath)
    {
      itsBorders = Coastline::read(path);
      itsBorderPath = path;
    }
  }
}

std::vector<Rgb> App::sampleSlice(int subWidth, int subHeight, float& dataMin,
                                  float& dataMax) const
{
  std::vector<Rgb> out(static_cast<std::size_t>(subWidth) * subHeight, Palette::missingColor());
  dataMin = std::numeric_limits<float>::infinity();
  dataMax = -std::numeric_limits<float>::infinity();

  const float spanU = itsViewport.uMax - itsViewport.uMin;
  const float spanV = itsViewport.vMax - itsViewport.vMin;

  // Viewport (u,v) ∈ [0..1]² of the source's native rectangle (NFmiArea XY
  // for projected sources, lat/lon bbox otherwise). Image-coord convention
  // (v=0 = top = north) so no row flip on screen.
  for (int sy = 0; sy < subHeight; ++sy)
  {
    const float vp = itsViewport.vMin + (static_cast<float>(sy) + 0.5F) / subHeight * spanV;
    for (int sx = 0; sx < subWidth; ++sx)
    {
      const float up = itsViewport.uMin + (static_cast<float>(sx) + 0.5F) / subWidth * spanU;
      double lat = 0;
      double lon = 0;
      itsSource->uvToLatLon(up, vp, lat, lon);
      const float val = transform(itsSource->interpolatedValue(lat, lon));
      if (val != kFloatMissing && std::isfinite(val) && std::abs(val) < 1e6F)
      {
        dataMin = std::min(dataMin, val);
        dataMax = std::max(dataMax, val);
      }
      out[static_cast<std::size_t>(sy) * subWidth + sx] = itsPalette.lookup(val);
    }
  }
  return out;
}

void App::renderCrossSection(int x1, int y1, int x2, int y2, UI& ui)
{
  double lat1 = 0;
  double lon1 = 0;
  double lat2 = 0;
  double lon2 = 0;
  if (!cellToLatLon(ui, x1, y1, lat1, lon1) || !cellToLatLon(ui, x2, y2, lat2, lon2))
    return;

  const int nLevels = static_cast<int>(itsSource->levelCount());
  if (nLevels < 2)
  {
    itsLastMessage = "Cross-section needs >= 2 levels in the file";
    return;
  }

  // Collect level values + sort indices so the popup shows high altitude /
  // low pressure on top.
  std::vector<float> levelValues(nLevels);
  const std::size_t savedLevel = itsSource->currentLevelIndex();
  for (int i = 0; i < nLevels; ++i) levelValues[i] = itsSource->levelValueAt(i);
  std::vector<int> levelOrder(nLevels);
  std::iota(levelOrder.begin(), levelOrder.end(), 0);
  // Pressure level: smaller value = higher altitude; show smallest at top.
  std::sort(levelOrder.begin(), levelOrder.end(),
            [&](int a, int b) { return levelValues[a] < levelValues[b]; });

  // Layout: chartW columns x nLevels rows.
  const int desiredW = std::min(80, COLS - 16);
  const int chartW = std::max(20, desiredW);
  const int chartH = nLevels;
  const int subW = chartW * 2;
  const int subH = chartH * 2;

  // Compute label width for level labels.
  int labelW = 0;
  for (float v : levelValues)
    labelW = std::max(labelW, static_cast<int>(fmt::format("{:g}", v).size()));
  labelW = std::max(labelW, 4);

  // Haversine distance for the X-axis scale.
  auto haversineKm = [](double la1, double lo1, double la2, double lo2) {
    const double r1 = la1 * M_PI / 180.0;
    const double r2 = la2 * M_PI / 180.0;
    const double dla = (la2 - la1) * M_PI / 180.0;
    const double dlo = (lo2 - lo1) * M_PI / 180.0;
    const double a = std::sin(dla / 2) * std::sin(dla / 2) +
                     std::cos(r1) * std::cos(r2) * std::sin(dlo / 2) * std::sin(dlo / 2);
    return 2.0 * 6371.0 * std::atan2(std::sqrt(a), std::sqrt(1 - a));
  };
  const double totalKm = haversineKm(lat1, lon1, lat2, lon2);

  // Sample.
  std::vector<Rgb> pixels(static_cast<std::size_t>(subW) * subH);
  for (int li = 0; li < nLevels; ++li)
  {
    const int srcLevel = levelOrder[li];  // model index
    itsSource->selectLevelIndex(static_cast<unsigned long>(srcLevel));
    for (int sx = 0; sx < subW; ++sx)
    {
      const double frac = (static_cast<double>(sx) + 0.5) / subW;
      const double lat = lat1 + frac * (lat2 - lat1);
      const double lon = lon1 + frac * (lon2 - lon1);
      const float val = transform(itsSource->interpolatedValue(lat, lon));
      const Rgb c = itsPalette.lookup(val);
      pixels[static_cast<std::size_t>(li * 2) * subW + sx] = c;
      pixels[static_cast<std::size_t>(li * 2 + 1) * subW + sx] = c;
    }
  }
  itsSource->selectLevelIndex(savedLevel);

  // Build raw-ANSI popup.
  // Width: border + label + " ┤ " + chart + border = 2 + labelW + 3 + chartW
  const int width = std::min(COLS - 4, labelW + chartW + 6);
  // Height: border + title + chart + axis + endpoints + footer + border
  const int height = chartH + 6;
  const int top = std::max(0, (LINES - height) / 2);
  const int left = std::max(0, (COLS - width) / 2);
  const int interiorW = width - 2;

  NFmiEnumConverter conv;
  std::string title = "Cross-section: " + std::string(itsSource->paramShortName(itsSource->currentParamId()));

  std::ostringstream os;

  auto pos = [&](int row, int col = 0) {
    os << "\x1b[" << (top + row + 1) << ';' << (left + col + 1) << 'H';
  };

  // Top border with title.
  pos(0);
  os << "\x1b[0m\x1b[48;5;0m\x1b[38;5;14m"
     << "\xe2\x94\x8c\xe2\x94\x80[\x1b[38;5;15m" << title << "\x1b[38;5;14m]";
  int titleConsumed = 4 + static_cast<int>(title.size());
  for (int i = 0; i < width - titleConsumed - 1; ++i) os << "\xe2\x94\x80";
  os << "\xe2\x94\x90\x1b[0m";

  // Chart rows: label │ <data row>.
  // Render each chart row as one cell-row of quadrant blocks via Renderer.
  // We do row-by-row to interleave with labels.
  for (int cy = 0; cy < chartH; ++cy)
  {
    pos(1 + cy);
    os << "\x1b[0m\x1b[48;5;0m\x1b[38;5;14m\xe2\x94\x82\x1b[48;5;0m\x1b[38;5;15m ";
    // Label.
    os << fmt::format("{:>{}g}", levelValues[levelOrder[cy]], labelW) << " \xe2\x94\xa4";

    // Render this row's chart cells using a tiny Renderer call.
    // Build the slice: 2 sub-rows of pixels for this level.
    std::vector<Rgb> rowPixels(static_cast<std::size_t>(subW) * 2);
    for (int sx = 0; sx < subW; ++sx)
    {
      rowPixels[sx] = pixels[static_cast<std::size_t>(cy * 2) * subW + sx];
      rowPixels[static_cast<std::size_t>(subW) + sx] =
          pixels[static_cast<std::size_t>(cy * 2 + 1) * subW + sx];
    }
    // Render at the chart column origin.
    itsRenderer.render(os, rowPixels, subW, 2, top + 1 + cy, left + 1 + labelW + 2 + 1);

    // Right padding inside the box.
    pos(1 + cy, interiorW + 1);  // not used; we just close the right border below.
    pos(1 + cy, width - 1);
    os << "\x1b[0m\x1b[48;5;0m\x1b[38;5;14m\xe2\x94\x82\x1b[0m";
  }

  // Distance axis.
  pos(1 + chartH);
  const int distW = std::max(1, chartW / 2 - 6);
  const std::string distLine =
      fmt::format(" 0 km {:>{}.{}f} km {:>{}.{}f} km", totalKm / 2, distW, 1, totalKm, distW, 1);
  os << "\x1b[0m\x1b[48;5;0m\x1b[38;5;14m\xe2\x94\x82\x1b[48;5;0m\x1b[38;5;15m";
  padSpaces(os, labelW + 3);  // align under chart
  os << distLine;
  // Pad to right border.
  int consumed = labelW + 3 + static_cast<int>(distLine.size());
  padSpaces(os, interiorW - consumed);
  os << "\x1b[0m\x1b[48;5;0m\x1b[38;5;14m\xe2\x94\x82\x1b[0m";

  // Endpoints row.
  pos(2 + chartH);
  const std::string endpts =
      fmt::format(" {:.2f}°N {:.2f}°E  ->  {:.2f}°N {:.2f}°E   total {:.1f} km", lat1, lon1, lat2,
                  lon2, totalKm);
  os << "\x1b[0m\x1b[48;5;0m\x1b[38;5;14m\xe2\x94\x82\x1b[48;5;0m\x1b[38;5;15m" << endpts;
  padSpaces(os, interiorW - static_cast<int>(endpts.size()));
  os << "\x1b[0m\x1b[48;5;0m\x1b[38;5;14m\xe2\x94\x82\x1b[0m";

  // Footer.
  pos(3 + chartH);
  std::string_view footer = " any key to close";
  os << "\x1b[0m\x1b[48;5;0m\x1b[38;5;14m\xe2\x94\x82\x1b[48;5;0m\x1b[38;5;15m"
     << footer;
  padSpaces(os, interiorW - static_cast<int>(footer.size()));
  os << "\x1b[0m\x1b[48;5;0m\x1b[38;5;14m\xe2\x94\x82\x1b[0m";

  // Bottom border.
  pos(height - 1);
  os << "\x1b[0m\x1b[48;5;0m\x1b[38;5;14m\xe2\x94\x94";
  for (int i = 0; i < width - 2; ++i) os << "\xe2\x94\x80";
  os << "\xe2\x94\x98\x1b[0m";

  const std::string s = os.str();
  std::fwrite(s.data(), 1, s.size(), stdout);
  std::fflush(stdout);

  // Wait for keypress to dismiss.
  // Direct ncurses wgetch on stdscr would refresh stdscr; route through UI.
  ui.waitInput();
}

bool App::ensureCityIndex() const
{
  if (itsCityIndexAttempted) return !itsCityIndex.empty();
  itsCityIndexAttempted = true;

  std::vector<std::filesystem::path> candidates{
      std::filesystem::path("/usr/share/smartmet/qdless/cities1000.tsv"),
  };
  try
  {
    const std::filesystem::path exeDir =
        boost::dll::program_location().parent_path().string();
    candidates.push_back(exeDir / "data" / "cities1000.tsv");
  }
  catch (const std::exception&)
  {
  }
  if (const char* home = std::getenv("HOME"))
    candidates.push_back(std::filesystem::path(home) / ".config" / "qdless" /
                         "cities1000.tsv");
  for (const auto& path : candidates)
  {
    if (std::filesystem::exists(path) && itsCityIndex.load(path.string())) return true;
  }
  return false;
}

void App::openPlaceSearch(UI& ui)
{
  if (!ensureCityIndex())
  {
    itsLastMessage = "Place search: cities1000.tsv not found";
    return;
  }

  // Viewport centre — used to bias city search toward nearby places, so
  // typing "kou" while looking at Scandinavia surfaces Kouvola ahead of
  // every Chinese city ending in "kou".
  double centerLat = 0;
  double centerLon = 0;
  itsSource->uvToLatLon((itsViewport.uMin + itsViewport.uMax) * 0.5,
                        (itsViewport.vMin + itsViewport.vMax) * 0.5, centerLat, centerLon);

  // The popup calls this lambda each keystroke; we keep the latest match
  // list outside so we can resolve the picked index back to a city.
  std::vector<std::size_t> lastMatchIds;
  auto matcher = [&](const std::string& q) -> std::vector<std::string> {
    lastMatchIds = itsCityIndex.search(q, 12, centerLat, centerLon);
    std::vector<std::string> rows;
    rows.reserve(lastMatchIds.size());
    for (std::size_t i : lastMatchIds)
    {
      const auto& c = itsCityIndex.at(i);
      rows.push_back(fmt::format("{}, {}  ({:.2f}, {:.2f})  pop {}", c.name, c.country, c.lat,
                                 c.lon, c.population));
    }
    return rows;
  };

  const int picked = ui.popupSearch("Place search", matcher);
  if (picked < 0 || picked >= static_cast<int>(lastMatchIds.size())) return;
  const auto& city = itsCityIndex.at(lastMatchIds[picked]);

  // Recentre viewport on the city in [0..1]² of the source's native space.
  double uD = 0;
  double vD = 0;
  itsSource->latLonToUV(city.lat, city.lon, uD, vD);
  const auto u = static_cast<float>(uD);
  const auto v = static_cast<float>(vD);

  const float halfSpan = 0.15F;  // 30% of full extent on each axis
  itsViewport.uMin = u - halfSpan;
  itsViewport.uMax = u + halfSpan;
  itsViewport.vMin = v - halfSpan;
  itsViewport.vMax = v + halfSpan;
  itsViewport.clamp();

  itsMarker = std::make_pair(city.lat, city.lon);
  itsLastMessage = std::string("Centred on ") + city.name + ", " + city.country;

  // Auto-open the time-series probe at the picked location. The popup runs
  // its own input loop until dismissed; on exit, control returns here and
  // the main loop redraws the map (with the marker still visible).
  drawMap(ui);
  openProbeAt(city.lat, city.lon, ui);
}

std::string App::exportPng(std::string& err) const
{
  const float spanU = itsViewport.uMax - itsViewport.uMin;
  const float spanV = itsViewport.vMax - itsViewport.vMin;
  if (spanU <= 0 || spanV <= 0) { err = "empty viewport"; return {}; }

  // Output size: aspect-ratio of the visible rectangle in the source's
  // native space (which preserves the native projection's shape), target
  // ~720 px tall. For unprojected backends this equals the lat/lon aspect.
  constexpr int targetH = 720;
  const double aspect = spanU / spanV;
  const int height = targetH;
  const int width = std::max(100, static_cast<int>(targetH * aspect));

  Imagine::NFmiImage image(width, height);
  image.Erase(Imagine::NFmiColorTools::MakeColor(255, 255, 255));

  for (int py = 0; py < height; ++py)
  {
    const float v = itsViewport.vMin + (static_cast<float>(py) + 0.5F) / height * spanV;
    for (int px = 0; px < width; ++px)
    {
      const float u = itsViewport.uMin + (static_cast<float>(px) + 0.5F) / width * spanU;
      double lat = 0;
      double lon = 0;
      itsSource->uvToLatLon(u, v, lat, lon);
      const float val = transform(itsSource->interpolatedValue(lat, lon));
      Rgb c = itsPalette.lookup(val);
      if (c.transparent) continue;  // leave white for "no data"
      image(px, py) = Imagine::NFmiColorTools::MakeColor(c.r, c.g, c.b);
    }
  }

  // Coastline + border overlay (Bresenham into image pixels).
  auto drawPolylineImg = [&](const std::vector<Polyline>& polys,
                             Imagine::NFmiColorTools::Color color) {
    if (polys.empty()) return;
    auto toPixel = [&](float lon, float lat) -> std::pair<int, int> {
      double u = 0;
      double v = 0;
      itsSource->latLonToUV(lat, lon, u, v);
      const double u01 = (u - itsViewport.uMin) / spanU;
      const double v01 = (v - itsViewport.vMin) / spanV;
      return {static_cast<int>(u01 * width), static_cast<int>(v01 * height)};
    };
    auto plot = [&](int x, int y) {
      if (x >= 0 && x < width && y >= 0 && y < height) image(x, y) = color;
    };
    auto line = [&](int x0, int y0, int x1, int y1) {
      int dx = std::abs(x1 - x0);
      int dy = -std::abs(y1 - y0);
      int sx = x0 < x1 ? 1 : -1;
      int sy = y0 < y1 ? 1 : -1;
      int err2 = dx + dy;
      while (true)
      {
        plot(x0, y0);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err2;
        if (e2 >= dy) { err2 += dy; x0 += sx; }
        if (e2 <= dx) { err2 += dx; y0 += sy; }
      }
    };
    for (const auto& pl : polys)
    {
      if (pl.lons.size() < 2) continue;
      auto prev = toPixel(pl.lons[0], pl.lats[0]);
      for (std::size_t i = 1; i < pl.lons.size(); ++i)
      {
        auto cur = toPixel(pl.lons[i], pl.lats[i]);
        if (std::abs(cur.first - prev.first) < width &&
            std::abs(cur.second - prev.second) < height)
          line(prev.first, prev.second, cur.first, cur.second);
        prev = cur;
      }
    }
  };
  drawPolylineImg(itsCoastlines, Imagine::NFmiColorTools::MakeColor(0, 0, 0));
  drawPolylineImg(itsBorders, Imagine::NFmiColorTools::MakeColor(90, 90, 90));

  // Build filename: <basename>_<param>_<YYYYMMDD_HHMM>.png in cwd.
  NFmiEnumConverter conv;
  std::filesystem::path inputPath(itsOpts.filename);
  const std::string base = inputPath.stem().string();
  const std::string param = itsSource->paramShortName(itsSource->currentParamId());
  NFmiMetTime t = itsSource->currentValidTime();
  const std::string filename =
      fmt::format("{}_{}_{:04}{:02}{:02}_{:02}{:02}.png", base, param,
                  static_cast<int>(t.GetYear()), static_cast<int>(t.GetMonth()),
                  static_cast<int>(t.GetDay()), static_cast<int>(t.GetHour()),
                  static_cast<int>(t.GetMin()));

#ifdef IMAGINE_IGNORE_FORMATS
  err = "PNG support disabled in imagine build";
  return {};
#else
  try
  {
    image.WritePng(filename);
    return filename;
  }
  catch (const std::exception& e)
  {
    err = e.what();
    return {};
  }
#endif
}

void App::overlayGraticule(std::vector<Rgb>& pixels, int subWidth, int subHeight) const
{
  const float spanU = itsViewport.uMax - itsViewport.uMin;
  const float spanV = itsViewport.vMax - itsViewport.vMin;
  if (spanU <= 0 || spanV <= 0) return;

  // Estimate the visible lat/lon extent by sampling along the viewport
  // border. With projected sources the extent is generally not rectilinear
  // in lat/lon (e.g. polar stereo), so sample densely enough to catch
  // off-axis extrema.
  double minLat = 90;
  double maxLat = -90;
  double minLon = 180;
  double maxLon = -180;
  constexpr int kEdgeSamples = 32;
  auto observe = [&](double u, double v) {
    double lat = 0;
    double lon = 0;
    itsSource->uvToLatLon(u, v, lat, lon);
    minLat = std::min(minLat, lat);
    maxLat = std::max(maxLat, lat);
    minLon = std::min(minLon, lon);
    maxLon = std::max(maxLon, lon);
  };
  for (int i = 0; i <= kEdgeSamples; ++i)
  {
    const double t = static_cast<double>(i) / kEdgeSamples;
    observe(itsViewport.uMin + t * spanU, itsViewport.vMin);  // top
    observe(itsViewport.uMin + t * spanU, itsViewport.vMax);  // bottom
    observe(itsViewport.uMin, itsViewport.vMin + t * spanV);  // left
    observe(itsViewport.uMax, itsViewport.vMin + t * spanV);  // right
  }
  if (maxLat <= minLat || maxLon <= minLon) return;

  const double lonStep = niceStep(maxLon - minLon, 6);
  const double latStep = niceStep(maxLat - minLat, 6);

  const Rgb gridColor{120, 120, 120};

  auto toSub = [&](double lat, double lon) -> std::pair<int, int> {
    double u = 0;
    double v = 0;
    itsSource->latLonToUV(lat, lon, u, v);
    const double u01 = (u - itsViewport.uMin) / spanU;
    const double v01 = (v - itsViewport.vMin) / spanV;
    return {static_cast<int>(u01 * subWidth), static_cast<int>(v01 * subHeight)};
  };

  auto plot = [&](int x, int y) {
    if (x >= 0 && x < subWidth && y >= 0 && y < subHeight)
      pixels[static_cast<std::size_t>(y) * subWidth + x] = gridColor;
  };

  auto drawLine = [&](int x0, int y0, int x1, int y1) {
    int dx = std::abs(x1 - x0);
    int dy = -std::abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (true)
    {
      plot(x0, y0);
      if (x0 == x1 && y0 == y1) break;
      int e2 = 2 * err;
      if (e2 >= dy) { err += dy; x0 += sx; }
      if (e2 <= dx) { err += dx; y0 += sy; }
    }
  };

  constexpr int kSamplesPerLine = 60;

  // Meridians (constant lon). Snap start to nearest multiple of step.
  const double startLon = std::ceil(minLon / lonStep) * lonStep;
  for (double lon = startLon; lon <= maxLon + lonStep * 0.5; lon += lonStep)
  {
    auto prev = toSub(minLat, lon);
    for (int s = 1; s <= kSamplesPerLine; ++s)
    {
      double lat = minLat + (maxLat - minLat) * s / kSamplesPerLine;
      auto cur = toSub(lat, lon);
      if (std::abs(cur.first - prev.first) < subWidth &&
          std::abs(cur.second - prev.second) < subHeight)
        drawLine(prev.first, prev.second, cur.first, cur.second);
      prev = cur;
    }
  }
  // Parallels (constant lat).
  const double startLat = std::ceil(minLat / latStep) * latStep;
  for (double lat = startLat; lat <= maxLat + latStep * 0.5; lat += latStep)
  {
    auto prev = toSub(lat, minLon);
    for (int s = 1; s <= kSamplesPerLine; ++s)
    {
      double lon = minLon + (maxLon - minLon) * s / kSamplesPerLine;
      auto cur = toSub(lat, lon);
      if (std::abs(cur.first - prev.first) < subWidth &&
          std::abs(cur.second - prev.second) < subHeight)
        drawLine(prev.first, prev.second, cur.first, cur.second);
      prev = cur;
    }
  }
}

std::string App::buildWindArrows(int cellW, int cellH, int originRow, int originCol)
{
  // Find U and V param IDs in this file. If either is missing, render nothing.
  NFmiEnumConverter conv;
  const int uEnum = conv.ToEnum("WindUMS");
  const int vEnum = conv.ToEnum("WindVMS");
  if (uEnum == kFmiBadParameter || vEnum == kFmiBadParameter) return {};
  auto hasParam = [this](int e) {
    return std::find(itsParamIds.begin(), itsParamIds.end(), e) != itsParamIds.end();
  };
  if (!hasParam(uEnum) || !hasParam(vEnum)) return {};

  // Save current param to restore at the end.
  const int savedParam = itsSource->currentParamId();

  const float spanU = itsViewport.uMax - itsViewport.uMin;
  const float spanV = itsViewport.vMax - itsViewport.vMin;
  if (spanU <= 0 || spanV <= 0) return {};

  // Place arrows on a sparser grid so they don't crowd. Aim ~1 arrow per
  // 4×2 cells (cells are roughly 1:2 aspect ratio in most fonts).
  const int stepX = 4;
  const int stepY = 2;

  // 8-directional arrow glyphs (UTF-8). Index 0 = →, ccw to 7 = ↘.
  static const std::array<const char*, 8> kArrows = {
      "\xe2\x86\x92",  // →
      "\xe2\x86\x97",  // ↗
      "\xe2\x86\x91",  // ↑
      "\xe2\x86\x96",  // ↖
      "\xe2\x86\x90",  // ←
      "\xe2\x86\x99",  // ↙
      "\xe2\x86\x93",  // ↓
      "\xe2\x86\x98",  // ↘
  };

  // Collect samples for all marker positions. We need U then V.
  struct Sample { int cx, cy; float u, v; };
  std::vector<Sample> samples;
  samples.reserve(static_cast<std::size_t>(cellW * cellH) / (stepX * stepY) + 1);

  itsSource->selectParamId(uEnum);
  for (int cy = stepY / 2; cy < cellH; cy += stepY)
  {
    const float vp = itsViewport.vMin + (static_cast<float>(cy) + 0.5F) / cellH * spanV;
    for (int cx = stepX / 2; cx < cellW; cx += stepX)
    {
      const float up = itsViewport.uMin + (static_cast<float>(cx) + 0.5F) / cellW * spanU;
      double lat = 0;
      double lon = 0;
      itsSource->uvToLatLon(up, vp, lat, lon);
      const float val = transform(itsSource->interpolatedValue(lat, lon));
      samples.push_back({cx, cy, val, 0.0F});
    }
  }
  itsSource->selectParamId(vEnum);
  std::size_t i = 0;
  for (int cy = stepY / 2; cy < cellH; cy += stepY)
  {
    const float vp = itsViewport.vMin + (static_cast<float>(cy) + 0.5F) / cellH * spanV;
    for (int cx = stepX / 2; cx < cellW; cx += stepX)
    {
      const float up = itsViewport.uMin + (static_cast<float>(cx) + 0.5F) / cellW * spanU;
      double lat = 0;
      double lon = 0;
      itsSource->uvToLatLon(up, vp, lat, lon);
      samples[i++].v = itsSource->interpolatedValue(lat, lon);
      // Wind components are not unit-shifted (m/s expected); skip transform.
    }
  }

  // Restore the original param so the rest of the UI stays consistent.
  itsSource->selectParamId(savedParam);

  // Compose arrow output as raw ANSI: each arrow positioned absolutely.
  std::ostringstream os;
  for (const auto& s : samples)
  {
    if (!std::isfinite(s.u) || !std::isfinite(s.v)) continue;
    if (std::abs(s.u) > 1e10F || std::abs(s.v) > 1e10F) continue;
    const float speed = std::sqrt(s.u * s.u + s.v * s.v);
    if (speed < 0.5F) continue;  // skip near-calm cells

    // Direction: meteorological u/v have u>0=eastward, v>0=northward.
    // atan2(v, u) gives angle from east axis CCW.
    const double angle = std::atan2(s.v, s.u);
    int idx = static_cast<int>(std::lround(angle / (M_PI / 4)));
    idx = ((idx % 8) + 8) % 8;

    // Colour by speed using the wind palette range (3..32 m/s).
    Rgb color{255, 255, 255};  // default bright white
    if (speed < 5)
      color = Rgb{200, 230, 255};
    else if (speed < 10)
      color = Rgb{120, 180, 255};
    else if (speed < 17)
      color = Rgb{255, 100, 200};
    else if (speed < 26)
      color = Rgb{255, 60, 60};
    else
      color = Rgb{255, 170, 0};

    os << "\x1b[" << (originRow + s.cy + 1) << ';' << (originCol + s.cx + 1) << 'H'
       << itsRenderer.fgEscape(color) << "\x1b[1m" << kArrows[idx] << "\x1b[0m";
  }
  return os.str();
}

void App::overlayPolylines(std::vector<Rgb>& pixels, int subWidth, int subHeight,
                           const std::vector<Polyline>& polylines, Rgb color) const
{
  if (polylines.empty()) return;
  const float spanU = itsViewport.uMax - itsViewport.uMin;
  const float spanV = itsViewport.vMax - itsViewport.vMin;
  if (spanU <= 0 || spanV <= 0) return;

  auto plot = [&](int x, int y) {
    if (x >= 0 && x < subWidth && y >= 0 && y < subHeight)
      pixels[static_cast<std::size_t>(y) * subWidth + x] = color;
  };

  auto drawLine = [&](int x0, int y0, int x1, int y1) {
    int dx = std::abs(x1 - x0);
    int dy = -std::abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (true)
    {
      plot(x0, y0);
      if (x0 == x1 && y0 == y1) break;
      int e2 = 2 * err;
      if (e2 >= dy)
      {
        err += dy;
        x0 += sx;
      }
      if (e2 <= dx)
      {
        err += dx;
        y0 += sy;
      }
    }
  };

  auto toSub = [&](float lon, float lat) -> std::pair<int, int> {
    double u = 0;
    double v = 0;
    itsSource->latLonToUV(lat, lon, u, v);
    const double u01 = (u - itsViewport.uMin) / spanU;
    const double v01 = (v - itsViewport.vMin) / spanV;
    return {static_cast<int>(u01 * subWidth), static_cast<int>(v01 * subHeight)};
  };

  for (const auto& pl : polylines)
  {
    if (pl.lons.size() < 2) continue;
    auto prev = toSub(pl.lons[0], pl.lats[0]);
    for (std::size_t i = 1; i < pl.lons.size(); ++i)
    {
      auto cur = toSub(pl.lons[i], pl.lats[i]);
      if (std::abs(cur.first - prev.first) < subWidth &&
          std::abs(cur.second - prev.second) < subHeight)
        drawLine(prev.first, prev.second, cur.first, cur.second);
      prev = cur;
    }
  }
}

void App::overlayMarker(std::vector<Rgb>& pixels, int subWidth, int subHeight) const
{
  if (!itsMarker.has_value() || subWidth <= 0 || subHeight <= 0) return;
  const float spanU = itsViewport.uMax - itsViewport.uMin;
  const float spanV = itsViewport.vMax - itsViewport.vMin;
  if (spanU <= 0 || spanV <= 0) return;
  double u = 0;
  double v = 0;
  itsSource->latLonToUV(itsMarker->first, itsMarker->second, u, v);
  const double u01 = (u - itsViewport.uMin) / spanU;
  const double v01 = (v - itsViewport.vMin) / spanV;
  const int cx = static_cast<int>(u01 * subWidth);
  const int cy = static_cast<int>(v01 * subHeight);

  const Rgb fg{255, 40, 40};
  const Rgb bg{255, 255, 255};
  auto plot = [&](int x, int y, Rgb c) {
    if (x >= 0 && x < subWidth && y >= 0 && y < subHeight)
      pixels[static_cast<std::size_t>(y) * subWidth + x] = c;
  };
  // White halo above/below and left/right of each cross arm so the marker
  // stands out against any palette colour.
  for (int d = -3; d <= 3; ++d)
  {
    plot(cx + d, cy - 1, bg);
    plot(cx + d, cy + 1, bg);
    plot(cx - 1, cy + d, bg);
    plot(cx + 1, cy + d, bg);
  }
  // Red cross arms.
  for (int d = -3; d <= 3; ++d)
  {
    plot(cx + d, cy, fg);
    plot(cx, cy + d, fg);
  }
  // White centre dot for visibility.
  plot(cx, cy, bg);
}

namespace
{
// Visible-codepoints width approximation for collision-avoidance: counts
// each leading byte of a UTF-8 sequence as one cell. East-Asian wide chars
// are not adjusted for, but city names are mostly Latin/Cyrillic/Arabic.
int displayCells(const std::string& s)
{
  int n = 0;
  for (unsigned char c : s)
    if ((c & 0xC0) != 0x80) ++n;
  return n;
}

// Discrete population-count steps the user can cycle through with PageDown
// (denser) and PageUp (sparser). Sorted ascending.
constexpr std::array<int, 7> kCityNSteps = {{5, 10, 25, 50, 100, 250, 500}};

// Pick visible cities (lat/lon inside viewport), sorted by population
// descending, capped at maxN.
std::vector<std::size_t> visibleCities(const CityIndex& idx, const DataSource& src,
                                       const Viewport& vp, int maxN)
{
  struct Hit { std::size_t i; int pop; };
  std::vector<Hit> hits;
  for (std::size_t i = 0; i < idx.size(); ++i)
  {
    const auto& c = idx.at(i);
    double u = 0;
    double v = 0;
    src.latLonToUV(c.lat, c.lon, u, v);
    if (u < vp.uMin || u > vp.uMax || v < vp.vMin || v > vp.vMax) continue;
    hits.push_back({i, c.population});
  }
  std::sort(hits.begin(), hits.end(),
            [](const Hit& a, const Hit& b) { return a.pop > b.pop; });
  if (static_cast<int>(hits.size()) > maxN) hits.resize(maxN);
  std::vector<std::size_t> out;
  out.reserve(hits.size());
  for (const auto& h : hits) out.push_back(h.i);
  return out;
}
}  // namespace

void App::overlayCities(std::vector<Rgb>& pixels, int subWidth, int subHeight) const
{
  if (!itsShowCities || subWidth <= 0 || subHeight <= 0) return;
  if (!ensureCityIndex()) return;
  const float spanU = itsViewport.uMax - itsViewport.uMin;
  const float spanV = itsViewport.vMax - itsViewport.vMin;
  if (spanU <= 0 || spanV <= 0) return;

  const auto picks = visibleCities(itsCityIndex, *itsSource, itsViewport, itsCityOverlayN);
  const Rgb dot{255, 255, 255};
  const Rgb halo{30, 30, 30};
  auto plot = [&](int x, int y, Rgb c) {
    if (x >= 0 && x < subWidth && y >= 0 && y < subHeight)
      pixels[static_cast<std::size_t>(y) * subWidth + x] = c;
  };
  for (std::size_t i : picks)
  {
    const auto& c = itsCityIndex.at(i);
    double u = 0;
    double v = 0;
    itsSource->latLonToUV(c.lat, c.lon, u, v);
    const int cx = static_cast<int>((u - itsViewport.uMin) / spanU * subWidth);
    const int cy = static_cast<int>((v - itsViewport.vMin) / spanV * subHeight);
    // 3×3 dark halo around a 1×1 white centre — readable against any palette.
    for (int dy = -1; dy <= 1; ++dy)
      for (int dx = -1; dx <= 1; ++dx) plot(cx + dx, cy + dy, halo);
    plot(cx, cy, dot);
  }
}

std::string App::buildCityLabels(int cellW, int cellH, int originRow, int originCol)
{
  if (!itsShowCities || cellW <= 0 || cellH <= 0) return {};
  if (!ensureCityIndex()) return {};
  const float spanU = itsViewport.uMax - itsViewport.uMin;
  const float spanV = itsViewport.vMax - itsViewport.vMin;
  if (spanU <= 0 || spanV <= 0) return {};

  const auto picks = visibleCities(itsCityIndex, *itsSource, itsViewport, itsCityOverlayN);
  // 1 cell of occupancy per (row, col); reject placements that overlap.
  std::vector<std::vector<bool>> occupied(cellH, std::vector<bool>(cellW, false));
  std::ostringstream os;
  for (std::size_t i : picks)
  {
    const auto& c = itsCityIndex.at(i);
    double u = 0;
    double v = 0;
    itsSource->latLonToUV(c.lat, c.lon, u, v);
    const int cellX = static_cast<int>((u - itsViewport.uMin) / spanU * cellW);
    const int cellY = static_cast<int>((v - itsViewport.vMin) / spanV * cellH);
    if (cellX < 0 || cellX >= cellW || cellY < 0 || cellY >= cellH) continue;

    const std::string& name = c.name;
    const int w = displayCells(name);
    // Place label one cell right of the dot. Try a few fallback offsets if
    // the primary placement collides with a higher-population label.
    static const std::array<std::pair<int, int>, 5> kOffsets = {{
        {1, 0},   // right
        {-1, 0},  // left (will need to be drawn from the right edge)
        {0, -1},  // above
        {0, 1},   // below
        {2, 0},   // further right
    }};
    bool placed = false;
    for (const auto& [dx, dy] : kOffsets)
    {
      const int rowY = cellY + dy;
      int startX;
      if (dx >= 0)
        startX = cellX + dx;
      else
        startX = cellX + dx - w + 1;  // anchor right edge to the left of dot
      if (rowY < 0 || rowY >= cellH || startX < 0 || startX + w > cellW) continue;
      bool collision = false;
      for (int k = 0; k < w; ++k)
        if (occupied[rowY][startX + k]) { collision = true; break; }
      if (collision) continue;
      for (int k = 0; k < w; ++k) occupied[rowY][startX + k] = true;
      // White on a dim background so the label is legible against any palette.
      os << "\x1b[" << (originRow + rowY + 1) << ';' << (originCol + startX + 1) << 'H'
         << "\x1b[1;48;5;235;38;5;231m" << name << "\x1b[0m";
      placed = true;
      break;
    }
    if (!placed) continue;
  }
  return os.str();
}

std::string App::currentTimeLabel() const
{
  NFmiMetTime t = itsSource->currentValidTime();
  return fmt::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02} UTC",
                     static_cast<int>(t.GetYear()), static_cast<int>(t.GetMonth()),
                     static_cast<int>(t.GetDay()), static_cast<int>(t.GetHour()),
                     static_cast<int>(t.GetMin()), static_cast<int>(t.GetSec()));
}

void App::renderTimeline(UI& ui)
{
  const int id = itsSource->currentParamId();
  std::string label = itsSource->paramShortName(id);
  label += "  ";
  label += currentTimeLabel();
  if (const std::string orig = originTimeLabel(); !orig.empty())
    label += "   (analysis " + orig + ")";
  if (itsAnimating) label += fmt::format("  [{} ms]", itsAnimationDelayMs);
  if (!itsLastMessage.empty())
  {
    label += "   ";
    label += itsLastMessage;
    itsLastMessage.clear();
  }
  ui.drawTimeline(label, static_cast<int>(itsSource->currentTimeIndex()),
                  static_cast<int>(itsSource->timeCount()));
  ui.drawStatusBar();
  doupdate();
}

std::string App::originTimeLabel() const
{
  NFmiMetTime t = itsSource->originTime();
  // Year 0 = explicit "no origin time"; pre-2000 = grid-files placeholder
  // (NetCDF without a reference-time attribute parses as ~1970-10-01).
  // Either way, suppress the suffix rather than displaying obvious noise.
  if (t.GetYear() < 2000) return {};
  return fmt::format("{:04}-{:02}-{:02} {:02}:{:02} UTC",
                     static_cast<int>(t.GetYear()), static_cast<int>(t.GetMonth()),
                     static_cast<int>(t.GetDay()), static_cast<int>(t.GetHour()),
                     static_cast<int>(t.GetMin()));
}

std::vector<std::string> App::paramLabels() const
{
  std::vector<std::string> out;
  out.reserve(itsParamIds.size());
  for (int id : itsParamIds) out.push_back(itsSource->paramShortName(id));
  return out;
}

std::vector<std::string> App::levelLabels() const
{
  std::vector<std::string> out;
  out.reserve(itsSource->levelCount());
  for (std::size_t i = 0; i < itsSource->levelCount(); ++i)
    out.push_back(fmt::format("{:g}", itsSource->levelValueAt(i)));
  return out;
}

void App::selectParam(int newIndex)
{
  if (newIndex < 0 || newIndex >= static_cast<int>(itsParamIds.size())) return;
  itsParamIndex = newIndex;
  itsSource->selectParamId(itsParamIds[newIndex]);
  loadPalette();  // re-resolve palette for the new parameter
}

void App::selectLevel(int newIndex)
{
  if (newIndex < 0 || newIndex >= static_cast<int>(itsSource->levelCount())) return;
  itsSource->selectLevelIndex(static_cast<unsigned long>(newIndex));
  itsOpts.levelIndex = newIndex;
}

bool App::cellToViewport(const UI& ui, int cellX, int cellY, float& u, float& v) const
{
  const auto& l = ui.layout();
  if (cellX < l.map.col || cellX >= l.map.col + l.map.width || cellY < l.map.row ||
      cellY >= l.map.row + l.map.height || l.map.width <= 0 || l.map.height <= 0)
    return false;
  const float relX = (static_cast<float>(cellX - l.map.col) + 0.5F) / l.map.width;
  const float relY = (static_cast<float>(cellY - l.map.row) + 0.5F) / l.map.height;
  u = itsViewport.uMin + relX * (itsViewport.uMax - itsViewport.uMin);
  v = itsViewport.vMin + relY * (itsViewport.vMax - itsViewport.vMin);
  return true;
}

bool App::cellToLatLon(const UI& ui, int cellX, int cellY, double& lat, double& lon) const
{
  float u = 0;
  float v = 0;
  if (!cellToViewport(ui, cellX, cellY, u, v)) return false;
  itsSource->uvToLatLon(u, v, lat, lon);
  return true;
}

void App::openProbe(int cellX, int cellY, UI& ui)
{
  double lat = 0;
  double lon = 0;
  if (!cellToLatLon(ui, cellX, cellY, lat, lon)) return;
  itsMarker = std::make_pair(lat, lon);
  openProbeAt(lat, lon, ui);
}

void App::openProbeAt(double lat, double lon, UI& ui)
{
  // The popup runs an event loop until the user closes it via the keyboard.
  // A click outside the chart but on the map area is reported back as a
  // request to re-probe at that cell — we update the marker, redraw the
  // map, and reopen the popup at the new location.
  while (true)
  {
    // Sample the current parameter at this lat/lon for every time step.
    // Save and restore the time index so the rest of the UI keeps its state.
    std::vector<float> series;
    std::vector<std::string> timeLabels;
    series.reserve(itsSource->timeCount());
    timeLabels.reserve(itsSource->timeCount());
    const std::size_t savedTime = itsSource->currentTimeIndex();
    for (std::size_t i = 0; i < itsSource->timeCount(); ++i)
    {
      itsSource->selectTimeIndex(i);
      series.push_back(transform(itsSource->interpolatedValue(lat, lon)));
      timeLabels.push_back(currentTimeLabel());
    }
    itsSource->selectTimeIndex(savedTime);

    NFmiEnumConverter conv;
    std::string param = itsSource->paramShortName(itsSource->currentParamId());

    // Callback: when the user presses an arrow inside the popup, update the
    // querydata's time index, refresh the bottom timeline label so the time
    // there tracks the popup marker, then redraw the map underneath. The
    // popup loop re-emits its raw-ANSI on top, so the popup stays visible.
    auto onTimeChange = [&](int newIdx) {
      itsSource->selectTimeIndex(static_cast<unsigned long>(newIdx));
      renderTimeline(ui);
      drawMap(ui);
    };

    // Translate the marker lat/lon into a terminal cell so the popup can
    // shift to the opposite quadrant and keep the crosshair visible.
    int avoidRow = -1;
    int avoidCol = -1;
    if (itsMarker.has_value())
    {
      const auto& l = ui.layout();
      const float spanU = itsViewport.uMax - itsViewport.uMin;
      const float spanV = itsViewport.vMax - itsViewport.vMin;
      if (spanU > 0 && spanV > 0 && l.map.width > 0 && l.map.height > 0)
      {
        double u = 0;
        double v = 0;
        itsSource->latLonToUV(itsMarker->first, itsMarker->second, u, v);
        const double u01 = (u - itsViewport.uMin) / spanU;
        const double v01 = (v - itsViewport.vMin) / spanV;
        if (u01 >= 0 && u01 <= 1 && v01 >= 0 && v01 <= 1)
        {
          avoidCol = l.map.col + static_cast<int>(u01 * l.map.width);
          avoidRow = l.map.row + static_cast<int>(v01 * l.map.height);
        }
      }
    }

    int clickRow = -1;
    int clickCol = -1;
    int finalIdx = ui.popupTimeseries(param, lat, lon, series, timeLabels,
                                      static_cast<int>(savedTime), itsRenderer, itsPalette,
                                      onTimeChange, avoidRow, avoidCol, &clickRow, &clickCol);
    itsSource->selectTimeIndex(static_cast<unsigned long>(finalIdx));

    if (clickRow < 0 || clickCol < 0) break;  // closed via keyboard

    double newLat = 0;
    double newLon = 0;
    if (!cellToLatLon(ui, clickCol, clickRow, newLat, newLon)) break;

    itsMarker = std::make_pair(newLat, newLon);
    drawMap(ui);
    lat = newLat;
    lon = newLon;
  }
}

bool App::handleKey(int key, UI& ui, bool& quit)
{
  switch (key)
  {
    case 'q':
    case 'Q':
    case 27:  // Esc
      quit = true;
      return false;

    case KEY_LEFT:
    {
      auto idx = itsSource->currentTimeIndex();
      if (idx > 0)
      {
        itsSource->selectTimeIndex(idx - 1);
        return true;
      }
      return false;
    }
    case KEY_RIGHT:
    {
      auto idx = itsSource->currentTimeIndex();
      if (idx + 1 < itsSource->timeCount())
      {
        itsSource->selectTimeIndex(idx + 1);
        return true;
      }
      return false;
    }
    case KEY_HOME:
      itsSource->selectTimeIndex(0);
      return true;
    case KEY_END:
      if (itsSource->timeCount() > 0)
        itsSource->selectTimeIndex(itsSource->timeCount() - 1);
      return true;

    case 'p':
    case 'P':
    {
      int picked = ui.popupMenu("Parameters", paramLabels(), itsParamIndex);
      if (picked >= 0) selectParam(picked);
      return true;  // even on cancel, we need to repaint over the popup
    }
    case 'L':  // uppercase only — lowercase 'l' is reserved for pan-right
    {
      int picked = ui.popupMenu("Levels", levelLabels(),
                                static_cast<int>(itsSource->currentLevelIndex()));
      if (picked >= 0) selectLevel(picked);
      return true;
    }

    case '+':
    case '=':
      itsViewport.zoom(0.7F);
      return true;
    case '-':
    case '_':
      itsViewport.zoom(1.0F / 0.7F);
      return true;
    case '0':
      itsViewport.reset();
      return true;

    case 'g':
    case 'G':
    {
      NFmiEnumConverter conv;
      std::string param = itsSource->paramShortName(itsSource->currentParamId());
      ui.popupLegend(param, itsPalette.name(), itsPalette, itsRenderer);
      return true;
    }

    case '?':
      ui.popupHelp();
      return true;

    case 'b':
    case 'B':
      itsOpts.noBorders = !itsOpts.noBorders;
      if (itsOpts.noBorders)
      {
        itsBorders.clear();
        itsBorderPath.clear();
      }
      else
      {
        itsBorderPath.clear();  // force reload on next drawMap
      }
      return true;

    case 'c':
    case 'C':
      itsOpts.noCoastline = !itsOpts.noCoastline;
      if (itsOpts.noCoastline)
      {
        itsCoastlines.clear();
        itsCoastlinePath.clear();
      }
      else
      {
        itsCoastlinePath.clear();
      }
      return true;

    case 'n':
    case 'N':
      itsShowGraticule = !itsShowGraticule;
      return true;

    case 'w':
    case 'W':
      itsShowWindArrows = !itsShowWindArrows;
      return true;

    case 'i':
    case 'I':
      itsShowCities = !itsShowCities;
      itsLastMessage = itsShowCities
                           ? fmt::format("Cities: top {} visible", itsCityOverlayN)
                           : "Cities off";
      return true;

    case KEY_NPAGE:
    {
      // Denser: step N up.
      auto it = std::upper_bound(kCityNSteps.begin(), kCityNSteps.end(), itsCityOverlayN);
      if (it != kCityNSteps.end()) itsCityOverlayN = *it;
      itsShowCities = true;
      itsLastMessage = fmt::format("Cities: top {}", itsCityOverlayN);
      return true;
    }
    case KEY_PPAGE:
    {
      // Sparser: step N down.
      auto it = std::lower_bound(kCityNSteps.begin(), kCityNSteps.end(), itsCityOverlayN);
      if (it != kCityNSteps.begin())
      {
        --it;
        itsCityOverlayN = *it;
      }
      itsShowCities = true;
      itsLastMessage = fmt::format("Cities: top {}", itsCityOverlayN);
      return true;
    }

    case 'e':
    case 'E':
    {
      std::string err;
      const std::string fname = exportPng(err);
      // Show the result on the timeline header next refresh.
      if (!fname.empty())
        itsLastMessage = "Saved " + fname;
      else
        itsLastMessage = "Export failed: " + err;
      return true;
    }

    case '/':
      openPlaceSearch(ui);
      return true;

    case 'x':
    case 'X':
      if (itsCrossPicks == 0)
      {
        itsCrossPicks = 2;
        itsLastMessage = "Cross-section: click first endpoint";
      }
      else
      {
        itsCrossPicks = 0;
        itsLastMessage = "Cross-section cancelled";
      }
      return true;

    // Pan: vim-style hjkl (lowercase) or shift-arrow.
    case 'h':
    case KEY_SLEFT:
      itsViewport.pan(-0.2F, 0);
      return true;
    case 'l':
    case KEY_SRIGHT:
      itsViewport.pan(0.2F, 0);
      return true;
    case 'j':
    case KEY_SR:  // shift+down → view shifts south
      itsViewport.pan(0, 0.2F);
      return true;
    case 'k':
    case KEY_SF:  // shift+up → view shifts north
      itsViewport.pan(0, -0.2F);
      return true;

    case KEY_RESIZE:
      ui.recomputeLayout();
      itsDragging = false;
      return true;

    case ' ':  // Space: toggle animation
      itsAnimating = !itsAnimating;
      return true;

    case KEY_UP:
      // Faster: shrink delay by ~30% per press, floor at 50 ms (~20 fps).
      itsAnimationDelayMs = std::max(50, static_cast<int>(itsAnimationDelayMs * 0.7));
      return true;

    case KEY_DOWN:
      // Slower: grow delay by ~30%, cap at 2000 ms (0.5 fps).
      itsAnimationDelayMs = std::min(2000, static_cast<int>(itsAnimationDelayMs / 0.7));
      return true;

    case KEY_MOUSE:
    {
      MEVENT ev;
      if (getmouse(&ev) != OK) return false;

      // Optional debug log so we can inspect what the terminal actually
      // sends. Set QDLESS_DEBUG_MOUSE=1 in the environment to enable.
      static const bool kMouseDebug = std::getenv("QDLESS_DEBUG_MOUSE") != nullptr;
      if (kMouseDebug)
      {
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        FILE* f = std::fopen("/tmp/qdless-mouse.log", "a");
        if (f != nullptr)
        {
          fmt::print(f, "mouse x={} y={} bstate=0x{:x}\n", ev.x, ev.y,
                     static_cast<unsigned long>(ev.bstate));
          std::fclose(f);
        }
      }

      const auto& l = ui.layout();
      const bool inMap =
          ev.x >= l.map.col && ev.x < l.map.col + l.map.width && ev.y >= l.map.row &&
          ev.y < l.map.row + l.map.height;

      // Wheel-up / wheel-down: kept for terminals that deliver wheel events
      // as BUTTON4/5 through ncurses. (Many don't, so double-click is the
      // primary zoom gesture.)
      const auto kWheelUp =
          static_cast<mmask_t>(BUTTON4_PRESSED | BUTTON4_RELEASED | BUTTON4_CLICKED);
      const auto kWheelDown =
          static_cast<mmask_t>(BUTTON5_PRESSED | BUTTON5_RELEASED | BUTTON5_CLICKED);
      if (((ev.bstate & kWheelUp) != 0U) && inMap)
      {
        float u = 0;
        float v = 0;
        if (cellToViewport(ui, ev.x, ev.y, u, v)) itsViewport.zoomAt(0.85F, u, v);
        return true;
      }
      if (((ev.bstate & kWheelDown) != 0U) && inMap)
      {
        float u = 0;
        float v = 0;
        if (cellToViewport(ui, ev.x, ev.y, u, v))
          itsViewport.zoomAt(1.0F / 0.85F, u, v);
        return true;
      }

      // Double-click: zoom in (left) or zoom out (right), anchored at the
      // click position. Set BEFORE single-click probe handler so we don't
      // also pop the probe.
      if (((ev.bstate & BUTTON1_DOUBLE_CLICKED) != 0U) && inMap)
      {
        float u = 0;
        float v = 0;
        if (cellToViewport(ui, ev.x, ev.y, u, v)) itsViewport.zoomAt(0.7F, u, v);
        itsDragging = false;  // double-click can be preceded by stray PRESS
        return true;
      }
      if (((ev.bstate & BUTTON3_DOUBLE_CLICKED) != 0U) && inMap)
      {
        float u = 0;
        float v = 0;
        if (cellToViewport(ui, ev.x, ev.y, u, v))
          itsViewport.zoomAt(1.0F / 0.7F, u, v);
        itsDragging = false;
        return true;
      }

      if (((ev.bstate & BUTTON1_PRESSED) != 0U) && inMap)
      {
        itsDragging = true;
        itsDragStartX = ev.x;
        itsDragStartY = ev.y;
        return false;  // no redraw yet
      }

      if ((ev.bstate & BUTTON1_RELEASED) != 0U)
      {
        if (!itsDragging) return false;
        itsDragging = false;
        const int dx = ev.x - itsDragStartX;
        const int dy = ev.y - itsDragStartY;
        // < 2 cells of motion → click, not drag.
        if (std::abs(dx) + std::abs(dy) < 2)
        {
          openProbe(itsDragStartX, itsDragStartY, ui);
          return true;
        }
        // Drag → pan. Negative because dragging the map content right means
        // the viewport "looks" further left.
        if (l.map.width > 0 && l.map.height > 0)
        {
          itsViewport.pan(-static_cast<float>(dx) / l.map.width,
                          -static_cast<float>(dy) / l.map.height);
        }
        return true;
      }

      // Cross-section pending pick: handle BEFORE the regular click→probe.
      if (itsCrossPicks > 0 && ((ev.bstate & BUTTON1_CLICKED) != 0U) && inMap)
      {
        if (itsCrossPicks == 2)
        {
          itsCrossX1 = ev.x;
          itsCrossY1 = ev.y;
          itsCrossPicks = 1;
          itsLastMessage = "Cross-section: click second endpoint";
        }
        else
        {
          renderCrossSection(itsCrossX1, itsCrossY1, ev.x, ev.y, ui);
          itsCrossPicks = 0;
          itsLastMessage.clear();
        }
        return true;
      }

      // Single-click (no drag) — some terminals deliver only CLICKED.
      if (((ev.bstate & BUTTON1_CLICKED) != 0U) && inMap && !itsDragging)
      {
        openProbe(ev.x, ev.y, ui);
        return true;
      }

      return false;
    }

    default:
      return false;
  }
}

void App::drawMap(UI& ui)
{
  const auto& l = ui.layout();
  if (l.map.height < 2 || l.map.width < 2) return;
  int subWidth = l.map.width * 2;
  int subHeight = l.map.height * 2;

  // Re-pick coastline resolution for the current viewport. Cheap when the
  // selected file is unchanged; reads ~100ms at h-resolution worst case.
  loadCoastlines();

  float dataMin = 0;
  float dataMax = 0;
  auto pixels = sampleSlice(subWidth, subHeight, dataMin, dataMax);
  if (itsShowGraticule) overlayGraticule(pixels, subWidth, subHeight);
  overlayPolylines(pixels, subWidth, subHeight, itsCoastlines, Rgb{0, 0, 0});
  overlayPolylines(pixels, subWidth, subHeight, itsBorders, Rgb{90, 90, 90});
  overlayCities(pixels, subWidth, subHeight);
  overlayMarker(pixels, subWidth, subHeight);

  // Bypass ncurses: write raw ANSI directly to stdout, positioned at map origin.
  std::ostringstream os;
  itsRenderer.render(os, pixels, subWidth, subHeight, l.map.row, l.map.col);

  // Wind arrow + city label overlays drawn AFTER the raster so glyphs sit
  // on top of the quadrant blocks.
  if (itsShowWindArrows)
    os << buildWindArrows(l.map.width, l.map.height, l.map.row, l.map.col);
  os << buildCityLabels(l.map.width, l.map.height, l.map.row, l.map.col);

  std::string s = os.str();
  std::fwrite(s.data(), 1, s.size(), stdout);
  std::fflush(stdout);
}

int App::runOnce()
{
  TerminalSize ts = terminalSize();
  int cellW = ts.cols;
  int cellH = std::max(1, ts.rows - 1);
  int subWidth = cellW * 2;
  int subHeight = cellH * 2;

  float dataMin = 0;
  float dataMax = 0;
  auto pixels = sampleSlice(subWidth, subHeight, dataMin, dataMax);
  overlayPolylines(pixels, subWidth, subHeight, itsCoastlines, Rgb{0, 0, 0});
  overlayPolylines(pixels, subWidth, subHeight, itsBorders, Rgb{90, 90, 90});
  overlayCities(pixels, subWidth, subHeight);
  overlayMarker(pixels, subWidth, subHeight);

  const int id = itsSource->currentParamId();
  std::string shortName = itsSource->paramShortName(id);
  const std::string origLabel = originTimeLabel();
  std::cout << "[qdless] " << itsOpts.filename << " | param: " << shortName << " | time: "
            << currentTimeLabel() << " (" << (itsSource->currentTimeIndex() + 1) << "/"
            << itsSource->timeCount() << ")";
  if (!origLabel.empty()) std::cout << " | analysis: " << origLabel;
  std::cout << " | level: " << itsSource->levelValueAt(itsSource->currentLevelIndex())
            << " (" << (itsSource->currentLevelIndex() + 1) << "/" << itsSource->levelCount()
            << ") | range: [" << dataMin << ", " << dataMax << "] | palette: " << itsPalette.name()
            << " | coast: " << itsCoastlines.size() << "+" << itsBorders.size() << " polylines\n";

  std::ostringstream os;
  itsRenderer.render(os, pixels, subWidth, subHeight, 1, 0);
  os << buildCityLabels(cellW, cellH, 1, 0);
  std::cout << os.str() << "\x1b[" << ts.rows << ";1H" << '\n';
  return 0;
}

int App::runInteractive()
{
  UI ui;

  bool quit = false;
  bool needRedraw = true;

  while (!quit)
  {
    if (needRedraw)
    {
      // Order matters: ncurses' first doupdate() force-paints the whole
      // screen with blanks, which would clobber a raw-ANSI map written
      // beforehand. So commit ncurses windows first, then draw the map
      // on top via raw escapes.
      renderTimeline(ui);
      drawMap(ui);
      needRedraw = false;
    }

    int key = ui.waitInput(itsAnimating ? itsAnimationDelayMs : -1);
    if (key == ERR)
    {
      // Timeout: advance to next frame.
      auto idx = itsSource->currentTimeIndex();
      const auto n = itsSource->timeCount();
      itsSource->selectTimeIndex((idx + 1) % n);
      needRedraw = true;
      continue;
    }
    needRedraw = handleKey(key, ui, quit);
  }
  return 0;
}
}  // namespace Qdless
