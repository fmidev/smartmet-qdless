#include "QdlessExitEffectCommon.h"
#include "QdlessMarionette.h"

namespace Qdless
{
namespace ee_detail
{

void effectAnubis(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      // Desert sky.
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
          const float l = s.transparent ? 60.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
          const float sf = static_cast<float>(y) / h;
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(180 - 30 * sf + l * 0.10F), u8(120 - 40 * sf + l * 0.08F),
                  u8(80 + 10 * sf + l * 0.06F), false};
        }
      // Sand horizon.
      const float groundY = h * 0.85F;
      for (int yy = static_cast<int>(groundY); yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx)
          dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{200, 170, 120, false};
      // Data-textured pyramid behind.
      const float cx = w * 0.5F;
      const float pyrW = mn * 0.40F, pyrH = mn * 0.35F;
      for (int yo = 0; yo <= static_cast<int>(pyrH); ++yo) {
        const float yf = yo / pyrH;
        const int half = static_cast<int>(pyrW * 0.5F * (1.0F - yf));
        for (int xo = -half; xo <= half; ++xo) {
          const int xx = static_cast<int>(cx + xo);
          const int yy = static_cast<int>(groundY - yo);
          if (xx < 0 || xx >= w || yy < 0 || yy >= h) continue;
          const Rgb d = sample(src, w, h, xx, yy);
          const float dr = d.transparent ? 200.0F : d.r;
          const bool shade = xo > 0;
          dst[static_cast<std::size_t>(yy) * w + xx] =
              shade ? Rgb{u8(160 + dr * 0.20F), u8(130), u8(80), false}
                    : Rgb{u8(210 + dr * 0.15F), u8(180), u8(120), false};
        }
      }
      // Anubis silhouette in foreground.
      const float ax = w * 0.20F;
      const float ay = groundY - mn * 0.05F;
      const Rgb body{20, 20, 30, false};
      const float fh = mn * 0.30F;
      // Torso.
      drawSeg(dst, w, h, ax, ay - fh * 0.20F, ax, ay, fh * 0.12F, ya, body);
      // Legs.
      drawSeg(dst, w, h, ax, ay, ax - fh * 0.04F, ay + fh * 0.20F, fh * 0.08F, ya, body);
      drawSeg(dst, w, h, ax, ay, ax + fh * 0.04F, ay + fh * 0.20F, fh * 0.08F, ya, body);
      // Arms.
      drawSeg(dst, w, h, ax, ay - fh * 0.15F, ax + fh * 0.15F, ay - fh * 0.05F, fh * 0.06F, ya, body);
      drawSeg(dst, w, h, ax, ay - fh * 0.15F, ax - fh * 0.15F, ay - fh * 0.05F, fh * 0.06F, ya, body);
      // Jackal head + ears.
      plotDot(dst, w, h, ax, ay - fh * 0.30F, fh * 0.10F, ya, body);
      drawSeg(dst, w, h, ax - fh * 0.04F, ay - fh * 0.36F, ax - fh * 0.10F, ay - fh * 0.45F,
              std::max(1.0F, mn * 0.008F), ya, body);
      drawSeg(dst, w, h, ax + fh * 0.04F, ay - fh * 0.36F, ax + fh * 0.10F, ay - fh * 0.45F,
              std::max(1.0F, mn * 0.008F), ya, body);
      // Snout pointing right.
      drawSeg(dst, w, h, ax, ay - fh * 0.30F, ax + fh * 0.16F, ay - fh * 0.30F,
              std::max(1.0F, mn * 0.012F), ya, body);
      // Ankh in his right hand.
      const float ankhX = ax + fh * 0.18F, ankhY = ay - fh * 0.05F;
      plotDot(dst, w, h, ankhX, ankhY - fh * 0.05F, fh * 0.03F, ya, Rgb{240, 200, 80, false});
      drawSeg(dst, w, h, ankhX, ankhY - fh * 0.02F, ankhX, ankhY + fh * 0.10F,
              std::max(1.0F, mn * 0.004F), ya, Rgb{240, 200, 80, false});
      drawSeg(dst, w, h, ankhX - fh * 0.04F, ankhY + fh * 0.03F, ankhX + fh * 0.04F, ankhY + fh * 0.03F,
              std::max(1.0F, mn * 0.004F), ya, Rgb{240, 200, 80, false});
    });
}

void effectChineseDragon(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
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
              Rgb{u8(180 - 20 * sf + l * 0.10F), u8(140 - 30 * sf + l * 0.08F),
                  u8(80 + 10 * sf + l * 0.06F), false};
        }
      // Long sinuous body — 24 segments.
      for (int i = 0; i < 24; ++i) {
        const float f = i / 23.0F;
        const float bx = w * (0.95F - 0.90F * f);
        const float by = h * (0.5F + 0.25F * std::sin(f * 6.2832F * 1.5F - t * 4.0F));
        const float bsz = mn * (0.05F - 0.025F * f);
        drawDataDisk(dst, w, h, src, bx, by, bsz, ya, 0.7F, t * 0.5F + i, Rgb{200, 50, 30, false});
      }
      // Head + horns at the front (head at f=0 → bx near w*0.95).
      const float headX = w * 0.95F;
      const float headY = h * (0.5F + 0.25F * std::sin(-t * 4.0F));
      drawDataDisk(dst, w, h, src, headX, headY, mn * 0.07F, ya, 0.7F, 0.0F, Rgb{240, 80, 30, false});
      drawSeg(dst, w, h, headX, headY - mn * 0.06F, headX - mn * 0.04F, headY - mn * 0.12F,
              std::max(1.0F, mn * 0.005F), ya, Rgb{240, 200, 50, false});
      drawSeg(dst, w, h, headX, headY - mn * 0.06F, headX + mn * 0.04F, headY - mn * 0.12F,
              std::max(1.0F, mn * 0.005F), ya, Rgb{240, 200, 50, false});
      // Whiskers.
      drawSeg(dst, w, h, headX + mn * 0.05F, headY, headX + mn * 0.15F, headY - mn * 0.05F,
              std::max(1.0F, mn * 0.003F), ya, Rgb{240, 200, 50, false});
      drawSeg(dst, w, h, headX + mn * 0.05F, headY, headX + mn * 0.15F, headY + mn * 0.05F,
              std::max(1.0F, mn * 0.003F), ya, Rgb{240, 200, 50, false});
    });
}

void effectGaruda(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
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
              Rgb{u8(220 - 40 * sf + l * 0.10F), u8(180 - 50 * sf + l * 0.08F),
                  u8(100 + 30 * sf + l * 0.06F), false};
        }
      const float cx = w * 0.5F, cy = h * 0.55F;
      const float wing = mn * 0.30F;
      const float flap = std::sin(t * 4.0F) * 0.4F;
      // Wings — data-filled triangles.
      for (int side = -1; side <= 1; side += 2) {
        for (int yo = -static_cast<int>(wing * 0.30F); yo <= static_cast<int>(wing * 0.10F); ++yo) {
          const float yf = (yo + wing * 0.30F) / (wing * 0.40F);
          const int half = static_cast<int>(wing * (0.50F * yf));
          for (int xo = 0; xo <= half; ++xo) {
            const int xx = static_cast<int>(cx + side * (wing * 0.10F + xo));
            const int yy = static_cast<int>(cy + yo + side * flap * mn * 0.05F);
            if (xx < 0 || xx >= w || yy < 0 || yy >= h) continue;
            const Rgb d = sample(src, w, h, xx, yy);
            const float dr = d.transparent ? 180.0F : d.r;
            const float dg = d.transparent ? 120.0F : d.g;
            dst[static_cast<std::size_t>(yy) * w + xx] =
                Rgb{u8(200 + dr * 0.20F), u8(150 + dg * 0.18F), u8(60), false};
          }
        }
      }
      // Body.
      drawSeg(dst, w, h, cx, cy - mn * 0.05F, cx, cy + mn * 0.12F, mn * 0.04F, ya, Rgb{160, 80, 30, false});
      // Eagle head.
      plotDot(dst, w, h, cx, cy - mn * 0.10F, mn * 0.04F, ya, Rgb{240, 220, 200, false});
      // Beak.
      drawSeg(dst, w, h, cx + mn * 0.03F, cy - mn * 0.10F, cx + mn * 0.06F, cy - mn * 0.08F,
              std::max(1.0F, mn * 0.008F), ya, Rgb{240, 160, 40, false});
      // Legs/talons.
      drawSeg(dst, w, h, cx - mn * 0.02F, cy + mn * 0.12F, cx - mn * 0.04F, cy + mn * 0.18F,
              std::max(1.0F, mn * 0.005F), ya, Rgb{200, 140, 80, false});
      drawSeg(dst, w, h, cx + mn * 0.02F, cy + mn * 0.12F, cx + mn * 0.04F, cy + mn * 0.18F,
              std::max(1.0F, mn * 0.005F), ya, Rgb{200, 140, 80, false});
    });
}

void effectMjolnir(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      const bool flash = t > 0.55F && std::fmod(t * 4.0F, 1.0F) < 0.2F;
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
          const float l = s.transparent ? 50.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
          const float fb = flash ? 60.0F : 0.0F;
          const float sf = static_cast<float>(y) / h;
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(40 + 30 * sf + l * 0.08F + fb), u8(50 + 30 * sf + l * 0.08F + fb),
                  u8(70 + 30 * sf + l * 0.08F + fb), false};
        }
      // Hammer falls.
      const float groundY = h * 0.78F;
      const float fall = std::clamp(t / 0.55F, 0.0F, 1.0F);
      const float hcx = w * 0.5F;
      const float hcy = h * 0.10F + fall * (groundY - mn * 0.08F - h * 0.10F);
      // Handle.
      drawSeg(dst, w, h, hcx, hcy + mn * 0.05F, hcx, hcy + mn * 0.20F,
              std::max(1.0F, mn * 0.010F), ya, Rgb{100, 70, 40, false});
      // Hammer head (data-textured).
      for (int yo = -static_cast<int>(mn * 0.06F); yo <= static_cast<int>(mn * 0.06F); ++yo)
        for (int xo = -static_cast<int>(mn * 0.10F); xo <= static_cast<int>(mn * 0.10F); ++xo) {
          const int xx = static_cast<int>(hcx + xo);
          const int yy = static_cast<int>(hcy + yo);
          if (xx < 0 || xx >= w || yy < 0 || yy >= h) continue;
          const Rgb d = sample(src, w, h, xx, yy);
          const float dr = d.transparent ? 180.0F : d.r;
          dst[static_cast<std::size_t>(yy) * w + xx] =
              Rgb{u8(180 + dr * 0.20F), u8(180 + dr * 0.10F), u8(200), false};
        }
      // Lightning bolts on impact.
      if (t > 0.55F) {
        for (int b = 0; b < 8; ++b) {
          const float ang = (b / 8.0F) * 6.2832F + hash(b) * 0.5F;
          float bx = hcx;
          float by = groundY - mn * 0.08F;
          for (int s = 0; s < 5; ++s) {
            const float nx = bx + std::cos(ang) * mn * 0.04F + (hash(b * 7 + s) - 0.5F) * mn * 0.02F;
            const float ny = by + std::sin(ang) * mn * 0.04F + (hash(b * 11 + s) - 0.5F) * mn * 0.02F;
            drawSeg(dst, w, h, bx, by, nx, ny, std::max(1.0F, mn * 0.003F), ya,
                    Rgb{240, 240, 255, false});
            bx = nx; by = ny;
          }
        }
      }
    });
}

void effectPandora(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
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
          const float l = s.transparent ? 30.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(30 + l * 0.10F), u8(20 + l * 0.08F), u8(40 + l * 0.10F), false};
        }
      const float cx = w * 0.5F, cy = h * 0.65F;
      const float W = mn * 0.10F, H = mn * 0.07F;
      // Box body.
      for (int yo = 0; yo <= static_cast<int>(H); ++yo)
        for (int xo = -static_cast<int>(W); xo <= static_cast<int>(W); ++xo) {
          const int xx = static_cast<int>(cx + xo);
          const int yy = static_cast<int>(cy + yo);
          if (xx >= 0 && xx < w && yy >= 0 && yy < h)
            dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{120, 80, 40, false};
        }
      // Box lid opens upward.
      const float lidOpen = std::clamp(t * 2.0F, 0.0F, 1.0F);
      const float lidA = -lidOpen * 1.4F;
      const float lidX0 = cx - W;
      const float lidY0 = cy;
      const float lidX1 = cx - W + std::cos(lidA) * (W * 2);
      const float lidY1 = cy + std::sin(lidA) * (W * 2);
      drawSeg(dst, w, h, lidX0, lidY0, lidX1, lidY1, std::max(1.0F, mn * 0.012F), ya,
              Rgb{180, 130, 60, false});
      // Motes escape.
      for (int i = 0; i < 30; ++i) {
        const float spawn = i / 30.0F;
        const float age = std::clamp(t - 0.30F - spawn * 0.40F, 0.0F, 1.0F);
        if (age <= 0) continue;
        const float ang = (hash(i) - 0.5F) * 2.0F;
        const float mx = cx + std::cos(ang) * age * mn * 0.30F;
        const float my = cy - age * mn * 0.50F;
        const Rgb d = sample(src, w, h, static_cast<int>(mx), static_cast<int>(my));
        const float dr = d.transparent ? 220.0F : d.r;
        const float dg = d.transparent ? 180.0F : d.g;
        plotDot(dst, w, h, mx, my, mn * 0.012F * (1 - age * 0.5F), ya,
                Rgb{u8(240 + dr * 0.05F), u8(180 + dg * 0.20F), u8(80), false});
      }
    });
}

void effectPegasus(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
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
              Rgb{u8(180 - 50 * sf + l * 0.10F), u8(180 - 40 * sf + l * 0.10F),
                  u8(220 - 30 * sf + l * 0.10F), false};
        }
      const float cx = -mn * 0.15F + t * (w + mn * 0.30F);
      const float cy = h * 0.50F + std::sin(t * 4.0F) * mn * 0.05F;
      const Rgb body{240, 240, 240, false};
      const float bL = mn * 0.18F;
      // Body (oval).
      for (int yo = -static_cast<int>(bL * 0.20F); yo <= static_cast<int>(bL * 0.20F); ++yo)
        for (int xo = -static_cast<int>(bL * 0.50F); xo <= static_cast<int>(bL * 0.30F); ++xo) {
          const float nx = xo / (bL * 0.50F), nyN = yo / (bL * 0.20F);
          if (nx * nx + nyN * nyN > 1.0F) continue;
          const int xx = static_cast<int>(cx + xo), yy = static_cast<int>(cy + yo);
          if (xx >= 0 && xx < w && yy >= 0 && yy < h)
            dst[static_cast<std::size_t>(yy) * w + xx] = body;
        }
      // Wings spread above (data-textured).
      const float flap = std::sin(t * 6.0F) * 0.4F + 0.4F;
      for (int side = -1; side <= 1; side += 2) {
        const float wEnd = cy - flap * mn * 0.20F;
        for (int k = 0; k < 12; ++k) {
          const float kf = k / 11.0F;
          const float wx = cx - side * kf * mn * 0.15F;
          const float wy = cy - flap * mn * 0.20F * (1 - kf);
          drawSeg(dst, w, h, cx, cy - bL * 0.10F, wx, wy, std::max(1.0F, mn * 0.003F), ya, body);
        }
        (void)wEnd;
      }
      // Head + legs.
      drawSeg(dst, w, h, cx + bL * 0.35F, cy - bL * 0.10F, cx + bL * 0.45F, cy - bL * 0.25F,
              std::max(1.0F, mn * 0.012F), ya, body);
      plotDot(dst, w, h, cx + bL * 0.45F, cy - bL * 0.25F, mn * 0.020F, ya, body);
      for (int leg = -1; leg <= 1; leg += 2) {
        drawSeg(dst, w, h, cx + leg * bL * 0.20F, cy + bL * 0.15F,
                cx + leg * bL * 0.20F, cy + bL * 0.30F, std::max(1.0F, mn * 0.005F), ya, body);
      }
    });
}

void effectPhoenix(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb d = sample(src, w, h, x, y);
          const float dl = d.transparent ? 80.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(60 + dl * 0.20F), u8(30 + dl * 0.10F), u8(20 + dl * 0.05F), false};
        }
      const float cx = w * 0.5F, cy = h * 0.65F;
      // Flames rising.
      for (int i = 0; i < 100; ++i) {
        const float fx = cx + (hash(i) - 0.5F) * mn * 0.30F;
        const float fage = std::fmod(t * 1.5F + hash(i * 3), 1.0F);
        const float fy = cy + mn * 0.15F - fage * mn * 0.30F;
        const Rgb d = sample(src, w, h, static_cast<int>(fx), static_cast<int>(fy));
        const float dr = d.transparent ? 220.0F : d.r;
        plotDot(dst, w, h, fx, fy, mn * 0.012F * (1 - fage), ya,
                Rgb{u8(255), u8(180 * (1 - fage) + dr * 0.05F), u8(60 * (1 - fage)), false});
      }
      // Phoenix body emerges (rises).
      const float rise = std::clamp(t * 1.4F, 0.0F, 1.0F);
      const float by = cy - rise * mn * 0.30F;
      const Rgb feather{240, 140, 30, false};
      const float bL = mn * 0.15F;
      // Body.
      drawSeg(dst, w, h, cx, by - bL * 0.20F, cx, by + bL * 0.15F, bL * 0.15F, ya, feather);
      // Head + beak.
      plotDot(dst, w, h, cx, by - bL * 0.30F, bL * 0.10F, ya, feather);
      drawSeg(dst, w, h, cx + bL * 0.05F, by - bL * 0.30F, cx + bL * 0.15F, by - bL * 0.28F,
              std::max(1.0F, mn * 0.005F), ya, Rgb{240, 200, 50, false});
      // Wings spread (data-textured tips).
      const float spread = rise;
      for (int side = -1; side <= 1; side += 2) {
        for (int k = 0; k < 8; ++k) {
          const float kf = k / 7.0F;
          const float wx = cx - side * spread * mn * 0.30F * (0.5F + 0.5F * kf);
          const float wy = by - spread * mn * 0.10F * (1 - kf);
          drawSeg(dst, w, h, cx, by - bL * 0.10F, wx, wy, std::max(1.0F, mn * 0.004F), ya,
                  Rgb{u8(255), u8(180 - 60 * kf), u8(40), false});
        }
      }
      // Tail feathers below.
      for (int k = 0; k < 5; ++k) {
        const float kf = k / 4.0F;
        const float tx = cx + (kf - 0.5F) * bL * 0.4F;
        drawSeg(dst, w, h, cx, by + bL * 0.10F, tx, by + bL * 0.40F, std::max(1.0F, mn * 0.005F), ya,
                Rgb{u8(255), u8(120), u8(40), false});
      }
      (void)hash;
    });
}

void effectQuetzalcoatl(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
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
              Rgb{u8(120 + l * 0.10F - 60 * sf), u8(80 + l * 0.10F - 20 * sf),
                  u8(60 + l * 0.10F + 20 * sf), false};
        }
      const float cx = w * 0.5F, cy = h * 0.5F;
      // Coil + segments.
      for (int i = 0; i < 30; ++i) {
        const float f = i / 29.0F;
        const float ang = f * 6.2832F * 1.8F + t * 2.0F;
        const float r = mn * 0.30F * (1 - f * 0.6F);
        const float sx = cx + std::cos(ang) * r;
        const float syp = cy + std::sin(ang) * r / ya;
        const float sz = mn * (0.04F - 0.02F * f);
        drawDataDisk(dst, w, h, src, sx, syp, sz, ya, 0.7F, ang, Rgb{60, 160, 80, false});
        // Feather tuft on every 3rd segment.
        if (i % 3 == 0) {
          const float nx = std::cos(ang + 1.5708F);
          const float ny = std::sin(ang + 1.5708F);
          drawSeg(dst, w, h, sx, syp, sx + nx * sz * 1.8F, syp + ny * sz * 1.8F / ya,
                  std::max(1.0F, mn * 0.003F), ya, Rgb{220, 200, 80, false});
        }
      }
      // Head at f=0 (outermost).
      const float ang0 = t * 2.0F;
      const float hx = cx + std::cos(ang0) * mn * 0.30F;
      const float hy = cy + std::sin(ang0) * mn * 0.30F / ya;
      drawDataDisk(dst, w, h, src, hx, hy, mn * 0.05F, ya, 0.7F, 0.0F, Rgb{60, 200, 120, false});
    });
}

void effectRagnarok(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      // Doom sky.
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
          const float l = s.transparent ? 40.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
          const float sf = static_cast<float>(y) / h;
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(180 - 100 * sf + l * 0.10F), u8(60 + 30 * sf + l * 0.08F),
                  u8(40 + 30 * sf + l * 0.06F), false};
        }
      // Sun being swallowed.
      const float swallow = std::clamp(t * 1.3F, 0.0F, 1.0F);
      const float sR = mn * 0.10F * (1.0F - swallow * 0.7F);
      const float sx = w * (0.6F + swallow * 0.20F);
      const float syp = h * 0.40F;
      drawDataDisk(dst, w, h, src, sx, syp, sR, ya, 0.5F, t * 0.5F, Rgb{255, 200, 80, false});
      // Wolf head from left, jaws open.
      const float wcx = w * 0.25F + swallow * w * 0.30F;
      const float wcy = h * 0.40F;
      const Rgb fur{60, 50, 40, false};
      const float wL = mn * 0.20F;
      // Lower jaw.
      drawSeg(dst, w, h, wcx, wcy + wL * 0.10F, wcx + wL * 0.60F, wcy + wL * 0.30F + swallow * wL * 0.05F,
              std::max(1.0F, mn * 0.012F), ya, fur);
      // Upper jaw.
      drawSeg(dst, w, h, wcx, wcy - wL * 0.05F, wcx + wL * 0.60F, wcy - wL * 0.20F - swallow * wL * 0.05F,
              std::max(1.0F, mn * 0.012F), ya, fur);
      // Snout connection.
      drawSeg(dst, w, h, wcx + wL * 0.60F, wcy - wL * 0.20F, wcx + wL * 0.60F, wcy + wL * 0.30F,
              std::max(1.0F, mn * 0.006F), ya, fur);
      // Head body.
      plotDot(dst, w, h, wcx - wL * 0.10F, wcy, wL * 0.25F, ya, fur);
      // Ear.
      drawSeg(dst, w, h, wcx - wL * 0.10F, wcy - wL * 0.20F, wcx - wL * 0.15F, wcy - wL * 0.35F,
              std::max(1.0F, mn * 0.008F), ya, fur);
      // Eye.
      plotDot(dst, w, h, wcx, wcy - wL * 0.05F, std::max(1.0F, mn * 0.008F), ya,
              Rgb{255, 80, 30, false});
      // Teeth.
      for (int k = 0; k < 4; ++k) {
        const float kf = k / 3.0F;
        drawSeg(dst, w, h, wcx + wL * 0.10F + kf * wL * 0.40F, wcy - wL * 0.05F,
                wcx + wL * 0.10F + kf * wL * 0.40F, wcy,
                std::max(1.0F, mn * 0.003F), ya, Rgb{240, 240, 240, false});
      }
    });
}

void effectYggdrasil(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      // Two-tone sky: blue above, brown below.
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
          const float l = s.transparent ? 60.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
          const float sf = static_cast<float>(y) / h;
          const bool below = y > h * 0.55F;
          dst[static_cast<std::size_t>(y) * w + x] =
              below ? Rgb{u8(80 + l * 0.10F - 30 * (sf - 0.55F) * 2),
                           u8(60 + l * 0.08F), u8(40 + l * 0.06F), false}
                    : Rgb{u8(80 + l * 0.10F), u8(120 + l * 0.10F),
                           u8(180 + l * 0.10F - 50 * sf), false};
        }
      const float cx = w * 0.5F;
      const float trunkTop = h * 0.15F;
      const float trunkBot = h * 0.85F;
      // Huge data-textured trunk.
      for (int yy = static_cast<int>(trunkTop); yy <= static_cast<int>(trunkBot); ++yy) {
        const float yf = (yy - trunkTop) / (trunkBot - trunkTop);
        const float halfW = mn * (0.04F + 0.02F * std::sin(yf * 3.14159F));
        for (int xo = -static_cast<int>(halfW); xo <= static_cast<int>(halfW); ++xo) {
          const int xx = static_cast<int>(cx + xo);
          if (xx < 0 || xx >= w || yy < 0 || yy >= h) continue;
          const Rgb d = sample(src, w, h, xx, yy);
          const float dr = d.transparent ? 120.0F : d.r;
          const float dg = d.transparent ? 80.0F : d.g;
          dst[static_cast<std::size_t>(yy) * w + xx] =
              Rgb{u8(100 + dr * 0.30F), u8(70 + dg * 0.30F), u8(40), false};
        }
      }
      // Up-branches (3) — leafy with data tint.
      for (int b = 0; b < 5; ++b) {
        const float ang = -1.57F + (b - 2) * 0.40F + std::sin(t * 1.5F) * 0.05F;
        const float bx = cx + std::cos(ang) * mn * 0.30F;
        const float by = trunkTop - std::sin(-ang) * mn * 0.05F * 5;
        drawSeg(dst, w, h, cx, trunkTop, bx, by, std::max(1.0F, mn * 0.005F), ya, Rgb{100, 70, 40, false});
        drawDataDisk(dst, w, h, src, bx, by, mn * 0.06F, ya, 0.7F, 0.0F, Rgb{60, 160, 60, false});
      }
      // Down-roots (3 realms).
      for (int r = 0; r < 3; ++r) {
        const float rang = 1.57F + (r - 1) * 0.50F;
        const float rx = cx + std::cos(rang) * mn * 0.25F;
        const float ry = trunkBot + std::sin(rang) * mn * 0.05F;
        drawSeg(dst, w, h, cx, trunkBot, rx, ry, std::max(1.0F, mn * 0.005F), ya,
                Rgb{80, 50, 30, false});
        // Realm marker (data-textured disk).
        drawDataDisk(dst, w, h, src, rx, ry, mn * 0.03F, ya, 0.8F, 0.0F, Rgb{160, 80, 40, false});
      }
    });
}

void effectAtlas(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  FootSprite f = loadPythonFootSprite();
  // Atlas-the-Titan as the marionette; the wave motion lifts an arm so
  // the Python foot (standing in for the world he holds aloft) tracks
  // the rising hand and gets the visible strain Atlas is famous for.
  DancerMotion mot = loadDancerMotion("wave");
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
          const float l = s.transparent ? 30.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
          const float sf = static_cast<float>(y) / h;
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(8 + l * 0.10F + 40 * sf), u8(6 + l * 0.10F + 20 * sf),
                  u8(20 + l * 0.10F), false};
        }
      const float cx = w * 0.5F, base = h * 0.95F;
      const Rgb sil{20, 20, 30, false};
      std::vector<std::array<double, 2>> joints;
      const float figH = h * 0.70F;
      const float phase = t * 1.5F * mot.anim.frameCount;
      drawDancer(dst, w, h, ya, cx, base, figH, mot, phase, sil, &joints);

      // Python foot rides whichever hand is currently higher (= the
      // raised arm during the wave). Wobble + tilt make the world look
      // genuinely heavy.
      const int lH = mot.ok ? mot.anim.jointIndex("LeftHand")  : -1;
      const int rH = mot.ok ? mot.anim.jointIndex("RightHand") : -1;
      double fx = cx, fy = h * 0.22F;
      if (lH >= 0 && rH >= 0 && lH < static_cast<int>(joints.size())
          && rH < static_cast<int>(joints.size()))
      {
        const auto& L = joints[lH];
        const auto& R = joints[rH];
        const auto& hand = (L[1] < R[1]) ? L : R;
        fx = hand[0];
        fy = hand[1] - mn * 0.05F;
      }
      const float wobble = std::sin(t * 4.0F) * mn * 0.012F;
      drawFootSprite(dst, w, h, ya, f.img.get(), f.fw, f.fh, f.needChromaKey,
                     fx + wobble, fy, mn * 0.25F, std::sin(t * 1.0F) * 0.08F + 0.05F);
    });
}

void effectDamoclesFoot(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  FootSprite f = loadPythonFootSprite();
  runFrames(renderer, w, h, 6000,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb d = sample(src, w, h, x, y);
          const float dl = d.transparent ? 40.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(50 + dl * 0.20F), u8(30 + dl * 0.15F), u8(40 + dl * 0.15F), false};
        }
      // Throne.
      const float cx = w * 0.5F;
      const Rgb gold{200, 160, 60, false};
      drawSeg(dst, w, h, cx - mn * 0.12F, h * 0.55F, cx - mn * 0.12F, h * 0.92F,
              mn * 0.018F, ya, gold);
      drawSeg(dst, w, h, cx + mn * 0.12F, h * 0.55F, cx + mn * 0.12F, h * 0.92F,
              mn * 0.018F, ya, gold);
      drawSeg(dst, w, h, cx - mn * 0.12F, h * 0.55F, cx + mn * 0.12F, h * 0.55F,
              mn * 0.020F, ya, gold);
      // King silhouette (Damocles) — pale, looking up.
      drawSeg(dst, w, h, cx, h * 0.50F, cx, h * 0.70F, mn * 0.030F, ya, Rgb{30, 20, 50, false});
      plotDot(dst, w, h, cx, h * 0.46F, mn * 0.030F, ya, Rgb{220, 180, 140, false});
      // Snap moment + free-fall.
      const float snapT = 0.75F;
      const bool snapped = t > snapT;
      const float fall = snapped ? (t - snapT) * 4.0F : 0.0F;  // accelerated drop
      const float footY = snapped ? std::min(h * 0.50F, h * 0.15F + fall * fall * mn * 0.50F)
                                  : h * 0.15F + std::sin(t * 2.0F) * mn * 0.005F;
      // Thread (fraying as t -> snapT).
      if (!snapped) {
        const int n = static_cast<int>((1.0F - t / snapT) * 8 + 2);
        for (int k = 0; k < n; ++k) {
          const float yy = h * (0.18F + (k / static_cast<float>(n)) * (footY / h - 0.18F));
          plotDot(dst, w, h, cx + (std::sin(yy * 0.5F + t) * mn * 0.002F), yy,
                  std::max(1.0F, mn * 0.001F), ya, Rgb{240, 220, 200, false});
        }
      }
      // Foot dangling / falling.
      drawFootSprite(dst, w, h, ya, f.img.get(), f.fw, f.fh, f.needChromaKey,
                     cx, footY, mn * 0.20F, snapped ? (fall * 3.0F) : (std::sin(t * 2.0F) * 0.10F));
    });
}

void effectEden(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  FootSprite f = loadPythonFootSprite();
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb d = sample(src, w, h, x, y);
          const float dl = d.transparent ? 50.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          const float sf = static_cast<float>(y) / h;
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(60 + dl * 0.20F + 60 * (1 - sf)), u8(120 + dl * 0.25F + 40 * (1 - sf)),
                  u8(80 + dl * 0.15F), false};
        }
      // Tree trunk + canopy.
      const float trunkX = w * 0.55F;
      drawSeg(dst, w, h, trunkX, h * 0.92F, trunkX, h * 0.30F, mn * 0.030F, ya,
              Rgb{80, 50, 20, false});
      for (int i = 0; i < 30; ++i) {
        const float a = i / 30.0F * 6.2832F;
        plotDot(dst, w, h, trunkX + std::cos(a) * mn * 0.18F,
                h * 0.30F + std::sin(a) * mn * 0.18F / ya, mn * 0.030F, ya,
                Rgb{40, 100, 50, false});
      }
      // Serpent — wavy line along trunk, with head.
      for (int k = 0; k < 12; ++k) {
        const float yf = h * (0.45F + 0.30F * (k / 11.0F));
        const float xx = trunkX + std::sin(yf * 0.08F + t) * mn * 0.04F;
        plotDot(dst, w, h, xx, yf, mn * 0.012F, ya, Rgb{60, 130, 30, false});
      }
      // Head — extends outward holding foot.
      const float reach = std::clamp(t * 1.5F, 0.0F, 1.0F);
      const float headX = trunkX + mn * 0.10F + reach * mn * 0.08F;
      const float headY = h * 0.55F;
      plotDot(dst, w, h, headX, headY, mn * 0.018F, ya, Rgb{80, 160, 40, false});
      // Forked tongue.
      drawSeg(dst, w, h, headX, headY, headX + mn * 0.025F, headY, std::max(1.0F, mn * 0.003F), ya,
              Rgb{180, 20, 60, false});
      // Foot offered.
      drawFootSprite(dst, w, h, ya, f.img.get(), f.fw, f.fh, f.needChromaKey,
                     headX + mn * 0.08F, headY, mn * 0.12F, std::sin(t * 1.5F) * 0.15F);
      // Eve silhouette (right side).
      drawSeg(dst, w, h, w * 0.80F, h * 0.92F, w * 0.80F, h * 0.55F, mn * 0.030F, ya,
              Rgb{180, 140, 110, false});
      plotDot(dst, w, h, w * 0.80F, h * 0.50F, mn * 0.025F, ya, Rgb{220, 180, 140, false});
      // Eve's arm reaching.
      drawSeg(dst, w, h, w * 0.80F, h * 0.58F, headX + mn * 0.05F, headY + mn * 0.02F,
              mn * 0.014F, ya, Rgb{220, 180, 140, false});
    });
}

void effectExcalibur(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  FootSprite f = loadPythonFootSprite();
  runFrames(renderer, w, h, 5800,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb d = sample(src, w, h, x, y);
          const float dl = d.transparent ? 50.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          const float sf = static_cast<float>(y) / h;
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(80 + dl * 0.15F + 40 * (1 - sf)), u8(100 + dl * 0.15F + 30 * (1 - sf)),
                  u8(130 + dl * 0.20F - 30 * sf), false};
        }
      // Stone — big grey blob.
      const float stoneCx = w * 0.5F, stoneCy = h * 0.78F;
      for (int yo = -static_cast<int>(mn * 0.10F); yo <= static_cast<int>(mn * 0.10F); ++yo)
        for (int xo = -static_cast<int>(mn * 0.22F); xo <= static_cast<int>(mn * 0.22F); ++xo) {
          const float nx = xo / (mn * 0.22F), nyN = yo / (mn * 0.10F);
          if (nx * nx + nyN * nyN > 1.0F) continue;
          const int xx = static_cast<int>(stoneCx + xo), yy = static_cast<int>(stoneCy + yo);
          if (xx >= 0 && xx < w && yy >= 0 && yy < h)
            dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{u8(110 + 30 * (1.0F - std::fabs(nx))), u8(110), u8(110), false};
        }
      // Pull progress: foot rises from stone.
      const float pull = std::clamp(t * 1.4F, 0.0F, 1.0F);
      const float footY = stoneCy - mn * 0.10F - pull * mn * 0.25F;
      // Glow ring at the moment of release.
      if (pull > 0.85F) {
        const float a = (pull - 0.85F) / 0.15F;
        for (int r = 0; r < 8; ++r) {
          const float rr = mn * (0.08F + 0.04F * r) * a;
          for (float ang = 0; ang < 6.2832F; ang += 0.05F)
            plotDot(dst, w, h, stoneCx + std::cos(ang) * rr, stoneCy + std::sin(ang) * rr / ya,
                    std::max(1.0F, mn * 0.002F), ya, Rgb{u8(255 * (1 - a)), u8(240 * (1 - a)), u8(120), false});
        }
      }
      // Foot — emerging vertically, sole at the top.
      drawFootSprite(dst, w, h, ya, f.img.get(), f.fw, f.fh, f.needChromaKey,
                     stoneCx, footY, mn * 0.22F, -1.5708F);
      // Arm pulling (above the foot).
      const float armY = footY - mn * 0.12F;
      drawSeg(dst, w, h, stoneCx, armY, stoneCx, armY - mn * 0.12F,
              mn * 0.030F, ya, Rgb{220, 180, 140, false});
      plotDot(dst, w, h, stoneCx, armY - mn * 0.13F, mn * 0.025F, ya, Rgb{220, 180, 140, false});
    });
}

void effectPandoraFoot(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
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
          const float dl = d.transparent ? 30.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(30 + dl * 0.15F), u8(20 + dl * 0.10F), u8(40 + dl * 0.10F), false};
        }
      const float cx = w * 0.5F, cy = h * 0.70F;
      // Box body.
      const float W = mn * 0.10F, H = mn * 0.07F;
      for (int yo = 0; yo <= static_cast<int>(H); ++yo)
        for (int xo = -static_cast<int>(W); xo <= static_cast<int>(W); ++xo) {
          const int xx = static_cast<int>(cx + xo), yy = static_cast<int>(cy + yo);
          if (xx >= 0 && xx < w && yy >= 0 && yy < h)
            dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{120, 80, 40, false};
        }
      // Lid opens upward.
      const float lidOpen = std::clamp(t * 1.6F, 0.0F, 1.0F);
      const float lidA = -lidOpen * 1.4F;
      drawSeg(dst, w, h, cx - W, cy, cx - W + std::cos(lidA) * (W * 2),
              cy + std::sin(lidA) * (W * 2), std::max(1.0F, mn * 0.012F), ya,
              Rgb{180, 130, 60, false});
      // Foot rises out, glowing.
      const float rise = std::clamp((t - 0.20F) / 0.50F, 0.0F, 1.0F);
      const float fcy = cy - rise * mn * 0.40F;
      drawFootSprite(dst, w, h, ya, f.img.get(), f.fw, f.fh, f.needChromaKey,
                     cx, fcy, mn * 0.16F, std::sin(t * 2.0F) * 0.10F);
      // Motes escaping.
      for (int i = 0; i < 30; ++i) {
        const float age = std::clamp(t - 0.25F - hash(i) * 0.30F, 0.0F, 1.0F);
        if (age <= 0) continue;
        const float ang = (hash(i) - 0.5F) * 2.0F;
        const float mx = cx + std::cos(ang) * age * mn * 0.30F;
        const float my = cy - age * mn * 0.50F;
        plotDot(dst, w, h, mx, my, mn * 0.012F * (1 - age * 0.5F), ya,
                Rgb{u8(240 - age * 100), u8(180 + age * 50), u8(80), false});
      }
    });
}

void effectTrojanFoot(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  FootSprite f = loadPythonFootSprite();
  runFrames(renderer, w, h, 5800,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb d = sample(src, w, h, x, y);
          const float dl = d.transparent ? 60.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(160 + dl * 0.20F), u8(120 + dl * 0.15F), u8(80 + dl * 0.10F), false};
        }
      // Ground.
      const float groundY = h * 0.85F;
      for (int xx = 0; xx < w; ++xx)
        dst[static_cast<std::size_t>(groundY) * w + xx] = Rgb{80, 60, 40, false};
      // Horse silhouette.
      const float cx = w * 0.5F, body = mn * 0.22F;
      // Body oval.
      for (int yo = -static_cast<int>(body * 0.35F); yo <= static_cast<int>(body * 0.35F); ++yo)
        for (int xo = -static_cast<int>(body * 1.1F); xo <= static_cast<int>(body * 0.7F); ++xo) {
          const float nx = xo / (body * 1.1F), nyN = yo / (body * 0.35F);
          if (nx * nx + nyN * nyN > 1.0F) continue;
          const int xx = static_cast<int>(cx + xo), yy = static_cast<int>(groundY - body * 0.5F + yo);
          if (xx >= 0 && xx < w && yy >= 0 && yy < h)
            dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{100, 70, 35, false};
        }
      // Head + neck.
      drawSeg(dst, w, h, cx + body * 0.7F, groundY - body * 0.5F,
              cx + body * 1.0F, groundY - body * 1.2F, std::max(1.0F, mn * 0.018F), ya,
              Rgb{100, 70, 35, false});
      plotDot(dst, w, h, cx + body * 1.0F, groundY - body * 1.2F, mn * 0.020F, ya,
              Rgb{100, 70, 35, false});
      // Legs.
      for (int leg = 0; leg < 4; ++leg) {
        const float lx = cx + (leg - 1.5F) * body * 0.40F;
        drawSeg(dst, w, h, lx, groundY - body * 0.20F, lx, groundY, mn * 0.020F, ya,
                Rgb{100, 70, 35, false});
      }
      // Side door + tiny feet pouring out.
      const float doorX = cx - body * 0.10F, doorY = groundY - body * 0.45F;
      const float opened = std::clamp(t * 1.4F, 0.0F, 1.0F);
      // Door panel.
      drawSeg(dst, w, h, doorX, doorY, doorX, doorY + body * 0.20F,
              std::max(1.0F, mn * 0.005F), ya, Rgb{60, 40, 20, false});
      // Tiny feet streaming out.
      for (int i = 0; i < 18; ++i) {
        const float age = std::clamp(t * 0.6F + hash(i) * 0.4F - 0.2F, 0.0F, 1.0F);
        if (age <= 0) continue;
        const float fx = doorX - age * mn * (0.10F + hash(i * 3) * 0.10F);
        const float fy = doorY + body * 0.10F + (hash(i * 5) - 0.5F) * body * 0.10F;
        drawFootSprite(dst, w, h, ya, f.img.get(), f.fw, f.fh, f.needChromaKey,
                       fx, fy, mn * 0.025F, hash(i * 7) * 6.0F);
      }
      (void)opened;
    });
}

void effectWilliamTell(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  FootSprite f = loadPythonFootSprite();
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb d = sample(src, w, h, x, y);
          const float dl = d.transparent ? 70.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          const float sf = static_cast<float>(y) / h;
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(160 + dl * 0.20F - 30 * sf), u8(180 + dl * 0.20F - 30 * sf),
                  u8(180 + dl * 0.20F + 20 * sf), false};
        }
      // Ground.
      const float groundY = h * 0.88F;
      for (int xx = 0; xx < w; ++xx)
        dst[static_cast<std::size_t>(groundY) * w + xx] = Rgb{60, 90, 40, false};
      // Son (right side).
      const float sonX = w * 0.78F;
      drawSeg(dst, w, h, sonX, groundY, sonX, groundY - mn * 0.20F, mn * 0.025F, ya, Rgb{40, 30, 20, false});
      plotDot(dst, w, h, sonX, groundY - mn * 0.23F, mn * 0.025F, ya, Rgb{220, 180, 140, false});
      // Foot balanced on his head.
      drawFootSprite(dst, w, h, ya, f.img.get(), f.fw, f.fh, f.needChromaKey,
                     sonX, groundY - mn * 0.32F, mn * 0.12F, 0.0F);
      // William Tell (left side) with crossbow.
      const float wX = w * 0.18F;
      drawSeg(dst, w, h, wX, groundY, wX, groundY - mn * 0.22F, mn * 0.025F, ya, Rgb{40, 30, 20, false});
      plotDot(dst, w, h, wX, groundY - mn * 0.25F, mn * 0.025F, ya, Rgb{220, 180, 140, false});
      // Crossbow arm extended.
      drawSeg(dst, w, h, wX, groundY - mn * 0.20F, wX + mn * 0.10F, groundY - mn * 0.25F,
              mn * 0.015F, ya, Rgb{40, 30, 20, false});
      // Arrow flying.
      const float arrowX = wX + mn * 0.10F + t * (sonX - wX - mn * 0.20F);
      const float arrowY = groundY - mn * 0.32F;
      drawSeg(dst, w, h, arrowX, arrowY, arrowX + mn * 0.04F, arrowY,
              std::max(1.0F, mn * 0.004F), ya, Rgb{100, 70, 30, false});
      plotDot(dst, w, h, arrowX + mn * 0.04F, arrowY, std::max(1.0F, mn * 0.006F), ya,
              Rgb{200, 30, 30, false});
      // Hole in the foot once the arrow reaches it.
      if (arrowX > sonX - mn * 0.04F)
        plotDot(dst, w, h, sonX, arrowY, mn * 0.014F, ya, Rgb{255, 60, 40, false});
    });
}

}  // namespace ee_detail
}  // namespace Qdless
