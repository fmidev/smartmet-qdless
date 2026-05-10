#include "QdlessApp.h"

#include "QdlessMultiFileSource.h"
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
const char* panelLayoutLabel(PanelLayout l)
{
  switch (l)
  {
    case PanelLayout::Single: return "Layout: single";
    case PanelLayout::Side:   return "Layout: side-by-side";
    case PanelLayout::Quad:   return "Layout: 2x2";
  }
  return "Layout: ?";
}

LineStyle nextLineStyle(LineStyle s)
{
  switch (s)
  {
    case LineStyle::Braille: return LineStyle::Thick;
    case LineStyle::Thick:   return LineStyle::None;
    case LineStyle::None:    return LineStyle::Braille;
  }
  return LineStyle::Braille;
}

const char* lineStyleLabel(LineStyle s)
{
  switch (s)
  {
    case LineStyle::Braille: return "braille";
    case LineStyle::Thick:   return "thick";
    case LineStyle::None:    return "off";
  }
  return "?";
}

CornerStyle nextCornerStyle(CornerStyle s)
{
  switch (s)
  {
    case CornerStyle::Sextant:       return CornerStyle::SmallTriangle;
    case CornerStyle::SmallTriangle: return CornerStyle::Square;
    case CornerStyle::Square:        return CornerStyle::Sextant;
  }
  return CornerStyle::Sextant;
}

const char* cornerStyleLabel(CornerStyle s)
{
  switch (s)
  {
    case CornerStyle::Sextant:       return "sextants";
    case CornerStyle::SmallTriangle: return "triangles";
    case CornerStyle::Square:        return "squares";
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
  if (subRow == 3) return (subCol == 0) ? 6U : 7U;
  return static_cast<unsigned>(subCol * 3 + subRow);
}

// Append a raw-ANSI cursor positioning + glyph for each cell of a vertical
// or horizontal separator. `glyph` is a UTF-8 box-drawing character.
void appendSeparator(std::ostringstream& os, int row, int col, int len, bool vertical,
                     const char* glyph)
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
  if (!itsOpts.pgConn.empty())
  {
    // PostGIS browser mode. Open the connection once and keep it
    // alive for the App's lifetime; [T] re-pickers reuse the same
    // dataset. The "PG:" prefix tells GDAL to dispatch to the
    // PostgreSQL driver.
    GDALAllRegister();
    const std::string opener = "PG:" + itsOpts.pgConn;
    auto* ds = static_cast<GDALDataset*>(GDALOpenEx(
        opener.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr));
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
  else if (!itsOpts.filenames.empty() && itsOpts.filenames.size() > 1)
    itsSource = std::make_unique<MultiFileSource>(itsOpts.filenames);
  else
    itsSource = DataSource::open(itsOpts.filename);
  itsPanels.resize(1);
  if (itsSource != nullptr) initFromSource();
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
  if (itsOpts.noCoastline) itsCoastlineStyle = LineStyle::None;
  if (itsOpts.noBorders) itsBorderStyle = LineStyle::None;
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
    itsShowGraticule = false;
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
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (s == "single") targetLayout = PanelLayout::Single;
    else if (s == "side" || s == "side-by-side") targetLayout = PanelLayout::Side;
    else if (s == "quad" || s == "2x2") targetLayout = PanelLayout::Quad;
    else throw std::runtime_error("qdless: unknown --layout '" + itsOpts.layoutOverride +
                                  "' (expected single, side, or quad)");
  }
  else if (overrideIdx.size() == 2)
    targetLayout = PanelLayout::Side;
  else if (overrideIdx.size() >= 3)
    targetLayout = PanelLayout::Quad;

  const std::size_t want = (targetLayout == PanelLayout::Quad)
                               ? 4
                               : (targetLayout == PanelLayout::Side ? 2 : 1);
  if (overrideIdx.size() > want)
    throw std::runtime_error("qdless: --layout " + itsOpts.layoutOverride + " holds " +
                             std::to_string(want) + " panel(s), but " +
                             std::to_string(overrideIdx.size()) + " parameters were given");

  auto resolveIndex = [](int requested, std::size_t size) -> std::size_t {
    if (size == 0) return 0;
    if (requested < 0) return size - 1;
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
        if (itsPanels[i].paramIndex == newParamIdx) continue;
        itsPanels[i].paramIndex = newParamIdx;
        // Re-resolve palette for this slot.
        const int savedActive = itsActivePanel;
        itsActivePanel = static_cast<int>(i);
        if (newParamIdx >= 0 && newParamIdx < n) itsSource->selectParamId(itsParamIds[newParamIdx]);
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
  if (ds == nullptr) throw std::runtime_error("openPgLayer: no PG dataset");
  // Try both the qualified ("schema.table") and bare ("table") name —
  // OGR's PG driver normally reports layers as "schema.name", but
  // GetLayerByName accepts either form.
  OGRLayer* layer = ds->GetLayerByName(schemaTable.c_str());
  if (layer == nullptr) layer = ds->GetLayerByName(stripSchema(schemaTable).c_str());
  if (layer == nullptr)
    throw std::runtime_error("PostGIS table not found: " + schemaTable);
  itsSource = std::make_unique<ShapeSource>(layer, schemaTable);
}

bool App::openPgPicker(UI& ui)
{
  auto* ds = static_cast<GDALDataset*>(itsPgDataset);
  if (ds == nullptr) return false;
  // Build the layer list once. Filtering by --schema (if given) is a
  // simple prefix match against the OGR-reported "schema.name".
  struct Entry
  {
    std::string fullName;
    std::string display;  // "schema.name (Polygon, 16 features)"
  };
  std::vector<Entry> entries;
  const std::string prefix =
      itsOpts.pgSchema.empty() ? std::string{} : itsOpts.pgSchema + ".";
  for (int i = 0; i < ds->GetLayerCount(); ++i)
  {
    OGRLayer* layer = ds->GetLayer(i);
    if (layer == nullptr) continue;
    std::string name = layer->GetName();
    if (!prefix.empty() && name.compare(0, prefix.size(), prefix) != 0) continue;
    // Hide non-spatial tables. OGR's PostgreSQL driver lists ALL
    // tables visible to the role, including pure attribute tables
    // (no geometry column) — those would land in the picker as
    // e.g. "wind" without anything renderable. wkbUnknown is OK
    // (mixed geometries), only wkbNone means "no geometry column".
    const OGRwkbGeometryType gt = layer->GetGeomType();
    if (gt == wkbNone) continue;
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
    throw std::runtime_error(
        std::string("PostGIS: no layers visible") +
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
  auto matcher = [&](const std::string& q) {
    std::vector<std::string> hits;
    lastIdx.clear();
    std::string lq = q;
    std::transform(lq.begin(), lq.end(), lq.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    for (std::size_t i = 0; i < entries.size(); ++i)
    {
      if (lq.empty())
      {
        hits.push_back(entries[i].display);
        lastIdx.push_back(static_cast<int>(i));
        continue;
      }
      std::string lower = entries[i].display;
      std::transform(lower.begin(), lower.end(), lower.begin(),
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
  if (sel < 0 || sel >= static_cast<int>(lastIdx.size())) return false;
  openPgLayer(entries[lastIdx[sel]].fullName);
  return true;
}

float App::transform(float v) const
{
  // Same sentinel detection as Palette::lookup; keep them in sync.
  if (v == kFloatMissing || !std::isfinite(v) || std::abs(v) > 1e6F) return v;
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
      if (v == kFloatMissing || !std::isfinite(v) || std::abs(v) > 1e6F) continue;
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
  }
  panel.palette = Palette::builtinRamp(lo, hi);
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

  if (itsCoastlineStyle != LineStyle::None)
  {
    auto path = Coastline::pickFile(itsOpts.coastlineDir, "GSHHS", span);
    if (!path.empty() && path != itsCoastlinePath)
    {
      itsCoastlines = Coastline::read(path, itsOpts.minLakeAreaKm2, itsOpts.minLakeRoundness,
                                      itsOpts.minIslandAreaKm2);
      itsCoastlinePath = path;
    }
  }
  if (itsBorderStyle != LineStyle::None)
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
    auto sample = [&](double u, double v) {
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
  if (itsStatsCacheValid && itsStatsCacheParam == paramId &&
      itsStatsCacheLevel == levelIdx &&
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
        if (val == kFloatMissing || !std::isfinite(val) || std::fabs(val) > 1e6F) continue;
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
  if (!itsCrossActive) return;

  const double lat1 = itsCrossLat1;
  const double lon1 = itsCrossLon1;
  const double lat2 = itsCrossLat2;
  const double lon2 = itsCrossLon2;

  const int nLevels = static_cast<int>(itsSource->levelCount());
  if (nLevels < 2) return;

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
  const int subRows = subRowsForStyle(itsCornerStyle);
  const int subW = chartW * 2;
  const int subH = chartH * subRows;

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
      const Rgb c = activePanel().palette.lookup(val);
      for (int sr = 0; sr < subRows; ++sr)
        pixels[static_cast<std::size_t>(li * subRows + sr) * subW + sx] = c;
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
      fmt::format(" {:.2f}°N {:.2f}°E  ->  {:.2f}°N {:.2f}°E   total {:.1f} km", lat1, lon1, lat2,
                  lon2, totalKm);
  os << "\x1b[0m\x1b[48;5;0m\x1b[38;5;14m\xe2\x94\x82\x1b[48;5;0m\x1b[38;5;15m" << endpts;
  padSpaces(os, interiorW - static_cast<int>(endpts.size()));
  os << "\x1b[0m\x1b[48;5;0m\x1b[38;5;14m\xe2\x94\x82\x1b[0m";

  // Footer.
  pos(3 + chartH);
  std::string_view footer = " 'x' close, \xe2\x86\x90/\xe2\x86\x92 step time, Space animate";
  // The arrows are 3 bytes each in UTF-8 but render as 1 cell, so subtract
  // the byte/cell skew when computing visible width for padding.
  const int footerCells = static_cast<int>(footer.size()) - 4;  // 2 arrows * 2 extra bytes
  os << "\x1b[0m\x1b[48;5;0m\x1b[38;5;14m\xe2\x94\x82\x1b[48;5;0m\x1b[38;5;15m"
     << footer;
  padSpaces(os, interiorW - footerCells);
  os << "\x1b[0m\x1b[48;5;0m\x1b[38;5;14m\xe2\x94\x82\x1b[0m";

  // Bottom border.
  pos(height - 1);
  os << "\x1b[0m\x1b[48;5;0m\x1b[38;5;14m\xe2\x94\x94";
  for (int i = 0; i < width - 2; ++i) os << "\xe2\x94\x80";
  os << "\xe2\x94\x98\x1b[0m";

  const std::string s = os.str();
  std::fwrite(s.data(), 1, s.size(), stdout);
  std::fflush(stdout);
  (void)ui;  // popup is non-modal; main loop handles input
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

  // Hand-rolled RGB buffer (interleaved RGBRGB…). Writing PNGs needs
  // a library either way; we use GDAL's PNG driver below since GDAL
  // is already a dependency for shapefiles / PostGIS / GeoTIFF.
  // Background = white so "no data" cells leave a clean canvas.
  std::vector<std::uint8_t> rgb(static_cast<std::size_t>(width) *
                                    static_cast<std::size_t>(height) * 3,
                                255);
  auto setPixel = [&](int x, int y, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    if (x < 0 || x >= width || y < 0 || y >= height) return;
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
      if (c.transparent) continue;  // leave white for "no data"
      setPixel(px, py, c.r, c.g, c.b);
    }
  }

  // Coastline + border overlay (Bresenham into the RGB buffer).
  auto drawPolylineImg = [&](const std::vector<Polyline>& polys,
                             std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    if (polys.empty()) return;
    auto toPixel = [&](float lon, float lat) -> std::pair<int, int> {
      double u = 0;
      double v = 0;
      itsSource->latLonToUV(lat, lon, u, v);
      const double u01 = (u - itsViewport.uMin) / spanU;
      const double v01 = (v - itsViewport.vMin) / spanV;
      return {static_cast<int>(u01 * width), static_cast<int>(v01 * height)};
    };
    auto line = [&](int x0, int y0, int x1, int y1) {
      int dx = std::abs(x1 - x0);
      int dy = -std::abs(y1 - y0);
      int sx = x0 < x1 ? 1 : -1;
      int sy = y0 < y1 ? 1 : -1;
      int err2 = dx + dy;
      while (true)
      {
        setPixel(x0, y0, r, g, b);
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
  drawPolylineImg(itsCoastlines, 0, 0, 0);
  drawPolylineImg(itsBorders, 90, 90, 90);

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
  CPLErr cerr = mem->RasterIO(GF_Write, 0, 0, width, height, rgb.data(), width,
                              height, GDT_Byte, 3, const_cast<int*>(bandList), 3,
                              static_cast<GSpacing>(width) * 3, 1, nullptr);
  if (cerr != CE_None)
  {
    GDALClose(mem);
    err = "GDAL RasterIO write failed";
    return {};
  }
  try
  {
    GDALDataset* png =
        pngDrv->CreateCopy(filename.c_str(), mem, FALSE, nullptr, nullptr, nullptr);
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

void App::appendPolylineBraille(std::ostringstream& os,
                                const std::vector<Polyline>& polylines, Rgb color,
                                const std::vector<Rgb>& pixels, int subWidth,
                                int originRow, int originCol) const
{
  if (polylines.empty() || subWidth <= 0) return;
  const int subRows = subRowsForStyle(itsCornerStyle);
  const int cellW = subWidth / 2;
  const int subHeight = static_cast<int>(pixels.size()) / std::max(1, subWidth);
  const int cellH = subHeight / subRows;
  if (cellW <= 0 || cellH <= 0) return;

  // Higher-resolution sub-cell grid for the line: 2 cols × 4 rows per cell.
  const int bW = cellW * 2;
  const int bH = cellH * 4;
  std::vector<unsigned char> mask(static_cast<std::size_t>(bW) * bH, 0);

  const float spanU = itsViewport.uMax - itsViewport.uMin;
  const float spanV = itsViewport.vMax - itsViewport.vMin;
  if (spanU <= 0 || spanV <= 0) return;

  auto plot = [&](int x, int y) {
    if (x >= 0 && x < bW && y >= 0 && y < bH)
      mask[static_cast<std::size_t>(y) * bW + x] = 1;
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

  auto toSub = [&](float lon, float lat) -> std::pair<int, int> {
    double u = 0;
    double v = 0;
    itsSource->latLonToUV(lat, lon, u, v);
    const double u01 = (u - itsViewport.uMin) / spanU;
    const double v01 = (v - itsViewport.vMin) / spanV;
    return {static_cast<int>(u01 * bW), static_cast<int>(v01 * bH)};
  };

  for (const auto& pl : polylines)
  {
    if (pl.lons.size() < 2) continue;
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
      if (cellMask == 0U) continue;
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
  const bool isShape = itsSource->isVector();
  const bool pgMode = (itsPgDataset != nullptr);
  ui.drawTimeline(label, static_cast<int>(itsSource->currentTimeIndex()),
                  static_cast<int>(itsSource->timeCount()));
  ui.drawStatusBar(itsSource->isImage(), isShape, pgMode);
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

namespace
{
std::string formatFileSize(std::uintmax_t bytes)
{
  // Human-readable: KB / MB / GB up to 3 significant digits.
  if (bytes < 1024) return fmt::format("{} B", bytes);
  const double kb = bytes / 1024.0;
  if (kb < 1024.0) return fmt::format("{:.1f} KB", kb);
  const double mb = kb / 1024.0;
  if (mb < 1024.0) return fmt::format("{:.1f} MB", mb);
  return fmt::format("{:.2f} GB", mb / 1024.0);
}

std::string formatMetTime(const NFmiMetTime& t)
{
  if (t.GetYear() < 2000) return {};
  return fmt::format("{:04}-{:02}-{:02} {:02}:{:02} UTC",
                     static_cast<int>(t.GetYear()), static_cast<int>(t.GetMonth()),
                     static_cast<int>(t.GetDay()), static_cast<int>(t.GetHour()),
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
  for (auto&& kv : itsSource->extraMetadata()) rows.push_back(std::move(kv));

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

  // Levels.
  const std::size_t nl = itsSource->levelCount();
  if (nl > 0)
  {
    float lo = itsSource->levelValueAt(0);
    float hi = lo;
    for (std::size_t i = 1; i < nl; ++i)
    {
      const float v = itsSource->levelValueAt(i);
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
    if (nl == 1)
      rows.emplace_back("Levels", fmt::format("1 ({:g})", lo));
    else
      rows.emplace_back("Levels", fmt::format("{} ({:g}..{:g})", nl, lo, hi));
  }

  rows.emplace_back("", "");

  // Parameter listing — name + units, comma-separated. The popup truncates
  // on the right with an ellipsis if it's wider than the popup allows.
  std::string paramsLine;
  for (std::size_t i = 0; i < itsParamIds.size(); ++i)
  {
    if (!paramsLine.empty()) paramsLine += ", ";
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
  if (!paramsLine.empty()) rows.emplace_back("", paramsLine);
  return rows;
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
  activePanel().paramIndex = newIndex;
  itsSource->selectParamId(itsParamIds[newIndex]);
  loadPalette();  // re-resolve palette for the active panel's new parameter
}

void App::selectLevel(int newIndex)
{
  if (newIndex < 0 || newIndex >= static_cast<int>(itsSource->levelCount())) return;
  itsSource->selectLevelIndex(static_cast<unsigned long>(newIndex));
  activePanel().levelIndex = static_cast<std::size_t>(newIndex);
  itsOpts.levelIndex = newIndex;
}

std::vector<PanelRect> App::currentPanelRects(int row, int col, int height, int width) const
{
  if (width <= 0 || height <= 0) return {};
  const PanelRect full{row, col, height, width};
  switch (itsPanelLayout)
  {
    case PanelLayout::Single:
      return {full};
    case PanelLayout::Side:
    {
      if (width < 4) return {full};
      const int leftW = (width - 1) / 2;
      const int rightW = width - 1 - leftW;
      return {
          {row, col, height, leftW},
          {row, col + leftW + 1, height, rightW},
      };
    }
    case PanelLayout::Quad:
    {
      if (width < 4 || height < 4) return {full};
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
  if (idx < 0 || idx >= static_cast<int>(itsPanels.size())) return;
  if (idx == itsActivePanel) return;
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
  if (n <= 1) return;
  const int next = ((itsActivePanel + step) % n + n) % n;
  setActivePanel(next);
}

void App::cyclePanelLayout()
{
  PanelLayout next = PanelLayout::Single;
  switch (itsPanelLayout)
  {
    case PanelLayout::Single: next = PanelLayout::Side; break;
    case PanelLayout::Side:   next = PanelLayout::Quad; break;
    case PanelLayout::Quad:   next = PanelLayout::Single; break;
  }
  setPanelLayout(next);
}

void App::setPanelLayout(PanelLayout layout)
{
  itsPanelLayout = layout;
  std::size_t want = 1;
  switch (layout)
  {
    case PanelLayout::Single: want = 1; break;
    case PanelLayout::Side:   want = 2; break;
    case PanelLayout::Quad:   want = 4; break;
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
      if (n > 0) p.paramIndex = (base.paramIndex + slot) % n;
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
    if (itsActivePanel >= static_cast<int>(itsPanels.size())) itsActivePanel = 0;
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
  if (!cellToViewport(ui, cellX, cellY, u, v)) return false;
  itsSource->uvToLatLon(u, v, lat, lon);
  return true;
}

void App::openProbe(int cellX, int cellY, UI& ui)
{
  // The probe is a time-series chart of the scalar value at the clicked
  // (lat, lon) over every available timestep. RGB triplets from a raw
  // image have no scalar interpretation — silently no-op.
  if (itsSource->isImage()) return;
  double lat = 0;
  double lon = 0;
  if (!cellToLatLon(ui, cellX, cellY, lat, lon)) return;
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
      if (!cellToLatLon(ui, click->x, click->y, nlat, nlon)) break;
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
    auto computeStats = [this]() {
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
    int finalIdx = ui.popupTimeseries(param, lat, lon, series, timeLabels,
                                      static_cast<int>(savedTime), itsRenderer,
                                      activePanel().palette, onTimeChange, computeStats,
                                      units, &itsAnimationDelayMs,
                                      avoidRow, avoidCol, &clickRow, &clickCol);
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
      case 'p': case 'P':
      case 'L':
      case 'g': case 'G':
      case 'b': case 'B':
      case 'c': case 'C':
      case 'n': case 'N':
      case 'w': case 'W':
      case 'i': case 'I':
      case '/':
      case 'x': case 'X':
        itsLastMessage = "Not available in image mode";
        return true;
    }
  }
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
      int picked = ui.popupMenu("Parameters", paramLabels(), activePanel().paramIndex);
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
        itsLastMessage =
            itsShapePaletteMode == 1 ? "Palette: rainbow" : "Palette: flat";
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
      // Re-open the PostGIS layer picker without quitting. Only
      // available when launched with --pg; the persistent dataset
      // means there's no libpq round-trip on each invocation.
      if (itsPgDataset == nullptr)
      {
        itsLastMessage = "Layer picker is only available with --pg";
        return true;
      }
      // Reset state that's specific to the previous layer, then
      // re-pick. If the user cancels, we keep the current source.
      auto saved = std::move(itsSource);
      try
      {
        if (!openPgPicker(ui))
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
      // Reset per-layer state so the new layer renders cleanly.
      itsShapeOutlines.clear();
      itsViewport.reset();
      itsMarker.reset();
      itsCrossActive = false;
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
      auto utf8Len = [](const std::string& s) {
        int w = 0;
        for (unsigned char c : s)
          if ((c & 0xC0) != 0x80) ++w;
        return w;
      };
      auto padRight = [&](const std::string& s, int w) {
        const int len = utf8Len(s);
        // Overlong values are left as-is (the popup clips on the
        // right edge); shorter values get spaces added.
        if (len >= w) return s;
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
      for (auto& w : colW) w = std::min(w, 24);
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
      auto matcher = [&](const std::string& query) {
        std::vector<std::string> hits;
        lastMatchIdx.clear();
        // Case-insensitive substring search across the whole row.
        std::string q = query;
        std::transform(q.begin(), q.end(), q.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        for (int i = 0; i < n; ++i)
        {
          if (q.empty())
          {
            hits.push_back(rows[i]);
            lastMatchIdx.push_back(i);
            continue;
          }
          std::string lower = rows[i];
          std::transform(lower.begin(), lower.end(), lower.begin(),
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
        itsLastMessage =
            std::string("Shape outlines: ") + lineStyleLabel(itsShapeOutlineStyle);
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
      if (itsCrossPicks > 0)
      {
        itsCrossPicks = 0;
        itsLastMessage = "Cross-section cancelled";
      }
      else if (itsCrossActive)
      {
        itsCrossActive = false;
        itsLastMessage = "Cross-section closed";
      }
      else
      {
        itsCrossPicks = 2;
        itsLastMessage = "Cross-section: click first endpoint";
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
    case '3':
    case '4':
      setActivePanel(key - '1');
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

      // Any left-button click on a panel makes that panel active. Wheel /
      // motion events leave the focus alone.
      const auto kButton1Click = static_cast<mmask_t>(
          BUTTON1_PRESSED | BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED);
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
          beginCrossSection(itsCrossX1, itsCrossY1, ev.x, ev.y, ui);
          itsCrossPicks = 0;
          if (itsCrossActive) itsLastMessage.clear();
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

  // Re-pick coastline resolution for the current viewport. Cheap when the
  // selected file is unchanged; reads ~100ms at h-resolution worst case.
  loadCoastlines();

  const auto rects = currentPanelRects(l.map.row, l.map.col, l.map.height, l.map.width);

  std::ostringstream os;
  const int savedActive = itsActivePanel;

  for (std::size_t i = 0; i < rects.size() && i < itsPanels.size(); ++i)
  {
    const PanelRect& r = rects[i];
    if (r.width < 2 || r.height < 2) continue;

    Panel& panel = itsPanels[i];
    if (panel.paramIndex >= 0 && panel.paramIndex < static_cast<int>(itsParamIds.size()))
      itsSource->selectParamId(itsParamIds[panel.paramIndex]);
    if (panel.levelIndex < itsSource->levelCount())
      itsSource->selectLevelIndex(panel.levelIndex);

    // sampleSlice() / transform() / overlays read activePanel(); make this
    // panel active for the duration of its render.
    itsActivePanel = static_cast<int>(i);

    const int subW = r.width * 2;
    const int subH = r.height * subRowsForStyle(itsCornerStyle);
    float dMin = 0;
    float dMax = 0;
    auto pixels = sampleSlice(subW, subH, dMin, dMax);
    if (itsShowGraticule) overlayGraticule(pixels, subW, subH);
    // Thick mode rasterises into the data buffer before the renderer so the
    // line shows as a half-cell quadrant block.
    if (itsCoastlineStyle == LineStyle::Thick)
      overlayPolylines(pixels, subW, subH, itsCoastlines, Rgb{0, 0, 0});
    if (itsBorderStyle == LineStyle::Thick)
      overlayPolylines(pixels, subW, subH, itsBorders, Rgb{90, 90, 90});
    if (itsShapeOutlineStyle == LineStyle::Thick)
      overlayPolylines(pixels, subW, subH, itsShapeOutlines, borderColor());
    overlayCities(pixels, subW, subH);
    overlayMarker(pixels, subW, subH);

    itsRenderer.render(os, pixels, subW, subH, r.row, r.col);

    // Braille mode draws on top of the rendered quadrant blocks so the
    // line is just a few dots wide; data colour shows through behind it.
    if (itsCoastlineStyle == LineStyle::Braille)
      appendPolylineBraille(os, itsCoastlines, Rgb{0, 0, 0}, pixels, subW, r.row, r.col);
    if (itsBorderStyle == LineStyle::Braille)
      appendPolylineBraille(os, itsBorders, Rgb{90, 90, 90}, pixels, subW, r.row, r.col);
    if (itsShapeOutlineStyle == LineStyle::Braille)
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
      if (r.width < 4 || r.height < 1) continue;
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
      os << "\x1b[" << (r.row + 1) << ';' << (r.col + 1) << 'H' << style << visible
         << "\x1b[0m";
    }
  }

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
  std::cout << "[qdless] " << itsOpts.filename << " | param: " << shortName << " | time: "
            << currentTimeLabel() << " (" << (itsSource->currentTimeIndex() + 1) << "/"
            << itsSource->timeCount() << ")";
  if (!origLabel.empty()) std::cout << " | analysis: " << origLabel;
  std::cout << " | level: " << itsSource->levelValueAt(itsSource->currentLevelIndex())
            << " (" << (itsSource->currentLevelIndex() + 1) << "/" << itsSource->levelCount()
            << ") | range: [" << dataMin << ", " << dataMax
            << "] | palette: " << activePanel().palette.name()
            << " | coast: " << itsCoastlines.size() << "+" << itsBorders.size() << " polylines\n";

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

int App::runInteractive()
{
  UI ui;

  // Deferred PostGIS layer pick: when launched with --pg but no
  // --table, the constructor stopped after opening the connection
  // (popups need the UI). Run the picker now; openPgLayer fills
  // itsSource and we follow up with the same source-dependent init
  // the file path runs in the ctor.
  if (itsSource == nullptr && itsPgDataset != nullptr)
  {
    if (!openPgPicker(ui)) return 0;  // user cancelled the picker
    initFromSource();
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
