#include "QdlessExitEffectCommon.h"

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
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  std::array<Rgb, kN> bones{};
  for (int i = 0; i < kN; ++i)
  {
    const Rgb v = sample(src, w, h, w * ((i + 0.5F) / kN), (h - 1) * 0.5F);
    const float vr = v.transparent ? 200.0F : v.r;
    const float vg = v.transparent ? 200.0F : v.g;
    const float vb = v.transparent ? 200.0F : v.b;
    bones[i] = Rgb{
        u8(228 * 0.7F + vr * 0.3F), u8(226 * 0.7F + vg * 0.3F), u8(208 * 0.7F + vb * 0.3F), false};
  }

  auto skeleton =
      [&](std::vector<Rgb>& dst, float cx, float baseY, float beat, const Rgb& bone, const Rgb& eye)
  {
    const float Hh = bodyH;
    const float bob = std::sin(beat) * Hh * 0.02F;
    const float hy = baseY - 0.46F * Hh - bob;
    const float neckY = baseY - 0.78F * Hh - bob;
    const float skullY = baseY - 0.88F * Hh - bob;
    const float shY = baseY - 0.73F * Hh - bob;
    const float rad = Hh * 0.085F;
    const float bR = std::max(1.0F, Hh * 0.016F);
    drawSeg(dst, w, h, cx, hy, cx, neckY, bR, ya, bone);  // spine
    for (int r = 0; r < 3; ++r)                           // ribs
    {
      const float ry = neckY + (hy - neckY) * (0.28F + 0.22F * r);
      const float rw = Hh * 0.11F * (1.0F - 0.13F * r);
      drawSeg(dst, w, h, cx - rw, ry, cx + rw, ry, bR * 0.7F, ya, bone);
    }
    const float shW = Hh * 0.13F;
    drawSeg(dst, w, h, cx - shW, shY, cx + shW, shY, bR, ya, bone);  // shoulders
    drawSeg(dst, w, h, cx - shW, shY, cx - shW * 0.85F, hy + Hh * 0.02F, bR * 0.9F, ya, bone);
    drawSeg(dst, w, h, cx + shW, shY, cx + shW * 0.85F, hy + Hh * 0.02F, bR * 0.9F, ya, bone);
    drawSeg(dst, w, h, cx - Hh * 0.07F, hy, cx + Hh * 0.07F, hy, bR, ya, bone);  // pelvis
    for (int s = -1; s <= 1; s += 2)                                             // legs
    {
      const float lift = std::max(0.0F, s > 0 ? std::sin(beat) : -std::sin(beat));
      const float hipx = cx + s * Hh * 0.06F;
      const float footx =
          (cx + s * Hh * 0.06F) + (cx + s * Hh * 0.24F - (cx + s * Hh * 0.06F)) * lift;
      const float footy = baseY + ((hy - Hh * 0.30F) - baseY) * lift;
      const float kneex =
          (cx + s * Hh * 0.05F) + (cx + s * Hh * 0.17F - (cx + s * Hh * 0.05F)) * lift;
      const float kneey = ((hy + baseY) * 0.5F) + ((hy - Hh * 0.02F) - (hy + baseY) * 0.5F) * lift;
      drawSeg(dst, w, h, hipx, hy, kneex, kneey, bR, ya, bone);
      drawSeg(dst, w, h, kneex, kneey, footx, footy, bR * 0.9F, ya, bone);
      drawSeg(dst, w, h, footx, footy, footx + s * Hh * 0.05F, footy, bR * 0.8F, ya, bone);
    }
    plotDot(dst, w, h, cx, skullY, rad, ya, bone);  // skull
    plotDot(
        dst, w, h, cx - rad * 0.42F, skullY - rad * 0.05F, std::max(1.0F, rad * 0.26F), ya, eye);
    plotDot(
        dst, w, h, cx + rad * 0.42F, skullY - rad * 0.05F, std::max(1.0F, rad * 0.26F), ya, eye);
    plotDot(dst, w, h, cx, skullY + rad * 0.4F, std::max(1.0F, rad * 0.16F), ya, eye);  // nasal
  };

  runFrames(
      renderer,
      w,
      h,
      5200,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float vf = std::clamp(1.0F - t * 4.5F, 0.0F, 1.0F);  // view -> dark stage
        for (std::size_t k = 0; k < dst.size(); ++k)
        {
          const Rgb& s0 = src[k];
          dst[k] = s0.transparent ? Rgb{0, 0, 0, false}
                                  : Rgb{u8(s0.r * vf), u8(s0.g * vf), u8(s0.b * vf), false};
        }
        const int fy = static_cast<int>(floorY);
        if (fy >= 0 && fy < h)
          for (int x = 0; x < w; ++x)
            dst[static_cast<std::size_t>(fy) * w + x] = Rgb{42, 42, 50, false};  // stage floor
        const float alpha = std::clamp((t - 0.10F) / 0.13F, 0.0F, 1.0F);
        if (alpha <= 0.0F)
          return;
        const float beat = t * 8.0F * speed;  // tempo (low natural beat: fast footwork already)
        for (int i = 0; i < kN; ++i)
        {
          const Rgb b{
              u8(bones[i].r * alpha), u8(bones[i].g * alpha), u8(bones[i].b * alpha), false};
          const Rgb e{u8(18 * alpha), u8(12 * alpha), u8(12 * alpha), false};
          skeleton(dst, w * ((i + 0.5F) / kN), floorY, beat, b, e);
        }
      });
}

void effectThriller(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  constexpr int kN = 5;
  const float floorY = (h - 1) * 0.94F;
  const float H = h * 0.58F;
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer,
            w,
            h,
            5000,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float alpha = skeletonStage(dst, src, w, h, t, static_cast<int>(floorY));
              if (alpha <= 0.0F)
                return;
              const float beat = t * 9.0F * speed;
              const float sway = std::sin(beat);
              const float bob = std::sin(beat * 2.0F) * 0.025F * H;
              const float headDX = sway * 0.05F * H;
              const Limb aL{-0.18F + sway * 0.05F, -0.05F, -0.10F + sway * 0.20F, -0.32F};
              const Limb aR{0.18F + sway * 0.05F, -0.05F, 0.10F + sway * 0.20F, -0.32F};
              const Limb lL{-0.01F + sway * 0.03F, 0.23F, 0.02F + sway * 0.05F, 0.46F};
              const Limb lR{0.01F + sway * 0.03F, 0.23F, -0.02F + sway * 0.05F, 0.46F};
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
                             headDX,
                             aL,
                             aR,
                             lL,
                             lR,
                             boneTint(src, w, h, cx, alpha),
                             Rgb{u8(18 * alpha), u8(12 * alpha), u8(12 * alpha), false});
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
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const Limb aL{-0.30F, -0.06F, -0.55F, -0.06F};  // arms out, linking the line
  const Limb aR{0.30F, -0.06F, 0.55F, -0.06F};
  runFrames(renderer,
            w,
            h,
            5200,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float alpha = skeletonStage(dst, src, w, h, t, static_cast<int>(floorY));
              if (alpha <= 0.0F)
                return;
              const float beat = t * 7.0F * speed;
              const float sway = std::sin(beat);
              const float kickL = std::max(0.0F, -std::sin(beat));
              const float kickR = std::max(0.0F, std::sin(beat));
              const Limb lL =
                  lerpLimb({-0.01F, 0.23F, 0.02F, 0.46F}, {0.10F, 0.10F, 0.22F, 0.16F}, kickL);
              const Limb lR =
                  lerpLimb({0.01F, 0.23F, -0.02F, 0.46F}, {-0.10F, 0.10F, -0.22F, 0.16F}, kickR);
              const float bob = std::fabs(sway) * 0.02F * H;
              const float headDX = sway * 0.03F * H;
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
                             headDX,
                             aL,
                             aR,
                             lL,
                             lR,
                             boneTint(src, w, h, cx, alpha),
                             Rgb{u8(18 * alpha), u8(12 * alpha), u8(12 * alpha), false});
              }
            });
}

void effectRussianDance(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  constexpr int kN = 5;
  const float floorY = (h - 1) * 0.94F;
  const float H = h * 0.58F;
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const Limb aL{0.12F, -0.08F, 0.16F, 0.05F};  // arms folded across the chest
  const Limb aR{-0.12F, -0.08F, -0.16F, 0.05F};
  runFrames(renderer,
            w,
            h,
            5000,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float alpha = skeletonStage(dst, src, w, h, t, static_cast<int>(floorY));
              if (alpha <= 0.0F)
                return;
              const float beat = t * 12.0F * speed;
              const float bob = -0.12F * H + std::sin(beat) * 0.04F * H;  // squat + bounce
              const float kickL = std::max(0.0F, -std::sin(beat));
              const float kickR = std::max(0.0F, std::sin(beat));
              // Cossack squat: the thighs splay out from the hips (the bend is at
              // the hip, not the knee) and the shins drop to the feet; on the beat
              // one leg shoots straight out to the side.
              const Limb lL =
                  lerpLimb({-0.20F, 0.16F, -0.09F, 0.33F}, {-0.16F, 0.06F, -0.55F, 0.06F}, kickL);
              const Limb lR =
                  lerpLimb({0.20F, 0.16F, 0.09F, 0.33F}, {0.16F, 0.06F, 0.55F, 0.06F}, kickR);
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

void effectBallet(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  constexpr int kN = 5;
  const float floorY = (h - 1) * 0.94F;
  const float H = h * 0.58F;
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const Limb lL{0.0F, 0.25F, 0.0F, 0.50F};       // supporting leg, straight on pointe
  const Limb lR{0.16F, 0.12F, 0.02F, 0.26F};     // working leg in passé (foot to knee)
  const Limb aL{-0.14F, -0.02F, -0.02F, 0.10F};  // arms rounded in first position
  const Limb aR{0.14F, -0.02F, 0.02F, 0.10F};
  runFrames(renderer,
            w,
            h,
            4500,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float alpha = skeletonStage(dst, src, w, h, t, static_cast<int>(floorY));
              if (alpha <= 0.0F)
                return;
              const float spin = t * 6.2832F * 1.2F * speed;  // ~6 turns at speed 5
              const float latScale = std::cos(spin);
              const float bob = 0.05F * H;  // relevé, held on pointe
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
                             Rgb{u8(18 * alpha), u8(12 * alpha), u8(12 * alpha), false},
                             latScale);
              }
            });
}

void effectMacarena(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  constexpr int kN = 5;
  const float floorY = (h - 1) * 0.94F;
  const float H = h * 0.58F;
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  // One arm's landmark positions, in order (right arm; the left arm uses the
  // mirror image, one count behind). {elbow.x, elbow.y, hand.x, hand.y} as
  // offsets from the shoulder in H units; +x is this arm's own side.
  static const std::array<Limb, 8> kArm = {{
      {0.18F, -0.02F, 0.42F, -0.02F},   // 0: out to the side, level (palm down)
      {0.18F, -0.02F, 0.42F, -0.02F},   // 1: hold (palm turns up)
      {0.02F, 0.02F, -0.20F, 0.02F},    // 2: hand crosses to the opposite shoulder
      {0.02F, 0.02F, -0.20F, 0.02F},    // 3: hold
      {0.06F, -0.18F, -0.06F, -0.34F},  // 4: hand behind the head
      {0.06F, -0.18F, -0.06F, -0.34F},  // 5: hold
      {-0.02F, 0.16F, -0.12F, 0.40F},   // 6: hand to the opposite hip
      {-0.02F, 0.16F, -0.12F, 0.40F},   // 7: hold
  }};
  auto mirror = [](const Limb& a) { return Limb{-a.ex, a.ey, -a.hx, a.hy}; };
  const Limb lL{-0.01F, 0.23F, 0.02F, 0.46F};
  const Limb lR{0.01F, 0.23F, -0.02F, 0.46F};
  runFrames(renderer,
            w,
            h,
            6000,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float alpha = skeletonStage(dst, src, w, h, t, static_cast<int>(floorY));
              if (alpha <= 0.0F)
                return;
              const float seg = 0.16F / speed;  // per-count hold time
              const float step = t / seg;
              const int idx = static_cast<int>(step) % 8;
              const int prev = (idx + 7) % 8;
              const float blend = std::clamp((step - std::floor(step)) / 0.3F, 0.0F, 1.0F);
              // Right arm steps through kArm; the left arm trails by one count.
              const Limb aR = lerpLimb(kArm[prev], kArm[idx], blend);
              const Limb aL = mirror(lerpLimb(kArm[(prev + 7) % 8], kArm[(idx + 7) % 8], blend));
              // A quarter-turn hop at the end of each eight-count.
              const float cyc = t / (8.0F * seg);
              const float frac = cyc - std::floor(cyc);
              const float jump = std::clamp((frac - 0.85F) / 0.15F, 0.0F, 1.0F);
              const float quarters = std::floor(cyc) + jump;
              // Quarter-turn facing, but floored so a side-on count stays a
              // narrow silhouette instead of collapsing to an invisible line.
              const float c = std::cos(quarters * 1.5708F);
              const float latScale = (c < 0.0F ? -1.0F : 1.0F) * (0.4F + 0.6F * std::fabs(c));
              const float hop = std::sin(jump * 3.14159F) * 0.06F * H;
              const float bob = -hop + std::sin(t * 9.0F * speed) * 0.02F * H;
              const float sway = std::sin(t * 4.5F * speed) * 0.03F * (w / static_cast<float>(kN));
              for (int i = 0; i < kN; ++i)
              {
                const float cx = w * ((i + 0.5F) / kN) + sway;
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
                             Rgb{u8(18 * alpha), u8(12 * alpha), u8(12 * alpha), false},
                             latScale);
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
