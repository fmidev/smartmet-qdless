#include "QdlessRenderer.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace Qdless
{
namespace
{
// 16 quadrant-block glyphs indexed by a 4-bit mask: bit0=TL, bit1=TR, bit2=BL,
// bit3=BR. The "lit" pixels are foreground; the "unlit" pixels are background.
constexpr std::array<const char*, 16> kQuadrant = {
    " ", "▘", "▝", "▀", "▖", "▌", "▞", "▛",
    "▗", "▚", "▐", "▜", "▄", "▙", "▟", "█"};

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

// Pick foreground/background for one terminal cell from its 4 sub-cells by
// k-means with k=2 (one iteration is enough at this scale).
void splitCell(const std::array<Rgb, 4>& q, Rgb& fg, Rgb& bg, std::uint8_t& mask)
{
  // Initial seeds: brightest and darkest of the four.
  auto luma = [](const Rgb& c) { return 30 * c.r + 59 * c.g + 11 * c.b; };
  int hi = 0;
  int lo = 0;
  for (int i = 1; i < 4; ++i)
  {
    if (luma(q[i]) > luma(q[hi])) hi = i;
    if (luma(q[i]) < luma(q[lo])) lo = i;
  }
  fg = q[hi];
  bg = q[lo];
  // Assign each sub-cell to nearer of fg/bg.
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
  for (int i = 0; i < 4; ++i)
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
  const int cellW = subWidth / 2;
  const int cellH = subHeight / 2;
  const Rgb missing = Palette::missingColor();

  for (int cy = 0; cy < cellH; ++cy)
  {
    os << "\x1b[" << (originRow + cy + 1) << ';' << (originCol + 1) << 'H';
    for (int cx = 0; cx < cellW; ++cx)
    {
      std::array<Rgb, 4> q{};
      for (int sy = 0; sy < 2; ++sy)
      {
        for (int sx = 0; sx < 2; ++sx)
        {
          int x = cx * 2 + sx;
          int y = cy * 2 + sy;
          int idx = y * subWidth + x;
          q[sy * 2 + sx] =
              (idx >= 0 && idx < static_cast<int>(pixels.size())) ? pixels[idx] : missing;
        }
      }
      // Count transparent sub-cells: those render as the terminal's default
      // background (no colour), so we treat them differently from opaque
      // colours — never use them as a "real" colour in k-means.
      int nTrans = 0;
      for (const auto& c : q)
        if (c.transparent) ++nTrans;

      if (nTrans == 4)
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
        for (int i = 0; i < 4; ++i)
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
        os << "\x1b[49m" << fgEscape(fgMean) << kQuadrant[mixedMask];
        continue;
      }

      Rgb fg{};
      Rgb bg{};
      std::uint8_t mask = 0;
      splitCell(q, fg, bg, mask);

      if (itsTruecolor)
      {
        os << "\x1b[38;2;" << static_cast<int>(fg.r) << ';' << static_cast<int>(fg.g) << ';'
           << static_cast<int>(fg.b) << "m\x1b[48;2;" << static_cast<int>(bg.r) << ';'
           << static_cast<int>(bg.g) << ';' << static_cast<int>(bg.b) << 'm';
      }
      else
      {
        os << "\x1b[38;5;" << xterm256(fg) << "m\x1b[48;5;" << xterm256(bg) << 'm';
      }
      os << kQuadrant[mask];
    }
    os << "\x1b[0m";
  }
  os << "\x1b[0m";
}
}  // namespace Qdless
