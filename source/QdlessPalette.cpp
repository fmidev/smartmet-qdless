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

Rgb Palette::lookup(float value) const
{
  // Treat sentinel/garbage values as missing. Catches kFloatMissing (32700)
  // plus large-magnitude sentinels like 9.999e20 (GRIB) or accidental
  // uninitialised reads showing up as ±1e29.
  if (value == kFloatMissing || !std::isfinite(value) || std::abs(value) > 1e10F)
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
