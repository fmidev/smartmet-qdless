#pragma once

#include "QdlessPalette.h"

#include <ostream>
#include <vector>

namespace Qdless
{
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

 private:
  bool itsTruecolor;
};
}  // namespace Qdless
