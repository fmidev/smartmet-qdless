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
  };

  static Palette loadFromFile(const std::string& path);
  static Palette builtinRamp(float dataMin, float dataMax);

  Rgb lookup(float value) const;
  bool empty() const { return itsBands.empty(); }
  const std::string& name() const { return itsName; }
  const std::vector<Band>& bands() const { return itsBands; }
  // "Missing" / "below palette range" cells render as the terminal's
  // default background (looks like normal terminal text area).
  static Rgb missingColor() { return {0, 0, 0, true}; }

 private:
  std::string itsName;
  std::vector<Band> itsBands;
};
}  // namespace Qdless
