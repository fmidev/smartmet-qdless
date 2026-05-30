#include "QdlessExitEffectCommon.h"

namespace Qdless
{
namespace ee_detail
{

void effectBanksyBalloon(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
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
                      Rgb{u8(220 + l * 0.10F), u8(212 + l * 0.10F), u8(196 + l * 0.08F), false};
                }
              const float rise = std::clamp(t / 0.85F, 0.0F, 1.0F);
              const float bx = w * (0.45F + 0.06F * std::sin(t * 1.7F));
              const float by = h * (1.05F - rise * 1.05F);
              const float R = mn * 0.10F;
              const Rgb red{210, 30, 30, false};
              // String trailing down.
              drawSeg(dst, w, h, bx, by + R * 1.05F, bx + std::sin(t * 1.5F) * mn * 0.04F,
                      h * 0.95F, std::max(1.0F, mn * 0.004F), ya, Rgb{60, 60, 60, false});
              // Heart shape: two circles + downward triangle. Shred from bottom
              // upward over the last quarter of the animation.
              const float shred = std::clamp((t - 0.72F) / 0.28F, 0.0F, 1.0F);
              const int yTop = static_cast<int>(by - R * 1.10F);
              const int yBot = static_cast<int>(by + R * 1.20F);
              for (int yy = yTop; yy <= yBot; ++yy)
              {
                if (yy < 0 || yy >= h) continue;
                const float ny = (yy - by) / R;
                for (int xx = static_cast<int>(bx - R * 1.15F);
                     xx <= static_cast<int>(bx + R * 1.15F); ++xx)
                {
                  if (xx < 0 || xx >= w) continue;
                  const float nx = (xx - bx) / R;
                  // Heart curve: (x² + y² - 1)³ - x² y³ ≤ 0
                  const float a = nx * nx + ny * ny - 1.0F;
                  const float heart = a * a * a - nx * nx * ny * ny * ny;
                  if (heart <= 0.0F)
                  {
                    // Below shred-front: replace with strip drifting downward.
                    const float shredFront = (1.0F - shred) * 2.2F - 1.1F;
                    if (ny > shredFront) continue;
                    dst[static_cast<std::size_t>(yy) * w + xx] = red;
                  }
                }
              }
              // Drifting paper strips below the shred front.
              if (shred > 0.0F)
              {
                for (int i = 0; i < 14; ++i)
                {
                  const float u = static_cast<float>(i) / 14.0F;
                  const float fx =
                      bx + (u - 0.5F) * R * 2.0F + std::sin(t * 4.0F + i) * mn * 0.02F;
                  const float fy = by + R * 1.0F + shred * shred * mn * 0.5F * (0.5F + u);
                  drawSeg(dst, w, h, fx, fy, fx + mn * 0.005F, fy + mn * 0.04F,
                          std::max(1.0F, mn * 0.005F), ya, red);
                }
              }
            });
}

void effectBoneChandelier(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  // Target shape, world-relative to the chandelier centre.
  struct Piece
  {
    float x, y;    // target (in units of R)
    float r;       // dot radius scale
    bool isSkull;  // skulls get eye pits
  };
  std::vector<Piece> pieces;
  const float R = mn * 0.18F;
  // Vertical chain from ceiling.
  for (int k = 0; k < 6; ++k)
    pieces.push_back({0.0F, -1.55F + k * 0.10F, 0.06F, false});
  // Central skull crown — 6 skulls in a horizontal ring.
  for (int k = 0; k < 6; ++k)
  {
    const float a = k / 6.0F * 6.2832F;
    pieces.push_back({std::cos(a) * 0.55F, std::sin(a) * 0.20F, 0.16F, true});
  }
  // Outer ring of finger bones.
  for (int k = 0; k < 16; ++k)
  {
    const float a = k / 16.0F * 6.2832F;
    pieces.push_back({std::cos(a) * 1.0F, std::sin(a) * 0.40F + 0.35F, 0.10F, false});
  }
  // Cross femurs at the bottom.
  pieces.push_back({-0.5F, 0.85F, 0.12F, false});
  pieces.push_back({0.5F, 0.85F, 0.12F, false});
  pieces.push_back({0.0F, 0.95F, 0.14F, true});  // bottom skull
  runFrames(
      renderer, w, h, 5600,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float dim = std::clamp(1.0F - t * 0.8F, 0.18F, 1.0F);
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 30.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(28 + l * 0.12F * dim), u8(22 + l * 0.10F * dim),
                    u8(18 + l * 0.08F * dim), false};
          }
        const float cxw = w * 0.5F;
        const float cy = h * 0.42F + std::sin(t * 1.4F) * mn * 0.012F;
        const Rgb bone{222, 210, 188, false};
        const Rgb eye{14, 10, 12, false};
        for (std::size_t i = 0; i < pieces.size(); ++i)
        {
          const Piece& p = pieces[i];
          const float pt = std::clamp(t / 0.85F - hash(static_cast<int>(i) * 7) * 0.20F, 0.0F, 1.0F);
          const float startX = cxw + (hash(static_cast<int>(i)) - 0.5F) * w * 0.6F;
          const float startY = h * 1.05F;
          const float targetX = cxw + p.x * R;
          const float targetY = cy + p.y * R;
          const float ease = 1.0F - (1.0F - pt) * (1.0F - pt);
          const float px = startX + (targetX - startX) * ease;
          const float py = startY + (targetY - startY) * ease;
          if (pt <= 0.0F) continue;
          plotDot(dst, w, h, px, py, mn * 0.022F * p.r * 3.0F, ya, bone);
          if (p.isSkull && pt > 0.7F)
          {
            plotDot(dst, w, h, px - mn * 0.012F, py - mn * 0.004F, std::max(1.0F, mn * 0.008F),
                    ya, eye);
            plotDot(dst, w, h, px + mn * 0.012F, py - mn * 0.004F, std::max(1.0F, mn * 0.008F),
                    ya, eye);
          }
        }
        // Tea-light flicker around the central ring once the structure assembles.
        if (t > 0.7F)
        {
          const float flick = (1.0F + std::sin(t * 13.0F)) * 0.5F;
          for (int k = 0; k < 6; ++k)
          {
            const float a = k / 6.0F * 6.2832F;
            plotDot(dst, w, h, cxw + std::cos(a) * 0.55F * R, cy + std::sin(a) * 0.20F * R - mn * 0.03F,
                    mn * 0.015F * (0.7F + 0.5F * flick), ya, Rgb{255, 210, 130, false});
          }
        }
      });
}

void effectCeciNestPas(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
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
                      Rgb{u8(218 + l * 0.10F), u8(204 + l * 0.10F), u8(168 + l * 0.08F), false};
                }
              const Rgb ink{34, 24, 18, false};
              const float pipeCx = w * 0.5F, pipeCy = h * 0.40F;
              const float bowlR = mn * 0.07F;
              const float stemLen = mn * 0.35F;
              const float draw = std::clamp(t / 0.55F, 0.0F, 1.0F);
              // Stem.
              const float stemEnd = pipeCx + stemLen * draw;
              drawSeg(dst, w, h, pipeCx, pipeCy, stemEnd, pipeCy, std::max(1.5F, mn * 0.010F), ya,
                      ink);
              // Bowl (rises out of the stem near its left end).
              if (draw > 0.55F)
              {
                const float bd = std::clamp((draw - 0.55F) / 0.45F, 0.0F, 1.0F);
                for (float a = -0.2F; a < 2.0F * 3.14159F * bd; a += 0.06F)
                {
                  const float bxd = pipeCx + std::sin(a) * bowlR;
                  const float byd = pipeCy - bowlR * 0.6F - bowlR * (1.0F - std::cos(a));
                  plotDot(dst, w, h, bxd, byd, std::max(1.0F, mn * 0.008F), ya, ink);
                }
              }
              // Text below the pipe — types out letter by letter.
              const std::string line = "CECI N'EST PAS UN TERMINAL";
              const float typeT = std::clamp((t - 0.45F) / 0.50F, 0.0F, 1.0F);
              const int nReveal = static_cast<int>(typeT * line.size());
              const int sc = std::max(2, static_cast<int>(mn / 70.0F));
              const float lineW = static_cast<float>(line.size()) * 6 * sc;
              const float startX = pipeCx - lineW * 0.5F;
              const float startY = pipeCy + mn * 0.20F;
              for (int ci = 0; ci < nReveal && ci < static_cast<int>(line.size()); ++ci)
              {
                const char ch = line[ci];
                const auto g = glyph5x7(ch);
                for (int fy = 0; fy < 7; ++fy)
                  for (int fx = 0; fx < 5; ++fx)
                    if (g[fy][fx] == '1')
                      plotDot(dst, w, h, startX + (ci * 6 + fx) * sc, startY + fy * sc,
                              std::max(1.0F, static_cast<float>(sc) * 0.5F), ya, ink);
              }
            });
}

void effectHokusaiWave(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(
      renderer, w, h, 5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float dim = std::clamp(1.0F - t * 1.2F, 0.30F, 1.0F);
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 70.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(230 + l * 0.06F * dim), u8(210 + l * 0.08F * dim),
                    u8(170 + l * 0.06F * dim), false};
          }
        const float adv = std::clamp(t / 0.85F, 0.0F, 1.0F);
        // Wave body: a curling arch from the right edge, growing as adv climbs.
        const float baseY = h * 0.62F;
        const float originX = w * (1.10F - adv * 0.95F);
        const Rgb prussian{18, 38, 100, false};
        const Rgb deep{8, 20, 60, false};
        const Rgb foam{240, 240, 245, false};
        const float reach = mn * 1.4F * adv;
        for (float u = 0.0F; u <= 1.0F; u += 0.015F)
        {
          const float ang = u * 3.10F;  // 0..pi -> arc
          const float r = reach * (0.30F + 0.7F * u);
          const float cxw = originX - r * std::sin(ang) * 0.7F;
          const float cyw = baseY - r * (1.0F - std::cos(ang)) * 0.45F;
          plotDot(dst, w, h, cxw, cyw, mn * 0.022F, ya, prussian);
          if (u > 0.65F)
          {
            // Curl tip — claws of foam fingers.
            const float fingerAng = ang + 0.6F * std::sin(t * 5.0F + u * 7.0F);
            const float fx = cxw + std::cos(fingerAng) * mn * 0.05F;
            const float fy = cyw + std::sin(fingerAng) * mn * 0.05F;
            plotDot(dst, w, h, fx, fy, mn * 0.012F, ya, foam);
          }
        }
        // Body shading — darker underbelly.
        for (float u = 0.0F; u <= 1.0F; u += 0.03F)
        {
          const float ang = u * 3.10F;
          const float r = reach * (0.30F + 0.7F * u) - mn * 0.05F;
          const float cxw = originX - r * std::sin(ang) * 0.7F;
          const float cyw = baseY - r * (1.0F - std::cos(ang)) * 0.45F + mn * 0.04F;
          plotDot(dst, w, h, cxw, cyw, mn * 0.018F, ya, deep);
        }
        // Sea surface in front.
        for (int x = 0; x < w; x += 4)
        {
          const float sy = baseY + std::sin(x * 0.18F + t * 6.0F) * mn * 0.012F + mn * 0.06F;
          plotDot(dst, w, h, static_cast<float>(x), sy, mn * 0.018F, ya, prussian);
        }
      });
}

void effectMagritteBowler(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float dim = 0.35F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 60.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  const float sf = static_cast<float>(y) / h;
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(150 + 80 * sf + l * 0.10F * dim),
                          u8(170 + 50 * sf + l * 0.10F * dim),
                          u8(200 + 20 * sf + l * 0.08F * dim), false};
                }
              const float cx = w * 0.5F, baseY = h * 0.85F;
              const float S = mn * 0.50F;
              const Rgb coat{20, 26, 40, false};
              const Rgb face{210, 175, 140, false};
              const Rgb hat{14, 14, 18, false};
              const Rgb apple{40, 160, 60, false};
              const Rgb appleLeaf{80, 50, 40, false};
              // Body: trapezoidal coat.
              for (int yy = static_cast<int>(baseY - S * 0.5F); yy <= static_cast<int>(baseY); ++yy)
              {
                const float yf = (yy - (baseY - S * 0.5F)) / (S * 0.5F);
                const int half = static_cast<int>(S * (0.20F + 0.20F * yf));
                for (int xo = -half; xo <= half; ++xo)
                  if (cx + xo >= 0 && cx + xo < w && yy >= 0 && yy < h)
                    dst[static_cast<std::size_t>(yy) * w + static_cast<int>(cx + xo)] = coat;
              }
              // Head.
              plotDot(dst, w, h, cx, baseY - S * 0.6F, S * 0.16F, ya, face);
              // Bowler hat: dome + flat brim.
              for (int yy = static_cast<int>(baseY - S * 0.82F); yy <= static_cast<int>(baseY - S * 0.70F);
                   ++yy)
              {
                const float yf = (yy - (baseY - S * 0.82F)) / (S * 0.12F);
                const int half = static_cast<int>(S * (0.14F + 0.04F * yf));
                for (int xo = -half; xo <= half; ++xo)
                  if (cx + xo >= 0 && cx + xo < w && yy >= 0 && yy < h)
                    dst[static_cast<std::size_t>(yy) * w + static_cast<int>(cx + xo)] = hat;
              }
              drawSeg(dst, w, h, cx - S * 0.20F, baseY - S * 0.70F, cx + S * 0.20F, baseY - S * 0.70F,
                      std::max(1.0F, mn * 0.008F), ya, hat);
              // Apple floats up to cover face.
              const float rise = std::clamp(t / 0.65F, 0.0F, 1.0F);
              const float ay = baseY - S * 0.10F - rise * S * 0.50F;
              plotDot(dst, w, h, cx, ay, S * 0.13F, ya, apple);
              drawSeg(dst, w, h, cx, ay - S * 0.13F, cx + S * 0.03F, ay - S * 0.22F,
                      std::max(1.0F, mn * 0.006F), ya, appleLeaf);
            });
}

void effectMunchScream(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(
      renderer, w, h, 5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        for (int y = 0; y < h; ++y)
        {
          const float sf = static_cast<float>(y) / h;
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 70.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            const float wave =
                std::sin(x * 0.06F + y * 0.04F + t * 2.0F) * 0.06F + 1.0F;
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8((220 - 40 * sf + l * 0.10F) * wave),
                    u8((110 - 80 * sf + l * 0.10F) * wave),
                    u8((50 + 30 * sf + l * 0.05F) * wave), false};
          }
        }
        const float arrive = std::clamp(t / 0.30F, 0.0F, 1.0F);
        const float cx = w * (0.32F + 0.04F * std::sin(t * 1.5F));
        const float cy = h * (0.55F + 0.03F * std::cos(t * 1.5F));
        const float S = mn * 0.30F * arrive;
        if (S < 4.0F) return;
        const Rgb fig{30, 20, 35, false};
        const Rgb pale{218, 195, 150, false};
        // Body — wavy trapezoid descending.
        for (int yy = static_cast<int>(cy + S * 0.05F); yy <= static_cast<int>(cy + S * 1.30F); ++yy)
        {
          const float yf = (yy - cy) / (S * 1.30F);
          const float wob = std::sin(yy * 0.15F + t * 3.0F) * S * 0.06F;
          const int half = static_cast<int>(S * (0.18F + 0.16F * yf));
          for (int xo = -half; xo <= half; ++xo)
            if (cx + xo + wob >= 0 && cx + xo + wob < w && yy >= 0 && yy < h)
              dst[static_cast<std::size_t>(yy) * w + static_cast<int>(cx + xo + wob)] = fig;
        }
        // Head — skull oval.
        plotDot(dst, w, h, cx, cy - S * 0.15F, S * 0.25F, ya, pale);
        // Eyes — hollow O's.
        plotDot(dst, w, h, cx - S * 0.08F, cy - S * 0.20F, S * 0.05F, ya, fig);
        plotDot(dst, w, h, cx + S * 0.08F, cy - S * 0.20F, S * 0.05F, ya, fig);
        // Mouth — vertical O.
        plotDot(dst, w, h, cx, cy - S * 0.05F, S * 0.06F, ya, fig);
        // Hands cupped to cheeks.
        plotDot(dst, w, h, cx - S * 0.25F, cy - S * 0.12F, S * 0.10F, ya, pale);
        plotDot(dst, w, h, cx + S * 0.25F, cy - S * 0.12F, S * 0.10F, ya, pale);
      });
}

void effectWarholBanana(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5000,
            [&](float t, std::vector<Rgb>& dst)
            {
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                  dst[static_cast<std::size_t>(y) * w + x] =
                      src[static_cast<std::size_t>(y) * w + x];
              const float p = std::clamp(t, 0.0F, 1.0F);
              const float bx = -mn * 0.4F + p * (w + mn * 0.8F);
              const float by = h * 0.5F;
              const float S = mn * 0.22F;
              const Rgb yellow{255, 220, 60, false};
              const Rgb tip{120, 90, 30, false};
              // Body — curved banana arc.
              for (float u = -1.0F; u <= 1.0F; u += 0.02F)
              {
                const float px = bx + u * S;
                const float py = by + std::sin(u * 1.8F) * S * 0.5F;
                for (float v = -0.18F; v <= 0.18F; v += 0.04F)
                {
                  const float ppx = px;
                  const float ppy = py + v * S * (1.0F - std::fabs(u) * 0.5F);
                  plotDot(dst, w, h, ppx, ppy, std::max(1.0F, mn * 0.012F), ya, yellow);
                }
              }
              // Stems on each end.
              plotDot(dst, w, h, bx - S * 1.0F, by + std::sin(-1.8F) * S * 0.5F + S * 0.1F,
                      mn * 0.012F, ya, tip);
              plotDot(dst, w, h, bx + S * 1.0F, by + std::sin(1.8F) * S * 0.5F - S * 0.05F,
                      mn * 0.012F, ya, tip);
              // Pale afterglow to the left of the banana — where it's been.
              const int trailEnd = static_cast<int>(bx - S * 0.8F);
              for (int xx = 0; xx < trailEnd && xx < w; ++xx)
              {
                const float fade = std::max(0.0F, 1.0F - (trailEnd - xx) / (mn * 0.7F));
                if (fade <= 0.0F) continue;
                const int yyTop = std::max(0, static_cast<int>(by - S * 0.7F));
                const int yyBot = std::min(h - 1, static_cast<int>(by + S * 0.7F));
                for (int yy = yyTop; yy <= yyBot; ++yy)
                {
                  Rgb& c = dst[static_cast<std::size_t>(yy) * w + xx];
                  c = Rgb{u8(c.r + (255 - c.r) * fade * 0.25F),
                          u8(c.g + (230 - c.g) * fade * 0.25F),
                          u8(c.b + (120 - c.b) * fade * 0.20F), false};
                }
              }
            });
}

void effectMonaLisa(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
          const float l = s.transparent ? 60.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
          const float sf = static_cast<float>(y) / h;
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(80 + 50 * (1 - sf) + l * 0.10F), u8(60 + 40 * (1 - sf) + l * 0.10F), u8(40 + 30 * sf + l * 0.08F), false};
        }
      const float cx = w * 0.5F, cy = h * 0.55F;
      const float FW = mn * 0.30F, FH = mn * 0.45F;
      // Frame.
      for (int yo = static_cast<int>(-FH); yo <= static_cast<int>(FH); ++yo)
        for (int xo = -static_cast<int>(FW); xo <= static_cast<int>(FW); ++xo) {
          const int xx = static_cast<int>(cx + xo), yy = static_cast<int>(cy + yo);
          if (xx < 0 || xx >= w || yy < 0 || yy >= h) continue;
          const float ax = std::fabs(xo) / FW, ay = std::fabs(yo) / FH;
          if (ax > 0.95F || ay > 0.95F)
            dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{180, 130, 50, false};
        }
      // Dress: data-sampled trapezoid below face.
      for (int yo = static_cast<int>(FH * 0.05F); yo <= static_cast<int>(FH * 0.95F); ++yo)
        for (int xo = -static_cast<int>(FW * 0.85F); xo <= static_cast<int>(FW * 0.85F); ++xo) {
          const float yf = yo / (FH * 0.95F);
          const float xf = std::fabs(xo) / (FW * 0.85F);
          if (xf > 0.40F + 0.40F * yf) continue;
          const int xx = static_cast<int>(cx + xo), yy = static_cast<int>(cy + yo);
          if (xx < 0 || xx >= w || yy < 0 || yy >= h) continue;
          const Rgb d = sample(src, w, h, xx, yy);
          const float dr = d.transparent ? 60.0F : d.r;
          const float dg = d.transparent ? 50.0F : d.g;
          const float db = d.transparent ? 30.0F : d.b;
          dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{u8(40 + dr * 0.25F), u8(30 + dg * 0.20F), u8(20 + db * 0.15F), false};
        }
      // Face (oval).
      drawDataDisk(dst, w, h, src, cx, cy - FH * 0.30F, FW * 0.30F, ya, 0.5F, 0.0F, Rgb{220, 190, 150, false});
      // Hair.
      drawSeg(dst, w, h, cx - FW * 0.30F, cy - FH * 0.50F, cx + FW * 0.30F, cy - FH * 0.50F,
              std::max(1.0F, mn * 0.020F), ya, Rgb{60, 40, 20, false});
      drawSeg(dst, w, h, cx - FW * 0.30F, cy - FH * 0.50F, cx - FW * 0.32F, cy - FH * 0.10F,
              std::max(1.0F, mn * 0.015F), ya, Rgb{60, 40, 20, false});
      drawSeg(dst, w, h, cx + FW * 0.30F, cy - FH * 0.50F, cx + FW * 0.32F, cy - FH * 0.10F,
              std::max(1.0F, mn * 0.015F), ya, Rgb{60, 40, 20, false});
      // Eyes.
      plotDot(dst, w, h, cx - FW * 0.10F, cy - FH * 0.35F, mn * 0.008F, ya, Rgb{40, 30, 20, false});
      plotDot(dst, w, h, cx + FW * 0.10F, cy - FH * 0.35F, mn * 0.008F, ya, Rgb{40, 30, 20, false});
      // Smile (ambiguous — slightly curved arc).
      const float smileT = std::clamp(t * 2.0F, 0.0F, 1.0F);
      for (int k = -8; k <= 8; ++k) {
        const float a = k / 8.0F;
        const float sx = cx + a * FW * 0.10F;
        const float syp = cy - FH * 0.18F + a * a * mn * 0.012F * smileT;
        plotDot(dst, w, h, sx, syp, std::max(1.0F, mn * 0.004F), ya, Rgb{120, 50, 40, false});
      }
    });
}

void effectSistine(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
          const float l = s.transparent ? 70.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(200 + l * 0.10F), u8(160 + l * 0.10F), u8(120 + l * 0.08F), false};
        }
      const float cy = h * 0.55F;
      const float reach = std::clamp(t * 1.2F, 0.0F, 1.0F);
      const float gap = mn * (0.10F - 0.08F * reach);
      const float god_x = w * 0.5F - gap - mn * 0.18F;
      const float adam_x = w * 0.5F + gap + mn * 0.18F;
      const Rgb skin{220, 180, 130, false};
      // God's hand from left.
      plotDot(dst, w, h, god_x - mn * 0.04F, cy, mn * 0.06F, ya, skin);  // palm
      drawSeg(dst, w, h, god_x - mn * 0.05F, cy, god_x + mn * 0.10F, cy - mn * 0.01F,
              std::max(1.0F, mn * 0.014F), ya, skin);  // finger
      plotDot(dst, w, h, god_x + mn * 0.10F, cy - mn * 0.01F, mn * 0.012F, ya, skin);
      // Adam's hand from right.
      plotDot(dst, w, h, adam_x + mn * 0.04F, cy + mn * 0.005F, mn * 0.055F, ya, skin);
      drawSeg(dst, w, h, adam_x + mn * 0.05F, cy + mn * 0.005F, adam_x - mn * 0.10F, cy - mn * 0.005F,
              std::max(1.0F, mn * 0.012F), ya, skin);
      plotDot(dst, w, h, adam_x - mn * 0.10F, cy - mn * 0.005F, mn * 0.010F, ya, skin);
      // Spark of life: a small glow between the fingertips.
      if (gap < mn * 0.05F) {
        const float glow = 1.0F - gap / (mn * 0.05F);
        const float midx = (god_x + mn * 0.10F + adam_x - mn * 0.10F) * 0.5F;
        const float midy = (cy - mn * 0.01F + cy - mn * 0.005F) * 0.5F;
        plotDot(dst, w, h, midx, midy, mn * 0.025F * glow, ya, Rgb{u8(255 * glow), u8(240 * glow), u8(180 * glow), false});
      }
      // Sun-like data disk above as background sky element.
      drawDataDisk(dst, w, h, src, w * 0.5F, h * 0.20F, mn * 0.10F, ya, 0.40F, t * 0.2F, Rgb{240, 200, 160, false});
    });
}

void effectCezanneStill(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  FootSprite f = loadPythonFootSprite();
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      // Painterly background.
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb d = sample(src, w, h, x, y);
          const float dl = d.transparent ? 50.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          const float sf = static_cast<float>(y) / h;
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(120 + dl * 0.30F - 20 * sf), u8(100 + dl * 0.30F - 30 * sf),
                  u8(70 + dl * 0.20F), false};
        }
      // Wooden table — horizontal slab.
      const float tableY = h * 0.68F;
      for (int yy = static_cast<int>(tableY); yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx) {
          const Rgb d = sample(src, w, h, xx, yy);
          const float dr = d.transparent ? 100.0F : d.r;
          const float yf = (yy - tableY) / (h - tableY);
          dst[static_cast<std::size_t>(yy) * w + xx] =
              Rgb{u8(140 + dr * 0.20F - 30 * yf), u8(80 - 20 * yf), u8(40), false};
        }
      // Bowl: a wide brown ellipse on the table.
      const float bowlCx = w * 0.5F, bowlCy = tableY - mn * 0.02F;
      for (int yo = -static_cast<int>(mn * 0.08F); yo <= static_cast<int>(mn * 0.02F); ++yo)
        for (int xo = -static_cast<int>(mn * 0.25F); xo <= static_cast<int>(mn * 0.25F); ++xo) {
          const float nx = xo / (mn * 0.25F), nyN = yo / (mn * 0.08F);
          if (nx * nx + nyN * nyN > 1.0F) continue;
          const int xx = static_cast<int>(bowlCx + xo), yy = static_cast<int>(bowlCy + yo);
          if (xx >= 0 && xx < w && yy >= 0 && yy < h)
            dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{120, 70, 30, false};
        }
      // Bowl rim ellipse.
      for (float a = 0; a < 6.2832F; a += 0.02F)
        plotDot(dst, w, h, bowlCx + std::cos(a) * mn * 0.25F,
                bowlCy - mn * 0.07F + std::sin(a) * mn * 0.025F / ya,
                std::max(1.0F, mn * 0.004F), ya, Rgb{80, 50, 20, false});
      // Feet inside / overhanging the bowl.
      const float settle = std::clamp(t * 1.2F, 0.0F, 1.0F);
      drawFootSprite(dst, w, h, ya, f.img.get(), f.fw, f.fh, f.needChromaKey,
                     bowlCx - mn * 0.10F, bowlCy - mn * 0.05F * settle, mn * 0.12F, -0.4F);
      drawFootSprite(dst, w, h, ya, f.img.get(), f.fw, f.fh, f.needChromaKey,
                     bowlCx + mn * 0.08F, bowlCy - mn * 0.06F * settle, mn * 0.11F, 0.3F);
      drawFootSprite(dst, w, h, ya, f.img.get(), f.fw, f.fh, f.needChromaKey,
                     bowlCx + mn * 0.18F, bowlCy - mn * 0.04F * settle, mn * 0.10F, 0.9F);
    });
}

}  // namespace ee_detail
}  // namespace Qdless
