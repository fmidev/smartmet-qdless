#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Qdless
{
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
};
}  // namespace Qdless
