#include "QdlessExitEffectCommon.h"
#include "QdlessMarionette.h"

namespace Qdless
{
namespace ee_detail
{

void effectSubmarine(
    const Renderer& renderer, const std::vector<Rgb>& src, int w, int h, std::mt19937& rng)
{
  const float ya = yAspectFor(renderer);
  const float cx = (w - 1) * 0.5F;
  const float surfaceY = h * 0.42F;  // final waterline
  const float hw = std::max(3.0F, h * 0.10F) * 4.5F;
  constexpr int kBub = 16;
  std::array<float, kBub> bubX{};
  std::array<float, kBub> bubPh{};
  std::uniform_real_distribution<float> dBx(-0.5F, 0.5F);
  std::uniform_real_distribution<float> dPh(0.0F, 1.0F);
  for (int i = 0; i < kBub; ++i)
  {
    bubX[i] = dBx(rng);
    bubPh[i] = dPh(rng);
  }
  runFrames(renderer,
            w,
            h,
            5000,
            [&](float t, std::vector<Rgb>& dst)
            {
              if (t < 0.35F)
              {
                // Frame drops out the bottom; ocean rises; sub drops to surface.
                const float ta = t / 0.35F;
                const int off = static_cast<int>(std::lround(ta * (h + 4)));
                for (int y = 0; y < h; ++y)
                {
                  const int sy = y - off;
                  if (sy >= 0 && sy < h)
                    for (int x = 0; x < w; ++x)
                      dst[static_cast<std::size_t>(y) * w + x] =
                          src[static_cast<std::size_t>(sy) * w + x];
                }
                drawOcean(dst, w, h, h - ta * (h - surfaceY), 1.0F);
                drawSubmarine(dst, w, h, ya, cx, -h * 0.3F + ta * (surfaceY + h * 0.3F));
              }
              else
              {
                const float tb = (t - 0.35F) / 0.65F;  // 0..1
                const float fade = tb < 0.8F ? 1.0F : std::max(0.0F, 1.0F - (tb - 0.8F) / 0.2F);
                drawOcean(dst, w, h, surfaceY, fade);
                const float scy = surfaceY + tb * (h - surfaceY + h * 0.25F);
                drawSubmarine(dst, w, h, ya, cx, scy);
                // Bubbles rise from the sub toward the surface.
                for (int i = 0; i < kBub; ++i)
                {
                  const float ph = std::fmod(bubPh[i] + tb * 1.6F, 1.0F);
                  const float by = scy - ph * (h * 0.6F);
                  if (by >= 0 && by < h)
                    plotDot(dst,
                            w,
                            h,
                            cx + bubX[i] * hw,
                            by,
                            std::max(1.0F, h * 0.012F),
                            ya,
                            Rgb{200, 225, 245, false});
                }
              }
            });
}

void effectRiverdance(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  constexpr int kN = 5;
  const float floorY = (h - 1) * 0.94F;
  const float bodyH = h * 0.60F;
  // Five marionette dancers driven by the CMU kick motion — high-kick
  // Irish step works. Each dancer is offset by a few frames so the
  // line looks lively rather than perfectly synced.
  DancerMotion mot = loadDancerMotion("kick");
  runFrames(
      renderer, w, h, 5200,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float alpha = skeletonStage(dst, src, w, h, t, static_cast<int>(floorY));
        if (alpha <= 0.0F) return;
        const float beat = t * 8.0F * speed;
        for (int i = 0; i < kN; ++i)
        {
          const float cx = w * ((i + 0.5F) / kN);
          const double phase = beat * 4.0 + i * 3.0;
          drawDancer(dst, w, h, ya, cx, floorY, bodyH, mot, phase,
                     boneTint(src, w, h, cx, alpha));
        }
      });
}

void effectThriller(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  constexpr int kN = 5;
  const float floorY = (h - 1) * 0.94F;
  const float H = h * 0.58F;
  // Zombie shuffle — the CMU sneak motion (subject 120 Mickey sneaky
  // walk) reads beautifully as a Thriller shamble. Each dancer is
  // phase-offset so the line looks possessed rather than choreographed.
  DancerMotion mot = loadDancerMotion("sneak");
  runFrames(renderer, w, h, 5000,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float alpha = skeletonStage(dst, src, w, h, t, static_cast<int>(floorY));
              if (alpha <= 0.0F) return;
              const float beat = t * 9.0F * speed;
              for (int i = 0; i < kN; ++i)
              {
                const float cx = w * ((i + 0.5F) / kN);
                const double phase = beat * 3.0 + i * 5.0;
                drawDancer(dst, w, h, ya, cx, floorY, H, mot, phase,
                           boneTint(src, w, h, cx, alpha));
              }
            });
}

void effectYMCA(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  constexpr int kN = 5;
  const float floorY = (h - 1) * 0.94F;
  const float H = h * 0.58F;
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  static const std::array<Limb, 4> kLA = {{{-0.14F, -0.19F, -0.28F, -0.46F},
                                           {-0.19F, -0.19F, -0.07F, 0.16F},
                                           {-0.17F, -0.20F, -0.31F, -0.06F},
                                           {-0.12F, -0.26F, -0.04F, -0.48F}}};
  static const std::array<Limb, 4> kRA = {{{0.14F, -0.19F, 0.28F, -0.46F},
                                           {0.19F, -0.19F, 0.07F, 0.16F},
                                           {-0.03F, -0.03F, -0.25F, 0.12F},
                                           {0.12F, -0.26F, 0.04F, -0.48F}}};
  const Limb lL{-0.01F, 0.23F, 0.02F, 0.46F};
  const Limb lR{0.01F, 0.23F, -0.02F, 0.46F};
  runFrames(renderer,
            w,
            h,
            5200,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float alpha = skeletonStage(dst, src, w, h, t, static_cast<int>(floorY));
              if (alpha <= 0.0F)
                return;
              const float seg = 0.16F / speed;  // letter-hold time
              const int idx = static_cast<int>(t / seg) % 4;
              const int prev = (idx + 3) % 4;
              const float blend = std::clamp((t / seg - std::floor(t / seg)) / 0.3F, 0.0F, 1.0F);
              const Limb aL = lerpLimb(kLA[prev], kLA[idx], blend);
              const Limb aR = lerpLimb(kRA[prev], kRA[idx], blend);
              const float bob = std::sin(t * 9.0F * speed) * 0.02F * H;
              for (int i = 0; i < kN; ++i)
              {
                const float cx = w * ((i + 0.5F) / kN);
                drawSkeleton(dst,
                             w,
                             h,
                             ya,
                             cx,
                             floorY,
                             H,
                             bob,
                             0.0F,
                             aL,
                             aR,
                             lL,
                             lR,
                             boneTint(src, w, h, cx, alpha),
                             Rgb{u8(18 * alpha), u8(12 * alpha), u8(12 * alpha), false});
              }
            });
}

void effectGreekDance(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  constexpr int kN = 5;
  const float floorY = (h - 1) * 0.94F;
  const float H = h * 0.58F;
  // Greek sirtaki / hasapiko line-dance — CMU walk is the closest
  // generic motion; the line of dancers all step in sync (offset 0
  // per dancer) so the kalamatianos chain reads.
  DancerMotion mot = loadDancerMotion("walk");
  runFrames(renderer, w, h, 5200,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float alpha = skeletonStage(dst, src, w, h, t, static_cast<int>(floorY));
              if (alpha <= 0.0F) return;
              const float beat = t * 7.0F * speed;
              for (int i = 0; i < kN; ++i)
              {
                const float cx = w * ((i + 0.5F) / kN);
                const double phase = beat * 5.0;  // in sync
                drawDancer(dst, w, h, ya, cx, floorY, H, mot, phase,
                           boneTint(src, w, h, cx, alpha));
              }
            });
}

void effectRussianDance(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  constexpr int kN = 5;
  const float floorY = (h - 1) * 0.94F;
  const float H = h * 0.58F;
  // Cossack squat-and-kick — the CMU kick captures the alternating
  // straight-leg shoot-out reasonably well. Each dancer is offset by
  // ~12 frames so the line of squat-kickers staggers across the stage.
  DancerMotion mot = loadDancerMotion("kick");
  runFrames(renderer, w, h, 5000,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float alpha = skeletonStage(dst, src, w, h, t, static_cast<int>(floorY));
              if (alpha <= 0.0F) return;
              const float beat = t * 12.0F * speed;
              for (int i = 0; i < kN; ++i)
              {
                const float cx = w * ((i + 0.5F) / kN);
                const double phase = beat * 3.0 + i * 12.0;
                drawDancer(dst, w, h, ya, cx, floorY, H, mot, phase,
                           boneTint(src, w, h, cx, alpha));
              }
            });
}

void effectBallet(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  constexpr int kN = 5;
  const float floorY = (h - 1) * 0.94F;
  const float H = h * 0.58F;
  // Ballet is choreographed turns and graceful poses — closest CMU
  // motion is salsa (continuous body sway). Dancers all in sync.
  DancerMotion mot = loadDancerMotion("salsa");
  runFrames(renderer, w, h, 4500,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float alpha = skeletonStage(dst, src, w, h, t, static_cast<int>(floorY));
              if (alpha <= 0.0F) return;
              const float spin = t * 6.2832F * 1.2F * speed;
              const double phase = (t * 1.0F + spin * 0.05F) * 100.0;
              for (int i = 0; i < kN; ++i)
              {
                const float cx = w * ((i + 0.5F) / kN);
                drawDancer(dst, w, h, ya, cx, floorY, H, mot, phase + i * 4.0,
                           boneTint(src, w, h, cx, alpha));
              }
            });
}

void effectMacarena(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  constexpr int kN = 5;
  const float floorY = (h - 1) * 0.94F;
  const float H = h * 0.58F;
  // CMU salsa stands in for the macarena hip wiggle — the iconic
  // arm-gesture sequence (palm-down → opposite shoulder → behind head →
  // opposite hip → hop) doesn't map to any available CMU motion, so the
  // marionettes do generic salsa hips instead. Each dancer phase-offset.
  DancerMotion mot = loadDancerMotion("salsa");
  runFrames(renderer, w, h, 6000,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float alpha = skeletonStage(dst, src, w, h, t, static_cast<int>(floorY));
              if (alpha <= 0.0F) return;
              const float beat = t * 8.0F * speed;
              const float sway = std::sin(t * 4.5F * speed) * 0.03F * (w / static_cast<float>(kN));
              for (int i = 0; i < kN; ++i)
              {
                const float cx = w * ((i + 0.5F) / kN) + sway;
                const double phase = beat * 4.0 + i * 6.0;
                drawDancer(dst, w, h, ya, cx, floorY, H, mot, phase,
                           boneTint(src, w, h, cx, alpha));
              }
            });
}

void effectBassSpiral(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float ya = yAspectFor(renderer);
  const float cx = w * 0.5F, cy = h * 0.5F, mn = std::min(static_cast<float>(w), h * ya);
  const float eyeR = mn * 0.48F, irisR = mn * 0.27F;
  runFrames(renderer,
            w,
            h,
            4800,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float spinR = std::pow(t, 1.1F) * mn * 0.72F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const float dx = x - cx, dy = (y - cy) * ya, r = std::hypot(dx, dy);
                  Rgb base;
                  if (r < irisR)
                  {
                    const Rgb d = sample(
                        src, w, h, (dx / irisR * 0.5F + 0.5F) * w, (dy / irisR * 0.5F + 0.5F) * h);
                    if (r < irisR * 0.22F)
                      base = Rgb{4, 4, 6, false};
                    else
                      base = Rgb{u8(d.r * 0.85F), u8(d.g * 0.85F), u8(d.b * 0.85F), false};
                  }
                  else if (r < eyeR)
                    base = Rgb{225, 220, 212, false};
                  else
                    base = Rgb{30, 22, 20, false};
                  if (r < spinR)
                  {
                    const float th = std::atan2(dy, dx);
                    const float arm = std::sin(th * 4.0F - std::log(r + 1.0F) * 2.6F + t * 7.0F);
                    if (arm > 0.0F)
                      base = Rgb{8, 8, 10, false};
                    else if (arm < -0.7F)
                      base = Rgb{240, 235, 226, false};
                  }
                  dst[static_cast<std::size_t>(y) * w + x] = base;
                }
            });
}

void effectSinginRain(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float postX = w * 0.55F;
  runFrames(renderer,
            w,
            h,
            5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float lantX = postX, lantY = h * 0.22F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 30.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  const float gd = std::hypot(x - lantX, (y - lantY) * ya);
                  const float glow = std::exp(-gd / (mn * 0.30F));
                  dst[static_cast<std::size_t>(y) * w + x] = Rgb{u8(16 + l * 0.10F + glow * 130),
                                                                 u8(24 + l * 0.12F + glow * 100),
                                                                 u8(38 + l * 0.18F + glow * 60),
                                                                 false};
                }
              drawSeg(dst,
                      w,
                      h,
                      postX,
                      h * 0.95F,
                      postX,
                      h * 0.22F,
                      std::max(1.0F, mn * 0.014F),
                      ya,
                      Rgb{18, 16, 18, false});
              plotDot(dst, w, h, lantX, lantY, mn * 0.06F, ya, Rgb{255, 230, 150, false});
              const float barX = postX - mn * 0.16F, barY = h * 0.26F;
              drawSeg(dst,
                      w,
                      h,
                      postX,
                      barY,
                      barX,
                      barY,
                      std::max(1.0F, mn * 0.012F),
                      ya,
                      Rgb{18, 16, 18, false});
              const float swing = std::sin(t * 2.2F) * 0.45F;
              const float L = mn * 0.20F;
              const float bodyX = barX + std::sin(swing) * L, bodyY = barY + std::cos(swing) * L;
              const Rgb suit{30, 26, 32, false};
              drawSeg(dst, w, h, barX, barY, bodyX, bodyY, std::max(1.0F, mn * 0.020F), ya, suit);
              plotDot(dst,
                      w,
                      h,
                      bodyX + std::sin(swing) * mn * 0.04F,
                      bodyY + std::cos(swing) * mn * 0.04F,
                      mn * 0.04F,
                      ya,
                      Rgb{210, 180, 150, false});
              const float kick = std::sin(t * 4.0F) * 0.5F;
              const float legX = bodyX + std::sin(swing) * mn * 0.18F;
              const float legY = bodyY + std::cos(swing) * mn * 0.18F;
              drawSeg(dst,
                      w,
                      h,
                      bodyX,
                      bodyY,
                      legX - kick * mn * 0.08F,
                      legY,
                      std::max(1.0F, mn * 0.014F),
                      ya,
                      suit);
              drawSeg(dst,
                      w,
                      h,
                      bodyX,
                      bodyY,
                      legX + kick * mn * 0.08F,
                      legY,
                      std::max(1.0F, mn * 0.014F),
                      ya,
                      suit);
              const float uHand = bodyX - mn * 0.10F, uHandY = bodyY - mn * 0.05F;
              drawSeg(dst,
                      w,
                      h,
                      bodyX,
                      bodyY - mn * 0.04F,
                      uHand,
                      uHandY,
                      std::max(1.0F, mn * 0.012F),
                      ya,
                      suit);
              const float uTop = uHandY - mn * 0.20F;
              drawSeg(dst, w, h, uHand, uHandY, uHand, uTop, std::max(1.0F, mn * 0.010F), ya, suit);
              for (int k = -5; k <= 5; ++k)
              {
                const float a = k / 5.0F;
                const float ux = uHand + a * mn * 0.18F;
                const float uy = uTop - (1.0F - a * a) * mn * 0.08F;
                plotDot(dst, w, h, ux, uy, mn * 0.024F, ya, Rgb{40, 32, 36, false});
              }
              for (int i = 0; i < 120; ++i)
              {
                const float rx = std::fmod(hash(i * 2) * w + t * w * 0.05F + i * 0.7F, (float)w);
                const float ry = std::fmod(hash(i * 2 + 1) * h + t * h * 1.6F + i * 0.4F, (float)h);
                drawSeg(dst,
                        w,
                        h,
                        rx,
                        ry,
                        rx - mn * 0.018F,
                        ry + mn * 0.04F,
                        std::max(1.0F, mn * 0.005F),
                        ya,
                        Rgb{170, 200, 230, false});
              }
            });
}

void effectSoundOfMusic(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float cx = w * 0.5F, cy = h * 0.55F;
  runFrames(
      renderer,
      w,
      h,
      5600,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float rot = t * 4.0F;
        const float ca = std::cos(rot), sa = std::sin(rot);
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const float dx = x - cx, dy = (y - cy) * ya;
            const float rx = dx * ca - dy * sa, ry = dx * sa + dy * ca;
            const Rgb s = sample(src, w, h, cx + rx, cy + ry / ya);
            const float l = s.transparent ? 60.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            const float sf = std::clamp(0.5F + (cy + ry / ya) / h * 0.5F, 0.0F, 1.0F);
            Rgb c;
            if (sf > 0.65F)  // green meadow underfoot
              c = Rgb{u8(60 + 60 * sf + l * 0.15F),
                      u8(130 + 50 * sf + l * 0.18F),
                      u8(50 + 20 * sf + l * 0.08F),
                      false};
            else  // alpine sky
              c = Rgb{u8(110 + 90 * sf + l * 0.10F),
                      u8(150 + 60 * sf + l * 0.10F),
                      u8(220 + l * 0.08F),
                      false};
            dst[static_cast<std::size_t>(y) * w + x] = c;
          }
        // the figure: a small spinning silhouette at centre, arms outstretched
        const float ht = mn * 0.14F;
        const Rgb maria{18, 14, 18, false}, dress{40, 70, 130, false};
        const float armSpin = std::sin(t * 6.0F) * 0.06F;  // little wobble
        const float fy = cy + ht * 0.4F;
        plotDot(dst, w, h, cx, fy - ht * 1.0F, ht * 0.13F, ya, maria);  // head
        drawSeg(
            dst, w, h, cx, fy - ht * 0.90F, cx, fy - ht * 0.35F, ht * 0.13F, ya, dress);  // body
        for (int leg = -1; leg <= 1; leg += 2)  // skirt flare into legs
          drawSeg(dst, w, h, cx, fy - ht * 0.35F, cx + leg * ht * 0.16F, fy, ht * 0.13F, ya, dress);
        drawSeg(dst,
                w,
                h,
                cx,
                fy - ht * 0.78F,
                cx - ht * 0.38F,
                fy - ht * 0.72F + armSpin * ht,
                ht * 0.08F,
                ya,
                maria);  // arms out
        drawSeg(dst,
                w,
                h,
                cx,
                fy - ht * 0.78F,
                cx + ht * 0.38F,
                fy - ht * 0.72F - armSpin * ht,
                ht * 0.08F,
                ya,
                maria);
      });
}

void effectBeethovenFifth(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
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
              Rgb{u8(240 + l * 0.05F), u8(230 + l * 0.05F), u8(200 + l * 0.05F), false};
        }
      // Five-line staff.
      const float staffY = h * 0.50F;
      const float staffSpace = mn * 0.03F;
      for (int i = 0; i < 5; ++i) {
        const float y0 = staffY - 2 * staffSpace + i * staffSpace;
        drawSeg(dst, w, h, w * 0.10F, y0, w * 0.90F, y0, std::max(1.0F, mn * 0.002F), ya,
                Rgb{20, 20, 20, false});
      }
      // 4 notes: three eighth notes (G-G-G) + one half note (E-flat).
      const float noteSpacing = w * 0.18F;
      const float startX = w * 0.20F;
      const float notesPlayed = std::clamp(t * 5.0F, 0.0F, 4.0F);
      for (int n = 0; n < 4; ++n) {
        if (n + 1 > notesPlayed) break;
        const float nx = startX + n * noteSpacing;
        // Three Gs (above the staff, line 4) then E-flat (line 5).
        const float ny = (n < 3) ? (staffY - 1 * staffSpace) : (staffY + 0.5F * staffSpace);
        plotDot(dst, w, h, nx, ny, mn * 0.020F, ya, Rgb{20, 20, 20, false});
        // Stem.
        drawSeg(dst, w, h, nx + mn * 0.018F, ny, nx + mn * 0.018F, ny - mn * 0.08F,
                std::max(1.0F, mn * 0.003F), ya, Rgb{20, 20, 20, false});
        // Eighth-note flag for first three.
        if (n < 3) {
          drawSeg(dst, w, h, nx + mn * 0.018F, ny - mn * 0.08F,
                  nx + mn * 0.040F, ny - mn * 0.06F, std::max(1.0F, mn * 0.005F), ya,
                  Rgb{20, 20, 20, false});
        }
      }
      // Big data-textured disk in the corner — visible weather data.
      drawDataDisk(dst, w, h, src, w * 0.85F, h * 0.20F, mn * 0.10F, ya, 0.5F, t * 0.5F,
                   Rgb{200, 60, 80, false});
    });
}

void effectConductorBaton(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb d = sample(src, w, h, x, y);
          const float dl = d.transparent ? 40.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(40 + dl * 0.20F), u8(30 + dl * 0.15F), u8(20 + dl * 0.10F), false};
        }
      // 4/4 pattern path: down-right-left-up.
      const float beat = std::fmod(t * 4.0F, 4.0F);
      const float cx = w * 0.5F, cy = h * 0.5F;
      const float R = mn * 0.25F;
      float bx = cx, by = cy;
      if (beat < 1) { by = cy - R + beat * 2 * R; }                                   // down
      else if (beat < 2) { by = cy + R; bx = cx + (beat - 1) * R; }                   // right
      else if (beat < 3) { by = cy + R - (beat - 2) * 2 * R; bx = cx + R; }           // up-right
      else { by = cy - R; bx = cx + R - (beat - 3) * R; }                              // back
      // Baton.
      const float baseX = cx - mn * 0.30F;
      const float baseY = cy + mn * 0.30F;
      drawSeg(dst, w, h, baseX, baseY, bx, by, std::max(1.0F, mn * 0.006F), ya,
              Rgb{220, 200, 160, false});
      plotDot(dst, w, h, baseX, baseY, mn * 0.020F, ya, Rgb{60, 40, 20, false});  // grip
      plotDot(dst, w, h, bx, by, mn * 0.015F, ya, Rgb{240, 220, 180, false});      // tip
    });
}

void effectHendrixGuitar(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
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
              Rgb{u8(30 + l * 0.10F), u8(20 + l * 0.05F), u8(20 + l * 0.05F), false};
        }
      // Guitar body (Strat-shape, simplified).
      const float cx = w * 0.5F, cy = h * 0.55F;
      const Rgb body{180, 80, 40, false};
      const float bL = mn * 0.30F;
      // Body oval.
      for (int yo = -static_cast<int>(bL * 0.18F); yo <= static_cast<int>(bL * 0.18F); ++yo)
        for (int xo = -static_cast<int>(bL * 0.30F); xo <= static_cast<int>(bL * 0.30F); ++xo) {
          const float nx = xo / (bL * 0.30F), nyN = yo / (bL * 0.18F);
          if (nx * nx + nyN * nyN > 1.0F) continue;
          const int xx = static_cast<int>(cx + xo), yy = static_cast<int>(cy + yo);
          if (xx >= 0 && xx < w && yy >= 0 && yy < h)
            dst[static_cast<std::size_t>(yy) * w + xx] = body;
        }
      // Neck.
      drawSeg(dst, w, h, cx + bL * 0.20F, cy, cx + bL * 0.70F, cy - bL * 0.15F,
              std::max(1.0F, mn * 0.020F), ya, Rgb{120, 70, 30, false});
      // Strings (6).
      for (int s = 0; s < 6; ++s) {
        const float yo = (s - 2.5F) * mn * 0.004F;
        drawSeg(dst, w, h, cx - bL * 0.20F, cy + yo, cx + bL * 0.70F, cy - bL * 0.15F + yo,
                std::max(1.0F, mn * 0.001F), ya, Rgb{220, 220, 220, false});
      }
      // Headstock.
      plotDot(dst, w, h, cx + bL * 0.75F, cy - bL * 0.18F, mn * 0.020F, ya,
              Rgb{60, 40, 20, false});
      // Flames climbing — data-textured.
      const float burn = std::clamp(t * 1.2F, 0.0F, 1.0F);
      for (int i = 0; i < 80; ++i) {
        const float fx = cx + (hash(i) - 0.5F) * bL * 0.6F;
        const float fage = std::fmod(t * 1.5F + hash(i * 3), 1.0F);
        const float fy = cy + mn * 0.10F - fage * mn * 0.35F * burn;
        const Rgb d = sample(src, w, h, static_cast<int>(fx), static_cast<int>(fy));
        const float dr = d.transparent ? 240.0F : d.r;
        plotDot(dst, w, h, fx, fy, mn * 0.015F * burn * (1 - fage), ya,
                Rgb{u8(255), u8(180 * (1 - fage) + dr * 0.05F), u8(40 * (1 - fage)), false});
      }
    });
}

void effectOperaCurtain(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
          dst[static_cast<std::size_t>(y) * w + x] = s.transparent ? Rgb{20, 20, 30, false} : s;
        }
      const float fall = std::clamp(t * 1.5F, 0.0F, 1.0F);
      const int hemY = static_cast<int>(fall * h);
      // Curtain pleats — data-tinted vertical bands.
      const int nPleats = 16;
      const float pleatW = static_cast<float>(w) / nPleats;
      for (int yy = 0; yy < hemY && yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx) {
          const Rgb d = sample(src, w, h, xx, yy);
          const float dr = d.transparent ? 180.0F : d.r;
          const float pleatPh = std::fmod(xx / pleatW, 1.0F);
          const float shadow = std::sin(pleatPh * 3.14159F);
          dst[static_cast<std::size_t>(yy) * w + xx] =
              Rgb{u8((140 + 40 * shadow) + dr * 0.20F), u8((20 + 20 * shadow)), u8((20)), false};
        }
      // Gold trim at the hem.
      if (hemY > 0 && hemY < h) {
        for (int xx = 0; xx < w; ++xx)
          dst[static_cast<std::size_t>(hemY) * w + xx] = Rgb{240, 200, 80, false};
      }
      // Tassels along the hem.
      for (int i = 0; i < 12; ++i) {
        const float tx = w * (0.05F + 0.08F * i);
        if (hemY < h - static_cast<int>(mn * 0.04F))
          drawSeg(dst, w, h, tx, hemY, tx, hemY + mn * 0.04F, std::max(1.0F, mn * 0.003F), ya,
                  Rgb{220, 180, 60, false});
      }
    });
}

void effectPianoKeys(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
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
              Rgb{u8(40 + l * 0.10F), u8(30 + l * 0.08F), u8(40 + l * 0.10F), false};
        }
      const float keyTop = h * 0.70F;
      const float keyBot = h * 0.92F;
      const int nWhite = 14;
      const float keyW = static_cast<float>(w) / nWhite;
      // White keys.
      for (int k = 0; k < nWhite; ++k) {
        const float kx = k * keyW;
        const bool pressed = (hash(k + static_cast<int>(t * 8)) > 0.7F);
        const float kCol = pressed ? 200 : 250;
        for (int yy = static_cast<int>(keyTop); yy <= static_cast<int>(keyBot); ++yy)
          for (int xx = static_cast<int>(kx + 2); xx <= static_cast<int>(kx + keyW - 2); ++xx)
            if (xx >= 0 && xx < w && yy >= 0 && yy < h)
              dst[static_cast<std::size_t>(yy) * w + xx] =
                  Rgb{u8(kCol), u8(kCol), u8(kCol - 20), false};
        // Ripple rising from pressed keys — data-tinted.
        if (pressed) {
          const float rip = std::fmod(t * 4.0F, 1.0F);
          const float rx = kx + keyW * 0.5F;
          const float ry = keyTop - rip * mn * 0.30F;
          const Rgb d = sample(src, w, h, static_cast<int>(rx), static_cast<int>(ry));
          const float dr = d.transparent ? 200.0F : d.r;
          plotDot(dst, w, h, rx, ry, mn * 0.015F * (1 - rip), ya,
                  Rgb{u8(220 + dr * 0.1F), u8(180), u8(120), false});
        }
      }
      // Black keys (between certain whites: 2-3, 4-5, 5-6 etc — simplified
      // every-other-pair pattern).
      for (int k = 0; k < nWhite - 1; ++k) {
        if (k % 7 == 2 || k % 7 == 6) continue;  // no black between B-C and E-F
        for (int yy = static_cast<int>(keyTop); yy <= static_cast<int>(keyTop + (keyBot - keyTop) * 0.6F); ++yy)
          for (int xx = static_cast<int>((k + 1) * keyW - keyW * 0.25F);
               xx <= static_cast<int>((k + 1) * keyW + keyW * 0.25F); ++xx)
            if (xx >= 0 && xx < w && yy >= 0 && yy < h)
              dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{20, 20, 30, false};
      }
    });
}

void effectSheetMusic(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
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
          const float dl = d.transparent ? 200.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(240 + dl * 0.05F), u8(230 + dl * 0.05F), u8(200 + dl * 0.05F), false};
        }
      // Five-line staff.
      const float staffY = h * 0.50F;
      const float ss = mn * 0.025F;
      for (int i = 0; i < 5; ++i)
        drawSeg(dst, w, h, 0, staffY - 2 * ss + i * ss, w, staffY - 2 * ss + i * ss,
                std::max(1.0F, mn * 0.002F), ya, Rgb{20, 20, 20, false});
      // Treble clef at left.
      const float clefX = w * 0.05F;
      for (float yy = staffY - 3 * ss; yy <= staffY + 3 * ss; yy += 0.5F)
        plotDot(dst, w, h, clefX + std::sin((yy - staffY) * 0.5F) * ss * 0.6F, yy,
                std::max(1.0F, mn * 0.003F), ya, Rgb{20, 20, 20, false});
      // Notes flowing across.
      for (int i = 0; i < 12; ++i) {
        const float nx = w * 0.10F + std::fmod((i / 12.0F) * w + t * w * 0.30F + hash(i) * 50, w * 0.9F);
        const float pitchY = staffY + (hash(i * 3) - 0.5F) * ss * 6;
        plotDot(dst, w, h, nx, pitchY, mn * 0.012F, ya, Rgb{20, 20, 20, false});
        drawSeg(dst, w, h, nx + mn * 0.012F, pitchY, nx + mn * 0.012F, pitchY - mn * 0.05F,
                std::max(1.0F, mn * 0.002F), ya, Rgb{20, 20, 20, false});
      }
    });
}

void effectStradivarius(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
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
              Rgb{u8(20 + l * 0.10F), u8(15 + l * 0.05F), u8(10 + l * 0.03F), false};
        }
      const float cx = w * 0.5F, cy = h * 0.55F;
      const float vL = mn * 0.30F;
      // Violin body — two ovals (upper bout, lower bout) + waist.
      auto drawBout = [&](float yc, float rx, float ry) {
        for (int yo = -static_cast<int>(ry); yo <= static_cast<int>(ry); ++yo)
          for (int xo = -static_cast<int>(rx); xo <= static_cast<int>(rx); ++xo) {
            const float nx = xo / rx, nyN = yo / ry;
            if (nx * nx + nyN * nyN > 1.0F) continue;
            const int xx = static_cast<int>(cx + xo), yy = static_cast<int>(yc + yo);
            if (xx < 0 || xx >= w || yy < 0 || yy >= h) continue;
            const Rgb d = sample(src, w, h, xx, yy);
            const float dr = d.transparent ? 180.0F : d.r;
            dst[static_cast<std::size_t>(yy) * w + xx] =
                Rgb{u8(140 + dr * 0.20F), u8(70), u8(30), false};
          }
      };
      drawBout(cy - vL * 0.25F, vL * 0.25F, vL * 0.18F);  // upper bout
      drawBout(cy + vL * 0.25F, vL * 0.30F, vL * 0.20F);  // lower bout
      // f-holes (left + right).
      for (int side = -1; side <= 1; side += 2) {
        for (float yy = cy - vL * 0.12F; yy <= cy + vL * 0.12F; yy += 1.0F) {
          const float curve = std::sin((yy - cy) * 8 / vL) * vL * 0.02F;
          plotDot(dst, w, h, cx + side * vL * 0.08F + curve, yy,
                  std::max(1.0F, mn * 0.002F), ya, Rgb{20, 10, 5, false});
        }
      }
      // Neck.
      drawSeg(dst, w, h, cx, cy - vL * 0.42F, cx, cy - vL * 0.65F,
              std::max(1.0F, mn * 0.014F), ya, Rgb{40, 20, 10, false});
      // Scroll.
      plotDot(dst, w, h, cx, cy - vL * 0.65F, mn * 0.020F, ya, Rgb{60, 30, 15, false});
      // Strings (4) — bow can be inferred.
      for (int s = 0; s < 4; ++s) {
        const float xo = (s - 1.5F) * mn * 0.003F;
        drawSeg(dst, w, h, cx + xo, cy + vL * 0.40F, cx + xo, cy - vL * 0.62F,
                std::max(1.0F, mn * 0.0015F), ya, Rgb{220, 220, 220, false});
      }
      // Bow sweeps in over time.
      const float bowProg = std::clamp(t * 2.0F, 0.0F, 1.0F);
      const float bowX = cx - vL * 0.30F + bowProg * vL * 0.60F;
      drawSeg(dst, w, h, bowX, cy - vL * 0.10F, bowX + vL * 0.50F, cy + vL * 0.10F,
              std::max(1.0F, mn * 0.005F), ya, Rgb{220, 200, 160, false});
      (void)t;
    });
}

void effectTheremin(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
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
              Rgb{u8(30 + l * 0.05F), u8(20 + l * 0.10F), u8(40 + l * 0.15F), false};
        }
      const float boxX = w * 0.50F, boxY = h * 0.70F;
      // Box (data-textured).
      for (int yo = -static_cast<int>(mn * 0.06F); yo <= 0; ++yo)
        for (int xo = -static_cast<int>(mn * 0.12F); xo <= static_cast<int>(mn * 0.12F); ++xo) {
          const int xx = static_cast<int>(boxX + xo);
          const int yy = static_cast<int>(boxY + yo);
          if (xx < 0 || xx >= w || yy < 0 || yy >= h) continue;
          const Rgb d = sample(src, w, h, xx, yy);
          const float dr = d.transparent ? 100.0F : d.r;
          dst[static_cast<std::size_t>(yy) * w + xx] =
              Rgb{u8(120 + dr * 0.20F), u8(80), u8(40), false};
        }
      // Pitch antenna (vertical, right) + volume antenna (horizontal, left).
      drawSeg(dst, w, h, boxX + mn * 0.10F, boxY - mn * 0.06F, boxX + mn * 0.10F, boxY - mn * 0.30F,
              std::max(1.0F, mn * 0.004F), ya, Rgb{220, 220, 220, false});
      drawSeg(dst, w, h, boxX - mn * 0.10F, boxY - mn * 0.03F, boxX - mn * 0.30F, boxY - mn * 0.03F,
              std::max(1.0F, mn * 0.004F), ya, Rgb{220, 220, 220, false});
      // Hand silhouettes hovering.
      plotDot(dst, w, h, boxX + mn * 0.10F + std::sin(t * 3.0F) * mn * 0.03F, boxY - mn * 0.18F,
              mn * 0.025F, ya, Rgb{220, 180, 150, false});
      plotDot(dst, w, h, boxX - mn * 0.20F + std::sin(t * 2.5F) * mn * 0.02F, boxY - mn * 0.03F,
              mn * 0.025F, ya, Rgb{220, 180, 150, false});
      // Sine wave radiating.
      for (float x = 0; x < w; x += 1.0F) {
        const float phase = (x - boxX) * 0.04F - t * 8.0F;
        const float ywv = h * 0.30F + std::sin(phase) * mn * 0.06F;
        plotDot(dst, w, h, x, ywv, std::max(1.0F, mn * 0.003F), ya,
                Rgb{120, 220, 240, false});
      }
    });
}

void effectValkyrieRide(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      // Storm sky.
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
          const float l = s.transparent ? 40.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
          const float sf = static_cast<float>(y) / h;
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(40 + 60 * (1 - sf) + l * 0.10F), u8(30 + 40 * (1 - sf) + l * 0.08F),
                  u8(50 + 50 * (1 - sf) + l * 0.10F), false};
        }
      // Lightning forks.
      const bool flash = std::fmod(t * 3.0F, 1.0F) < 0.1F;
      if (flash) {
        for (int b = 0; b < 2; ++b) {
          float bx = hash(b) * w;
          float by = 0;
          for (int s = 0; s < 6; ++s) {
            const float nx = bx + (hash(b * 7 + s) - 0.5F) * mn * 0.05F;
            const float ny = by + mn * 0.10F;
            drawSeg(dst, w, h, bx, by, nx, ny, std::max(1.0F, mn * 0.003F), ya,
                    Rgb{240, 240, 255, false});
            bx = nx; by = ny;
          }
        }
      }
      // Valkyrie + horse silhouette, charging right.
      const float cx = -mn * 0.20F + t * (w + mn * 0.40F);
      const float cy = h * 0.55F;
      const Rgb body{20, 14, 30, false};
      const float bL = mn * 0.20F;
      // Horse body.
      for (int yo = -static_cast<int>(bL * 0.15F); yo <= static_cast<int>(bL * 0.15F); ++yo)
        for (int xo = -static_cast<int>(bL * 0.45F); xo <= static_cast<int>(bL * 0.30F); ++xo) {
          const float nx = xo / (bL * 0.45F), nyN = yo / (bL * 0.15F);
          if (nx * nx + nyN * nyN > 1.0F) continue;
          const int xx = static_cast<int>(cx + xo), yy = static_cast<int>(cy + yo);
          if (xx >= 0 && xx < w && yy >= 0 && yy < h)
            dst[static_cast<std::size_t>(yy) * w + xx] = body;
        }
      // Horse head + legs.
      drawSeg(dst, w, h, cx + bL * 0.30F, cy - bL * 0.10F, cx + bL * 0.45F, cy - bL * 0.25F,
              std::max(1.0F, mn * 0.012F), ya, body);
      plotDot(dst, w, h, cx + bL * 0.45F, cy - bL * 0.25F, mn * 0.020F, ya, body);
      for (int leg = -2; leg <= 2; leg += 2) {
        const float gallop = std::sin(t * 8.0F + leg) * bL * 0.10F;
        drawSeg(dst, w, h, cx + leg * bL * 0.10F, cy + bL * 0.10F,
                cx + leg * bL * 0.10F + gallop, cy + bL * 0.30F,
                std::max(1.0F, mn * 0.006F), ya, body);
      }
      // Rider with spear + wing.
      drawSeg(dst, w, h, cx, cy - bL * 0.05F, cx, cy - bL * 0.30F, bL * 0.06F, ya, body);
      plotDot(dst, w, h, cx, cy - bL * 0.32F, bL * 0.06F, ya, body);  // helmet
      drawSeg(dst, w, h, cx + bL * 0.04F, cy - bL * 0.20F, cx + bL * 0.30F, cy - bL * 0.50F,
              std::max(1.0F, mn * 0.005F), ya, Rgb{200, 180, 80, false});  // spear
      // Wing on rider (data-textured).
      for (int k = 0; k < 6; ++k) {
        const float kf = k / 5.0F;
        const float wx = cx - bL * 0.10F - kf * bL * 0.30F;
        const float wy = cy - bL * 0.20F - kf * bL * 0.10F;
        drawSeg(dst, w, h, cx, cy - bL * 0.25F, wx, wy, std::max(1.0F, mn * 0.004F), ya,
                Rgb{u8(140), u8(120), u8(180), false});
      }
    });
}

void effectVinylSpin(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
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
              Rgb{u8(30 + l * 0.10F), u8(20 + l * 0.08F), u8(40 + l * 0.10F), false};
        }
      const float cx = w * 0.5F, cy = h * 0.5F;
      const float R = mn * 0.35F;
      // LP disk.
      for (int yy = static_cast<int>(cy - R / ya); yy <= static_cast<int>(cy + R / ya); ++yy)
        for (int xx = static_cast<int>(cx - R); xx <= static_cast<int>(cx + R); ++xx) {
          const float dx = xx - cx, dy = (yy - cy) * ya;
          const float r = std::sqrt(dx * dx + dy * dy);
          if (r > R) continue;
          if (xx < 0 || xx >= w || yy < 0 || yy >= h) continue;
          if (r < R * 0.20F) continue;  // label area, drawn separately
          const float groove = std::sin(r * 0.8F) * 30;
          dst[static_cast<std::size_t>(yy) * w + xx] =
              Rgb{u8(20 + groove * 0.5F), u8(20 + groove * 0.5F), u8(20 + groove * 0.5F), false};
        }
      // Data-textured label (centre).
      drawDataDisk(dst, w, h, src, cx, cy, R * 0.20F, ya, 0.5F, t * 33.0F / 60.0F * 6.2832F,
                   Rgb{200, 60, 60, false});
      // Centre hole.
      plotDot(dst, w, h, cx, cy, R * 0.015F, ya, Rgb{0, 0, 0, false});
      // Tonearm.
      drawSeg(dst, w, h, cx + R * 1.10F, cy - R * 0.80F, cx + R * 0.4F, cy - R * 0.10F,
              std::max(1.0F, mn * 0.005F), ya, Rgb{200, 200, 200, false});
      plotDot(dst, w, h, cx + R * 1.10F, cy - R * 0.80F, mn * 0.015F, ya, Rgb{60, 60, 60, false});
    });
}

}  // namespace ee_detail
}  // namespace Qdless
