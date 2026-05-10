#include "QdlessPalette.h"

#include <newbase/NFmiGlobals.h>

#include <json/json.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <stdexcept>

namespace Qdless
{
namespace
{
std::optional<float> readBound(const Json::Value& v)
{
  if (v.isNull()) return std::nullopt;
  if (!v.isNumeric()) return std::nullopt;
  return static_cast<float>(v.asDouble());
}

Rgb readRgb(const Json::Value& v)
{
  if (!v.isArray() || v.size() != 3)
    throw std::runtime_error("palette: rgb must be a 3-element array");
  return Rgb{static_cast<std::uint8_t>(v[0].asUInt()),
             static_cast<std::uint8_t>(v[1].asUInt()),
             static_cast<std::uint8_t>(v[2].asUInt())};
}

Rgb interpolate(const Rgb& a, const Rgb& b, float t)
{
  t = std::clamp(t, 0.0F, 1.0F);
  auto blend = [t](std::uint8_t x, std::uint8_t y) {
    return static_cast<std::uint8_t>(x + t * (static_cast<int>(y) - static_cast<int>(x)));
  };
  return Rgb{blend(a.r, b.r), blend(a.g, b.g), blend(a.b, b.b)};
}
}  // namespace

Palette Palette::loadFromFile(const std::string& path)
{
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot open palette file: " + path);

  Json::CharReaderBuilder builder;
  Json::Value root;
  std::string errs;
  if (!Json::parseFromStream(builder, in, &root, &errs))
    throw std::runtime_error("palette: parse error in " + path + ": " + errs);

  Palette p;
  p.itsName = root.get("name", "").asString();
  const auto& bands = root["isobands"];
  if (!bands.isArray()) throw std::runtime_error("palette: missing isobands array in " + path);

  p.itsBands.reserve(bands.size());
  for (const auto& b : bands)
    p.itsBands.push_back(Band{readBound(b["lo"]), readBound(b["hi"]), readRgb(b["rgb"])});
  return p;
}

Palette Palette::builtinRamp(float dataMin, float dataMax)
{
  // Twelve-stop blue -> cyan -> green -> yellow -> orange -> red ramp.
  static constexpr std::array<Rgb, 12> stops = {{{20, 20, 80}, {30, 60, 160}, {40, 110, 220},
                                                 {60, 170, 240}, {120, 220, 240}, {180, 240, 200},
                                                 {220, 240, 140}, {240, 220, 80}, {240, 170, 40},
                                                 {230, 110, 30}, {200, 50, 30}, {130, 20, 30}}};
  constexpr int N = static_cast<int>(stops.size());

  Palette p;
  p.itsName = "<builtin>";
  if (!std::isfinite(dataMin) || !std::isfinite(dataMax) || dataMax <= dataMin)
  {
    p.itsBands.push_back(Band{std::nullopt, std::nullopt, stops[N / 2]});
    return p;
  }

  const float step = (dataMax - dataMin) / (N - 1);
  p.itsBands.reserve(N);
  for (int i = 0; i < N - 1; ++i)
  {
    float lo = dataMin + i * step;
    float hi = dataMin + (i + 1) * step;
    Rgb mid = interpolate(stops[i], stops[i + 1], 0.5F);
    p.itsBands.push_back(Band{lo, hi, mid});
  }
  return p;
}

Palette Palette::flatFill(Rgb color, std::string name)
{
  // Single band [0.5, +∞). Anything below 0.5 falls through to the
  // missing-value path in lookup() and renders transparent. Used by
  // ShapeSource where interpolatedValue returns 0 outside polygons
  // and 1..N (a feature ID) inside.
  Palette p;
  p.itsName = std::move(name);
  p.itsBands.push_back(Band{0.5F, std::nullopt, color});
  return p;
}

Palette Palette::rainbowCycle(int n, std::string name)
{
  // One band per feature id (1..n inclusive), each at a hue rotated
  // by the golden-angle so adjacent ids look maximally different.
  // Saturation/value are fixed; transparent below 0.5 like flatFill.
  Palette p;
  p.itsName = std::move(name);
  if (n <= 0) return p;
  p.itsBands.reserve(static_cast<std::size_t>(n));
  // HSV → RGB (S=0.7, V=0.9). Hue in turns ∈ [0,1).
  auto hsvToRgb = [](float hueTurns, float s, float v) -> Rgb {
    const float h = hueTurns - std::floor(hueTurns);
    const int i = static_cast<int>(h * 6.0F);
    const float f = h * 6.0F - i;
    const float p1 = v * (1 - s);
    const float q = v * (1 - f * s);
    const float t = v * (1 - (1 - f) * s);
    float r = 0;
    float g = 0;
    float b = 0;
    switch (i % 6)
    {
      case 0: r = v; g = t; b = p1; break;
      case 1: r = q; g = v; b = p1; break;
      case 2: r = p1; g = v; b = t; break;
      case 3: r = p1; g = q; b = v; break;
      case 4: r = t; g = p1; b = v; break;
      default: r = v; g = p1; b = q; break;
    }
    return Rgb{static_cast<std::uint8_t>(r * 255),
               static_cast<std::uint8_t>(g * 255),
               static_cast<std::uint8_t>(b * 255)};
  };
  constexpr float kGolden = 0.61803398875F;
  for (int i = 0; i < n; ++i)
  {
    const float hue = std::fmod(0.05F + static_cast<float>(i) * kGolden, 1.0F);
    const float lo = static_cast<float>(i) + 0.5F;
    const float hi = static_cast<float>(i) + 1.5F;
    p.itsBands.push_back(Band{lo, hi, hsvToRgb(hue, 0.7F, 0.9F)});
  }
  return p;
}

Rgb Palette::lookup(float value) const
{
  // Treat sentinel/garbage values as missing. Catches kFloatMissing (32700),
  // grid-files' ParamValueMissing (-16777216 ≈ -1.67e7, returned for
  // out-of-grid points by getGridValueByLatLonCoordinate), GRIB's 9.999e20,
  // NetCDF's NC_FILL_FLOAT, and accidental uninitialised reads (±1e29).
  // Threshold 1e6 is well above any real meteorological value (max ~5e4 for
  // geopotential height) and well below typical sentinels.
  if (value == kFloatMissing || !std::isfinite(value) || std::abs(value) > 1e6F)
    return missingColor();
  if (itsBands.empty()) return missingColor();

  for (const auto& b : itsBands)
  {
    bool aboveLo = !b.lo.has_value() || value >= *b.lo;
    bool belowHi = !b.hi.has_value() || value < *b.hi;
    if (aboveLo && belowHi) return b.rgb;
  }
  // No band matched. Open-ended bands (lo or hi null) already match in the
  // loop above, so falling through means the value is genuinely below the
  // lowest closed lo or above the highest closed hi — i.e. the palette has
  // chosen not to colour it. Render as missing (terminal default bg).
  return missingColor();
}
}  // namespace Qdless
