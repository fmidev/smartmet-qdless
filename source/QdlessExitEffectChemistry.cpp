#include "QdlessExitEffectCommon.h"

namespace Qdless
{
namespace ee_detail
{

void effectBenzene(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5200,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float dim = 0.15F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 36.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(16 + l * 0.08F * dim), u8(20 + l * 0.08F * dim),
                          u8(28 + l * 0.10F * dim), false};
                }
              const float cx = w * 0.5F, cy = h * 0.5F;
              const float R = mn * 0.26F;
              const float aRot = t * 1.2F;
              const Rgb cTint{200, 200, 210, false};
              const Rgb hTint{220, 110, 110, false};
              for (int i = 0; i < 6; ++i)
              {
                const float a1 = aRot + i * 6.2832F / 6.0F;
                const float a2 = aRot + (i + 1) * 6.2832F / 6.0F;
                const float c1x = cx + std::cos(a1) * R;
                const float c1y = cy + std::sin(a1) * R / ya;
                const float c2x = cx + std::cos(a2) * R;
                const float c2y = cy + std::sin(a2) * R / ya;
                drawSeg(dst, w, h, c1x, c1y, c2x, c2y, std::max(1.0F, mn * 0.005F), ya,
                        Rgb{210, 210, 210, false});
                if (i % 2 == 0)
                {
                  const float dx = c2x - c1x, dy = c2y - c1y;
                  const float nl = std::hypot(dx, dy);
                  const float nx = -dy / nl, ny = dx / nl;
                  const float oo = mn * 0.020F;
                  drawSeg(dst, w, h, c1x + nx * oo, c1y + ny * oo, c2x + nx * oo,
                          c2y + ny * oo, std::max(1.0F, mn * 0.004F), ya,
                          Rgb{200, 200, 200, false});
                }
                drawDataDisk(dst, w, h, src, c1x, c1y, mn * 0.035F, ya, 0.75F, t * 0.6F, cTint);
                const float hx = cx + std::cos(a1) * (R + mn * 0.10F);
                const float hy = cy + std::sin(a1) * (R + mn * 0.10F) / ya;
                drawDataDisk(dst, w, h, src, hx, hy, mn * 0.020F, ya, 0.70F, t * 0.6F, hTint);
              }
              for (float a = 0; a < 6.2832F; a += 0.04F)
                plotDot(dst, w, h, cx + std::cos(a) * R * 0.62F,
                        cy + std::sin(a) * R * 0.62F / ya, std::max(1.0F, mn * 0.003F), ya,
                        Rgb{160, 180, 230, false});
            });
}

void effectBuckyball(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  // 60 vertices of a truncated icosahedron — generated via the golden ratio
  // construction. Three cyclic permutations of (0, ±1, ±3φ), (±1, ±(2+φ),
  // ±2φ), (±φ, ±2, ±(2φ+1)) give all 60.
  constexpr float phi = 1.618033988F;
  std::vector<std::array<float, 3>> verts;
  auto pm = [](float v, int s) { return s == 0 ? v : -v; };
  for (int sa = 0; sa < 2; ++sa)
    for (int sb = 0; sb < 2; ++sb)
    {
      verts.push_back({0, pm(1, sa), pm(3 * phi, sb)});
      verts.push_back({pm(3 * phi, sb), 0, pm(1, sa)});
      verts.push_back({pm(1, sa), pm(3 * phi, sb), 0});
    }
  for (int sa = 0; sa < 2; ++sa)
    for (int sb = 0; sb < 2; ++sb)
      for (int sc = 0; sc < 2; ++sc)
      {
        verts.push_back({pm(1, sa), pm(2 + phi, sb), pm(2 * phi, sc)});
        verts.push_back({pm(2 * phi, sc), pm(1, sa), pm(2 + phi, sb)});
        verts.push_back({pm(2 + phi, sb), pm(2 * phi, sc), pm(1, sa)});
        verts.push_back({pm(phi, sa), pm(2, sb), pm(2 * phi + 1, sc)});
        verts.push_back({pm(2 * phi + 1, sc), pm(phi, sa), pm(2, sb)});
        verts.push_back({pm(2, sb), pm(2 * phi + 1, sc), pm(phi, sa)});
      }
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float dim = 0.10F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 20.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(10 + l * 0.06F * dim), u8(14 + l * 0.06F * dim),
                          u8(24 + l * 0.10F * dim), false};
                }
              const float cyw = std::cos(t * 1.0F), syw = std::sin(t * 1.0F);
              const float cp = std::cos(t * 0.7F), sp = std::sin(t * 0.7F);
              const float scale = mn / 12.0F;
              const float cx = w * 0.5F, cy = h * 0.5F;
              struct Pt { float sx, sy, z; };
              std::vector<Pt> pts(verts.size());
              for (std::size_t i = 0; i < verts.size(); ++i)
              {
                const float x = verts[i][0], yv = verts[i][1], z = verts[i][2];
                const float x1 = x * cyw - z * syw, z1 = x * syw + z * cyw;
                const float y2 = yv * cp - z1 * sp, z2 = yv * sp + z1 * cp;
                pts[i] = {cx + x1 * scale, cy + y2 * scale / ya, z2};
              }
              for (const auto& p : pts)
              {
                const float depth = std::clamp((p.z + 6.0F) / 12.0F, 0.2F, 1.0F);
                drawDataDisk(dst, w, h, src, p.sx, p.sy, mn * 0.022F, ya, 0.7F * depth,
                             t * 0.5F, Rgb{u8(120 * depth + 40), u8(160 * depth + 40),
                                            u8(180 * depth + 40), false});
              }
            });
}

void effectPeriodicTable(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  // Position table (row, col) for each Z=1..118. 0 means "no cell."
  // Simplified: just lay out the standard short form for visual punch.
  constexpr int kCols = 18, kRows = 7;
  static const char* const kSyms[kRows][kCols] = {
      {"H",  "",   "",   "",   "",   "",   "",   "",   "",   "",   "",   "",   "",   "",   "",   "",   "",   "He"},
      {"Li", "Be", "",   "",   "",   "",   "",   "",   "",   "",   "",   "",   "B",  "C",  "N",  "O",  "F",  "Ne"},
      {"Na", "Mg", "",   "",   "",   "",   "",   "",   "",   "",   "",   "",   "Al", "Si", "P",  "S",  "Cl", "Ar"},
      {"K",  "Ca", "Sc", "Ti", "V",  "Cr", "Mn", "Fe", "Co", "Ni", "Cu", "Zn", "Ga", "Ge", "As", "Se", "Br", "Kr"},
      {"Rb", "Sr", "Y",  "Zr", "Nb", "Mo", "Tc", "Ru", "Rh", "Pd", "Ag", "Cd", "In", "Sn", "Sb", "Te", "I",  "Xe"},
      {"Cs", "Ba", "*",  "Hf", "Ta", "W",  "Re", "Os", "Ir", "Pt", "Au", "Hg", "Tl", "Pb", "Bi", "Po", "At", "Rn"},
      {"Fr", "Ra", "*",  "Rf", "Db", "Sg", "Bh", "Hs", "Mt", "Ds", "Rg", "Cn", "Nh", "Fl", "Mc", "Lv", "Ts", "Og"}};
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float dim = 0.10F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  dst[static_cast<std::size_t>(y) * w + x] =
                      s.transparent ? Rgb{16, 16, 22, false}
                                    : Rgb{u8(s.r * dim), u8(s.g * dim + 4), u8(s.b * dim + 8), false};
                }
              const float cellW = std::min(w * 0.9F / kCols, h * 0.78F / kRows);
              const float cellH = cellW;
              const float startX = (w - cellW * kCols) * 0.5F;
              const float startY = (h - cellH * kRows) * 0.5F;
              const int total = kRows * kCols;
              const int revealed = std::min(total, static_cast<int>(t * total * 1.2F));
              for (int r = 0; r < kRows; ++r)
                for (int c = 0; c < kCols; ++c)
                {
                  const int idx = r * kCols + c;
                  if (idx >= revealed) continue;
                  const char* sym = kSyms[r][c];
                  if (sym[0] == '\0') continue;
                  const float xa = startX + c * cellW, xb = xa + cellW * 0.94F;
                  const float yya = startY + r * cellH, yyb = yya + cellH * 0.94F;
                  // Tint: sample the data at the cell's anchor and blend with
                  // a per-group hue.
                  const Rgb d = sample(src, w, h, (xa + xb) * 0.5F, (yya + yyb) * 0.5F);
                  const float dr = d.transparent ? 80.0F : d.r;
                  const float dg = d.transparent ? 80.0F : d.g;
                  const float db = d.transparent ? 120.0F : d.b;
                  const Rgb fill{u8(80 + dr * 0.45F), u8(80 + dg * 0.45F), u8(110 + db * 0.40F),
                                 false};
                  for (int yy = static_cast<int>(yya); yy <= static_cast<int>(yyb); ++yy)
                    for (int xx = static_cast<int>(xa); xx <= static_cast<int>(xb); ++xx)
                      if (xx >= 0 && xx < w && yy >= 0 && yy < h)
                        dst[static_cast<std::size_t>(yy) * w + xx] = fill;
                  // Symbol text (1-2 chars).
                  const int sc = std::max(1, static_cast<int>(cellW / 14.0F));
                  for (int ci = 0; ci < 2 && sym[ci]; ++ci)
                  {
                    const auto g = glyph5x7(sym[ci]);
                    for (int fy = 0; fy < 7; ++fy)
                      for (int fx = 0; fx < 5; ++fx)
                        if (g[fy][fx] == '1')
                          plotDot(dst, w, h, xa + cellW * 0.15F + (ci * 6 + fx) * sc,
                                  yya + cellH * 0.25F + fy * sc,
                                  std::max(1.0F, static_cast<float>(sc) * 0.5F), ya,
                                  Rgb{240, 240, 240, false});
                  }
                }
            });
}

void effectFlameTest(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  static const std::array<Rgb, 5> kFlames = {Rgb{230, 60, 70, false},
                                             Rgb{255, 220, 60, false},
                                             Rgb{200, 100, 220, false},
                                             Rgb{60, 230, 90, false},
                                             Rgb{220, 30, 60, false}};
  static const std::array<const char*, 5> kLabels = {"LI", "NA", "K", "CU", "SR"};
  runFrames(
      renderer, w, h, 5600,
      [&](float t, std::vector<Rgb>& dst)
      {
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
            dst[static_cast<std::size_t>(y) * w + x] = Rgb{12, 14, 22, false};
        const int stage = std::min(4, static_cast<int>(t * 5.0F));
        const float local = t * 5.0F - stage;
        const Rgb& flameCol = kFlames[stage];
        const float burnerCx = w * 0.5F;
        const float burnerTop = h * 0.78F;
        // Burner shaft
        for (int yy = static_cast<int>(burnerTop); yy <= static_cast<int>(h * 0.95F); ++yy)
        {
          const int half = static_cast<int>(mn * 0.020F);
          for (int xo = -half; xo <= half; ++xo)
          {
            const int xx = static_cast<int>(burnerCx + xo);
            if (xx >= 0 && xx < w && yy >= 0 && yy < h)
              dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{60, 60, 70, false};
          }
        }
        // Flame: a teardrop shape with the data sampled inside and tinted.
        const float flameH = mn * 0.50F;
        const float flameW = mn * 0.18F;
        for (int yy = static_cast<int>(burnerTop - flameH); yy < static_cast<int>(burnerTop); ++yy)
        {
          const float yf = (burnerTop - yy) / flameH;  // 0 at base, 1 at tip
          const float half = flameW * std::sin(yf * 3.14159F) * (0.9F + 0.1F * std::sin(t * 30.0F + yf * 6.0F));
          for (int xo = -static_cast<int>(half); xo <= static_cast<int>(half); ++xo)
          {
            const int xx = static_cast<int>(burnerCx + xo);
            if (xx < 0 || xx >= w || yy < 0 || yy >= h) continue;
            const Rgb d = sample(src, w, h, xx, yy);
            const float dr = d.transparent ? 100.0F : d.r;
            const float dg = d.transparent ? 100.0F : d.g;
            const float db = d.transparent ? 100.0F : d.b;
            const float mix = 0.60F;
            dst[static_cast<std::size_t>(yy) * w + xx] =
                Rgb{u8(flameCol.r * mix + dr * (1 - mix) * 0.7F),
                    u8(flameCol.g * mix + dg * (1 - mix) * 0.7F),
                    u8(flameCol.b * mix + db * (1 - mix) * 0.7F), false};
          }
        }
        // Label
        const std::string lab(kLabels[stage]);
        const int sc = std::max(3, static_cast<int>(mn / 25.0F));
        const float lineW = static_cast<float>(lab.size()) * 6 * sc;
        for (int ci = 0; ci < static_cast<int>(lab.size()); ++ci)
        {
          const auto g = glyph5x7(lab[ci]);
          for (int fy = 0; fy < 7; ++fy)
            for (int fx = 0; fx < 5; ++fx)
              if (g[fy][fx] == '1')
                plotDot(dst, w, h, burnerCx - lineW * 0.5F + (ci * 6 + fx) * sc,
                        h * 0.12F + fy * sc, std::max(1.0F, static_cast<float>(sc) * 0.5F), ya,
                        flameCol);
        }
        (void)local;
      });
}

void effectLavaLamp(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 5600,
            [&](float t, std::vector<Rgb>& dst)
            {
              // Glass envelope with warm orange backlight.
              const float cxw = w * 0.5F;
              const float lampW = mn * 0.32F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const float dx = std::fabs(x - cxw);
                  const float sf = static_cast<float>(y) / h;
                  if (dx < lampW)
                  {
                    const float glow = 1.0F - dx / lampW;
                    dst[static_cast<std::size_t>(y) * w + x] =
                        Rgb{u8(80 + 150 * glow * (1 - sf * 0.5F)),
                            u8(40 + 60 * glow * (1 - sf * 0.5F)),
                            u8(20 + 20 * glow * (1 - sf * 0.5F)), false};
                  }
                  else
                  {
                    const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                    const float l =
                        s.transparent ? 30.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                    dst[static_cast<std::size_t>(y) * w + x] =
                        Rgb{u8(20 + l * 0.10F), u8(20 + l * 0.10F), u8(28 + l * 0.10F), false};
                  }
                }
              const Rgb wax{240, 110, 40, false};
              for (int i = 0; i < 7; ++i)
              {
                const float phase = hash(i) * 6.2832F;
                const float speed = 0.4F + hash(i * 3) * 0.4F;
                const float bx = cxw + (hash(i * 7) - 0.5F) * lampW * 0.7F;
                const float by = h * (0.85F - 0.65F * (0.5F + 0.5F * std::sin(t * speed + phase)));
                const float bR = mn * (0.025F + 0.020F * hash(i * 11));
                drawDataDisk(dst, w, h, src, bx, by, bR, ya, 0.85F, t * 0.5F + i, wax);
              }
              // Lamp cap + base.
              for (int yy = 0; yy <= static_cast<int>(mn * 0.03F); ++yy)
              {
                const int xa = static_cast<int>(cxw - lampW * 1.05F);
                const int xb = static_cast<int>(cxw + lampW * 1.05F);
                for (int xx = xa; xx <= xb; ++xx)
                  if (xx >= 0 && xx < w && yy < h)
                    dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{60, 50, 40, false};
              }
              for (int yy = h - static_cast<int>(mn * 0.04F); yy < h; ++yy)
              {
                const int xa = static_cast<int>(cxw - lampW * 1.05F);
                const int xb = static_cast<int>(cxw + lampW * 1.05F);
                for (int xx = xa; xx <= xb; ++xx)
                  if (xx >= 0 && xx < w && yy >= 0 && yy < h)
                    dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{60, 50, 40, false};
              }
            });
}

void effectNaClLattice(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float dim = 0.10F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  dst[static_cast<std::size_t>(y) * w + x] =
                      s.transparent
                          ? Rgb{12, 12, 18, false}
                          : Rgb{u8(s.r * dim), u8(s.g * dim), u8(s.b * dim + 6), false};
                }
              const float cyw = std::cos(t * 0.8F), syw = std::sin(t * 0.8F);
              const float cp = std::cos(0.4F), sp = std::sin(0.4F);
              const float spacing = mn * 0.06F;
              const float cx = w * 0.5F, cy = h * 0.5F;
              const int extent = 3;
              const float grow = std::clamp(t * 1.5F, 0.0F, 1.0F);
              for (int ix = -extent; ix <= extent; ++ix)
                for (int iy = -extent; iy <= extent; ++iy)
                  for (int iz = -extent; iz <= extent; ++iz)
                  {
                    const float r0 = std::sqrt(static_cast<float>(ix * ix + iy * iy + iz * iz));
                    if (r0 > grow * (extent + 1)) continue;
                    const float wx = ix * spacing;
                    const float wy = iy * spacing;
                    const float wz = iz * spacing;
                    const float rx = wx * cyw - wz * syw;
                    const float rz = wx * syw + wz * cyw;
                    const float ry = wy * cp - rz * sp;
                    const float rz2 = wy * sp + rz * cp;
                    const float sx = cx + rx;
                    const float sy = cy + ry / ya;
                    const float depth = std::clamp((rz2 + extent * spacing) / (2 * extent * spacing),
                                                   0.2F, 1.0F);
                    const bool na = ((ix + iy + iz) & 1) == 0;
                    const Rgb tint = na ? Rgb{u8(200 * depth), u8(80 * depth), u8(220 * depth), false}
                                         : Rgb{u8(70 * depth), u8(220 * depth), u8(120 * depth), false};
                    drawDataDisk(dst, w, h, src, sx, sy, mn * 0.025F * depth, ya, 0.85F, t * 0.5F,
                                 tint);
                  }
            });
}

void effectPhStrip(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  static const std::array<Rgb, 7> kRamp = {Rgb{220, 30, 30, false},   Rgb{240, 130, 30, false},
                                           Rgb{240, 220, 30, false}, Rgb{60, 200, 60, false},
                                           Rgb{40, 130, 220, false}, Rgb{100, 30, 200, false},
                                           Rgb{60, 0, 110, false}};
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 60.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(40 + l * 0.20F), u8(40 + l * 0.20F), u8(48 + l * 0.20F), false};
                }
              const float cxw = w * 0.5F;
              const float stripW = mn * 0.18F;
              const float stripTop = h * 0.10F;
              const float stripBot = h * 0.90F;
              const float dyeFront = stripBot - (stripBot - stripTop) * std::clamp(t * 1.2F, 0.0F, 1.0F);
              for (int yy = static_cast<int>(stripTop); yy <= static_cast<int>(stripBot); ++yy)
              {
                // Strip colour: dyed below dyeFront, pale above.
                const float yf = (yy - stripTop) / (stripBot - stripTop);
                Rgb tint;
                if (static_cast<float>(yy) > dyeFront)
                {
                  const float seg = yf * 6.0F;
                  const int i = std::clamp(static_cast<int>(seg), 0, 5);
                  const float f = seg - i;
                  const Rgb& a = kRamp[i];
                  const Rgb& b = kRamp[i + 1];
                  tint = Rgb{u8(a.r + (b.r - a.r) * f), u8(a.g + (b.g - a.g) * f),
                             u8(a.b + (b.b - a.b) * f), false};
                }
                else
                {
                  tint = Rgb{240, 232, 200, false};
                }
                for (int xo = -static_cast<int>(stripW * 0.5F);
                     xo <= static_cast<int>(stripW * 0.5F); ++xo)
                {
                  const int xx = static_cast<int>(cxw + xo);
                  if (xx < 0 || xx >= w || yy < 0 || yy >= h) continue;
                  const Rgb d = sample(src, w, h, xx, yy);
                  const float dr = d.transparent ? 200.0F : d.r;
                  const float dg = d.transparent ? 200.0F : d.g;
                  const float db = d.transparent ? 200.0F : d.b;
                  dst[static_cast<std::size_t>(yy) * w + xx] =
                      Rgb{u8(tint.r * 0.62F + dr * 0.30F), u8(tint.g * 0.62F + dg * 0.30F),
                          u8(tint.b * 0.62F + db * 0.30F), false};
                }
              }
            });
}

void effectChromatography(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float paperLeft = w * 0.30F, paperRight = w * 0.70F;
              const float paperTop = h * 0.05F, paperBot = h * 0.92F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const bool onPaper = x >= paperLeft && x <= paperRight && y >= paperTop &&
                                       y <= paperBot;
                  const Rgb d = sample(src, w, h, x, y);
                  const float dr = d.transparent ? 220.0F : d.r;
                  const float dg = d.transparent ? 220.0F : d.g;
                  const float db = d.transparent ? 200.0F : d.b;
                  if (onPaper)
                    dst[static_cast<std::size_t>(y) * w + x] =
                        Rgb{u8(220 + dr * 0.10F), u8(212 + dg * 0.10F),
                            u8(190 + db * 0.10F), false};
                  else
                    dst[static_cast<std::size_t>(y) * w + x] =
                        Rgb{u8(30 + dr * 0.10F), u8(30 + dg * 0.10F),
                            u8(38 + db * 0.10F), false};
                }
              // Solvent front rises from the bottom.
              const float startY = paperBot - mn * 0.04F;
              const float frontY = startY - (startY - paperTop) * std::clamp(t * 1.1F, 0.0F, 1.0F);
              for (int x = static_cast<int>(paperLeft); x <= static_cast<int>(paperRight); ++x)
              {
                const float yf = frontY + std::sin(x * 0.4F + t * 5.0F) * mn * 0.005F;
                for (int yy = static_cast<int>(yf); yy <= static_cast<int>(startY); ++yy)
                  if (yy >= 0 && yy < h)
                  {
                    Rgb& c = dst[static_cast<std::size_t>(yy) * w + x];
                    c = Rgb{u8(c.r * 0.8F), u8(c.g * 0.85F), u8(c.b * 0.95F + 30), false};
                  }
              }
              // Three dye spots, each with its own Rf.
              static const std::array<Rgb, 3> kDyes = {Rgb{200, 30, 30, false},
                                                       Rgb{220, 200, 30, false},
                                                       Rgb{30, 60, 220, false}};
              static const std::array<float, 3> kRf = {0.55F, 0.40F, 0.25F};
              for (int d = 0; d < 3; ++d)
              {
                const float cx = paperLeft + (paperRight - paperLeft) * (0.30F + 0.20F * d);
                const float climbedY =
                    startY - (startY - paperTop) * std::clamp(t * 1.1F, 0.0F, 1.0F) * kRf[d];
                plotDot(dst, w, h, cx, climbedY, mn * 0.022F, ya, kDyes[d]);
              }
            });
}

void effectBrownian(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  // Pre-walk the path so it's deterministic.
  constexpr int N = 240;
  std::vector<std::pair<float, float>> path(N);
  float px = w * 0.5F, py = h * 0.5F;
  for (int i = 0; i < N; ++i)
  {
    const float a = hash(i * 3) * 6.2832F;
    const float r = mn * 0.025F * hash(i * 7);
    px = std::clamp(px + std::cos(a) * r, w * 0.05F, w * 0.95F);
    py = std::clamp(py + std::sin(a) * r, h * 0.05F, h * 0.95F);
    path[i] = {px, py};
  }
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float dim = 0.30F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 60.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(30 + l * 0.15F * dim), u8(30 + l * 0.15F * dim),
                          u8(40 + l * 0.20F * dim), false};
                }
              const int head = std::min(N - 1, static_cast<int>(t * N));
              for (int i = 1; i <= head; ++i)
              {
                const float ageF = 1.0F - static_cast<float>(head - i) / N;
                drawSeg(dst, w, h, path[i - 1].first, path[i - 1].second, path[i].first,
                        path[i].second, std::max(1.0F, mn * 0.004F * ageF), ya,
                        Rgb{u8(180 * ageF), u8(200 * ageF), u8(240 * ageF), false});
              }
              if (head < N)
              {
                drawDataDisk(dst, w, h, src, path[head].first, path[head].second, mn * 0.020F, ya,
                             0.80F, t * 2.0F, Rgb{240, 240, 255, false});
              }
              // Many tiny invisible "molecules" hinted as flecks.
              for (int i = 0; i < 60; ++i)
              {
                const float fx = std::fmod(hash(i) * w + t * 80, static_cast<float>(w));
                const float fy = std::fmod(hash(i * 3) * h + t * 50, static_cast<float>(h));
                plotDot(dst, w, h, fx, fy, std::max(1.0F, mn * 0.002F), ya,
                        Rgb{120, 120, 160, false});
              }
            });
}

void effectAcidTrip(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  (void)renderer;
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5200,
            [&](float t, std::vector<Rgb>& dst)
            {
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float br = s.transparent ? 100.0F : s.r;
                  const float bg = s.transparent ? 100.0F : s.g;
                  const float bb = s.transparent ? 120.0F : s.b;
                  const float wob = std::sin(x * 0.06F + y * 0.08F + t * 8.0F);
                  const float swap = 0.5F + 0.5F * std::sin(t * 4.0F);
                  const float r = br * (1 - swap) + bg * swap + 60 * wob;
                  const float g = bg * (1 - swap) + bb * swap + 60 * std::sin(t * 7.0F + x * 0.05F);
                  const float b = bb * (1 - swap) + br * swap + 60 * std::sin(t * 5.0F + y * 0.05F);
                  dst[static_cast<std::size_t>(y) * w + x] = Rgb{u8(r), u8(g), u8(b), false};
                }
            });
}

void effectCatalyst(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 5200,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float dim = 0.20F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 40.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(28 + l * 0.10F * dim), u8(30 + l * 0.10F * dim),
                          u8(36 + l * 0.10F * dim), false};
                }
              const float surfaceY = h * 0.80F;
              for (int x = 0; x < w; ++x)
                for (int yo = 0; yo <= static_cast<int>(mn * 0.04F); ++yo)
                {
                  const int yy = static_cast<int>(surfaceY) + yo;
                  if (yy < h)
                    dst[static_cast<std::size_t>(yy) * w + x] = Rgb{120, 100, 60, false};
                }
              // Molecules bounce against the surface.
              for (int i = 0; i < 14; ++i)
              {
                const float phase = hash(i) * 6.2832F;
                const float bounce = std::fabs(std::sin(t * 3.0F + phase));
                const float bx = w * (0.1F + 0.8F * hash(i * 3));
                const float by = surfaceY - mn * 0.06F - bounce * mn * 0.25F;
                const Rgb tint =
                    (i & 1) ? Rgb{210, 120, 80, false} : Rgb{80, 180, 210, false};
                drawDataDisk(dst, w, h, src, bx, by, mn * 0.022F, ya, 0.75F, t + i, tint);
              }
            });
}

void effectGlowStick(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5200,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float dim = 0.15F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 40.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(14 + l * 0.06F * dim), u8(18 + l * 0.08F * dim),
                          u8(20 + l * 0.06F * dim), false};
                }
              const float midY = h * 0.5F;
              const float tubeLen = w * 0.7F;
              const float tubeR = mn * 0.04F;
              const float snapX = w * 0.5F;
              const float snapT = 0.18F;
              const float snapAng = std::clamp((t - snapT) / 0.05F, 0.0F, 1.0F) * 0.18F;
              const float spread = std::clamp((t - snapT) / 0.50F, 0.0F, 1.0F);
              const float L = tubeLen * 0.5F;
              for (float u = -1.0F; u <= 1.0F; u += 1.0F / (tubeLen * 0.5F))
              {
                const float bend = (u < 0) ? -snapAng : snapAng;
                const float xx = w * 0.5F + u * L;
                const float yy = midY + std::sin(bend) * std::fabs(u) * mn * 0.04F;
                const float yLo = yy - tubeR, yHi = yy + tubeR;
                const bool inGlow = std::fabs(xx - snapX) / L < spread;
                for (int iy = static_cast<int>(yLo); iy <= static_cast<int>(yHi); ++iy)
                {
                  if (iy < 0 || iy >= h) continue;
                  const int ix = static_cast<int>(xx);
                  if (ix < 0 || ix >= w) continue;
                  if (inGlow)
                  {
                    const Rgb d = sample(src, w, h, xx, iy);
                    const float dg = d.transparent ? 200.0F : d.g;
                    dst[static_cast<std::size_t>(iy) * w + ix] =
                        Rgb{u8(120 + dg * 0.20F), u8(255), u8(140 + dg * 0.20F), false};
                  }
                  else
                  {
                    dst[static_cast<std::size_t>(iy) * w + ix] = Rgb{200, 200, 210, false};
                  }
                }
              }
            });
}

void effectMentos(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 50.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(16 + l * 0.10F), u8(20 + l * 0.10F), u8(24 + l * 0.10F), false};
                }
              const float cxw = w * 0.5F;
              const float bottleTop = h * 0.40F;
              const float bottleBot = h * 0.92F;
              // Bottle body (data-filled).
              for (int yy = static_cast<int>(bottleTop); yy <= static_cast<int>(bottleBot); ++yy)
              {
                const float yf = (yy - bottleTop) / (bottleBot - bottleTop);
                const int half = static_cast<int>(mn * (0.05F + 0.04F * yf));
                for (int xo = -half; xo <= half; ++xo)
                {
                  const int xx = static_cast<int>(cxw + xo);
                  if (xx < 0 || xx >= w || yy >= h) continue;
                  const Rgb d = sample(src, w, h, xx, yy);
                  const float dr = d.transparent ? 90.0F : d.r;
                  const float dg = d.transparent ? 40.0F : d.g;
                  const float db = d.transparent ? 30.0F : d.b;
                  dst[static_cast<std::size_t>(yy) * w + xx] =
                      Rgb{u8(90 + dr * 0.30F), u8(40 + dg * 0.20F), u8(30 + db * 0.15F), false};
                }
              }
              // Bottle cap.
              for (int yy = static_cast<int>(bottleTop - mn * 0.04F); yy < static_cast<int>(bottleTop);
                   ++yy)
              {
                const int half = static_cast<int>(mn * 0.04F);
                for (int xo = -half; xo <= half; ++xo)
                {
                  const int xx = static_cast<int>(cxw + xo);
                  if (xx >= 0 && xx < w && yy >= 0)
                    dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{160, 30, 30, false};
                }
              }
              // Drop a few Mentos in early.
              if (t < 0.25F)
              {
                for (int i = 0; i < 3; ++i)
                {
                  const float my = bottleTop - mn * 0.10F + (t / 0.25F) * mn * 0.15F + i * mn * 0.04F;
                  drawDataDisk(dst, w, h, src, cxw + (i - 1) * mn * 0.02F, my, mn * 0.020F, ya,
                               0.75F, t * 2.0F, Rgb{230, 230, 220, false});
                }
              }
              // Erupting foam fountain.
              if (t > 0.25F)
              {
                const float ft = (t - 0.25F) / 0.75F;
                const Rgb foam{220, 220, 230, false};
                for (int i = 0; i < 40; ++i)
                {
                  const float ph = hash(i) * 6.2832F;
                  const float vx = std::cos(ph) * mn * 0.04F;
                  const float vy = -mn * (0.20F + hash(i * 3) * 0.30F);
                  const float fx = cxw + vx * ft * 5.0F;
                  const float fy = bottleTop + vy * ft + 9.8F * mn * 0.5F * ft * ft;
                  if (fy < bottleTop + mn * 0.05F)
                    drawDataDisk(dst, w, h, src, fx, fy, mn * 0.015F, ya, 0.85F, t + i, foam);
                }
              }
            });
}

void effectAvogadro(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5200,
            [&](float t, std::vector<Rgb>& dst)
            {
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 60.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  const float sf = static_cast<float>(y) / h;
                  if (y > h * 0.78F)
                    dst[static_cast<std::size_t>(y) * w + x] =
                        Rgb{u8(80 + l * 0.10F), u8(60 + l * 0.08F), u8(40 + l * 0.06F), false};
                  else
                    dst[static_cast<std::size_t>(y) * w + x] =
                        Rgb{u8(140 + 50 * (1 - sf) + l * 0.10F),
                            u8(180 + 50 * (1 - sf) + l * 0.10F),
                            u8(220 - 30 * sf + l * 0.10F), false};
                }
              const float groundY = h * 0.78F;
              const float cxw = w * 0.5F;
              // Hole — dark ellipse.
              plotDot(dst, w, h, cxw, groundY, mn * 0.08F, ya, Rgb{20, 14, 10, false});
              // Mole pokes up.
              const float rise = std::clamp(t / 0.5F, 0.0F, 1.0F);
              const float my = groundY - rise * mn * 0.10F;
              drawDataDisk(dst, w, h, src, cxw, my, mn * 0.07F, ya, 0.7F, 0.0F,
                           Rgb{80, 60, 50, false});
              // Tiny eyes + pink nose.
              plotDot(dst, w, h, cxw - mn * 0.020F, my - mn * 0.020F, std::max(1.0F, mn * 0.005F), ya,
                      Rgb{20, 10, 10, false});
              plotDot(dst, w, h, cxw + mn * 0.020F, my - mn * 0.020F, std::max(1.0F, mn * 0.005F), ya,
                      Rgb{20, 10, 10, false});
              plotDot(dst, w, h, cxw, my, std::max(1.0F, mn * 0.008F), ya,
                      Rgb{220, 130, 150, false});
              // Sign hovering above.
              if (rise >= 1.0F)
              {
                const std::string line = "6.022 X 10^23";
                const int sc = std::max(2, static_cast<int>(mn / 60.0F));
                const float lineW = static_cast<float>(line.size()) * 6 * sc;
                const float signX = cxw - lineW * 0.5F;
                const float signY = my - mn * 0.20F;
                for (int xo = -static_cast<int>(lineW * 0.55F);
                     xo <= static_cast<int>(lineW * 0.55F); ++xo)
                  for (int yo = -static_cast<int>(sc * 5); yo <= static_cast<int>(sc * 5); ++yo)
                  {
                    const int xx = static_cast<int>(cxw + xo);
                    const int yy = static_cast<int>(signY + yo);
                    if (xx >= 0 && xx < w && yy >= 0 && yy < h)
                      dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{240, 235, 200, false};
                  }
                for (int ci = 0; ci < static_cast<int>(line.size()); ++ci)
                {
                  const auto g = glyph5x7(line[ci]);
                  for (int fy = 0; fy < 7; ++fy)
                    for (int fx = 0; fx < 5; ++fx)
                      if (g[fy][fx] == '1')
                        plotDot(dst, w, h, signX + (ci * 6 + fx) * sc,
                                signY - sc * 3 + fy * sc,
                                std::max(1.0F, static_cast<float>(sc) * 0.5F), ya,
                                Rgb{40, 30, 20, false});
                }
              }
            });
}

void effectMendeleev(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  constexpr int kCols = 18, kRows = 7;
  static const char* const kSyms[kRows][kCols] = {
      {"H",  "",   "",   "",   "",   "",   "",   "",   "",   "",   "",   "",   "",   "",   "",   "",   "",   "He"},
      {"Li", "Be", "",   "",   "",   "",   "",   "",   "",   "",   "",   "",   "B",  "C",  "N",  "O",  "F",  "Ne"},
      {"Na", "Mg", "",   "",   "",   "",   "",   "",   "",   "",   "",   "",   "Al", "Si", "P",  "S",  "Cl", "Ar"},
      {"K",  "Ca", "Sc", "Ti", "V",  "Cr", "Mn", "Fe", "Co", "Ni", "Cu", "Zn", "Ga", "Ge", "As", "Se", "Br", "Kr"},
      {"Rb", "Sr", "Y",  "Zr", "Nb", "Mo", "Tc", "Ru", "Rh", "Pd", "Ag", "Cd", "In", "Sn", "Sb", "Te", "I",  "Xe"},
      {"Cs", "Ba", "*",  "Hf", "Ta", "W",  "Re", "Os", "Ir", "Pt", "Au", "Hg", "Tl", "Pb", "Bi", "Po", "At", "Rn"},
      {"Fr", "Ra", "*",  "Rf", "Db", "Sg", "Bh", "Hs", "Mt", "Ds", "Rg", "Cn", "Nh", "Fl", "Mc", "Lv", "Ts", "Og"}};
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 30.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(20 + l * 0.10F), u8(22 + l * 0.10F), u8(36 + l * 0.14F), false};
                }
              const float cellW = std::min(w * 0.9F / kCols, h * 0.78F / kRows);
              const float cellH = cellW;
              const float startX = (w - cellW * kCols) * 0.5F;
              const float startY = (h - cellH * kRows) * 0.5F;
              for (int r = 0; r < kRows; ++r)
                for (int c = 0; c < kCols; ++c)
                {
                  const char* sym = kSyms[r][c];
                  if (sym[0] == '\0') continue;
                  const float tx = startX + c * cellW + cellW * 0.5F;
                  const float ty = startY + r * cellH + cellH * 0.5F;
                  const int seed = r * kCols + c;
                  const float startSx = hash(seed) * w;
                  const float startSy = -mn * 0.1F;
                  const float dropT = std::clamp(t - hash(seed * 3) * 0.4F, 0.0F, 1.0F);
                  const float ease = 1.0F - (1.0F - dropT) * (1.0F - dropT);
                  const float px = startSx + (tx - startSx) * ease;
                  const float py = startSy + (ty - startSy) * ease;
                  if (dropT <= 0.0F) continue;
                  const int sc = std::max(1, static_cast<int>(cellW / 12.0F));
                  for (int ci = 0; ci < 2 && sym[ci]; ++ci)
                  {
                    const auto g = glyph5x7(sym[ci]);
                    for (int fy = 0; fy < 7; ++fy)
                      for (int fx = 0; fx < 5; ++fx)
                        if (g[fy][fx] == '1')
                          plotDot(dst, w, h, px - sc * 3 + (ci * 6 + fx) * sc,
                                  py - sc * 3 + fy * sc,
                                  std::max(1.0F, static_cast<float>(sc) * 0.5F), ya,
                                  Rgb{u8(180 + 75 * dropT), u8(180 + 60 * dropT),
                                      u8(120 + 40 * dropT), false});
                  }
                }
            });
}

void effectElectrolysis(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              // Solution: data dimmed and shifted toward sky-blue.
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 70.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(20 + l * 0.18F), u8(40 + l * 0.20F), u8(80 + l * 0.30F), false};
                }
              const float cxl = w * 0.35F, cxr = w * 0.65F;
              const float botY = h * 0.85F;
              // Electrodes (vertical bars).
              for (int yy = static_cast<int>(h * 0.20F); yy <= static_cast<int>(botY); ++yy)
              {
                for (int xo = -2; xo <= 2; ++xo)
                {
                  const int xL = static_cast<int>(cxl + xo), xR = static_cast<int>(cxr + xo);
                  if (yy >= 0 && yy < h)
                  {
                    if (xL >= 0 && xL < w) dst[static_cast<std::size_t>(yy) * w + xL] = Rgb{220, 220, 220, false};
                    if (xR >= 0 && xR < w) dst[static_cast<std::size_t>(yy) * w + xR] = Rgb{220, 220, 220, false};
                  }
                }
              }
              // Bubbles: more from left (cathode → H2), fewer from right (anode → O2).
              for (int i = 0; i < 40; ++i)
              {
                const float lifetime = hash(i) * 2.0F;
                const float bt = std::fmod(t + lifetime, 1.0F);
                const float bx = cxl + (hash(i * 3) - 0.5F) * mn * 0.04F;
                const float by = botY - bt * (botY - h * 0.15F);
                drawDataDisk(dst, w, h, src, bx, by, mn * 0.014F, ya, 0.50F, t + i,
                             Rgb{220, 220, 240, false});
              }
              for (int i = 0; i < 20; ++i)
              {
                const float lifetime = hash(i * 17) * 2.0F;
                const float bt = std::fmod(t + lifetime, 1.0F);
                const float bx = cxr + (hash(i * 5) - 0.5F) * mn * 0.04F;
                const float by = botY - bt * (botY - h * 0.15F);
                drawDataDisk(dst, w, h, src, bx, by, mn * 0.014F, ya, 0.50F, t + i,
                             Rgb{220, 220, 240, false});
              }
              // + / - labels.
              const int sc = std::max(2, static_cast<int>(mn / 40.0F));
              const auto gp = glyph5x7('+');
              const auto gm = glyph5x7('-');
              for (int fy = 0; fy < 7; ++fy)
                for (int fx = 0; fx < 5; ++fx)
                {
                  if (gm[fy][fx] == '1')
                    plotDot(dst, w, h, cxl + (fx - 2.5F) * sc, h * 0.10F + fy * sc,
                            std::max(1.0F, static_cast<float>(sc) * 0.5F), ya,
                            Rgb{240, 240, 240, false});
                  if (gp[fy][fx] == '1')
                    plotDot(dst, w, h, cxr + (fx - 2.5F) * sc, h * 0.10F + fy * sc,
                            std::max(1.0F, static_cast<float>(sc) * 0.5F), ya,
                            Rgb{240, 240, 240, false});
                }
            });
}

void effectSoapBubble(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(
      renderer, w, h, 5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 40.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(20 + l * 0.10F), u8(22 + l * 0.10F), u8(30 + l * 0.10F), false};
          }
        const float pop = std::clamp((t - 0.85F) / 0.15F, 0.0F, 1.0F);
        const float cx = w * 0.5F + std::sin(t * 1.5F) * mn * 0.04F;
        const float cy = h * 0.5F + std::cos(t * 1.3F) * mn * 0.03F;
        const float R = mn * 0.35F * (1.0F + pop * 0.3F);
        // Fill the bubble interior with sampled data, modulated by iridescent
        // fringes.
        for (int yy = static_cast<int>(cy - R / ya); yy <= static_cast<int>(cy + R / ya); ++yy)
        {
          if (yy < 0 || yy >= h) continue;
          for (int xx = static_cast<int>(cx - R); xx <= static_cast<int>(cx + R); ++xx)
          {
            if (xx < 0 || xx >= w) continue;
            const float dx = xx - cx, dy = (yy - cy) * ya;
            const float r = std::hypot(dx, dy);
            if (r > R) continue;
            const Rgb d = sample(src, w, h, xx, yy);
            const float dr = d.transparent ? 200.0F : d.r;
            const float dg = d.transparent ? 200.0F : d.g;
            const float db = d.transparent ? 220.0F : d.b;
            const float fringe = std::sin(r / mn * 30.0F + t * 6.0F);
            const float hue = fringe * 0.5F + 0.5F;
            const float fr = (std::sin(hue * 6.2832F) + 1) * 60;
            const float fg = (std::sin(hue * 6.2832F + 2.094F) + 1) * 60;
            const float fb = (std::sin(hue * 6.2832F + 4.189F) + 1) * 60;
            const float edge = 1.0F - std::clamp((R - r) / (R * 0.20F), 0.0F, 1.0F);
            const float alpha = 1.0F - pop;
            dst[static_cast<std::size_t>(yy) * w + xx] =
                Rgb{u8((dr * 0.55F + fr + edge * 80) * alpha + dr * (1 - alpha)),
                    u8((dg * 0.55F + fg + edge * 80) * alpha + dg * (1 - alpha)),
                    u8((db * 0.55F + fb + edge * 80) * alpha + db * (1 - alpha)), false};
          }
        }
        // Specular highlight.
        if (pop < 0.5F)
          plotDot(dst, w, h, cx - R * 0.35F, cy - R * 0.40F / ya, R * 0.08F, ya,
                  Rgb{255, 255, 255, false});
      });
}

void effectLiesegang(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5200,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float cx = w * 0.5F, cy = h * 0.5F;
              const float maxR = std::min(w, h) * 0.42F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const float dx = x - cx, dy = (y - cy) * ya;
                  const float r = std::hypot(dx, dy);
                  const Rgb d = sample(src, w, h, x, y);
                  const float dr = d.transparent ? 100.0F : d.r;
                  const float dg = d.transparent ? 100.0F : d.g;
                  const float db = d.transparent ? 120.0F : d.b;
                  if (r > maxR)
                    dst[static_cast<std::size_t>(y) * w + x] =
                        Rgb{u8(20 + dr * 0.08F), u8(24 + dg * 0.08F), u8(30 + db * 0.10F), false};
                  else
                  {
                    const float band = std::sin(r / mn * 22.0F - t * 5.0F);
                    const float front = std::clamp(t * maxR * 1.3F - r, 0.0F, 1.0F);
                    const float intensity = front * (0.5F + 0.5F * band);
                    dst[static_cast<std::size_t>(y) * w + x] =
                        Rgb{u8(220 + dr * 0.20F - intensity * 120),
                            u8(180 + dg * 0.20F - intensity * 60),
                            u8(80 + db * 0.20F + intensity * 100), false};
                  }
                }
              // Dish rim.
              for (float a = 0; a < 6.2832F; a += 0.02F)
                plotDot(dst, w, h, cx + std::cos(a) * maxR, cy + std::sin(a) * maxR / ya,
                        std::max(1.0F, mn * 0.005F), ya, Rgb{200, 200, 200, false});
            });
}

void effectPhaseTransition(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  constexpr int N = 56;
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float dim = 0.20F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 50.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(20 + l * 0.10F * dim), u8(20 + l * 0.10F * dim),
                          u8(30 + l * 0.10F * dim), false};
                }
              const float gasT = 0.35F, liqT = 0.65F;
              for (int i = 0; i < N; ++i)
              {
                const int gx = i % 8, gy = i / 8;
                const float latticeX = w * 0.18F + gx * w * 0.09F;
                const float latticeY = h * 0.25F + gy * mn * 0.10F;
                float px = latticeX + (hash(i) - 0.5F) * w * 0.6F * std::sin(t * 4.0F + i);
                float py = latticeY + (hash(i * 3) - 0.5F) * h * 0.6F * std::cos(t * 4.0F + i);
                if (t > gasT)
                {
                  const float mix = std::clamp((t - gasT) / (liqT - gasT), 0.0F, 1.0F);
                  px = px * (1 - mix) + latticeX * mix;
                  py = py * (1 - mix) + latticeY * mix;
                }
                if (t > liqT)
                {
                  px = latticeX;
                  py = latticeY;
                }
                drawDataDisk(dst, w, h, src, px, py, mn * 0.020F, ya, 0.75F, t + i,
                             Rgb{180, 220, 240, false});
              }
              // Phase label.
              const char* lab = t < gasT ? "GAS" : (t < liqT ? "LIQUID" : "SOLID");
              const int sc = std::max(3, static_cast<int>(mn / 25.0F));
              const std::string s(lab);
              const float lineW = static_cast<float>(s.size()) * 6 * sc;
              for (int ci = 0; ci < static_cast<int>(s.size()); ++ci)
              {
                const auto g = glyph5x7(s[ci]);
                for (int fy = 0; fy < 7; ++fy)
                  for (int fx = 0; fx < 5; ++fx)
                    if (g[fy][fx] == '1')
                      plotDot(dst, w, h, w * 0.5F - lineW * 0.5F + (ci * 6 + fx) * sc,
                              h * 0.85F + fy * sc, std::max(1.0F, static_cast<float>(sc) * 0.5F),
                              ya, Rgb{240, 240, 240, false});
              }
            });
}

}  // namespace ee_detail
}  // namespace Qdless
