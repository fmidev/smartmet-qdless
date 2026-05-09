#include "QdlessRenderer.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

namespace Qdless
{
namespace
{
// 16 quadrant-block glyphs indexed by a 4-bit mask: bit0=TL, bit1=TR, bit2=BL,
// bit3=BR. The "lit" pixels are foreground; the "unlit" pixels are background.
constexpr std::array<const char*, 16> kQuadrant = {
    " ", "▘", "▝", "▀", "▖", "▌", "▞", "▛",
    "▗", "▚", "▐", "▜", "▄", "▙", "▟", "█"};

// 1/12-cell corner triangles from Symbols for Legacy Computing (U+1FB00).
// Each glyph fills only a small triangle hugging one cell-corner (hypotenuse
// from edge-midpoint to the 1/3 mark on the adjacent edge). For lone-fg
// masks (1/2/4/8) the small filled region IS the lone — emit as-is. For
// lone-bg masks (7/11/13/14) the small region needs to show the lone *bg*
// colour, so we swap fg/bg on output (no all-but-corner complement glyph
// exists). Indexed by the same 4-bit mask as kQuadrant.
constexpr std::array<const char*, 16> kSmallTriangle = {
    nullptr, "🭗", "🭢", nullptr, "🬼", nullptr, nullptr, "🭇",
    "🭇",    nullptr, nullptr, "🭢", nullptr, "🬼", "🭗", nullptr};

constexpr bool smallTriangleSwapsColors(std::uint8_t mask)
{
  return mask == 7 || mask == 11 || mask == 13 || mask == 14;
}

// 64 sextant glyphs indexed by 6-bit mask:
//   bit0=TL, bit1=TR, bit2=ML, bit3=MR, bit4=BL, bit5=BR.
// 60 patterns live in the dedicated U+1FB00..U+1FB3B block (Symbols for
// Legacy Computing). The remaining 4 are encoded elsewhere as block
// elements: empty (space), all-on (full block), left half, right half.
// Build the table once at first use.
const std::array<const char*, 64>& sextantTable()
{
  static const auto kTable = []() {
    std::array<const char*, 64> t{};
    static std::array<std::string, 64> store;
    for (int m = 0; m < 64; ++m)
    {
      const char* literal = nullptr;
      if (m == 0) literal = " ";
      else if (m == 21) literal = "▌";  // U+258C left half
      else if (m == 42) literal = "▐";  // U+2590 right half
      else if (m == 63) literal = "█";  // U+2588 full
      if (literal != nullptr)
      {
        t[m] = literal;
        continue;
      }
      // Sextant glyphs: U+1FB00 holds mask=1; codepoints jump over the
      // four masks reserved for block elements above.
      const int skip = (m > 21) + (m > 42);
      const unsigned cp = 0x1FB00U + static_cast<unsigned>(m - 1 - skip);
      // Encode the 4-byte UTF-8 sequence for U+1Fxxx.
      std::string& s = store[m];
      s.resize(4);
      s[0] = static_cast<char>(0xF0 | (cp >> 18));
      s[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      s[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      s[3] = static_cast<char>(0x80 | (cp & 0x3F));
      t[m] = s.c_str();
    }
    return t;
  }();
  return kTable;
}

bool detectTruecolor()
{
  // NOLINTNEXTLINE(concurrency-mt-unsafe) - called once at construction.
  const char* ct = std::getenv("COLORTERM");
  if (ct == nullptr) return false;
  return std::strcmp(ct, "truecolor") == 0 || std::strcmp(ct, "24bit") == 0;
}

// xterm-256 quantization: 6x6x6 color cube indices 16..231.
int xterm256(const Rgb& c)
{
  auto step = [](std::uint8_t v) {
    static constexpr std::array<int, 6> levels = {0, 95, 135, 175, 215, 255};
    int best = 0;
    int bestDist = 256 * 256;
    for (int i = 0; i < 6; ++i)
    {
      int d = (static_cast<int>(v) - levels[i]) * (static_cast<int>(v) - levels[i]);
      if (d < bestDist)
      {
        bestDist = d;
        best = i;
      }
    }
    return best;
  };
  int r = step(c.r);
  int g = step(c.g);
  int b = step(c.b);
  // Detect grayscale candidates and check if grayscale ramp is closer.
  if (std::abs(c.r - c.g) < 8 && std::abs(c.g - c.b) < 8)
  {
    int gray = (static_cast<int>(c.r) + c.g + c.b) / 3;
    int idx = std::clamp((gray - 8) / 10, 0, 23);
    int grayLevel = 8 + idx * 10;
    int dGray = (gray - grayLevel) * (gray - grayLevel);
    static constexpr std::array<int, 6> levels = {0, 95, 135, 175, 215, 255};
    int dCube = (c.r - levels[r]) * (c.r - levels[r]) + (c.g - levels[g]) * (c.g - levels[g]) +
                (c.b - levels[b]) * (c.b - levels[b]);
    if (dGray * 3 < dCube) return 232 + idx;
  }
  return 16 + 36 * r + 6 * g + b;
}

// Pick foreground/background for one terminal cell from its N sub-cells by
// k-means with k=2 (one iteration is enough at this scale). N is 4 for
// quadrant rendering, 6 for sextant, 8 for octant. Mask uses bit i to mark
// sub-cell i as belonging to the fg cluster.
void splitCell(const Rgb* q, int n, Rgb& fg, Rgb& bg, std::uint8_t& mask)
{
  auto luma = [](const Rgb& c) { return 30 * c.r + 59 * c.g + 11 * c.b; };
  int hi = 0;
  int lo = 0;
  for (int i = 1; i < n; ++i)
  {
    if (luma(q[i]) > luma(q[hi])) hi = i;
    if (luma(q[i]) < luma(q[lo])) lo = i;
  }
  fg = q[hi];
  bg = q[lo];
  auto dist2 = [](const Rgb& a, const Rgb& b) {
    int dr = static_cast<int>(a.r) - b.r;
    int dg = static_cast<int>(a.g) - b.g;
    int db = static_cast<int>(a.b) - b.b;
    return dr * dr + dg * dg + db * db;
  };
  mask = 0;
  std::array<int, 2> sumR = {0, 0};
  std::array<int, 2> sumG = {0, 0};
  std::array<int, 2> sumB = {0, 0};
  std::array<int, 2> count = {0, 0};
  for (int i = 0; i < n; ++i)
  {
    int side = dist2(q[i], fg) < dist2(q[i], bg) ? 1 : 0;
    if (side == 1) mask |= static_cast<std::uint8_t>(1U << i);
    sumR[side] += q[i].r;
    sumG[side] += q[i].g;
    sumB[side] += q[i].b;
    ++count[side];
  }
  if (count[0] > 0)
    bg = Rgb{static_cast<std::uint8_t>(sumR[0] / count[0]),
             static_cast<std::uint8_t>(sumG[0] / count[0]),
             static_cast<std::uint8_t>(sumB[0] / count[0])};
  if (count[1] > 0)
    fg = Rgb{static_cast<std::uint8_t>(sumR[1] / count[1]),
             static_cast<std::uint8_t>(sumG[1] / count[1]),
             static_cast<std::uint8_t>(sumB[1] / count[1])};
}
}  // namespace

Renderer::Renderer() : itsTruecolor(detectTruecolor()) {}

std::string Renderer::bgEscape(Rgb c) const
{
  std::ostringstream os;
  if (c.transparent)
  {
    os << "\x1b[49m";
    return os.str();
  }
  if (itsTruecolor)
  {
    os << "\x1b[48;2;" << static_cast<int>(c.r) << ';' << static_cast<int>(c.g) << ';'
       << static_cast<int>(c.b) << 'm';
  }
  else
  {
    os << "\x1b[48;5;" << xterm256(c) << 'm';
  }
  return os.str();
}

std::string Renderer::fgEscape(Rgb c) const
{
  std::ostringstream os;
  if (c.transparent)
  {
    os << "\x1b[39m";
    return os.str();
  }
  if (itsTruecolor)
  {
    os << "\x1b[38;2;" << static_cast<int>(c.r) << ';' << static_cast<int>(c.g) << ';'
       << static_cast<int>(c.b) << 'm';
  }
  else
  {
    os << "\x1b[38;5;" << xterm256(c) << 'm';
  }
  return os.str();
}

void Renderer::render(std::ostream& os,
                      const std::vector<Rgb>& pixels,
                      int subWidth,
                      int subHeight,
                      int originRow,
                      int originCol) const
{
  const int subRows = subRowsForStyle(itsCornerStyle);
  const int cellW = subWidth / 2;
  const int cellH = subHeight / subRows;
  const int n = subRows * 2;  // sub-cells per terminal cell (4 or 6)
  const bool sextant = (itsCornerStyle == CornerStyle::Sextant);
  const Rgb missing = Palette::missingColor();

  // Glyph for an N-bit mask in the current style. SmallTriangle bevels are
  // applied separately at the 3:1-mask level on quadrant modes.
  auto baseGlyph = [&](std::uint8_t m) -> const char* {
    return sextant ? sextantTable()[m] : kQuadrant[m];
  };

  for (int cy = 0; cy < cellH; ++cy)
  {
    os << "\x1b[" << (originRow + cy + 1) << ';' << (originCol + 1) << 'H';
    for (int cx = 0; cx < cellW; ++cx)
    {
      std::array<Rgb, 8> q{};  // up to 8 sub-cells (octant); we use n of them
      for (int sy = 0; sy < subRows; ++sy)
      {
        for (int sx = 0; sx < 2; ++sx)
        {
          int x = cx * 2 + sx;
          int y = cy * subRows + sy;
          int idx = y * subWidth + x;
          q[sy * 2 + sx] =
              (idx >= 0 && idx < static_cast<int>(pixels.size())) ? pixels[idx] : missing;
        }
      }
      // Count transparent sub-cells: those render as the terminal's default
      // background (no colour), so we treat them differently from opaque
      // colours — never use them as a "real" colour in k-means.
      int nTrans = 0;
      for (int i = 0; i < n; ++i)
        if (q[i].transparent) ++nTrans;

      if (nTrans == n)
      {
        // Fully transparent cell: blank, no colour.
        os << "\x1b[0m ";
        continue;
      }
      if (nTrans > 0)
      {
        // Mixed: opaque sub-cells become the foreground, transparent ones
        // are background (terminal default).
        std::uint8_t mixedMask = 0;
        int sumR = 0;
        int sumG = 0;
        int sumB = 0;
        int count = 0;
        for (int i = 0; i < n; ++i)
        {
          if (!q[i].transparent)
          {
            mixedMask |= static_cast<std::uint8_t>(1U << i);
            sumR += q[i].r;
            sumG += q[i].g;
            sumB += q[i].b;
            ++count;
          }
        }
        Rgb fgMean{static_cast<std::uint8_t>(sumR / count),
                   static_cast<std::uint8_t>(sumG / count),
                   static_cast<std::uint8_t>(sumB / count)};
        os << "\x1b[49m" << fgEscape(fgMean) << baseGlyph(mixedMask);
        continue;
      }

      Rgb fg{};
      Rgb bg{};
      std::uint8_t mask = 0;
      splitCell(q.data(), n, fg, bg, mask);

      const char* glyph = nullptr;
      bool swapColors = false;
      if (itsCornerStyle == CornerStyle::SmallTriangle)
      {
        glyph = kSmallTriangle[mask];
        swapColors = (glyph != nullptr) && smallTriangleSwapsColors(mask);
      }
      if (glyph == nullptr) glyph = baseGlyph(mask);

      const Rgb& outFg = swapColors ? bg : fg;
      const Rgb& outBg = swapColors ? fg : bg;
      if (itsTruecolor)
      {
        os << "\x1b[38;2;" << static_cast<int>(outFg.r) << ';' << static_cast<int>(outFg.g) << ';'
           << static_cast<int>(outFg.b) << "m\x1b[48;2;" << static_cast<int>(outBg.r) << ';'
           << static_cast<int>(outBg.g) << ';' << static_cast<int>(outBg.b) << 'm';
      }
      else
      {
        os << "\x1b[38;5;" << xterm256(outFg) << "m\x1b[48;5;" << xterm256(outBg) << 'm';
      }
      os << glyph;
    }
    os << "\x1b[0m";
  }
  os << "\x1b[0m";
}
}  // namespace Qdless
