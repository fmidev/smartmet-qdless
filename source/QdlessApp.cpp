#include "QdlessApp.h"

#include "QdlessExitEffect.h"
#include "QdlessPhenomena.h"
#include "QdlessExtrema.h"
#include "QdlessMultiFileSource.h"
#include "QdlessOdimVolumeSource.h"
#include "QdlessQueryDataSource.h"
#include "QdlessShapeSource.h"
#include "QdlessUI.h"

#include <gdal_priv.h>
#include <ogrsf_frmts.h>

#include <newbase/NFmiEnumConverter.h>
#include <newbase/NFmiGlobals.h>
#include <newbase/NFmiPoint.h>

#include <ncurses.h>

#include <boost/dll/runtime_symbol_info.hpp>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <json/json.h>

#include <sys/ioctl.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace Qdless
{
namespace
{
const char* panelLayoutLabel(PanelLayout l)
{
  switch (l)
  {
    case PanelLayout::Single:
      return "Layout: single";
    case PanelLayout::Side:
      return "Layout: side-by-side";
    case PanelLayout::Quad:
      return "Layout: 2x2";
  }
  return "Layout: ?";
}

LineStyle nextLineStyle(LineStyle s)
{
  switch (s)
  {
    case LineStyle::Braille:
      return LineStyle::Thick;
    case LineStyle::Thick:
      return LineStyle::None;
    case LineStyle::None:
      return LineStyle::Braille;
  }
  return LineStyle::Braille;
}

const char* lineStyleLabel(LineStyle s)
{
  switch (s)
  {
    case LineStyle::Braille:
      return "braille";
    case LineStyle::Thick:
      return "thick";
    case LineStyle::None:
      return "off";
  }
  return "?";
}

CornerStyle nextCornerStyle(CornerStyle s)
{
  switch (s)
  {
    case CornerStyle::Sextant:
      return CornerStyle::SmallTriangle;
    case CornerStyle::SmallTriangle:
      return CornerStyle::Square;
    case CornerStyle::Square:
      return CornerStyle::Sextant;
  }
  return CornerStyle::Sextant;
}

const char* cornerStyleLabel(CornerStyle s)
{
  switch (s)
  {
    case CornerStyle::Sextant:
      return "sextants";
    case CornerStyle::SmallTriangle:
      return "triangles";
    case CornerStyle::Square:
      return "squares";
  }
  return "?";
}

// Encode a braille codepoint U+2800+mask as a 3-byte UTF-8 string. Mirrors
// the helper in QdlessUI.cpp (kept duplicated to avoid exposing it as a
// public symbol; the layouts are identical).
//   col 0 → bits 0,1,2,6 (rows 0..3); col 1 → bits 3,4,5,7.
std::string brailleGlyph(unsigned mask)
{
  std::string s(3, '\0');
  s[0] = static_cast<char>(0xE2);
  s[1] = static_cast<char>(0xA0 | ((mask >> 6) & 0x03));
  s[2] = static_cast<char>(0x80 | (mask & 0x3F));
  return s;
}

unsigned brailleBit(int subCol, int subRow)
{
  if (subRow == 3)
    return (subCol == 0) ? 6U : 7U;
  return static_cast<unsigned>(subCol * 3 + subRow);
}

// Append a raw-ANSI cursor positioning + glyph for each cell of a vertical
// or horizontal separator. `glyph` is a UTF-8 box-drawing character.
void appendSeparator(
    std::ostringstream& os, int row, int col, int len, bool vertical, const char* glyph)
{
  // ESC[<row>;<col>H is 1-based; map our 0-based positions accordingly.
  // Use a dim cyan-on-black scheme that matches the popup borders.
  static const char* kReset = "\x1b[0m";
  static const char* kStyle = "\x1b[40m\x1b[36m";  // bg black, fg cyan
  for (int i = 0; i < len; ++i)
  {
    const int r = vertical ? row + i : row;
    const int c = vertical ? col : col + i;
    os << "\x1b[" << (r + 1) << ';' << (c + 1) << 'H' << kStyle << glyph << kReset;
  }
}

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
  if (n > 0)
    os << std::string(static_cast<std::size_t>(n), ' ');
}

// "Nice" tick step for an axis spanning `range`: one of {1,2,5}*10^k.
double niceStep(double range, int maxTicks)
{
  if (range <= 0 || maxTicks < 1)
    return 1.0;
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
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

// Returns true if any of `needles` (already lowercase) appears in `text`
// (compared case-insensitively).
bool nameContains(const std::string& text, std::initializer_list<const char*> needles)
{
  const std::string lower = toLower(text);
  return std::any_of(needles.begin(),
                     needles.end(),
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
UnitGuess guessFromUnits(const std::string& shortName,
                         const std::string& longName,
                         const std::string& units)
{
  UnitGuess g;
  const std::string both = shortName + " " + longName;

  // Sea-temperature names take priority over the generic temperature
  // palette so the realistic −2…+25 °C ramp is used instead of the
  // atmospheric −50…+50 °C one.
  auto isSeaTempName = [&]
  {
    return nameContains(both,
                        {"sea_surface_temperature",
                         "sea surface temperature",
                         "sea_temperature",
                         "sea temperature",
                         "temperaturesea"}) ||
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
             shortName == "T-K" || shortName == "TD" || shortName == "TD-K" || shortName == "T2m")
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
  else if (units == "m/s" || units == "m s-1" || units == "m s**-1" || units == "ms-1" ||
           units == "ms**-1")
  {
    if (nameContains(both, {"gust"}))
      g.palette = "windgust";
    else if (shortName == "WindUMS" || shortName == "WindVMS" ||
             nameContains(both,
                          {"u-component of wind",
                           "v-component of wind",
                           "u component of wind",
                           "v component of wind"}))
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
    else if (nameContains(both, {"wind"}) && !nameContains(both,
                                                           {"u-component",
                                                            "v-component",
                                                            "u_component",
                                                            "v_component",
                                                            "windums",
                                                            "windvms"}))
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
          if (lowercased(key) == needle)
            return palettes[key].asString();
        }
      }
    }
  }
  // Fall back to the built-in defaults so it works without an installed conf.
  std::string needle = lowercased(paramName);
  for (const auto& [param, palette] : builtinPaletteMap())
    if (lowercased(param) == needle)
      return palette;
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
  auto bound = [&](float& lo, float& hi)
  {
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
  if (!itsOpts.pgConn.empty())
  {
    // PostGIS browser mode. Open the connection once and keep it
    // alive for the App's lifetime; [T] re-pickers reuse the same
    // dataset. The "PG:" prefix tells GDAL to dispatch to the
    // PostgreSQL driver.
    GDALAllRegister();
    const std::string opener = "PG:" + itsOpts.pgConn;
    auto* ds = static_cast<GDALDataset*>(
        GDALOpenEx(opener.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr));
    if (ds == nullptr)
      throw std::runtime_error("PostGIS connection failed: " + itsOpts.pgConn);
    itsPgDataset = ds;
    itsPanels.resize(1);
    if (!itsOpts.pgTable.empty())
    {
      openPgLayer(itsOpts.pgTable);
    }
    else
    {
      // No --table given: defer the picker until the UI is up — the
      // popupSearch widget needs an active ncurses session.
      itsSource = nullptr;
    }
  }
  else if (!itsOpts.browseRoot.empty())
  {
    // PNG-tree browse mode: defer source creation until the UI is up
    // so the picker can run (same shape as the deferred PG path).
    itsSource = nullptr;
  }
  else if (!itsOpts.filenames.empty() && itsOpts.filenames.size() > 1)
    itsSource = std::make_unique<MultiFileSource>(itsOpts.filenames);
  else
    itsSource = DataSource::open(itsOpts.filename);
  itsPanels.resize(1);
  if (itsSource != nullptr)
    initFromSource();
}

void App::initFromSource()
{
  buildIndices();

  // Capture shapefile outlines into their own slot so [B] / [C] keep
  // cycling GSHHS political borders / coastlines unchanged. Drawn
  // last in the overlay stack (on top of GSHHS) since the user is
  // here to look at the shape, not the country lines.
  if (auto* shp = dynamic_cast<const ShapeSource*>(itsSource.get()))
    itsShapeOutlines = shp->outlines();

  // Seed live overlay styles from CLI flags. After startup these are
  // cycled by the `c` / `b` keys; itsOpts.noCoastline / noBorders are not
  // updated further.
  if (itsOpts.noCoastline)
    itsCoastlineStyle = LineStyle::None;
  if (itsOpts.noBorders)
    itsBorderStyle = LineStyle::None;
  // Image mode (naked PNG/WebP/JPEG/...): force every geographic overlay
  // off. The image has no projection — drawing a coastline at unit-square
  // (lat,lon) coords would spew lines at the wrong scale on top of a
  // pre-rendered radar PNG that already burned its coastline in. The
  // keyboard handler also gates the `c`/`b` toggles so the user can't
  // accidentally turn them back on.
  if (itsSource->isImage())
  {
    itsCoastlineStyle = LineStyle::None;
    itsBorderStyle = LineStyle::None;
    itsGraticuleStyle = LineStyle::None;
    itsShowWindArrows = false;
    itsShowCities = false;
  }

  // Apply parameter overrides. -p accepts a comma-separated list:
  //   1 entry  -> Single layout
  //   2        -> Side
  //   3 or 4   -> Quad (with 4th panel cloning the first if only 3 given)
  //   >4       -> error
  // --layout overrides the count-derived choice but must hold at least the
  // number of parameters given.
  std::vector<int> overrideIdx;
  if (!itsOpts.parameterOverrides.empty())
  {
    if (itsOpts.parameterOverrides.size() > 4)
      throw std::runtime_error("qdless: -p accepts at most 4 parameters (got " +
                               std::to_string(itsOpts.parameterOverrides.size()) + ")");
    NFmiEnumConverter conv;
    overrideIdx.reserve(itsOpts.parameterOverrides.size());
    for (const auto& name : itsOpts.parameterOverrides)
    {
      const int id = conv.ToEnum(name);
      auto it = std::find(itsParamIds.begin(), itsParamIds.end(), id);
      if (id == kFmiBadParameter || it == itsParamIds.end())
        throw std::runtime_error("qdless: parameter not found: " + name);
      overrideIdx.push_back(static_cast<int>(it - itsParamIds.begin()));
    }
    activePanel().paramIndex = overrideIdx.front();
    itsSource->selectParamId(itsParamIds[overrideIdx.front()]);
  }

  // Pick layout: explicit --layout, else from override count.
  PanelLayout targetLayout = PanelLayout::Single;
  if (!itsOpts.layoutOverride.empty())
  {
    std::string s = itsOpts.layoutOverride;
    for (auto& c : s)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (s == "single")
      targetLayout = PanelLayout::Single;
    else if (s == "side" || s == "side-by-side")
      targetLayout = PanelLayout::Side;
    else if (s == "quad" || s == "2x2")
      targetLayout = PanelLayout::Quad;
    else
      throw std::runtime_error("qdless: unknown --layout '" + itsOpts.layoutOverride +
                               "' (expected single, side, or quad)");
  }
  else if (overrideIdx.size() == 2)
    targetLayout = PanelLayout::Side;
  else if (overrideIdx.size() >= 3)
    targetLayout = PanelLayout::Quad;

  const std::size_t want =
      (targetLayout == PanelLayout::Quad) ? 4 : (targetLayout == PanelLayout::Side ? 2 : 1);
  if (overrideIdx.size() > want)
    throw std::runtime_error("qdless: --layout " + itsOpts.layoutOverride + " holds " +
                             std::to_string(want) + " panel(s), but " +
                             std::to_string(overrideIdx.size()) + " parameters were given");

  auto resolveIndex = [](int requested, std::size_t size) -> std::size_t
  {
    if (size == 0)
      return 0;
    if (requested < 0)
      return size - 1;
    if (static_cast<std::size_t>(requested) >= size)
      throw std::runtime_error("qdless: index " + std::to_string(requested) +
                               " out of range (size " + std::to_string(size) + ")");
    return static_cast<std::size_t>(requested);
  };
  itsSource->selectTimeIndex(resolveIndex(itsOpts.timeIndex, itsSource->timeCount()));
  const std::size_t levelIdx = resolveIndex(itsOpts.levelIndex, itsSource->levelCount());
  itsSource->selectLevelIndex(levelIdx);
  activePanel().levelIndex = levelIdx;

  // Resolve the active panel's palette before any further setPanelLayout
  // grow: setPanelLayout's clone path copies the active panel's palette
  // into new slots, then re-resolves per parameter.
  loadPalette();
  loadCoastlines();

  // Grow into the requested layout. setPanelLayout clones the active panel
  // and rotates paramIndex by slot; for explicit overrides we then assign
  // each slot from the user-provided list (with slot 3 cloning slot 0 when
  // only 3 parameters were given).
  if (want > 1)
  {
    setPanelLayout(targetLayout);
    if (!overrideIdx.empty())
    {
      const int n = static_cast<int>(itsParamIds.size());
      for (std::size_t i = 0; i < itsPanels.size(); ++i)
      {
        const std::size_t src = (i < overrideIdx.size()) ? i : 0;
        const int newParamIdx = overrideIdx[src];
        if (itsPanels[i].paramIndex == newParamIdx)
          continue;
        itsPanels[i].paramIndex = newParamIdx;
        // Re-resolve palette for this slot.
        const int savedActive = itsActivePanel;
        itsActivePanel = static_cast<int>(i);
        if (newParamIdx >= 0 && newParamIdx < n)
          itsSource->selectParamId(itsParamIds[newParamIdx]);
        itsSource->selectLevelIndex(itsPanels[i].levelIndex);
        loadPalette();
        itsActivePanel = savedActive;
      }
      // Restore source to active panel.
      if (activePanel().paramIndex >= 0 &&
          activePanel().paramIndex < static_cast<int>(itsParamIds.size()))
        itsSource->selectParamId(itsParamIds[activePanel().paramIndex]);
      itsSource->selectLevelIndex(activePanel().levelIndex);
    }
  }
  // The auto-shift toast from loadPalette() is informational; suppress it
  // on startup so we don't ship the splash with stale text.
  itsLastMessage.clear();
  // Phenomenon detectors look at the currently-selected param/level. Run
  // them now so the first redraw already shows the hint for whichever
  // field the user landed on (or the CLI pinned).
  refreshPhenomenonHint();
}

App::~App()
{
  if (itsPgDataset != nullptr)
  {
    GDALClose(static_cast<GDALDataset*>(itsPgDataset));
    itsPgDataset = nullptr;
  }
}

namespace
{
// Strip a "schema." prefix when only the bare table name was wanted,
// or return the input unchanged. Used for matching layer names that
// OGR's PostgreSQL driver reports as "schema.name".
std::string stripSchema(const std::string& full)
{
  auto dot = full.find('.');
  return (dot == std::string::npos) ? full : full.substr(dot + 1);
}
}  // namespace

void App::openPgLayer(const std::string& schemaTable)
{
  auto* ds = static_cast<GDALDataset*>(itsPgDataset);
  if (ds == nullptr)
    throw std::runtime_error("openPgLayer: no PG dataset");
  // Try both the qualified ("schema.table") and bare ("table") name —
  // OGR's PG driver normally reports layers as "schema.name", but
  // GetLayerByName accepts either form.
  OGRLayer* layer = ds->GetLayerByName(schemaTable.c_str());
  if (layer == nullptr)
    layer = ds->GetLayerByName(stripSchema(schemaTable).c_str());
  if (layer == nullptr)
    throw std::runtime_error("PostGIS table not found: " + schemaTable);
  itsSource = std::make_unique<ShapeSource>(layer, schemaTable);
}

bool App::openPgPicker(UI& ui)
{
  auto* ds = static_cast<GDALDataset*>(itsPgDataset);
  if (ds == nullptr)
    return false;
  // Build the layer list once. Filtering by --schema (if given) is a
  // simple prefix match against the OGR-reported "schema.name".
  struct Entry
  {
    std::string fullName;
    std::string display;  // "schema.name (Polygon, 16 features)"
  };
  std::vector<Entry> entries;
  const std::string prefix = itsOpts.pgSchema.empty() ? std::string{} : itsOpts.pgSchema + ".";
  for (int i = 0; i < ds->GetLayerCount(); ++i)
  {
    OGRLayer* layer = ds->GetLayer(i);
    if (layer == nullptr)
      continue;
    std::string name = layer->GetName();
    if (!prefix.empty() && name.compare(0, prefix.size(), prefix) != 0)
      continue;
    // Hide non-spatial tables. OGR's PostgreSQL driver lists ALL
    // tables visible to the role, including pure attribute tables
    // (no geometry column) — those would land in the picker as
    // e.g. "wind" without anything renderable. wkbUnknown is OK
    // (mixed geometries), only wkbNone means "no geometry column".
    const OGRwkbGeometryType gt = layer->GetGeomType();
    if (gt == wkbNone)
      continue;
    const char* gtype = OGRGeometryTypeToName(gt);
    // GetFeatureCount(false) skips the COUNT(*) round-trip; OGR
    // returns -1 if the driver can't supply a fast estimate.
    GIntBig n = layer->GetFeatureCount(/*force=*/0);
    std::string display = name;
    display += "  (";
    display += (gtype ? gtype : "Geometry");
    if (n >= 0)
    {
      display += ", ";
      display += std::to_string(static_cast<long long>(n));
      display += " features";
    }
    display += ")";
    entries.push_back({std::move(name), std::move(display)});
  }
  if (entries.empty())
  {
    throw std::runtime_error(std::string("PostGIS: no layers visible") +
                             (prefix.empty() ? "" : " in schema " + itsOpts.pgSchema));
  }
  // Auto-pick when only one match — the user's intent is unambiguous.
  if (entries.size() == 1)
  {
    openPgLayer(entries.front().fullName);
    return true;
  }
  // popupSearch over the entries; substring-filter on the display
  // string. The matcher's lastIdx vector remembers which entry each
  // visible row came from.
  std::vector<int> lastIdx;
  auto matcher = [&](const std::string& q)
  {
    std::vector<std::string> hits;
    lastIdx.clear();
    std::string lq = q;
    std::transform(
        lq.begin(), lq.end(), lq.begin(), [](unsigned char c) { return std::tolower(c); });
    for (std::size_t i = 0; i < entries.size(); ++i)
    {
      if (lq.empty())
      {
        hits.push_back(entries[i].display);
        lastIdx.push_back(static_cast<int>(i));
        continue;
      }
      std::string lower = entries[i].display;
      std::transform(lower.begin(),
                     lower.end(),
                     lower.begin(),
                     [](unsigned char c) { return std::tolower(c); });
      if (lower.find(lq) != std::string::npos)
      {
        hits.push_back(entries[i].display);
        lastIdx.push_back(static_cast<int>(i));
      }
    }
    return hits;
  };
  const std::string title =
      "PostGIS layers" + (prefix.empty() ? std::string{} : " in " + itsOpts.pgSchema);
  const int sel = ui.popupSearch(title, matcher);
  if (sel < 0 || sel >= static_cast<int>(lastIdx.size()))
    return false;
  openPgLayer(entries[lastIdx[sel]].fullName);
  return true;
}

namespace
{
bool isPngFile(const std::filesystem::path& p)
{
  if (!p.has_extension())
    return false;
  std::string ext = p.extension().string();
  std::transform(
      ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
  return ext == ".png";
}

// True if the directory contains at least one *.png as a regular file.
bool hasPngDirectChild(const std::filesystem::path& dir)
{
  std::error_code ec;
  for (auto it = std::filesystem::directory_iterator(dir, ec);
       it != std::filesystem::directory_iterator();
       it.increment(ec))
  {
    if (ec)
      return false;
    if (it->is_regular_file(ec) && isPngFile(it->path()))
      return true;
  }
  return false;
}

// Collect *.png files in `dir`, sorted by filename so the animation
// plays in timestamp order (production filenames embed a sortable
// timestamp prefix).
std::vector<std::string> collectSortedPngs(const std::filesystem::path& dir)
{
  std::vector<std::string> out;
  std::error_code ec;
  for (auto it = std::filesystem::directory_iterator(dir, ec);
       it != std::filesystem::directory_iterator();
       it.increment(ec))
  {
    if (ec)
      break;
    if (it->is_regular_file(ec) && isPngFile(it->path()))
      out.push_back(it->path().string());
  }
  std::sort(out.begin(), out.end());
  return out;
}
}  // namespace

void App::scanBrowseTree()
{
  itsBrowseLeaves.clear();
  itsBrowseLeavesScanned = true;
  if (itsOpts.browseRoot.empty())
    return;
  const std::filesystem::path root(itsOpts.browseRoot);
  std::error_code ec;
  if (!std::filesystem::is_directory(root, ec))
    return;

  // Pre-order walk. A directory that directly contains PNGs is a leaf
  // and we don't descend further into it — production trees put images
  // in the terminal-most directory only.
  std::vector<std::filesystem::path> stack{root};
  while (!stack.empty())
  {
    auto cur = stack.back();
    stack.pop_back();
    if (hasPngDirectChild(cur))
    {
      BrowseLeaf leaf;
      leaf.fullPath = cur.string();
      leaf.relPath = std::filesystem::relative(cur, root, ec).string();
      if (leaf.relPath.empty() || leaf.relPath == ".")
        leaf.relPath = root.filename().string();
      itsBrowseLeaves.push_back(std::move(leaf));
      continue;
    }
    std::vector<std::filesystem::path> kids;
    for (auto it = std::filesystem::directory_iterator(cur, ec);
         it != std::filesystem::directory_iterator();
         it.increment(ec))
    {
      if (ec)
        break;
      if (it->is_directory(ec))
        kids.push_back(it->path());
    }
    // Sort descending so that pop_back gives ascending traversal — keeps
    // the picker's default ordering predictable when the user opens it
    // before typing a query.
    std::sort(kids.rbegin(), kids.rend());
    for (auto& k : kids)
      stack.push_back(std::move(k));
  }
  // Final sort by display path for stable listing.
  std::sort(itsBrowseLeaves.begin(),
            itsBrowseLeaves.end(),
            [](const BrowseLeaf& a, const BrowseLeaf& b) { return a.relPath < b.relPath; });
}

void App::openBrowseLeaf(const std::string& dir)
{
  auto files = collectSortedPngs(dir);
  if (files.empty())
    throw std::runtime_error("No PNG files in " + dir);
  itsSource = std::make_unique<MultiFileSource>(files);
}

bool App::openBrowsePicker(UI& ui)
{
  scanBrowseTree();
  if (itsBrowseLeaves.empty())
  {
    itsLastMessage = "No PNG-containing directories under " + itsOpts.browseRoot;
    return false;
  }

  // Two modes for the same dataset, toggled by Tab:
  //   - search: flat fuzzy filter over relative leaf paths
  //   - browse: a column navigator that walks subdirs one level at a time
  // Both end by calling openBrowseLeaf and returning true.
  enum class Mode
  {
    Search,
    Browse
  };
  Mode mode = Mode::Search;

  // Column navigator state: the path being inspected, expressed relative
  // to itsOpts.browseRoot ("" = root itself). Persists across Tab
  // round-trips so the user doesn't lose their position when switching
  // modes.
  std::filesystem::path here;

  while (true)
  {
    if (mode == Mode::Search)
    {
      std::vector<int> lastIdx;
      auto matcher = [&](const std::string& q)
      {
        std::vector<std::string> hits;
        lastIdx.clear();
        std::string lq = q;
        std::transform(
            lq.begin(), lq.end(), lq.begin(), [](unsigned char c) { return std::tolower(c); });
        for (std::size_t i = 0; i < itsBrowseLeaves.size(); ++i)
        {
          const auto& path = itsBrowseLeaves[i].relPath;
          if (lq.empty())
          {
            hits.push_back(path);
            lastIdx.push_back(static_cast<int>(i));
            continue;
          }
          std::string lower = path;
          std::transform(lower.begin(),
                         lower.end(),
                         lower.begin(),
                         [](unsigned char c) { return std::tolower(c); });
          if (lower.find(lq) != std::string::npos)
          {
            hits.push_back(path);
            lastIdx.push_back(static_cast<int>(i));
          }
        }
        return hits;
      };
      const std::string title = "PNG animations in " + itsOpts.browseRoot;
      const int sel = ui.popupSearch(title, matcher, /*header=*/{}, /*allowTab=*/true);
      if (sel == UI::kPopupSearchTab)
      {
        mode = Mode::Browse;
        continue;
      }
      if (sel < 0 || sel >= static_cast<int>(lastIdx.size()))
        return false;
      openBrowseLeaf(itsBrowseLeaves[lastIdx[sel]].fullPath);
      return true;
    }

    // Browse mode. Show subdirs of `here` plus, if `here` is itself a
    // leaf, a "Load PNGs here" entry at the top. Special items:
    //   index 0: ".." when `here` is non-empty
    //   index 1 (or 0): "[Load PNGs here]" when current dir is a leaf
    const std::filesystem::path absHere = here.empty()
                                              ? std::filesystem::path(itsOpts.browseRoot)
                                              : std::filesystem::path(itsOpts.browseRoot) / here;
    std::vector<std::filesystem::path> subdirs;
    bool isLeaf = hasPngDirectChild(absHere);
    if (!isLeaf)
    {
      std::error_code ec2;
      for (auto it = std::filesystem::directory_iterator(absHere, ec2);
           it != std::filesystem::directory_iterator();
           it.increment(ec2))
      {
        if (ec2)
          break;
        if (it->is_directory(ec2))
          subdirs.push_back(it->path());
      }
      std::sort(subdirs.begin(), subdirs.end());
    }

    std::vector<std::string> items;
    enum class Kind
    {
      Up,
      Load,
      Subdir
    };
    std::vector<Kind> kinds;
    if (!here.empty())
    {
      items.emplace_back("..");
      kinds.push_back(Kind::Up);
    }
    if (isLeaf)
    {
      items.emplace_back("[Load PNGs in this directory]");
      kinds.push_back(Kind::Load);
    }
    for (const auto& s : subdirs)
    {
      items.push_back(s.filename().string() + "/");
      kinds.push_back(Kind::Subdir);
    }
    if (items.empty())
    {
      // Defensive: empty directory that's neither leaf nor has subdirs.
      // Auto-step up and try again.
      if (here.empty())
        return false;
      here = here.parent_path();
      continue;
    }
    const std::string title =
        "Browse: " +
        (here.empty() ? itsOpts.browseRoot : itsOpts.browseRoot + "/" + here.string()) +
        "  (Tab: search)";
    const int sel = ui.popupMenu(title, items, 0, /*allowTab=*/true);
    if (sel == UI::kPopupSearchTab)
    {
      mode = Mode::Search;
      continue;
    }
    if (sel < 0)
      return false;
    switch (kinds[sel])
    {
      case Kind::Up:
        here = here.parent_path();
        break;
      case Kind::Load:
        openBrowseLeaf(absHere.string());
        return true;
      case Kind::Subdir:
      {
        // Recover the picked subdir from the items[] string (strip
        // trailing '/'). Index into subdirs would be cleaner — compute
        // it back through the items[] layout offsets.
        int subIdx = sel;
        if (!here.empty())
          --subIdx;  // skip ".."
        if (isLeaf)
          --subIdx;  // skip "[Load PNGs here]"
        if (subIdx >= 0 && subIdx < static_cast<int>(subdirs.size()))
          here = std::filesystem::relative(subdirs[subIdx], itsOpts.browseRoot);
        break;
      }
    }
  }
}

float App::transform(float v) const
{
  // Same sentinel detection as Palette::lookup; keep them in sync.
  if (v == kFloatMissing || !std::isfinite(v) || std::abs(v) > 1e6F)
    return v;
  const auto& p = activePanel();
  return v * p.valueScale + p.valueOffset;
}

void App::buildIndices()
{
  itsParamIds = itsSource->paramIds();
}

Rgb App::borderColor() const
{
  // Bright saturated green for shapefile outlines — distinct from
  // GSHHS coastlines (black) and political borders (grey-90), and
  // legible on both the flat-grey fill and the rainbow palette.
  return Rgb{0, 220, 0};
}

std::optional<Palette> App::loadPaletteByName(const std::string& name) const
{
  if (name.empty())
    return std::nullopt;
  std::vector<std::filesystem::path> candidates{
      std::filesystem::path(itsOpts.paletteDir) / (name + ".json"),
  };
  try
  {
    const std::filesystem::path exeDir = boost::dll::program_location().parent_path().string();
    candidates.push_back(exeDir / "palettes" / (name + ".json"));
    candidates.push_back(exeDir / ".." / "share" / "smartmet" / "qdless" / "palettes" /
                         (name + ".json"));
  }
  catch (const std::exception&)
  {
  }
  if (const char* home = std::getenv("HOME"))
    candidates.push_back(std::filesystem::path(home) / ".config" / "qdless" / "palettes" /
                         (name + ".json"));
  for (const auto& path : candidates)
  {
    try
    {
      return Palette::loadFromFile(path.string());
    }
    catch (const std::exception&)
    {
    }
  }
  return std::nullopt;
}

void App::loadPalette()
{
  Panel& panel = activePanel();
  // Shapefile shortcut: the source recommends a flat-fill or rainbow
  // palette directly. The qdless.conf parameter→palette table doesn't
  // apply (the "parameter" is a feature ID, not a meteorological
  // quantity) and the unit-guess machinery would just no-op.
  if (auto* shp = dynamic_cast<const ShapeSource*>(itsSource.get()); shp != nullptr)
  {
    panel.valueScale = 1.0F;
    panel.valueOffset = 0.0F;
    // Mode 0 = flat fill (the source's recommendation honours any
    // --color flag). Mode 1 = rainbowPalette() — one band per burn
    // id that actually appears in the rasterised grid (so the [G]
    // legend doesn't list invisible sub-pixel polygons), each band
    // labelled by the feature's NAME / NIMI / first text field.
    // Cycled by [R].
    if (itsShapePaletteMode == 1)
      panel.palette = shp->rainbowPalette();
    else
      panel.palette = shp->recommendedPalette();
    return;
  }
  const int id = itsSource->currentParamId();
  const std::string shortName = itsSource->paramShortName(id);
  const std::string longName = itsSource->paramLongName(id);
  const std::string units = itsSource->paramUnits(id);

  // Auto-detect units. Result has a value transform (scale/offset) to bring
  // the data into the palette's canonical unit system, plus a palette name
  // suggestion (used only as a last-resort fallback).
  const UnitGuess guess = guessFromUnits(shortName, longName, units);
  panel.valueScale = guess.scale;
  panel.valueOffset = guess.offset;
  if (guess.scale != 1.0F || guess.offset != 0.0F)
  {
    if (guess.scale == 1.0F)
      itsLastMessage = fmt::format("Auto-shift: {} + {:g}", units, guess.offset);
    else
      itsLastMessage = fmt::format("Auto-shift: {} × {:g}", units, guess.scale);
  }

  std::string paletteName = itsOpts.paletteOverride;
  if (paletteName.empty())
    paletteName = paletteForParam(itsOpts.configFile, shortName);
  if (paletteName.empty())
    paletteName = paletteForParam(itsOpts.configFile, longName);
  if (paletteName.empty())
    paletteName = guess.palette;

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
      const std::filesystem::path exeDir = boost::dll::program_location().parent_path().string();
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
        panel.palette = Palette::loadFromFile(path.string());
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
      if (v == kFloatMissing || !std::isfinite(v) || std::abs(v) > 1e6F)
        continue;
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
  }
  panel.palette = Palette::builtinRamp(lo, hi);
}

void App::loadCoastlines(int subPixelsW, int subPixelsH)
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

  // Startup path passes 0 because the UI hasn't laid out yet; fall back
  // to the raw terminal size. Use Braille's sub-pixel multipliers (2×
  // horizontal, 4× vertical), which is the finest grid the coastline
  // can actually be drawn at — picking against the finer grid ensures
  // the chosen file resolves smoothly even when Braille is active.
  if (subPixelsW <= 0 || subPixelsH <= 0)
  {
    const auto ts = terminalSize();
    subPixelsW = std::max(1, ts.cols) * 2;
    subPixelsH = std::max(1, ts.rows) * 4;
  }
  const float lonSpan = static_cast<float>(maxLon - minLon);
  const float latSpan = static_cast<float>(maxLat - minLat);
  const float dppLon = lonSpan / static_cast<float>(std::max(1, subPixelsW));
  const float dppLat = latSpan / static_cast<float>(std::max(1, subPixelsH));
  // Take the finer (smaller) axis so the polyline reads smooth in both
  // directions; clamp to a tiny positive to keep degenerate viewports
  // out of the densest branch.
  const float degPerPix = std::max(1e-6F, std::min(dppLon, dppLat));

  if (itsCoastlineStyle != LineStyle::None)
  {
    auto path = Coastline::pickFile(itsOpts.coastlineDir, "GSHHS", degPerPix);
    if (!path.empty() && path != itsCoastlinePath)
    {
      itsCoastlines = Coastline::read(
          path, itsOpts.minLakeAreaKm2, itsOpts.minLakeRoundness, itsOpts.minIslandAreaKm2);
      itsCoastlinePath = path;
    }
  }
  if (itsBorderStyle != LineStyle::None)
  {
    auto path = Coastline::pickFile(itsOpts.coastlineDir, "border", degPerPix);
    if (!path.empty() && path != itsBorderPath)
    {
      itsBorders = Coastline::read(path);
      itsBorderPath = path;
    }
  }
}

std::vector<Rgb> App::sampleSlice(int subWidth, int subHeight, float& dataMin, float& dataMax) const
{
  std::vector<Rgb> out(static_cast<std::size_t>(subWidth) * subHeight, Palette::missingColor());
  dataMin = std::numeric_limits<float>::infinity();
  dataMax = -std::numeric_limits<float>::infinity();

  const float spanU = itsViewport.uMax - itsViewport.uMin;
  const float spanV = itsViewport.vMax - itsViewport.vMin;

  // Hint the source about the viewport — vector sources (ShapeSource)
  // use this to refine their rasterisation when the user zooms in
  // past the base resolution. Compute the lat/lon bbox by sampling
  // the four corners through uvToLatLon; non-shapefile sources
  // ignore the hint via the no-op default.
  {
    LatLonBox vbox;
    vbox.minLat = std::numeric_limits<double>::infinity();
    vbox.maxLat = -std::numeric_limits<double>::infinity();
    vbox.minLon = std::numeric_limits<double>::infinity();
    vbox.maxLon = -std::numeric_limits<double>::infinity();
    auto sample = [&](double u, double v)
    {
      double lat = 0;
      double lon = 0;
      itsSource->uvToLatLon(u, v, lat, lon);
      if (std::isfinite(lat) && std::isfinite(lon))
      {
        vbox.minLat = std::min(vbox.minLat, lat);
        vbox.maxLat = std::max(vbox.maxLat, lat);
        vbox.minLon = std::min(vbox.minLon, lon);
        vbox.maxLon = std::max(vbox.maxLon, lon);
      }
    };
    sample(itsViewport.uMin, itsViewport.vMin);
    sample(itsViewport.uMax, itsViewport.vMin);
    sample(itsViewport.uMin, itsViewport.vMax);
    sample(itsViewport.uMax, itsViewport.vMax);
    if (std::isfinite(vbox.minLat))
      itsSource->prepareViewport(vbox, subWidth, subHeight);
  }

  // Image-mode short-circuit: pixels go straight from the source to the
  // sub-cell buffer with no value/palette layer. Min/max are left at their
  // sentinel infinities so the legend bar (which the App's drawer also
  // suppresses in image mode) doesn't display "[inf, -inf]".
  if (itsSource->isImage())
  {
    for (int sy = 0; sy < subHeight; ++sy)
    {
      const float vp = itsViewport.vMin + (static_cast<float>(sy) + 0.5F) / subHeight * spanV;
      for (int sx = 0; sx < subWidth; ++sx)
      {
        const float up = itsViewport.uMin + (static_cast<float>(sx) + 0.5F) / subWidth * spanU;
        out[static_cast<std::size_t>(sy) * subWidth + sx] = itsSource->pixelAtUV(up, vp);
      }
    }
    return out;
  }

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
      out[static_cast<std::size_t>(sy) * subWidth + sx] = activePanel().palette.lookup(val);
    }
  }
  return out;
}

std::vector<App::ViewportStats> App::ensureViewportStats() const
{
  // Cache hit: same param, level, and viewport as last computation.
  // Compare viewport components with a small epsilon — an interactive
  // pan/zoom always produces a numerically distinct viewport, but
  // exact-equality is fragile against floating-point round-trips.
  const int paramId = itsSource->currentParamId();
  const std::size_t levelIdx = itsSource->currentLevelIndex();
  constexpr float kEps = 1e-6F;
  if (itsStatsCacheValid && itsStatsCacheParam == paramId && itsStatsCacheLevel == levelIdx &&
      std::fabs(itsStatsCacheViewport.uMin - itsViewport.uMin) < kEps &&
      std::fabs(itsStatsCacheViewport.uMax - itsViewport.uMax) < kEps &&
      std::fabs(itsStatsCacheViewport.vMin - itsViewport.vMin) < kEps &&
      std::fabs(itsStatsCacheViewport.vMax - itsViewport.vMax) < kEps)
    return itsStatsCacheSeries;

  // Sample at a fixed resolution that's coarser than the screen but
  // dense enough for stable min / max estimation. 256x128 is ~33k
  // samples per time step, around 1M total for a 24-step file —
  // well under a second on a modern CPU.
  constexpr int kSampleW = 256;
  constexpr int kSampleH = 128;
  const float spanU = itsViewport.uMax - itsViewport.uMin;
  const float spanV = itsViewport.vMax - itsViewport.vMin;

  const std::size_t nt = itsSource->timeCount();
  std::vector<ViewportStats> out(nt);
  if (nt == 0 || spanU <= 0 || spanV <= 0)
  {
    itsStatsCacheParam = paramId;
    itsStatsCacheLevel = levelIdx;
    itsStatsCacheViewport = itsViewport;
    itsStatsCacheSeries = out;
    itsStatsCacheValid = true;
    return itsStatsCacheSeries;
  }

  // Save and restore the source's time index so we don't disturb the
  // outer UI state (timeline, animation, etc).
  auto* mut = const_cast<DataSource*>(itsSource.get());
  const std::size_t savedTime = itsSource->currentTimeIndex();
  for (std::size_t t = 0; t < nt; ++t)
  {
    mut->selectTimeIndex(t);
    double sum = 0;
    int count = 0;
    float lo = std::numeric_limits<float>::infinity();
    float hi = -std::numeric_limits<float>::infinity();
    for (int sy = 0; sy < kSampleH; ++sy)
    {
      const float vp = itsViewport.vMin + (static_cast<float>(sy) + 0.5F) / kSampleH * spanV;
      for (int sx = 0; sx < kSampleW; ++sx)
      {
        const float up = itsViewport.uMin + (static_cast<float>(sx) + 0.5F) / kSampleW * spanU;
        double lat = 0;
        double lon = 0;
        itsSource->uvToLatLon(up, vp, lat, lon);
        const float val = transform(itsSource->interpolatedValue(lat, lon));
        if (val == kFloatMissing || !std::isfinite(val) || std::fabs(val) > 1e6F)
          continue;
        sum += val;
        ++count;
        lo = std::min(lo, val);
        hi = std::max(hi, val);
      }
    }
    ViewportStats s;
    if (count > 0)
    {
      s.min = lo;
      s.max = hi;
      s.mean = static_cast<float>(sum / count);
      s.valid = true;
    }
    out[t] = s;
  }
  mut->selectTimeIndex(savedTime);

  itsStatsCacheParam = paramId;
  itsStatsCacheLevel = levelIdx;
  itsStatsCacheViewport = itsViewport;
  itsStatsCacheSeries = std::move(out);
  itsStatsCacheValid = true;
  return itsStatsCacheSeries;
}

void App::beginCrossSection(int x1, int y1, int x2, int y2, UI& ui)
{
  double lat1 = 0;
  double lon1 = 0;
  double lat2 = 0;
  double lon2 = 0;
  if (!cellToLatLon(ui, x1, y1, lat1, lon1) || !cellToLatLon(ui, x2, y2, lat2, lon2))
    return;

  if (itsSource->levelCount() < 2)
  {
    itsLastMessage = "Cross-section needs >= 2 levels in the file";
    return;
  }

  itsCrossLat1 = lat1;
  itsCrossLon1 = lon1;
  itsCrossLat2 = lat2;
  itsCrossLon2 = lon2;
  itsCrossActive = true;
}

void App::drawCrossSection(UI& ui)
{
  if (!itsCrossActive)
    return;

  const double lat1 = itsCrossLat1;
  const double lon1 = itsCrossLon1;
  const double lat2 = itsCrossLat2;
  const double lon2 = itsCrossLon2;

  const bool heightMode = itsSource->hasNativeHeight() && itsCrossHeightAxis;

  // Haversine distance for the X-axis scale.
  auto haversineKm = [](double la1, double lo1, double la2, double lo2)
  {
    const double r1 = la1 * M_PI / 180.0;
    const double r2 = la2 * M_PI / 180.0;
    const double dla = (la2 - la1) * M_PI / 180.0;
    const double dlo = (lo2 - lo1) * M_PI / 180.0;
    const double a = std::sin(dla / 2) * std::sin(dla / 2) +
                     std::cos(r1) * std::cos(r2) * std::sin(dlo / 2) * std::sin(dlo / 2);
    return 2.0 * 6371.0 * std::atan2(std::sqrt(a), std::sqrt(1 - a));
  };
  const double totalKm = haversineKm(lat1, lon1, lat2, lon2);

  // Hovmöller mode disables heightMode (mutually exclusive).
  const bool hovmollerMode = itsCrossTimeAxis && itsSource->timeCount() > 1;
  const bool effectiveHeightMode = !hovmollerMode && heightMode;

  // Per-row labels. In level mode the rows correspond to source levels
  // (sorted into draw order); in height mode they're heights in km
  // spanning the source's heightRangeKm(); in Hovmöller mode they're
  // time stamps spanning the file's time series.
  std::vector<std::string> rowLabels;
  std::vector<int> levelOrder;     // populated only in level mode
  std::vector<int> hovmollerTimes;  // populated only in Hovmöller mode

  double heightLoKm = 0;
  double heightHiKm = 0;
  int chartH = 0;

  std::size_t savedLevel = itsSource->currentLevelIndex();
  std::size_t savedTime = itsSource->currentTimeIndex();

  if (hovmollerMode)
  {
    const int nTimes = static_cast<int>(itsSource->timeCount());
    // One chart cell row per time step, capped so the popup still fits a
    // standard terminal. If there are more times than rows, sub-sample
    // evenly so the full series still appears.
    const int maxRows = std::max(8, LINES - 12);
    chartH = std::min(nTimes, maxRows);
    hovmollerTimes.resize(chartH);
    rowLabels.resize(chartH);
    for (int cy = 0; cy < chartH; ++cy)
    {
      // Oldest at top, newest at bottom (standard Hovmöller convention).
      const int ti = (chartH == 1) ? 0 : (cy * (nTimes - 1)) / (chartH - 1);
      hovmollerTimes[cy] = ti;
      itsSource->selectTimeIndex(static_cast<std::size_t>(ti));
      const NFmiMetTime t = itsSource->currentValidTime();
      rowLabels[cy] = fmt::format("{:02}-{:02} {:02}:{:02}",
                                  static_cast<int>(t.GetMonth()),
                                  static_cast<int>(t.GetDay()),
                                  static_cast<int>(t.GetHour()),
                                  static_cast<int>(t.GetMin()));
    }
    itsSource->selectTimeIndex(savedTime);
  }
  else if (effectiveHeightMode)
  {
    auto range = itsSource->heightRangeKm();
    heightLoKm = range.first;
    heightHiKm = range.second;
    // One chart row per 1 km when the span is small, else cap at 16 so
    // the popup still fits a standard 24-row terminal.
    const double span = heightHiKm - heightLoKm;
    chartH = std::clamp(static_cast<int>(std::round(span)), 6, 16);
    rowLabels.reserve(chartH);
    for (int cy = 0; cy < chartH; ++cy)
    {
      // Row cy spans [heightHiKm − cy·step, heightHiKm − (cy+1)·step].
      // Label with the row's top edge — gives clean "12, 11, …, 1, 0".
      const double top_km = heightHiKm - (heightHiKm - heightLoKm) * static_cast<double>(cy) /
                                             static_cast<double>(chartH);
      rowLabels.push_back(fmt::format("{:g} km", top_km));
    }
  }
  else
  {
    const int nLevels = static_cast<int>(itsSource->levelCount());
    if (nLevels < 2)
      return;
    chartH = nLevels;
    std::vector<float> levelValues(nLevels);
    rowLabels.assign(nLevels, std::string{});
    for (int i = 0; i < nLevels; ++i)
    {
      levelValues[i] = itsSource->levelValueAt(i);
      rowLabels[i] = itsSource->levelLabel(static_cast<std::size_t>(i));
    }
    levelOrder.resize(nLevels);
    std::iota(levelOrder.begin(), levelOrder.end(), 0);
    // Pressure-style: smaller value = higher altitude (smallest at top).
    // Height-style on non-RHI sources: largest at top so ground sits low.
    const bool ascend = itsSource->levelsAscendWithValue();
    std::sort(
        levelOrder.begin(),
        levelOrder.end(),
        [&](int a, int b)
        { return ascend ? levelValues[a] > levelValues[b] : levelValues[a] < levelValues[b]; });
    // Reorder labels to match draw order.
    std::vector<std::string> tmp(nLevels);
    for (int i = 0; i < nLevels; ++i)
      tmp[i] = rowLabels[levelOrder[i]];
    rowLabels = std::move(tmp);
  }

  // Layout.
  const int desiredW = std::min(80, COLS - 16);
  const int chartW = std::max(20, desiredW);
  const int subRows = subRowsForStyle(itsCornerStyle);
  const int subW = chartW * 2;
  const int subH = chartH * subRows;

  // Compute label width for row labels.
  int labelW = 0;
  for (const auto& s : rowLabels)
    labelW = std::max(labelW, static_cast<int>(s.size()));
  labelW = std::max(labelW, 4);

  // Sample.
  std::vector<Rgb> pixels(static_cast<std::size_t>(subW) * subH);
  if (hovmollerMode)
  {
    // One sweep per chart row at a specific time index. The current level
    // stays selected — Hovmöller is distance × time at a fixed level.
    for (int cy = 0; cy < chartH; ++cy)
    {
      itsSource->selectTimeIndex(static_cast<std::size_t>(hovmollerTimes[cy]));
      for (int sx = 0; sx < subW; ++sx)
      {
        const double frac = (static_cast<double>(sx) + 0.5) / subW;
        const double lat = lat1 + frac * (lat2 - lat1);
        const double lon = lon1 + frac * (lon2 - lon1);
        const float val = transform(itsSource->interpolatedValue(lat, lon));
        const Rgb c = activePanel().palette.lookup(val);
        for (int sr = 0; sr < subRows; ++sr)
          pixels[static_cast<std::size_t>(cy * subRows + sr) * subW + sx] = c;
      }
    }
    itsSource->selectTimeIndex(savedTime);
  }
  else if (effectiveHeightMode)
  {
    // Per-pixel sampling: each pixel (sy, sx) has its own (h, lat, lon).
    // The top sub-row maps to heightHiKm, the bottom to heightLoKm.
    for (int sy = 0; sy < subH; ++sy)
    {
      const double v = static_cast<double>(sy) + 0.5;
      const double h_km = heightHiKm - (heightHiKm - heightLoKm) * v / static_cast<double>(subH);
      for (int sx = 0; sx < subW; ++sx)
      {
        const double frac = (static_cast<double>(sx) + 0.5) / subW;
        const double lat = lat1 + frac * (lat2 - lat1);
        const double lon = lon1 + frac * (lon2 - lon1);
        const float val = transform(itsSource->interpolatedValueAtHeight(lat, lon, h_km));
        pixels[static_cast<std::size_t>(sy) * subW + sx] = activePanel().palette.lookup(val);
      }
    }
  }
  else
  {
    // One sweep per chart row, sampled at the line position only.
    const int nLevels = chartH;
    for (int li = 0; li < nLevels; ++li)
    {
      const int srcLevel = levelOrder[li];
      itsSource->selectLevelIndex(static_cast<unsigned long>(srcLevel));
      for (int sx = 0; sx < subW; ++sx)
      {
        const double frac = (static_cast<double>(sx) + 0.5) / subW;
        const double lat = lat1 + frac * (lat2 - lat1);
        const double lon = lon1 + frac * (lon2 - lon1);
        const float val = transform(itsSource->interpolatedValue(lat, lon));
        const Rgb c = activePanel().palette.lookup(val);
        for (int sr = 0; sr < subRows; ++sr)
          pixels[static_cast<std::size_t>(li * subRows + sr) * subW + sx] = c;
      }
    }
    itsSource->selectLevelIndex(savedLevel);
  }

  // Build raw-ANSI popup.
  // Width: border + label + " ┤ " + chart + border = 2 + labelW + 3 + chartW
  const int width = std::min(COLS - 4, labelW + chartW + 6);
  // Height: border + title + chart + axis + endpoints + footer + border
  const int height = chartH + 6;
  // Dock the popup to the half of the map that has less of the cross-section
  // line in it, so the on-map line + endpoint markers stay visible. The
  // mouse-tracked dot ([[track-cross-hover-dot]]) lives on the line, so this
  // matters: a centred popup would cover most of the line.
  const auto& l = ui.layout();
  double midLat = (lat1 + lat2) / 2;
  double midLon = (lon1 + lon2) / 2;
  double u01 = 0;
  double v01 = 0;
  {
    double uMid = 0;
    double vMid = 0;
    itsSource->latLonToUV(midLat, midLon, uMid, vMid);
    const float spanU = itsViewport.uMax - itsViewport.uMin;
    const float spanV = itsViewport.vMax - itsViewport.vMin;
    u01 = (spanU > 0) ? (uMid - itsViewport.uMin) / spanU : 0.5;
    v01 = (spanV > 0) ? (vMid - itsViewport.vMin) / spanV : 0.5;
  }
  // Line lives in the bottom half of the map → put the popup at the top of
  // the screen. Otherwise dock to bottom (just above the timeline).
  const int top =
      (v01 > 0.5) ? std::max(0, l.map.row) : std::max(0, l.map.row + l.map.height - height);
  // Horizontal: shift away from the line's horizontal centre when possible
  // so a roughly N–S line isn't covered. Falls back to centred when the map
  // is too narrow to offer a useful side.
  int left = std::max(0, (COLS - width) / 2);
  if (width + 4 < COLS)
  {
    if (u01 < 0.5)
      left = std::max(0, COLS - width - 2);
    else
      left = 2;
  }
  const int interiorW = width - 2;

  NFmiEnumConverter conv;
  std::string title =
      (hovmollerMode ? "Hovmöller: " : "Cross-section: ") +
      std::string(itsSource->paramShortName(itsSource->currentParamId()));

  std::ostringstream os;

  auto pos = [&](int row, int col = 0)
  { os << "\x1b[" << (top + row + 1) << ';' << (left + col + 1) << 'H'; };

  // Top border with title.
  pos(0);
  os << "\x1b[0m\x1b[48;5;0m\x1b[38;5;14m"
     << "\xe2\x94\x8c\xe2\x94\x80[\x1b[38;5;15m" << title << "\x1b[38;5;14m]";
  // "Hovmöller" has one 2-byte char (ö) that occupies a single cell, so
  // byte count overstates cell count by 1 in Hovmöller mode.
  int titleConsumed = 4 + static_cast<int>(title.size()) - (hovmollerMode ? 1 : 0);
  for (int i = 0; i < width - titleConsumed - 1; ++i)
    os << "\xe2\x94\x80";
  os << "\xe2\x94\x90\x1b[0m";

  // Chart rows: label │ <data row>.
  // Render each chart row as one cell-row of quadrant blocks via Renderer.
  // We do row-by-row to interleave with labels.
  for (int cy = 0; cy < chartH; ++cy)
  {
    pos(1 + cy);
    os << "\x1b[0m\x1b[48;5;0m\x1b[38;5;14m\xe2\x94\x82\x1b[48;5;0m\x1b[38;5;15m ";
    // Label.
    os << fmt::format("{:>{}}", rowLabels[cy], labelW) << " \xe2\x94\xa4";

    // Render this row's chart cells using a tiny Renderer call.
    // Build the slice: subRows sub-rows of pixels for this level.
    std::vector<Rgb> rowPixels(static_cast<std::size_t>(subW) * subRows);
    for (int sr = 0; sr < subRows; ++sr)
      for (int sx = 0; sx < subW; ++sx)
        rowPixels[static_cast<std::size_t>(sr) * subW + sx] =
            pixels[static_cast<std::size_t>(cy * subRows + sr) * subW + sx];
    // Render at the chart column origin.
    itsRenderer.render(os, rowPixels, subW, subRows, top + 1 + cy, left + 1 + labelW + 2 + 1);

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
      fmt::format(" {:.2f}°N {:.2f}°E  ->  {:.2f}°N {:.2f}°E   total {:.1f} km",
                  lat1,
                  lon1,
                  lat2,
                  lon2,
                  totalKm);
  os << "\x1b[0m\x1b[48;5;0m\x1b[38;5;14m\xe2\x94\x82\x1b[48;5;0m\x1b[38;5;15m" << endpts;
  padSpaces(os, interiorW - static_cast<int>(endpts.size()));
  os << "\x1b[0m\x1b[48;5;0m\x1b[38;5;14m\xe2\x94\x82\x1b[0m";

  // Footer. Mention the [Y] toggle only when the source actually offers
  // both axes — for pressure/hybrid data it's a no-op. 'H' toggles
  // Hovmöller when the source has more than one time step.
  pos(3 + chartH);
  const bool offerH = itsSource->timeCount() > 1;
  std::string footerStr;
  if (hovmollerMode)
    footerStr = std::string(" 'x' close, 'H' back to ") +
                (heightMode ? "height" : "levels") +
                std::string(", \xe2\x86\x90/\xe2\x86\x92 step time");
  else if (itsSource->hasNativeHeight())
    footerStr = std::string(" 'x' close, 'y' Y-axis: ") +
                (effectiveHeightMode ? "km \xe2\x86\x92 angle" : "angle \xe2\x86\x92 km") +
                (offerH ? std::string(", 'H' Hovmöller") : std::string()) +
                ", \xe2\x86\x90/\xe2\x86\x92 step time";
  else
    footerStr = std::string(" 'x' close") +
                (offerH ? std::string(", 'H' Hovmöller") : std::string()) +
                ", \xe2\x86\x90/\xe2\x86\x92 step time, Space animate";
  // UTF-8 byte / cell skew. Each ←/→ arrow is 3 bytes / 1 cell (2-byte
  // overhead). Each ö in "Hovmöller" is 2 bytes / 1 cell (1-byte overhead).
  int arrowsInFooter = 2;  // ← and →
  if (!hovmollerMode && itsSource->hasNativeHeight()) ++arrowsInFooter;  // angle → km
  const int ohlerCount = (!hovmollerMode && offerH) ? 1 : 0;
  const int footerCells =
      static_cast<int>(footerStr.size()) - 2 * arrowsInFooter - 1 * ohlerCount;
  os << "\x1b[0m\x1b[48;5;0m\x1b[38;5;14m\xe2\x94\x82\x1b[48;5;0m\x1b[38;5;15m" << footerStr;
  padSpaces(os, interiorW - footerCells);
  os << "\x1b[0m\x1b[48;5;0m\x1b[38;5;14m\xe2\x94\x82\x1b[0m";

  // Bottom border.
  pos(height - 1);
  os << "\x1b[0m\x1b[48;5;0m\x1b[38;5;14m\xe2\x94\x94";
  for (int i = 0; i < width - 2; ++i)
    os << "\xe2\x94\x80";
  os << "\xe2\x94\x98\x1b[0m";

  const std::string s = os.str();
  std::fwrite(s.data(), 1, s.size(), stdout);
  std::fflush(stdout);

  // Cache chart-area rectangle in cell coords so the mouse-motion handler
  // can map (ev.x, ev.y) → fraction along the great-circle. The chart
  // begins one row below the top border, after the label column + " ┤ ".
  itsCrossChartRow = top + 1;
  itsCrossChartCol = left + labelW + 4;
  itsCrossChartW = chartW;
  itsCrossChartH = chartH;
}

bool App::ensureCityIndex() const
{
  if (itsCityIndexAttempted)
    return !itsCityIndex.empty();
  itsCityIndexAttempted = true;

  std::vector<std::filesystem::path> candidates{
      std::filesystem::path("/usr/share/smartmet/qdless/cities1000.tsv"),
  };
  try
  {
    const std::filesystem::path exeDir = boost::dll::program_location().parent_path().string();
    candidates.push_back(exeDir / "data" / "cities1000.tsv");
  }
  catch (const std::exception&)
  {
  }
  if (const char* home = std::getenv("HOME"))
    candidates.push_back(std::filesystem::path(home) / ".config" / "qdless" / "cities1000.tsv");
  for (const auto& path : candidates)
  {
    if (std::filesystem::exists(path) && itsCityIndex.load(path.string()))
      return true;
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
                        (itsViewport.vMin + itsViewport.vMax) * 0.5,
                        centerLat,
                        centerLon);

  // The popup calls this lambda each keystroke; we keep the latest match
  // list outside so we can resolve the picked index back to a city.
  std::vector<std::size_t> lastMatchIds;
  auto matcher = [&](const std::string& q) -> std::vector<std::string>
  {
    lastMatchIds = itsCityIndex.search(q, 12, centerLat, centerLon);
    std::vector<std::string> rows;
    rows.reserve(lastMatchIds.size());
    for (std::size_t i : lastMatchIds)
    {
      const auto& c = itsCityIndex.at(i);
      rows.push_back(fmt::format(
          "{}, {}  ({:.2f}, {:.2f})  pop {}", c.name, c.country, c.lat, c.lon, c.population));
    }
    return rows;
  };

  const int picked = ui.popupSearch("Place search", matcher);
  if (picked < 0 || picked >= static_cast<int>(lastMatchIds.size()))
    return;
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
  if (spanU <= 0 || spanV <= 0)
  {
    err = "empty viewport";
    return {};
  }

  // Output size: aspect-ratio of the visible rectangle in the source's
  // native space (which preserves the native projection's shape), target
  // ~720 px tall. For unprojected backends this equals the lat/lon aspect.
  constexpr int targetH = 720;
  const double aspect = spanU / spanV;
  const int height = targetH;
  const int width = std::max(100, static_cast<int>(targetH * aspect));

  // Hand-rolled RGB buffer (interleaved RGBRGB…). Writing PNGs needs
  // a library either way; we use GDAL's PNG driver below since GDAL
  // is already a dependency for shapefiles / PostGIS / GeoTIFF.
  // Background = white so "no data" cells leave a clean canvas.
  std::vector<std::uint8_t> rgb(
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3, 255);
  auto setPixel = [&](int x, int y, std::uint8_t r, std::uint8_t g, std::uint8_t b)
  {
    if (x < 0 || x >= width || y < 0 || y >= height)
      return;
    const auto idx = (static_cast<std::size_t>(y) * width + x) * 3;
    rgb[idx] = r;
    rgb[idx + 1] = g;
    rgb[idx + 2] = b;
  };

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
      Rgb c = activePanel().palette.lookup(val);
      if (c.transparent)
        continue;  // leave white for "no data"
      setPixel(px, py, c.r, c.g, c.b);
    }
  }

  // Coastline + border overlay (Bresenham into the RGB buffer).
  auto drawPolylineImg =
      [&](const std::vector<Polyline>& polys, std::uint8_t r, std::uint8_t g, std::uint8_t b)
  {
    if (polys.empty())
      return;
    auto toPixel = [&](float lon, float lat) -> std::pair<int, int>
    {
      double u = 0;
      double v = 0;
      itsSource->latLonToUV(lat, lon, u, v);
      const double u01 = (u - itsViewport.uMin) / spanU;
      const double v01 = (v - itsViewport.vMin) / spanV;
      return {static_cast<int>(u01 * width), static_cast<int>(v01 * height)};
    };
    auto line = [&](int x0, int y0, int x1, int y1)
    {
      int dx = std::abs(x1 - x0);
      int dy = -std::abs(y1 - y0);
      int sx = x0 < x1 ? 1 : -1;
      int sy = y0 < y1 ? 1 : -1;
      int err2 = dx + dy;
      while (true)
      {
        setPixel(x0, y0, r, g, b);
        if (x0 == x1 && y0 == y1)
          break;
        int e2 = 2 * err2;
        if (e2 >= dy)
        {
          err2 += dy;
          x0 += sx;
        }
        if (e2 <= dx)
        {
          err2 += dx;
          y0 += sy;
        }
      }
    };
    for (const auto& pl : polys)
    {
      if (pl.lons.size() < 2)
        continue;
      auto prev = toPixel(pl.lons[0], pl.lats[0]);
      for (std::size_t i = 1; i < pl.lons.size(); ++i)
      {
        auto cur = toPixel(pl.lons[i], pl.lats[i]);
        if (std::abs(cur.first - prev.first) < width && std::abs(cur.second - prev.second) < height)
          line(prev.first, prev.second, cur.first, cur.second);
        prev = cur;
      }
    }
  };
  drawPolylineImg(itsCoastlines, 0, 0, 0);
  drawPolylineImg(itsBorders, 90, 90, 90);

  // Build filename: <basename>_<param>_<YYYYMMDD_HHMM>.png in cwd.
  NFmiEnumConverter conv;
  std::filesystem::path inputPath(itsOpts.filename);
  const std::string base = inputPath.stem().string();
  const std::string param = itsSource->paramShortName(itsSource->currentParamId());
  NFmiMetTime t = itsSource->currentValidTime();
  const std::string filename = fmt::format("{}_{}_{:04}{:02}{:02}_{:02}{:02}.png",
                                           base,
                                           param,
                                           static_cast<int>(t.GetYear()),
                                           static_cast<int>(t.GetMonth()),
                                           static_cast<int>(t.GetDay()),
                                           static_cast<int>(t.GetHour()),
                                           static_cast<int>(t.GetMin()));

  // Encode via GDAL's PNG driver. Build an in-memory MEM dataset
  // with three Byte bands, blit our interleaved RGB buffer into it
  // (one stride trick replaces three per-band RasterIO calls),
  // CreateCopy to PNG. GDAL is already linked for shapefiles /
  // GeoTIFF / PostGIS / raw images so this drops the
  // smartmet-library-imagine dependency entirely.
  GDALAllRegister();
  GDALDriver* memDrv = GetGDALDriverManager()->GetDriverByName("MEM");
  GDALDriver* pngDrv = GetGDALDriverManager()->GetDriverByName("PNG");
  if (memDrv == nullptr || pngDrv == nullptr)
  {
    err = "GDAL MEM/PNG driver unavailable";
    return {};
  }
  GDALDataset* mem = memDrv->Create("", width, height, 3, GDT_Byte, nullptr);
  if (mem == nullptr)
  {
    err = "GDAL MEM Create failed";
    return {};
  }
  // Dataset-level RasterIO with explicit pixel/line/band spacing
  // writes the interleaved RGB buffer in one call. nPixelSpace=3
  // (3 bytes per pixel), nLineSpace=width*3, nBandSpace=1 (band 0
  // is at offset 0, band 1 at offset 1, …).
  const int bandList[3] = {1, 2, 3};
  CPLErr cerr = mem->RasterIO(GF_Write,
                              0,
                              0,
                              width,
                              height,
                              rgb.data(),
                              width,
                              height,
                              GDT_Byte,
                              3,
                              const_cast<int*>(bandList),
                              3,
                              static_cast<GSpacing>(width) * 3,
                              1,
                              nullptr);
  if (cerr != CE_None)
  {
    GDALClose(mem);
    err = "GDAL RasterIO write failed";
    return {};
  }
  try
  {
    GDALDataset* png = pngDrv->CreateCopy(filename.c_str(), mem, FALSE, nullptr, nullptr, nullptr);
    GDALClose(mem);
    if (png == nullptr)
    {
      err = "GDAL PNG CreateCopy failed";
      return {};
    }
    GDALClose(png);
    return filename;
  }
  catch (const std::exception& e)
  {
    GDALClose(mem);
    err = e.what();
    return {};
  }
}

std::vector<std::array<int, 4>> App::traceGraticuleSegments(int bW, int bH) const
{
  std::vector<std::array<int, 4>> out;
  const float spanU = itsViewport.uMax - itsViewport.uMin;
  const float spanV = itsViewport.vMax - itsViewport.vMin;
  if (spanU <= 0 || spanV <= 0 || bW <= 0 || bH <= 0)
    return out;

  // Estimate the visible lat/lon extent by sampling along the viewport
  // border. With projected sources the extent is generally not rectilinear
  // in lat/lon (e.g. polar stereo), so sample densely enough to catch
  // off-axis extrema.
  double minLat = 90;
  double maxLat = -90;
  double minLon = 180;
  double maxLon = -180;
  constexpr int kEdgeSamples = 32;
  auto observe = [&](double u, double v)
  {
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
  if (maxLat <= minLat || maxLon <= minLon)
    return out;

  const double lonStep = niceStep(maxLon - minLon, 6);
  const double latStep = niceStep(maxLat - minLat, 6);

  // Projected sources (e.g. rotated lat/lon) cover a geographic bbox larger
  // than the actual data region — meridian/parallel samples near the bbox
  // edges fall outside the grid. For those points latLonToUV silently falls
  // back to bbox interpolation, producing a (u, v) that doesn't correspond
  // to where this lat/lon is on the projection and drawing spurious line
  // segments that hug the viewport edges. A round-trip distinguishes the
  // two cases: in-grid points hit the same projection in both directions
  // and round-trip to floating-point precision; fallback points diverge.
  auto toSub = [&](double lat, double lon) -> std::optional<std::pair<int, int>>
  {
    double u = 0;
    double v = 0;
    itsSource->latLonToUV(lat, lon, u, v);
    double latBack = 0;
    double lonBack = 0;
    itsSource->uvToLatLon(u, v, latBack, lonBack);
    double dlon = std::fmod(std::abs(lon - lonBack), 360.0);
    if (dlon > 180.0)
      dlon = 360.0 - dlon;
    if (std::abs(lat - latBack) > 0.5 || dlon > 0.5)
      return std::nullopt;
    const double u01 = (u - itsViewport.uMin) / spanU;
    const double v01 = (v - itsViewport.vMin) / spanV;
    return std::pair<int, int>{static_cast<int>(u01 * bW), static_cast<int>(v01 * bH)};
  };

  // Emit segment only when both endpoints are valid AND the jump is small
  // enough to be a real adjacent pair (catches antimeridian wraps that the
  // round-trip can't see).
  auto emit =
      [&](const std::optional<std::pair<int, int>>& a, const std::optional<std::pair<int, int>>& b)
  {
    if (!a || !b)
      return;
    if (std::abs(b->first - a->first) >= bW / 2)
      return;
    if (std::abs(b->second - a->second) >= bH / 2)
      return;
    out.push_back({a->first, a->second, b->first, b->second});
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
      emit(prev, cur);
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
      emit(prev, cur);
      prev = cur;
    }
  }
  return out;
}

void App::overlayGraticule(std::vector<Rgb>& pixels, int subWidth, int subHeight) const
{
  const Rgb gridColor{120, 120, 120};

  auto plot = [&](int x, int y)
  {
    if (x >= 0 && x < subWidth && y >= 0 && y < subHeight)
      pixels[static_cast<std::size_t>(y) * subWidth + x] = gridColor;
  };

  auto drawLine = [&](int x0, int y0, int x1, int y1)
  {
    int dx = std::abs(x1 - x0);
    int dy = -std::abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (true)
    {
      plot(x0, y0);
      if (x0 == x1 && y0 == y1)
        break;
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

  for (const auto& s : traceGraticuleSegments(subWidth, subHeight))
    drawLine(s[0], s[1], s[2], s[3]);
}

void App::appendGraticuleBraille(std::ostringstream& os,
                                 const std::vector<Rgb>& pixels,
                                 int subWidth,
                                 int originRow,
                                 int originCol) const
{
  if (subWidth <= 0)
    return;
  const int subRows = subRowsForStyle(itsCornerStyle);
  const int cellW = subWidth / 2;
  const int subHeight = static_cast<int>(pixels.size()) / std::max(1, subWidth);
  const int cellH = subHeight / subRows;
  if (cellW <= 0 || cellH <= 0)
    return;

  // Higher-resolution sub-cell grid for the line: 2 cols × 4 rows per cell.
  const int bW = cellW * 2;
  const int bH = cellH * 4;
  std::vector<unsigned char> mask(static_cast<std::size_t>(bW) * bH, 0);

  auto plot = [&](int x, int y)
  {
    if (x >= 0 && x < bW && y >= 0 && y < bH)
      mask[static_cast<std::size_t>(y) * bW + x] = 1;
  };

  auto drawLine = [&](int x0, int y0, int x1, int y1)
  {
    int dx = std::abs(x1 - x0);
    int dy = -std::abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (true)
    {
      plot(x0, y0);
      if (x0 == x1 && y0 == y1)
        break;
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

  for (const auto& s : traceGraticuleSegments(bW, bH))
    drawLine(s[0], s[1], s[2], s[3]);

  const Rgb gridColor{120, 120, 120};
  std::string out;
  out.reserve(static_cast<std::size_t>(cellW) * 16);
  for (int cy = 0; cy < cellH; ++cy)
  {
    for (int cx = 0; cx < cellW; ++cx)
    {
      unsigned cellMask = 0;
      for (int sy = 0; sy < 4; ++sy)
      {
        for (int sx = 0; sx < 2; ++sx)
        {
          const int bx = cx * 2 + sx;
          const int by = cy * 4 + sy;
          if (mask[static_cast<std::size_t>(by) * bW + bx] != 0U)
            cellMask |= 1U << brailleBit(sx, sy);
        }
      }
      if (cellMask == 0U)
        continue;
      const Rgb bg = pixels[static_cast<std::size_t>(cy * subRows) * subWidth + (cx * 2)];
      out += "\x1b[";
      out += std::to_string(originRow + cy + 1);
      out += ';';
      out += std::to_string(originCol + cx + 1);
      out += 'H';
      out += itsRenderer.bgEscape(bg);
      out += itsRenderer.fgEscape(gridColor);
      out += brailleGlyph(cellMask);
    }
  }
  if (!out.empty())
  {
    out += "\x1b[0m";
    os << out;
  }
}

bool App::hasWindComponents() const
{
  NFmiEnumConverter conv;
  const int uEnum = conv.ToEnum("WindUMS");
  const int vEnum = conv.ToEnum("WindVMS");
  if (uEnum == kFmiBadParameter || vEnum == kFmiBadParameter)
    return false;
  const auto& ids = itsParamIds;
  return std::find(ids.begin(), ids.end(), uEnum) != ids.end() &&
         std::find(ids.begin(), ids.end(), vEnum) != ids.end();
}

std::string App::buildWindArrows(int cellW, int cellH, int originRow, int originCol)
{
  // Find U and V param IDs in this file. If either is missing, render nothing.
  if (!hasWindComponents())
    return {};
  NFmiEnumConverter conv;
  const int uEnum = conv.ToEnum("WindUMS");
  const int vEnum = conv.ToEnum("WindVMS");

  // Save current param to restore at the end.
  const int savedParam = itsSource->currentParamId();

  const float spanU = itsViewport.uMax - itsViewport.uMin;
  const float spanV = itsViewport.vMax - itsViewport.vMin;
  if (spanU <= 0 || spanV <= 0)
    return {};

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
  struct Sample
  {
    int cx, cy;
    float u, v;
  };
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
    if (!std::isfinite(s.u) || !std::isfinite(s.v))
      continue;
    if (std::abs(s.u) > 1e10F || std::abs(s.v) > 1e10F)
      continue;
    const float speed = std::sqrt(s.u * s.u + s.v * s.v);
    if (speed < 0.5F)
      continue;  // skip near-calm cells

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

void App::overlayPolylines(std::vector<Rgb>& pixels,
                           int subWidth,
                           int subHeight,
                           const std::vector<Polyline>& polylines,
                           Rgb color) const
{
  if (polylines.empty())
    return;
  const float spanU = itsViewport.uMax - itsViewport.uMin;
  const float spanV = itsViewport.vMax - itsViewport.vMin;
  if (spanU <= 0 || spanV <= 0)
    return;

  auto plot = [&](int x, int y)
  {
    if (x >= 0 && x < subWidth && y >= 0 && y < subHeight)
      pixels[static_cast<std::size_t>(y) * subWidth + x] = color;
  };

  auto drawLine = [&](int x0, int y0, int x1, int y1)
  {
    int dx = std::abs(x1 - x0);
    int dy = -std::abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (true)
    {
      plot(x0, y0);
      if (x0 == x1 && y0 == y1)
        break;
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

  auto toSub = [&](float lon, float lat) -> std::pair<int, int>
  {
    double u = 0;
    double v = 0;
    itsSource->latLonToUV(lat, lon, u, v);
    const double u01 = (u - itsViewport.uMin) / spanU;
    const double v01 = (v - itsViewport.vMin) / spanV;
    return {static_cast<int>(u01 * subWidth), static_cast<int>(v01 * subHeight)};
  };

  for (const auto& pl : polylines)
  {
    if (pl.lons.size() < 2)
      continue;
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

void App::buildUtopiaGeo(int subWidth,
                         int subHeight,
                         std::vector<Rgb>& lines,
                         std::vector<char>& swedenMask) const
{
  if (itsCoastlines.empty() && itsBorders.empty())
    return;  // not a geographic view
  const float spanU = itsViewport.uMax - itsViewport.uMin;
  const float spanV = itsViewport.vMax - itsViewport.vMin;
  if (spanU <= 0 || spanV <= 0)
    return;

  // Coastlines + borders rendered on black — what stays once the data fades.
  lines.assign(static_cast<std::size_t>(subWidth) * subHeight, Rgb{0, 0, 0});
  overlayPolylines(lines, subWidth, subHeight, itsCoastlines, Rgb{210, 210, 220});
  overlayPolylines(lines, subWidth, subHeight, itsBorders, Rgb{120, 120, 135});

  // An approximate outline of mainland Sweden (lon, lat), walked as a loop.
  static const std::array<std::pair<float, float>, 16> kSweden = {{{13.4F, 55.35F},
                                                                   {12.0F, 57.7F},
                                                                   {11.2F, 58.9F},
                                                                   {12.2F, 61.0F},
                                                                   {12.6F, 63.0F},
                                                                   {14.1F, 64.5F},
                                                                   {15.4F, 66.3F},
                                                                   {17.9F, 68.4F},
                                                                   {20.3F, 69.05F},
                                                                   {23.65F, 67.95F},
                                                                   {24.15F, 65.83F},
                                                                   {21.5F, 63.3F},
                                                                   {19.0F, 60.9F},
                                                                   {18.3F, 59.4F},
                                                                   {16.6F, 56.5F},
                                                                   {14.7F, 56.0F}}};
  std::array<float, kSweden.size()> px{};
  std::array<float, kSweden.size()> py{};
  for (std::size_t i = 0; i < kSweden.size(); ++i)
  {
    double u = 0;
    double v = 0;
    itsSource->latLonToUV(kSweden[i].second, kSweden[i].first, u, v);
    px[i] = static_cast<float>((u - itsViewport.uMin) / spanU * subWidth);
    py[i] = static_cast<float>((v - itsViewport.vMin) / spanV * subHeight);
  }
  // Point-in-polygon (ray casting) for every sub-pixel.
  swedenMask.assign(static_cast<std::size_t>(subWidth) * subHeight, 0);
  for (int y = 0; y < subHeight; ++y)
  {
    const float fy = y + 0.5F;
    for (int x = 0; x < subWidth; ++x)
    {
      const float fx = x + 0.5F;
      bool inside = false;
      for (std::size_t i = 0, j = kSweden.size() - 1; i < kSweden.size(); j = i++)
        if (((py[i] > fy) != (py[j] > fy)) &&
            (fx < (px[j] - px[i]) * (fy - py[i]) / (py[j] - py[i] + 1e-9F) + px[i]))
          inside = !inside;
      if (inside)
        swedenMask[static_cast<std::size_t>(y) * subWidth + x] = 1;
    }
  }
}

void App::appendPolylineBraille(std::ostringstream& os,
                                const std::vector<Polyline>& polylines,
                                Rgb color,
                                const std::vector<Rgb>& pixels,
                                int subWidth,
                                int originRow,
                                int originCol) const
{
  if (polylines.empty() || subWidth <= 0)
    return;
  const int subRows = subRowsForStyle(itsCornerStyle);
  const int cellW = subWidth / 2;
  const int subHeight = static_cast<int>(pixels.size()) / std::max(1, subWidth);
  const int cellH = subHeight / subRows;
  if (cellW <= 0 || cellH <= 0)
    return;

  // Higher-resolution sub-cell grid for the line: 2 cols × 4 rows per cell.
  const int bW = cellW * 2;
  const int bH = cellH * 4;
  std::vector<unsigned char> mask(static_cast<std::size_t>(bW) * bH, 0);

  const float spanU = itsViewport.uMax - itsViewport.uMin;
  const float spanV = itsViewport.vMax - itsViewport.vMin;
  if (spanU <= 0 || spanV <= 0)
    return;

  auto plot = [&](int x, int y)
  {
    if (x >= 0 && x < bW && y >= 0 && y < bH)
      mask[static_cast<std::size_t>(y) * bW + x] = 1;
  };

  auto drawLine = [&](int x0, int y0, int x1, int y1)
  {
    int dx = std::abs(x1 - x0);
    int dy = -std::abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (true)
    {
      plot(x0, y0);
      if (x0 == x1 && y0 == y1)
        break;
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

  auto toSub = [&](float lon, float lat) -> std::pair<int, int>
  {
    double u = 0;
    double v = 0;
    itsSource->latLonToUV(lat, lon, u, v);
    const double u01 = (u - itsViewport.uMin) / spanU;
    const double v01 = (v - itsViewport.vMin) / spanV;
    return {static_cast<int>(u01 * bW), static_cast<int>(v01 * bH)};
  };

  for (const auto& pl : polylines)
  {
    if (pl.lons.size() < 2)
      continue;
    auto prev = toSub(pl.lons[0], pl.lats[0]);
    for (std::size_t i = 1; i < pl.lons.size(); ++i)
    {
      auto cur = toSub(pl.lons[i], pl.lats[i]);
      if (std::abs(cur.first - prev.first) < bW && std::abs(cur.second - prev.second) < bH)
        drawLine(prev.first, prev.second, cur.first, cur.second);
      prev = cur;
    }
  }

  // For each cell with any dots, emit a positioned braille glyph. Background
  // is sampled from the top-left sub-pixel of the cell so the underlying
  // data colour shows through behind the line dots.
  std::string out;
  out.reserve(static_cast<std::size_t>(cellW) * 16);
  for (int cy = 0; cy < cellH; ++cy)
  {
    for (int cx = 0; cx < cellW; ++cx)
    {
      unsigned cellMask = 0;
      for (int sy = 0; sy < 4; ++sy)
      {
        for (int sx = 0; sx < 2; ++sx)
        {
          const int bx = cx * 2 + sx;
          const int by = cy * 4 + sy;
          if (mask[static_cast<std::size_t>(by) * bW + bx] != 0U)
            cellMask |= 1U << brailleBit(sx, sy);
        }
      }
      if (cellMask == 0U)
        continue;
      const Rgb bg = pixels[static_cast<std::size_t>(cy * subRows) * subWidth + (cx * 2)];
      out += "\x1b[";
      out += std::to_string(originRow + cy + 1);
      out += ';';
      out += std::to_string(originCol + cx + 1);
      out += 'H';
      out += itsRenderer.bgEscape(bg);
      out += itsRenderer.fgEscape(color);
      out += brailleGlyph(cellMask);
    }
  }
  if (!out.empty())
  {
    out += "\x1b[0m";
    os << out;
  }
}

void App::overlayMarker(std::vector<Rgb>& pixels, int subWidth, int subHeight) const
{
  if (!itsMarker.has_value() || subWidth <= 0 || subHeight <= 0)
    return;
  const float spanU = itsViewport.uMax - itsViewport.uMin;
  const float spanV = itsViewport.vMax - itsViewport.vMin;
  if (spanU <= 0 || spanV <= 0)
    return;
  double u = 0;
  double v = 0;
  itsSource->latLonToUV(itsMarker->first, itsMarker->second, u, v);
  const double u01 = (u - itsViewport.uMin) / spanU;
  const double v01 = (v - itsViewport.vMin) / spanV;
  const int cx = static_cast<int>(u01 * subWidth);
  const int cy = static_cast<int>(v01 * subHeight);

  const Rgb fg{255, 40, 40};
  const Rgb bg{255, 255, 255};
  auto plot = [&](int x, int y, Rgb c)
  {
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
    if ((c & 0xC0) != 0x80)
      ++n;
  return n;
}

// Discrete population-count steps the user can cycle through with PageDown
// (denser) and PageUp (sparser). Sorted ascending.
constexpr std::array<int, 7> kCityNSteps = {{5, 10, 25, 50, 100, 250, 500}};

// Pick visible cities (lat/lon inside viewport), sorted by population
// descending, capped at maxN.
std::vector<std::size_t> visibleCities(const CityIndex& idx,
                                       const DataSource& src,
                                       const Viewport& vp,
                                       int maxN)
{
  struct Hit
  {
    std::size_t i;
    int pop;
  };
  std::vector<Hit> hits;
  for (std::size_t i = 0; i < idx.size(); ++i)
  {
    const auto& c = idx.at(i);
    double u = 0;
    double v = 0;
    src.latLonToUV(c.lat, c.lon, u, v);
    if (u < vp.uMin || u > vp.uMax || v < vp.vMin || v > vp.vMax)
      continue;
    hits.push_back({i, c.population});
  }
  std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.pop > b.pop; });
  if (static_cast<int>(hits.size()) > maxN)
    hits.resize(maxN);
  std::vector<std::size_t> out;
  out.reserve(hits.size());
  for (const auto& h : hits)
    out.push_back(h.i);
  return out;
}
}  // namespace

void App::overlayCities(std::vector<Rgb>& pixels, int subWidth, int subHeight) const
{
  if (!itsShowCities || subWidth <= 0 || subHeight <= 0)
    return;
  if (!ensureCityIndex())
    return;
  const float spanU = itsViewport.uMax - itsViewport.uMin;
  const float spanV = itsViewport.vMax - itsViewport.vMin;
  if (spanU <= 0 || spanV <= 0)
    return;

  const auto picks = visibleCities(itsCityIndex, *itsSource, itsViewport, itsCityOverlayN);
  const Rgb dot{255, 255, 255};
  const Rgb halo{30, 30, 30};
  auto plot = [&](int x, int y, Rgb c)
  {
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
      for (int dx = -1; dx <= 1; ++dx)
        plot(cx + dx, cy + dy, halo);
    plot(cx, cy, dot);
  }
}

void App::overlayCrossSection(std::vector<Rgb>& pixels, int subWidth, int subHeight) const
{
  if (!itsCrossActive || subWidth <= 0 || subHeight <= 0)
    return;
  const float spanU = itsViewport.uMax - itsViewport.uMin;
  const float spanV = itsViewport.vMax - itsViewport.vMin;
  if (spanU <= 0 || spanV <= 0)
    return;

  auto toSub = [&](double lat, double lon) -> std::pair<int, int>
  {
    double u = 0;
    double v = 0;
    itsSource->latLonToUV(lat, lon, u, v);
    const double u01 = (u - itsViewport.uMin) / spanU;
    const double v01 = (v - itsViewport.vMin) / spanV;
    return {static_cast<int>(u01 * subWidth), static_cast<int>(v01 * subHeight)};
  };

  auto plot = [&](int x, int y, Rgb c)
  {
    if (x >= 0 && x < subWidth && y >= 0 && y < subHeight)
      pixels[static_cast<std::size_t>(y) * subWidth + x] = c;
  };

  auto drawSeg = [&](int x0, int y0, int x1, int y1, Rgb halo, Rgb line)
  {
    int dx = std::abs(x1 - x0);
    int dy = -std::abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (true)
    {
      for (int hy = -1; hy <= 1; ++hy)
        for (int hx = -1; hx <= 1; ++hx)
          if (hx != 0 || hy != 0)
            plot(x0 + hx, y0 + hy, halo);
      plot(x0, y0, line);
      if (x0 == x1 && y0 == y1)
        break;
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

  // Sample the great-circle in lat/lon-linear steps to match how the
  // chart itself samples the section (QdlessApp.cpp:1238). Step count is
  // generous enough that even a fully diagonal viewport gets continuous
  // pixels.
  const Rgb halo{255, 255, 255};
  const Rgb line{255, 40, 40};
  const int steps = std::max(subWidth, subHeight) * 2;
  auto prev = toSub(itsCrossLat1, itsCrossLon1);
  for (int i = 1; i <= steps; ++i)
  {
    const double frac = static_cast<double>(i) / steps;
    const double lat = itsCrossLat1 + frac * (itsCrossLat2 - itsCrossLat1);
    const double lon = itsCrossLon1 + frac * (itsCrossLon2 - itsCrossLon1);
    auto cur = toSub(lat, lon);
    // Antimeridian / projection wrap guard: skip absurd jumps.
    if (std::abs(cur.first - prev.first) < subWidth &&
        std::abs(cur.second - prev.second) < subHeight)
      drawSeg(prev.first, prev.second, cur.first, cur.second, halo, line);
    prev = cur;
  }

  // Endpoint markers: small filled discs with a halo, visible against
  // any palette and any line colour.
  auto drawEndpoint = [&](double lat, double lon, Rgb c)
  {
    const auto p = toSub(lat, lon);
    for (int dy = -3; dy <= 3; ++dy)
      for (int dx = -3; dx <= 3; ++dx)
        if (dx * dx + dy * dy <= 9)
          plot(p.first + dx, p.second + dy, halo);
    for (int dy = -2; dy <= 2; ++dy)
      for (int dx = -2; dx <= 2; ++dx)
        if (dx * dx + dy * dy <= 4)
          plot(p.first + dx, p.second + dy, c);
  };
  drawEndpoint(itsCrossLat1, itsCrossLon1, Rgb{40, 40, 255});
  drawEndpoint(itsCrossLat2, itsCrossLon2, Rgb{40, 40, 255});

  // Mouse-tracked hover dot: bigger and brighter so it stands out from
  // the endpoint markers.
  if (itsCrossHoverLatLon.has_value())
  {
    const auto p = toSub(itsCrossHoverLatLon->first, itsCrossHoverLatLon->second);
    for (int dy = -4; dy <= 4; ++dy)
      for (int dx = -4; dx <= 4; ++dx)
        if (dx * dx + dy * dy <= 16)
          plot(p.first + dx, p.second + dy, halo);
    for (int dy = -3; dy <= 3; ++dy)
      for (int dx = -3; dx <= 3; ++dx)
        if (dx * dx + dy * dy <= 9)
          plot(p.first + dx, p.second + dy, Rgb{255, 220, 40});
  }
}

std::string App::buildCityLabels(int cellW, int cellH, int originRow, int originCol)
{
  if (!itsShowCities || cellW <= 0 || cellH <= 0)
    return {};
  if (!ensureCityIndex())
    return {};
  const float spanU = itsViewport.uMax - itsViewport.uMin;
  const float spanV = itsViewport.vMax - itsViewport.vMin;
  if (spanU <= 0 || spanV <= 0)
    return {};

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
    if (cellX < 0 || cellX >= cellW || cellY < 0 || cellY >= cellH)
      continue;

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
      if (rowY < 0 || rowY >= cellH || startX < 0 || startX + w > cellW)
        continue;
      bool collision = false;
      for (int k = 0; k < w; ++k)
        if (occupied[rowY][startX + k])
        {
          collision = true;
          break;
        }
      if (collision)
        continue;
      for (int k = 0; k < w; ++k)
        occupied[rowY][startX + k] = true;
      // White on a dim background so the label is legible against any palette.
      os << "\x1b[" << (originRow + rowY + 1) << ';' << (originCol + startX + 1) << 'H'
         << "\x1b[1;48;5;235;38;5;231m" << name << "\x1b[0m";
      placed = true;
      break;
    }
    if (!placed)
      continue;
  }
  return os.str();
}

std::string App::currentTimeLabel() const
{
  NFmiMetTime t = itsSource->currentValidTime();
  return fmt::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02} UTC",
                     static_cast<int>(t.GetYear()),
                     static_cast<int>(t.GetMonth()),
                     static_cast<int>(t.GetDay()),
                     static_cast<int>(t.GetHour()),
                     static_cast<int>(t.GetMin()),
                     static_cast<int>(t.GetSec()));
}

void App::renderTimeline(UI& ui)
{
  const int id = itsSource->currentParamId();
  std::string label = itsSource->paramShortName(id);
  label += "  ";
  label += currentTimeLabel();
  // Level reminder for multi-level sources (pressure/hybrid/elangle/CAPPI).
  // Single-level sources skip this so we don't waste bar real estate.
  if (itsSource->levelCount() > 1)
  {
    label += fmt::format("  L:{}", itsSource->levelLabel(itsSource->currentLevelIndex()));
    // Multi-type GRIBs: surface the active type name so the user can see
    // whether they're looking at pressure / hybrid / height / ...  For a
    // single-type file the type suffix is already implicit in the label
    // (e.g. "850 hPa") and would just clutter.
    const auto groups = itsSource->levelGroupsForParam(id);
    if (groups.size() > 1)
    {
      const int g = itsSource->currentLevelGroupIndex(id);
      if (g >= 0 && g < static_cast<int>(groups.size()))
        label += fmt::format(" ({})", groups[static_cast<std::size_t>(g)].typeName);
    }
  }
  if (const std::string orig = originTimeLabel(); !orig.empty())
    label += "   (analysis " + orig + ")";
  if (itsAnimating)
    label += fmt::format("  [{} ms]", itsAnimationDelayMs);
  if (!itsLastMessage.empty())
  {
    label += "   ";
    label += itsLastMessage;
    itsLastMessage.clear();
  }
  else if (!itsPhenomenonHint.empty())
  {
    // Persistent hint from the phenomenon detectors. Yields to any
    // transient itsLastMessage so user actions stay informative; comes
    // back automatically the next redraw.
    label += "   ";
    label += itsPhenomenonHint;
  }
  const bool isShape = itsSource->isVector();
  const bool pgMode = (itsPgDataset != nullptr);
  ui.drawTimeline(label,
                  static_cast<int>(itsSource->currentTimeIndex()),
                  static_cast<int>(itsSource->timeCount()));
  ui.drawStatusBar(
      itsSource->isImage(), isShape, pgMode, !itsOpts.browseRoot.empty(), hasWindComponents());
  doupdate();
}

std::string App::originTimeLabel() const
{
  NFmiMetTime t = itsSource->originTime();
  // Year 0 = explicit "no origin time"; pre-2000 = grid-files placeholder
  // (NetCDF without a reference-time attribute parses as ~1970-10-01).
  // Either way, suppress the suffix rather than displaying obvious noise.
  if (t.GetYear() < 2000)
    return {};
  return fmt::format("{:04}-{:02}-{:02} {:02}:{:02} UTC",
                     static_cast<int>(t.GetYear()),
                     static_cast<int>(t.GetMonth()),
                     static_cast<int>(t.GetDay()),
                     static_cast<int>(t.GetHour()),
                     static_cast<int>(t.GetMin()));
}

namespace
{
std::string formatFileSize(std::uintmax_t bytes)
{
  // Human-readable: KB / MB / GB up to 3 significant digits.
  if (bytes < 1024)
    return fmt::format("{} B", bytes);
  const double kb = bytes / 1024.0;
  if (kb < 1024.0)
    return fmt::format("{:.1f} KB", kb);
  const double mb = kb / 1024.0;
  if (mb < 1024.0)
    return fmt::format("{:.1f} MB", mb);
  return fmt::format("{:.2f} GB", mb / 1024.0);
}

std::string formatMetTime(const NFmiMetTime& t)
{
  if (t.GetYear() < 2000)
    return {};
  return fmt::format("{:04}-{:02}-{:02} {:02}:{:02} UTC",
                     static_cast<int>(t.GetYear()),
                     static_cast<int>(t.GetMonth()),
                     static_cast<int>(t.GetDay()),
                     static_cast<int>(t.GetHour()),
                     static_cast<int>(t.GetMin()));
}
}  // namespace

std::vector<std::pair<std::string, std::string>> App::buildMetadataRows() const
{
  std::vector<std::pair<std::string, std::string>> rows;
  std::error_code ec;
  const std::filesystem::path filePath(itsOpts.filename);
  rows.emplace_back("File", filePath.filename().string());
  if (filePath.has_parent_path())
    rows.emplace_back("Directory", filePath.parent_path().string());
  if (auto sz = std::filesystem::file_size(filePath, ec); !ec)
    rows.emplace_back("Size", formatFileSize(sz));

  // Backend-specific extras (format, producer, grid type, dimensions, …)
  // come right after the file identification.
  for (auto&& kv : itsSource->extraMetadata())
    rows.push_back(std::move(kv));

  rows.emplace_back("", "");

  // Geographic extent.
  const auto bbox = itsSource->boundingBox();
  rows.emplace_back("Lat range", fmt::format("{:.3f}..{:.3f}", bbox.minLat, bbox.maxLat));
  rows.emplace_back("Lon range", fmt::format("{:.3f}..{:.3f}", bbox.minLon, bbox.maxLon));

  // Times.
  if (auto origin = formatMetTime(itsSource->originTime()); !origin.empty())
    rows.emplace_back("Reference", origin);
  const std::size_t nt = itsSource->timeCount();
  if (nt > 0)
  {
    // Sample the first and last time without leaving the source mutated.
    const auto savedT = itsSource->currentTimeIndex();
    auto* mut = const_cast<DataSource*>(itsSource.get());
    mut->selectTimeIndex(0);
    const std::string first = formatMetTime(itsSource->currentValidTime());
    mut->selectTimeIndex(nt - 1);
    const std::string last = formatMetTime(itsSource->currentValidTime());
    mut->selectTimeIndex(savedT);
    if (nt == 1)
      rows.emplace_back("Times", fmt::format("1 step at {}", first));
    else
      rows.emplace_back("Times", fmt::format("{} steps, {} -> {}", nt, first, last));
  }

  // Levels — ignore non-finite sentinels (used by sources with synthetic
  // levels like PVOL's "MAX" composite) for the min/max display.
  const std::size_t nl = itsSource->levelCount();
  if (nl > 0)
  {
    float lo = std::numeric_limits<float>::infinity();
    float hi = -std::numeric_limits<float>::infinity();
    std::size_t finiteCount = 0;
    for (std::size_t i = 0; i < nl; ++i)
    {
      const float v = itsSource->levelValueAt(i);
      if (!std::isfinite(v))
        continue;
      lo = std::min(lo, v);
      hi = std::max(hi, v);
      ++finiteCount;
    }
    if (finiteCount == 1)
      rows.emplace_back("Levels", fmt::format("{} ({:g})", nl, lo));
    else if (finiteCount > 1)
      rows.emplace_back("Levels", fmt::format("{} ({:g}..{:g})", nl, lo, hi));
    else
      rows.emplace_back("Levels", fmt::format("{}", nl));
  }
  // Level-type inventory — one row per type present for the active
  // parameter, sized to indicate "Temperature has 13 pressure surfaces
  // *and* 65 hybrid surfaces in this file." Only shown when more than
  // one type exists; single-type files are already covered by "Levels".
  {
    const auto groups = itsSource->levelGroupsForParam(itsSource->currentParamId());
    if (groups.size() > 1)
    {
      std::string s;
      for (std::size_t i = 0; i < groups.size(); ++i)
      {
        if (i)
          s += ", ";
        s += fmt::format("{} ({})", groups[i].typeName, groups[i].values.size());
      }
      rows.emplace_back("Level types", s);
    }
  }

  rows.emplace_back("", "");

  // Parameter listing — name + units, comma-separated. The popup truncates
  // on the right with an ellipsis if it's wider than the popup allows.
  std::string paramsLine;
  for (std::size_t i = 0; i < itsParamIds.size(); ++i)
  {
    if (!paramsLine.empty())
      paramsLine += ", ";
    const int id = itsParamIds[i];
    paramsLine += itsSource->paramShortName(id);
    if (auto u = itsSource->paramUnits(id); !u.empty())
    {
      paramsLine += " [";
      paramsLine += u;
      paramsLine += "]";
    }
  }
  rows.emplace_back("Params", fmt::format("{}", itsParamIds.size()));
  if (!paramsLine.empty())
    rows.emplace_back("", paramsLine);
  return rows;
}

std::vector<std::string> App::paramLabels() const
{
  std::vector<std::string> out;
  out.reserve(itsParamIds.size());
  for (int id : itsParamIds)
    out.push_back(itsSource->paramShortName(id));
  return out;
}

std::vector<std::string> App::levelLabels() const
{
  std::vector<std::string> out;
  out.reserve(itsSource->levelCount());
  for (std::size_t i = 0; i < itsSource->levelCount(); ++i)
    out.push_back(itsSource->levelLabel(i));
  return out;
}

void App::refreshPhenomenonHint()
{
  // Single-frame detectors are ~3-5 ms total on a 72×36 sample. Run
  // them synchronously here — the brief pause is imperceptible.
  // Temporal detectors (block, static) self-skip on files with fewer
  // than 3 time steps and otherwise add ~25 ms of work. If that
  // becomes a noticeable hitch on large files, this is the place to
  // dispatch the call to a worker thread (it needs a mutex around the
  // DataSource because NFmiQueryData isn't thread-safe — see notes
  // in QdlessPhenomena.cpp).
  if (!itsSource) {
    itsPhenomenonHint.clear();
    itsPhenomenonHasAnchor = false;
    return;
  }
  const auto hint = detectPhenomena(*itsSource);
  if (hint.score > 0)
  {
    itsPhenomenonHint = hint.message + "  " + hint.suggestion;
    itsPhenomenonAnchorLat = hint.anchorLat;
    itsPhenomenonAnchorLon = hint.anchorLon;
    itsPhenomenonHasAnchor = hint.hasAnchor;
  }
  else
  {
    itsPhenomenonHint.clear();
    itsPhenomenonHasAnchor = false;
  }
}

void App::selectParam(int newIndex)
{
  if (newIndex < 0 || newIndex >= static_cast<int>(itsParamIds.size()))
    return;
  activePanel().paramIndex = newIndex;
  const int paramId = itsParamIds[newIndex];
  itsSource->selectParamId(paramId);
  // Restore the panel's last-active level group for this parameter (or
  // clamp to 0 if the parameter only exists on one type now). The source
  // owns the per-(param, group) level memory, so flipping the group here
  // also snaps the level index back to where the user left it.
  const int nGroups = static_cast<int>(itsSource->levelGroupsForParam(paramId).size());
  int g = activePanel().levelGroupIdx;
  if (g < 0 || g >= std::max(1, nGroups))
    g = 0;
  activePanel().levelGroupIdx = g;
  itsSource->selectLevelGroup(paramId, g);
  activePanel().levelIndex = itsSource->currentLevelIndex();
  loadPalette();  // re-resolve palette for the active panel's new parameter
  refreshPhenomenonHint();
}

void App::selectLevel(int newIndex)
{
  if (newIndex < 0 || newIndex >= static_cast<int>(itsSource->levelCount()))
    return;
  itsSource->selectLevelIndex(static_cast<unsigned long>(newIndex));
  activePanel().levelIndex = static_cast<std::size_t>(newIndex);
  itsOpts.levelIndex = newIndex;
  refreshPhenomenonHint();
}

std::vector<PanelRect> App::currentPanelRects(int row, int col, int height, int width) const
{
  if (width <= 0 || height <= 0)
    return {};
  const PanelRect full{row, col, height, width};
  switch (itsPanelLayout)
  {
    case PanelLayout::Single:
      return {full};
    case PanelLayout::Side:
    {
      if (width < 4)
        return {full};
      const int leftW = (width - 1) / 2;
      const int rightW = width - 1 - leftW;
      return {
          {row, col, height, leftW},
          {row, col + leftW + 1, height, rightW},
      };
    }
    case PanelLayout::Quad:
    {
      if (width < 4 || height < 4)
        return {full};
      const int leftW = (width - 1) / 2;
      const int rightW = width - 1 - leftW;
      const int topH = (height - 1) / 2;
      const int botH = height - 1 - topH;
      return {
          {row, col, topH, leftW},
          {row, col + leftW + 1, topH, rightW},
          {row + topH + 1, col, botH, leftW},
          {row + topH + 1, col + leftW + 1, botH, rightW},
      };
    }
  }
  return {full};
}

std::optional<int> App::panelAtCell(const UI& ui, int cellX, int cellY) const
{
  const auto& l = ui.layout();
  const auto rects = currentPanelRects(l.map.row, l.map.col, l.map.height, l.map.width);
  for (std::size_t i = 0; i < rects.size() && i < itsPanels.size(); ++i)
  {
    const auto& r = rects[i];
    if (cellX >= r.col && cellX < r.col + r.width && cellY >= r.row && cellY < r.row + r.height)
      return static_cast<int>(i);
  }
  return std::nullopt;
}

void App::setActivePanel(int idx)
{
  if (idx < 0 || idx >= static_cast<int>(itsPanels.size()))
    return;
  if (idx == itsActivePanel)
    return;
  itsActivePanel = idx;
  // Restore source state so the rest of the UI sees the new active panel.
  if (activePanel().paramIndex >= 0 &&
      activePanel().paramIndex < static_cast<int>(itsParamIds.size()))
    itsSource->selectParamId(itsParamIds[activePanel().paramIndex]);
  if (activePanel().levelIndex < itsSource->levelCount())
    itsSource->selectLevelIndex(activePanel().levelIndex);
}

void App::cycleActivePanel(int step)
{
  const int n = static_cast<int>(itsPanels.size());
  if (n <= 1)
    return;
  const int next = ((itsActivePanel + step) % n + n) % n;
  setActivePanel(next);
}

void App::cyclePanelLayout()
{
  PanelLayout next = PanelLayout::Single;
  switch (itsPanelLayout)
  {
    case PanelLayout::Single:
      next = PanelLayout::Side;
      break;
    case PanelLayout::Side:
      next = PanelLayout::Quad;
      break;
    case PanelLayout::Quad:
      next = PanelLayout::Single;
      break;
  }
  setPanelLayout(next);
}

void App::setPanelLayout(PanelLayout layout)
{
  itsPanelLayout = layout;
  std::size_t want = 1;
  switch (layout)
  {
    case PanelLayout::Single:
      want = 1;
      break;
    case PanelLayout::Side:
      want = 2;
      break;
    case PanelLayout::Quad:
      want = 4;
      break;
  }

  // Grow: clone the active panel for each new slot and rotate paramIndex
  // so the user immediately sees different data.
  if (itsPanels.size() < want)
  {
    const Panel base = activePanel();
    const int n = static_cast<int>(itsParamIds.size());
    while (itsPanels.size() < want)
    {
      const int slot = static_cast<int>(itsPanels.size());
      Panel p = base;
      if (n > 0)
        p.paramIndex = (base.paramIndex + slot) % n;
      itsPanels.push_back(p);

      // Resolve palette / value-shift for the new panel.
      const int savedActive = itsActivePanel;
      itsActivePanel = slot;
      if (p.paramIndex >= 0 && p.paramIndex < n)
        itsSource->selectParamId(itsParamIds[p.paramIndex]);
      if (p.levelIndex < itsSource->levelCount())
        itsSource->selectLevelIndex(p.levelIndex);
      loadPalette();
      itsActivePanel = savedActive;
    }
  }
  // Shrink: drop trailing panels.
  if (itsPanels.size() > want)
  {
    itsPanels.resize(want);
    if (itsActivePanel >= static_cast<int>(itsPanels.size()))
      itsActivePanel = 0;
  }

  // Restore source to the active panel's selection.
  if (activePanel().paramIndex >= 0 &&
      activePanel().paramIndex < static_cast<int>(itsParamIds.size()))
    itsSource->selectParamId(itsParamIds[activePanel().paramIndex]);
  if (activePanel().levelIndex < itsSource->levelCount())
    itsSource->selectLevelIndex(activePanel().levelIndex);

  itsLastMessage = panelLayoutLabel(layout);
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
  if (!cellToViewport(ui, cellX, cellY, u, v))
    return false;
  itsSource->uvToLatLon(u, v, lat, lon);
  return true;
}

void App::openProbe(int cellX, int cellY, UI& ui)
{
  // The probe is a time-series chart of the scalar value at the clicked
  // (lat, lon) over every available timestep. RGB triplets from a raw
  // image have no scalar interpretation — silently no-op.
  if (itsSource->isImage())
    return;
  double lat = 0;
  double lon = 0;
  if (!cellToLatLon(ui, cellX, cellY, lat, lon))
    return;
  // Shapefile sources don't have a scalar time axis either — every
  // burn id is a feature index, not a measurement. Repurpose the
  // click as "show this polygon's .dbf attributes".
  if (auto* shp = dynamic_cast<const ShapeSource*>(itsSource.get()))
  {
    auto attrs = shp->attributesAt(lat, lon);
    if (attrs.empty())
    {
      itsLastMessage = "No feature here";
      return;
    }
    // Loop: clicking outside the popup re-probes the new location
    // and pops up its attributes, so the user can hop between
    // polygons by clicking around without keystrokes between.
    itsMarker = std::make_pair(lat, lon);
    auto click = ui.popupMetadata("Feature attributes", attrs);
    while (click.has_value())
    {
      double nlat = 0;
      double nlon = 0;
      if (!cellToLatLon(ui, click->x, click->y, nlat, nlon))
        break;
      auto next = shp->attributesAt(nlat, nlon);
      if (next.empty())
      {
        itsLastMessage = "No feature here";
        break;
      }
      itsMarker = std::make_pair(nlat, nlon);
      click = ui.popupMetadata("Feature attributes", next);
    }
    return;
  }
  itsMarker = std::make_pair(lat, lon);
  openProbeAt(lat, lon, ui);
}

void App::openProbeAt(double lat, double lon, UI& ui)
{
  // Single-time, multi-level files (e.g. radar PVOL) have a degenerate
  // time-series, so the probe popup samples across LEVELS instead and
  // becomes a vertical profile chart. Arrow keys then step elevations;
  // the marker on the chart tracks the active level. Falls back to the
  // standard time-series whenever there's more than one time step.
  const bool useLevels = (itsSource->timeCount() <= 1 && itsSource->levelCount() > 1);

  // The popup runs an event loop until the user closes it via the keyboard.
  // A click outside the chart but on the map area is reported back as a
  // request to re-probe at that cell — we update the marker, redraw the
  // map, and reopen the popup at the new location.
  while (true)
  {
    // Sample the current parameter at this lat/lon for every step (each
    // step is one time index in the default mode, one level in VPR mode).
    // Save and restore the iterated index so the rest of the UI keeps state.
    std::vector<float> series;
    std::vector<std::string> stepLabels;
    const std::size_t stepCount = useLevels ? itsSource->levelCount() : itsSource->timeCount();
    series.reserve(stepCount);
    stepLabels.reserve(stepCount);
    const std::size_t savedTime = itsSource->currentTimeIndex();
    const std::size_t savedLevel = itsSource->currentLevelIndex();
    for (std::size_t i = 0; i < stepCount; ++i)
    {
      if (useLevels)
        itsSource->selectLevelIndex(i);
      else
        itsSource->selectTimeIndex(i);
      series.push_back(transform(itsSource->interpolatedValue(lat, lon)));
      stepLabels.push_back(useLevels ? itsSource->levelLabel(i) : currentTimeLabel());
    }
    if (useLevels)
      itsSource->selectLevelIndex(savedLevel);
    else
      itsSource->selectTimeIndex(savedTime);

    NFmiEnumConverter conv;
    std::string param = itsSource->paramShortName(itsSource->currentParamId());

    // Callback: when the user presses an arrow inside the popup, update the
    // iterated dimension (time or level) and redraw the map. Bottom timeline
    // only refreshes on time changes — for VPR it stays on the one valid
    // time and would just flicker.
    auto onTimeChange = [&](int newIdx)
    {
      if (useLevels)
      {
        itsSource->selectLevelIndex(static_cast<unsigned long>(newIdx));
        drawMap(ui);
      }
      else
      {
        itsSource->selectTimeIndex(static_cast<unsigned long>(newIdx));
        renderTimeline(ui);
        drawMap(ui);
      }
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
    auto computeStats = [this]()
    {
      UI::StatsSeries s;
      const auto cached = ensureViewportStats();
      s.min.reserve(cached.size());
      s.mean.reserve(cached.size());
      s.max.reserve(cached.size());
      for (const auto& vs : cached)
      {
        s.min.push_back(vs.valid ? vs.min : kFloatMissing);
        s.mean.push_back(vs.valid ? vs.mean : kFloatMissing);
        s.max.push_back(vs.valid ? vs.max : kFloatMissing);
      }
      return s;
    };
    const std::string units = itsSource->paramUnits(itsSource->currentParamId());
    // Viewport stats sweep across times; they're meaningless when the popup
    // is iterating levels, so suppress that overlay in VPR mode.
    int finalIdx = ui.popupTimeseries(param,
                                      lat,
                                      lon,
                                      series,
                                      stepLabels,
                                      static_cast<int>(useLevels ? savedLevel : savedTime),
                                      itsRenderer,
                                      activePanel().palette,
                                      onTimeChange,
                                      useLevels ? std::function<UI::StatsSeries()>{} : computeStats,
                                      units,
                                      &itsAnimationDelayMs,
                                      avoidRow,
                                      avoidCol,
                                      &clickRow,
                                      &clickCol);
    if (useLevels)
      itsSource->selectLevelIndex(static_cast<unsigned long>(finalIdx));
    else
      itsSource->selectTimeIndex(static_cast<unsigned long>(finalIdx));

    if (clickRow < 0 || clickCol < 0)
      break;  // closed via keyboard

    double newLat = 0;
    double newLon = 0;
    if (!cellToLatLon(ui, clickCol, clickRow, newLat, newLon))
      break;

    itsMarker = std::make_pair(newLat, newLon);
    drawMap(ui);
    lat = newLat;
    lon = newLon;
  }
}

bool App::handleKey(int key, UI& ui, bool& quit)
{
  // In raw-image mode the keys for parameter / level / legend popups,
  // overlay toggles (graticule, wind, cities, coast, borders), place
  // search, and cross-section all do nothing useful — there's no
  // parameter to switch, no palette legend, no projection. Intercept
  // them up front so the user gets a clear status message instead of
  // an empty popup or a silently-toggled flag with no visible effect.
  if (itsSource->isImage())
  {
    switch (key)
    {
      case 'p':
      case 'P':
      case 'L':
      case 'g':
      case 'G':
      case 'b':
      case 'B':
      case 'c':
      case 'C':
      case 'n':
      case 'N':
      case 'w':
      case 'W':
      case 'i':
      case 'I':
      case '/':
      case 'x':
      case 'X':
        itsLastMessage = "Not available in image mode";
        return true;
    }
  }
  // 3D mode remaps the navigation keys: hjkl orbit + tilt, +/- zoom, 0
  // reset, [/] threshold. Everything else falls through to the normal
  // handlers below so quit, parameter / level menu, etc. still work.
  if (itsMode3DCurtain)
  {
    // Mode 4: hjkl + +/- + 0 keep camera semantics from mode 3 so the
    // muscle memory carries over; arrow keys move the active curtain
    // endpoint(s) so a pure-keyboard workflow can fly the slice around
    // without leaving the viewport.
    constexpr double kYawStep = 0.1;
    constexpr double kPitchStep = 0.08;
    constexpr double kZoomStep = 0.8;
    const auto bb = itsSource->boundingBox();
    const double dLat = (bb.maxLat - bb.minLat) * kCurtainStepFrac;
    const double dLon = (bb.maxLon - bb.minLon) * kCurtainStepFrac;
    auto moveActive = [&](double dlat, double dlon)
    {
      if (itsCurtainActiveEnd == CurtainEnd::A || itsCurtainActiveEnd == CurtainEnd::Both)
      {
        itsCurtainLat1 += dlat;
        itsCurtainLon1 += dlon;
      }
      if (itsCurtainActiveEnd == CurtainEnd::B || itsCurtainActiveEnd == CurtainEnd::Both)
      {
        itsCurtainLat2 += dlat;
        itsCurtainLon2 += dlon;
      }
    };
    const bool inView = itsCurtainActiveEnd == CurtainEnd::View;
    auto subModeLabel = [&]()
    {
      switch (itsCurtainActiveEnd)
      {
        case CurtainEnd::A: return "A";
        case CurtainEnd::B: return "B";
        case CurtainEnd::Both: return "A+B";
        case CurtainEnd::View: return "View";
      }
      return "?";
    };
    switch (key)
    {
      // hjkl always controls the camera so power users keep a direct
      // shortcut regardless of which sub-mode the arrow keys are bound
      // to. Arrow keys are the contextual binding.
      case 'h':
        itsCamYaw -= kYawStep;
        return true;
      case 'l':
        itsCamYaw += kYawStep;
        return true;
      case 'k':
        itsCamPitch = std::clamp(itsCamPitch + kPitchStep, 0.0, M_PI_2);
        return true;
      case 'j':
        itsCamPitch = std::clamp(itsCamPitch - kPitchStep, 0.0, M_PI_2);
        return true;
      case '+':
      case '=':
        // View sub-mode: +/- zooms the camera (existing 3D behaviour).
        // Edit sub-modes: +/- adjusts the curtain animation speed so
        // the user can tune the rotation / swing without leaving the
        // sub-mode. The speed only affects frames that are *animated*
        // — toggling all animations off makes the keys do nothing
        // visible, but the multiplier is still remembered.
        if (inView)
        {
          itsCamZoom /= kZoomStep;
        }
        else
        {
          itsCurtainAnimSpeed = std::min(kCurtainSpeedMax, itsCurtainAnimSpeed * kCurtainSpeedStep);
          itsLastMessage = fmt::format("Curtain anim speed: {:.2f}x", itsCurtainAnimSpeed);
        }
        return true;
      case '-':
      case '_':
        if (inView)
        {
          itsCamZoom *= kZoomStep;
        }
        else
        {
          itsCurtainAnimSpeed = std::max(kCurtainSpeedMin, itsCurtainAnimSpeed / kCurtainSpeedStep);
          itsLastMessage = fmt::format("Curtain anim speed: {:.2f}x", itsCurtainAnimSpeed);
        }
        return true;
      case '0':
        itsCamYaw = 0;
        itsCamPitch = 0.6;
        itsCamZoom = 1.0;
        return true;
      case '\t':
      {
        // Cycle A -> B -> Both -> View -> A. The four-way cycle keeps
        // the original endpoint-edit modes available and adds a clean
        // "camera-only" mode where arrows orbit the scene.
        switch (itsCurtainActiveEnd)
        {
          case CurtainEnd::A: itsCurtainActiveEnd = CurtainEnd::B; break;
          case CurtainEnd::B: itsCurtainActiveEnd = CurtainEnd::Both; break;
          case CurtainEnd::Both: itsCurtainActiveEnd = CurtainEnd::View; break;
          case CurtainEnd::View: itsCurtainActiveEnd = CurtainEnd::A; break;
        }
        itsLastMessage = fmt::format("Curtain sub-mode: {}", subModeLabel());
        return true;
      }
      case KEY_BTAB:
      {
        // Shift-Tab: same cycle in reverse so the user can back out of
        // an accidental over-tab without going all the way around again.
        switch (itsCurtainActiveEnd)
        {
          case CurtainEnd::A: itsCurtainActiveEnd = CurtainEnd::View; break;
          case CurtainEnd::B: itsCurtainActiveEnd = CurtainEnd::A; break;
          case CurtainEnd::Both: itsCurtainActiveEnd = CurtainEnd::B; break;
          case CurtainEnd::View: itsCurtainActiveEnd = CurtainEnd::Both; break;
        }
        itsLastMessage = fmt::format("Curtain sub-mode: {}", subModeLabel());
        return true;
      }
      case KEY_LEFT:
        if (inView) itsCamYaw -= kYawStep;
        else moveActive(0.0, -dLon);
        return true;
      case KEY_RIGHT:
        if (inView) itsCamYaw += kYawStep;
        else moveActive(0.0, dLon);
        return true;
      case KEY_UP:
        if (inView) itsCamPitch = std::clamp(itsCamPitch + kPitchStep, 0.0, M_PI_2);
        else moveActive(dLat, 0.0);
        return true;
      case KEY_DOWN:
        if (inView) itsCamPitch = std::clamp(itsCamPitch - kPitchStep, 0.0, M_PI_2);
        else moveActive(-dLat, 0.0);
        return true;
      case KEY_NPAGE:
        itsCurtainHeightKm = std::max(2.0, itsCurtainHeightKm - 1.0);
        itsLastMessage = fmt::format("Curtain ceiling: {:g} km", itsCurtainHeightKm);
        return true;
      case KEY_PPAGE:
        itsCurtainHeightKm = std::min(30.0, itsCurtainHeightKm + 1.0);
        itsLastMessage = fmt::format("Curtain ceiling: {:g} km", itsCurtainHeightKm);
        return true;
      // Animation toggles work in every sub-mode — they're properties
      // of the curtain, not the camera. Lowercase x s r o T keep the
      // single-letter mnemonics; uppercase T avoids shadowing the
      // global 't' (corner-style cycle).
      case 'x':
      case 'X':
        itsCurtainTwoPlane = !itsCurtainTwoPlane;
        itsLastMessage = fmt::format("X-cross: {}", itsCurtainTwoPlane ? "on" : "off");
        return true;
      case 's':
      case 'S':
        itsCurtainAutoSwing = !itsCurtainAutoSwing;
        itsLastMessage = fmt::format("Swing: {}", itsCurtainAutoSwing ? "on" : "off");
        return true;
      case 'r':
      case 'R':
        itsCurtainAutoRotate = !itsCurtainAutoRotate;
        itsLastMessage = fmt::format("Rotate: {}", itsCurtainAutoRotate ? "on" : "off");
        return true;
      case 'o':
      case 'O':
        itsCurtainAutoOrbit = !itsCurtainAutoOrbit;
        itsLastMessage = fmt::format("Orbit: {}", itsCurtainAutoOrbit ? "on" : "off");
        return true;
      case 'T':
        itsCurtainAutoTilt = !itsCurtainAutoTilt;
        itsLastMessage = fmt::format("Tilt: {}", itsCurtainAutoTilt ? "on" : "off");
        return true;
      default:
        break;  // fall through to global keys (quit, menus, animation, ...)
    }
  }
  if (itsModeGlobe)
  {
    constexpr double kSpinStep = 0.12;  // ≈ 6.9° per press
    constexpr double kZoomStep = 0.8;
    // Keep the centre latitude just shy of the poles so the east/up basis
    // never degenerates while the user is tilting.
    constexpr double kLatLimit = M_PI_2 - 0.01;
    switch (key)
    {
      case 'h':
      case KEY_LEFT:
        itsCamYaw -= kSpinStep;  // spin west
        return true;
      case 'l':
      case KEY_RIGHT:
        itsCamYaw += kSpinStep;  // spin east
        return true;
      case 'k':
      case KEY_UP:
        itsCamPitch = std::clamp(itsCamPitch + kSpinStep, -kLatLimit, kLatLimit);
        return true;
      case 'j':
      case KEY_DOWN:
        itsCamPitch = std::clamp(itsCamPitch - kSpinStep, -kLatLimit, kLatLimit);
        return true;
      case '+':
      case '=':
        itsCamZoom = std::min(20.0, itsCamZoom / kZoomStep);
        return true;
      case '-':
      case '_':
        itsCamZoom = std::max(0.5, itsCamZoom * kZoomStep);
        return true;
      case '0':
      {
        // Re-centre on the data and reset zoom.
        const auto bb = itsSource->boundingBox();
        itsCamYaw = ((bb.minLon + bb.maxLon) * 0.5) * M_PI / 180.0;
        itsCamPitch =
            std::clamp(((bb.minLat + bb.maxLat) * 0.5) * M_PI / 180.0, -kLatLimit, kLatLimit);
        itsCamZoom = 1.0;
        return true;
      }
      case 'G':
        break;  // fall through to the toggle handler below (lowercase g = legend)
      default:
        break;
    }
  }
  if (itsMode3D)
  {
    constexpr double kYawStep = 0.1;  // ≈ 5.7°
    constexpr double kPitchStep = 0.08;
    constexpr double kZoomStep = 0.8;
    switch (key)
    {
      case 'h':
      case KEY_LEFT:
        itsCamYaw -= kYawStep;
        return true;
      case 'l':
      case KEY_RIGHT:
        itsCamYaw += kYawStep;
        return true;
      case 'k':
      case KEY_UP:
        itsCamPitch = std::clamp(itsCamPitch + kPitchStep, 0.0, M_PI_2);
        return true;
      case 'j':
      case KEY_DOWN:
        itsCamPitch = std::clamp(itsCamPitch - kPitchStep, 0.0, M_PI_2);
        return true;
      case '+':
      case '=':
        itsCamZoom /= kZoomStep;
        return true;
      case '-':
      case '_':
        itsCamZoom *= kZoomStep;
        return true;
      case '0':
        itsCamYaw = 0;
        itsCamPitch = 0.6;
        itsCamZoom = 1.0;
        return true;
      case ',':
      case '<':
        itsThreshold3D -= 5;
        itsLastMessage = fmt::format("3D threshold: {:.0f} {}", itsThreshold3D, itsThreshold3DUnit);
        return true;
      case '.':
      case '>':
        itsThreshold3D += 5;
        itsLastMessage = fmt::format("3D threshold: {:.0f} {}", itsThreshold3D, itsThreshold3DUnit);
        return true;
      case KEY_PPAGE:  // PgUp — taller storms
        itsVexagger3D = std::min(50.0, itsVexagger3D * 1.4);
        itsLastMessage = fmt::format("3D vertical exaggeration: {:.1f}×", itsVexagger3D);
        return true;
      case KEY_NPAGE:  // PgDn — flatter storms
        itsVexagger3D = std::max(1.0, itsVexagger3D / 1.4);
        itsLastMessage = fmt::format("3D vertical exaggeration: {:.1f}×", itsVexagger3D);
        return true;
      case 'x':
      case 'X':
      {
        // Toggle the persistent-extrema overlay (volumetric QueryData only):
        // swap the full point cloud for the detected anomaly air masses.
        const auto* qd = dynamic_cast<const QueryDataSource*>(itsSource.get());
        if (qd != nullptr && qd->isVolumetric())
        {
          itsShowExtrema = !itsShowExtrema;
          itsLastMessage = itsShowExtrema
                               ? "Extrema: persistent anomaly air masses (x = back to cloud)"
                               : "Extrema off — full volume cloud";
        }
        else
        {
          itsLastMessage = "Extrema view needs a volumetric QueryData file (hybrid / pressure)";
        }
        return true;
      }
      case '3':
        // Fall through to the normal toggle handler below.
        break;
      default:
        break;
    }
  }
  switch (key)
  {
    case 'q':
    case 'Q':
    case 27:  // Esc
      quit = true;
      return false;

    case KEY_F(9):
    {
      // Hidden: preview exit effects without quitting. Each press plays the
      // next effect in sequence and names it on the timeline.
      const int idx = itsExitEffectPreview;
      playExitEffect(idx);
      itsExitEffectPreview = (idx + 1) % exitEffectCount();
      itsLastMessage =
          fmt::format("Exit effect {}/{}: {}", idx + 1, exitEffectCount(), exitEffectName(idx));
      ui.touch();
      return true;
    }

    case KEY_F(10):
    {
      // Hidden: replay the last exit effect verbatim (same effect + seed).
      if (itsLastExitSeed != 0)
        playExitEffect(itsLastExitIndex, itsLastExitSeed);
      else
        playExitEffect(-1, 0);
      itsLastMessage = fmt::format("Exit effect (repeat): {}", exitEffectName(itsLastExitIndex));
      ui.touch();
      return true;
    }

    case KEY_F(8):
    {
      // Two-step exit-effect picker: choose a theme, then an effect.
      // 211 effects in one flat list is too much to scroll; the theme
      // grouping turns it into ~10 short menus. Esc out of the inner
      // popup returns to the theme list; Esc out of the theme list
      // closes the picker. The theme cursor + per-theme effect cursor
      // are remembered across F8 invocations.
      const int nThemes = exitThemeCount();
      while (true)
      {
        std::vector<std::string> themeItems;
        std::vector<int> themeCounts(static_cast<std::size_t>(nThemes), 0);
        themeItems.reserve(static_cast<std::size_t>(nThemes));
        for (int i = 0; i < exitEffectCount(); ++i)
        {
          const int t = exitEffectTheme(i);
          if (t >= 0 && t < nThemes)
            ++themeCounts[static_cast<std::size_t>(t)];
        }
        for (int t = 0; t < nThemes; ++t)
          themeItems.emplace_back(fmt::format(
              "{} ({})", exitThemeName(t), themeCounts[static_cast<std::size_t>(t)]));
        const int themeSel =
            ui.popupMenu("Exit effect — choose a theme", themeItems, itsExitThemePreview);
        if (themeSel < 0)
          break;  // Esc — close picker entirely
        itsExitThemePreview = themeSel;

        // Build the effects-in-this-theme list, remembering the global
        // index so the selection maps back to playExitEffect.
        std::vector<std::string> effectItems;
        std::vector<int> globalIdx;
        for (int i = 0; i < exitEffectCount(); ++i)
        {
          if (exitEffectTheme(i) == themeSel)
          {
            effectItems.emplace_back(exitEffectName(i));
            globalIdx.push_back(i);
          }
        }
        if (effectItems.empty())
          continue;  // empty theme (shouldn't happen) — back to theme picker

        // Default the inner cursor to whatever effect was last previewed
        // if it lives in this theme; otherwise start at 0.
        int innerStart = 0;
        for (int k = 0; k < static_cast<int>(globalIdx.size()); ++k)
          if (globalIdx[static_cast<std::size_t>(k)] == itsExitEffectPreview)
          {
            innerStart = k;
            break;
          }
        const int innerSel = ui.popupMenu(
            fmt::format("Exit effect — {}", exitThemeName(themeSel)),
            effectItems, innerStart);
        if (innerSel < 0)
          continue;  // Esc out of inner — back to theme picker
        const int sel = globalIdx[static_cast<std::size_t>(innerSel)];
        itsExitEffectPreview = sel;
        // For word reveal the current line is forced; other effects ignore it.
        playExitEffect(sel, 0, exitWordline(itsExitWordPreview));
        itsLastMessage = fmt::format("Exit effect: {}", exitEffectName(sel));
        break;
      }
      ui.touch();
      return true;
    }

    case KEY_F(11):
    {
      // Hidden: cycle the current effect's sub-choice. For word reveal, step
      // through the anthology lines; for any other effect, re-roll the seed.
      if (itsLastExitIndex == exitEffectIndexByName("word reveal"))
      {
        itsExitWordPreview = (itsExitWordPreview + 1) % exitWordlineCount();
        playExitEffect(itsLastExitIndex, 0, exitWordline(itsExitWordPreview));
        itsLastMessage = fmt::format("Exit text {}/{}: {}",
                                     itsExitWordPreview + 1,
                                     exitWordlineCount(),
                                     exitWordline(itsExitWordPreview));
      }
      else
      {
        playExitEffect(itsLastExitIndex, 0);  // <0 -> random; else fresh-seed variant
        itsLastMessage = fmt::format("Exit effect (variant): {}", exitEffectName(itsLastExitIndex));
      }
      ui.touch();
      return true;
    }

    case KEY_F(12):
    {
      // Hidden: pick a word-reveal anthology line from a menu and play it.
      std::vector<std::string> items;
      items.reserve(static_cast<std::size_t>(exitWordlineCount()));
      for (int i = 0; i < exitWordlineCount(); ++i)
        items.emplace_back(exitWordline(i));
      const int sel = ui.popupMenu("Exit text", items, itsExitWordPreview);
      if (sel >= 0)
      {
        itsExitWordPreview = sel;
        playExitEffect(exitEffectIndexByName("word reveal"), 0, exitWordline(sel));
        itsLastMessage = fmt::format("Exit text: {}", exitWordline(sel));
      }
      ui.touch();
      return true;
    }

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
      // Live preview: as the highlight moves, commit the candidate
      // parameter and repaint the map underneath so the user can sweep
      // through all parameters by holding ↓ / ↑. Esc reverts to the
      // entry-time selection; Enter or a hotkey leaves the previewed
      // parameter active.
      const int savedIdx = activePanel().paramIndex;
      auto preview = [this, &ui](int idx)
      {
        selectParam(idx);
        renderTimeline(ui);
        drawMap(ui);
        drawCrossSection(ui);
      };
      int picked = ui.popupMenu("Parameters", paramLabels(), savedIdx, false, preview);
      if (picked < 0)
        selectParam(savedIdx);
      else if (picked != activePanel().paramIndex)
        selectParam(picked);
      return true;  // repaint over the popup
    }
    case 'L':  // uppercase only — lowercase 'l' is reserved for pan-right
    {
      // Live preview, matching the parameter popup.
      const int paramId = itsSource->currentParamId();
      const auto groups = itsSource->levelGroupsForParam(paramId);
      const int savedGroup = activePanel().levelGroupIdx;
      const int savedLevel = static_cast<int>(itsSource->currentLevelIndex());

      // Build the menu: one header per group, followed by that group's
      // levels. Remember which row maps to which (group, level) so we
      // can flip both atomically on selection.
      std::vector<UI::MenuRow> rows;
      std::vector<std::pair<int, int>> rowToGL;  // (groupIdx, levelIdx) per row; (-1,-1) for headers
      int currentRow = 0;
      const bool multi = groups.size() > 1;
      for (std::size_t gi = 0; gi < groups.size(); ++gi)
      {
        if (multi)
        {
          UI::MenuRow h;
          h.label = groups[gi].typeName + fmt::format(" ({})", groups[gi].values.size());
          h.isHeader = true;
          rows.push_back(std::move(h));
          rowToGL.emplace_back(-1, -1);
        }
        for (std::size_t li = 0; li < groups[gi].values.size(); ++li)
        {
          UI::MenuRow r;
          // Per-row label: just the type-aware value string (the header
          // already names the type when multi; we keep the unit suffix
          // for clarity in single-group case too).
          r.label = DataSource::formatLevelByType(groups[gi].typeId, groups[gi].values[li]);
          rows.push_back(std::move(r));
          rowToGL.emplace_back(static_cast<int>(gi), static_cast<int>(li));
          if (static_cast<int>(gi) == savedGroup && static_cast<int>(li) == savedLevel)
            currentRow = static_cast<int>(rows.size()) - 1;
        }
      }
      if (rows.empty())
        return true;  // nothing to pick

      auto applyRow = [&](int row)
      {
        if (row < 0 || row >= static_cast<int>(rowToGL.size()))
          return;
        const auto [gi, li] = rowToGL[row];
        if (gi < 0)
          return;
        if (gi != activePanel().levelGroupIdx)
        {
          activePanel().levelGroupIdx = gi;
          itsSource->selectLevelGroup(paramId, gi);
        }
        selectLevel(li);
      };
      auto preview = [&](int row)
      {
        applyRow(row);
        renderTimeline(ui);
        drawMap(ui);
        drawCrossSection(ui);
      };

      int picked = multi ? ui.popupMenuSections("Levels", rows, currentRow, preview)
                         : ui.popupMenu("Levels",
                                        [&]
                                        {
                                          std::vector<std::string> ss;
                                          ss.reserve(rows.size());
                                          for (const auto& r : rows)
                                            ss.push_back(r.label);
                                          return ss;
                                        }(),
                                        savedLevel,
                                        false,
                                        [&](int li) { preview(li); });
      if (picked < 0)
      {
        // Cancelled — restore everything to what it was when the popup opened.
        if (savedGroup != activePanel().levelGroupIdx)
        {
          activePanel().levelGroupIdx = savedGroup;
          itsSource->selectLevelGroup(paramId, savedGroup);
        }
        selectLevel(savedLevel);
      }
      else
      {
        applyRow(multi ? picked : picked);
      }
      return true;
    }

    // Quick level step without opening the popup. '<' / '>' for the
    // shift-required form and ',' / '.' for the unshifted keys — both
    // work the same so users can pick whichever is on top of their key.
    case '<':
    case ',':
    case '>':
    case '.':
      if (itsSource->levelCount() > 1)
      {
        const int n = static_cast<int>(itsSource->levelCount());
        const int delta = (key == '<' || key == ',') ? -1 : +1;
        int cur = static_cast<int>(itsSource->currentLevelIndex());
        cur = std::clamp(cur + delta, 0, n - 1);
        selectLevel(cur);
        itsLastMessage = fmt::format(
            "Level: {} ({}/{})", itsSource->levelLabel(static_cast<std::size_t>(cur)), cur + 1, n);
      }
      return true;

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
    {
      NFmiEnumConverter conv;
      std::string param = itsSource->paramShortName(itsSource->currentParamId());
      const Palette& pal = activePanel().palette;
      ui.popupLegend(param, pal.name(), pal, itsRenderer);
      return true;
    }

    case '?':
    {
      UI::HelpContext ctx;
      ctx.isImage = itsSource->isImage();
      ctx.isShape = itsSource->isVector();
      ctx.isPg = (itsPgDataset != nullptr);
      ctx.hasTimeAxis = itsSource->timeCount() > 1;
      ctx.hasMultipleParams = itsSource->paramIds().size() > 1;
      ctx.hasMultipleLevels = itsSource->levelCount() > 1;
      ctx.hasNativeHeight = itsSource->hasNativeHeight();
      ctx.has3DVolume = sourceSupports3D();
      ui.popupHelp(ctx);
    }
      return true;

    case 'r':
    case 'R':
    {
      // Palette cycle. Currently only meaningful for shapefile sources
      // (flat fill ↔ rainbow per burn id); a no-op with a status hint
      // for other backends so the user gets feedback either way.
      if (itsSource->isVector())
      {
        itsShapePaletteMode = (itsShapePaletteMode + 1) % 2;
        loadPalette();
        itsLastMessage = itsShapePaletteMode == 1 ? "Palette: rainbow" : "Palette: flat";
      }
      else
      {
        itsLastMessage = "Palette cycle is only available for shapefiles";
      }
      return true;
    }

    case 'd':
    case 'D':
    {
      // Re-open the data-source picker without quitting. Available in
      // two modes:
      //   --pg  : PostGIS layer picker (persistent dataset, no libpq
      //           round-trip on each invocation).
      //   --dir : PNG-tree leaf picker (rescans the tree so newly
      //           arrived directories appear).
      const bool inPgMode = (itsPgDataset != nullptr);
      const bool inBrowseMode = !itsOpts.browseRoot.empty();
      if (!inPgMode && !inBrowseMode)
      {
        itsLastMessage = "Picker is only available with --pg or --dir tree";
        return true;
      }
      // Reset state that's specific to the previous layer/leaf, then
      // re-pick. If the user cancels, we keep the current source.
      auto saved = std::move(itsSource);
      try
      {
        const bool ok = inPgMode ? openPgPicker(ui) : openBrowsePicker(ui);
        if (!ok)
        {
          itsSource = std::move(saved);
          return true;
        }
      }
      catch (...)
      {
        itsSource = std::move(saved);
        throw;
      }
      // Reset per-source state so the new selection renders cleanly.
      itsShapeOutlines.clear();
      itsViewport.reset();
      itsMarker.reset();
      itsCrossActive = false;
      itsCrossChartW = 0;
      itsCrossHoverLatLon.reset();
      initFromSource();
      return true;
    }

    case 'a':
    case 'A':
    {
      // Attributes table for shapefiles. popupSearch already gives us
      // a scrollable, type-to-filter list; build one row per feature
      // showing "#NN  field1=val1 | field2=val2 | …" and filter the
      // search query as a substring match against the whole row.
      // On Enter we drop the marker on the picked feature's centroid
      // and pop up its full attribute table (popupMetadata).
      auto* shp = dynamic_cast<const ShapeSource*>(itsSource.get());
      if (shp == nullptr)
      {
        itsLastMessage = "Attributes table is only available for shapefiles";
        return true;
      }
      const int n = shp->featureCount();
      // Format the table as fixed-width columns: one column per .dbf
      // field plus a leading "#NN" feature index. Compute the per-
      // column widths from header + value lengths, then pad each row
      // to those widths so the values line up vertically and the
      // header (drawn by popupSearch above the body) labels them
      // exactly.
      const auto& fields = shp->fieldNames();
      const std::size_t nfields = fields.size();
      // Count UTF-8 codepoints, not bytes — Finnish placenames
      // (Ähtäri, Selkämeri) have 2-byte chars whose .size() is double
      // their visible width, which mis-pads the columns.
      auto utf8Len = [](const std::string& s)
      {
        int w = 0;
        for (unsigned char c : s)
          if ((c & 0xC0) != 0x80)
            ++w;
        return w;
      };
      auto padRight = [&](const std::string& s, int w)
      {
        const int len = utf8Len(s);
        // Overlong values are left as-is (the popup clips on the
        // right edge); shorter values get spaces added.
        if (len >= w)
          return s;
        return s + std::string(static_cast<std::size_t>(w - len), ' ');
      };
      // Index column is just wide enough for "#" + largest id.
      int idxColW = static_cast<int>(std::to_string(n).size()) + 1;
      idxColW = std::max(idxColW, 3);
      // Per-column width = max of header + all values, capped at 24.
      std::vector<int> colW(nfields, 0);
      for (std::size_t k = 0; k < nfields; ++k)
        colW[k] = utf8Len(fields[k]);
      for (int i = 0; i < n; ++i)
      {
        const auto& attrs = shp->featureAttributes(i);
        for (std::size_t k = 0; k < nfields && k < attrs.size(); ++k)
          colW[k] = std::max(colW[k], utf8Len(attrs[k].second));
      }
      for (auto& w : colW)
        w = std::min(w, 24);
      // Header row: " #  | FIELD1 | FIELD2 | …"
      std::string header = padRight("#", idxColW);
      for (std::size_t k = 0; k < nfields; ++k)
      {
        header += " | ";
        header += padRight(fields[k], colW[k]);
      }
      // Data rows.
      std::vector<std::string> rows;
      rows.reserve(static_cast<std::size_t>(n));
      for (int i = 0; i < n; ++i)
      {
        std::string row = padRight("#" + std::to_string(i + 1), idxColW);
        const auto& attrs = shp->featureAttributes(i);
        for (std::size_t k = 0; k < nfields; ++k)
        {
          row += " | ";
          row += padRight(k < attrs.size() ? attrs[k].second : std::string{}, colW[k]);
        }
        rows.push_back(std::move(row));
      }
      // Indexed matcher: return matching rows AND remember which
      // feature each match maps to. popupSearch returns the index
      // into the most recent matches array, so we capture that here.
      std::vector<int> lastMatchIdx;
      auto matcher = [&](const std::string& query)
      {
        std::vector<std::string> hits;
        lastMatchIdx.clear();
        // Case-insensitive substring search across the whole row.
        std::string q = query;
        std::transform(
            q.begin(), q.end(), q.begin(), [](unsigned char c) { return std::tolower(c); });
        for (int i = 0; i < n; ++i)
        {
          if (q.empty())
          {
            hits.push_back(rows[i]);
            lastMatchIdx.push_back(i);
            continue;
          }
          std::string lower = rows[i];
          std::transform(lower.begin(),
                         lower.end(),
                         lower.begin(),
                         [](unsigned char c) { return std::tolower(c); });
          if (lower.find(q) != std::string::npos)
          {
            hits.push_back(rows[i]);
            lastMatchIdx.push_back(i);
          }
        }
        return hits;
      };
      const int matchSel = ui.popupSearch("Attributes (search to filter)", matcher, header);
      if (matchSel >= 0 && matchSel < static_cast<int>(lastMatchIdx.size()))
      {
        const int featureIdx = lastMatchIdx[matchSel];
        const auto [lat, lon] = shp->featureCentroid(featureIdx);
        itsMarker = std::make_pair(lat, lon);
        ui.popupMetadata("Feature #" + std::to_string(featureIdx + 1),
                         shp->featureAttributes(featureIdx));
      }
      return true;
    }

    case 'o':
    case 'O':
    {
      // Shapefile outline style cycle (Braille → Thick → None). Only
      // active when there's something to outline; on other sources the
      // key falls through silently (no message — there's no overlay
      // for it to talk about).
      if (!itsShapeOutlines.empty())
      {
        itsShapeOutlineStyle = nextLineStyle(itsShapeOutlineStyle);
        itsLastMessage = std::string("Shape outlines: ") + lineStyleLabel(itsShapeOutlineStyle);
        return true;
      }
      return false;
    }

    case 'M':
      ui.popupMetadata("File metadata", buildMetadataRows());
      return true;

    case 'b':
    case 'B':
    {
      const LineStyle prev = itsBorderStyle;
      itsBorderStyle = nextLineStyle(prev);
      if (itsBorderStyle == LineStyle::None)
      {
        itsBorders.clear();
        itsBorderPath.clear();
      }
      else if (prev == LineStyle::None)
      {
        itsBorderPath.clear();  // force reload on next drawMap
      }
      itsLastMessage = std::string("Borders: ") + lineStyleLabel(itsBorderStyle);
      return true;
    }

    case 'c':
    case 'C':
    {
      const LineStyle prev = itsCoastlineStyle;
      itsCoastlineStyle = nextLineStyle(prev);
      if (itsCoastlineStyle == LineStyle::None)
      {
        itsCoastlines.clear();
        itsCoastlinePath.clear();
      }
      else if (prev == LineStyle::None)
      {
        itsCoastlinePath.clear();
      }
      itsLastMessage = std::string("Coastlines: ") + lineStyleLabel(itsCoastlineStyle);
      return true;
    }

    case 't':
    case 'T':
      itsCornerStyle = nextCornerStyle(itsCornerStyle);
      itsRenderer.setCornerStyle(itsCornerStyle);
      itsLastMessage = std::string("Corners: ") + cornerStyleLabel(itsCornerStyle);
      return true;

    case 's':
    case 'S':
    {
      if (!itsCaps.kitty && !itsCaps.sixel)
      {
        itsLastMessage = "Graphics: neither Kitty nor Sixel supported by this terminal";
        return true;
      }
      // Cycle through Block -> Kitty (if supported) -> Sixel (if supported) -> Block.
      // Kitty wins ties because it's RGBA (no quantisation) and cheaper to encode.
      auto next = [&](GraphicsMode m)
      {
        if (m == GraphicsMode::Block)
          return itsCaps.kitty ? GraphicsMode::Kitty
                               : (itsCaps.sixel ? GraphicsMode::Sixel : GraphicsMode::Block);
        if (m == GraphicsMode::Kitty)
          return itsCaps.sixel ? GraphicsMode::Sixel : GraphicsMode::Block;
        return GraphicsMode::Block;  // from Sixel back to off
      };
      itsGraphicsMode = next(itsGraphicsMode);
      const char* label = itsGraphicsMode == GraphicsMode::Kitty   ? "Kitty"
                          : itsGraphicsMode == GraphicsMode::Sixel ? "Sixel"
                                                                   : "OFF";
      if (itsGraphicsMode == GraphicsMode::Block)
        itsLastMessage = "Graphics: OFF";
      else
        itsLastMessage =
            fmt::format("Graphics: {} ({}×{}px cells)", label, itsCaps.cellPxW, itsCaps.cellPxH);
      // Clear the screen — graphics blobs and block-glyph cells don't
      // share grid layout, so leftover pixels from the previous mode
      // would bleed through where the new mode writes less ink.
      std::fputs("\x1b[2J", stdout);
      return true;
    }

    case 'n':
    case 'N':
      itsGraticuleStyle = nextLineStyle(itsGraticuleStyle);
      itsLastMessage = std::string("Graticule: ") + lineStyleLabel(itsGraticuleStyle);
      return true;

    case 'w':
    case 'W':
      itsShowWindArrows = !itsShowWindArrows;
      return true;

    case 'i':
    case 'I':
      itsShowCities = !itsShowCities;
      itsLastMessage =
          itsShowCities ? fmt::format("Cities: top {} visible", itsCityOverlayN) : "Cities off";
      return true;

    case KEY_NPAGE:
    {
      // Denser: step N up.
      auto it = std::upper_bound(kCityNSteps.begin(), kCityNSteps.end(), itsCityOverlayN);
      if (it != kCityNSteps.end())
        itsCityOverlayN = *it;
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
      if (itsCrossPicks > 0)
      {
        itsCrossPicks = 0;
        itsLastMessage = "Cross-section cancelled";
      }
      else if (itsCrossActive)
      {
        itsCrossActive = false;
        itsCrossChartW = 0;
        itsCrossHoverLatLon.reset();
        itsLastMessage = "Cross-section closed";
      }
      else
      {
        itsCrossPicks = 2;
        // Each new cross-section opens in height mode when supported,
        // matching the [X]Section bottom-bar default. Stays user-toggled
        // within a single section but doesn't persist across sections.
        itsCrossHeightAxis = true;
        itsLastMessage = "Cross-section: click first endpoint";
      }
      return true;

    case 'y':
    case 'Y':
      // Toggle the cross-section Y-axis between km (true RHI) and one
      // row per elevation angle. Only meaningful when the section is
      // open and the source can sample at arbitrary heights.
      if (itsCrossActive && itsSource->hasNativeHeight())
      {
        itsCrossHeightAxis = !itsCrossHeightAxis;
        itsCrossTimeAxis = false;  // mutually exclusive with Hovmöller
        itsLastMessage = itsCrossHeightAxis ? "Cross-section: Y-axis = height (km)"
                                            : "Cross-section: Y-axis = elevation angle";
      }
      else if (itsCrossActive)
      {
        itsLastMessage = "Y-axis toggle: this source has no 3D height info";
      }
      return true;

    case 'H':
      // Hovmöller toggle: when the cross-section is open and the source has
      // multiple time steps, swap the chart's Y-axis from level/height to
      // time. The line is sampled at the current level across every time
      // step. Uppercase only — lowercase 'h' is the vim-style pan-left.
      if (itsCrossActive && itsSource->timeCount() > 1)
      {
        itsCrossTimeAxis = !itsCrossTimeAxis;
        itsLastMessage = itsCrossTimeAxis ? "Cross-section: Hovmöller (time vs distance)"
                                          : "Cross-section: level/height vs distance";
      }
      else if (itsCrossActive)
      {
        itsLastMessage = "Hovmöller toggle: this source has only one time step";
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

    case KEY_F(2):
      cyclePanelLayout();
      return true;

    case '\t':
      cycleActivePanel(+1);
      return true;
    case KEY_BTAB:
      cycleActivePanel(-1);
      return true;

    case '1':
    case '2':
    case '4':
      setActivePanel(key - '1');
      return true;

    case '3':
      // Toggle 3D point-cloud view. Active for PVOL polar volumes and for
      // QueryData sources that carry a height field (GeomHeight /
      // GeopHeight) alongside multiple levels. The renderer auto-reverts
      // to 2D for everything else.
      if (sourceSupports3D())
      {
        itsMode3D = !itsMode3D;
        if (itsMode3D)
        {
          apply3DDefaultsForSource();
          itsMode3DCurtain = false;  // mutually exclusive with mode 4
        }
        itsLastMessage =
            itsMode3D ? "3D: h/l yaw, j/k pitch, +/- zoom, 0 reset, ,/. threshold" : "3D mode off";
      }
      else
      {
        itsLastMessage = "3D mode: needs a polar volume or a multi-level QueryData file";
      }
      return true;

    case 'v':
    case 'V':
      // Toggle 3D + cross-section curtain (the "vertical slice" view).
      // `4` and `5` are taken by panel selection; `v` reads as "vertical
      // slice." Needs hasNativeHeight() so the curtain can be sampled via
      // interpolatedValueAtHeight(lat, lon, km).
      if (itsSource->hasNativeHeight())
      {
        itsMode3DCurtain = !itsMode3DCurtain;
        if (itsMode3DCurtain)
        {
          itsMode3D = false;  // mutually exclusive with the point-cloud mode
          apply3DDefaultsForSource();
          ensureCurtainEndpoints();
          itsCurtainAnimTickValid = false;  // first frame is the anchor
          itsLastMessage =
              "3D curtain: Tab cycles A/B/Both/View, arrows act on current sub-mode, "
              "+/- speed (Edit) or zoom (View), x/s/r/o/T toggle anims, "
              "PgUp/PgDn ceiling, 0 reset";
        }
        else
        {
          itsLastMessage = "3D curtain off";
        }
      }
      else
      {
        itsLastMessage = "3D curtain: source has no native height axis";
      }
      return true;

    case 'G':
      // Toggle the orthographic globe view. Available for any gridded
      // geographic source; centres on the data and resets zoom on entry.
      if (sourceSupportsGlobe())
      {
        itsModeGlobe = !itsModeGlobe;
        if (itsModeGlobe)
        {
          itsMode3D = false;  // mutually exclusive with the 3D point-cloud
          itsMode3DCurtain = false;  // and the curtain
          const auto bb = itsSource->boundingBox();
          constexpr double kLatLimit = M_PI_2 - 0.01;
          itsCamYaw = ((bb.minLon + bb.maxLon) * 0.5) * M_PI / 180.0;
          itsCamPitch =
              std::clamp(((bb.minLat + bb.maxLat) * 0.5) * M_PI / 180.0, -kLatLimit, kLatLimit);
          itsCamZoom = 1.0;
          itsLastMessage = "Globe: h/l spin, j/k tilt, +/- zoom, 0 recenter, G off";
        }
        else
        {
          itsLastMessage = "Globe off";
        }
      }
      else
      {
        itsLastMessage = "Globe view: needs a gridded geographic source";
      }
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
      if (getmouse(&ev) != OK)
        return false;

      // Optional debug log so we can inspect what the terminal actually
      // sends. Set QDLESS_DEBUG_MOUSE=1 in the environment to enable.
      static const bool kMouseDebug = std::getenv("QDLESS_DEBUG_MOUSE") != nullptr;
      if (kMouseDebug)
      {
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        FILE* f = std::fopen("/tmp/qdless-mouse.log", "a");
        if (f != nullptr)
        {
          fmt::print(f,
                     "mouse x={} y={} bstate=0x{:x}\n",
                     ev.x,
                     ev.y,
                     static_cast<unsigned long>(ev.bstate));
          std::fclose(f);
        }
      }

      const auto& l = ui.layout();
      const bool inMap = ev.x >= l.map.col && ev.x < l.map.col + l.map.width && ev.y >= l.map.row &&
                         ev.y < l.map.row + l.map.height;

      // Cross-section hover: when the mouse is over the chart area, project
      // the column → fraction → (lat, lon) along the great-circle and store
      // it as the hover dot. The dot is rendered by overlayCrossSection on
      // the next drawMap call. Leaving the chart clears the dot.
      if (itsCrossActive && itsCrossChartW > 0)
      {
        const bool inChart = ev.x >= itsCrossChartCol && ev.x < itsCrossChartCol + itsCrossChartW &&
                             ev.y >= itsCrossChartRow && ev.y < itsCrossChartRow + itsCrossChartH;
        std::optional<std::pair<double, double>> next;
        if (inChart)
        {
          const double frac = (static_cast<double>(ev.x - itsCrossChartCol) + 0.5) / itsCrossChartW;
          const double lat = itsCrossLat1 + frac * (itsCrossLat2 - itsCrossLat1);
          const double lon = itsCrossLon1 + frac * (itsCrossLon2 - itsCrossLon1);
          next = std::make_pair(lat, lon);
        }
        if (next != itsCrossHoverLatLon)
        {
          itsCrossHoverLatLon = next;
          return true;  // redraw to update the on-map dot
        }
      }

      // Any left-button click on a panel makes that panel active. Wheel /
      // motion events leave the focus alone.
      const auto kButton1Click =
          static_cast<mmask_t>(BUTTON1_PRESSED | BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED);
      if ((ev.bstate & kButton1Click) != 0U)
      {
        if (auto panel = panelAtCell(ui, ev.x, ev.y); panel.has_value())
          setActivePanel(*panel);
      }

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
        if (cellToViewport(ui, ev.x, ev.y, u, v))
          itsViewport.zoomAt(0.85F, u, v);
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
        if (cellToViewport(ui, ev.x, ev.y, u, v))
          itsViewport.zoomAt(0.7F, u, v);
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
        if (!itsDragging)
          return false;
        itsDragging = false;
        const int dx = ev.x - itsDragStartX;
        const int dy = ev.y - itsDragStartY;
        // < 2 cells of motion → click, not drag.
        if (std::abs(dx) + std::abs(dy) < 2)
        {
          // Route short PRESS+RELEASE pairs to the cross-section picker
          // when one is pending. Some terminals deliver PRESS/RELEASE
          // without a synthesised CLICKED, so the cross-section branch
          // below never fires for them and the probe popup wins instead.
          if (itsCrossPicks > 0)
          {
            if (itsCrossPicks == 2)
            {
              itsCrossX1 = itsDragStartX;
              itsCrossY1 = itsDragStartY;
              itsCrossPicks = 1;
              itsLastMessage = "Cross-section: click second endpoint";
            }
            else
            {
              beginCrossSection(itsCrossX1, itsCrossY1, itsDragStartX, itsDragStartY, ui);
              itsCrossPicks = 0;
              if (itsCrossActive)
                itsLastMessage.clear();
            }
            return true;
          }
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
          beginCrossSection(itsCrossX1, itsCrossY1, ev.x, ev.y, ui);
          itsCrossPicks = 0;
          if (itsCrossActive)
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
  if (itsModeGlobe)
  {
    if (sourceSupportsGlobe())
    {
      drawGlobe(ui.layout());
      return;
    }
    // Source can't be drawn on the globe (e.g. paged to an image in a
    // batch); silently fall back to the 2D map.
    itsModeGlobe = false;
  }
  if (itsMode3DCurtain)
  {
    if (itsSource->hasNativeHeight())
    {
      draw3DCrossSection(ui.layout());
      return;
    }
    // Source lost its height axis (e.g. user paged to a different file in
    // a batch); silently drop back to 2D rather than render nothing.
    itsMode3DCurtain = false;
  }
  if (itsMode3D)
  {
    if (dynamic_cast<const OdimVolumeSource*>(itsSource.get()) != nullptr)
    {
      draw3D(ui.layout());
      return;
    }
    if (const auto* qd = dynamic_cast<const QueryDataSource*>(itsSource.get()); qd != nullptr)
    {
      // Prefer the real volumetric path when the file has a height field
      // (hybrid / pressure levels). Fall back to the synthetic surface
      // stack for "flat" files that only carry surface params.
      if (qd->isVolumetric())
      {
        draw3DQueryData(ui.layout());
        return;
      }
      if (qd->isSurfaceStack())
      {
        draw3DSurfaceStack(ui.layout());
        return;
      }
    }
    // Drop back to 2D if the active source can't be drawn in 3D — keeps
    // the toggle safe across paging through a multi-file batch.
    itsMode3D = false;
  }

  const auto& l = ui.layout();
  if (l.map.height < 2 || l.map.width < 2)
    return;

  // Re-pick coastline resolution for the current viewport / map size.
  // Cheap when the selected file is unchanged; reads ~100ms at
  // h-resolution worst case. Sub-pixel dims use Braille's 2×4 grid —
  // the finest path the coastline can be drawn at — so the choice holds
  // up regardless of which line style is active.
  loadCoastlines(l.map.width * 2, l.map.height * 4);

  const auto rects = currentPanelRects(l.map.row, l.map.col, l.map.height, l.map.width);

  std::ostringstream os;
  const int savedActive = itsActivePanel;

  for (std::size_t i = 0; i < rects.size() && i < itsPanels.size(); ++i)
  {
    const PanelRect& r = rects[i];
    if (r.width < 2 || r.height < 2)
      continue;

    Panel& panel = itsPanels[i];
    if (panel.paramIndex >= 0 && panel.paramIndex < static_cast<int>(itsParamIds.size()))
      itsSource->selectParamId(itsParamIds[panel.paramIndex]);
    if (panel.levelIndex < itsSource->levelCount())
      itsSource->selectLevelIndex(panel.levelIndex);

    // sampleSlice() / transform() / overlays read activePanel(); make this
    // panel active for the duration of its render.
    itsActivePanel = static_cast<int>(i);

    // Sub-pixel grid: 2×subRows per cell in block-glyph mode; one sub-pixel
    // per terminal pixel in any graphics mode (so coastlines / data
    // sampling / overlays all upscale to the actual pixel grid naturally).
    const bool gfx = itsGraphicsMode != GraphicsMode::Block;
    const int subPerCellX = gfx ? itsCaps.cellPxW : 2;
    const int subPerCellY = gfx ? itsCaps.cellPxH : subRowsForStyle(itsCornerStyle);
    const int subW = r.width * subPerCellX;
    const int subH = r.height * subPerCellY;
    float dMin = 0;
    float dMax = 0;
    auto pixels = sampleSlice(subW, subH, dMin, dMax);
    // Thick mode rasterises into the data buffer before the renderer so the
    // line shows as a half-cell quadrant block. In graphics mode, braille
    // overlays would land on top of the pixel raster with the wrong grid
    // (braille glyphs use cell-positioned characters); promote any Braille
    // line style to Thick for the lifetime of this render so the lines
    // make it into the pixel pass.
    const LineStyle effGrat =
        gfx && itsGraticuleStyle == LineStyle::Braille ? LineStyle::Thick : itsGraticuleStyle;
    const LineStyle effCoast =
        gfx && itsCoastlineStyle == LineStyle::Braille ? LineStyle::Thick : itsCoastlineStyle;
    const LineStyle effBord =
        gfx && itsBorderStyle == LineStyle::Braille ? LineStyle::Thick : itsBorderStyle;
    const LineStyle effShape =
        gfx && itsShapeOutlineStyle == LineStyle::Braille ? LineStyle::Thick : itsShapeOutlineStyle;
    if (effGrat == LineStyle::Thick)
      overlayGraticule(pixels, subW, subH);
    if (effCoast == LineStyle::Thick)
      overlayPolylines(pixels, subW, subH, itsCoastlines, Rgb{0, 0, 0});
    if (effBord == LineStyle::Thick)
      overlayPolylines(pixels, subW, subH, itsBorders, Rgb{90, 90, 90});
    if (effShape == LineStyle::Thick)
      overlayPolylines(pixels, subW, subH, itsShapeOutlines, borderColor());
    overlayCities(pixels, subW, subH);
    overlayCrossSection(pixels, subW, subH);
    overlayMarker(pixels, subW, subH);

    switch (itsGraphicsMode)
    {
      case GraphicsMode::Kitty:
        renderKitty(os, pixels, subW, subH, r.row, r.col);
        break;
      case GraphicsMode::Sixel:
        renderSixel(os, pixels, subW, subH, r.row, r.col);
        break;
      case GraphicsMode::Block:
      default:
        itsRenderer.render(os, pixels, subW, subH, r.row, r.col);
        break;
    }

    // Braille mode draws on top of the rendered quadrant blocks so the
    // line is just a few dots wide; data colour shows through behind it.
    // Skipped when sixel promoted these to Thick above.
    if (effGrat == LineStyle::Braille)
      appendGraticuleBraille(os, pixels, subW, r.row, r.col);
    if (effCoast == LineStyle::Braille)
      appendPolylineBraille(os, itsCoastlines, Rgb{0, 0, 0}, pixels, subW, r.row, r.col);
    if (effBord == LineStyle::Braille)
      appendPolylineBraille(os, itsBorders, Rgb{90, 90, 90}, pixels, subW, r.row, r.col);
    if (effShape == LineStyle::Braille)
      appendPolylineBraille(os, itsShapeOutlines, borderColor(), pixels, subW, r.row, r.col);

    if (itsShowWindArrows)
      os << buildWindArrows(r.width, r.height, r.row, r.col);
    os << buildCityLabels(r.width, r.height, r.row, r.col);
  }

  // Restore source + active panel to user's selection so probe / legend /
  // cross-section / timeline see the right state.
  itsActivePanel = savedActive;
  if (activePanel().paramIndex >= 0 &&
      activePanel().paramIndex < static_cast<int>(itsParamIds.size()))
    itsSource->selectParamId(itsParamIds[activePanel().paramIndex]);
  if (activePanel().levelIndex < itsSource->levelCount())
    itsSource->selectLevelIndex(activePanel().levelIndex);

  // Separators between panels. Only drawn when the layout actually split.
  // Use one-eighth-block glyphs (▏ U+258F at the cell's left edge, ▔ U+2594
  // at the cell's top edge) for a much thinner border than ─/│ would give.
  // The two glyphs anchor to adjacent cell edges, so the Quad cross meets
  // cleanly when the vertical pass runs after the horizontal pass and wins
  // at the intersection cell.
  if (rects.size() > 1)
  {
    if (itsPanelLayout == PanelLayout::Side && rects.size() == 2)
    {
      const int gutterCol = rects[0].col + rects[0].width;
      appendSeparator(os, l.map.row, gutterCol, l.map.height, true, "\xe2\x96\x8f");
    }
    else if (itsPanelLayout == PanelLayout::Quad && rects.size() == 4)
    {
      const int gutterCol = rects[0].col + rects[0].width;
      const int gutterRow = rects[0].row + rects[0].height;
      appendSeparator(os, gutterRow, l.map.col, l.map.width, false, "\xe2\x96\x94");
      appendSeparator(os, l.map.row, gutterCol, l.map.height, true, "\xe2\x96\x8f");
    }

    // Per-panel labels: "[N] paramName" at top-left of each panel. The active
    // panel uses bold yellow; inactive panels use dim grey. Skipped for the
    // single-panel layout (no ambiguity).
    for (std::size_t i = 0; i < rects.size() && i < itsPanels.size(); ++i)
    {
      const auto& r = rects[i];
      if (r.width < 4 || r.height < 1)
        continue;
      const Panel& p = itsPanels[i];
      std::string name;
      if (p.paramIndex >= 0 && p.paramIndex < static_cast<int>(itsParamIds.size()))
        name = itsSource->paramShortName(itsParamIds[p.paramIndex]);
      const std::string label = "[" + std::to_string(i + 1) + "] " + name;
      const std::string visible = label.substr(0, static_cast<std::size_t>(r.width));
      const bool active = (static_cast<int>(i) == itsActivePanel);
      // Bold + bright yellow for active, dim white for inactive. Black bg
      // matches the map raster behind it.
      const char* style = active ? "\x1b[1m\x1b[40m\x1b[93m" : "\x1b[2m\x1b[40m\x1b[37m";
      os << "\x1b[" << (r.row + 1) << ';' << (r.col + 1) << 'H' << style << visible << "\x1b[0m";
    }
  }

  std::string s = os.str();
  std::fwrite(s.data(), 1, s.size(), stdout);
  std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// 3D point-cloud renderer for ODIM PVOL sources.
// ---------------------------------------------------------------------------
//
// Camera state: orbit-around-radar with (yaw, pitch, zoom).
//   yaw=0, pitch=0  → camera south of radar, looking north horizontally.
//   pitch=π/2       → camera directly above, looking straight down.
//
// Basis vectors (derived once per frame, used by every projection):
//   right   = ( cos ψ,           sin ψ,           0     )
//   up      = (-sin ψ · sin φ,   cos ψ · sin φ,   cos φ )
//   forward = (-sin ψ · cos φ,   cos ψ · cos φ,  -sin φ )
//
// Project world P to sub-pixel:
//   sx     = dot(P, right)    sy = dot(P, up)
//   depth  = dot(P, forward)  (smaller = closer to camera)
//   col    = subW/2 + xscale · sx
//   row    = subH/2 - yscale · sy        (terminal rows grow downward)
//
// The yscale carries the cell-aspect correction so a unit-circle in the
// world plane comes out as a circle (not an oval) on the terminal —
// terminal cells are ≈ 1:2 wide:tall, so a sub-pixel row covers more
// physical height than a sub-pixel column.
void App::draw3D(const Layout& layout)
{
  const auto* pvol = dynamic_cast<const OdimVolumeSource*>(itsSource.get());
  if (!pvol)
  {
    itsMode3D = false;
    return;
  }

  const auto& l = layout;
  if (l.map.height < 4 || l.map.width < 4)
    return;

  // The [c]/[b] handlers clear itsCoastlines / itsBorders when cycled
  // to "off" and clear the cached path when cycled back, expecting the
  // next drawMap to reload from disk. drawMap delegates to us in 3D
  // mode, so we have to drive that reload ourselves — otherwise the
  // map underlay vanishes after one c→off→on round-trip and never
  // comes back until the user toggles [3] back to 2D.
  loadCoastlines(l.map.width * 2, l.map.height * 4);

  // Single-panel only in 3D mode; ignores split layouts on purpose since
  // the same volume would just appear twice.
  const int cellW = l.map.width;
  const int cellH = l.map.height;

  // In 3D, demote Thick coastlines/borders to Braille — thick lines
  // crowd the volume; Braille keeps the geographic context without
  // clutter. Honours None (still hidden) and explicit Braille (still
  // Braille). The user can press [c]/[b] to cycle as usual.
  const LineStyle effCoast =
      (itsCoastlineStyle == LineStyle::Thick) ? LineStyle::Braille : itsCoastlineStyle;
  const LineStyle effBord =
      (itsBorderStyle == LineStyle::Thick) ? LineStyle::Braille : itsBorderStyle;
  const int subRows = subRowsForStyle(itsCornerStyle);
  const int subW = cellW * 2;
  const int subH = cellH * subRows;

  // Cell aspect: a terminal monospace cell is ~1:2 (width:height), and we
  // pack `subRows` sub-pixel rows into each cell-row. So one sub-pixel
  // row covers (cellH/subRows) physical pixels while one sub-pixel column
  // covers (cellW/2). Ratio (column-px / row-px) = subRows / 4 (assuming
  // 1:2 cell aspect). Scale screen_y by this so the picture stays round.
  const double aspect = static_cast<double>(subRows) / 4.0;

  // Camera basis.
  const double cy = std::cos(itsCamYaw);
  const double sy_ = std::sin(itsCamYaw);
  const double cp = std::cos(itsCamPitch);
  const double sp = std::sin(itsCamPitch);
  const double rightX = cy, rightY = sy_, rightZ = 0;
  const double upX = -sy_ * sp, upY = cy * sp, upZ = cp;
  const double fwdX = -sy_ * cp, fwdY = cy * cp, fwdZ = -sp;

  // Fit the radar's horizontal extent in the viewport at zoom=1.
  const double extent = pvol->maxRangeMeters();  // metres
  const double xscale = (subW / 2.0) / extent * itsCamZoom;
  const double yscale = xscale / aspect;
  const double depthScale = xscale;  // depth uses world metres; only the
                                     // ordering matters for the z-buffer

  // Buffers. INT_MAX z = unpainted.
  std::vector<Rgb> pixels(static_cast<std::size_t>(subW) * subH, Rgb{0, 0, 0, true});
  std::vector<float> zbuf(static_cast<std::size_t>(subW) * subH,
                          std::numeric_limits<float>::infinity());

  auto project = [&](double wx, double wy, double wz, double& col, double& row, float& depth)
  {
    const double sx = rightX * wx + rightY * wy + rightZ * wz;
    const double sy = upX * wx + upY * wy + upZ * wz;
    const double dp = fwdX * wx + fwdY * wy + fwdZ * wz;
    col = subW / 2.0 + xscale * sx;
    row = subH / 2.0 - yscale * sy;
    depth = static_cast<float>(dp * depthScale);
  };

  auto plot = [&](int c, int r, float depth, Rgb color)
  {
    if (c < 0 || c >= subW || r < 0 || r >= subH)
      return;
    const std::size_t idx = static_cast<std::size_t>(r) * subW + c;
    if (depth < zbuf[idx])
    {
      zbuf[idx] = depth;
      pixels[idx] = color;
    }
  };

  // DDA line in sub-pixel space with per-pixel z-buffer test. Used for
  // map polylines projected onto the ground plane.
  auto drawLine =
      [&](double wx0, double wy0, double wz0, double wx1, double wy1, double wz1, Rgb color)
  {
    double c0 = 0, r0 = 0, c1 = 0, r1 = 0;
    float d0 = 0, d1 = 0;
    project(wx0, wy0, wz0, c0, r0, d0);
    project(wx1, wy1, wz1, c1, r1, d1);
    const double dc = c1 - c0;
    const double dr = r1 - r0;
    const double dd = static_cast<double>(d1) - static_cast<double>(d0);
    const int steps = static_cast<int>(std::ceil(std::max(std::abs(dc), std::abs(dr))));
    if (steps <= 0)
    {
      plot(static_cast<int>(std::round(c0)), static_cast<int>(std::round(r0)), d0, color);
      return;
    }
    for (int i = 0; i <= steps; ++i)
    {
      const double t = static_cast<double>(i) / steps;
      const int c = static_cast<int>(std::round(c0 + t * dc));
      const int r = static_cast<int>(std::round(r0 + t * dr));
      const float d = static_cast<float>(d0 + t * dd);
      plot(c, r, d, color);
    }
  };

  // Flat-Earth (lat,lon) → (east,north) in metres, anchored at the radar.
  // Good to a few percent inside 250 km — adequate for the prototype.
  const auto [radarLat, radarLon] = pvol->radarLatLon();
  const double R_e = 6371000.0;
  const double cosLat0 = std::cos(radarLat * M_PI / 180.0);
  auto latLonToXY = [&](double lat, double lon, double& x, double& y)
  {
    x = (lon - radarLon) * (M_PI / 180.0) * R_e * cosLat0;
    y = (lat - radarLat) * (M_PI / 180.0) * R_e;
  };

  // Ground plane: coastlines + borders, drawn to the quadrant buffer for
  // Thick style (covers more area, easy to see at distance). Braille
  // style is handled later as an overlay after the main render so it
  // can use the finer 2×4 sub-cell grid the renderer can't reach.
  // Z = -1 (one metre below ground) so radar bins at h=0 still paint over.
  auto drawPolylinesThick = [&](const std::vector<Polyline>& polys, Rgb color)
  {
    for (const auto& p : polys)
    {
      if (p.lats.size() < 2)
        continue;
      double x0 = 0, y0 = 0;
      latLonToXY(p.lats[0], p.lons[0], x0, y0);
      for (std::size_t i = 1; i < p.lats.size(); ++i)
      {
        double x1 = 0, y1 = 0;
        latLonToXY(p.lats[i], p.lons[i], x1, y1);
        drawLine(x0, y0, -1, x1, y1, -1, color);
        x0 = x1;
        y0 = y1;
      }
    }
  };
  if (effCoast == LineStyle::Thick)
    drawPolylinesThick(itsCoastlines, Rgb{200, 200, 200});
  if (effBord == LineStyle::Thick)
    drawPolylinesThick(itsBorders, Rgb{120, 120, 120});

  // Range rings at 50, 100, 150, 200 km — visual anchor for distance.
  {
    constexpr int kSegs = 96;
    const Rgb ringColor{60, 100, 60};
    for (double r_km : {50.0, 100.0, 150.0, 200.0})
    {
      const double r_m = r_km * 1000.0;
      if (r_m > extent)
        continue;
      double prevX = r_m, prevY = 0;
      for (int i = 1; i <= kSegs; ++i)
      {
        const double a = 2.0 * M_PI * i / kSegs;
        const double x = r_m * std::cos(a);
        const double y = r_m * std::sin(a);
        drawLine(prevX, prevY, -1, x, y, -1, ringColor);
        prevX = x;
        prevY = y;
      }
    }
  }

  // Radar points. Iterate all bins; cheap raw threshold first to skip the
  // ~95 % clear-air cells before doing the gain/offset multiply and the
  // projection.
  const double R_eff = 4.0 / 3.0 * 6371000.0;
  const std::size_t nSweeps = pvol->sweepCount();
  for (std::size_t si = 0; si < nSweeps; ++si)
  {
    const auto s = pvol->sweepAt(si);
    if (!s.raw)
      continue;
    const double elRad = s.elangle * M_PI / 180.0;
    const double cosE = std::cos(elRad);
    const double sinE = std::sin(elRad);
    // Threshold in raw units. Solves itsThreshold3D = gain * raw + offset.
    const float rawThreshold = static_cast<float>((itsThreshold3D - s.offset) / s.gain);
    for (std::size_t ray = 0; ray < s.nrays; ++ray)
    {
      // Azimuth from north, clockwise. Same convention as sampleSweep.
      const double az = (ray + 0.5) * 2.0 * M_PI / s.nrays;
      const double sinAz = std::sin(az);
      const double cosAz = std::cos(az);
      const float* row = s.raw + ray * s.nbins;
      for (std::size_t bin = 0; bin < s.nbins; ++bin)
      {
        const float raw = row[bin];
        if (raw == static_cast<float>(s.nodata) || raw == static_cast<float>(s.undetect))
          continue;
        if (raw < rawThreshold)
          continue;
        const float value = static_cast<float>(s.gain * raw + s.offset);

        const double R = s.rstart + (bin + 0.5) * s.rscale;
        // Bin's 3D position: ground projection R·cos(α) along the ray,
        // height R·sin(α) plus 4/3-Earth curvature lift.
        const double rg = R * cosE;
        const double wx = rg * sinAz;  // east
        const double wy = rg * cosAz;  // north
        // Vertical exaggeration: storms are ~30:1 wide:tall in true
        // geometry; this stretches Z so the volume's depth structure
        // is legible without changing horizontal positions.
        const double wz = (R * sinE + R * R / (2.0 * R_eff)) * itsVexagger3D;

        double col = 0, rowSx = 0;
        float depth = 0;
        project(wx, wy, wz, col, rowSx, depth);
        // Splat each bin to a 2×2 sub-pixel block: a single sub-pixel
        // per bin makes the volume look sparse even with 500 radial
        // bins, because adjacent bins along a ray collapse onto the
        // same screen column at low elevations. The splat fills the
        // gaps without losing resolution on dense storms (the z-buffer
        // keeps the closest sample wherever they overlap).
        const Rgb color = activePanel().palette.lookup(transform(value));
        const int c = static_cast<int>(col);
        const int r = static_cast<int>(rowSx);
        plot(c, r, depth, color);
        plot(c + 1, r, depth, color);
        plot(c, r + 1, depth, color);
        plot(c + 1, r + 1, depth, color);
      }
    }
  }

  // Render to stdout.
  std::ostringstream os;
  cache3DRaster(pixels, subW, subH);
  itsRenderer.render(os, pixels, subW, subH, l.map.row, l.map.col);

  // Braille overlay for line styles set to Braille. Uses a finer 2×4
  // sub-cell mask than the renderer's quadrant grid, with the camera's
  // z-buffer (sampled at the nearest quadrant pixel) so radar bins
  // still occlude coastlines behind them.
  const bool brailleCoast = effCoast == LineStyle::Braille && !itsCoastlines.empty();
  const bool brailleBorder = effBord == LineStyle::Braille && !itsBorders.empty();
  if (brailleCoast || brailleBorder)
  {
    const int bW = cellW * 2;
    const int bH = cellH * 4;
    std::vector<unsigned char> dotMask(static_cast<std::size_t>(bW) * bH, 0);
    std::vector<Rgb> dotColor(static_cast<std::size_t>(bW) * bH, Rgb{0, 0, 0});

    // Braille-grid projection: same x, but y scaled by 4/subRows so the
    // same world coords land on the right braille row.
    const double yFineScale = 4.0 / subRows;
    auto projectB = [&](double wx, double wy, double wz, int& bx, int& by, float& depth)
    {
      const double sx = rightX * wx + rightY * wy + rightZ * wz;
      const double sy = upX * wx + upY * wy + upZ * wz;
      const double dp = fwdX * wx + fwdY * wy + fwdZ * wz;
      bx = static_cast<int>(std::round(bW / 2.0 + xscale * sx));
      by = static_cast<int>(std::round(bH / 2.0 - yscale * yFineScale * sy));
      depth = static_cast<float>(dp * depthScale);
    };

    auto plotB = [&](int bx, int by, float depth, Rgb color)
    {
      if (bx < 0 || bx >= bW || by < 0 || by >= bH)
        return;
      // Z-test against the quadrant z-buffer at the nearest sub-pixel.
      const int qy = by * subRows / 4;
      const std::size_t qidx = static_cast<std::size_t>(qy) * subW + bx;
      if (depth >= zbuf[qidx])
        return;
      const std::size_t bidx = static_cast<std::size_t>(by) * bW + bx;
      dotMask[bidx] = 1;
      dotColor[bidx] = color;
    };

    auto lineB =
        [&](double wx0, double wy0, double wz0, double wx1, double wy1, double wz1, Rgb color)
    {
      int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
      float d0 = 0, d1 = 0;
      projectB(wx0, wy0, wz0, x0, y0, d0);
      projectB(wx1, wy1, wz1, x1, y1, d1);
      const int adx = std::abs(x1 - x0);
      const int ady = std::abs(y1 - y0);
      const int steps = std::max(adx, ady);
      if (steps <= 0)
      {
        plotB(x0, y0, d0, color);
        return;
      }
      for (int i = 0; i <= steps; ++i)
      {
        const double t = static_cast<double>(i) / steps;
        const int cx = static_cast<int>(std::round(x0 + t * (x1 - x0)));
        const int cy = static_cast<int>(std::round(y0 + t * (y1 - y0)));
        const float dt = static_cast<float>(d0 + t * (d1 - d0));
        plotB(cx, cy, dt, color);
      }
    };

    auto drawPolyB = [&](const std::vector<Polyline>& polys, Rgb color)
    {
      for (const auto& p : polys)
      {
        if (p.lats.size() < 2)
          continue;
        double x0 = 0, y0 = 0;
        latLonToXY(p.lats[0], p.lons[0], x0, y0);
        for (std::size_t i = 1; i < p.lats.size(); ++i)
        {
          double x1 = 0, y1 = 0;
          latLonToXY(p.lats[i], p.lons[i], x1, y1);
          lineB(x0, y0, -1, x1, y1, -1, color);
          x0 = x1;
          y0 = y1;
        }
      }
    };

    if (brailleCoast)
      drawPolyB(itsCoastlines, Rgb{200, 200, 200});
    if (brailleBorder)
      drawPolyB(itsBorders, Rgb{120, 120, 120});

    // Emit positioned braille glyphs. BG sampled from the underlying
    // quadrant pixel so radar volume colours still show through.
    std::string brailleOut;
    for (int cy = 0; cy < cellH; ++cy)
    {
      for (int cx = 0; cx < cellW; ++cx)
      {
        unsigned cellMask = 0;
        Rgb cellFg{200, 200, 200};
        for (int sy = 0; sy < 4; ++sy)
        {
          for (int sx = 0; sx < 2; ++sx)
          {
            const int bx = cx * 2 + sx;
            const int by = cy * 4 + sy;
            const std::size_t bidx = static_cast<std::size_t>(by) * bW + bx;
            if (dotMask[bidx] != 0U)
            {
              cellMask |= 1U << brailleBit(sx, sy);
              cellFg = dotColor[bidx];
            }
          }
        }
        if (cellMask == 0U)
          continue;
        const int qy = cy * subRows;
        const Rgb bg = pixels[static_cast<std::size_t>(qy) * subW + cx * 2];
        brailleOut += "\x1b[";
        brailleOut += std::to_string(l.map.row + cy + 1);
        brailleOut += ';';
        brailleOut += std::to_string(l.map.col + cx + 1);
        brailleOut += 'H';
        brailleOut += itsRenderer.bgEscape(bg);
        brailleOut += itsRenderer.fgEscape(cellFg);
        brailleOut += brailleGlyph(cellMask);
      }
    }
    if (!brailleOut.empty())
    {
      os << brailleOut << "\x1b[0m";
    }
  }

  // Camera HUD bottom-right.
  const std::string hud =
      fmt::format(" 3D  yaw={:.0f}°  pitch={:.0f}°  zoom={:.2f}×  vex={:.0f}×  thresh={:.0f}{} ",
                  itsCamYaw * 180.0 / M_PI,
                  itsCamPitch * 180.0 / M_PI,
                  itsCamZoom,
                  itsVexagger3D,
                  itsThreshold3D,
                  itsThreshold3DUnit);
  const int hudRow = l.map.row + l.map.height - 1;
  const int hudCol =
      std::max(l.map.col, l.map.col + l.map.width - static_cast<int>(hud.size()) - 1);
  os << "\x1b[" << (hudRow + 1) << ';' << (hudCol + 1) << "H"
     << "\x1b[48;5;235m\x1b[38;5;15m" << hud << "\x1b[0m";

  const std::string s = os.str();
  std::fwrite(s.data(), 1, s.size(), stdout);
  std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// 3D point-cloud renderer for QueryData sources with a height field.
// ---------------------------------------------------------------------------
//
// Same camera + z-buffer + 2×2 splat as draw3D, but the point cloud
// comes from QueryDataSource::sampleVolume() which yields one tuple
// per (level, grid-cell) of the currently-active parameter, with the
// height read from GeomHeight (or GeopHeight/g). The world frame is a
// flat-Earth east/north plane anchored at the centre of the data's
// lat/lon bounding box — accurate to a few percent across the ~2000 km
// extent of typical NWP domains, which is good enough for an
// interactive viewer.
//
// Differences from the PVOL path:
//   - extent comes from boundingBox() not maxRangeMeters()
//   - threshold is in % (cloud cover) not dBZ
//   - default vexagger is much larger (~50×) because the troposphere
//     is two orders of magnitude thinner than NWP domains are wide
//   - the point cloud is dense at every cell (not sparse like radar
//     echoes), so threshold gating matters more for both clutter and
//     for keeping the inside of the cloud blob hidden by its near face
void App::draw3DQueryData(const Layout& layout)
{
  const auto* qd = dynamic_cast<const QueryDataSource*>(itsSource.get());
  if (qd == nullptr || !qd->isVolumetric())
  {
    itsMode3D = false;
    return;
  }

  const auto& l = layout;
  if (l.map.height < 4 || l.map.width < 4)
    return;

  // Same coastline reload trick as draw3D — see comment there.
  loadCoastlines(l.map.width * 2, l.map.height * 4);

  const int cellW = l.map.width;
  const int cellH = l.map.height;
  const int subRows = subRowsForStyle(itsCornerStyle);
  const int subW = cellW * 2;
  const int subH = cellH * subRows;
  const double aspect = static_cast<double>(subRows) / 4.0;

  // Demote Thick → Braille in 3D so thick lines don't crowd the volume.
  const LineStyle effCoast =
      (itsCoastlineStyle == LineStyle::Thick) ? LineStyle::Braille : itsCoastlineStyle;
  const LineStyle effBord =
      (itsBorderStyle == LineStyle::Thick) ? LineStyle::Braille : itsBorderStyle;

  // Camera basis (copied verbatim from draw3D — same maths).
  const double cy = std::cos(itsCamYaw);
  const double sy_ = std::sin(itsCamYaw);
  const double cp = std::cos(itsCamPitch);
  const double sp = std::sin(itsCamPitch);
  const double rightX = cy, rightY = sy_, rightZ = 0;
  const double upX = -sy_ * sp, upY = cy * sp, upZ = cp;
  const double fwdX = -sy_ * cp, fwdY = cy * cp, fwdZ = -sp;

  // Bbox-centred flat-Earth frame. extent = half-width of the larger
  // axis (in metres) so the data fits the viewport at zoom=1 with the
  // smaller axis under-using the screen — same convention as the PVOL
  // path where `extent = maxRangeMeters()` is the data's outer radius.
  const auto bb = itsSource->boundingBox();
  const double lat0 = (bb.minLat + bb.maxLat) * 0.5;
  const double lon0 = (bb.minLon + bb.maxLon) * 0.5;
  constexpr double R_e = 6371000.0;
  const double cosLat0 = std::cos(lat0 * M_PI / 180.0);
  const double extentX = (bb.maxLon - bb.minLon) * (M_PI / 180.0) * R_e * cosLat0 * 0.5;
  const double extentY = (bb.maxLat - bb.minLat) * (M_PI / 180.0) * R_e * 0.5;
  const double extent = std::max(extentX, extentY);
  if (extent <= 0.0)
    return;

  const double xscale = (subW / 2.0) / extent * itsCamZoom;
  const double yscale = xscale / aspect;
  const double depthScale = xscale;

  std::vector<Rgb> pixels(static_cast<std::size_t>(subW) * subH, Rgb{0, 0, 0, true});
  std::vector<float> zbuf(static_cast<std::size_t>(subW) * subH,
                          std::numeric_limits<float>::infinity());

  auto project = [&](double wx, double wy, double wz, double& col, double& row, float& depth)
  {
    const double sx = rightX * wx + rightY * wy + rightZ * wz;
    const double sy = upX * wx + upY * wy + upZ * wz;
    const double dp = fwdX * wx + fwdY * wy + fwdZ * wz;
    col = subW / 2.0 + xscale * sx;
    row = subH / 2.0 - yscale * sy;
    depth = static_cast<float>(dp * depthScale);
  };

  auto plot = [&](int c, int r, float depth, Rgb color)
  {
    if (c < 0 || c >= subW || r < 0 || r >= subH)
      return;
    const std::size_t idx = static_cast<std::size_t>(r) * subW + c;
    if (depth < zbuf[idx])
    {
      zbuf[idx] = depth;
      pixels[idx] = color;
    }
  };

  auto drawLine =
      [&](double wx0, double wy0, double wz0, double wx1, double wy1, double wz1, Rgb color)
  {
    double c0 = 0, r0 = 0, c1 = 0, r1 = 0;
    float d0 = 0, d1 = 0;
    project(wx0, wy0, wz0, c0, r0, d0);
    project(wx1, wy1, wz1, c1, r1, d1);
    const double dc = c1 - c0;
    const double dr = r1 - r0;
    const double dd = static_cast<double>(d1) - static_cast<double>(d0);
    const int steps = static_cast<int>(std::ceil(std::max(std::abs(dc), std::abs(dr))));
    if (steps <= 0)
    {
      plot(static_cast<int>(std::round(c0)), static_cast<int>(std::round(r0)), d0, color);
      return;
    }
    for (int i = 0; i <= steps; ++i)
    {
      const double t = static_cast<double>(i) / steps;
      const int c = static_cast<int>(std::round(c0 + t * dc));
      const int r = static_cast<int>(std::round(r0 + t * dr));
      const float d = static_cast<float>(d0 + t * dd);
      plot(c, r, d, color);
    }
  };

  // Flat-Earth lat/lon → east/north metres, anchored at the bbox centre.
  auto latLonToXY = [&](double lat, double lon, double& x, double& y)
  {
    x = (lon - lon0) * (M_PI / 180.0) * R_e * cosLat0;
    y = (lat - lat0) * (M_PI / 180.0) * R_e;
  };

  auto drawPolylinesThick = [&](const std::vector<Polyline>& polys, Rgb color)
  {
    for (const auto& p : polys)
    {
      if (p.lats.size() < 2)
        continue;
      double x0 = 0, y0 = 0;
      latLonToXY(p.lats[0], p.lons[0], x0, y0);
      for (std::size_t i = 1; i < p.lats.size(); ++i)
      {
        double x1 = 0, y1 = 0;
        latLonToXY(p.lats[i], p.lons[i], x1, y1);
        drawLine(x0, y0, -1, x1, y1, -1, color);
        x0 = x1;
        y0 = y1;
      }
    }
  };
  if (effCoast == LineStyle::Thick)
    drawPolylinesThick(itsCoastlines, Rgb{200, 200, 200});
  if (effBord == LineStyle::Thick)
    drawPolylinesThick(itsBorders, Rgb{120, 120, 120});

  const double vexagger = itsVexagger3D;

  if (!itsShowExtrema)
  {
    // Volume points. The sampler walks every (level, grid-cell) of the
    // active parameter; we threshold and splat in the camera frame. The
    // sampler restores info state on return, so the cb is the only place
    // we touch per-point work — keep it lean.
    const float threshold = itsThreshold3D;
    qd->sampleVolume(
        [&](const QueryDataSource::VolumeSample& s)
        {
          if (!std::isfinite(s.value) || s.value < threshold)
            return;
          double wx = 0, wy = 0;
          latLonToXY(s.lat, s.lon, wx, wy);
          const double wz = s.heightMeters * vexagger;
          double col = 0, rowSx = 0;
          float depth = 0;
          project(wx, wy, wz, col, rowSx, depth);
          const Rgb color = activePanel().palette.lookup(transform(s.value));
          const int c = static_cast<int>(col);
          const int r = static_cast<int>(rowSx);
          plot(c, r, depth, color);
          plot(c + 1, r, depth, color);
          plot(c, r + 1, depth, color);
          plot(c + 1, r + 1, depth, color);
        });
  }
  else
  {
    // Persistent-extrema view: instead of the full cloud, draw only the
    // most persistent anomaly "air masses" — each merge-tree blob as a solid
    // palette-coloured mass (so the data still drives the colour), with a
    // bright vertical stem from the ground to the peak and a marker at the
    // extremum, giving the cross-section-style read the user asked for.
    ensureExtremaCache();
    const VolumeGrid& g = itsExtremaGrid;
    const std::size_t slice = g.sliceCount();
    if (slice > 0 && g.heights.size() == g.cellCount())
    {
      auto cellWorld = [&](std::size_t flat, double& wx, double& wy, double& wz)
      {
        const std::size_t s = flat % slice;
        latLonToXY(g.lats[s], g.lons[s], wx, wy);
        wz = static_cast<double>(g.heights[flat]) * vexagger;
      };
      for (const auto& f : itsExtremaFeatures)
      {
        // The blob mass: a 2×2 splat per cell, palette-coloured by the raw
        // value; the z-buffer makes it read as a solid body from any angle.
        for (const std::size_t flat : f.blob)
        {
          double wx = 0, wy = 0, wz = 0;
          cellWorld(flat, wx, wy, wz);
          double col = 0, row = 0;
          float depth = 0;
          project(wx, wy, wz, col, row, depth);
          const float v = g.values[flat];
          const Rgb color = std::isfinite(v) ? activePanel().palette.lookup(transform(v))
                                             : Rgb{160, 160, 160};
          const int c = static_cast<int>(col);
          const int r = static_cast<int>(row);
          plot(c, r, depth, color);
          plot(c + 1, r, depth, color);
          plot(c, r + 1, depth, color);
          plot(c + 1, r + 1, depth, color);
        }
        // Bright stem + marker at the peak (warm = max, cool = min).
        const Rgb tint = (f.kind == ExtremumKind::Max) ? Rgb{255, 90, 80} : Rgb{90, 170, 255};
        double px = 0, py = 0, pz = 0;
        cellWorld(f.peakCell, px, py, pz);
        drawLine(px, py, 0.0, px, py, pz, tint);
        double mc = 0, mr = 0;
        float md = 0;
        project(px, py, pz, mc, mr, md);
        const int cc = static_cast<int>(mc);
        const int rr = static_cast<int>(mr);
        for (int dy = -1; dy <= 1; ++dy)
          for (int dx = -1; dx <= 1; ++dx)
            plot(cc + dx, rr + dy, md - 1.0F, tint);  // bias toward camera so it stays visible
      }
    }
  }

  std::ostringstream os;
  cache3DRaster(pixels, subW, subH);
  itsRenderer.render(os, pixels, subW, subH, l.map.row, l.map.col);

  // Braille overlay for coastlines / borders — identical mechanics to
  // draw3D, just with the bbox-centred latLonToXY plugged in.
  const bool brailleCoast = effCoast == LineStyle::Braille && !itsCoastlines.empty();
  const bool brailleBorder = effBord == LineStyle::Braille && !itsBorders.empty();
  if (brailleCoast || brailleBorder)
  {
    const int bW = cellW * 2;
    const int bH = cellH * 4;
    std::vector<unsigned char> dotMask(static_cast<std::size_t>(bW) * bH, 0);
    std::vector<Rgb> dotColor(static_cast<std::size_t>(bW) * bH, Rgb{0, 0, 0});
    const double yFineScale = 4.0 / subRows;
    auto projectB = [&](double wx, double wy, double wz, int& bx, int& by, float& depth)
    {
      const double sx = rightX * wx + rightY * wy + rightZ * wz;
      const double sy = upX * wx + upY * wy + upZ * wz;
      const double dp = fwdX * wx + fwdY * wy + fwdZ * wz;
      bx = static_cast<int>(std::round(bW / 2.0 + xscale * sx));
      by = static_cast<int>(std::round(bH / 2.0 - yscale * yFineScale * sy));
      depth = static_cast<float>(dp * depthScale);
    };
    auto plotB = [&](int bx, int by, float depth, Rgb color)
    {
      if (bx < 0 || bx >= bW || by < 0 || by >= bH)
        return;
      const int qy = by * subRows / 4;
      const std::size_t qidx = static_cast<std::size_t>(qy) * subW + bx;
      if (depth >= zbuf[qidx])
        return;
      const std::size_t bidx = static_cast<std::size_t>(by) * bW + bx;
      dotMask[bidx] = 1;
      dotColor[bidx] = color;
    };
    auto lineB =
        [&](double wx0, double wy0, double wz0, double wx1, double wy1, double wz1, Rgb color)
    {
      int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
      float d0 = 0, d1 = 0;
      projectB(wx0, wy0, wz0, x0, y0, d0);
      projectB(wx1, wy1, wz1, x1, y1, d1);
      const int adx = std::abs(x1 - x0);
      const int ady = std::abs(y1 - y0);
      const int steps = std::max(adx, ady);
      if (steps <= 0)
      {
        plotB(x0, y0, d0, color);
        return;
      }
      for (int i = 0; i <= steps; ++i)
      {
        const double t = static_cast<double>(i) / steps;
        const int cx = static_cast<int>(std::round(x0 + t * (x1 - x0)));
        const int cy_ = static_cast<int>(std::round(y0 + t * (y1 - y0)));
        const float dt = static_cast<float>(d0 + t * (d1 - d0));
        plotB(cx, cy_, dt, color);
      }
    };
    auto drawPolyB = [&](const std::vector<Polyline>& polys, Rgb color)
    {
      for (const auto& p : polys)
      {
        if (p.lats.size() < 2)
          continue;
        double x0 = 0, y0 = 0;
        latLonToXY(p.lats[0], p.lons[0], x0, y0);
        for (std::size_t i = 1; i < p.lats.size(); ++i)
        {
          double x1 = 0, y1 = 0;
          latLonToXY(p.lats[i], p.lons[i], x1, y1);
          lineB(x0, y0, -1, x1, y1, -1, color);
          x0 = x1;
          y0 = y1;
        }
      }
    };
    if (brailleCoast)
      drawPolyB(itsCoastlines, Rgb{200, 200, 200});
    if (brailleBorder)
      drawPolyB(itsBorders, Rgb{120, 120, 120});

    std::string brailleOut;
    for (int cy_ = 0; cy_ < cellH; ++cy_)
    {
      for (int cx = 0; cx < cellW; ++cx)
      {
        unsigned cellMask = 0;
        Rgb cellFg{200, 200, 200};
        for (int sy = 0; sy < 4; ++sy)
        {
          for (int sx = 0; sx < 2; ++sx)
          {
            const int bx = cx * 2 + sx;
            const int by = cy_ * 4 + sy;
            const std::size_t bidx = static_cast<std::size_t>(by) * bW + bx;
            if (dotMask[bidx] != 0U)
            {
              cellMask |= 1U << brailleBit(sx, sy);
              cellFg = dotColor[bidx];
            }
          }
        }
        if (cellMask == 0U)
          continue;
        const int qy = cy_ * subRows;
        const Rgb bg = pixels[static_cast<std::size_t>(qy) * subW + cx * 2];
        brailleOut += "\x1b[";
        brailleOut += std::to_string(l.map.row + cy_ + 1);
        brailleOut += ';';
        brailleOut += std::to_string(l.map.col + cx + 1);
        brailleOut += 'H';
        brailleOut += itsRenderer.bgEscape(bg);
        brailleOut += itsRenderer.fgEscape(cellFg);
        brailleOut += brailleGlyph(cellMask);
      }
    }
    if (!brailleOut.empty())
      os << brailleOut << "\x1b[0m";
  }

  const std::string hud =
      itsShowExtrema
          ? fmt::format(" 3D  yaw={:.0f}°  pitch={:.0f}°  zoom={:.2f}×  vex={:.0f}×  "
                        "extrema={} masses  [x] cloud ",
                        itsCamYaw * 180.0 / M_PI,
                        itsCamPitch * 180.0 / M_PI,
                        itsCamZoom,
                        itsVexagger3D,
                        itsExtremaFeatures.size())
          : fmt::format(" 3D  yaw={:.0f}°  pitch={:.0f}°  zoom={:.2f}×  vex={:.0f}×  thresh={:.0f}{} ",
                        itsCamYaw * 180.0 / M_PI,
                        itsCamPitch * 180.0 / M_PI,
                        itsCamZoom,
                        itsVexagger3D,
                        itsThreshold3D,
                        itsThreshold3DUnit);
  const int hudRow = l.map.row + l.map.height - 1;
  const int hudCol =
      std::max(l.map.col, l.map.col + l.map.width - static_cast<int>(hud.size()) - 1);
  os << "\x1b[" << (hudRow + 1) << ';' << (hudCol + 1) << "H"
     << "\x1b[48;5;235m\x1b[38;5;15m" << hud << "\x1b[0m";

  const std::string s = os.str();
  std::fwrite(s.data(), 1, s.size(), stdout);
  std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// Surface-stack 3D renderer for QueryData files without a real vertical axis.
// ---------------------------------------------------------------------------
//
// fmi.sqd carries Precipitation1h, FogIntensity, LowCloudCover,
// MediumCloudCover, HighCloudCover as separate "surface" parameters with
// no per-cell height. We stack them at canonical heights so the user
// gets a cartoon 3D weather picture — enough to read where the
// precipitation column is, where the fog lies, and how the cloud decks
// stack vertically. Same camera / z-buffer / splat pipeline as the real
// volumetric path, but the point cloud is much sparser (5 layers × grid).
//
// itsThreshold3D acts on the cloud-cover layers only; precipitation and
// fog use their own fixed thresholds (mm/h and index value respectively)
// because the , / . keys would otherwise wipe out one of the three
// fundamentally-different-unit signals.
void App::draw3DSurfaceStack(const Layout& layout)
{
  const auto* qd = dynamic_cast<const QueryDataSource*>(itsSource.get());
  if (qd == nullptr || !qd->isSurfaceStack())
  {
    itsMode3D = false;
    return;
  }

  const auto& l = layout;
  if (l.map.height < 4 || l.map.width < 4)
    return;
  loadCoastlines(l.map.width * 2, l.map.height * 4);

  const int cellW = l.map.width;
  const int cellH = l.map.height;
  const int subRows = subRowsForStyle(itsCornerStyle);
  const int subW = cellW * 2;
  const int subH = cellH * subRows;
  const double aspect = static_cast<double>(subRows) / 4.0;

  // Demote Thick → Braille in 3D so thick lines don't crowd the stack.
  const LineStyle effCoast =
      (itsCoastlineStyle == LineStyle::Thick) ? LineStyle::Braille : itsCoastlineStyle;
  const LineStyle effBord =
      (itsBorderStyle == LineStyle::Thick) ? LineStyle::Braille : itsBorderStyle;

  // Camera basis.
  const double cy = std::cos(itsCamYaw);
  const double sy_ = std::sin(itsCamYaw);
  const double cp = std::cos(itsCamPitch);
  const double sp = std::sin(itsCamPitch);
  const double rightX = cy, rightY = sy_, rightZ = 0;
  const double upX = -sy_ * sp, upY = cy * sp, upZ = cp;
  const double fwdX = -sy_ * cp, fwdY = cy * cp, fwdZ = -sp;

  const auto bb = itsSource->boundingBox();
  const double lat0 = (bb.minLat + bb.maxLat) * 0.5;
  const double lon0 = (bb.minLon + bb.maxLon) * 0.5;
  constexpr double R_e = 6371000.0;
  const double cosLat0 = std::cos(lat0 * M_PI / 180.0);
  const double extentX = (bb.maxLon - bb.minLon) * (M_PI / 180.0) * R_e * cosLat0 * 0.5;
  const double extentY = (bb.maxLat - bb.minLat) * (M_PI / 180.0) * R_e * 0.5;
  const double extent = std::max(extentX, extentY);
  if (extent <= 0.0)
    return;

  const double xscale = (subW / 2.0) / extent * itsCamZoom;
  const double yscale = xscale / aspect;
  const double depthScale = xscale;

  std::vector<Rgb> pixels(static_cast<std::size_t>(subW) * subH, Rgb{0, 0, 0, true});
  std::vector<float> zbuf(static_cast<std::size_t>(subW) * subH,
                          std::numeric_limits<float>::infinity());

  auto project = [&](double wx, double wy, double wz, double& col, double& row, float& depth)
  {
    const double sx = rightX * wx + rightY * wy + rightZ * wz;
    const double sy = upX * wx + upY * wy + upZ * wz;
    const double dp = fwdX * wx + fwdY * wy + fwdZ * wz;
    col = subW / 2.0 + xscale * sx;
    row = subH / 2.0 - yscale * sy;
    depth = static_cast<float>(dp * depthScale);
  };
  auto plot = [&](int c, int r, float depth, Rgb color)
  {
    if (c < 0 || c >= subW || r < 0 || r >= subH)
      return;
    const std::size_t idx = static_cast<std::size_t>(r) * subW + c;
    if (depth < zbuf[idx])
    {
      zbuf[idx] = depth;
      pixels[idx] = color;
    }
  };
  auto drawLine =
      [&](double wx0, double wy0, double wz0, double wx1, double wy1, double wz1, Rgb color)
  {
    double c0 = 0, r0 = 0, c1 = 0, r1 = 0;
    float d0 = 0, d1 = 0;
    project(wx0, wy0, wz0, c0, r0, d0);
    project(wx1, wy1, wz1, c1, r1, d1);
    const double dc = c1 - c0, dr = r1 - r0;
    const double dd = static_cast<double>(d1) - static_cast<double>(d0);
    const int steps = static_cast<int>(std::ceil(std::max(std::abs(dc), std::abs(dr))));
    if (steps <= 0)
    {
      plot(static_cast<int>(std::round(c0)), static_cast<int>(std::round(r0)), d0, color);
      return;
    }
    for (int i = 0; i <= steps; ++i)
    {
      const double t = static_cast<double>(i) / steps;
      plot(static_cast<int>(std::round(c0 + t * dc)),
           static_cast<int>(std::round(r0 + t * dr)),
           static_cast<float>(d0 + t * dd),
           color);
    }
  };
  auto latLonToXY = [&](double lat, double lon, double& x, double& y)
  {
    x = (lon - lon0) * (M_PI / 180.0) * R_e * cosLat0;
    y = (lat - lat0) * (M_PI / 180.0) * R_e;
  };
  auto drawPolylinesThick = [&](const std::vector<Polyline>& polys, Rgb color)
  {
    for (const auto& p : polys)
    {
      if (p.lats.size() < 2)
        continue;
      double x0 = 0, y0 = 0;
      latLonToXY(p.lats[0], p.lons[0], x0, y0);
      for (std::size_t i = 1; i < p.lats.size(); ++i)
      {
        double x1 = 0, y1 = 0;
        latLonToXY(p.lats[i], p.lons[i], x1, y1);
        drawLine(x0, y0, -1, x1, y1, -1, color);
        x0 = x1;
        y0 = y1;
      }
    }
  };
  if (effCoast == LineStyle::Thick)
    drawPolylinesThick(itsCoastlines, Rgb{200, 200, 200});
  if (effBord == LineStyle::Thick)
    drawPolylinesThick(itsBorders, Rgb{120, 120, 120});

  // Layer table. Heights are canonical mid-of-deck values:
  //   precipitation column starts at ground;
  //   fog hugs the surface (raised slightly so it isn't z-fought by precip);
  //   low cloud  ~ 1.5 km (cloud base 0..2 km);
  //   medium     ~ 3.5 km (2..6 km);
  //   high       ~ 8 km   (6..12 km, mostly cirrus).
  //
  // Each layer pulls its own palette from qdless.conf via paletteForParam
  // and falls back to a sensible builtin if unavailable. The cloud-cover
  // layers share one threshold (itsThreshold3D); precipitation and fog
  // have parameter-specific thresholds because their units aren't %.
  struct Layer
  {
    FmiParameterName paramId;
    const char* shortName;
    double heightKm;
    float threshold;          // value below this is skipped
    const char* paletteName;  // override for stack viewing
    Rgb fallback;             // tint when palette can't be loaded
  };
  const float cloudThreshold = itsThreshold3D;
  const std::array<Layer, 5> layers{{
      {kFmiPrecipitation1h, "Precipitation1h", 0.0, 0.1F, "precipitation1h", Rgb{80, 120, 200}},
      {kFmiFogIntensity, "FogIntensity", 0.1, 0.5F, "fog_intensity", Rgb{200, 200, 100}},
      {kFmiLowCloudCover,
       "LowCloudCover",
       1.5,
       cloudThreshold,
       "totalcloudcover_color",
       Rgb{220, 220, 220}},
      {kFmiMediumCloudCover,
       "MediumCloudCover",
       3.5,
       cloudThreshold,
       "totalcloudcover_color",
       Rgb{200, 200, 220}},
      {kFmiHighCloudCover,
       "HighCloudCover",
       8.0,
       cloudThreshold,
       "totalcloudcover_color",
       Rgb{180, 200, 240}},
  }};

  const double vexagger = itsVexagger3D;
  // Convert km → metres → exaggerated metres. Done per-layer (constant
  // across all cells of that layer) so each sample is just a multiply.
  for (const auto& layer : layers)
  {
    auto pal = loadPaletteByName(layer.paletteName);
    const double wz = layer.heightKm * 1000.0 * vexagger;
    qd->sampleSlab(layer.paramId,
                   [&](double lat, double lon, float v)
                   {
                     if (!std::isfinite(v) || v < layer.threshold)
                       return;
                     double wx = 0, wy = 0;
                     latLonToXY(lat, lon, wx, wy);
                     double col = 0, rowSx = 0;
                     float depth = 0;
                     project(wx, wy, wz, col, rowSx, depth);
                     const Rgb color = pal ? pal->lookup(v) : layer.fallback;
                     const int c = static_cast<int>(col);
                     const int r = static_cast<int>(rowSx);
                     plot(c, r, depth, color);
                     plot(c + 1, r, depth, color);
                     plot(c, r + 1, depth, color);
                     plot(c + 1, r + 1, depth, color);
                   });
  }

  std::ostringstream os;
  cache3DRaster(pixels, subW, subH);
  itsRenderer.render(os, pixels, subW, subH, l.map.row, l.map.col);

  // Coastline / border braille overlay — same shape as the other 3D
  // paths. Pulled out as a lambda over the per-renderer scaling so the
  // diff with draw3DQueryData stays obvious.
  const bool brailleCoast = effCoast == LineStyle::Braille && !itsCoastlines.empty();
  const bool brailleBorder = effBord == LineStyle::Braille && !itsBorders.empty();
  if (brailleCoast || brailleBorder)
  {
    const int bW = cellW * 2;
    const int bH = cellH * 4;
    std::vector<unsigned char> dotMask(static_cast<std::size_t>(bW) * bH, 0);
    std::vector<Rgb> dotColor(static_cast<std::size_t>(bW) * bH, Rgb{0, 0, 0});
    const double yFineScale = 4.0 / subRows;
    auto projectB = [&](double wx, double wy, double wz, int& bx, int& by, float& depth)
    {
      const double sx = rightX * wx + rightY * wy + rightZ * wz;
      const double sy = upX * wx + upY * wy + upZ * wz;
      const double dp = fwdX * wx + fwdY * wy + fwdZ * wz;
      bx = static_cast<int>(std::round(bW / 2.0 + xscale * sx));
      by = static_cast<int>(std::round(bH / 2.0 - yscale * yFineScale * sy));
      depth = static_cast<float>(dp * depthScale);
    };
    auto plotB = [&](int bx, int by, float depth, Rgb color)
    {
      if (bx < 0 || bx >= bW || by < 0 || by >= bH)
        return;
      const int qy = by * subRows / 4;
      const std::size_t qidx = static_cast<std::size_t>(qy) * subW + bx;
      if (depth >= zbuf[qidx])
        return;
      const std::size_t bidx = static_cast<std::size_t>(by) * bW + bx;
      dotMask[bidx] = 1;
      dotColor[bidx] = color;
    };
    auto lineB =
        [&](double wx0, double wy0, double wz0, double wx1, double wy1, double wz1, Rgb color)
    {
      int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
      float d0 = 0, d1 = 0;
      projectB(wx0, wy0, wz0, x0, y0, d0);
      projectB(wx1, wy1, wz1, x1, y1, d1);
      const int adx = std::abs(x1 - x0), ady = std::abs(y1 - y0);
      const int steps = std::max(adx, ady);
      if (steps <= 0)
      {
        plotB(x0, y0, d0, color);
        return;
      }
      for (int i = 0; i <= steps; ++i)
      {
        const double t = static_cast<double>(i) / steps;
        plotB(static_cast<int>(std::round(x0 + t * (x1 - x0))),
              static_cast<int>(std::round(y0 + t * (y1 - y0))),
              static_cast<float>(d0 + t * (d1 - d0)),
              color);
      }
    };
    auto drawPolyB = [&](const std::vector<Polyline>& polys, Rgb color)
    {
      for (const auto& p : polys)
      {
        if (p.lats.size() < 2)
          continue;
        double x0 = 0, y0 = 0;
        latLonToXY(p.lats[0], p.lons[0], x0, y0);
        for (std::size_t i = 1; i < p.lats.size(); ++i)
        {
          double x1 = 0, y1 = 0;
          latLonToXY(p.lats[i], p.lons[i], x1, y1);
          lineB(x0, y0, -1, x1, y1, -1, color);
          x0 = x1;
          y0 = y1;
        }
      }
    };
    if (brailleCoast)
      drawPolyB(itsCoastlines, Rgb{200, 200, 200});
    if (brailleBorder)
      drawPolyB(itsBorders, Rgb{120, 120, 120});

    std::string brailleOut;
    for (int cy_ = 0; cy_ < cellH; ++cy_)
    {
      for (int cx = 0; cx < cellW; ++cx)
      {
        unsigned cellMask = 0;
        Rgb cellFg{200, 200, 200};
        for (int sy = 0; sy < 4; ++sy)
          for (int sx = 0; sx < 2; ++sx)
          {
            const int bx = cx * 2 + sx;
            const int by = cy_ * 4 + sy;
            const std::size_t bidx = static_cast<std::size_t>(by) * bW + bx;
            if (dotMask[bidx] != 0U)
            {
              cellMask |= 1U << brailleBit(sx, sy);
              cellFg = dotColor[bidx];
            }
          }
        if (cellMask == 0U)
          continue;
        const int qy = cy_ * subRows;
        const Rgb bg = pixels[static_cast<std::size_t>(qy) * subW + cx * 2];
        brailleOut += "\x1b[";
        brailleOut += std::to_string(l.map.row + cy_ + 1);
        brailleOut += ';';
        brailleOut += std::to_string(l.map.col + cx + 1);
        brailleOut += 'H';
        brailleOut += itsRenderer.bgEscape(bg);
        brailleOut += itsRenderer.fgEscape(cellFg);
        brailleOut += brailleGlyph(cellMask);
      }
    }
    if (!brailleOut.empty())
      os << brailleOut << "\x1b[0m";
  }

  const std::string hud = fmt::format(
      " 3D stack  yaw={:.0f}°  pitch={:.0f}°  zoom={:.2f}×  vex={:.0f}×  "
      "cloud≥{:.0f}{} ",
      itsCamYaw * 180.0 / M_PI,
      itsCamPitch * 180.0 / M_PI,
      itsCamZoom,
      itsVexagger3D,
      itsThreshold3D,
      itsThreshold3DUnit);
  const int hudRow = l.map.row + l.map.height - 1;
  const int hudCol =
      std::max(l.map.col, l.map.col + l.map.width - static_cast<int>(hud.size()) - 1);
  os << "\x1b[" << (hudRow + 1) << ';' << (hudCol + 1) << "H"
     << "\x1b[48;5;235m\x1b[38;5;15m" << hud << "\x1b[0m";

  const std::string s = os.str();
  std::fwrite(s.data(), 1, s.size(), stdout);
  std::fflush(stdout);
}

void App::ensureCurtainEndpoints()
{
  if (itsCurtainEndpointsInitialised)
    return;
  // Default: a diagonal across the inner ~50% of the data bbox so both
  // endpoints land squarely inside the viewport at the typical zoom and
  // pitch. The user can drag them with the arrow keys after that.
  const auto bb = itsSource->boundingBox();
  itsCurtainLat1 = bb.minLat + (bb.maxLat - bb.minLat) * 0.30;
  itsCurtainLon1 = bb.minLon + (bb.maxLon - bb.minLon) * 0.30;
  itsCurtainLat2 = bb.minLat + (bb.maxLat - bb.minLat) * 0.70;
  itsCurtainLon2 = bb.minLon + (bb.maxLon - bb.minLon) * 0.70;
  // Default ceiling = source's heightRangeKm upper bound (12 km for the
  // generic default, ~18 km for radar volumes that reach higher).
  const auto [_, hi] = itsSource->heightRangeKm();
  itsCurtainHeightKm = std::clamp(hi, 2.0, 30.0);
  itsCurtainEndpointsInitialised = true;
}

void App::tickCurtainAnimation()
{
  // Wall-clock dt since the last frame, scaled by the user's speed
  // multiplier. The first call seeds the anchor and ticks zero so the
  // animation doesn't snap by the entire startup delay. dt is also
  // clamped to a sane upper bound — if the program stalled (debugger,
  // popup, etc.) we don't want to skip half a rotation when it resumes.
  const auto now = std::chrono::steady_clock::now();
  if (!itsCurtainAnimTickValid)
  {
    itsCurtainAnimLastTick = now;
    itsCurtainAnimTickValid = true;
    return;
  }
  const std::chrono::duration<double> diff = now - itsCurtainAnimLastTick;
  itsCurtainAnimLastTick = now;
  const double dt = std::clamp(diff.count(), 0.0, 0.20) * itsCurtainAnimSpeed;
  if (dt <= 0.0)
    return;
  // Base angular rates. At speed = 1 a full swing back-and-forth takes
  // ~2.5 s (so a left-to-right traverse of the data is ~1.2 s), rotate
  // and tilt complete a full revolution in ~2.5 s, and the centre's
  // orbit cycles every ~5 s — orbit is a meta-motion that should feel
  // slower than the primary motions superimposed on it. All scale
  // together with the speed multiplier, so +/- in Edit mode keeps the
  // relative tempo consistent.
  constexpr double kSwingHz = 0.40;
  constexpr double kRotateHz = 0.40;
  constexpr double kOrbitHz = 0.20;
  constexpr double kTiltHz = 0.40;
  constexpr double kTau = 2.0 * M_PI;
  // Phases only advance for flags that are currently on. This means a
  // user can park the rotation at a particular angle, switch swing on
  // for a moment, then return to rotation without the rotate angle
  // having drifted while swing was running.
  if (itsCurtainAutoSwing)
    itsCurtainSwingPhase = std::fmod(itsCurtainSwingPhase + dt * kTau * kSwingHz, kTau);
  if (itsCurtainAutoRotate)
    itsCurtainRotatePhase = std::fmod(itsCurtainRotatePhase + dt * kTau * kRotateHz, kTau);
  if (itsCurtainAutoOrbit)
    itsCurtainOrbitPhase = std::fmod(itsCurtainOrbitPhase + dt * kTau * kOrbitHz, kTau);
  if (itsCurtainAutoTilt)
    itsCurtainTiltPhase = std::fmod(itsCurtainTiltPhase + dt * kTau * kTiltHz, kTau);
}

void App::draw3DCrossSection(const Layout& layout)
{
  // Inverse-projection raycaster + curtain. Same camera + flat-Earth
  // frame as draw3DQueryData, but per-output-pixel work instead of
  // per-grid-cell splat. Surface paints come from the source's bottom
  // (highest-pressure / lowest-height) level; curtain content comes
  // from interpolatedValueAtHeight which the existing 2D cross-section
  // already uses for height-mode sources.
  const int cellW = layout.map.width;
  const int cellH = layout.map.height;
  if (cellW < 2 || cellH < 2)
    return;
  const int subRows = subRowsForStyle(itsCornerStyle);
  const int subW = cellW * 2;
  const int subH = cellH * subRows;
  const double aspect = static_cast<double>(subRows) / 4.0;

  // Demote Thick → Braille in 3D so thick lines don't crowd the curtain.
  const LineStyle effCoast =
      (itsCoastlineStyle == LineStyle::Thick) ? LineStyle::Braille : itsCoastlineStyle;
  const LineStyle effBord =
      (itsBorderStyle == LineStyle::Thick) ? LineStyle::Braille : itsBorderStyle;

  // Camera basis (verbatim from draw3DQueryData).
  const double cy = std::cos(itsCamYaw);
  const double sy_ = std::sin(itsCamYaw);
  const double cp = std::cos(itsCamPitch);
  const double sp = std::sin(itsCamPitch);
  const double rightX = cy, rightY = sy_, rightZ = 0;
  const double upX = -sy_ * sp, upY = cy * sp, upZ = cp;
  const double fwdX = -sy_ * cp, fwdY = cy * cp, fwdZ = -sp;

  // Flat-Earth frame anchored at bbox centre.
  const auto bb = itsSource->boundingBox();
  const double lat0 = (bb.minLat + bb.maxLat) * 0.5;
  const double lon0 = (bb.minLon + bb.maxLon) * 0.5;
  constexpr double R_e = 6371000.0;
  const double cosLat0 = std::cos(lat0 * M_PI / 180.0);
  const double extentX = (bb.maxLon - bb.minLon) * (M_PI / 180.0) * R_e * cosLat0 * 0.5;
  const double extentY = (bb.maxLat - bb.minLat) * (M_PI / 180.0) * R_e * 0.5;
  const double extent = std::max(extentX, extentY);
  if (extent <= 0.0)
    return;
  const double xscale = (subW / 2.0) / extent * itsCamZoom;
  const double yscale = xscale / aspect;
  const double depthScale = xscale;
  const double vexagger = itsVexagger3D;

  auto latLonToXY = [&](double lat, double lon, double& x, double& y)
  {
    x = (lon - lon0) * (M_PI / 180.0) * R_e * cosLat0;
    y = (lat - lat0) * (M_PI / 180.0) * R_e;
  };
  auto xyToLatLon = [&](double x, double y, double& lat, double& lon)
  {
    lat = lat0 + y / ((M_PI / 180.0) * R_e);
    lon = lon0 + x / ((M_PI / 180.0) * R_e * cosLat0);
  };

  std::vector<Rgb> pixels(static_cast<std::size_t>(subW) * subH, Rgb{0, 0, 0, true});
  std::vector<float> zbuf(static_cast<std::size_t>(subW) * subH,
                          std::numeric_limits<float>::infinity());

  auto project = [&](double wx, double wy, double wz, double& col, double& row, float& depth)
  {
    const double sx = rightX * wx + rightY * wy + rightZ * wz;
    const double sy = upX * wx + upY * wy + upZ * wz;
    const double dp = fwdX * wx + fwdY * wy + fwdZ * wz;
    col = subW / 2.0 + xscale * sx;
    row = subH / 2.0 - yscale * sy;
    depth = static_cast<float>(dp * depthScale);
  };
  auto plot = [&](int c, int r, float depth, Rgb color)
  {
    if (c < 0 || c >= subW || r < 0 || r >= subH)
      return;
    const std::size_t idx = static_cast<std::size_t>(r) * subW + c;
    if (depth < zbuf[idx])
    {
      zbuf[idx] = depth;
      pixels[idx] = color;
    }
  };
  auto drawLine =
      [&](double wx0, double wy0, double wz0, double wx1, double wy1, double wz1, Rgb color)
  {
    double c0 = 0, r0 = 0, c1 = 0, r1 = 0;
    float d0 = 0, d1 = 0;
    project(wx0, wy0, wz0, c0, r0, d0);
    project(wx1, wy1, wz1, c1, r1, d1);
    const double dc = c1 - c0;
    const double dr = r1 - r0;
    const double dd = static_cast<double>(d1) - static_cast<double>(d0);
    const int steps = static_cast<int>(std::ceil(std::max(std::abs(dc), std::abs(dr))));
    if (steps <= 0)
    {
      plot(static_cast<int>(std::round(c0)), static_cast<int>(std::round(r0)), d0, color);
      return;
    }
    for (int i = 0; i <= steps; ++i)
    {
      const double t = static_cast<double>(i) / steps;
      const int c = static_cast<int>(std::round(c0 + t * dc));
      const int r = static_cast<int>(std::round(r0 + t * dr));
      const float d = static_cast<float>(d0 + t * dd);
      plot(c, r, d, color);
    }
  };

  // Bottom level for the surface paint. Level *value* ordering is
  // unreliable across NWP conventions — pressure decreases with altitude
  // (1000 hPa at surface), hybrid index numbers MEPS/HARMONIE/IFS-style
  // from TOA = 1 down to surface = N (so the value ascends but altitude
  // descends), WRF goes the other way. Probe GeomHeight at the bbox
  // centre and pick whichever level sits lowest; if there is no height
  // axis (e.g. PVOL ground-plane paint), fall through with bottomIdx=0,
  // which is the right answer for elevation-angle sources.
  const int savedLevel = static_cast<int>(itsSource->currentLevelIndex());
  const int nLevels = static_cast<int>(itsSource->levelCount());
  int bottomIdx = 0;
  if (nLevels > 1 && itsSource->hasNativeHeight())
  {
    const auto centre = itsSource->sampleColumnProfile(
        (bb.minLat + bb.maxLat) * 0.5, (bb.minLon + bb.maxLon) * 0.5);
    if (static_cast<int>(centre.heightsM.size()) == nLevels)
    {
      float minH = std::numeric_limits<float>::infinity();
      for (int i = 0; i < nLevels; ++i)
      {
        const float h = centre.heightsM[static_cast<std::size_t>(i)];
        if (h != kFloatMissing && std::isfinite(h) && h < minH)
        {
          minH = h;
          bottomIdx = i;
        }
      }
    }
  }
  else if (nLevels > 1 && !itsSource->levelsAscendWithValue())
  {
    // Pressure with no height field: the largest value (1000 hPa) is
    // the surface. Same trick as the GridFilesSource descending sort.
    int best = 0;
    float bestV = itsSource->levelValueAt(0);
    for (int i = 1; i < nLevels; ++i)
    {
      const float v = itsSource->levelValueAt(i);
      if (v > bestV)
      {
        bestV = v;
        best = i;
      }
    }
    bottomIdx = best;
  }
  itsSource->selectLevelIndex(static_cast<std::size_t>(bottomIdx));

  // Animation tick + effective endpoints. The user's edit-mode arrow keys
  // move itsCurtainLat1/Lon1/Lat2/Lon2 (the "base" curtain). Each enabled
  // animation transforms the base into a different on-screen curtain:
  //   orbit  - slides the centre around a circle in world coords
  //   rotate - continuously rotates the half-vector about the centre
  //   swing  - slides the plane back and forth along its own normal
  //   tilt   - tilts the plane(s) about their long axis (the AB line)
  // The X-cross adds a second plane perpendicular to the first through
  // the same centre. Toggles for each animation live on x s r o T.
  tickCurtainAnimation();
  double baseAx = 0, baseAy = 0, baseBx = 0, baseBy = 0;
  latLonToXY(itsCurtainLat1, itsCurtainLon1, baseAx, baseAy);
  latLonToXY(itsCurtainLat2, itsCurtainLon2, baseBx, baseBy);
  const double baseCx = (baseAx + baseBx) * 0.5;
  const double baseCy = (baseAy + baseBy) * 0.5;
  const double baseHx = (baseBx - baseAx) * 0.5;
  const double baseHy = (baseBy - baseAy) * 0.5;
  double cxC = baseCx, cyC = baseCy;
  if (itsCurtainAutoOrbit)
  {
    const double orbitR = std::max(extent * 0.20, std::hypot(baseHx, baseHy) * 0.40);
    cxC = baseCx + orbitR * std::cos(itsCurtainOrbitPhase);
    cyC = baseCy + orbitR * std::sin(itsCurtainOrbitPhase);
  }
  // Rotate the half-vector about the (possibly orbit-shifted) centre.
  const double rotAz = itsCurtainAutoRotate ? itsCurtainRotatePhase : 0.0;
  const double cosAz = std::cos(rotAz);
  const double sinAz = std::sin(rotAz);
  const double hxR = baseHx * cosAz - baseHy * sinAz;
  const double hyR = baseHx * sinAz + baseHy * cosAz;
  // Swing translates the plane along its own normal — i.e. the perp
  // direction to the (post-rotate) AB. Amplitude = one bbox-extent so a
  // full half-period traverses the data from one side to the other.
  // If rotate is also on, the swept axis spins with the plane, which
  // is the natural "stays perpendicular to the plane" behaviour.
  if (itsCurtainAutoSwing)
  {
    const double Lrot = std::hypot(hxR, hyR);
    if (Lrot > 1e-9)
    {
      const double perpX = -hyR / Lrot;
      const double perpY = hxR / Lrot;
      const double offset = extent * std::sin(itsCurtainSwingPhase);
      cxC += offset * perpX;
      cyC += offset * perpY;
    }
  }
  const double tiltAngle = itsCurtainAutoTilt ? itsCurtainTiltPhase : 0.0;
  const double cosT = std::cos(tiltAngle);
  const double sinT = std::sin(tiltAngle);
  const double curtainHmaxWorld = itsCurtainHeightKm * 1000.0 * vexagger;

  // One plane = one half of the X-cross. Both planes share the camera
  // basis and the tilt angle; only the (A,B) endpoints differ.
  struct Plane
  {
    double ax, ay;
    double abx, aby;
    double abLen2;
    double upx, upy, upz;
    double nx, ny, nz;
    double aDotN;
    double rightDotN, upDotN, fwdDotN;
    std::vector<DataSource::ColumnProfile> profiles;
    double lat1, lon1, lat2, lon2;
  };

  auto buildPlane = [&](double Ax, double Ay, double Bx, double By, Plane& p)
  {
    p.ax = Ax;
    p.ay = Ay;
    p.abx = Bx - Ax;
    p.aby = By - Ay;
    p.abLen2 = p.abx * p.abx + p.aby * p.aby;
    const double L = std::sqrt(std::max(p.abLen2, 1e-12));
    // Tilt rotates the curtain's "up" about the AB axis by tiltAngle.
    // right = (abx, aby, 0)/L; un-tilted up = (0,0,1).
    // up_rotated = up*cos(t) + (right × up)*sin(t)
    //            = (aby/L*sin(t), -abx/L*sin(t), cos(t))
    p.upx = (p.aby / L) * sinT;
    p.upy = (-p.abx / L) * sinT;
    p.upz = cosT;
    // Normal = right × up_rotated.
    p.nx = (p.aby / L) * cosT;
    p.ny = (-p.abx / L) * cosT;
    p.nz = -sinT;
    p.aDotN = p.ax * p.nx + p.ay * p.ny;  // A is at z = 0
    p.rightDotN = rightX * p.nx + rightY * p.ny;  // rightZ = 0
    p.upDotN = upX * p.nx + upY * p.ny + upZ * p.nz;
    p.fwdDotN = fwdX * p.nx + fwdY * p.ny + fwdZ * p.nz;
    // Column profile cache along A->B. The shear approximation samples
    // the AB-column at u with the hit point's world-z as the altitude:
    // exact for tilt = 0 and a useful approximation otherwise (the
    // curtain leans, the along-track structure stays anchored to AB).
    xyToLatLon(Ax, Ay, p.lat1, p.lon1);
    xyToLatLon(Bx, By, p.lat2, p.lon2);
    const int nCols = std::max(64, subW);
    p.profiles.clear();
    p.profiles.reserve(static_cast<std::size_t>(nCols));
    for (int c = 0; c < nCols; ++c)
    {
      const double u = (nCols > 1) ? static_cast<double>(c) / (nCols - 1) : 0.0;
      const double lat = p.lat1 + u * (p.lat2 - p.lat1);
      const double lon = p.lon1 + u * (p.lon2 - p.lon1);
      p.profiles.push_back(itsSource->sampleColumnProfile(lat, lon));
    }
  };

  Plane plane1, plane2;
  buildPlane(cxC - hxR, cyC - hyR, cxC + hxR, cyC + hyR, plane1);
  if (itsCurtainTwoPlane)
  {
    const double hx2 = -hyR;  // perpendicular half-vector (rotated 90°)
    const double hy2 = hxR;
    buildPlane(cxC - hx2, cyC - hy2, cxC + hx2, cyC + hy2, plane2);
  }

  // Sample one column profile at along-track u and metric height by
  // lerping across two flanking pre-sampled columns. Pure function of
  // (profiles, u, heightM) — same helper works for both planes.
  auto sampleColumn =
      [&](const std::vector<DataSource::ColumnProfile>& profiles, double u, double heightM) -> float
  {
    const int nCols = static_cast<int>(profiles.size());
    if (nCols == 0)
      return kFloatMissing;
    const double cf = std::clamp(u, 0.0, 1.0) * (nCols - 1);
    const int c0 = std::clamp(static_cast<int>(std::floor(cf)), 0, nCols - 1);
    const int c1 = std::min(nCols - 1, c0 + 1);
    const float wc = static_cast<float>(cf - c0);
    auto sampleAt = [&](const DataSource::ColumnProfile& pp) -> float
    {
      const std::size_t n = pp.heightsM.size();
      if (n < 2)
        return n == 1 ? pp.values[0] : kFloatMissing;
      for (std::size_t i = 0; i + 1 < n; ++i)
      {
        const float hA = pp.heightsM[i];
        const float hB = pp.heightsM[i + 1];
        if (hA == kFloatMissing || hB == kFloatMissing)
          continue;
        const float lo = std::min(hA, hB);
        const float hi = std::max(hA, hB);
        if (heightM < lo || heightM > hi)
          continue;
        const float vA = pp.values[i];
        const float vB = pp.values[i + 1];
        if (vA == kFloatMissing || vB == kFloatMissing)
          return kFloatMissing;
        const float t = (hB != hA) ? (static_cast<float>(heightM) - hA) / (hB - hA) : 0.0F;
        return vA + (vB - vA) * t;
      }
      return kFloatMissing;
    };
    const float v0 = sampleAt(profiles[c0]);
    const float v1 = sampleAt(profiles[c1]);
    if (v0 == kFloatMissing)
      return v1;
    if (v1 == kFloatMissing)
      return v0;
    return v0 * (1.0F - wc) + v1 * wc;
  };

  auto rayHitsPlane = [&](const Plane& pl, double scx, double scy,
                          double& uOut, double& zWorldOut, float& depthOut) -> bool
  {
    if (pl.abLen2 < 1e-6 || std::abs(pl.fwdDotN) < 1e-9)
      return false;
    const double tCurtain = (pl.aDotN - scx * pl.rightDotN - scy * pl.upDotN) / pl.fwdDotN;
    const double px = scx * rightX + scy * upX + tCurtain * fwdX;
    const double py = scx * rightY + scy * upY + tCurtain * fwdY;
    const double pz = scy * upZ + tCurtain * fwdZ;
    const double dx = px - pl.ax;
    const double dy = py - pl.ay;
    const double u = (dx * pl.abx + dy * pl.aby) / pl.abLen2;
    if (u < 0.0 || u > 1.0)
      return false;
    // Height along the tilted "up" axis. With tilt=0 this reduces to pz.
    const double hUp = dx * pl.upx + dy * pl.upy + pz * pl.upz;
    if (hUp < 0.0 || hUp > curtainHmaxWorld)
      return false;
    uOut = u;
    zWorldOut = pz;
    depthOut = static_cast<float>(tCurtain * depthScale);
    return true;
  };

  // Per-pixel ray-cast. Intersect against ground + active plane(s),
  // paint nearest hit. One shared z-buffer so the planes occlude each
  // other and the ground naturally.
  for (int sy = 0; sy < subH; ++sy)
  {
    for (int sx = 0; sx < subW; ++sx)
    {
      const double scx = (sx - subW / 2.0) / xscale;
      const double scy = (subH / 2.0 - sy) / yscale;

      bool haveGround = false;
      double gx = 0, gy = 0;
      float gDepth = 0;
      if (std::abs(fwdZ) > 1e-9)
      {
        const double tGround = -scy * upZ / fwdZ;
        gx = scx * rightX + scy * upX + tGround * fwdX;
        gy = scx * rightY + scy * upY + tGround * fwdY;
        if (gx > -extent * 1.1 && gx < extent * 1.1 && gy > -extent * 1.1 && gy < extent * 1.1)
        {
          gDepth = static_cast<float>(tGround * depthScale);
          haveGround = true;
        }
      }

      double u1 = 0, z1 = 0;
      float d1 = 0;
      const bool hit1 = rayHitsPlane(plane1, scx, scy, u1, z1, d1);

      bool hit2 = false;
      double u2 = 0, z2 = 0;
      float d2 = 0;
      if (itsCurtainTwoPlane)
        hit2 = rayHitsPlane(plane2, scx, scy, u2, z2, d2);

      enum class Pick : std::uint8_t { None, Ground, P1, P2 };
      Pick pick = Pick::None;
      float bestD = std::numeric_limits<float>::infinity();
      if (haveGround && gDepth < bestD) { bestD = gDepth; pick = Pick::Ground; }
      if (hit1 && d1 < bestD)           { bestD = d1;     pick = Pick::P1; }
      if (hit2 && d2 < bestD)           { bestD = d2;     pick = Pick::P2; }

      if (pick == Pick::P1 || pick == Pick::P2)
      {
        const Plane& pl = (pick == Pick::P1) ? plane1 : plane2;
        const double u = (pick == Pick::P1) ? u1 : u2;
        const double zw = (pick == Pick::P1) ? z1 : z2;
        const float raw = sampleColumn(pl.profiles, u, zw / vexagger);
        if (raw != kFloatMissing && std::isfinite(raw))
        {
          const float val = transform(raw);
          plot(sx, sy, bestD, activePanel().palette.lookup(val));
          continue;
        }
        // Curtain pixel had no data — fall through to ground if any.
      }
      if (haveGround)
      {
        double lat = 0, lon = 0;
        xyToLatLon(gx, gy, lat, lon);
        if (lat >= bb.minLat && lat <= bb.maxLat && lon >= bb.minLon && lon <= bb.maxLon)
        {
          const float val = transform(itsSource->interpolatedValue(lat, lon));
          if (std::isfinite(val))
            plot(sx, sy, gDepth, activePanel().palette.lookup(val));
        }
      }
    }
  }
  itsSource->selectLevelIndex(static_cast<std::size_t>(savedLevel));

  // Coastlines / borders projected onto the ground plane. Thick path
  // rasterises directly into pixels via drawLine; the Braille path uses a
  // 2×4 sub-cell mask composited as glyphs over the final raster (see
  // below). Thick is demoted to Braille in 3D by effCoast/effBord upstream,
  // so this branch is normally inactive unless that demotion is removed.
  auto drawPolylinesThick = [&](const std::vector<Polyline>& polys, Rgb color)
  {
    for (const auto& p : polys)
    {
      if (p.lats.size() < 2)
        continue;
      double x0 = 0, y0 = 0;
      latLonToXY(p.lats[0], p.lons[0], x0, y0);
      for (std::size_t i = 1; i < p.lats.size(); ++i)
      {
        double x1 = 0, y1 = 0;
        latLonToXY(p.lats[i], p.lons[i], x1, y1);
        drawLine(x0, y0, 0, x1, y1, 0, color);
        x0 = x1;
        y0 = y1;
      }
    }
  };
  if (effCoast == LineStyle::Thick)
    drawPolylinesThick(itsCoastlines, Rgb{200, 200, 200});
  if (effBord == LineStyle::Thick)
    drawPolylinesThick(itsBorders, Rgb{120, 120, 120});

  // Endpoint poles + curtain rectangle outlines. Each plane shows its
  // own poles in distinct colours so the camera orbit doesn't lose track
  // of which is which. With tilt, the "top edge" follows the tilted up
  // direction so the outline matches the rendered geometry.
  auto drawCurtainOutline = [&](const Plane& pl, Rgb colA, Rgb colB, Rgb colTop)
  {
    const double H = curtainHmaxWorld;
    const double topAx = pl.ax + H * pl.upx;
    const double topAy = pl.ay + H * pl.upy;
    const double topAz = H * pl.upz;
    const double bx = pl.ax + pl.abx;
    const double by = pl.ay + pl.aby;
    const double topBx = bx + H * pl.upx;
    const double topBy = by + H * pl.upy;
    const double topBz = H * pl.upz;
    drawLine(pl.ax, pl.ay, 0, topAx, topAy, topAz, colA);
    drawLine(bx, by, 0, topBx, topBy, topBz, colB);
    drawLine(topAx, topAy, topAz, topBx, topBy, topBz, colTop);
    drawLine(pl.ax, pl.ay, 0, bx, by, 0, colTop);
  };
  drawCurtainOutline(plane1, Rgb{255, 220, 80}, Rgb{80, 200, 255}, Rgb{180, 180, 180});
  if (itsCurtainTwoPlane)
    drawCurtainOutline(plane2, Rgb{255, 140, 200}, Rgb{200, 255, 140}, Rgb{140, 140, 140});

  // Status footer. Show sub-mode, the rendered (animated) endpoints,
  // active animation flags, and the speed multiplier so the user can
  // see what the toggles are doing.
  const char* subModeLbl = "?";
  switch (itsCurtainActiveEnd)
  {
    case CurtainEnd::A: subModeLbl = "A"; break;
    case CurtainEnd::B: subModeLbl = "B"; break;
    case CurtainEnd::Both: subModeLbl = "A+B"; break;
    case CurtainEnd::View: subModeLbl = "View"; break;
  }
  std::string flags;
  flags.reserve(5);
  flags.push_back(itsCurtainTwoPlane ? 'X' : '.');
  flags.push_back(itsCurtainAutoSwing ? 'S' : '.');
  flags.push_back(itsCurtainAutoRotate ? 'R' : '.');
  flags.push_back(itsCurtainAutoOrbit ? 'O' : '.');
  flags.push_back(itsCurtainAutoTilt ? 'T' : '.');
  const std::string hud = fmt::format(
      "3D curtain  mode:{}  A:{:.2f},{:.2f}  B:{:.2f},{:.2f}  ceil:{:g} km  zoom:{:.1f}x  "
      "[{}] {:.2f}x",
      subModeLbl, plane1.lat1, plane1.lon1, plane1.lat2, plane1.lon2,
      itsCurtainHeightKm, itsCamZoom, flags, itsCurtainAnimSpeed);

  std::ostringstream os;
  cache3DRaster(pixels, subW, subH);
  itsRenderer.render(os, pixels, subW, subH, layout.map.row, layout.map.col);

  // Braille overlay for coastlines / borders. Same 2×4 sub-cell trick as
  // draw3DQueryData — projects each polyline vertex into screen + depth,
  // z-tests against zbuf at the underlying sub-cell, then composites the
  // resulting glyphs over the rendered raster.
  const bool brailleCoast = effCoast == LineStyle::Braille && !itsCoastlines.empty();
  const bool brailleBorder = effBord == LineStyle::Braille && !itsBorders.empty();
  if (brailleCoast || brailleBorder)
  {
    const int bW = cellW * 2;
    const int bH = cellH * 4;
    std::vector<unsigned char> dotMask(static_cast<std::size_t>(bW) * bH, 0);
    std::vector<Rgb> dotColor(static_cast<std::size_t>(bW) * bH, Rgb{0, 0, 0});
    const double yFineScale = 4.0 / subRows;
    auto projectB = [&](double wx, double wy, double wz, int& bx, int& by, float& depth)
    {
      const double sx = rightX * wx + rightY * wy + rightZ * wz;
      const double sy = upX * wx + upY * wy + upZ * wz;
      const double dp = fwdX * wx + fwdY * wy + fwdZ * wz;
      bx = static_cast<int>(std::round(bW / 2.0 + xscale * sx));
      by = static_cast<int>(std::round(bH / 2.0 - yscale * yFineScale * sy));
      depth = static_cast<float>(dp * depthScale);
    };
    auto plotB = [&](int bx, int by, float depth, Rgb color)
    {
      if (bx < 0 || bx >= bW || by < 0 || by >= bH)
        return;
      const int qy = by * subRows / 4;
      const std::size_t qidx = static_cast<std::size_t>(qy) * subW + bx;
      if (depth >= zbuf[qidx])
        return;
      const std::size_t bidx = static_cast<std::size_t>(by) * bW + bx;
      dotMask[bidx] = 1;
      dotColor[bidx] = color;
    };
    auto lineB =
        [&](double wx0, double wy0, double wz0, double wx1, double wy1, double wz1, Rgb color)
    {
      int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
      float d0 = 0, d1 = 0;
      projectB(wx0, wy0, wz0, x0, y0, d0);
      projectB(wx1, wy1, wz1, x1, y1, d1);
      const int adx = std::abs(x1 - x0);
      const int ady = std::abs(y1 - y0);
      const int steps = std::max(adx, ady);
      if (steps <= 0)
      {
        plotB(x0, y0, d0, color);
        return;
      }
      for (int i = 0; i <= steps; ++i)
      {
        const double t = static_cast<double>(i) / steps;
        const int cx = static_cast<int>(std::round(x0 + t * (x1 - x0)));
        const int cy_ = static_cast<int>(std::round(y0 + t * (y1 - y0)));
        const float dt = static_cast<float>(d0 + t * (d1 - d0));
        plotB(cx, cy_, dt, color);
      }
    };
    auto drawPolyB = [&](const std::vector<Polyline>& polys, Rgb color)
    {
      for (const auto& p : polys)
      {
        if (p.lats.size() < 2)
          continue;
        double x0 = 0, y0 = 0;
        latLonToXY(p.lats[0], p.lons[0], x0, y0);
        for (std::size_t i = 1; i < p.lats.size(); ++i)
        {
          double x1 = 0, y1 = 0;
          latLonToXY(p.lats[i], p.lons[i], x1, y1);
          lineB(x0, y0, 0, x1, y1, 0, color);
          x0 = x1;
          y0 = y1;
        }
      }
    };
    if (brailleCoast)
      drawPolyB(itsCoastlines, Rgb{200, 200, 200});
    if (brailleBorder)
      drawPolyB(itsBorders, Rgb{120, 120, 120});

    std::string brailleOut;
    for (int cy_ = 0; cy_ < cellH; ++cy_)
    {
      for (int cx = 0; cx < cellW; ++cx)
      {
        unsigned cellMask = 0;
        Rgb cellFg{200, 200, 200};
        for (int sy = 0; sy < 4; ++sy)
        {
          for (int sx = 0; sx < 2; ++sx)
          {
            const int bx = cx * 2 + sx;
            const int by = cy_ * 4 + sy;
            const std::size_t bidx = static_cast<std::size_t>(by) * bW + bx;
            if (dotMask[bidx] != 0U)
            {
              cellMask |= 1U << brailleBit(sx, sy);
              cellFg = dotColor[bidx];
            }
          }
        }
        if (cellMask == 0U)
          continue;
        const int qy = cy_ * subRows;
        const Rgb bg = pixels[static_cast<std::size_t>(qy) * subW + cx * 2];
        brailleOut += "\x1b[";
        brailleOut += std::to_string(layout.map.row + cy_ + 1);
        brailleOut += ';';
        brailleOut += std::to_string(layout.map.col + cx + 1);
        brailleOut += 'H';
        brailleOut += itsRenderer.bgEscape(bg);
        brailleOut += itsRenderer.fgEscape(cellFg);
        brailleOut += brailleGlyph(cellMask);
      }
    }
    if (!brailleOut.empty())
      os << brailleOut << "\x1b[0m";
  }

  os << "\x1b[" << layout.map.row + layout.map.height << ";" << layout.map.col + 1 << "H"
     << "\x1b[0m\x1b[7m " << hud << " \x1b[0m";
  std::cout << os.str();
  std::cout.flush();
}

bool App::sourceSupports3D() const
{
  if (dynamic_cast<const OdimVolumeSource*>(itsSource.get()) != nullptr)
    return true;
  if (const auto* qd = dynamic_cast<const QueryDataSource*>(itsSource.get()); qd != nullptr)
    return qd->isVolumetric() || qd->isSurfaceStack();
  return false;
}

bool App::sourceSupportsGlobe() const
{
  // The globe sampler only needs interpolatedValue(lat, lon), which every
  // gridded geographic backend provides. Images (no scalar field) and
  // vector layers (rasterised to feature ids) are excluded.
  return itsSource != nullptr && itsSource->category() == SourceCategory::Gridded;
}

// ---------------------------------------------------------------------------
// Globe renderer (key [g]). See the header comment on drawGlobe() for the
// data flow. The maths:
//   * The world is a unit sphere; geographic (lat,lon) maps to an ECEF unit
//     vector  (cosφcosλ, cosφsinλ, sinφ).
//   * The camera orbits the centre. itsCamYaw = centre longitude, itsCamPitch
//     = centre latitude; the outward normal there is the vector pointing at
//     the camera (n). Screen east = normalise(Z × n), screen up = n × east.
//   * Orthographic projection: a surface point P projects to
//        col = W/2 + xscale·(P·east),  row = H/2 − yscale·(P·up)
//     and faces the camera iff (P·n) > 0. Depth = −(P·n): the near pole is
//     closest, the limb is at depth 0, the far hemisphere is culled.
//   * Data fill is the inverse: for each sub-pixel inside the disc we solve
//     the orthographic ray for the near intersection and read its (lat,lon).
// ---------------------------------------------------------------------------
void App::drawGlobe(const Layout& layout)
{
  if (!sourceSupportsGlobe())
  {
    itsModeGlobe = false;
    return;
  }

  const auto& l = layout;
  if (l.map.height < 4 || l.map.width < 4)
    return;

  // Coastlines / borders are read globally; only the GSHHS resolution tracks
  // the 2D viewport. Reload (cheap when unchanged) so the [c]/[b] off→on
  // round-trip restores them, exactly as draw3D does.
  loadCoastlines(l.map.width * 2, l.map.height * 4);

  const int cellW = l.map.width;
  const int cellH = l.map.height;
  const int subRows = subRowsForStyle(itsCornerStyle);
  const int subW = cellW * 2;
  const int subH = cellH * subRows;

  // Physical aspect of one sub-pixel, from the terminal's *actual* reported
  // cell size (\e[16t probe, cached in itsCaps), not a hardcoded 1:2 guess —
  // fonts vary, and an assumed ratio turns the globe into an ellipse. Each
  // cell holds 2 sub-pixel columns and `subRows` rows, so a sub-column is
  // cellPxW/2 wide and a sub-row cellPxH/subRows tall. For the disc to come
  // out round, equal world distance must map to equal physical extent, i.e.
  //   yscale / xscale = (sub-column width) / (sub-row height).
  const double subColPx = std::max(1, itsCaps.cellPxW) / 2.0;
  const double subRowPx = std::max(1, itsCaps.cellPxH) / static_cast<double>(subRows);
  const double rowPerCol = subColPx / subRowPx;  // < 1 when sub-rows are taller

  // Camera basis from the centre (lat,lon).
  const double cLon = itsCamYaw;
  const double cLat = itsCamPitch;
  auto ecef = [](double lat, double lon, double& x, double& y, double& z)
  {
    const double cl = std::cos(lat);
    x = cl * std::cos(lon);
    y = cl * std::sin(lon);
    z = std::sin(lat);
  };
  double nx = 0, ny = 0, nz = 0;
  ecef(cLat, cLon, nx, ny, nz);  // outward normal at the centre = toward camera
  // east = normalise(Z × n) = normalise(-n.y, n.x, 0); falls back to the
  // longitude tangent at the poles (where the cross product vanishes).
  double ex = -std::sin(cLon);
  double ey = std::cos(cLon);
  double ez = 0.0;
  {
    const double cx = -ny;
    const double cy = nx;
    const double len = std::sqrt(cx * cx + cy * cy);
    if (len > 1e-9)
    {
      ex = cx / len;
      ey = cy / len;
      ez = 0.0;
    }
  }
  // up = n × east
  const double ux = ny * ez - nz * ey;
  const double uy = nz * ex - nx * ez;
  const double uz = nx * ey - ny * ex;

  // Orthographic scale: fit the unit sphere into the map with a 2-sub-pixel
  // margin on the tighter axis, honouring the physical sub-pixel aspect.
  // Vertical extent in sub-rows is 2·radius·yscale = 2·radius·xscale·rowPerCol,
  // so the vertical fit budget divides by rowPerCol.
  const double radius = 1.0;
  const double fitX = (subW / 2.0 - 2.0) / radius;
  const double fitY = (subH / 2.0 - 2.0) / radius / rowPerCol;
  const double xscale = std::max(1.0, std::min(fitX, fitY)) * itsCamZoom;
  const double yscale = xscale * rowPerCol;
  const double cx0 = subW / 2.0;
  const double cy0 = subH / 2.0;

  std::vector<Rgb> pixels(static_cast<std::size_t>(subW) * subH, Rgb{0, 0, 0, true});
  std::vector<float> zbuf(static_cast<std::size_t>(subW) * subH,
                          std::numeric_limits<float>::infinity());

  auto plot = [&](int c, int r, float depth, Rgb color)
  {
    if (c < 0 || c >= subW || r < 0 || r >= subH)
      return;
    const std::size_t idx = static_cast<std::size_t>(r) * subW + c;
    if (depth < zbuf[idx])
    {
      zbuf[idx] = depth;
      pixels[idx] = color;
    }
  };

  // --- Data fill: inverse-orthographic ray-cast onto the near hemisphere.
  const Palette& pal = activePanel().palette;
  const Rgb ocean{16, 26, 48};  // bare-sphere base, limb-darkened below
  for (int r = 0; r < subH; ++r)
  {
    const double sy = (cy0 - (r + 0.5)) / yscale;
    for (int c = 0; c < subW; ++c)
    {
      const double sx = ((c + 0.5) - cx0) / xscale;
      const double rr = sx * sx + sy * sy;
      if (rr > radius * radius)
        continue;  // outside the disc → leave transparent (space)
      const double d = std::sqrt(radius * radius - rr);  // depth toward camera
      // Near intersection P = sx·east + sy·up + d·n (already a unit vector).
      const double px = sx * ex + sy * ux + d * nx;
      const double py = sx * ey + sy * uy + d * ny;
      const double pz = sx * ez + sy * uz + d * nz;
      const double lat = std::asin(std::clamp(pz, -1.0, 1.0)) * 180.0 / M_PI;
      const double lon = std::atan2(py, px) * 180.0 / M_PI;

      const float val = transform(itsSource->interpolatedValue(lat, lon));
      Rgb color{};
      bool painted = false;
      if (val != kFloatMissing && std::isfinite(val) && std::abs(val) < 1e6F)
      {
        color = pal.lookup(val);
        painted = !color.transparent;
      }
      if (!painted)
      {
        // No data here: shade the bare sphere so it still reads as a globe.
        const double sh = 0.35 + 0.65 * d;  // 1 at the centre, 0.35 at the limb
        color = Rgb{static_cast<std::uint8_t>(ocean.r * sh),
                    static_cast<std::uint8_t>(ocean.g * sh),
                    static_cast<std::uint8_t>(ocean.b * sh)};
      }
      const std::size_t idx = static_cast<std::size_t>(r) * subW + c;
      zbuf[idx] = static_cast<float>(-d);
      pixels[idx] = color;
    }
  }

  // --- Geographic overlays: project (lat,lon) lifted just above the surface
  // so they win the z-test against the fill, and cull the far hemisphere.
  constexpr double kLift = 1.004;
  auto project = [&](double X, double Y, double Z, double& col, double& row, double& facing)
  {
    facing = X * nx + Y * ny + Z * nz;
    col = cx0 + xscale * (X * ex + Y * ey + Z * ez);
    row = cy0 - yscale * (X * ux + Y * uy + Z * uz);
  };
  auto drawGeoLine = [&](double lat0, double lon0, double lat1, double lon1, Rgb color)
  {
    double x0 = 0, y0 = 0, z0 = 0, x1 = 0, y1 = 0, z1 = 0;
    ecef(lat0 * M_PI / 180.0, lon0 * M_PI / 180.0, x0, y0, z0);
    ecef(lat1 * M_PI / 180.0, lon1 * M_PI / 180.0, x1, y1, z1);
    double c0 = 0, r0 = 0, f0 = 0, c1 = 0, r1 = 0, f1 = 0;
    project(kLift * x0, kLift * y0, kLift * z0, c0, r0, f0);
    project(kLift * x1, kLift * y1, kLift * z1, c1, r1, f1);
    if (f0 <= 0 && f1 <= 0)
      return;  // entire segment on the far hemisphere
    const int steps =
        static_cast<int>(std::ceil(std::max(std::abs(c1 - c0), std::abs(r1 - r0))));
    if (steps <= 0)
    {
      if (f0 > 0)
        plot(static_cast<int>(std::round(c0)), static_cast<int>(std::round(r0)),
             static_cast<float>(-f0), color);
      return;
    }
    for (int i = 0; i <= steps; ++i)
    {
      const double t = static_cast<double>(i) / steps;
      const double f = f0 + t * (f1 - f0);  // approximate facing along the chord
      if (f <= 0)
        continue;  // crossed the limb — drop the back-facing part
      const int cc = static_cast<int>(std::round(c0 + t * (c1 - c0)));
      const int rr = static_cast<int>(std::round(r0 + t * (r1 - r0)));
      plot(cc, rr, static_cast<float>(-f), color);
    }
  };
  auto drawPolys = [&](const std::vector<Polyline>& polys, Rgb color)
  {
    for (const auto& p : polys)
      for (std::size_t i = 1; i < p.lats.size(); ++i)
        drawGeoLine(p.lats[i - 1], p.lons[i - 1], p.lats[i], p.lons[i], color);
  };

  // Graticule first (dimmest), then borders, then coastlines on top.
  if (itsGraticuleStyle != LineStyle::None)
  {
    const Rgb grat{70, 70, 82};
    const Rgb equ{120, 110, 70};
    for (int lat = -60; lat <= 60; lat += 30)
      for (int lon = -180; lon < 180; lon += 5)
        drawGeoLine(lat, lon, lat, lon + 5, lat == 0 ? equ : grat);
    for (int lon = -180; lon < 180; lon += 30)
      for (int lat = -85; lat < 85; lat += 5)
        drawGeoLine(lat, lon, lat + 5, lon, grat);
  }
  if (itsBorderStyle != LineStyle::None)
    drawPolys(itsBorders, Rgb{120, 120, 120});
  if (itsCoastlineStyle != LineStyle::None)
    drawPolys(itsCoastlines, Rgb{210, 210, 210});

  // --- Emit. Cache the raster so playExitEffect can animate the globe too.
  std::ostringstream os;
  cache3DRaster(pixels, subW, subH);
  itsRenderer.render(os, pixels, subW, subH, l.map.row, l.map.col);

  const std::string hud = fmt::format(" Globe  lat={:.0f}°  lon={:.0f}°  zoom={:.2f}×  [G] exit ",
                                      cLat * 180.0 / M_PI,
                                      cLon * 180.0 / M_PI,
                                      itsCamZoom);
  const int hudRow = l.map.row + l.map.height - 1;
  const int hudCol =
      std::max(l.map.col, l.map.col + l.map.width - static_cast<int>(hud.size()) - 1);
  os << "\x1b[" << (hudRow + 1) << ';' << (hudCol + 1) << "H"
     << "\x1b[48;5;235m\x1b[38;5;15m" << hud << "\x1b[0m";

  const std::string s = os.str();
  std::fwrite(s.data(), 1, s.size(), stdout);
  std::fflush(stdout);
}

void App::apply3DDefaultsForSource()
{
  // Defaults are picked per backend because the natural threshold scale
  // and aspect ratio of the data differ wildly: dBZ ranges roughly
  // -30..70 while cloud cover is 0..100, and NWP domains are two orders
  // of magnitude wider than they are tall.
  if (dynamic_cast<const QueryDataSource*>(itsSource.get()) != nullptr)
  {
    // 50% gates out the thin / partial-cover cells that the
    // totalcloudcover_color palette still paints visibly, so the volume
    // reads as cloud bodies and not a solid blob. , / . steps from here.
    // Surface-stack mode reads the same threshold as the cloud-cover
    // gate; precip and fog have their own fixed thresholds.
    itsThreshold3D = 50.0F;  // %; mostly-cloudy or more
    itsThreshold3DUnit = "%";
    itsVexagger3D = 50.0;  // 2000 km wide × ~30 km tall → ~70:1
  }
  else
  {
    itsThreshold3D = -10.0F;  // dBZ; -10 captures most real echoes
    itsThreshold3DUnit = "dBZ";
    itsVexagger3D = 8.0;
  }
}

void App::ensureExtremaCache()
{
  const auto* qd = dynamic_cast<const QueryDataSource*>(itsSource.get());
  if (qd == nullptr || !qd->isVolumetric())
  {
    itsExtremaFeatures.clear();
    itsExtremaGrid = VolumeGrid{};
    itsExtremaKey.clear();
    return;
  }
  const std::string key =
      fmt::format("{}|{}", itsSource->currentParamId(), itsSource->currentTimeIndex());
  if (key == itsExtremaKey && itsExtremaGrid.cellCount() > 0)
    return;  // cache hit — param and time unchanged

  itsExtremaKey = key;
  itsExtremaFeatures.clear();
  itsExtremaGrid = VolumeGrid{};

  // Cap the working resolution so the merge tree (and the read) stay
  // interactive on big hybrid volumes — this is what kept the view at ~2 s
  // per frame during time animation, where the per-frame param/time change
  // forces a recompute. ~400k cells ≈ 0.15 s; features are for visualisation,
  // so the coarser lattice is fine.
  constexpr std::size_t kCellBudget = 400000;
  VolumeGrid g;
  if (!qd->sampleVolumeGrid(
          g.nx, g.ny, g.nz, g.values, g.heights, g.lats, g.lons, kCellBudget))
    return;
  itsExtremaGrid = g;  // keep the original (un-detrended) field for colour + coords

  VolumeGrid detr = g;  // detrend a copy so colours stay true to the raw values
  detrendPerLevelMedian(detr);

  auto maxima = findExtrema(detr, ExtremumKind::Max, 0.0F, 0, 60000);
  auto minima = findExtrema(detr, ExtremumKind::Min, 0.0F, 0, 60000);

  // Keep non-global features whose persistence is a meaningful fraction of
  // the *strongest real feature* — scaling to the global range fails because
  // that range is set by the single most extreme cell, dwarfing the genuine
  // features. Features come sorted by descending persistence with the global
  // one first, so the first non-global entry is the strongest. Capped so the
  // view stays legible.
  constexpr std::size_t kCap = 8;
  auto keepProminent = [&](std::vector<Feature>& v)
  {
    float ref = 0.0F;
    for (const auto& f : v)
      if (!f.isGlobal)
      {
        ref = f.persistence;  // strongest non-global
        break;
      }
    if (ref <= 0.0F)
      return;
    const float floor = 0.15F * ref;
    std::size_t kept = 0;
    for (auto& f : v)
    {
      if (f.isGlobal || f.persistence < floor)
        continue;
      itsExtremaFeatures.push_back(std::move(f));
      if (++kept >= kCap)
        break;
    }
  };
  keepProminent(maxima);
  keepProminent(minima);
}

int App::dumpExtremaReport() const
{
  const auto* qd = dynamic_cast<const QueryDataSource*>(itsSource.get());
  if (qd == nullptr || !qd->isVolumetric())
  {
    std::cerr << "qdless: --extrema needs a multi-level QueryData file with a "
                 "height field (hybrid / pressure levels)\n";
    return 1;
  }

  VolumeGrid grid;
  if (!qd->sampleVolumeGrid(
          grid.nx, grid.ny, grid.nz, grid.values, grid.heights, grid.lats, grid.lons))
  {
    std::cerr << "qdless: --extrema: could not extract a structured volume grid\n";
    return 1;
  }

  const std::string param = itsSource->paramShortName(itsSource->currentParamId());
  const std::string units = itsSource->paramUnits(itsSource->currentParamId());

  detrendPerLevelMedian(grid);

  constexpr std::size_t kTopN = 12;
  const auto maxima = findExtrema(grid, ExtremumKind::Max, 0.0F, kTopN);
  const auto minima = findExtrema(grid, ExtremumKind::Min, 0.0F, kTopN);

  std::cout << "[qdless --extrema] " << itsOpts.filename << " | param: " << param << " (" << units
            << ") | time: " << currentTimeLabel() << " | grid: " << grid.nx << "x" << grid.ny << "x"
            << grid.nz << " | detrend: per-level area-weighted median (values are anomalies)\n";

  auto printOne = [&](const char* tag, const Feature& f)
  {
    std::cout << fmt::format(
        "  {}  lat={:7.3f}  lon={:8.3f}  h={:6.2f} km  anom={:+9.3g} {}  persist={:8.3g} {}  "
        "blob={}{} cells{}\n",
        tag, f.lat, f.lon, f.heightMeters / 1000.0, f.value, units, f.persistence, units,
        f.blob.size(), f.blobTruncated ? "+" : "", f.isGlobal ? "  [global]" : "");
  };

  std::cout << "Persistent maxima (" << maxima.size() << "):\n";
  for (const auto& f : maxima)
    printOne("MAX", f);
  std::cout << "Persistent minima (" << minima.size() << "):\n";
  for (const auto& f : minima)
    printOne("MIN", f);
  return 0;
}

int App::runOnce()
{
  if (itsSource == nullptr)
  {
    std::cerr << "qdless: --dump cannot be used with deferred-source modes "
                 "(--pg without --table, or --dir on a tree root). Pass a "
                 "concrete file or leaf directory.\n";
    return 1;
  }
  // --extrema (without --3d): persistence/merge-tree analysis of the active
  // 3D parameter as a text report (no raster). With --3d it instead renders
  // the blob view (handled in the start3D block below via itsShowExtrema).
  if (itsOpts.dumpExtrema && !itsOpts.start3D)
    return dumpExtremaReport();

  TerminalSize ts = terminalSize();
  int cellW = ts.cols;
  int cellH = std::max(1, ts.rows - 1);
  int subWidth = cellW * 2;
  int subHeight = cellH * 2;

  // --globe: build a UI-less Layout and route through the globe renderer.
  // Falls back to the 2D dump if the source isn't gridded geographic.
  if (itsOpts.startGlobe && sourceSupportsGlobe())
  {
    itsModeGlobe = true;
    const auto bb = itsSource->boundingBox();
    constexpr double kLatLimit = M_PI_2 - 0.01;
    itsCamYaw = ((bb.minLon + bb.maxLon) * 0.5) * M_PI / 180.0;
    itsCamPitch =
        std::clamp(((bb.minLat + bb.maxLat) * 0.5) * M_PI / 180.0, -kLatLimit, kLatLimit);
    itsCamZoom = 1.0;
    Layout layout{};
    layout.map = {0, 0, cellH, cellW};
    drawGlobe(layout);
    std::cout << "\x1b[" << ts.rows << ";1H\n";
    return 0;
  }

  // --3d: build a UI-less Layout and route through the 3D renderer.
  // Useful for CI snapshots / regression checks since the 3D code paths
  // are otherwise interactive-only. Falls back to 2D dump if the active
  // source isn't 3D-capable so the flag is safe to set globally.
  if (itsOpts.start3D && sourceSupports3D())
  {
    itsMode3D = true;
    apply3DDefaultsForSource();
    itsShowExtrema = itsOpts.dumpExtrema;  // --3d --extrema → render the blob view
    Layout layout{};
    layout.map = {0, 0, cellH, cellW};
    loadCoastlines(cellW * 2, cellH * 4);
    if (dynamic_cast<const OdimVolumeSource*>(itsSource.get()) != nullptr)
    {
      draw3D(layout);
    }
    else if (const auto* qd = dynamic_cast<const QueryDataSource*>(itsSource.get()); qd != nullptr)
    {
      if (qd->isVolumetric())
        draw3DQueryData(layout);
      else
        draw3DSurfaceStack(layout);
    }
    std::cout << "\x1b[" << ts.rows << ";1H\n";
    return 0;
  }

  float dataMin = 0;
  float dataMax = 0;
  auto pixels = sampleSlice(subWidth, subHeight, dataMin, dataMax);
  if (itsCoastlineStyle == LineStyle::Thick)
    overlayPolylines(pixels, subWidth, subHeight, itsCoastlines, Rgb{0, 0, 0});
  if (itsBorderStyle == LineStyle::Thick)
    overlayPolylines(pixels, subWidth, subHeight, itsBorders, Rgb{90, 90, 90});
  if (itsShapeOutlineStyle == LineStyle::Thick)
    overlayPolylines(pixels, subWidth, subHeight, itsShapeOutlines, borderColor());
  overlayCities(pixels, subWidth, subHeight);
  overlayMarker(pixels, subWidth, subHeight);

  const int id = itsSource->currentParamId();
  std::string shortName = itsSource->paramShortName(id);
  const std::string origLabel = originTimeLabel();
  std::cout << "[qdless] " << itsOpts.filename << " | param: " << shortName
            << " | time: " << currentTimeLabel() << " (" << (itsSource->currentTimeIndex() + 1)
            << "/" << itsSource->timeCount() << ")";
  if (!origLabel.empty())
    std::cout << " | analysis: " << origLabel;
  std::cout << " | level: " << itsSource->levelLabel(itsSource->currentLevelIndex()) << " ("
            << (itsSource->currentLevelIndex() + 1) << "/" << itsSource->levelCount()
            << ") | range: [" << dataMin << ", " << dataMax
            << "] | palette: " << activePanel().palette.name()
            << " | coast: " << itsCoastlines.size() << "+" << itsBorders.size() << " polylines";
  if (!itsPhenomenonHint.empty())
    std::cout << " | hint: " << itsPhenomenonHint;
  std::cout << '\n';

  std::ostringstream os;
  itsRenderer.render(os, pixels, subWidth, subHeight, 1, 0);
  if (itsCoastlineStyle == LineStyle::Braille)
    appendPolylineBraille(os, itsCoastlines, Rgb{0, 0, 0}, pixels, subWidth, 1, 0);
  if (itsBorderStyle == LineStyle::Braille)
    appendPolylineBraille(os, itsBorders, Rgb{90, 90, 90}, pixels, subWidth, 1, 0);
  if (itsShapeOutlineStyle == LineStyle::Braille)
    appendPolylineBraille(os, itsShapeOutlines, borderColor(), pixels, subWidth, 1, 0);
  os << buildCityLabels(cellW, cellH, 1, 0);
  std::cout << os.str() << "\x1b[" << ts.rows << ";1H" << '\n';
  return 0;
}

void App::cache3DRaster(const std::vector<Rgb>& pixels, int subW, int subH)
{
  itsLast3DRaster = pixels;  // copy: the caller still needs pixels for braille overlays
  itsLast3DRasterW = subW;
  itsLast3DRasterH = subH;
}

void App::playExitEffect(int effectIndex, unsigned seed, const std::string& wordsOverride)
{
  // The automatic quit path honours the opt-out; explicit previews / repeats /
  // menu picks (effectIndex >= 0) don't.
  if (effectIndex < 0 && itsOpts.noExitEffect)
    return;

  // Resolve the "choose for me" pick against the optional --exit-effect
  // allow-list (comma-separated names). One name pins it; several pick at
  // random among them; empty / all-unknown leaves the full random pool.
  if (effectIndex < 0 && !itsOpts.exitEffects.empty())
  {
    std::vector<int> allowed;
    std::string tok;
    auto add = [&]()
    {
      const int idx = Qdless::exitEffectIndexByName(tok);
      if (idx >= 0)
        allowed.push_back(idx);
      tok.clear();
    };
    for (char c : itsOpts.exitEffects)
    {
      if (c == ',')
        add();
      else
        tok.push_back(c);
    }
    add();
    if (!allowed.empty())
    {
      std::mt19937 rng(std::random_device{}());
      effectIndex = allowed[rng() % allowed.size()];
    }
  }

  // 3D views own the screen with their own point-cloud raster; animate the most
  // recent one. Otherwise snapshot the current 2D view at full-terminal
  // resolution (sampleSlice reads the active panel's selection, which drawMap
  // restored before we got here) and bake in the marker / cross-section /
  // cities overlays so the freeze-frame matches what was on screen.
  std::vector<Rgb> frame;
  int subW = 0;
  int subH = 0;
  std::vector<Rgb> utopiaLines;  // coastlines+borders on black (for the Utopia effect)
  std::vector<char> swedenMask;  // 0/1 mask of pixels inside Sweden
  if (itsMode3D && !itsLast3DRaster.empty())
  {
    frame = itsLast3DRaster;
    subW = itsLast3DRasterW;
    subH = itsLast3DRasterH;
  }
  else
  {
    const auto ts = terminalSize();
    subW = ts.cols * 2;
    subH = ts.rows * subRowsForStyle(itsCornerStyle);
    if (subW < 2 || subH < 2)
      return;
    float dMin = 0;
    float dMax = 0;
    frame = sampleSlice(subW, subH, dMin, dMax);
    overlayCities(frame, subW, subH);
    overlayCrossSection(frame, subW, subH);
    overlayMarker(frame, subW, subH);
    buildUtopiaGeo(subW, subH, utopiaLines, swedenMask);
  }

  if (subW < 2 || subH < 2 || frame.empty())
    return;
  const std::string& words = wordsOverride.empty() ? itsOpts.exitMessage : wordsOverride;
  const auto played = Qdless::playExitEffect(itsRenderer,
                                             std::move(frame),
                                             subW,
                                             subH,
                                             effectIndex,
                                             seed,
                                             words,
                                             utopiaLines.empty() ? nullptr : &utopiaLines,
                                             swedenMask.empty() ? nullptr : &swedenMask);
  itsLastExitIndex = played.index;
  itsLastExitSeed = played.seed;
}

int App::runInteractive()
{
  // Probe Kitty / sixel + cell pixel size BEFORE ncurses takes over the
  // terminal (initscr clobbers termios and intercepts stdin). The probe
  // briefly raw-modes stdin to read the DA1 / \e[16t / Kitty replies, then
  // restores it. No-op on non-tty contexts.
  itsCaps = probeTerminalCapabilities();

  UI ui;

  // Deferred PostGIS layer pick: when launched with --pg but no
  // --table, the constructor stopped after opening the connection
  // (popups need the UI). Run the picker now; openPgLayer fills
  // itsSource and we follow up with the same source-dependent init
  // the file path runs in the ctor.
  if (itsSource == nullptr && itsPgDataset != nullptr)
  {
    if (!openPgPicker(ui))
      return 0;  // user cancelled the picker
    initFromSource();
  }
  // Deferred PNG-tree pick: same shape as the PG path. --dir on a tree
  // root left itsSource null in the ctor; open the picker now and run
  // initFromSource on the picked leaf.
  if (itsSource == nullptr && !itsOpts.browseRoot.empty())
  {
    if (!openBrowsePicker(ui))
      return 0;
    initFromSource();
  }

  // --3d: boot straight into 3D mode when the active source supports it.
  // Silently fall back to 2D otherwise so passing --3d at startup is safe
  // regardless of what file the user later opens via the in-app pickers.
  if (itsOpts.start3D && sourceSupports3D())
  {
    itsMode3D = true;
    apply3DDefaultsForSource();
  }
  // --globe: boot straight into the globe view, centred on the data.
  if (itsOpts.startGlobe && sourceSupportsGlobe())
  {
    itsModeGlobe = true;
    itsMode3D = false;
    const auto bb = itsSource->boundingBox();
    constexpr double kLatLimit = M_PI_2 - 0.01;
    itsCamYaw = ((bb.minLon + bb.maxLon) * 0.5) * M_PI / 180.0;
    itsCamPitch =
        std::clamp(((bb.minLat + bb.maxLat) * 0.5) * M_PI / 180.0, -kLatLimit, kLatLimit);
    itsCamZoom = 1.0;
  }

  bool quit = false;
  bool needRedraw = true;

  while (!quit)
  {
    if (needRedraw)
    {
      // DEC mode 2026 (synchronized output): the terminal buffers all
      // rendering between begin and end so the user sees one composed
      // frame instead of timeline → map → popup as three flashes.
      // Terminals that don't support 2026 ignore the private-mode set,
      // so the sequence is safe to emit unconditionally.
      std::fputs("\x1b[?2026h", stdout);

      // Order matters: ncurses' first doupdate() force-paints the whole
      // screen with blanks, which would clobber a raw-ANSI map written
      // beforehand. So commit ncurses windows first, then draw the map
      // on top via raw escapes.
      renderTimeline(ui);
      drawMap(ui);
      drawCrossSection(ui);

      std::fputs("\x1b[?2026l", stdout);
      std::fflush(stdout);
      needRedraw = false;
    }

    // While any curtain animation is on we need to redraw on a fixed
    // cadence — the curtain's swing/rotate/orbit/tilt phases advance in
    // wall-clock time, and without a periodic wake-up there's nothing to
    // trigger a frame. ~33 ms ≈ 30 FPS, fast enough that a 2-3 s rotation
    // looks smooth. Plain time-step animation (Space) keeps its own
    // user-chosen delay; if both are on, take the faster of the two so
    // the curtain still animates smoothly.
    const bool curtainAnim = itsMode3DCurtain &&
                             (itsCurtainAutoSwing || itsCurtainAutoRotate ||
                              itsCurtainAutoOrbit || itsCurtainAutoTilt);
    int waitMs = -1;
    if (itsAnimating && curtainAnim)
      waitMs = std::min(itsAnimationDelayMs, 33);
    else if (itsAnimating)
      waitMs = itsAnimationDelayMs;
    else if (curtainAnim)
      waitMs = 33;
    int key = ui.waitInput(waitMs);
    if (key == ERR)
    {
      // Timeout. With time-step animation: advance the time index. With
      // a curtain animation but no time-step: just redraw — the next
      // tick of the curtain phases happens inside draw3DCrossSection.
      if (itsAnimating)
      {
        auto idx = itsSource->currentTimeIndex();
        const auto n = itsSource->timeCount();
        itsSource->selectTimeIndex((idx + 1) % n);
      }
      needRedraw = true;
      continue;
    }
    needRedraw = handleKey(key, ui, quit);
  }

  // Going-away present: a brief randomly-chosen animation over the last frame,
  // played while curses is still up (the UI destructor's endwin runs after).
  playExitEffect();
  return 0;
}
}  // namespace Qdless
