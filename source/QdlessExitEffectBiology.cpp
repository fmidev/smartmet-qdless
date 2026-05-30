#include "QdlessExitEffectCommon.h"

namespace Qdless
{
namespace ee_detail
{

void effectDNA(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float dim = 0.20F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 30.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(18 + l * 0.10F * dim), u8(22 + l * 0.10F * dim),
                          u8(40 + l * 0.14F * dim), false};
                }
              const float cx = w * 0.5F;
              const float R = mn * 0.13F;
              const Rgb strandA{230, 120, 130, false};
              const Rgb strandB{120, 210, 220, false};
              const int nTurns = 5;
              for (int k = 0; k < h; ++k)
              {
                const float u = k / static_cast<float>(h);
                const float ang = u * nTurns * 6.2832F + t * 2.5F;
                const float xA = cx + std::cos(ang) * R;
                const float xB = cx + std::cos(ang + 3.14159F) * R;
                plotDot(dst, w, h, xA, k, std::max(1.0F, mn * 0.010F), ya, strandA);
                plotDot(dst, w, h, xB, k, std::max(1.0F, mn * 0.010F), ya, strandB);
                if (k % 5 == 0 && std::sin(ang) > 0)
                {
                  for (int rr = 1; rr < 12; ++rr)
                  {
                    const float ux = xA + (xB - xA) * rr / 12.0F;
                    const Rgb d = sample(src, w, h, ux, static_cast<float>(k));
                    const float dr = d.transparent ? 180.0F : d.r;
                    const float dg = d.transparent ? 180.0F : d.g;
                    const float db = d.transparent ? 200.0F : d.b;
                    plotDot(dst, w, h, ux, k, std::max(1.0F, mn * 0.005F), ya,
                            Rgb{u8(dr * 0.7F + 60), u8(dg * 0.7F + 60), u8(db * 0.7F + 60),
                                false});
                  }
                }
              }
            });
}

void effectMarchOfProgress(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              // Sepia background.
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 100.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(190 + l * 0.10F), u8(160 + l * 0.10F), u8(110 + l * 0.08F), false};
                }
              const float groundY = h * 0.88F;
              for (int x = 0; x < w; ++x)
                dst[static_cast<std::size_t>(groundY) * w + x] = Rgb{60, 50, 40, false};
              const int N = 5;
              for (int i = 0; i < N; ++i)
              {
                const float frac = static_cast<float>(i) / (N - 1);
                const float cx = w * 0.12F + frac * w * 0.76F;
                const float ht = mn * (0.30F + 0.10F * frac);
                // Hunch decreases as i increases.
                const float hunch = (1.0F - frac) * 0.6F;
                const float walk = std::sin(t * 12.0F + i) * 0.3F;
                const Rgb fig{30, 22, 18, false};
                // Head.
                plotDot(dst, w, h, cx + hunch * ht * 0.3F, groundY - ht + hunch * ht * 0.2F,
                        ht * 0.10F, ya, fig);
                // Torso.
                drawSeg(dst, w, h, cx + hunch * ht * 0.2F, groundY - ht * 0.85F + hunch * ht * 0.15F,
                        cx, groundY - ht * 0.40F, ht * 0.10F, ya, fig);
                // Legs.
                drawSeg(dst, w, h, cx, groundY - ht * 0.40F, cx - ht * 0.15F + walk * ht * 0.1F,
                        groundY, ht * 0.08F, ya, fig);
                drawSeg(dst, w, h, cx, groundY - ht * 0.40F, cx + ht * 0.15F - walk * ht * 0.1F,
                        groundY, ht * 0.08F, ya, fig);
                // Arms.
                drawSeg(dst, w, h, cx, groundY - ht * 0.70F, cx - ht * 0.20F + walk * ht * 0.15F,
                        groundY - ht * 0.40F, ht * 0.06F, ya, fig);
                drawSeg(dst, w, h, cx, groundY - ht * 0.70F, cx + ht * 0.20F - walk * ht * 0.15F,
                        groundY - ht * 0.40F, ht * 0.06F, ya, fig);
              }
              // Time arrow.
              drawSeg(dst, w, h, w * 0.08F, h * 0.95F, w * 0.92F, h * 0.95F,
                      std::max(1.0F, mn * 0.004F), ya, Rgb{40, 30, 20, false});
              drawSeg(dst, w, h, w * 0.92F, h * 0.95F, w * 0.88F, h * 0.93F,
                      std::max(1.0F, mn * 0.004F), ya, Rgb{40, 30, 20, false});
            });
}

void effectTreeOfLife(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 30.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(20 + l * 0.10F), u8(22 + l * 0.10F), u8(20 + l * 0.06F), false};
                }
              const float cx = w * 0.5F, cy = h * 0.5F;
              const float maxR = std::min(w, h) * 0.40F;
              // Recursive-ish branching: start at centre, fan out.
              const Rgb branch{220, 200, 140, false};
              const int rings = 5;
              for (int r = 0; r < rings; ++r)
              {
                const float r0 = maxR * r / rings;
                const float r1 = maxR * (r + 1) / rings;
                const int n = (1 << (r + 1));
                const float reveal = std::clamp(t * rings - r, 0.0F, 1.0F);
                if (reveal <= 0.0F) continue;
                for (int i = 0; i < n; ++i)
                {
                  const float a = i / static_cast<float>(n) * 6.2832F;
                  const float x0 = cx + std::cos(a) * r0;
                  const float y0 = cy + std::sin(a) * r0 / ya;
                  const float x1 = cx + std::cos(a) * (r0 + (r1 - r0) * reveal);
                  const float y1 = cy + std::sin(a) * (r0 + (r1 - r0) * reveal) / ya;
                  drawSeg(dst, w, h, x0, y0, x1, y1, std::max(1.0F, mn * 0.004F), ya, branch);
                }
              }
              // Leaf-tip dots.
              const int leafN = 32;
              for (int i = 0; i < leafN; ++i)
              {
                const float a = i / static_cast<float>(leafN) * 6.2832F;
                const float lx = cx + std::cos(a) * maxR;
                const float ly = cy + std::sin(a) * maxR / ya;
                drawDataDisk(dst, w, h, src, lx, ly, mn * 0.012F, ya, 0.7F, 0.0F,
                             Rgb{180, 220, 140, false});
              }
              // Centre LUCA dot.
              plotDot(dst, w, h, cx, cy, mn * 0.020F, ya, Rgb{255, 240, 180, false});
            });
}

void effectFinches(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  struct BeakShape { float length, depth; const char* name; };
  static const std::array<BeakShape, 4> kBeaks = {
      BeakShape{0.30F, 0.06F, "WARBLER"},
      BeakShape{0.40F, 0.10F, "INSECT"},
      BeakShape{0.50F, 0.20F, "GROUND"},
      BeakShape{0.60F, 0.30F, "LARGE"}};
  runFrames(
      renderer, w, h, 5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float dim = 0.20F;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 60.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(200 + l * 0.10F * dim), u8(220 + l * 0.10F * dim),
                    u8(240 + l * 0.10F * dim), false};
          }
        // Common ancestor at centre.
        const float cx = w * 0.5F, cy = h * 0.5F;
        drawDataDisk(dst, w, h, src, cx, cy, mn * 0.05F, ya, 0.7F, 0.0F,
                     Rgb{120, 80, 60, false});
        // Four offspring at corners.
        const std::array<std::pair<float, float>, 4> positions = {
            std::make_pair(0.25F, 0.20F), std::make_pair(0.75F, 0.20F),
            std::make_pair(0.25F, 0.80F), std::make_pair(0.75F, 0.80F)};
        for (int i = 0; i < 4; ++i)
        {
          const float tx = w * positions[i].first;
          const float tyy = h * positions[i].second;
          const float reveal = std::clamp(t * 1.5F - i * 0.15F, 0.0F, 1.0F);
          if (reveal <= 0.0F) continue;
          drawSeg(dst, w, h, cx, cy, cx + (tx - cx) * reveal, cy + (tyy - cy) * reveal,
                  std::max(1.0F, mn * 0.004F), ya, Rgb{40, 30, 20, false});
          if (reveal > 0.8F)
          {
            // Beak silhouette = data-textured head + triangular beak.
            const BeakShape& b = kBeaks[i];
            const float headR = mn * 0.05F;
            drawDataDisk(dst, w, h, src, tx, tyy, headR, ya, 0.7F, 0.0F,
                         Rgb{180, 130, 90, false});
            // Beak as triangle pointing right.
            const float bx0 = tx + headR;
            const float bx1 = bx0 + mn * b.length;
            const float bd = mn * b.depth;
            for (float u = 0; u < 1.0F; u += 0.02F)
            {
              const float bx = bx0 + (bx1 - bx0) * u;
              const float byHalf = bd * (1.0F - u);
              plotDot(dst, w, h, bx, tyy, std::max(1.0F, byHalf), ya, Rgb{160, 100, 60, false});
            }
            // Name label.
            const int sc = std::max(1, static_cast<int>(mn / 60.0F));
            const std::string name(b.name);
            for (int ci = 0; ci < static_cast<int>(name.size()); ++ci)
            {
              const auto g = glyph5x7(name[ci]);
              for (int fy = 0; fy < 7; ++fy)
                for (int fx = 0; fx < 5; ++fx)
                  if (g[fy][fx] == '1')
                    plotDot(dst, w, h, tx + (ci * 6 + fx) * sc - name.size() * 3 * sc,
                            tyy + headR + sc * 2 + fy * sc,
                            std::max(1.0F, static_cast<float>(sc) * 0.5F), ya,
                            Rgb{40, 30, 20, false});
            }
          }
        }
      });
}

void effectPepperedMoth(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(
      renderer, w, h, 5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        // Bark texture darkens with t.
        const float darkness = std::clamp(t * 0.9F, 0.0F, 1.0F);
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 100.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            const float scratch = std::sin(x * 0.20F + y * 0.04F) * 30 + std::sin(y * 0.15F) * 20;
            const float base = 200 - darkness * 180 + scratch * 0.3F;
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(base * 0.55F + l * 0.10F), u8(base * 0.50F + l * 0.08F),
                    u8(base * 0.42F + l * 0.06F), false};
          }
        // Pale moths fade out as t increases; dark moths fade in.
        const int nL = static_cast<int>((1.0F - darkness) * 8);
        const int nD = static_cast<int>(darkness * 8);
        for (int i = 0; i < nL; ++i)
        {
          const float mx = hash(i) * w;
          const float my = hash(i * 3) * h;
          const float mS = mn * 0.025F;
          plotDot(dst, w, h, mx, my, mS, ya, Rgb{210, 200, 180, false});
          plotDot(dst, w, h, mx - mS * 0.6F, my, mS * 0.5F, ya, Rgb{210, 200, 180, false});
          plotDot(dst, w, h, mx + mS * 0.6F, my, mS * 0.5F, ya, Rgb{210, 200, 180, false});
        }
        for (int i = 0; i < nD; ++i)
        {
          const float mx = hash((i + 99) * 7) * w;
          const float my = hash((i + 99) * 11) * h;
          const float mS = mn * 0.025F;
          plotDot(dst, w, h, mx, my, mS, ya, Rgb{40, 30, 20, false});
          plotDot(dst, w, h, mx - mS * 0.6F, my, mS * 0.5F, ya, Rgb{40, 30, 20, false});
          plotDot(dst, w, h, mx + mS * 0.6F, my, mS * 0.5F, ya, Rgb{40, 30, 20, false});
        }
      });
}

void effectCambrian(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
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
                  const float l = s.transparent ? 30.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  const float sf = static_cast<float>(y) / h;
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(8 + 20 * sf + l * 0.08F), u8(30 + 30 * sf + l * 0.10F),
                          u8(60 + 40 * sf + l * 0.12F), false};
                }
              const float floorY = h * 0.92F;
              for (int x = 0; x < w; ++x)
                for (int yo = 0; yo <= 2; ++yo)
                {
                  const int yy = static_cast<int>(floorY) + yo;
                  if (yy < h) dst[static_cast<std::size_t>(yy) * w + x] = Rgb{100, 80, 60, false};
                }
              const int N = 12;
              for (int i = 0; i < N; ++i)
              {
                const float reveal = std::clamp(t * 1.5F - hash(i) * 0.4F, 0.0F, 1.0F);
                if (reveal <= 0.0F) continue;
                const float bx = w * (0.08F + 0.84F * hash(i * 7));
                const float by = floorY - mn * (0.05F + 0.20F * hash(i * 11)) * reveal;
                const float bS = mn * (0.030F + 0.025F * hash(i * 13)) * reveal;
                drawDataDisk(dst, w, h, src, bx, by, bS, ya, 0.75F, t + i,
                             Rgb{u8(180 + hash(i * 19) * 75), u8(120 + hash(i * 23) * 100),
                                 u8(100 + hash(i * 29) * 100), false});
                // Eyes — varying numbers (1, 2, 5).
                const int nEyes = 1 + static_cast<int>(hash(i * 31) * 5);
                for (int e = 0; e < nEyes; ++e)
                {
                  const float ea = e / static_cast<float>(nEyes) * 6.2832F;
                  plotDot(dst, w, h, bx + std::cos(ea) * bS * 0.6F,
                          by + std::sin(ea) * bS * 0.6F, std::max(1.0F, bS * 0.18F), ya,
                          Rgb{30, 20, 30, false});
                }
                // Tentacles.
                for (int k = 0; k < 4; ++k)
                {
                  const float ka = k / 4.0F * 6.2832F + t;
                  drawSeg(dst, w, h, bx, by, bx + std::cos(ka) * bS * 1.2F,
                          by + std::sin(ka) * bS * 1.2F, std::max(1.0F, mn * 0.004F), ya,
                          Rgb{180, 130, 100, false});
                }
              }
            });
}

void effectOutOfAfrica(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                  dst[static_cast<std::size_t>(y) * w + x] = Rgb{6, 10, 22, false};
              const float cx = w * 0.5F, cy = h * 0.5F;
              const float R = std::min(w, h) * 0.40F;
              // Earth globe — data-textured.
              drawDataDisk(dst, w, h, src, cx, cy, R, ya, 0.85F, t * 0.4F,
                           Rgb{100, 180, 120, false});
              // East Africa origin (relative to centre of globe).
              const float ox = cx + R * 0.10F;
              const float oy = cy + R * 0.05F;
              plotDot(dst, w, h, ox, oy, mn * 0.022F, ya, Rgb{255, 80, 60, false});
              // Migration arrows fanning out.
              const std::array<std::pair<float, float>, 6> targets = {
                  std::make_pair(-0.35F, -0.20F),  // Europe
                  std::make_pair(0.20F, -0.20F),   // Asia
                  std::make_pair(0.40F, 0.00F),    // SE Asia
                  std::make_pair(0.55F, 0.25F),    // Australia
                  std::make_pair(-0.65F, 0.00F),   // Americas (across Atlantic shortcut)
                  std::make_pair(-0.50F, 0.20F)};  // South America
              for (int i = 0; i < 6; ++i)
              {
                const float reveal = std::clamp(t * 1.5F - i * 0.18F, 0.0F, 1.0F);
                if (reveal <= 0.0F) continue;
                const float tx = cx + targets[i].first * R;
                const float tyy = cy + targets[i].second * R / ya;
                const float px = ox + (tx - ox) * reveal;
                const float py = oy + (tyy - oy) * reveal;
                drawSeg(dst, w, h, ox, oy, px, py, std::max(1.0F, mn * 0.005F), ya,
                        Rgb{255, 200, 120, false});
                // Arrowhead.
                if (reveal > 0.8F)
                  plotDot(dst, w, h, px, py, mn * 0.012F, ya, Rgb{255, 200, 120, false});
              }
            });
}

void effectCellDivides(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float dim = 0.18F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 40.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(30 + l * 0.10F * dim), u8(30 + l * 0.10F * dim),
                          u8(38 + l * 0.10F * dim), false};
                }
              const int stage = std::min(5, static_cast<int>(t * 6.0F));
              const int nCells = 1 << stage;  // 1, 2, 4, 8, 16, 32
              const float cellR = mn * (0.30F / std::sqrt(static_cast<float>(nCells)));
              const float cx = w * 0.5F, cy = h * 0.5F;
              const Rgb tint{180, 200, 220, false};
              for (int i = 0; i < nCells; ++i)
              {
                const float a = i / static_cast<float>(nCells) * 6.2832F;
                const float ring = std::sqrt(static_cast<float>(i)) * cellR * 0.7F;
                const float bx = cx + std::cos(a + t) * ring;
                const float by = cy + std::sin(a + t) * ring / ya;
                drawDataDisk(dst, w, h, src, bx, by, cellR, ya, 0.80F, t + i, tint);
                // Nucleus dot.
                plotDot(dst, w, h, bx, by, cellR * 0.25F, ya, Rgb{80, 60, 100, false});
              }
              // Generation label.
              const int sc = std::max(2, static_cast<int>(mn / 50.0F));
              const std::string lab = "N=" + std::to_string(nCells);
              for (int ci = 0; ci < static_cast<int>(lab.size()); ++ci)
              {
                const auto g = glyph5x7(lab[ci]);
                for (int fy = 0; fy < 7; ++fy)
                  for (int fx = 0; fx < 5; ++fx)
                    if (g[fy][fx] == '1')
                      plotDot(dst, w, h, w * 0.08F + (ci * 6 + fx) * sc, h * 0.08F + fy * sc,
                              std::max(1.0F, static_cast<float>(sc) * 0.5F), ya,
                              Rgb{220, 240, 255, false});
              }
            });
}

void effectPunctuated(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(
      renderer, w, h, 5200,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float dim = 0.25F;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 40.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(24 + l * 0.10F * dim), u8(24 + l * 0.10F * dim),
                    u8(32 + l * 0.10F * dim), false};
          }
        // Axes.
        const float ax0 = w * 0.10F, ax1 = w * 0.92F;
        const float ay0 = h * 0.85F, ay1 = h * 0.15F;
        drawSeg(dst, w, h, ax0, ay0, ax1, ay0, std::max(1.0F, mn * 0.004F), ya,
                Rgb{220, 220, 220, false});
        drawSeg(dst, w, h, ax0, ay0, ax0, ay1, std::max(1.0F, mn * 0.004F), ya,
                Rgb{220, 220, 220, false});
        // Step-line: 4 plateaus separated by jumps.
        struct Step { float fracX; float level; };
        static const std::array<Step, 5> steps = {Step{0.00F, 0.15F}, Step{0.20F, 0.35F},
                                                  Step{0.45F, 0.55F}, Step{0.70F, 0.75F},
                                                  Step{0.90F, 0.90F}};
        const float reveal = std::clamp(t * 1.2F, 0.0F, 1.0F);
        float prevX = ax0, prevY = ay0 - (ay0 - ay1) * steps[0].level;
        for (std::size_t i = 0; i < steps.size(); ++i)
        {
          const float sx = ax0 + (ax1 - ax0) * steps[i].fracX;
          const float sy = ay0 - (ay0 - ay1) * steps[i].level;
          if (sx / w > reveal) break;
          // Flat plateau.
          drawSeg(dst, w, h, prevX, prevY, sx, prevY, std::max(1.0F, mn * 0.006F), ya,
                  Rgb{120, 200, 240, false});
          // Vertical jump.
          drawSeg(dst, w, h, sx, prevY, sx, sy, std::max(1.0F, mn * 0.006F), ya,
                  Rgb{220, 120, 80, false});
          prevX = sx;
          prevY = sy;
        }
        // Continuation to current reveal.
        const float revX = ax0 + (ax1 - ax0) * reveal;
        if (revX > prevX)
          drawSeg(dst, w, h, prevX, prevY, revX, prevY, std::max(1.0F, mn * 0.006F), ya,
                  Rgb{120, 200, 240, false});
        // Labels.
        const int sc = std::max(1, static_cast<int>(mn / 70.0F));
        const std::string lab = "PUNCTUATED EQUILIBRIUM";
        for (int ci = 0; ci < static_cast<int>(lab.size()); ++ci)
        {
          const char ch = lab[ci];
          if (ch == ' ') continue;
          const auto g = glyph5x7(ch);
          for (int fy = 0; fy < 7; ++fy)
            for (int fx = 0; fx < 5; ++fx)
              if (g[fy][fx] == '1')
                plotDot(dst, w, h, ax0 + (ci * 6 + fx) * sc, h * 0.04F + fy * sc,
                        std::max(1.0F, static_cast<float>(sc) * 0.5F), ya,
                        Rgb{200, 200, 220, false});
        }
      });
}

void effectGalapagos(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5800,
            [&](float t, std::vector<Rgb>& dst)
            {
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 70.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  const float sf = static_cast<float>(y) / h;
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(140 + 80 * (1 - sf) + l * 0.10F),
                          u8(180 + 60 * (1 - sf) + l * 0.10F),
                          u8(140 + 30 * sf + l * 0.08F), false};
                }
              const float groundY = h * 0.85F;
              for (int x = 0; x < w; ++x)
                dst[static_cast<std::size_t>(groundY) * w + x] = Rgb{80, 60, 40, false};
              const float cx = -mn * 0.4F + t * (w + mn * 0.6F);
              const float S = mn * 0.25F;
              const Rgb skin{60, 80, 50, false};
              // Shell — data-textured dome.
              drawDataDisk(dst, w, h, src, cx, groundY - S * 0.4F, S * 0.7F, ya, 0.80F, 0.0F,
                           Rgb{80, 110, 60, false});
              // Underbody.
              for (int yo = -static_cast<int>(S * 0.08F); yo <= static_cast<int>(S * 0.08F); ++yo)
                for (int xo = -static_cast<int>(S * 0.65F); xo <= static_cast<int>(S * 0.65F); ++xo)
                {
                  const int xx = static_cast<int>(cx + xo);
                  const int yy = static_cast<int>(groundY - S * 0.05F) + yo;
                  if (xx >= 0 && xx < w && yy >= 0 && yy < h)
                    dst[static_cast<std::size_t>(yy) * w + xx] = skin;
                }
              // Neck + head.
              drawSeg(dst, w, h, cx - S * 0.55F, groundY - S * 0.45F, cx - S * 0.80F,
                      groundY - S * 0.65F, S * 0.10F, ya, skin);
              plotDot(dst, w, h, cx - S * 0.85F, groundY - S * 0.65F, S * 0.08F, ya, skin);
              // Legs.
              for (int leg = 0; leg < 4; ++leg)
              {
                const float lx = cx - S * 0.50F + leg * S * 0.30F;
                drawSeg(dst, w, h, lx, groundY - S * 0.10F, lx, groundY, S * 0.07F, ya, skin);
              }
            });
}

void effectDarwin(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float dim = 0.30F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 100.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(170 + l * 0.10F * dim), u8(140 + l * 0.10F * dim),
                          u8(95 + l * 0.10F * dim), false};
                }
              const float fade = std::clamp(t * 1.6F, 0.0F, 1.0F);
              const float cx = w * 0.5F, cy = h * 0.42F;
              const float S = mn * 0.30F;
              const Rgb ink{30, 20, 14, false};
              // Head (top dome).
              plotDot(dst, w, h, cx, cy - S * 0.15F, S * 0.25F * fade, ya, ink);
              // Beard (large oval below).
              for (int yo = static_cast<int>(S * 0.05F); yo <= static_cast<int>(S * 0.55F); ++yo)
              {
                const float yf = (yo - S * 0.05F) / (S * 0.50F);
                const int half = static_cast<int>(S * (0.30F + 0.10F * std::sin(yf * 3.14159F)) * fade);
                for (int xo = -half; xo <= half; ++xo)
                {
                  const int xx = static_cast<int>(cx + xo);
                  const int yy = static_cast<int>(cy + yo);
                  if (xx >= 0 && xx < w && yy >= 0 && yy < h)
                    dst[static_cast<std::size_t>(yy) * w + xx] = ink;
                }
              }
              // Eyebrows.
              if (fade > 0.6F)
              {
                drawSeg(dst, w, h, cx - S * 0.12F, cy - S * 0.20F, cx - S * 0.05F,
                        cy - S * 0.22F, std::max(1.0F, mn * 0.005F), ya, Rgb{200, 200, 200, false});
                drawSeg(dst, w, h, cx + S * 0.05F, cy - S * 0.22F, cx + S * 0.12F,
                        cy - S * 0.20F, std::max(1.0F, mn * 0.005F), ya, Rgb{200, 200, 200, false});
              }
              // Type out "ON THE ORIGIN OF SPECIES".
              const std::string text = "ON THE ORIGIN OF SPECIES";
              const float typeT = std::clamp((t - 0.45F) / 0.50F, 0.0F, 1.0F);
              const int nReveal = static_cast<int>(typeT * text.size());
              const int sc = std::max(2, static_cast<int>(mn / 50.0F));
              const float lineW = static_cast<float>(text.size()) * 6 * sc;
              for (int ci = 0; ci < nReveal && ci < static_cast<int>(text.size()); ++ci)
              {
                const char ch = text[ci];
                if (ch == ' ') continue;
                const auto g = glyph5x7(ch);
                for (int fy = 0; fy < 7; ++fy)
                  for (int fx = 0; fx < 5; ++fx)
                    if (g[fy][fx] == '1')
                      plotDot(dst, w, h, cx - lineW * 0.5F + (ci * 6 + fx) * sc,
                              h * 0.85F + fy * sc,
                              std::max(1.0F, static_cast<float>(sc) * 0.5F), ya, ink);
              }
            });
}

void effectPeacock(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 30.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(20 + l * 0.10F), u8(28 + l * 0.10F), u8(40 + l * 0.10F), false};
                }
              const float cx = w * 0.5F, cy = h * 0.82F;
              const float spread = std::clamp(t * 1.2F, 0.0F, 1.0F);
              const int N = 80;
              for (int i = 0; i < N; ++i)
              {
                const float frac = i / static_cast<float>(N);
                const float a = (frac - 0.5F) * 3.14159F * 0.9F;
                const float r = mn * 0.55F * spread;
                const float fx = cx + std::sin(a) * r;
                const float fy = cy - std::cos(a) * r / ya;
                // Feather quill.
                drawSeg(dst, w, h, cx, cy, fx, fy, std::max(1.0F, mn * 0.002F), ya,
                        Rgb{60, 90, 30, false});
                // Eye spot.
                drawDataDisk(dst, w, h, src, fx, fy, mn * 0.022F, ya, 0.85F, frac * 6.0F,
                             Rgb{60, 100, 180, false});
                plotDot(dst, w, h, fx, fy, mn * 0.010F, ya, Rgb{30, 50, 100, false});
                plotDot(dst, w, h, fx, fy, mn * 0.005F, ya, Rgb{220, 180, 60, false});
              }
              // Body.
              plotDot(dst, w, h, cx, cy, mn * 0.05F, ya, Rgb{20, 60, 100, false});
              // Head + crest.
              plotDot(dst, w, h, cx, cy - mn * 0.07F, mn * 0.020F, ya, Rgb{30, 100, 160, false});
              for (int k = -2; k <= 2; ++k)
              {
                drawSeg(dst, w, h, cx + k * mn * 0.005F, cy - mn * 0.08F,
                        cx + k * mn * 0.010F, cy - mn * 0.12F, std::max(1.0F, mn * 0.003F), ya,
                        Rgb{30, 100, 160, false});
              }
            });
}

void effectMendel(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 80.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(220 + l * 0.08F), u8(212 + l * 0.08F), u8(186 + l * 0.06F), false};
                }
              const float cx = w * 0.5F, cy = h * 0.5F;
              const float S = std::min(w, h) * 0.30F;
              // 2x2 grid.
              const Rgb ink{40, 30, 20, false};
              for (int g = 0; g < 3; ++g)
              {
                drawSeg(dst, w, h, cx - S, cy - S + g * S, cx + S, cy - S + g * S,
                        std::max(1.0F, mn * 0.004F), ya, ink);
                drawSeg(dst, w, h, cx - S + g * S, cy - S, cx - S + g * S, cy + S,
                        std::max(1.0F, mn * 0.004F), ya, ink);
              }
              static const std::array<const char*, 4> kCells = {"YY", "Yy", "Yy", "yy"};
              const int sc = std::max(2, static_cast<int>(mn / 40.0F));
              for (int gr = 0; gr < 2; ++gr)
                for (int gc = 0; gc < 2; ++gc)
                {
                  const int idx = gr * 2 + gc;
                  const float reveal = std::clamp(t * 1.5F - idx * 0.15F, 0.0F, 1.0F);
                  if (reveal <= 0.0F) continue;
                  const float ccx = cx - S * 0.5F + gc * S;
                  const float ccy = cy - S * 0.5F + gr * S;
                  // Letters.
                  const std::string ll(kCells[idx]);
                  for (int ci = 0; ci < 2; ++ci)
                  {
                    const auto g = glyph5x7(ll[ci]);
                    for (int fy = 0; fy < 7; ++fy)
                      for (int fx = 0; fx < 5; ++fx)
                        if (g[fy][fx] == '1')
                          plotDot(dst, w, h, ccx - sc * 5 + (ci * 6 + fx) * sc,
                                  ccy - sc * 3 + fy * sc,
                                  std::max(1.0F, static_cast<float>(sc) * 0.5F), ya, ink);
                  }
                  // Pea pod.
                  const Rgb yellow{220, 200, 80, false};
                  const Rgb green{120, 180, 80, false};
                  const Rgb wrinkly{160, 130, 60, false};
                  const bool dominant = idx != 3;
                  drawDataDisk(dst, w, h, src, ccx, ccy + sc * 8, S * 0.20F, ya, 0.75F, 0.0F,
                               dominant ? yellow : wrinkly);
                  (void)green;
                }
              // Header.
              const std::string hdr = "F2 GENERATION 3:1";
              const int sh = std::max(2, static_cast<int>(mn / 40.0F));
              for (int ci = 0; ci < static_cast<int>(hdr.size()); ++ci)
              {
                const char ch = hdr[ci];
                if (ch == ' ') continue;
                const auto g = glyph5x7(ch);
                for (int fy = 0; fy < 7; ++fy)
                  for (int fx = 0; fx < 5; ++fx)
                    if (g[fy][fx] == '1')
                      plotDot(dst, w, h, w * 0.5F - hdr.size() * 3 * sh + (ci * 6 + fx) * sh,
                              h * 0.10F + fy * sh,
                              std::max(1.0F, static_cast<float>(sh) * 0.5F), ya, ink);
              }
            });
}

void effectMutation(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
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
                  const float l = s.transparent ? 30.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(20 + l * 0.10F * dim), u8(24 + l * 0.10F * dim),
                          u8(20 + l * 0.06F * dim), false};
                }
              const int sc = std::max(2, static_cast<int>(mn / 30.0F));
              const int cellW = 6 * sc;
              const float scrollPx = t * cellW * 12;
              const int charW = w / cellW + 2;
              const char nts[] = {'A', 'C', 'G', 'T'};
              const Rgb cols[4] = {Rgb{220, 90, 80, false}, Rgb{220, 200, 60, false},
                                   Rgb{80, 200, 100, false}, Rgb{80, 160, 230, false}};
              for (int row = 0; row < 6; ++row)
              {
                const float rowY = h * 0.18F + row * mn * 0.12F;
                for (int i = 0; i < charW; ++i)
                {
                  const float px = w - (i * cellW + std::fmod(scrollPx, cellW)) - cellW;
                  const int seed = row * 137 + i + static_cast<int>(scrollPx / cellW);
                  int nti = static_cast<int>(hash(seed) * 4) % 4;
                  // Mutation: at row 3, halfway through, force a colour shift.
                  bool mutated = false;
                  if (row >= 3 && i > 5 && t > 0.4F)
                  {
                    nti = (nti + 2) % 4;
                    mutated = true;
                  }
                  const auto g = glyph5x7(nts[nti]);
                  for (int fy = 0; fy < 7; ++fy)
                    for (int fx = 0; fx < 5; ++fx)
                      if (g[fy][fx] == '1')
                        plotDot(dst, w, h, px + fx * sc, rowY + fy * sc,
                                std::max(1.0F, static_cast<float>(sc) * 0.5F), ya,
                                mutated ? Rgb{255, 80, 80, false} : cols[nti]);
                }
              }
            });
}

void effectBeagle(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(
      renderer, w, h, 5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float horY = h * 0.62F;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 60.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            if (y > horY)
            {
              const float ripple = std::sin(x * 0.3F + y * 0.15F + t * 6.0F);
              dst[static_cast<std::size_t>(y) * w + x] =
                  Rgb{u8(10 + l * 0.10F + ripple * 10), u8(30 + l * 0.12F + ripple * 14),
                      u8(60 + l * 0.18F + ripple * 18), false};
            }
            else
            {
              const float sf = static_cast<float>(y) / horY;
              dst[static_cast<std::size_t>(y) * w + x] =
                  Rgb{u8(8 + 20 * (1 - sf) + l * 0.06F), u8(12 + 20 * (1 - sf) + l * 0.06F),
                      u8(20 + 30 * (1 - sf) + l * 0.08F), false};
            }
          }
        for (int i = 0; i < 60; ++i)
        {
          const int sx = static_cast<int>(hash(i) * w);
          const int sy = static_cast<int>(hash(i * 3) * horY);
          const float tw = 0.5F + 0.5F * std::sin(t * 10.0F + i);
          dst[static_cast<std::size_t>(sy) * w + sx] = Rgb{u8(180 * tw), u8(180 * tw), u8(220 * tw), false};
        }
        // Ship moves left-to-right.
        const float sx = -mn * 0.3F + t * (w + mn * 0.6F);
        const float sy = horY - mn * 0.05F;
        const float SS = mn * 0.18F;
        const Rgb hull{20, 16, 14, false};
        // Hull (trapezoid).
        for (int yo = 0; yo <= static_cast<int>(SS * 0.18F); ++yo)
        {
          const float yf = yo / (SS * 0.18F);
          const int half = static_cast<int>(SS * (0.32F - 0.10F * yf));
          for (int xo = -half; xo <= half; ++xo)
          {
            const int xx = static_cast<int>(sx + xo);
            const int yy = static_cast<int>(sy + yo);
            if (xx >= 0 && xx < w && yy >= 0 && yy < h)
              dst[static_cast<std::size_t>(yy) * w + xx] = hull;
          }
        }
        // Three masts.
        for (int m = -1; m <= 1; ++m)
        {
          const float mx = sx + m * SS * 0.20F;
          drawSeg(dst, w, h, mx, sy, mx, sy - SS * 0.55F, std::max(1.0F, mn * 0.004F), ya, hull);
          // Sails.
          for (int s2 = 0; s2 < 3; ++s2)
          {
            const float sy2 = sy - SS * (0.12F + s2 * 0.16F);
            drawSeg(dst, w, h, mx - SS * 0.13F, sy2, mx + SS * 0.13F, sy2, SS * 0.05F, ya,
                    Rgb{230, 220, 200, false});
          }
        }
        // Years label.
        const std::string lab = "1831 - 1836";
        const int sc = std::max(2, static_cast<int>(mn / 50.0F));
        for (int ci = 0; ci < static_cast<int>(lab.size()); ++ci)
        {
          const char ch = lab[ci];
          if (ch == ' ') continue;
          const auto g = glyph5x7(ch);
          for (int fy = 0; fy < 7; ++fy)
            for (int fx = 0; fx < 5; ++fx)
              if (g[fy][fx] == '1')
                plotDot(dst, w, h, w * 0.5F - lab.size() * 3 * sc + (ci * 6 + fx) * sc,
                        h * 0.92F + fy * sc, std::max(1.0F, static_cast<float>(sc) * 0.5F), ya,
                        Rgb{220, 200, 160, false});
        }
      });
}

void effectEndosymbiosis(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float dim = 0.25F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 50.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(40 + l * 0.10F * dim), u8(60 + l * 0.10F * dim),
                          u8(50 + l * 0.10F * dim), false};
                }
              const float hostCx = w * 0.45F, hostCy = h * 0.5F;
              const float hostR = mn * 0.22F;
              // Host cell (data-textured).
              drawDataDisk(dst, w, h, src, hostCx, hostCy, hostR, ya, 0.70F, t * 0.3F,
                           Rgb{180, 200, 230, false});
              // Nucleus.
              plotDot(dst, w, h, hostCx + hostR * 0.20F, hostCy - hostR * 0.10F, hostR * 0.25F, ya,
                      Rgb{100, 80, 140, false});
              // Smaller bacterium approaches and enters.
              const float approach = std::clamp(t * 1.5F, 0.0F, 1.0F);
              const float bx = w * 0.85F - approach * (w * 0.85F - hostCx + hostR * 0.50F);
              const float by = hostCy + std::sin(t * 2.0F) * mn * 0.04F * (1 - approach);
              const float bR = mn * 0.08F;
              drawDataDisk(dst, w, h, src, bx, by, bR, ya, 0.75F, t * 1.0F,
                           Rgb{220, 130, 80, false});
              // Cristae folds inside the bacterium when settled.
              if (approach >= 0.7F)
              {
                const float inT = (approach - 0.7F) / 0.3F;
                for (int k = -2; k <= 2; ++k)
                {
                  const float ky = by + k * bR * 0.30F;
                  drawSeg(dst, w, h, bx - bR * 0.5F, ky, bx + bR * 0.5F, ky,
                          std::max(1.0F, mn * 0.003F * inT), ya, Rgb{120, 50, 30, false});
                }
              }
            });
}

void effectLucy(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 80.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(170 + l * 0.10F), u8(140 + l * 0.10F), u8(90 + l * 0.08F), false};
                }
              const float cx = w * 0.5F, baseY = h * 0.92F;
              const float H = mn * 0.50F;
              const Rgb bone{220, 215, 195, false};
              // Skull.
              plotDot(dst, w, h, cx, baseY - H * 0.92F, H * 0.10F, ya, bone);
              // Jaw.
              plotDot(dst, w, h, cx + H * 0.02F, baseY - H * 0.84F, H * 0.06F, ya, bone);
              // Spine.
              drawSeg(dst, w, h, cx, baseY - H * 0.80F, cx, baseY - H * 0.45F,
                      std::max(1.0F, mn * 0.004F), ya, bone);
              // Ribs.
              for (int r = 0; r < 4; ++r)
              {
                const float yy = baseY - H * (0.75F - r * 0.05F);
                drawSeg(dst, w, h, cx - H * (0.06F + r * 0.005F), yy,
                        cx + H * (0.06F + r * 0.005F), yy, std::max(1.0F, mn * 0.003F), ya, bone);
              }
              // Pelvis.
              for (int xo = -static_cast<int>(H * 0.10F); xo <= static_cast<int>(H * 0.10F); ++xo)
                for (int yo = 0; yo <= static_cast<int>(H * 0.05F); ++yo)
                {
                  const int xx = static_cast<int>(cx + xo);
                  const int yy = static_cast<int>(baseY - H * 0.45F) + yo;
                  if (xx >= 0 && xx < w && yy >= 0 && yy < h)
                    dst[static_cast<std::size_t>(yy) * w + xx] = bone;
                }
              // Legs (bent slightly, bipedal).
              drawSeg(dst, w, h, cx - H * 0.05F, baseY - H * 0.40F, cx - H * 0.08F, baseY,
                      std::max(1.0F, mn * 0.004F), ya, bone);
              drawSeg(dst, w, h, cx + H * 0.05F, baseY - H * 0.40F, cx + H * 0.08F, baseY,
                      std::max(1.0F, mn * 0.004F), ya, bone);
              // Arms.
              drawSeg(dst, w, h, cx, baseY - H * 0.72F, cx - H * 0.10F, baseY - H * 0.48F,
                      std::max(1.0F, mn * 0.003F), ya, bone);
              drawSeg(dst, w, h, cx, baseY - H * 0.72F, cx + H * 0.10F, baseY - H * 0.48F,
                      std::max(1.0F, mn * 0.003F), ya, bone);
              // Speech bubble at the end.
              if (t > 0.5F)
              {
                const std::string line = "3.2 MA";
                const int sc = std::max(3, static_cast<int>(mn / 25.0F));
                const float lineW = static_cast<float>(line.size()) * 6 * sc;
                const float bx = cx + H * 0.20F, by = baseY - H * 0.90F;
                for (int xo = -static_cast<int>(lineW * 0.55F);
                     xo <= static_cast<int>(lineW * 0.55F); ++xo)
                  for (int yo = -static_cast<int>(sc * 5); yo <= static_cast<int>(sc * 5); ++yo)
                  {
                    const int xx = static_cast<int>(bx + xo);
                    const int yy = static_cast<int>(by + yo);
                    if (xx >= 0 && xx < w && yy >= 0 && yy < h)
                      dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{240, 230, 200, false};
                  }
                for (int ci = 0; ci < static_cast<int>(line.size()); ++ci)
                {
                  const char ch = line[ci];
                  if (ch == ' ') continue;
                  const auto g = glyph5x7(ch);
                  for (int fy = 0; fy < 7; ++fy)
                    for (int fx = 0; fx < 5; ++fx)
                      if (g[fy][fx] == '1')
                        plotDot(dst, w, h, bx - lineW * 0.5F + (ci * 6 + fx) * sc,
                                by - sc * 3 + fy * sc,
                                std::max(1.0F, static_cast<float>(sc) * 0.5F), ya,
                                Rgb{40, 30, 20, false});
                }
              }
            });
}

void effectMitochondrialEve(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
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
            const float l = s.transparent ? 30.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(20 + l * 0.10F), u8(20 + l * 0.10F), u8(28 + l * 0.10F), false};
          }
        const float cx = w * 0.5F, cy = h * 0.5F;
        const int N = 24;
        for (int i = 0; i < N; ++i)
        {
          const float a = i / static_cast<float>(N) * 6.2832F;
          const float R = std::min(w, h) * 0.4F;
          const float reveal = std::clamp(t * 1.5F - i * 0.02F, 0.0F, 1.0F);
          if (reveal <= 0.0F) continue;
          const float dx = cx + std::cos(a) * R * reveal;
          const float dy = cy + std::sin(a) * R * reveal / ya;
          drawSeg(dst, w, h, cx, cy, dx, dy, std::max(1.0F, mn * 0.003F), ya,
                  Rgb{240, 180, 200, false});
          drawDataDisk(dst, w, h, src, dx, dy, mn * 0.018F, ya, 0.75F, 0.0F,
                       Rgb{200, 140, 160, false});
        }
        // Eve at centre — data-textured.
        drawDataDisk(dst, w, h, src, cx, cy, mn * 0.05F, ya, 0.85F, t * 0.2F,
                     Rgb{255, 100, 140, false});
        plotDot(dst, w, h, cx, cy, mn * 0.020F, ya, Rgb{255, 240, 240, false});
      });
}

void effectCoEvolution(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 70.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  const float sf = static_cast<float>(y) / h;
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(200 - 30 * sf + l * 0.10F), u8(160 - 20 * sf + l * 0.10F),
                          u8(110 - 30 * sf + l * 0.08F), false};
                }
              const float groundY = h * 0.92F;
              for (int x = 0; x < w; ++x)
                for (int yo = 0; yo <= 2; ++yo)
                  if (groundY + yo < h)
                    dst[static_cast<std::size_t>(groundY + yo) * w + x] = Rgb{120, 90, 50, false};
              const float scale = t;
              // Tree on the right (acacia umbrella).
              const float treeX = w * 0.7F;
              const float treeTop = groundY - mn * 0.65F * scale;
              drawSeg(dst, w, h, treeX, groundY, treeX, treeTop, std::max(1.0F, mn * 0.012F), ya,
                      Rgb{90, 60, 30, false});
              for (int b = 0; b < 8; ++b)
              {
                const float a = (b - 3.5F) * 0.3F;
                const float bx = treeX + std::sin(a) * mn * 0.18F;
                const float by = treeTop - std::cos(a) * mn * 0.08F;
                plotDot(dst, w, h, bx, by, mn * 0.055F, ya, Rgb{70, 130, 70, false});
              }
              // Giraffe on the left.
              const float gx = w * 0.30F;
              const float bodyY = groundY - mn * 0.18F;
              const Rgb hide{220, 180, 110, false};
              const Rgb spots{120, 80, 40, false};
              // Body
              for (int yo = -static_cast<int>(mn * 0.06F); yo <= static_cast<int>(mn * 0.06F); ++yo)
                for (int xo = -static_cast<int>(mn * 0.12F); xo <= static_cast<int>(mn * 0.12F); ++xo)
                {
                  const int xx = static_cast<int>(gx + xo);
                  const int yy = static_cast<int>(bodyY + yo);
                  if (xx >= 0 && xx < w && yy >= 0 && yy < h)
                    dst[static_cast<std::size_t>(yy) * w + xx] = hide;
                }
              // Spots
              for (int i = 0; i < 12; ++i)
              {
                const float sa = i / 12.0F * 6.2832F;
                plotDot(dst, w, h, gx + std::cos(sa) * mn * 0.08F,
                        bodyY + std::sin(sa) * mn * 0.04F, std::max(1.0F, mn * 0.012F), ya, spots);
              }
              // Long neck — grows with t.
              const float neckTop = groundY - mn * 0.40F - mn * 0.30F * scale;
              drawSeg(dst, w, h, gx + mn * 0.08F, bodyY - mn * 0.04F, gx + mn * 0.20F, neckTop,
                      std::max(1.0F, mn * 0.04F), ya, hide);
              // Head + tongue.
              plotDot(dst, w, h, gx + mn * 0.22F, neckTop, mn * 0.030F, ya, hide);
              drawSeg(dst, w, h, gx + mn * 0.24F, neckTop, gx + mn * 0.34F, neckTop,
                      std::max(1.0F, mn * 0.006F), ya, Rgb{220, 90, 130, false});
              // Legs.
              for (int leg = 0; leg < 4; ++leg)
              {
                const float lx = gx - mn * 0.10F + leg * mn * 0.07F;
                drawSeg(dst, w, h, lx, bodyY + mn * 0.04F, lx, groundY, std::max(1.0F, mn * 0.014F),
                        ya, hide);
              }
            });
}

void effectSelection(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  constexpr int N = 30;
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float dim = 0.25F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 50.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(40 + l * 0.10F * dim), u8(50 + l * 0.10F * dim),
                          u8(36 + l * 0.10F * dim), false};
                }
              for (int i = 0; i < N; ++i)
              {
                const float x0 = hash(i) * w;
                const float y0 = hash(i * 3) * h;
                const float sz = mn * (0.012F + 0.030F * hash(i * 7));
                const bool large = sz > mn * 0.025F;
                const bool culled = large && t > 0.4F;
                if (culled && t > 0.4F + hash(i * 9) * 0.3F) continue;
                drawDataDisk(dst, w, h, src, x0, y0, sz, ya, 0.75F, t + i,
                             Rgb{160, 200, 120, false});
              }
              // Predator hint: a darting shadow.
              if (t > 0.3F && t < 0.6F)
              {
                const float pt = (t - 0.3F) / 0.3F;
                const float px = -mn * 0.2F + pt * (w + mn * 0.4F);
                const float py = h * 0.5F + std::sin(pt * 6.0F) * mn * 0.15F;
                plotDot(dst, w, h, px, py, mn * 0.06F, ya, Rgb{30, 20, 30, false});
              }
            });
}

void effectCetacean(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
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
            const float l = s.transparent ? 60.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            const float sf = static_cast<float>(y) / h;
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(120 + 80 * (1 - sf) + l * 0.08F), u8(170 + 50 * (1 - sf) + l * 0.10F),
                    u8(220 - 40 * sf + l * 0.12F), false};
          }
        const float groundY = h * 0.85F;
        for (int x = 0; x < w; ++x)
          for (int yo = 0; yo <= 1; ++yo)
            dst[static_cast<std::size_t>(groundY + yo) * w + x] = Rgb{60, 100, 130, false};
        // Three sequential silhouettes; each reveals over its portion of t.
        const Rgb body{30, 30, 40, false};
        for (int stage = 0; stage < 3; ++stage)
        {
          const float reveal = std::clamp(t * 3.0F - stage, 0.0F, 1.0F);
          if (reveal <= 0.0F) continue;
          const float cx = w * (0.20F + stage * 0.30F);
          const float cy = groundY - mn * 0.10F;
          const float bodyL = mn * 0.16F;
          // Body = elongated ellipse.
          for (int yo = -static_cast<int>(mn * 0.03F); yo <= static_cast<int>(mn * 0.03F); ++yo)
            for (int xo = -static_cast<int>(bodyL); xo <= static_cast<int>(bodyL); ++xo)
            {
              const float nx = xo / bodyL, ny = yo / (mn * 0.03F);
              if (nx * nx + ny * ny > 1.0F) continue;
              const int xx = static_cast<int>(cx + xo);
              const int yy = static_cast<int>(cy + yo);
              if (xx >= 0 && xx < w && yy >= 0 && yy < h)
                dst[static_cast<std::size_t>(yy) * w + xx] = body;
            }
          // Legs (stage 0 + 1) or flippers (stage 2).
          if (stage == 0)
          {
            for (int leg = 0; leg < 4; ++leg)
            {
              const float lx = cx + (leg - 1.5F) * bodyL * 0.4F;
              drawSeg(dst, w, h, lx, cy, lx, groundY, std::max(1.0F, mn * 0.010F), ya, body);
            }
          }
          else if (stage == 1)
          {
            for (int leg = 0; leg < 4; ++leg)
            {
              const float lx = cx + (leg - 1.5F) * bodyL * 0.4F;
              drawSeg(dst, w, h, lx, cy + mn * 0.02F, lx + mn * 0.025F, cy + mn * 0.06F,
                      std::max(1.0F, mn * 0.008F), ya, body);
            }
          }
          else
          {
            // Flippers + tail fluke
            drawSeg(dst, w, h, cx - bodyL * 0.4F, cy + mn * 0.02F, cx - bodyL * 0.6F,
                    cy + mn * 0.07F, std::max(1.0F, mn * 0.010F), ya, body);
            drawSeg(dst, w, h, cx + bodyL, cy, cx + bodyL * 1.4F, cy - mn * 0.05F,
                    std::max(1.0F, mn * 0.014F), ya, body);
            drawSeg(dst, w, h, cx + bodyL, cy, cx + bodyL * 1.4F, cy + mn * 0.05F,
                    std::max(1.0F, mn * 0.014F), ya, body);
          }
          // Eye.
          plotDot(dst, w, h, cx - bodyL * 0.7F, cy - mn * 0.005F, std::max(1.0F, mn * 0.005F), ya,
                  Rgb{220, 220, 220, false});
        }
      });
}

}  // namespace ee_detail
}  // namespace Qdless
