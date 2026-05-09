#pragma once

#include "QdlessPalette.h"

#include <cstdint>
#include <ostream>
#include <vector>

namespace Qdless
{
// Per-cell rendering style. Cycled with the `t` key. All modes emit one
// fg + one bg colour per terminal cell (terminal limitation); they differ
// in sub-pixel grid density and glyph repertoire:
//   Sextant       — 2×3 sub-pixels, 64 sextant glyphs (U+1FB00..1FB3B plus
//                   space/full/halves). 1.5× vertical resolution and
//                   near-square sub-pixels in a typical 1:2 terminal cell.
//                   Default. Needs the Symbols-for-Legacy-Computing block
//                   (Unicode 13, 2020); newer fonts only.
//   SmallTriangle — 2×2 sub-pixels with 16 quadrant block glyphs, plus
//                   1/12-cell corner-triangle bevels (U+1FB57 etc.)
//                   substituted on 3:1 cells. Same font requirement as
//                   Sextant (the bevels also live in U+1FB00..U+1FBFF).
//   Square        — 2×2 sub-pixels with the 16 quadrant block glyphs only.
//                   Universal fallback; the Block Elements range
//                   (U+2580..259F) ships with every monospace font.
enum class CornerStyle : std::uint8_t
{
  Sextant,
  SmallTriangle,
  Square,
};

// Sub-pixel rows per terminal cell for a given style. Width is always 2.
constexpr int subRowsForStyle(CornerStyle s)
{
  return s == CornerStyle::Sextant ? 3 : 2;
}

// Renders an Rgb buffer to a UTF-8 terminal using quadrant-block glyphs (one
// terminal cell carries a 2x2 sub-pixel grid). Detects truecolor vs xterm-256
// via the COLORTERM environment variable.
class Renderer
{
 public:
  Renderer();

  // Width/height are in *sub-cells* (so terminal cell width = subWidth/2,
  // cell height = subHeight/2). pixels is row-major, length subWidth*subHeight.
  // originRow/originCol position the output at an absolute screen location
  // using ESC[r;cH cursor moves; row 0 col 0 = top-left of the terminal.
  void render(std::ostream& os,
              const std::vector<Rgb>& pixels,
              int subWidth,
              int subHeight,
              int originRow = 0,
              int originCol = 0) const;

  // ANSI escape for a background color in either truecolor or xterm-256.
  // Useful for drawing legend swatches outside of the main raster.
  std::string bgEscape(Rgb c) const;
  // Same for foreground.
  std::string fgEscape(Rgb c) const;

  bool truecolor() const { return itsTruecolor; }

  void setCornerStyle(CornerStyle s) { itsCornerStyle = s; }
  CornerStyle cornerStyle() const { return itsCornerStyle; }

 private:
  bool itsTruecolor;
  CornerStyle itsCornerStyle = CornerStyle::Sextant;
};
}  // namespace Qdless
