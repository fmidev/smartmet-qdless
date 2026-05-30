#include "QdlessExitEffectCommon.h"

namespace Qdless
{
namespace ee_detail
{

void effectPyramids(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      // Desert sky → sand gradient with data sampled in.
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
          const float l = s.transparent ? 60.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
          const float sf = static_cast<float>(y) / h;
          const bool desert = y > h * 0.62F;
          dst[static_cast<std::size_t>(y) * w + x] =
              desert ? Rgb{u8(210 + l * 0.10F - 30 * sf), u8(170 + l * 0.10F - 30 * sf), u8(110 + l * 0.10F - 30 * sf), false}
                     : Rgb{u8(240 - 40 * sf + l * 0.06F), u8(180 - 60 * sf + l * 0.06F), u8(120 + 10 * sf + l * 0.05F), false};
        }
      const float groundY = h * 0.78F;
      const float grow = std::clamp(t * 1.3F, 0.0F, 1.0F);
      const Rgb stone{210, 180, 130, false};
      const Rgb shadow{160, 130, 80, false};
      // Three pyramids: big middle, medium left, small right.
      auto pyramid = [&](float cx, float baseW, float ht) {
        for (int yo = 0; yo <= static_cast<int>(ht * grow); ++yo) {
          const float yf = yo / ht;
          const float half = baseW * 0.5F * (1.0F - yf);
          for (int xo = -static_cast<int>(half); xo <= static_cast<int>(half); ++xo) {
            const int xx = static_cast<int>(cx + xo);
            const int yy = static_cast<int>(groundY - yo);
            if (xx < 0 || xx >= w || yy < 0 || yy >= h) continue;
            const Rgb& c = (xo > 0) ? shadow : stone;
            dst[static_cast<std::size_t>(yy) * w + xx] = c;
          }
        }
      };
      pyramid(w * 0.50F, mn * 0.32F, mn * 0.30F);
      pyramid(w * 0.30F, mn * 0.24F, mn * 0.22F);
      pyramid(w * 0.72F, mn * 0.18F, mn * 0.16F);
      // Sphinx — small dome to the right of the small pyramid (data-textured body).
      const float spX = w * 0.85F, spY = groundY - mn * 0.04F;
      drawDataDisk(dst, w, h, src, spX, spY, mn * 0.05F, ya, 0.7F, 0.0F, Rgb{200, 160, 110, false});
      plotDot(dst, w, h, spX - mn * 0.02F, spY - mn * 0.04F, mn * 0.02F, ya, Rgb{200, 160, 110, false});
      // Sun — data-textured disk.
      drawDataDisk(dst, w, h, src, w * 0.85F, h * 0.18F, mn * 0.08F, ya, 0.55F, t * 0.3F, Rgb{255, 230, 160, false});
      // Palms — stylised silhouettes on the right.
      drawSeg(dst, w, h, w * 0.10F, groundY, w * 0.10F, groundY - mn * 0.15F, std::max(1.0F, mn * 0.004F), ya, Rgb{80, 60, 40, false});
      for (int k = -3; k <= 3; ++k) {
        const float a = k / 3.0F * 0.6F;
        drawSeg(dst, w, h, w * 0.10F, groundY - mn * 0.15F,
                w * 0.10F + std::sin(a) * mn * 0.08F, groundY - mn * 0.15F - std::cos(a) * mn * 0.06F,
                std::max(1.0F, mn * 0.003F), ya, Rgb{60, 120, 60, false});
      }
    });
}

void effectStonehenge(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
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
          const float sf = static_cast<float>(y) / h;
          dst[static_cast<std::size_t>(y) * w + x] =
              (y > h * 0.72F)
                  ? Rgb{u8(50 + 20 * sf + l * 0.10F), u8(60 + 30 * sf + l * 0.10F), u8(40 + 10 * sf + l * 0.08F), false}
                  : Rgb{u8(220 - 60 * sf + l * 0.08F), u8(140 - 20 * sf + l * 0.08F), u8(80 + 20 * sf + l * 0.06F), false};
        }
      const float groundY = h * 0.78F;
      const float sunY = groundY - std::clamp(t / 0.5F, 0.0F, 1.0F) * mn * 0.30F;
      drawDataDisk(dst, w, h, src, w * 0.5F, sunY, mn * 0.10F, ya, 0.45F, t * 0.3F, Rgb{255, 200, 100, false});
      // Trilithons: pairs of uprights with a lintel across the top, in a curved row.
      const Rgb stone{120, 110, 100, false};
      for (int i = 0; i < 7; ++i) {
        const float a = (i - 3.0F) * 0.32F;
        const float cx = w * 0.5F + std::sin(a) * mn * 0.40F;
        const float zoom = std::cos(a);
        const float stoneH = mn * 0.20F * zoom;
        const float stoneW = mn * 0.045F * zoom;
        const float baseY = groundY;
        // Two uprights.
        for (int s = -1; s <= 1; s += 2) {
          drawDataDisk(dst, w, h, src, cx + s * mn * 0.04F * zoom, baseY - stoneH * 0.5F, stoneW, ya, 0.85F, 0.0F, stone);
          (void)stoneW;
          for (int yo = 0; yo <= static_cast<int>(stoneH); ++yo)
            for (int xo = -static_cast<int>(stoneW * 0.5F); xo <= static_cast<int>(stoneW * 0.5F); ++xo) {
              const int xx = static_cast<int>(cx + s * mn * 0.04F * zoom + xo);
              const int yy = static_cast<int>(baseY - yo);
              if (xx >= 0 && xx < w && yy >= 0 && yy < h) {
                const Rgb d = sample(src, w, h, xx, yy);
                const float dr = d.transparent ? 100.0F : d.r;
                dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{u8(80 + dr * 0.2F), u8(75), u8(70), false};
              }
            }
        }
        // Lintel.
        for (int xo = -static_cast<int>(mn * 0.07F * zoom); xo <= static_cast<int>(mn * 0.07F * zoom); ++xo) {
          const int xx = static_cast<int>(cx + xo);
          for (int yo = 0; yo <= static_cast<int>(mn * 0.02F * zoom); ++yo) {
            const int yy = static_cast<int>(baseY - stoneH) - yo;
            if (xx >= 0 && xx < w && yy >= 0 && yy < h) {
              const Rgb d = sample(src, w, h, xx, yy);
              const float dr = d.transparent ? 100.0F : d.r;
              dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{u8(70 + dr * 0.18F), u8(68), u8(64), false};
            }
          }
        }
      }
    });
}

void effectTrojanHorse(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
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
              Rgb{u8(140 + 80 * (1 - sf) + l * 0.10F), u8(90 + 50 * (1 - sf) + l * 0.10F), u8(60 + 20 * sf + l * 0.06F), false};
        }
      const float groundY = h * 0.85F;
      for (int x = 0; x < w; ++x)
        dst[static_cast<std::size_t>(groundY) * w + x] = Rgb{80, 60, 40, false};
      // Walls of Troy on the right.
      const float wallX = w * 0.78F;
      for (int yy = static_cast<int>(h * 0.20F); yy <= static_cast<int>(groundY); ++yy)
        for (int xo = 0; xo < static_cast<int>(mn * 0.08F); ++xo) {
          const int xx = static_cast<int>(wallX) + xo;
          if (xx >= 0 && xx < w) {
            const bool brick = ((yy / 8) % 2 == 0) ^ ((xo / 8) % 2 == 0);
            dst[static_cast<std::size_t>(yy) * w + xx] = brick ? Rgb{160, 120, 80, false} : Rgb{120, 90, 60, false};
          }
        }
      // Battlement.
      for (int xo = 0; xo < static_cast<int>(mn * 0.08F); xo += static_cast<int>(mn * 0.012F)) {
        const int xx = static_cast<int>(wallX) + xo;
        const int top = static_cast<int>(h * 0.20F);
        if (xx >= 0 && xx < w && top - 4 >= 0)
          for (int yo = 0; yo < 4; ++yo)
            dst[static_cast<std::size_t>(top - yo) * w + xx] = Rgb{160, 120, 80, false};
      }
      // Horse silhouette + data-textured body.
      const float arrive = std::clamp(t * 1.4F, 0.0F, 1.0F);
      const float hcx = w * 0.30F + arrive * w * 0.20F;
      const float hcy = groundY - mn * 0.13F;
      const float HS = mn * 0.13F;
      drawDataDisk(dst, w, h, src, hcx, hcy, HS, ya, 0.85F, 0.0F, Rgb{130, 90, 50, false});
      // Body: elongated oval data-textured.
      for (int yo = -static_cast<int>(HS * 0.40F); yo <= static_cast<int>(HS * 0.40F); ++yo)
        for (int xo = -static_cast<int>(HS * 1.20F); xo <= static_cast<int>(HS * 0.80F); ++xo) {
          const float nx = xo / (HS * 1.20F), nyN = yo / (HS * 0.40F);
          if (nx * nx + nyN * nyN > 1.0F) continue;
          const int xx = static_cast<int>(hcx + xo), yy = static_cast<int>(hcy + yo);
          if (xx < 0 || xx >= w || yy < 0 || yy >= h) continue;
          const Rgb d = sample(src, w, h, xx, yy);
          const float dr = d.transparent ? 130.0F : d.r;
          const float dg = d.transparent ? 90.0F : d.g;
          const float db = d.transparent ? 50.0F : d.b;
          dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{u8(130 * 0.5F + dr * 0.4F), u8(90 * 0.5F + dg * 0.4F), u8(50 * 0.5F + db * 0.4F), false};
        }
      // Neck + head.
      drawSeg(dst, w, h, hcx + HS * 0.7F, hcy, hcx + HS * 1.1F, hcy - HS * 0.6F, HS * 0.20F, ya, Rgb{120, 80, 40, false});
      plotDot(dst, w, h, hcx + HS * 1.20F, hcy - HS * 0.70F, HS * 0.22F, ya, Rgb{120, 80, 40, false});
      // Legs (4).
      for (int leg = 0; leg < 4; ++leg) {
        const float lx = hcx + (leg - 1.5F) * HS * 0.40F;
        drawSeg(dst, w, h, lx, hcy + HS * 0.30F, lx, groundY, HS * 0.10F, ya, Rgb{100, 70, 35, false});
      }
      // Door in belly.
      for (int yo = -static_cast<int>(HS * 0.10F); yo <= static_cast<int>(HS * 0.10F); ++yo)
        for (int xo = -static_cast<int>(HS * 0.15F); xo <= static_cast<int>(HS * 0.15F); ++xo) {
          const int xx = static_cast<int>(hcx + xo), yy = static_cast<int>(hcy + HS * 0.10F + yo);
          if (xx >= 0 && xx < w && yy >= 0 && yy < h)
            dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{40, 30, 20, false};
        }
    });
}

void effectParthenon(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
          const float l = s.transparent ? 50.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
          const float sf = static_cast<float>(y) / h;
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(220 + l * 0.08F - 50 * sf), u8(150 + l * 0.06F - 20 * sf), u8(70 + l * 0.05F + 30 * sf), false};
        }
      const float baseY = h * 0.82F;
      // Hill/Acropolis underneath.
      for (int y = static_cast<int>(baseY); y < h; ++y)
        for (int x = 0; x < w; ++x)
          dst[static_cast<std::size_t>(y) * w + x] = Rgb{120, 100, 80, false};
      const float left = w * 0.18F, right = w * 0.82F;
      const float colTop = baseY - mn * 0.30F;
      const float colBot = baseY;
      const Rgb marble{230, 220, 200, false};
      // Eight columns.
      const int nCols = 8;
      const float reveal = std::clamp(t / 0.6F, 0.0F, 1.0F);
      for (int c = 0; c < nCols; ++c) {
        const float frac = static_cast<float>(c) / (nCols - 1);
        const float cx = left + (right - left) * frac;
        const float colW = mn * 0.025F;
        const float topRev = colBot - (colBot - colTop) * std::clamp(reveal * nCols - c, 0.0F, 1.0F);
        for (int yy = static_cast<int>(topRev); yy <= static_cast<int>(colBot); ++yy)
          for (int xo = -static_cast<int>(colW * 0.5F); xo <= static_cast<int>(colW * 0.5F); ++xo) {
            const int xx = static_cast<int>(cx + xo);
            if (xx < 0 || xx >= w || yy < 0 || yy >= h) continue;
            const Rgb d = sample(src, w, h, xx, yy);
            const float dr = d.transparent ? 200.0F : d.r;
            const float dg = d.transparent ? 200.0F : d.g;
            const float db = d.transparent ? 200.0F : d.b;
            dst[static_cast<std::size_t>(yy) * w + xx] =
                Rgb{u8(marble.r * 0.6F + dr * 0.30F), u8(marble.g * 0.6F + dg * 0.30F), u8(marble.b * 0.6F + db * 0.30F), false};
          }
      }
      // Entablature (horizontal beam across the tops).
      if (reveal > 0.85F) {
        for (int yo = -static_cast<int>(mn * 0.025F); yo <= 0; ++yo)
          for (int x = static_cast<int>(left); x <= static_cast<int>(right); ++x) {
            const int yy = static_cast<int>(colTop) + yo;
            if (yy >= 0 && yy < h)
              dst[static_cast<std::size_t>(yy) * w + x] = Rgb{200, 190, 170, false};
          }
        // Pediment triangle.
        for (int yo = 0; yo <= static_cast<int>(mn * 0.05F); ++yo) {
          const float yf = yo / (mn * 0.05F);
          const int half = static_cast<int>((right - left) * 0.5F * (1.0F - yf));
          for (int xo = -half; xo <= half; ++xo) {
            const int xx = static_cast<int>(w * 0.5F + xo);
            const int yy = static_cast<int>(colTop - mn * 0.025F) - yo;
            if (xx >= 0 && xx < w && yy >= 0 && yy < h)
              dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{215, 205, 180, false};
          }
        }
      }
    });
}

void effectPompeii(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      const float ashDim = std::clamp(t * 0.6F, 0.0F, 0.85F);
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
          const float l = s.transparent ? 50.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
          const float gloom = 0.5F + 0.5F * ashDim;
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(180 - 100 * gloom + l * 0.05F), u8(120 - 70 * gloom + l * 0.05F), u8(80 - 50 * gloom + l * 0.04F), false};
        }
      const float groundY = h * 0.85F;
      for (int x = 0; x < w; ++x)
        dst[static_cast<std::size_t>(groundY) * w + x] = Rgb{60, 50, 40, false};
      // Vesuvius — large data-textured cone in background.
      const float vcx = w * 0.50F, vcy = h * 0.30F;
      for (int yy = static_cast<int>(vcy); yy <= static_cast<int>(groundY); ++yy) {
        const float yf = (yy - vcy) / (groundY - vcy);
        const int half = static_cast<int>(mn * 0.30F * yf);
        for (int xo = -half; xo <= half; ++xo) {
          const int xx = static_cast<int>(vcx + xo);
          if (xx < 0 || xx >= w || yy >= h) continue;
          const Rgb d = sample(src, w, h, xx, yy);
          const float dr = d.transparent ? 80.0F : d.r;
          const float dg = d.transparent ? 70.0F : d.g;
          const float db = d.transparent ? 60.0F : d.b;
          dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{u8(80 + dr * 0.20F), u8(60 + dg * 0.20F), u8(40 + db * 0.15F), false};
        }
      }
      // Eruption plume.
      const Rgb ashColor{180, 160, 150, false};
      const Rgb lavaColor{255, 100, 30, false};
      for (int i = 0; i < 60; ++i) {
        const float ph = hash(i) * 6.2832F;
        const float pt = std::fmod(t + hash(i * 3), 1.0F);
        const float px = vcx + (hash(i * 7) - 0.5F) * mn * 0.20F + std::cos(ph) * mn * 0.05F;
        const float py = vcy - pt * mn * 0.30F + std::sin(ph) * mn * 0.05F;
        plotDot(dst, w, h, px, py, mn * 0.025F * (1.0F - pt), ya, ashColor);
      }
      for (int i = 0; i < 12; ++i) {
        const float a = (hash(i) - 0.5F) * 0.6F;
        const float pt = std::fmod(t * 0.5F + hash(i * 5), 1.0F);
        plotDot(dst, w, h, vcx + std::sin(a) * mn * 0.30F * pt, vcy + std::cos(a) * mn * 0.05F + pt * mn * 0.20F,
                mn * 0.012F, ya, lavaColor);
      }
      // Roman buildings — small silhouettes; gradually buried.
      const float burial = std::clamp(t * 0.6F, 0.0F, 1.0F) * mn * 0.06F;
      for (int b = 0; b < 6; ++b) {
        const float bx = w * 0.10F + b * w * 0.13F;
        const float bw = mn * 0.05F;
        const float bh = mn * (0.04F + 0.02F * std::sin(b * 3.0F));
        for (int yo = -static_cast<int>(bh); yo <= 0; ++yo)
          for (int xo = -static_cast<int>(bw * 0.5F); xo <= static_cast<int>(bw * 0.5F); ++xo) {
            const int xx = static_cast<int>(bx + xo);
            const int yy = static_cast<int>(groundY + yo + burial);
            if (xx >= 0 && xx < w && yy >= 0 && yy < h && yo > -burial)
              dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{170, 150, 130, false};  // ash burial
            else if (xx >= 0 && xx < w && yy >= 0 && yy < h)
              dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{120, 90, 60, false};
          }
      }
    });
}

void effectMagnaCarta(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
          const float l = s.transparent ? 50.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(40 + l * 0.10F), u8(30 + l * 0.08F), u8(20 + l * 0.06F), false};
        }
      // Parchment block.
      const float pxL = w * 0.18F, pxR = w * 0.82F;
      const float pyT = h * 0.12F, pyB = h * 0.88F;
      for (int yy = static_cast<int>(pyT); yy <= static_cast<int>(pyB); ++yy)
        for (int xx = static_cast<int>(pxL); xx <= static_cast<int>(pxR); ++xx) {
          if (xx < 0 || xx >= w || yy < 0 || yy >= h) continue;
          const Rgb d = sample(src, w, h, xx, yy);
          const float dr = d.transparent ? 220.0F : d.r;
          const float dg = d.transparent ? 200.0F : d.g;
          const float db = d.transparent ? 160.0F : d.b;
          dst[static_cast<std::size_t>(yy) * w + xx] =
              Rgb{u8(232 + dr * 0.05F), u8(216 + dg * 0.05F), u8(170 + db * 0.05F), false};
        }
      // Edges (slight darker fringe).
      for (int yy = static_cast<int>(pyT); yy <= static_cast<int>(pyB); ++yy)
        for (int xx = static_cast<int>(pxL); xx <= static_cast<int>(pxR); ++xx) {
          if (xx == static_cast<int>(pxL) || xx == static_cast<int>(pxR) ||
              yy == static_cast<int>(pyT) || yy == static_cast<int>(pyB))
            dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{170, 140, 90, false};
        }
      // Title: "MAGNA CARTA" scratched in left-to-right.
      const std::string title = "MAGNA CARTA";
      const int sc = std::max(3, static_cast<int>(mn / 25.0F));
      const float lineW = static_cast<float>(title.size()) * 6 * sc;
      const int nReveal = static_cast<int>(std::clamp(t * 1.3F, 0.0F, 1.0F) * title.size());
      const float startX = (w - lineW) * 0.5F;
      const float startY = pyT + mn * 0.12F;
      for (int ci = 0; ci < nReveal && ci < static_cast<int>(title.size()); ++ci) {
        const char ch = title[ci];
        if (ch == ' ') continue;
        const auto g = glyph5x7(ch);
        for (int fy = 0; fy < 7; ++fy)
          for (int fx = 0; fx < 5; ++fx)
            if (g[fy][fx] == '1')
              plotDot(dst, w, h, startX + (ci * 6 + fx) * sc, startY + fy * sc,
                      std::max(1.0F, static_cast<float>(sc) * 0.5F), ya, Rgb{30, 20, 10, false});
      }
      // Date: "1215" types out at the bottom in the second half.
      if (t > 0.6F) {
        const std::string date = "1215";
        const float dateW = static_cast<float>(date.size()) * 6 * sc;
        for (int ci = 0; ci < static_cast<int>(date.size()); ++ci) {
          const char ch = date[ci];
          const auto g = glyph5x7(ch);
          for (int fy = 0; fy < 7; ++fy)
            for (int fx = 0; fx < 5; ++fx)
              if (g[fy][fx] == '1')
                plotDot(dst, w, h, (w - dateW) * 0.5F + (ci * 6 + fx) * sc, pyB - mn * 0.10F + fy * sc,
                        std::max(1.0F, static_cast<float>(sc) * 0.5F), ya, Rgb{120, 30, 30, false});
        }
      }
      // Quill — moves with the type position.
      if (nReveal < static_cast<int>(title.size())) {
        const float qx = startX + (nReveal * 6 + 2) * sc;
        drawSeg(dst, w, h, qx, startY + sc * 3, qx + mn * 0.04F, startY + sc * 3 - mn * 0.10F,
                std::max(1.0F, mn * 0.005F), ya, Rgb{200, 200, 200, false});
        plotDot(dst, w, h, qx, startY + sc * 3, std::max(1.0F, mn * 0.005F), ya, Rgb{30, 20, 10, false});
      }
    });
}

void effectVikingLongboat(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      const float seaY = h * 0.62F;
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
          const float l = s.transparent ? 60.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
          if (y > seaY) {
            const float ripple = std::sin(x * 0.18F + y * 0.10F + t * 6.0F);
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(20 + l * 0.10F + ripple * 10), u8(50 + l * 0.14F + ripple * 14), u8(90 + l * 0.18F + ripple * 18), false};
          } else {
            const float sf = static_cast<float>(y) / seaY;
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(140 + 80 * (1 - sf) + l * 0.08F), u8(120 + 60 * (1 - sf) + l * 0.08F), u8(80 + 60 * sf + l * 0.06F), false};
          }
        }
      const float bx = -mn * 0.4F + t * (w + mn * 0.8F);
      const float by = seaY - mn * 0.05F;
      const float L = mn * 0.36F;
      const Rgb hull{80, 40, 20, false};
      // Hull (curved up on both ends).
      for (int xo = -static_cast<int>(L); xo <= static_cast<int>(L); ++xo) {
        const float xf = xo / L;
        const float bend = mn * 0.10F * std::pow(std::fabs(xf), 3.0F);
        for (int yo = 0; yo <= static_cast<int>(mn * 0.05F); ++yo) {
          const int xx = static_cast<int>(bx + xo);
          const int yy = static_cast<int>(by + yo - bend);
          if (xx >= 0 && xx < w && yy >= 0 && yy < h)
            dst[static_cast<std::size_t>(yy) * w + xx] = hull;
        }
      }
      // Dragon prow.
      drawSeg(dst, w, h, bx + L * 0.92F, by, bx + L * 1.18F, by - mn * 0.12F, std::max(1.0F, mn * 0.010F), ya, hull);
      plotDot(dst, w, h, bx + L * 1.18F, by - mn * 0.12F, mn * 0.020F, ya, Rgb{180, 30, 30, false});
      // Stern.
      drawSeg(dst, w, h, bx - L * 0.92F, by, bx - L * 1.12F, by - mn * 0.09F, std::max(1.0F, mn * 0.008F), ya, hull);
      // Shields (round, data-textured).
      for (int s = 0; s < 7; ++s) {
        const float sx = bx + (s - 3.0F) * mn * 0.06F;
        const Rgb stripe = (s & 1) ? Rgb{180, 30, 30, false} : Rgb{240, 220, 180, false};
        drawDataDisk(dst, w, h, src, sx, by - mn * 0.025F, mn * 0.025F, ya, 0.75F, 0.0F, stripe);
      }
      // Mast + sail.
      const float mx = bx, mast_top = by - mn * 0.30F;
      drawSeg(dst, w, h, mx, by - mn * 0.05F, mx, mast_top, std::max(1.0F, mn * 0.006F), ya, hull);
      // Sail: striped rectangle.
      const float sailH = mn * 0.20F, sailW = mn * 0.22F;
      for (int yo = 0; yo <= static_cast<int>(sailH); ++yo) {
        const Rgb stripe = ((yo / static_cast<int>(sailH / 5)) & 1) ? Rgb{220, 60, 60, false} : Rgb{220, 220, 200, false};
        for (int xo = -static_cast<int>(sailW * 0.5F); xo <= static_cast<int>(sailW * 0.5F); ++xo) {
          const int xx = static_cast<int>(mx + xo);
          const int yy = static_cast<int>(mast_top + yo);
          if (xx >= 0 && xx < w && yy >= 0 && yy < h)
            dst[static_cast<std::size_t>(yy) * w + xx] = stripe;
        }
      }
    });
}

void effectGutenberg(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
          const float l = s.transparent ? 40.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(40 + l * 0.10F), u8(30 + l * 0.08F), u8(20 + l * 0.06F), false};
        }
      const float cx = w * 0.5F;
      const Rgb wood{120, 70, 30, false};
      // Press frame: vertical posts.
      drawSeg(dst, w, h, cx - mn * 0.25F, h * 0.12F, cx - mn * 0.25F, h * 0.88F, std::max(1.0F, mn * 0.012F), ya, wood);
      drawSeg(dst, w, h, cx + mn * 0.25F, h * 0.12F, cx + mn * 0.25F, h * 0.88F, std::max(1.0F, mn * 0.012F), ya, wood);
      // Top beam.
      drawSeg(dst, w, h, cx - mn * 0.30F, h * 0.12F, cx + mn * 0.30F, h * 0.12F, std::max(1.0F, mn * 0.014F), ya, wood);
      // Bed of press at bottom.
      drawSeg(dst, w, h, cx - mn * 0.30F, h * 0.78F, cx + mn * 0.30F, h * 0.78F, std::max(1.0F, mn * 0.014F), ya, wood);
      // Screw + platen — moves down with t over a cycle.
      const float cycle = std::fmod(t * 1.6F, 1.0F);
      const float platenY = h * (0.25F + 0.40F * cycle);
      drawSeg(dst, w, h, cx, h * 0.12F, cx, platenY, std::max(1.0F, mn * 0.008F), ya, wood);
      // Platen plate (data-sampled).
      for (int yo = 0; yo <= static_cast<int>(mn * 0.03F); ++yo)
        for (int xo = -static_cast<int>(mn * 0.18F); xo <= static_cast<int>(mn * 0.18F); ++xo) {
          const int xx = static_cast<int>(cx + xo);
          const int yy = static_cast<int>(platenY + yo);
          if (xx < 0 || xx >= w || yy < 0 || yy >= h) continue;
          const Rgb d = sample(src, w, h, xx, yy);
          const float dr = d.transparent ? 130.0F : d.r;
          dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{u8(140 + dr * 0.2F), u8(80), u8(30), false};
        }
      // Paper on the bed.
      for (int yo = 0; yo <= 2; ++yo)
        for (int xo = -static_cast<int>(mn * 0.20F); xo <= static_cast<int>(mn * 0.20F); ++xo) {
          const int xx = static_cast<int>(cx + xo);
          const int yy = static_cast<int>(h * 0.76F) - yo;
          if (xx >= 0 && xx < w && yy >= 0 && yy < h)
            dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{240, 232, 200, false};
        }
      // Imprinted text on paper.
      const std::string line = "1450";
      const int sc = std::max(2, static_cast<int>(mn / 35.0F));
      const float lineW = static_cast<float>(line.size()) * 6 * sc;
      for (int ci = 0; ci < static_cast<int>(line.size()); ++ci) {
        const char ch = line[ci];
        const auto g = glyph5x7(ch);
        for (int fy = 0; fy < 7; ++fy)
          for (int fx = 0; fx < 5; ++fx)
            if (g[fy][fx] == '1')
              plotDot(dst, w, h, cx - lineW * 0.5F + (ci * 6 + fx) * sc, h * 0.74F + fy * sc,
                      std::max(1.0F, static_cast<float>(sc) * 0.5F), ya, Rgb{40, 30, 20, false});
      }
    });
}

void effectGuillotine(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
          const float l = s.transparent ? 30.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(40 + l * 0.10F), u8(30 + l * 0.08F), u8(30 + l * 0.08F), false};
        }
      // Crowd silhouettes at the bottom.
      const float groundY = h * 0.88F;
      for (int x = 0; x < w; ++x)
        dst[static_cast<std::size_t>(groundY) * w + x] = Rgb{10, 10, 14, false};
      for (int i = 0; i < 40; ++i) {
        const float hx = (i / 40.0F) * w;
        const float hy = groundY - mn * (0.02F + 0.03F * std::sin(i * 1.7F));
        plotDot(dst, w, h, hx, hy, mn * 0.015F, ya, Rgb{20, 20, 25, false});
      }
      // Guillotine frame.
      const float cx = w * 0.5F;
      const Rgb wood{100, 60, 30, false};
      drawSeg(dst, w, h, cx - mn * 0.10F, groundY - mn * 0.50F, cx - mn * 0.10F, groundY,
              std::max(1.0F, mn * 0.012F), ya, wood);
      drawSeg(dst, w, h, cx + mn * 0.10F, groundY - mn * 0.50F, cx + mn * 0.10F, groundY,
              std::max(1.0F, mn * 0.012F), ya, wood);
      drawSeg(dst, w, h, cx - mn * 0.10F, groundY - mn * 0.50F, cx + mn * 0.10F, groundY - mn * 0.50F,
              std::max(1.0F, mn * 0.014F), ya, wood);
      // Blade — rises in the first half, falls in the second.
      const float fall = (t > 0.55F) ? std::clamp((t - 0.55F) / 0.10F, 0.0F, 1.0F) : 0.0F;
      const float bladeY = groundY - mn * 0.50F + std::clamp(t / 0.55F, 0.0F, 1.0F) * mn * 0.10F + fall * mn * 0.32F;
      const Rgb steel{200, 200, 220, false};
      for (int yo = 0; yo <= static_cast<int>(mn * 0.08F); ++yo)
        for (int xo = -static_cast<int>(mn * 0.09F); xo <= static_cast<int>(mn * 0.09F); ++xo) {
          const float xf = (xo + mn * 0.09F) / (mn * 0.18F);
          const float yf = yo / (mn * 0.08F);
          if (yf > 1.0F - xf) continue;
          const int xx = static_cast<int>(cx + xo), yy = static_cast<int>(bladeY + yo);
          if (xx >= 0 && xx < w && yy >= 0 && yy < h)
            dst[static_cast<std::size_t>(yy) * w + xx] = steel;
        }
      // Tricolor flag on a pole to the right.
      const float fpx = w * 0.92F;
      drawSeg(dst, w, h, fpx, groundY, fpx, groundY - mn * 0.40F, std::max(1.0F, mn * 0.006F), ya, wood);
      const float wave = std::sin(t * 8.0F) * mn * 0.012F;
      static const std::array<Rgb, 3> kFR = {Rgb{30, 60, 180, false}, Rgb{240, 240, 240, false}, Rgb{220, 30, 30, false}};
      for (int b = 0; b < 3; ++b) {
        const float bx0 = fpx - mn * 0.12F + b * mn * 0.04F;
        const float bx1 = bx0 + mn * 0.04F;
        const float byT = groundY - mn * 0.40F;
        const float byB = byT + mn * 0.20F;
        for (int yy = static_cast<int>(byT); yy <= static_cast<int>(byB); ++yy)
          for (int xx = static_cast<int>(bx0); xx <= static_cast<int>(bx1); ++xx) {
            const int xxw = xx + static_cast<int>(wave * std::sin(yy * 0.4F));
            if (xxw >= 0 && xxw < w && yy >= 0 && yy < h)
              dst[static_cast<std::size_t>(yy) * w + xxw] = kFR[b];
          }
      }
    });
}

void effectNapoleon(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
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
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(170 + l * 0.10F), u8(150 + l * 0.10F), u8(120 + l * 0.08F), false};
        }
      // Background tricolor bands.
      const float xa = w * 0.18F, xb = w * 0.82F;
      const float ya0 = h * 0.20F, ya1 = h * 0.80F;
      const Rgb& blue = (Rgb){30, 60, 180, false};
      const Rgb& white = (Rgb){240, 240, 240, false};
      const Rgb& red = (Rgb){220, 30, 30, false};
      const std::array<Rgb, 3> bands = {blue, white, red};
      for (int yy = static_cast<int>(ya0); yy <= static_cast<int>(ya1); ++yy)
        for (int xx = static_cast<int>(xa); xx <= static_cast<int>(xb); ++xx) {
          const float xf = (xx - xa) / (xb - xa);
          const int bIdx = std::clamp(static_cast<int>(xf * 3.0F), 0, 2);
          if (xx >= 0 && xx < w && yy >= 0 && yy < h)
            dst[static_cast<std::size_t>(yy) * w + xx] = bands[bIdx];
        }
      // Napoleon silhouette centre.
      const float cx = w * 0.5F, cy = h * 0.55F;
      const Rgb fig{20, 14, 10, false};
      // Body.
      for (int yo = -static_cast<int>(mn * 0.08F); yo <= static_cast<int>(mn * 0.15F); ++yo) {
        const float yf = (yo + mn * 0.08F) / (mn * 0.23F);
        const int half = static_cast<int>(mn * (0.05F + 0.04F * yf));
        for (int xo = -half; xo <= half; ++xo) {
          const int xx = static_cast<int>(cx + xo), yy = static_cast<int>(cy + yo);
          if (xx >= 0 && xx < w && yy >= 0 && yy < h)
            dst[static_cast<std::size_t>(yy) * w + xx] = fig;
        }
      }
      // Head.
      drawDataDisk(dst, w, h, src, cx, cy - mn * 0.12F, mn * 0.04F, ya, 0.7F, 0.0F, Rgb{210, 180, 140, false});
      // Bicorne hat.
      for (int xo = -static_cast<int>(mn * 0.09F); xo <= static_cast<int>(mn * 0.09F); ++xo) {
        const float xf = std::fabs(xo) / (mn * 0.09F);
        const int yy = static_cast<int>(cy - mn * 0.17F + xf * xf * mn * 0.02F);
        for (int yo = 0; yo < static_cast<int>(mn * 0.020F); ++yo) {
          const int xx = static_cast<int>(cx + xo);
          if (xx >= 0 && xx < w && yy + yo >= 0 && yy + yo < h)
            dst[static_cast<std::size_t>(yy + yo) * w + xx] = fig;
        }
      }
      // Hand in coat (slightly offset white triangle).
      drawSeg(dst, w, h, cx - mn * 0.03F, cy - mn * 0.06F, cx + mn * 0.02F, cy + mn * 0.02F,
              std::max(1.0F, mn * 0.008F), ya, Rgb{240, 230, 200, false});
      // Year label.
      const std::string year = "1804";
      const int sc = std::max(3, static_cast<int>(mn / 25.0F));
      const float lineW = static_cast<float>(year.size()) * 6 * sc;
      for (int ci = 0; ci < static_cast<int>(year.size()); ++ci) {
        const auto g = glyph5x7(year[ci]);
        for (int fy = 0; fy < 7; ++fy)
          for (int fx = 0; fx < 5; ++fx)
            if (g[fy][fx] == '1')
              plotDot(dst, w, h, cx - lineW * 0.5F + (ci * 6 + fx) * sc, h * 0.86F + fy * sc,
                      std::max(1.0F, static_cast<float>(sc) * 0.5F), ya, Rgb{40, 30, 20, false});
      }
    });
}

void effectColumbus(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      const float seaY = h * 0.62F;
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
          const float l = s.transparent ? 60.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
          if (y > seaY) {
            const float ripple = std::sin(x * 0.20F + y * 0.10F + t * 4.0F);
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(20 + l * 0.10F + ripple * 8), u8(60 + l * 0.15F + ripple * 10), u8(110 + l * 0.20F + ripple * 14), false};
          } else {
            const float sf = static_cast<float>(y) / seaY;
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(220 - 50 * sf + l * 0.06F), u8(180 - 40 * sf + l * 0.06F), u8(140 + 10 * sf + l * 0.06F), false};
          }
        }
      // Three caravels in a row, advancing right-to-left as they sail west.
      for (int s = 0; s < 3; ++s) {
        const float bx = w * 1.15F - t * w * 1.2F + s * mn * 0.20F;
        if (bx < -mn * 0.2F) continue;
        const float by = seaY - mn * 0.04F;
        const float SS = mn * 0.08F;
        const Rgb hull{60, 40, 20, false};
        // Hull (small trapezoid).
        for (int yo = 0; yo <= static_cast<int>(SS * 0.30F); ++yo) {
          const float yf = yo / (SS * 0.30F);
          const int half = static_cast<int>(SS * (1.0F - 0.30F * yf));
          for (int xo = -half; xo <= half; ++xo) {
            const int xx = static_cast<int>(bx + xo), yy = static_cast<int>(by + yo);
            if (xx >= 0 && xx < w && yy >= 0 && yy < h)
              dst[static_cast<std::size_t>(yy) * w + xx] = hull;
          }
        }
        // Three masts + square sails with red cross.
        for (int m = -1; m <= 1; ++m) {
          const float mx = bx + m * SS * 0.4F;
          drawSeg(dst, w, h, mx, by, mx, by - SS * 1.3F, std::max(1.0F, mn * 0.004F), ya, hull);
          for (int sy = 0; sy < 2; ++sy) {
            const float syp = by - SS * (0.4F + sy * 0.4F);
            const float sw = SS * 0.3F;
            for (int yo = -static_cast<int>(SS * 0.15F); yo <= 0; ++yo)
              for (int xo = -static_cast<int>(sw); xo <= static_cast<int>(sw); ++xo) {
                const int xx = static_cast<int>(mx + xo), yy = static_cast<int>(syp + yo);
                if (xx >= 0 && xx < w && yy >= 0 && yy < h)
                  dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{240, 230, 200, false};
              }
            // Red cross on each sail.
            drawSeg(dst, w, h, mx, syp - SS * 0.15F, mx, syp, std::max(1.0F, mn * 0.004F), ya, Rgb{200, 30, 30, false});
            drawSeg(dst, w, h, mx - sw, syp - SS * 0.075F, mx + sw, syp - SS * 0.075F, std::max(1.0F, mn * 0.004F), ya, Rgb{200, 30, 30, false});
          }
        }
      }
      // "1492".
      const std::string year = "1492";
      const int sc = std::max(3, static_cast<int>(mn / 25.0F));
      const float lineW = static_cast<float>(year.size()) * 6 * sc;
      for (int ci = 0; ci < static_cast<int>(year.size()); ++ci) {
        const auto g = glyph5x7(year[ci]);
        for (int fy = 0; fy < 7; ++fy)
          for (int fx = 0; fx < 5; ++fx)
            if (g[fy][fx] == '1')
              plotDot(dst, w, h, w * 0.5F - lineW * 0.5F + (ci * 6 + fx) * sc, h * 0.90F + fy * sc,
                      std::max(1.0F, static_cast<float>(sc) * 0.5F), ya, Rgb{200, 180, 140, false});
      }
    });
}

void effectGalileoTelescope(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      // Night sky.
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
          const float l = s.transparent ? 30.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
          const float sf = static_cast<float>(y) / h;
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(8 + 12 * (1 - sf) + l * 0.04F), u8(10 + 14 * (1 - sf) + l * 0.04F), u8(24 + 30 * (1 - sf) + l * 0.10F), false};
        }
      // Stars.
      for (int i = 0; i < 80; ++i) {
        const int sx = static_cast<int>(hash(i) * w);
        const int sy = static_cast<int>(hash(i * 3) * h * 0.7F);
        const float tw = 0.5F + 0.5F * std::sin(t * 10.0F + i);
        dst[static_cast<std::size_t>(sy) * w + sx] = Rgb{u8(160 * tw), u8(160 * tw), u8(200 * tw), false};
      }
      // Moon (data-textured).
      drawDataDisk(dst, w, h, src, w * 0.75F, h * 0.30F, mn * 0.15F, ya, 0.7F, t * 0.3F, Rgb{220, 210, 180, false});
      // Galileo's figure (bottom-left).
      const float fx = w * 0.20F, fy = h * 0.75F;
      const float S = mn * 0.15F;
      const Rgb robe{40, 30, 50, false};
      const Rgb skin{210, 180, 140, false};
      // Robe.
      for (int yo = 0; yo <= static_cast<int>(S * 1.2F); ++yo) {
        const float yf = yo / (S * 1.2F);
        const int half = static_cast<int>(S * (0.20F + 0.15F * yf));
        for (int xo = -half; xo <= half; ++xo) {
          const int xx = static_cast<int>(fx + xo), yy = static_cast<int>(fy + yo);
          if (xx >= 0 && xx < w && yy >= 0 && yy < h)
            dst[static_cast<std::size_t>(yy) * w + xx] = robe;
        }
      }
      // Head + beard.
      plotDot(dst, w, h, fx, fy - S * 0.10F, S * 0.13F, ya, skin);
      for (int yo = 0; yo <= static_cast<int>(S * 0.15F); ++yo) {
        const int half = static_cast<int>(S * (0.10F - yo / (S * 0.15F) * 0.06F));
        for (int xo = -half; xo <= half; ++xo) {
          const int xx = static_cast<int>(fx + xo), yy = static_cast<int>(fy - S * 0.02F + yo);
          if (xx >= 0 && xx < w && yy >= 0 && yy < h)
            dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{220, 220, 220, false};
        }
      }
      // Telescope: from figure's eye toward moon.
      const float tx0 = fx + S * 0.10F, ty0 = fy - S * 0.08F;
      const float tx1 = w * 0.55F, ty1 = h * 0.45F;
      drawSeg(dst, w, h, tx0, ty0, tx1, ty1, std::max(1.0F, mn * 0.012F), ya, Rgb{180, 140, 60, false});
      // Telescope tip (slightly larger).
      plotDot(dst, w, h, tx1, ty1, mn * 0.015F, ya, Rgb{220, 180, 80, false});
      // "E PUR SI MUOVE" — types out.
      const std::string text = "E PUR SI MUOVE";
      const int sc = std::max(2, static_cast<int>(mn / 50.0F));
      const float lineW = static_cast<float>(text.size()) * 6 * sc;
      const int nReveal = std::min(static_cast<int>(text.size()), static_cast<int>(std::clamp(t * 1.5F - 0.3F, 0.0F, 1.0F) * text.size()));
      for (int ci = 0; ci < nReveal; ++ci) {
        const char ch = text[ci];
        if (ch == ' ') continue;
        const auto g = glyph5x7(ch);
        for (int fy0 = 0; fy0 < 7; ++fy0)
          for (int fx0 = 0; fx0 < 5; ++fx0)
            if (g[fy0][fx0] == '1')
              plotDot(dst, w, h, (w - lineW) * 0.5F + (ci * 6 + fx0) * sc, h * 0.92F + fy0 * sc,
                      std::max(1.0F, static_cast<float>(sc) * 0.5F), ya, Rgb{220, 210, 180, false});
      }
    });
}

void effectEiffel(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
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
              Rgb{u8(220 + l * 0.05F - 30 * sf), u8(160 + l * 0.05F - 60 * sf), u8(140 - 60 * sf + l * 0.06F), false};
        }
      const float groundY = h * 0.90F;
      for (int x = 0; x < w; ++x)
        dst[static_cast<std::size_t>(groundY) * w + x] = Rgb{40, 30, 30, false};
      const float cx = w * 0.5F;
      const float towerTop = groundY - mn * 0.70F;
      const float reveal = std::clamp(t * 1.3F, 0.0F, 1.0F);
      const float topReveal = groundY - (groundY - towerTop) * reveal;
      const Rgb iron{60, 40, 30, false};
      // Tower outline: legs curve in toward the top.
      auto width_at = [&](float y) {
        const float yf = (groundY - y) / (groundY - towerTop);
        return mn * (0.18F * (1.0F - 0.85F * yf));
      };
      for (int yy = static_cast<int>(topReveal); yy <= static_cast<int>(groundY); ++yy) {
        const float ww = width_at(yy);
        // Left leg
        for (int yo = 0; yo < 2; ++yo) {
          const int xL = static_cast<int>(cx - ww), xR = static_cast<int>(cx + ww);
          if (xL >= 0 && xL < w && yy + yo < h) dst[static_cast<std::size_t>(yy + yo) * w + xL] = iron;
          if (xR >= 0 && xR < w && yy + yo < h) dst[static_cast<std::size_t>(yy + yo) * w + xR] = iron;
        }
      }
      // Cross-hatching every N rows.
      const int spacing = static_cast<int>(mn * 0.04F);
      for (int yy = static_cast<int>(topReveal); yy <= static_cast<int>(groundY); yy += spacing) {
        const float ww = width_at(yy);
        const float ww2 = width_at(yy + spacing);
        drawSeg(dst, w, h, cx - ww, yy, cx + ww2, yy + spacing, std::max(1.0F, mn * 0.002F), ya, iron);
        drawSeg(dst, w, h, cx + ww, yy, cx - ww2, yy + spacing, std::max(1.0F, mn * 0.002F), ya, iron);
      }
      // Tower platforms (horizontal bars at characteristic heights).
      for (float frac : {0.20F, 0.50F, 0.78F}) {
        const float yy = groundY - (groundY - towerTop) * frac;
        if (yy > topReveal) {
          const float ww = width_at(yy);
          drawSeg(dst, w, h, cx - ww, yy, cx + ww, yy, std::max(1.0F, mn * 0.004F), ya, iron);
        }
      }
      // Sun (data-textured).
      drawDataDisk(dst, w, h, src, w * 0.78F, h * 0.30F, mn * 0.09F, ya, 0.5F, t * 0.3F, Rgb{255, 200, 130, false});
    });
}

void effectStephenson(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
          const float l = s.transparent ? 60.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
          const float sf = static_cast<float>(y) / h;
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(180 - 30 * sf + l * 0.08F), u8(170 - 30 * sf + l * 0.08F), u8(160 + 20 * sf + l * 0.06F), false};
        }
      const float trackY = h * 0.78F;
      // Tracks.
      drawSeg(dst, w, h, 0, trackY, w, trackY, std::max(1.0F, mn * 0.004F), ya, Rgb{60, 50, 40, false});
      drawSeg(dst, w, h, 0, trackY + mn * 0.015F, w, trackY + mn * 0.015F, std::max(1.0F, mn * 0.004F), ya, Rgb{60, 50, 40, false});
      // Sleepers.
      for (int x = 0; x < w; x += static_cast<int>(mn * 0.04F))
        drawSeg(dst, w, h, x, trackY - mn * 0.005F, x, trackY + mn * 0.020F, std::max(1.0F, mn * 0.008F), ya, Rgb{80, 50, 30, false});
      const float ex = -mn * 0.3F + t * (w + mn * 0.5F);
      const float by = trackY - mn * 0.04F;
      const Rgb body{180, 30, 30, false};
      const Rgb black{20, 20, 20, false};
      // Boiler (horizontal cylinder).
      for (int yo = -static_cast<int>(mn * 0.04F); yo <= 0; ++yo)
        for (int xo = -static_cast<int>(mn * 0.10F); xo <= static_cast<int>(mn * 0.06F); ++xo) {
          const int xx = static_cast<int>(ex + xo), yy = static_cast<int>(by + yo);
          if (xx >= 0 && xx < w && yy >= 0 && yy < h)
            dst[static_cast<std::size_t>(yy) * w + xx] = body;
        }
      // Tall chimney.
      for (int yo = -static_cast<int>(mn * 0.12F); yo < -static_cast<int>(mn * 0.04F); ++yo)
        for (int xo = -static_cast<int>(mn * 0.018F); xo <= static_cast<int>(mn * 0.018F); ++xo) {
          const int xx = static_cast<int>(ex - mn * 0.06F + xo), yy = static_cast<int>(by + yo);
          if (xx >= 0 && xx < w && yy >= 0 && yy < h)
            dst[static_cast<std::size_t>(yy) * w + xx] = black;
        }
      // Wheels.
      for (int wh = 0; wh < 3; ++wh) {
        const float wx = ex - mn * 0.08F + wh * mn * 0.08F;
        plotDot(dst, w, h, wx, by + mn * 0.03F, mn * 0.025F, ya, black);
        plotDot(dst, w, h, wx, by + mn * 0.03F, mn * 0.010F, ya, Rgb{160, 30, 30, false});
      }
      // Steam — data-textured puffs.
      for (int i = 0; i < 14; ++i) {
        const float pt = std::fmod(t * 1.5F + hash(i) * 1.0F, 1.0F);
        const float sx = ex - mn * 0.06F + pt * mn * 0.40F + (hash(i * 3) - 0.5F) * mn * 0.04F;
        const float syp = by - mn * 0.12F - pt * mn * 0.20F;
        drawDataDisk(dst, w, h, src, sx, syp, mn * 0.025F * (1.0F - pt), ya, 0.4F, 0.0F, Rgb{220, 220, 220, false});
      }
    });
}

void effectWrightFlyer(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
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
              Rgb{u8(200 - 60 * sf + l * 0.10F), u8(220 - 50 * sf + l * 0.10F), u8(240 - 30 * sf + l * 0.10F), false};
        }
      const float groundY = h * 0.78F;
      // Dunes.
      for (int x = 0; x < w; ++x) {
        const int dy = static_cast<int>(groundY + std::sin(x * 0.05F) * mn * 0.02F);
        for (int yy = dy; yy < h; ++yy)
          if (yy >= 0 && yy < h)
            dst[static_cast<std::size_t>(yy) * w + x] = Rgb{200, 170, 120, false};
      }
      // Plane: starts on ground, lifts up.
      const float liftT = std::clamp((t - 0.30F) / 0.40F, 0.0F, 1.0F);
      const float plyOff = -liftT * mn * 0.25F;
      const float plx = w * 0.30F + t * w * 0.30F;
      const float ply = groundY - mn * 0.06F + plyOff;
      const Rgb fabric{220, 200, 160, false};
      const Rgb strut{60, 40, 20, false};
      // Upper wing.
      drawSeg(dst, w, h, plx - mn * 0.15F, ply - mn * 0.04F, plx + mn * 0.15F, ply - mn * 0.04F, std::max(1.0F, mn * 0.008F), ya, fabric);
      // Lower wing.
      drawSeg(dst, w, h, plx - mn * 0.15F, ply + mn * 0.02F, plx + mn * 0.15F, ply + mn * 0.02F, std::max(1.0F, mn * 0.008F), ya, fabric);
      // Struts between wings.
      for (int s = -2; s <= 2; ++s) {
        const float sx = plx + s * mn * 0.05F;
        drawSeg(dst, w, h, sx, ply - mn * 0.04F, sx, ply + mn * 0.02F, std::max(1.0F, mn * 0.002F), ya, strut);
      }
      // Pilot (silhouette on lower wing).
      plotDot(dst, w, h, plx, ply + mn * 0.005F, std::max(1.0F, mn * 0.008F), ya, Rgb{20, 20, 30, false});
      // Tail.
      drawSeg(dst, w, h, plx + mn * 0.15F, ply, plx + mn * 0.25F, ply, std::max(1.0F, mn * 0.005F), ya, strut);
      drawSeg(dst, w, h, plx + mn * 0.25F, ply - mn * 0.02F, plx + mn * 0.25F, ply + mn * 0.02F, std::max(1.0F, mn * 0.005F), ya, fabric);
      // Front propeller.
      const float prop = t * 30.0F;
      const float pcx = plx - mn * 0.17F;
      drawSeg(dst, w, h, pcx, ply - mn * 0.015F * std::cos(prop), pcx, ply + mn * 0.015F * std::cos(prop),
              std::max(1.0F, mn * 0.003F), ya, Rgb{150, 150, 150, false});
      // "1903"
      const std::string year = "1903";
      const int sc = std::max(3, static_cast<int>(mn / 25.0F));
      const float lineW = static_cast<float>(year.size()) * 6 * sc;
      for (int ci = 0; ci < static_cast<int>(year.size()); ++ci) {
        const auto g = glyph5x7(year[ci]);
        for (int fy = 0; fy < 7; ++fy)
          for (int fx = 0; fx < 5; ++fx)
            if (g[fy][fx] == '1')
              plotDot(dst, w, h, w * 0.5F - lineW * 0.5F + (ci * 6 + fx) * sc, h * 0.10F + fy * sc,
                      std::max(1.0F, static_cast<float>(sc) * 0.5F), ya, Rgb{40, 30, 20, false});
      }
    });
}

void effectSputnik(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 5200,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
          const float l = s.transparent ? 30.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(8 + l * 0.05F), u8(10 + l * 0.05F), u8(22 + l * 0.10F), false};
        }
      for (int i = 0; i < 100; ++i) {
        const int sx = static_cast<int>(hash(i) * w);
        const int sy = static_cast<int>(hash(i * 3) * h);
        const float tw = 0.5F + 0.5F * std::sin(t * 10.0F + i);
        dst[static_cast<std::size_t>(sy) * w + sx] = Rgb{u8(180 * tw), u8(180 * tw), u8(220 * tw), false};
      }
      // Earth on the right edge (data-textured).
      drawDataDisk(dst, w, h, src, w * 1.05F, h * 0.5F, mn * 0.45F, ya, 0.85F, t * 0.2F, Rgb{120, 180, 200, false});
      // Sputnik orbits in an ellipse.
      const float a = t * 6.2832F;
      const float ox = w * 0.6F + std::cos(a) * w * 0.45F;
      const float oy = h * 0.5F + std::sin(a) * h * 0.30F / ya;
      const float SS = mn * 0.04F;
      drawDataDisk(dst, w, h, src, ox, oy, SS, ya, 0.85F, t * 4.0F, Rgb{200, 200, 220, false});
      // Four antennas.
      const float spinA = t * 8.0F;
      for (int k = 0; k < 4; ++k) {
        const float ka = k / 4.0F * 6.2832F + spinA;
        drawSeg(dst, w, h, ox, oy, ox + std::cos(ka) * SS * 2.5F, oy + std::sin(ka) * SS * 2.5F,
                std::max(1.0F, mn * 0.003F), ya, Rgb{200, 200, 220, false});
      }
      // Pulse "BEEP BEEP".
      const float beep = 0.5F + 0.5F * std::sin(t * 12.0F);
      const std::string b = "BEEP";
      const int sc = std::max(2, static_cast<int>(mn / 35.0F));
      for (int ci = 0; ci < static_cast<int>(b.size()); ++ci) {
        const auto g = glyph5x7(b[ci]);
        for (int fy = 0; fy < 7; ++fy)
          for (int fx = 0; fx < 5; ++fx)
            if (g[fy][fx] == '1')
              plotDot(dst, w, h, w * 0.1F + (ci * 6 + fx) * sc, h * 0.85F + fy * sc,
                      std::max(1.0F, static_cast<float>(sc) * 0.5F * beep), ya, Rgb{u8(255 * beep), u8(80 * beep), u8(80 * beep), false});
      }
    });
}

void effectApollo11(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      // Black space.
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          dst[static_cast<std::size_t>(y) * w + x] = Rgb{4, 4, 10, false};
      // Stars.
      for (int i = 0; i < 80; ++i) {
        const int sx = static_cast<int>(hash(i) * w);
        const int sy = static_cast<int>(hash(i * 3) * h * 0.6F);
        const float tw = 0.5F + 0.5F * std::sin(t * 8.0F + i);
        dst[static_cast<std::size_t>(sy) * w + sx] = Rgb{u8(180 * tw), u8(180 * tw), u8(200 * tw), false};
      }
      // Moon surface — sample data into the lunar regolith (the surface that's
      // also a giant data-filled flat plane).
      const float groundY = h * 0.65F;
      for (int y = static_cast<int>(groundY); y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb d = sample(src, w, h, x, y);
          const float dr = d.transparent ? 140.0F : d.r;
          const float dg = d.transparent ? 140.0F : d.g;
          const float db = d.transparent ? 140.0F : d.b;
          dst[static_cast<std::size_t>(y) * w + x] = Rgb{u8(120 + dr * 0.20F), u8(120 + dg * 0.20F), u8(115 + db * 0.18F), false};
        }
      // Earthrise at the horizon (data-textured).
      drawDataDisk(dst, w, h, src, w * 0.18F, h * 0.32F, mn * 0.10F, ya, 0.85F, t * 0.3F, Rgb{60, 100, 180, false});
      // Lunar module silhouette.
      const float lcx = w * 0.65F, lcy = groundY - mn * 0.06F;
      const Rgb metal{200, 200, 210, false};
      const Rgb gold{220, 180, 80, false};
      // Body — gold descent stage on top of legs.
      for (int yo = 0; yo <= static_cast<int>(mn * 0.04F); ++yo)
        for (int xo = -static_cast<int>(mn * 0.05F); xo <= static_cast<int>(mn * 0.05F); ++xo) {
          const int xx = static_cast<int>(lcx + xo), yy = static_cast<int>(lcy + yo);
          if (xx >= 0 && xx < w && yy >= 0 && yy < h)
            dst[static_cast<std::size_t>(yy) * w + xx] = gold;
        }
      // Ascent stage (pyramid-ish).
      for (int yo = -static_cast<int>(mn * 0.05F); yo < 0; ++yo) {
        const float yf = -yo / (mn * 0.05F);
        const int half = static_cast<int>(mn * (0.04F + 0.01F * yf));
        for (int xo = -half; xo <= half; ++xo) {
          const int xx = static_cast<int>(lcx + xo), yy = static_cast<int>(lcy + yo);
          if (xx >= 0 && xx < w && yy >= 0 && yy < h)
            dst[static_cast<std::size_t>(yy) * w + xx] = metal;
        }
      }
      // Four landing legs.
      for (int leg = -1; leg <= 1; leg += 2) {
        drawSeg(dst, w, h, lcx + leg * mn * 0.04F, lcy + mn * 0.04F,
                lcx + leg * mn * 0.08F, lcy + mn * 0.06F, std::max(1.0F, mn * 0.003F), ya, metal);
      }
      // Astronaut silhouette + flag.
      const float ax = w * 0.40F;
      const float ay = lcy + mn * 0.03F;
      const Rgb suit{220, 220, 220, false};
      plotDot(dst, w, h, ax, ay - mn * 0.06F, mn * 0.014F, ya, suit);  // helmet
      plotDot(dst, w, h, ax, ay - mn * 0.06F, std::max(1.0F, mn * 0.006F), ya, Rgb{20, 20, 40, false});  // visor
      drawSeg(dst, w, h, ax, ay - mn * 0.05F, ax, ay + mn * 0.01F, std::max(1.0F, mn * 0.012F), ya, suit);  // body
      drawSeg(dst, w, h, ax, ay + mn * 0.01F, ax - mn * 0.015F, ay + mn * 0.05F, std::max(1.0F, mn * 0.008F), ya, suit);  // leg
      drawSeg(dst, w, h, ax, ay + mn * 0.01F, ax + mn * 0.015F, ay + mn * 0.05F, std::max(1.0F, mn * 0.008F), ya, suit);  // leg
      // Flag pole + flag (red/white/blue rectangles).
      const float fpx = ax + mn * 0.04F;
      drawSeg(dst, w, h, fpx, ay + mn * 0.05F, fpx, ay - mn * 0.10F, std::max(1.0F, mn * 0.003F), ya, suit);
      for (int yo = -static_cast<int>(mn * 0.10F); yo < -static_cast<int>(mn * 0.06F); ++yo)
        for (int xo = 0; xo <= static_cast<int>(mn * 0.045F); ++xo) {
          const int xx = static_cast<int>(fpx + xo), yy = static_cast<int>(ay + yo);
          if (xx >= 0 && xx < w && yy >= 0 && yy < h) {
            const bool stripe = ((yo + static_cast<int>(mn * 0.10F)) / 2) % 2 == 0;
            dst[static_cast<std::size_t>(yy) * w + xx] = stripe ? Rgb{220, 30, 30, false} : Rgb{240, 240, 240, false};
          }
        }
      // "ONE SMALL STEP".
      const std::string line = "ONE SMALL STEP";
      const int sc = std::max(2, static_cast<int>(mn / 45.0F));
      const float lineW = static_cast<float>(line.size()) * 6 * sc;
      const int nReveal = std::min(static_cast<int>(line.size()),
                                   static_cast<int>(std::clamp(t * 1.5F - 0.4F, 0.0F, 1.0F) * line.size()));
      for (int ci = 0; ci < nReveal; ++ci) {
        const char ch = line[ci];
        if (ch == ' ') continue;
        const auto g = glyph5x7(ch);
        for (int fy = 0; fy < 7; ++fy)
          for (int fx = 0; fx < 5; ++fx)
            if (g[fy][fx] == '1')
              plotDot(dst, w, h, w * 0.5F - lineW * 0.5F + (ci * 6 + fx) * sc, h * 0.10F + fy * sc,
                      std::max(1.0F, static_cast<float>(sc) * 0.5F), ya, Rgb{240, 240, 240, false});
      }
    });
}

void effectBerlinWall(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
          const float l = s.transparent ? 50.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
          const float sf = static_cast<float>(y) / h;
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(40 + 60 * (1 - sf) + l * 0.10F), u8(60 + 70 * (1 - sf) + l * 0.10F), u8(100 + 50 * (1 - sf) + l * 0.10F), false};
        }
      const float groundY = h * 0.88F;
      // Wall: a horizontal slab. Surface is data-tinted to "carry the graffiti".
      const float wallTop = h * 0.30F;
      for (int yy = static_cast<int>(wallTop); yy <= static_cast<int>(groundY); ++yy)
        for (int xx = 0; xx < w; ++xx) {
          const Rgb d = sample(src, w, h, xx, yy);
          const float dr = d.transparent ? 100.0F : d.r;
          const float dg = d.transparent ? 100.0F : d.g;
          const float db = d.transparent ? 100.0F : d.b;
          // Knock chunks out of the wall over time.
          const float chunkProb = hash(static_cast<int>(xx / 6) * 13 + static_cast<int>(yy / 6) * 7);
          if (t > 0.3F && chunkProb < std::clamp((t - 0.3F) * 1.5F, 0.0F, 0.5F)) {
            dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{u8(40 + 80 * (1 - static_cast<float>(yy) / h) + dr * 0.1F),
                                                              u8(60 + 90 * (1 - static_cast<float>(yy) / h) + dg * 0.1F),
                                                              u8(100 + 60 * (1 - static_cast<float>(yy) / h) + db * 0.1F), false};
            continue;
          }
          dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{u8(180 + dr * 0.20F), u8(170 + dg * 0.20F), u8(160 + db * 0.20F), false};
        }
      // Top horizontal pipe (iconic).
      for (int xx = 0; xx < w; ++xx)
        for (int yo = 0; yo < static_cast<int>(mn * 0.020F); ++yo) {
          const int yy = static_cast<int>(wallTop) + yo;
          if (yy >= 0 && yy < h)
            dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{200, 200, 210, false};
        }
      // "1989" sprayed in red.
      const std::string year = "1989";
      const int sc = std::max(3, static_cast<int>(mn / 18.0F));
      const float lineW = static_cast<float>(year.size()) * 6 * sc;
      for (int ci = 0; ci < static_cast<int>(year.size()); ++ci) {
        const auto g = glyph5x7(year[ci]);
        for (int fy = 0; fy < 7; ++fy)
          for (int fx = 0; fx < 5; ++fx)
            if (g[fy][fx] == '1')
              plotDot(dst, w, h, w * 0.5F - lineW * 0.5F + (ci * 6 + fx) * sc, h * 0.45F + fy * sc,
                      std::max(1.0F, static_cast<float>(sc) * 0.5F), ya, Rgb{200, 30, 30, false});
      }
      // Sledgehammers from the right.
      for (int i = 0; i < 3; ++i) {
        const float hammerCycle = std::fmod(t * 3.0F + i * 0.4F, 1.0F);
        const float swing = std::sin(hammerCycle * 3.14159F) * 0.6F;
        const float hx = w * 0.85F + std::cos(swing) * mn * 0.10F;
        const float hy = h * 0.50F + i * mn * 0.08F + std::sin(swing) * mn * 0.06F;
        drawSeg(dst, w, h, w * 0.95F, h * 0.50F + i * mn * 0.08F, hx, hy, std::max(1.0F, mn * 0.005F), ya, Rgb{80, 50, 30, false});
        plotDot(dst, w, h, hx, hy, mn * 0.025F, ya, Rgb{120, 120, 130, false});
      }
    });
}

void effectCrown(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  FootSprite f = loadPythonFootSprite();
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      // Velvet throne-room background.
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb d = sample(src, w, h, x, y);
          const float dl = d.transparent ? 40.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(80 + dl * 0.30F), u8(20 + dl * 0.15F), u8(60 + dl * 0.20F), false};
        }
      // Throne.
      const float thrX = w * 0.5F, thrY = h * 0.65F;
      const Rgb gold{200, 160, 60, false};
      drawSeg(dst, w, h, thrX - mn * 0.12F, thrY, thrX - mn * 0.12F, h * 0.92F,
              std::max(1.0F, mn * 0.020F), ya, gold);
      drawSeg(dst, w, h, thrX + mn * 0.12F, thrY, thrX + mn * 0.12F, h * 0.92F,
              std::max(1.0F, mn * 0.020F), ya, gold);
      drawSeg(dst, w, h, thrX - mn * 0.12F, thrY, thrX + mn * 0.12F, thrY,
              std::max(1.0F, mn * 0.020F), ya, gold);
      // King silhouette.
      const Rgb royal{30, 20, 50, false};
      drawSeg(dst, w, h, thrX, h * 0.55F, thrX, h * 0.78F, mn * 0.035F, ya, royal);
      plotDot(dst, w, h, thrX, h * 0.50F, mn * 0.035F, ya, Rgb{220, 180, 140, false});
      // Foot perched on king's head.
      const float bobble = std::sin(t * 3.0F) * mn * 0.005F;
      drawFootSprite(dst, w, h, ya, f.img.get(), f.fw, f.fh, f.needChromaKey,
                     thrX + bobble, h * 0.40F, mn * 0.18F, 0.0F);
      // Bowing subjects.
      for (int i = 0; i < 5; ++i) {
        const float sx = w * (0.15F + 0.15F * i);
        drawSeg(dst, w, h, sx, h * 0.88F, sx + mn * 0.02F, h * 0.97F, mn * 0.020F, ya, Rgb{40, 30, 60, false});
      }
    });
}

void effectLibertyTorch(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  FootSprite f = loadPythonFootSprite();
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb d = sample(src, w, h, x, y);
          const float dl = d.transparent ? 50.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          const float sf = static_cast<float>(y) / h;
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(140 + dl * 0.20F - 50 * sf), u8(100 + dl * 0.15F - 30 * sf),
                  u8(70 + dl * 0.10F + 30 * sf), false};
        }
      // Water at the bottom.
      for (int yy = static_cast<int>(h * 0.85F); yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx)
          dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{50, 80, 110, false};
      // Liberty silhouette — gown + body, arm raised.
      const float cx = w * 0.45F;
      const Rgb green{50, 130, 110, false};
      // Body.
      for (int yo = 0; yo <= static_cast<int>(h * 0.30F); ++yo) {
        const float yf = yo / (h * 0.30F);
        const int half = static_cast<int>(mn * (0.04F + 0.06F * yf));
        for (int xo = -half; xo <= half; ++xo) {
          const int xx = static_cast<int>(cx + xo), yy = static_cast<int>(h * 0.55F + yo);
          if (xx >= 0 && xx < w && yy >= 0 && yy < h)
            dst[static_cast<std::size_t>(yy) * w + xx] = green;
        }
      }
      // Head with spikes.
      plotDot(dst, w, h, cx, h * 0.50F, mn * 0.030F, ya, green);
      for (int k = 0; k < 7; ++k) {
        const float a = (k - 3) * 0.35F - 1.5708F;
        drawSeg(dst, w, h, cx, h * 0.50F, cx + std::cos(a) * mn * 0.05F,
                h * 0.50F + std::sin(a) * mn * 0.05F, std::max(1.0F, mn * 0.005F), ya, green);
      }
      // Raised arm + foot.
      drawSeg(dst, w, h, cx + mn * 0.02F, h * 0.55F, cx + mn * 0.10F, h * 0.30F,
              mn * 0.020F, ya, green);
      const float footCx = cx + mn * 0.10F;
      const float footCy = h * 0.22F;
      drawFootSprite(dst, w, h, ya, f.img.get(), f.fw, f.fh, f.needChromaKey,
                     footCx, footCy, mn * 0.13F, -0.2F);
      // Flames at the top.
      for (int i = 0; i < 30; ++i) {
        const float fa = std::fmod(t * 2.0F + hash(i), 1.0F);
        const float fx = footCx + (hash(i * 3) - 0.5F) * mn * 0.06F;
        const float fy = footCy - mn * 0.06F - fa * mn * 0.08F;
        plotDot(dst, w, h, fx, fy, mn * 0.010F * (1 - fa), ya,
                Rgb{u8(255), u8(180 * (1 - fa)), u8(40 * (1 - fa)), false});
      }
      // Tablet in left hand.
      drawSeg(dst, w, h, cx - mn * 0.10F, h * 0.70F, cx - mn * 0.12F, h * 0.78F,
              std::max(1.0F, mn * 0.014F), ya, Rgb{220, 200, 160, false});
    });
}

void effectMoonFlag(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  FootSprite f = loadPythonFootSprite();
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      // Black space.
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          dst[static_cast<std::size_t>(y) * w + x] = Rgb{4, 4, 10, false};
      for (int i = 0; i < 60; ++i)
        dst[static_cast<std::size_t>(hash(i) * h * 0.6F) * w + static_cast<int>(hash(i * 3) * w)] =
            Rgb{220, 220, 240, false};
      // Lunar surface — grey, data-tinted.
      const float groundY = h * 0.70F;
      for (int yy = static_cast<int>(groundY); yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx) {
          const Rgb d = sample(src, w, h, xx, yy);
          const float dl = d.transparent ? 100.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          dst[static_cast<std::size_t>(yy) * w + xx] =
              Rgb{u8(120 + dl * 0.20F), u8(120 + dl * 0.20F), u8(115 + dl * 0.15F), false};
        }
      // Earth in the sky.
      drawDataDisk(dst, w, h, src, w * 0.20F, h * 0.25F, mn * 0.08F, ya, 0.85F, t * 0.2F,
                   Rgb{60, 120, 180, false});
      // Astronaut.
      const float ax = w * 0.55F;
      const Rgb suit{220, 220, 220, false};
      plotDot(dst, w, h, ax, groundY - mn * 0.10F, mn * 0.020F, ya, suit);  // helmet
      plotDot(dst, w, h, ax, groundY - mn * 0.10F, std::max(1.0F, mn * 0.008F), ya,
              Rgb{20, 20, 40, false});                                       // visor
      drawSeg(dst, w, h, ax, groundY - mn * 0.08F, ax, groundY, mn * 0.018F, ya, suit);
      drawSeg(dst, w, h, ax, groundY, ax - mn * 0.02F, groundY + mn * 0.05F,
              mn * 0.010F, ya, suit);
      drawSeg(dst, w, h, ax, groundY, ax + mn * 0.02F, groundY + mn * 0.05F,
              mn * 0.010F, ya, suit);
      // Foot — "flag", planted upright (sole-down).
      const float plant = std::clamp(t * 1.5F, 0.0F, 1.0F);
      const float footCx = ax + mn * 0.10F;
      const float footCy = groundY - mn * 0.10F + (1 - plant) * mn * 0.30F;
      drawFootSprite(dst, w, h, ya, f.img.get(), f.fw, f.fh, f.needChromaKey,
                     footCx, footCy, mn * 0.18F, 1.5708F);
      // Astronaut arm reaches toward the foot.
      drawSeg(dst, w, h, ax, groundY - mn * 0.06F, footCx - mn * 0.04F,
              footCy + mn * 0.04F, mn * 0.010F, ya, suit);
    });
}

void effectNewton(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  FootSprite f = loadPythonFootSprite();
  runFrames(renderer, w, h, 5600,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb d = sample(src, w, h, x, y);
          const float dl = d.transparent ? 70.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          const float sf = static_cast<float>(y) / h;
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(140 + dl * 0.20F - 30 * sf), u8(180 + dl * 0.20F - 30 * sf),
                  u8(160 + dl * 0.15F + 20 * sf), false};
        }
      // Tree.
      const float trunkX = w * 0.6F, groundY = h * 0.85F;
      drawSeg(dst, w, h, trunkX, groundY, trunkX, h * 0.35F, mn * 0.025F, ya,
              Rgb{80, 50, 20, false});
      // Canopy — green blobs.
      for (int i = 0; i < 30; ++i) {
        const float a = i / 30.0F * 6.2832F;
        plotDot(dst, w, h, trunkX + std::cos(a) * mn * 0.18F,
                h * 0.35F + std::sin(a) * mn * 0.18F / ya, mn * 0.030F, ya,
                Rgb{60, 130, 50, false});
      }
      // Newton — sitting under tree.
      const float nx = w * 0.35F, ny = groundY - mn * 0.10F;
      drawSeg(dst, w, h, nx, ny, nx, groundY, mn * 0.030F, ya, Rgb{40, 30, 20, false});
      plotDot(dst, w, h, nx, ny - mn * 0.025F, mn * 0.030F, ya, Rgb{220, 180, 140, false});
      // Periwig (curls).
      for (int k = -2; k <= 2; ++k)
        plotDot(dst, w, h, nx + k * mn * 0.018F, ny - mn * 0.05F, mn * 0.018F, ya,
                Rgb{200, 200, 200, false});
      // Foot drops from canopy onto Newton's head.
      const float p = std::clamp(t * 1.4F, 0.0F, 1.0F);
      const float footY = h * 0.45F + p * p * (ny - mn * 0.05F - h * 0.45F);
      drawFootSprite(dst, w, h, ya, f.img.get(), f.fw, f.fh, f.needChromaKey,
                     nx, footY, mn * 0.13F, 1.5708F);
      // Impact stars at touchdown.
      if (p > 0.97F) {
        for (int k = 0; k < 6; ++k) {
          const float a = k * 6.2832F / 6;
          plotDot(dst, w, h, nx + std::cos(a) * mn * 0.05F,
                  ny + std::sin(a) * mn * 0.05F / ya, std::max(1.0F, mn * 0.005F), ya,
                  Rgb{255, 240, 100, false});
        }
      }
    });
}

void effectOlympicTorch(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  FootSprite f = loadPythonFootSprite();
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb d = sample(src, w, h, x, y);
          const float dl = d.transparent ? 60.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          const float sf = static_cast<float>(y) / h;
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(120 + dl * 0.20F + 40 * sf), u8(120 + dl * 0.20F), u8(60), false};
        }
      // Track lines (athletics stadium).
      for (int k = 0; k < 6; ++k)
        for (int x = 0; x < w; ++x) {
          const int yy = static_cast<int>(h * (0.80F + k * 0.02F));
          if (yy >= 0 && yy < h)
            dst[static_cast<std::size_t>(yy) * w + x] = Rgb{220, 220, 220, false};
        }
      // Runner — silhouette running right.
      const float rx = -mn * 0.10F + t * (w + mn * 0.20F);
      const float ry = h * 0.65F;
      const Rgb body{30, 30, 50, false};
      // Body bobs.
      const float bob = std::sin(t * 16.0F) * mn * 0.005F;
      // Torso.
      drawSeg(dst, w, h, rx, ry + bob, rx, ry + mn * 0.08F + bob, mn * 0.025F, ya, body);
      plotDot(dst, w, h, rx, ry - mn * 0.015F + bob, mn * 0.020F, ya, body);
      // Legs running.
      const float legSwing = std::sin(t * 16.0F) * mn * 0.04F;
      drawSeg(dst, w, h, rx, ry + mn * 0.08F + bob,
              rx - legSwing, ry + mn * 0.16F + bob, mn * 0.012F, ya, body);
      drawSeg(dst, w, h, rx, ry + mn * 0.08F + bob,
              rx + legSwing, ry + mn * 0.16F + bob, mn * 0.012F, ya, body);
      // Raised arm + foot torch.
      drawSeg(dst, w, h, rx, ry + bob, rx + mn * 0.08F, ry - mn * 0.10F + bob,
              mn * 0.012F, ya, body);
      const float fcx = rx + mn * 0.08F;
      const float fcy = ry - mn * 0.16F + bob;
      drawFootSprite(dst, w, h, ya, f.img.get(), f.fw, f.fh, f.needChromaKey,
                     fcx, fcy, mn * 0.11F, -0.2F);
      // Flame.
      for (int i = 0; i < 25; ++i) {
        const float fa = std::fmod(t * 4.0F + hash(i), 1.0F);
        plotDot(dst, w, h, fcx + (hash(i * 3) - 0.5F) * mn * 0.04F, fcy - mn * 0.06F - fa * mn * 0.07F,
                mn * 0.010F * (1 - fa), ya,
                Rgb{u8(255), u8(160 * (1 - fa)), u8(30 * (1 - fa)), false});
      }
    });
}

}  // namespace ee_detail
}  // namespace Qdless
