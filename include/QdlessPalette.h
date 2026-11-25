#pragma once

#include <newbase/NFmiGlobals.h>

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Qdless
{
// Treat sentinel/garbage values as missing. Catches kFloatMissing (32700),
// GRIB's 9.999e20, NetCDF's NC_FILL_FLOAT and accidental uninitialised reads
// (±1e29). The threshold must stay well above real values: accumulated
// radiation fields in J/m² reach 1e8 over a long forecast. grid-files'
// ParamValueMissing (-16777216) is mapped to NaN by GridFilesSource.
inline bool isMissingValue(float value)
{
  return value == kFloatMissing || !std::isfinite(value) || std::abs(value) > 1e10F;
}

struct Rgb
{
  std::uint8_t r = 0;
  std::uint8_t g = 0;
  std::uint8_t b = 0;
  // When true, the renderer leaves the cell at the terminal's default
  // background instead of painting a colour. Used for "no data" / "below
  // palette threshold" sub-cells.
  bool transparent = false;
};

class Palette
{
 public:
  struct Band
  {
    std::optional<float> lo;
    std::optional<float> hi;
    Rgb rgb{};
    // Optional human-readable label rendered by popupLegend in place
    // of the formatted "lo .. hi" range. Used by ShapeSource to label
    // each rainbow band by the feature's NAME/NIMI (or "#N").
    std::string label;
  };

  static Palette loadFromFile(const std::string& path);
  // Continuous ramp over [dataMin, dataMax], used when no named palette
  // applies. Colours are interpolated per value (values outside the range
  // clamp to the end colours); the bands only describe it for the legend.
  static Palette builtinRamp(float dataMin, float dataMax);
  // Flat-fill palette: any value ≥ 0.5 paints `color`, anything below
  // is transparent. Used by the shapefile backend so non-feature cells
  // leave the terminal background showing.
  static Palette flatFill(Rgb color, std::string name = "shape-flat");
  // Cycling rainbow palette: integer indices 1..N each get a distinct
  // hue (golden-ratio cycling so adjacent indices look different); 0
  // is transparent. Used by ShapeSource with --color-by feature.
  static Palette rainbowCycle(int n, std::string name = "shape-rainbow");

  Rgb lookup(float value) const;
  bool empty() const { return itsBands.empty(); }
  const std::string& name() const { return itsName; }
  void setName(std::string name) { itsName = std::move(name); }
  const std::vector<Band>& bands() const { return itsBands; }
  // Mutable bands accessor; used by ShapeSource to assemble a
  // labelled rainbow palette without hand-rolling the band list
  // through reflection.
  std::vector<Band>& bands() { return itsBands; }
  // "Missing" / "below palette range" cells render as the terminal's
  // default background (looks like normal terminal text area).
  static Rgb missingColor() { return {0, 0, 0, true}; }

 private:
  std::string itsName;
  std::vector<Band> itsBands;
  // Continuous colour stops evenly spaced over [itsRampMin, itsRampMax].
  // When set, lookup() interpolates these instead of matching the bands.
  std::vector<Rgb> itsRampStops;
  float itsRampMin = 0;
  float itsRampMax = 0;
};
}  // namespace Qdless
