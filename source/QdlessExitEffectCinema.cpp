#include "QdlessExitEffectCommon.h"
#include "QdlessMarionette.h"

namespace Qdless
{
namespace ee_detail
{

void effectMatrix(
    const Renderer& renderer, const std::vector<Rgb>& src, int w, int h, std::mt19937& rng)
{
  std::vector<float> delay(w);
  std::vector<float> speed(w);
  std::uniform_real_distribution<float> dDelay(0.0F, 0.45F);
  std::uniform_real_distribution<float> dSpeed(0.9F, 1.5F);
  for (int x = 0; x < w; ++x)
  {
    delay[x] = dDelay(rng);
    speed[x] = dSpeed(rng);
  }
  runFrames(
      renderer,
      w,
      h,
      2400,
      [&](float t, std::vector<Rgb>& dst)
      {
        for (int x = 0; x < w; ++x)
        {
          const float tt = (t - delay[x]) / std::max(0.05F, 1.0F - delay[x]);
          if (tt <= 0.0F)
          {
            // Column hasn't started falling yet: copy it verbatim.
            for (int y = 0; y < h; ++y)
              dst[static_cast<std::size_t>(y) * w + x] = src[static_cast<std::size_t>(y) * w + x];
            continue;
          }
          const int off = static_cast<int>(std::lround(tt * speed[x] * (h + 2)));
          for (int y = 0; y < h; ++y)
          {
            const int sy = y - off;
            if (sy >= 0 && sy < h)
              dst[static_cast<std::size_t>(y) * w + x] = src[static_cast<std::size_t>(sy) * w + x];
          }
        }
      });
}

void effectTrain(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float bodyH = std::max(6.0F, h * 0.34F);
  const float halfLen = bodyH * 1.3F;   // half the boiler+cab length
  const float baseY = (h - 1) * 0.58F;  // wheels sit a little below centre
  const Rgb engine{40, 40, 48, false};
  const Rgb cabCol{90, 35, 35, false};
  const Rgb wheel{150, 150, 160, false};
  const Rgb light{255, 230, 120, false};
  runFrames(
      renderer,
      w,
      h,
      3200,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float cx = -halfLen + t * (w + 2.0F * halfLen);  // train centre
        // Base: blank where the train has passed, frame ahead.
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const std::size_t idx = static_cast<std::size_t>(y) * w + x;
            dst[idx] = (x < cx) ? kBlank : src[idx];
          }
        auto fillRect = [&](float ax, float bx, float ay, float by, const Rgb& c)
        {
          const int rx0 = std::max(0, static_cast<int>(std::lround(ax)));
          const int rx1 = std::min(w - 1, static_cast<int>(std::lround(bx)));
          const int ry0 = std::max(0, static_cast<int>(std::lround(ay)));
          const int ry1 = std::min(h - 1, static_cast<int>(std::lround(by)));
          for (int y = ry0; y <= ry1; ++y)
            for (int x = rx0; x <= rx1; ++x)
              dst[static_cast<std::size_t>(y) * w + x] = c;
        };
        const float top = baseY - bodyH;
        fillRect(cx - halfLen * 0.2F, cx + halfLen, top + bodyH * 0.25F, baseY, engine);  // boiler
        fillRect(cx - halfLen, cx - halfLen * 0.2F, top - bodyH * 0.15F, baseY, cabCol);  // cab
        fillRect(cx + halfLen * 0.55F,
                 cx + halfLen * 0.78F,
                 top - bodyH * 0.45F,
                 top + bodyH * 0.25F,
                 engine);  // smokestack
        plotDot(dst,
                w,
                h,
                cx + halfLen,
                baseY - bodyH * 0.45F,
                std::max(1.0F, bodyH * 0.12F),
                ya,
                light);  // headlight
        const float wr = bodyH * 0.22F;
        plotDot(dst, w, h, cx - halfLen * 0.55F, baseY, wr, ya, wheel);
        plotDot(dst, w, h, cx + halfLen * 0.10F, baseY, wr, ya, wheel);
        plotDot(dst, w, h, cx + halfLen * 0.70F, baseY, wr, ya, wheel);
        // Smoke puffs trailing up and back from the stack, fading.
        for (int p = 0; p < 4; ++p)
        {
          const float fp = static_cast<float>(p);
          const auto g = static_cast<std::uint8_t>(std::lround(150.0F - fp * 30.0F));
          plotDot(dst,
                  w,
                  h,
                  cx + halfLen * 0.66F - fp * bodyH * 0.5F,
                  top - bodyH * 0.5F - fp * bodyH * 0.4F,
                  bodyH * (0.18F + fp * 0.06F),
                  ya,
                  Rgb{g, g, g, false});
        }
      });
}

void effectBond(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float cx = (w - 1) * 0.5F;
  const float cy = (h - 1) * 0.5F;
  const float maxR = std::sqrt(cx * cx + (cy * ya) * (cy * ya));
  const float R = 0.42F * maxR;
  const float ringW = std::max(1.5F, R * 0.06F);
  const Rgb black{0, 0, 0, false};
  runFrames(renderer,
            w,
            h,
            3600,
            [&](float t, std::vector<Rgb>& dst)
            {
              if (t < 0.55F)
              {
                // Barrel iris: frame inside the circle, white rim, black outside.
                for (int y = 0; y < h; ++y)
                  for (int x = 0; x < w; ++x)
                  {
                    const float dx = x - cx;
                    const float dy = (y - cy) * ya;
                    const float r = std::sqrt(dx * dx + dy * dy);
                    const std::size_t idx = static_cast<std::size_t>(y) * w + x;
                    if (r > R + ringW)
                      dst[idx] = black;
                    else if (r > R)
                      dst[idx] = Rgb{235, 235, 235, false};
                    else
                      dst[idx] = src[idx];
                  }
              }
              else if (t < 0.62F)
              {
                // Muzzle flash.
                std::fill(dst.begin(), dst.end(), Rgb{255, 255, 255, false});
              }
              else if (t < 0.9F)
              {
                // Blood washes down the screen from the top.
                const int curtain = static_cast<int>(std::lround((t - 0.62F) / 0.28F * h));
                for (int y = 0; y < h; ++y)
                {
                  const Rgb c = (y < curtain) ? Rgb{150, 0, 0, false} : black;
                  for (int x = 0; x < w; ++x)
                    dst[static_cast<std::size_t>(y) * w + x] = c;
                }
              }
              else
              {
                // Red drains to black.
                const float k = std::max(0.0F, 1.0F - (t - 0.9F) / 0.1F);
                std::fill(dst.begin(),
                          dst.end(),
                          Rgb{static_cast<std::uint8_t>(std::lround(150.0F * k)), 0, 0, false});
              }
            });
}

void effectTearsInRain(
    const Renderer& renderer, const std::vector<Rgb>& src, int w, int h, std::mt19937& rng)
{
  const float ya = yAspectFor(renderer);
  const float cx = (w - 1) * 0.5F;
  const float cy = (h - 1) * 0.5F;
  auto luma = [](const Rgb& c) { return 30 * c.r + 59 * c.g + 11 * c.b; };

  struct Drop
  {
    float x, speed, len, phase;
  };
  const int nRain = std::max(24, w / 3);
  std::vector<Drop> rain(static_cast<std::size_t>(nRain));
  std::uniform_real_distribution<float> dX(0.0F, static_cast<float>(w - 1));
  std::uniform_real_distribution<float> dSpeed(0.8F, 1.6F);
  std::uniform_real_distribution<float> dLen(h * 0.10F, h * 0.30F);
  std::uniform_real_distribution<float> dPhase(0.0F, 1.0F);
  for (auto& d : rain)
    d = Drop{dX(rng), dSpeed(rng), dLen(rng), dPhase(rng)};
  const Rgb rainCol{150, 175, 205, false};

  runFrames(
      renderer,
      w,
      h,
      10000,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float fade = std::max(0.0F, 1.0F - t);                   // the moments dim
        const int off = static_cast<int>(std::lround(t * h * 0.45F));  // and sink away
        // Weeping content: sink + fade, colours running downward in streaks.
        for (int x = 0; x < w; ++x)
        {
          Rgb carried = kBlank;
          for (int y = 0; y < h; ++y)
          {
            const int sy = y - off;
            Rgb s = (sy >= 0 && sy < h) ? src[static_cast<std::size_t>(sy) * w + x] : kBlank;
            if (!s.transparent)
              s = Rgb{static_cast<std::uint8_t>(s.r * fade),
                      static_cast<std::uint8_t>(s.g * fade),
                      static_cast<std::uint8_t>(s.b * fade),
                      false};
            if (!s.transparent && luma(s) > luma(carried))
              carried = s;
            else
              carried = Rgb{static_cast<std::uint8_t>(carried.r * 0.82F),
                            static_cast<std::uint8_t>(carried.g * 0.82F),
                            static_cast<std::uint8_t>(carried.b * 0.82F),
                            carried.transparent};
            dst[static_cast<std::size_t>(y) * w + x] = carried;
          }
        }
        // Rain, fading out over the last 40%.
        const float rainFade = std::max(0.0F, 1.0F - std::max(0.0F, (t - 0.6F) / 0.4F));
        for (const auto& d : rain)
        {
          // ×6 so each drop falls several times across the long run.
          const float head = std::fmod(d.phase + t * d.speed * 6.0F, 1.0F) * (h + d.len) - d.len;
          const int len = static_cast<int>(d.len);
          const int xx = static_cast<int>(d.x);
          for (int k = 0; k < len; ++k)
          {
            const int yy = static_cast<int>(head) - k;
            if (yy < 0 || yy >= h)
              continue;
            const float b = (1.0F - static_cast<float>(k) / static_cast<float>(len)) * rainFade;
            dst[static_cast<std::size_t>(yy) * w + xx] =
                Rgb{static_cast<std::uint8_t>(rainCol.r * b),
                    static_cast<std::uint8_t>(rainCol.g * b),
                    static_cast<std::uint8_t>(rainCol.b * b),
                    false};
          }
        }
        // A white dove lifts off and ascends in the final stretch.
        if (t > 0.5F)
        {
          const float td = (t - 0.5F) / 0.5F;                    // 0..1
          const float dcy = cy - td * (cy + h * 0.2F);           // rises off the top
          const float flap = std::sin(t * 70.0F) * (h * 0.05F);  // ~11 wingbeats
          const float span = std::max(3.0F, w * 0.05F);
          const float rise = h * 0.06F;
          const float df = std::max(0.0F, 1.0F - td);
          const Rgb dove{static_cast<std::uint8_t>(235 * df),
                         static_cast<std::uint8_t>(235 * df),
                         static_cast<std::uint8_t>(245 * df),
                         false};
          for (int s = -1; s <= 1; s += 2)
            for (float u = 0.0F; u <= 1.0F; u += 0.1F)
              plotDot(dst, w, h, cx + s * span * u, dcy - (rise + flap) * u, 1.2F, ya, dove);
        }
      });
}

void effectKoyaanisqatsi(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float cx = (w - 1) * 0.5F;
  const float cy = (h - 1) * 0.5F;
  runFrames(renderer,
            w,
            h,
            3200,
            [&](float t, std::vector<Rgb>& dst)
            {
              // Tilt: a wobble whose amplitude and frequency grow, then a
              // topple into a full accelerating spin past t=0.65.
              float angle = std::sin(t * (6.0F + 30.0F * t)) * (0.6F * t);
              if (t > 0.65F)
              {
                const float tb = (t - 0.65F) / 0.35F;
                angle += tb * tb * 7.0F;
              }
              const float fade = t < 0.7F ? 1.0F : std::max(0.0F, 1.0F - (t - 0.7F) / 0.3F);
              const float ca = std::cos(angle);
              const float sa = std::sin(angle);
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  // Inverse rotation about the centre (aspect-correct).
                  const float dx = x - cx;
                  const float dy = (y - cy) * ya;
                  const float rx = dx * ca + dy * sa;
                  const float ry = -dx * sa + dy * ca;
                  const Rgb s = sample(src, w, h, cx + rx, cy + ry / ya);
                  if (s.transparent)
                    continue;
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{static_cast<std::uint8_t>(s.r * fade),
                          static_cast<std::uint8_t>(s.g * fade),
                          static_cast<std::uint8_t>(s.b * fade),
                          false};
                }
            });
}

void effectRosebud(
    const Renderer& renderer, const std::vector<Rgb>& src, int w, int h, std::mt19937& rng)
{
  const float ya = yAspectFor(renderer);
  const float cx = (w - 1) * 0.5F;
  const float cy = (h - 1) * 0.5F;
  const float maxR = std::sqrt(cx * cx + (cy * ya) * (cy * ya));
  const auto word = wordTargets("ROSEBUD", w, h, 0.7F, 0.26F);
  struct Flake
  {
    float x, speed, phase;
  };
  constexpr int kSnow = 60;
  std::vector<Flake> snow(kSnow);
  std::uniform_real_distribution<float> dX(0.0F, static_cast<float>(w));
  std::uniform_real_distribution<float> dSpeed(0.5F, 1.2F);
  std::uniform_real_distribution<float> dPhase(0.0F, 1.0F);
  for (auto& f : snow)
    f = Flake{dX(rng), dSpeed(rng), dPhase(rng)};

  runFrames(
      renderer,
      w,
      h,
      5000,
      [&](float t, std::vector<Rgb>& dst)
      {
        // Dim the frame and close a soft vignette from the edges.
        const float dim = std::max(0.0F, 1.0F - t * 1.1F);
        const float vig = maxR * (1.0F - 0.85F * t);
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& c = src[static_cast<std::size_t>(y) * w + x];
            if (c.transparent)
              continue;
            const float dx = x - cx;
            const float dy = (y - cy) * ya;
            const float r = std::sqrt(dx * dx + dy * dy);
            float k = dim;
            if (r > vig)
              k *= std::max(0.0F, 1.0F - (r - vig) / (maxR * 0.35F));
            if (k <= 0.0F)
              continue;
            dst[static_cast<std::size_t>(y) * w + x] = Rgb{static_cast<std::uint8_t>(c.r * k),
                                                           static_cast<std::uint8_t>(c.g * k),
                                                           static_cast<std::uint8_t>(c.b * k),
                                                           false};
          }
        // Snow drifting down (fades out near the end).
        const float snowFade = std::max(0.0F, 1.0F - std::max(0.0F, (t - 0.7F) / 0.3F));
        for (const auto& f : snow)
        {
          const float yy = std::fmod(f.phase + t * f.speed * 2.0F, 1.0F) * h;
          const float xx = f.x + 3.0F * std::sin((yy + f.phase * 50.0F) * 0.1F);
          const int ix = (static_cast<int>(xx) % w + w) % w;
          const int iy = static_cast<int>(yy);
          if (iy >= 0 && iy < h)
          {
            const auto v = static_cast<std::uint8_t>(std::lround(210.0F * snowFade));
            dst[static_cast<std::size_t>(iy) * w + ix] = Rgb{v, v, v, false};
          }
        }
        // ROSEBUD glows in, holds, then fades with the last breath.
        float wb = (t > 0.25F) ? std::min(1.0F, (t - 0.25F) / 0.2F) : 0.0F;
        if (t > 0.75F)
          wb *= std::max(0.0F, 1.0F - (t - 0.75F) / 0.25F);
        if (wb > 0.0F)
        {
          const auto v = static_cast<std::uint8_t>(std::lround(235.0F * wb));
          const Rgb glow{
              v, static_cast<std::uint8_t>(v * 0.85F), static_cast<std::uint8_t>(v * 0.7F), false};
          for (const auto& [x, y] : word)
            dst[static_cast<std::size_t>(y) * w + x] = glow;
        }
      });
}

void effectHal9000(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float cx = (w - 1) * 0.5F;
  const float cy = (h - 1) * 0.5F;
  const float R = std::min(0.40F * static_cast<float>(h), 0.45F * static_cast<float>(w));
  runFrames(renderer,
            w,
            h,
            5500,
            [&](float t, std::vector<Rgb>& dst)
            {
              // Phase A: implode the captured frame toward the centre, fading.
              if (t < 0.32F)
              {
                const float s = std::max(0.0F, 1.0F - t / 0.32F);
                const float fa = 1.0F - t / 0.32F;
                for (int y = 0; y < h; ++y)
                  for (int x = 0; x < w; ++x)
                  {
                    const Rgb& c = src[static_cast<std::size_t>(y) * w + x];
                    if (c.transparent)
                      continue;
                    const int nx = static_cast<int>(std::lround(cx + (x - cx) * s));
                    const int ny = static_cast<int>(std::lround(cy + (y - cy) * s));
                    if (nx >= 0 && nx < w && ny >= 0 && ny < h)
                      dst[static_cast<std::size_t>(ny) * w + nx] =
                          Rgb{static_cast<std::uint8_t>(c.r * fa),
                              static_cast<std::uint8_t>(c.g * fa),
                              static_cast<std::uint8_t>(c.b * fa),
                              false};
                  }
              }
              // Eye brightness: fade in, hold, then the slow deactivation dim.
              float b = 0.0F;
              if (t >= 0.22F && t < 0.34F)
                b = (t - 0.22F) / 0.12F;
              else if (t >= 0.34F && t < 0.62F)
                b = 1.0F;
              else if (t >= 0.62F && t < 0.90F)
                b = 1.0F - (t - 0.62F) / 0.28F;
              if (b > 0.0F)
              {
                for (int y = 0; y < h; ++y)
                  for (int x = 0; x < w; ++x)
                  {
                    const float dx = x - cx;
                    const float dy = (y - cy) * ya;
                    const float dist = std::sqrt(dx * dx + dy * dy);
                    if (dist > R)
                      continue;
                    Rgb c;
                    if (dist < 0.16F * R)
                      c = Rgb{255, 210, 140, false};  // hot lens centre
                    else
                    {
                      const float q = 1.0F - dist / R;  // 0 edge .. 1 centre
                      c = Rgb{static_cast<std::uint8_t>(std::lround(60.0F + 195.0F * q)),
                              static_cast<std::uint8_t>(std::lround(30.0F * q * q)),
                              0,
                              false};
                    }
                    dst[static_cast<std::size_t>(y) * w + x] =
                        Rgb{static_cast<std::uint8_t>(c.r * b),
                            static_cast<std::uint8_t>(c.g * b),
                            static_cast<std::uint8_t>(c.b * b),
                            false};
                  }
              }
              // A last hot pinpoint, winking out.
              if (t >= 0.88F && t < 0.98F)
              {
                const float pf = 1.0F - (t - 0.88F) / 0.10F;
                const auto v = static_cast<std::uint8_t>(std::lround(255.0F * pf));
                plotDot(dst,
                        w,
                        h,
                        cx,
                        cy,
                        std::max(1.0F, R * 0.06F),
                        ya,
                        Rgb{v,
                            static_cast<std::uint8_t>(v * 0.8F),
                            static_cast<std::uint8_t>(v * 0.5F),
                            false});
              }
            });
}

void effectStarWars(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  int bw = 0;
  int bh = 0;
  const std::vector<char> text = buildTextBitmap({"THE END"}, bw, bh);
  const Rgb yellow{255, 213, 60, false};
  runFrames(renderer,
            w,
            h,
            4200,
            [&](float t, std::vector<Rgb>& dst)
            {
              // Background: the coloured view fading to black.
              const float fade = std::max(0.0F, 1.0F - t * 1.1F);
              for (std::size_t i = 0; i < dst.size(); ++i)
              {
                const Rgb& s0 = src[i];
                dst[i] = s0.transparent ? Rgb{0, 0, 0, false}
                                        : Rgb{static_cast<std::uint8_t>(s0.r * fade),
                                              static_cast<std::uint8_t>(s0.g * fade),
                                              static_cast<std::uint8_t>(s0.b * fade),
                                              false};
              }
              // "THE END" recedes from just below the screen to far away, dimming
              // as it goes and fading out entirely over the last sixth.
              const float scrollFront = 0.5F + t * 7.0F;
              const float gFade = std::clamp(1.0F - (t - 0.84F) / 0.16F, 0.0F, 1.0F);
              drawCrawl(dst, w, h, text, bw, bh, scrollFront, yellow, gFade, 0.75F);
            });
}

void effectDoves(
    const Renderer& renderer, const std::vector<Rgb>& src, int w, int h, std::mt19937& rng)
{
  const float ya = yAspectFor(renderer);
  constexpr int kN = 40;
  struct Bird
  {
    float sx;
    float sy;
    float ang;       // heading, radians from straight up (+ = toward +x)
    float spd;       // screen-heights travelled over the run
    float phase;     // wingbeat phase
    float span;      // half-wingspan
    float baseRise;  // nominal wing height
    float launch;    // take-off time in [0,1]
  };
  std::uniform_real_distribution<float> ux(0.05F, 0.95F);
  std::uniform_real_distribution<float> uy(0.35F, 0.95F);
  std::uniform_real_distribution<float> uang(-1.15F, 1.15F);  // up to ~66 deg either side
  std::uniform_real_distribution<float> uspd(1.0F, 2.0F);
  std::uniform_real_distribution<float> uph(0.0F, 6.2832F);
  std::uniform_real_distribution<float> usz(0.7F, 1.3F);
  std::uniform_real_distribution<float> ulaunch(0.0F, 0.35F);
  const float baseSpan = std::max(4.0F, w * 0.045F);
  std::array<Bird, kN> birds{};
  for (auto& b : birds)
  {
    b.sx = ux(rng) * w;
    b.sy = uy(rng) * h;
    b.ang = uang(rng);
    b.spd = uspd(rng);
    b.phase = uph(rng);
    b.span = baseSpan * usz(rng);
    b.baseRise = b.span * 1.0F;  // tall chevron so the wingbeat reads clearly
    b.launch = ulaunch(rng);
  }
  const Rgb perched{200, 200, 210, false};
  const Rgb flying{240, 240, 248, false};
  runFrames(renderer,
            w,
            h,
            3000,
            [&](float t, std::vector<Rgb>& dst)
            {
              // The view fades to black as the flock lifts off.
              const float fade = std::clamp(1.0F - t * 1.3F, 0.0F, 1.0F);
              for (std::size_t i = 0; i < dst.size(); ++i)
              {
                const Rgb& s0 = src[i];
                dst[i] = s0.transparent ? Rgb{0, 0, 0, false}
                                        : Rgb{static_cast<std::uint8_t>(s0.r * fade),
                                              static_cast<std::uint8_t>(s0.g * fade),
                                              static_cast<std::uint8_t>(s0.b * fade),
                                              false};
              }
              for (const auto& b : birds)
              {
                if (t < b.launch)
                {
                  // Perched, wings folded, waiting to startle.
                  drawDove(dst, w, h, b.sx, b.sy, b.span, b.baseRise * 0.35F, b.ang, ya, perched);
                  continue;
                }
                const float tt = t - b.launch;
                const float dist = b.spd * tt * h;
                const float px = b.sx + std::sin(b.ang) * dist;
                const float py = b.sy - std::cos(b.ang) * dist;  // climbs
                // A big wingbeat: the wings sweep from nearly folded flat all the
                // way up, like the lone dove in "tears in rain".
                const float beat = std::sin(tt * 38.0F + b.phase);
                const float rise = b.baseRise * (0.55F + 0.55F * beat);
                drawDove(dst, w, h, px, py, b.span, rise, b.ang, ya, flying);
              }
            });
}

void effectThanos(
    const Renderer& renderer, const std::vector<Rgb>& src, int w, int h, std::mt19937& rng)
{
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  std::vector<float> td(static_cast<std::size_t>(w) * h);
  std::vector<float> jt(td.size());
  std::uniform_real_distribution<float> u01(0.0F, 1.0F);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
    {
      const std::size_t i = static_cast<std::size_t>(y) * w + x;
      const float sweep = (static_cast<float>(x) / w + static_cast<float>(y) / h) * 0.5F;
      td[i] = sweep * 0.55F + u01(rng) * 0.18F;  // dissolve start
      jt[i] = u01(rng);
    }
  constexpr float life = 0.30F;
  runFrames(renderer,
            w,
            h,
            3000,
            [&](float t, std::vector<Rgb>& dst)
            {
              std::fill(dst.begin(), dst.end(), Rgb{0, 0, 0, false});
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const std::size_t i = static_cast<std::size_t>(y) * w + x;
                  const Rgb& s0 = src[i];
                  if (s0.transparent)
                    continue;
                  const float age = t - td[i];
                  if (age < 0.0F)
                  {
                    dst[i] = s0;  // not yet dust
                    continue;
                  }
                  if (age >= life)
                    continue;  // gone
                  const float k = age / life;
                  const float drift = k * k;
                  const float nx = x + 0.30F * w * drift + (jt[i] - 0.5F) * 0.12F * w * drift;
                  const float ny = y - 0.16F * h * drift +
                                   std::sin(jt[i] * 6.2832F + t * 5.0F) * 0.04F * h * drift;
                  const int ix = static_cast<int>(std::lround(nx));
                  const int iy = static_cast<int>(std::lround(ny));
                  if (ix < 0 || ix >= w || iy < 0 || iy >= h)
                    continue;
                  const float fade = 1.0F - k;
                  const Rgb c{u8((s0.r + (150 - s0.r) * k) * fade),
                              u8((s0.g + (150 - s0.g) * k) * fade),
                              u8((s0.b + (155 - s0.b) * k) * fade),
                              false};
                  dst[static_cast<std::size_t>(iy) * w + ix] = c;
                }
            });
}

void effectInterstellar(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float cx = (w - 1) * 0.5F;
  const float cy = (h - 1) * 0.5F;
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float rs = 0.15F * mn;   // shadow radius
  const float diskTilt = 0.20F;  // near edge-on
  const float rDin = 1.35F * rs;
  const float rDout = 2.9F * rs;
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(
      renderer,
      w,
      h,
      5200,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float appear = std::clamp(t / 0.22F, 0.0F, 1.0F);  // the hole forms from the view
        const float spin = t * 1.4F;
        const float rsN = rs * appear;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const std::size_t i = static_cast<std::size_t>(y) * w + x;
            const float dx = x - cx;
            const float dy = (y - cy) * ya;
            const float r = std::sqrt(dx * dx + dy * dy);
            const float ang = std::atan2(dy, dx);
            // Lensed background: deflect inward near the hole, then dim to space.
            const float defl = appear * 0.85F * rs * rs / std::max(r, 0.4F * rs);
            const float sr = r + defl;
            const Rgb bg = sample(src, w, h, cx + sr * std::cos(ang), cy + sr * std::sin(ang) / ya);
            const float sky = 1.0F - 0.7F * appear;
            float gr = (bg.transparent ? 0.0F : bg.r) * sky;
            float gg = (bg.transparent ? 0.0F : bg.g) * sky;
            float gb = (bg.transparent ? 0.0F : bg.b) * sky;
            const bool inShadow = r < rsN;
            // Tilted edge-on accretion disk.
            float diskAmt = 0.0F;
            float dopp = 1.0F;
            const float planeZ = dy / diskTilt;
            const float rhoD = std::sqrt(dx * dx + planeZ * planeZ);
            if (rhoD > rDin && rhoD < rDout)
            {
              const float edge = std::min(1.0F, std::min(rhoD - rDin, rDout - rhoD) / (0.5F * rs));
              const float az = std::atan2(planeZ, dx) + spin;
              diskAmt = edge * (0.55F + 0.45F * std::sin(rhoD * 0.7F));
              dopp = 0.55F + 0.65F * std::cos(az);  // approaching side brighter
            }
            const bool frontDisk = diskAmt > 0.05F && dy > 0.0F;
            // Warm lensed halo hugging the shadow.
            const float he = (r - 1.5F * rs) / (0.5F * rs);
            const float halo = std::exp(-he * he);
            constexpr float wr = 255.0F;
            constexpr float wg = 176.0F;
            constexpr float wb = 92.0F;
            if (inShadow && !frontDisk)
            {
              gr = gg = gb = 0.0F;  // the shadow
            }
            if (!inShadow)
            {
              const float hh = halo * appear * 0.9F;
              gr += wr * hh;
              gg += wg * hh;
              gb += wb * hh;
            }
            if (frontDisk || (diskAmt > 0.05F && !inShadow))
            {
              const float dd = diskAmt * appear * dopp * 1.1F;
              gr += wr * dd;
              gg += wg * dd * 0.95F;
              gb += wb * dd * 0.85F;
            }
            const float pe = (r - rsN * 1.04F) / (0.07F * rs + 1.0F);
            const float pr = std::exp(-pe * pe) * appear;  // photon ring
            gr += 255.0F * pr;
            gg += 235.0F * pr;
            gb += 210.0F * pr;
            dst[i] = Rgb{u8(gr), u8(gg), u8(gb), false};
          }
      });
}

// Muybridge gallery: 4×4 grid of his most famous motion studies, each
// playing simultaneously as a rotoscoped silhouette over the dimmed
// weather data behind. Twelve human studies and four animal ones (the
// galloping horse Sallie Gardner gets pride of place because she was the
// very first rotoscoped subject in 1878). Sprites are loaded from
// data/muybridge/<motion>/frame_NN.png — generated offline by
// scripts/gif2muybridge.py from the original Wikimedia plates.
void effectMuybridge(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto motions = loadAllMuybridgeMotions();

  // Tiny left-aligned 5×7 text renderer for the captions. Each pixel is a
  // 1-cell block at the integer (x,y) the user asks for.
  auto drawText = [&](std::vector<Rgb>& dst, const char* s, float px, float py, Rgb color)
  {
    constexpr int kFW = 5, kFH = 7, kGap = 1;
    int x0 = static_cast<int>(std::round(px));
    const int y0 = static_cast<int>(std::round(py));
    for (const char* p = s; *p; ++p)
    {
      const auto g = glyph5x7(static_cast<char>(std::toupper(static_cast<unsigned char>(*p))));
      for (int fy = 0; fy < kFH; ++fy)
        for (int fx = 0; fx < kFW; ++fx)
        {
          if (g[fy][fx] != '1')
            continue;
          const int xx = x0 + fx;
          const int yy = y0 + fy;
          if (xx >= 0 && xx < w && yy >= 0 && yy < h)
            dst[static_cast<std::size_t>(yy) * w + xx] = color;
        }
      x0 += kFW + kGap;
    }
  };

  // 4×4 grid covers exactly the 16 motion slots.
  constexpr int cols = 4;
  constexpr int rows = 4;
  const float cellW = static_cast<float>(w) / cols;
  const float cellH = static_cast<float>(h) / rows;
  // Sprite height = 78% of cell, leaving room for a caption strip.
  const float spriteH = cellH * 0.78F;

  runFrames(
      renderer,
      w,
      h,
      6400,
      [&](float t, std::vector<Rgb>& dst)
      {
        // Sepia-dimmed data backdrop with thin black dividing lines so each
        // cell reads as its own frame, scrapbook-style.
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 40.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(180 + l * 0.18F), u8(155 + l * 0.16F), u8(118 + l * 0.14F), false};
          }
        // Dividing lines.
        const Rgb divCol{40, 28, 18, false};
        for (int c = 1; c < cols; ++c)
        {
          const int xx = static_cast<int>(c * cellW);
          for (int yy = 0; yy < h; ++yy)
            dst[static_cast<std::size_t>(yy) * w + xx] = divCol;
        }
        for (int r = 1; r < rows; ++r)
        {
          const int yy = static_cast<int>(r * cellH);
          for (int xx = 0; xx < w; ++xx)
            dst[static_cast<std::size_t>(yy) * w + xx] = divCol;
        }

        // Every motion advances at its own speed proportional to its frame
        // count — slower motions (more frames) and faster ones (fewer
        // frames) all roughly cycle ~2× over the effect's lifetime.
        for (int idx = 0; idx < kMuybridgeMotionCount; ++idx)
        {
          const int r = idx / cols;
          const int c = idx % cols;
          const float cx = (c + 0.5F) * cellW;
          const float cy = (r + 0.5F) * cellH - cellH * 0.05F;

          const auto& m = motions[idx];
          if (m.frames.empty())
            continue;
          const int nf = static_cast<int>(m.frames.size());
          // Cycle ~2.2 times over the runtime; offset each motion by its
          // index so they don't all hit the same gait phase together.
          const float phase = t * 2.2F + idx * 0.13F;
          const int fi =
              static_cast<int>(std::floor(phase * nf)) %
              std::max(1, nf);

          drawMuybridgeFrame(dst, w, h, ya, m, fi, cx, cy, spriteH,
                             Rgb{18, 14, 10, false});

          // Caption strip at the bottom of the cell with the motion label.
          const char* label = kMuybridgeLabels[idx];
          const std::size_t llen = std::strlen(label);
          const float capW = llen * 6.0F;  // 5px glyph + 1px gap
          const float capX = cx - capW * 0.5F;
          const float capY = (r + 1) * cellH - cellH * 0.12F;
          drawText(dst, label, capX, capY, Rgb{30, 20, 10, false});
        }

        // Title strip at top.
        const char* title = "MUYBRIDGE - ANIMAL LOCOMOTION 1887";
        const std::size_t tlen = std::strlen(title);
        const float tw = tlen * 6.0F;
        drawText(dst, title, w * 0.5F - tw * 0.5F, 4.0F, Rgb{15, 8, 4, false});
      });
}

void effectNeo(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float floorY = (h - 1) * 0.96F;
  const float H = h * 0.86F;  // one large figure, near full height
  const float cx = w * 0.5F;
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  // Deep, low stance: both knees splay strongly outward — past the feet — and
  // drop low so he is almost kneeling; feet stay planted at the floor (the deep
  // kneel itself comes from the large negative bob below). Knee is the widest
  // point of each leg; the shin angles back inward to the planted foot.
  const Limb lR{0.40F, 0.14F, 0.30F, 0.24F};    // front leg: knee out to the right
  const Limb lL{-0.42F, 0.16F, -0.36F, 0.24F};  // rear leg: knee out to the left, pushed back
  // Near (right) arm points at the opponent — long reach, angled slightly down
  // so it reads apart from the shoulder line; far (left) arm raised up and out,
  // clear of the head.
  const Limb aR{0.22F, 0.04F, 0.62F, 0.08F};
  const Limb aL{-0.16F, -0.16F, -0.30F, -0.42F};
  runFrames(
      renderer,
      w,
      h,
      5200,
      [&](float t, std::vector<Rgb>& dst)
      {
        // Dim the view almost to black for the void behind the rain.
        const float vf = std::clamp(1.0F - t * 4.5F, 0.0F, 1.0F) * 0.25F;
        for (std::size_t k = 0; k < dst.size(); ++k)
        {
          const Rgb& s0 = src[k];
          dst[k] = s0.transparent ? Rgb{0, 0, 0, false}
                                  : Rgb{u8(s0.r * vf), u8(s0.g * vf), u8(s0.b * vf), false};
        }
        // Green digital rain: one stream every other sub-column, each with its
        // own phase and speed, a bright head and a fading tail.
        const float tail = h * 0.28F;
        for (int xc = 0; xc < w; xc += 2)
        {
          const float ph = hash(xc * 3 + 1);
          const float spd = 1.4F + hash(xc * 7 + 5) * 2.2F;
          const float headY = std::fmod(ph + t * spd, 1.0F) * (h + tail) - tail;
          for (int d = 0; d < static_cast<int>(tail); ++d)
          {
            const int y = static_cast<int>(headY) - d;
            if (y < 0 || y >= h)
              continue;
            const float in = 1.0F - d / tail;
            const Rgb c = (d == 0) ? Rgb{180, 255, 190, false}
                                   : Rgb{u8(20 * in), u8(70 + 175 * in), u8(40 * in), false};
            dst[static_cast<std::size_t>(y) * w + xc] = c;
          }
        }
        // Faint dojo floor.
        const int fy = static_cast<int>(floorY);
        if (fy >= 0 && fy < h)
          for (int x = 0; x < w; ++x)
            dst[static_cast<std::size_t>(fy) * w + x] = Rgb{0, 60, 24, false};
        const float alpha = std::clamp((t - 0.10F) / 0.13F, 0.0F, 1.0F);
        if (alpha <= 0.0F)
          return;
        // Turn side-on to the opponent as the stance is taken, then hold.
        const float turn = std::clamp((t - 0.15F) / 0.20F, 0.0F, 1.0F);
        const float latScale = 1.0F - 0.42F * turn;  // 1.0 facing us -> 0.58 strongly side-on
        // A slight kneel plus quiet breathing — no other motion.
        const float bob = -0.22F * H + std::sin(t * 6.0F) * 0.008F * H;  // deep kneel + breathing
        const Rgb bone = boneTint(src, w, h, cx, alpha);
        const Rgb eye{u8(40 * alpha), u8(255 * alpha), u8(80 * alpha), false};  // Matrix-green
        drawSkeleton(dst, w, h, ya, cx, floorY, H, bob, 0.0F, aL, aR, lL, lR, bone, eye, latScale);
      });
}

void effectTruman(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float doorCx = w * 0.66F;
  const float doorSill = h * 0.44F;  // top of the stairs == door base
  const float doorW = std::max(6.0F, w * 0.14F);
  const float doorH = std::max(8.0F, h * 0.22F);
  const float dtop = doorSill - doorH;
  const float stairX0 = w * 0.15F, stairY0 = h * 0.95F;  // foot of the stairs
  const float stairX1 = doorCx - doorW * 0.5F;
  constexpr int kSteps = 7;
  const int txtScale = std::max(1, static_cast<int>(std::lround(w / 130.0F)));
  runFrames(
      renderer,
      w,
      h,
      5800,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float dim = std::clamp(1.0F - t * 1.6F, 0.20F, 1.0F);
        for (int y = 0; y < h; ++y)
        {
          const bool sea = y > h * 0.62F;
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float b = s.transparent ? 90.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            const float l = b * dim;
            dst[static_cast<std::size_t>(y) * w + x] =
                sea ? Rgb{u8(8 + l * 0.10F), u8(26 + l * 0.16F), u8(52 + l * 0.20F), false}
                    : Rgb{u8(70 + l * 0.18F), u8(118 + l * 0.20F), u8(178 + l * 0.20F), false};
          }
        }
        auto fillRect = [&](float x0, float y0, float x1, float y1, const Rgb& c)
        {
          for (int y = std::max(0, static_cast<int>(y0));
               y <= std::min(h - 1, static_cast<int>(y1));
               ++y)
            for (int x = std::max(0, static_cast<int>(x0));
                 x <= std::min(w - 1, static_cast<int>(x1));
                 ++x)
              dst[static_cast<std::size_t>(y) * w + x] = c;
        };
        auto fillRectData = [&](float x0, float y0, float x1, float y1, const Rgb& base, float kBase)
        {
          for (int yy = std::max(0, static_cast<int>(y0));
               yy <= std::min(h - 1, static_cast<int>(y1));
               ++yy)
            for (int xx = std::max(0, static_cast<int>(x0));
                 xx <= std::min(w - 1, static_cast<int>(x1));
                 ++xx)
            {
              const Rgb d = sample(src, w, h, xx, yy);
              const float dr = d.transparent ? 120.0F : d.r;
              const float dg = d.transparent ? 120.0F : d.g;
              const float db = d.transparent ? 120.0F : d.b;
              dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{u8(base.r * kBase + dr * (1 - kBase)),
                                                               u8(base.g * kBase + dg * (1 - kBase)),
                                                               u8(base.b * kBase + db * (1 - kBase)),
                                                               false};
            }
        };
        for (int i = 0; i < kSteps; ++i)  // staircase — risers + treads wear the data
        {
          const float f0 = static_cast<float>(i) / kSteps;
          const float f1 = static_cast<float>(i + 1) / kSteps;
          const float sx = stairX0 + (stairX1 - stairX0) * f0;
          const float sx2 = stairX0 + (stairX1 - stairX0) * f1;
          const float sy = stairY0 + (doorSill - stairY0) * f1;
          fillRectData(sx, sy, sx2 + 1, stairY0, Rgb{150, 146, 140, false}, 0.62F);
          fillRectData(
              sx, sy, sx2 + 1, sy + std::max(1.0F, h * 0.012F), Rgb{212, 208, 198, false}, 0.72F);
        }
        const float open = std::clamp((t - 0.58F) / 0.16F, 0.0F, 1.0F);
        const float dl = doorCx - doorW * 0.5F, dr = doorCx + doorW * 0.5F;
        fillRect(dl - 2, dtop - 2, dr + 2, doorSill, Rgb{236, 232, 220, false});  // lit frame
        fillRect(dl,
                 dtop,
                 dr,
                 doorSill,
                 Rgb{u8(118 * (1 - open)), u8(150 * (1 - open)), u8(196 * (1 - open)), false});
        auto stampText =
            [&](const std::string& str, float leftX, float topY, int scale, const Rgb& c)
        {
          constexpr int kFW = 5, kFH = 7, kGap = 1;
          for (int ci = 0; ci < static_cast<int>(str.size()); ++ci)
          {
            const auto g =
                glyph5x7(static_cast<char>(std::toupper(static_cast<unsigned char>(str[ci]))));
            const int cox = ci * (kFW + kGap) * scale;
            for (int fy = 0; fy < kFH; ++fy)
              for (int fx = 0; fx < kFW; ++fx)
              {
                if (g[fy][fx] != '1')
                  continue;
                for (int sy = 0; sy < scale; ++sy)
                  for (int sx = 0; sx < scale; ++sx)
                  {
                    const int x = static_cast<int>(leftX) + cox + fx * scale + sx;
                    const int y = static_cast<int>(topY) + fy * scale + sy;
                    if (x >= 0 && x < w && y >= 0 && y < h)
                      dst[static_cast<std::size_t>(y) * w + x] = c;
                  }
              }
          }
        };
        const float signW = 4 * 6 * txtScale - txtScale;
        stampText("EXIT",
                  doorCx - signW * 0.5F,
                  dtop - 7 * txtScale - 2,
                  txtScale,
                  Rgb{40, 255, 90, false});
        auto drawPerson = [&](float cx, float feetY, float ht, float lean, const Rgb& c)
        {
          const float headR = ht * 0.13F;
          const float hipY = feetY - ht * 0.46F;
          const float neckY = feetY - ht * 0.78F;
          const float lx = lean * ht;
          drawSeg(dst, w, h, cx, hipY, cx + lx, neckY, ht * 0.09F, ya, c);
          plotDot(dst, w, h, cx + lx, neckY - headR, headR, ya, c);
          drawSeg(dst, w, h, cx, hipY, cx - ht * 0.11F, feetY, ht * 0.07F, ya, c);
          drawSeg(dst, w, h, cx, hipY, cx + ht * 0.11F, feetY, ht * 0.07F, ya, c);
          const float shY = neckY + ht * 0.06F;
          drawSeg(dst, w, h, cx + lx * 0.8F, shY, cx - ht * 0.10F + lx, hipY, ht * 0.06F, ya, c);
          drawSeg(dst,
                  w,
                  h,
                  cx + lx * 0.8F,
                  shY,
                  cx + ht * 0.14F + lx,
                  hipY - ht * 0.02F,
                  ht * 0.06F,
                  ya,
                  c);
        };
        float fx, fyFeet, ht = h * 0.15F, lean = 0.0F;
        bool drawFig = true;
        if (t < 0.50F)
        {
          const float wf = std::clamp((t - 0.06F) / 0.44F, 0.0F, 1.0F);
          fx = (stairX0 + 10) + (doorCx - (stairX0 + 10)) * wf;
          fyFeet = (stairY0 - 4) + ((doorSill - 2) - (stairY0 - 4)) * wf;
          lean = 0.14F + std::sin(t * 40.0F) * 0.03F;  // trudging up
        }
        else if (t < 0.62F)
        {
          fx = doorCx;
          fyFeet = doorSill - 2;
          lean = 0.10F +
                 std::sin(std::clamp((t - 0.50F) / 0.12F, 0.0F, 1.0F) * 3.14159F) * 0.5F;  // bow
        }
        else
        {
          fx = doorCx;
          fyFeet = doorSill - 2;
          const float ef = std::clamp((t - 0.62F) / 0.18F, 0.0F, 1.0F);
          ht = h * 0.15F * (1.0F - 0.6F * ef);  // step into the dark
          if (ef >= 1.0F)
            drawFig = false;
        }
        if (drawFig)
          drawPerson(fx, fyFeet, ht, lean, Rgb{16, 20, 30, false});
        const float flood = std::clamp((t - 0.80F) / 0.20F, 0.0F, 1.0F);
        if (flood > 0.0F)
          plotDot(dst,
                  w,
                  h,
                  doorCx,
                  dtop + doorH * 0.5F,
                  flood * std::hypot(static_cast<float>(w), static_cast<float>(h)),
                  ya,
                  Rgb{0, 0, 0, false});
      });
}

void effectMoonRocket(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float moonR = 0.33F * mn;
  const float moonCx = w * 0.52F, moonCy = h * 0.46F;
  const float eyeRx = moonCx + moonR * 0.40F, eyeRy = moonCy - moonR * 0.16F;
  const float eyeLx = moonCx - moonR * 0.40F, eyeLy = moonCy - moonR * 0.16F;
  const float hitT = 0.64F;
  runFrames(
      renderer,
      w,
      h,
      5200,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float nf = std::clamp(1.0F - t * 3.0F, 0.0F, 1.0F);
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 0.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b) * nf;
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(6 + l * 0.05F), u8(8 + l * 0.05F), u8(20 + l * 0.07F), false};
          }
        for (int i = 0; i < 70; ++i)
        {
          const int sx = static_cast<int>(hash(i * 2) * w),
                    sy = static_cast<int>(hash(i * 2 + 1) * h);
          if (sx < 0 || sx >= w || sy < 0 || sy >= h)
            continue;
          const float tw = 0.45F + 0.55F * std::sin(t * 8.0F + i);
          dst[static_cast<std::size_t>(sy) * w + sx] =
              Rgb{u8(170 * tw), u8(170 * tw), u8(195 * tw), false};
        }
        const float mi = std::clamp(t * 5.0F, 0.0F, 1.0F);
        drawDataDisk(dst,
                     w,
                     h,
                     src,
                     moonCx,
                     moonCy,
                     moonR,
                     ya,
                     0.70F,
                     t * 0.25F,
                     Rgb{u8(238 * mi), u8(232 * mi), u8(196 * mi), false});
        const bool hit = t >= hitT;
        const float wince = std::clamp((t - hitT) / 0.10F, 0.0F, 1.0F);
        const Rgb feat{u8(120 * mi), u8(94 * mi), u8(64 * mi), false};
        auto drawRocket = [&](float x, float y, float scl)
        {
          const float L = moonR * 0.46F * scl, R = moonR * 0.085F * scl;
          const Rgb body{u8(205 * mi), u8(205 * mi), u8(212 * mi), false};
          const Rgb nose{u8(150 * mi), u8(150 * mi), u8(160 * mi), false};
          for (int i = 0; i <= 14; ++i)
          {
            const float f = i / 14.0F, along = (f - 0.5F) * L;
            const float q = (f - 0.42F) / 0.5F;
            float rad = R * std::sqrt(std::max(0.0F, 1.0F - q * q));
            if (f > 0.82F)
              rad *= (1.0F - (f - 0.82F) / 0.18F);
            plotDot(dst, w, h, x + along, y, std::max(1.0F, rad), ya, f > 0.80F ? nose : body);
          }
          drawSeg(dst,
                  w,
                  h,
                  x - L * 0.5F,
                  y,
                  x - L * 0.60F,
                  y - R * 1.7F,
                  std::max(1.0F, R * 0.35F),
                  ya,
                  body);
          drawSeg(dst,
                  w,
                  h,
                  x - L * 0.5F,
                  y,
                  x - L * 0.60F,
                  y + R * 1.7F,
                  std::max(1.0F, R * 0.35F),
                  ya,
                  body);
        };
        plotDot(dst, w, h, eyeLx, eyeLy, moonR * 0.10F, ya, feat);
        drawSeg(dst,
                w,
                h,
                eyeLx - moonR * 0.14F,
                eyeLy - moonR * 0.20F,
                eyeLx + moonR * 0.10F,
                eyeLy - moonR * 0.16F,
                std::max(1.0F, moonR * 0.035F),
                ya,
                feat);
        drawSeg(dst,
                w,
                h,
                moonCx,
                moonCy - moonR * 0.02F,
                moonCx - moonR * 0.05F,
                moonCy + moonR * 0.16F,
                std::max(1.0F, moonR * 0.045F),
                ya,
                feat);
        {
          const float my = moonCy + moonR * 0.42F, curve = hit ? -0.12F : 0.10F;
          for (int k = -5; k <= 5; ++k)
          {
            const float fxn = k / 5.0F;
            plotDot(dst,
                    w,
                    h,
                    moonCx + fxn * moonR * 0.26F,
                    my - curve * moonR * (1.0F - fxn * fxn),
                    std::max(1.0F, moonR * 0.03F),
                    ya,
                    feat);
          }
        }
        if (!hit)
        {
          plotDot(dst, w, h, eyeRx, eyeRy, moonR * 0.10F, ya, feat);
          drawSeg(dst,
                  w,
                  h,
                  eyeRx - moonR * 0.10F,
                  eyeRy - moonR * 0.16F,
                  eyeRx + moonR * 0.14F,
                  eyeRy - moonR * 0.20F,
                  std::max(1.0F, moonR * 0.035F),
                  ya,
                  feat);
          const float rp = std::clamp((t - 0.14F) / (hitT - 0.14F), 0.0F, 1.0F);
          const float rx = -0.12F * w + (eyeRx - (-0.12F * w)) * rp;
          const float ry =
              h * 0.10F + (eyeRy - h * 0.10F) * rp - std::sin(rp * 3.14159F) * moonR * 0.55F;
          drawRocket(rx, ry, 1.0F);
        }
        else
        {
          const Rgb gunk{u8(48 * mi), u8(34 * mi), u8(24 * mi), false};
          plotDot(dst, w, h, eyeRx, eyeRy, moonR * (0.12F + 0.10F * wince), ya, gunk);
          for (int k = 0; k < 8; ++k)
          {
            const float a = k / 8.0F * 6.2832F, dd = moonR * 0.22F * wince;
            plotDot(dst,
                    w,
                    h,
                    eyeRx + std::cos(a) * dd,
                    eyeRy + std::sin(a) * dd,
                    std::max(1.0F, moonR * 0.035F),
                    ya,
                    gunk);
          }
          drawSeg(dst,
                  w,
                  h,
                  eyeRx - moonR * 0.16F,
                  eyeRy - moonR * 0.10F,
                  eyeRx + moonR * 0.10F,
                  eyeRy - moonR * 0.24F,
                  std::max(1.0F, moonR * 0.035F),
                  ya,
                  feat);
          drawRocket(eyeRx - moonR * 0.16F, eyeRy - moonR * 0.04F, 0.9F);
          if (wince < 0.4F)
          {
            const float fl = 1.0F - wince / 0.4F;
            plotDot(dst,
                    w,
                    h,
                    eyeRx,
                    eyeRy,
                    moonR * (0.10F + 0.5F * fl),
                    ya,
                    Rgb{u8(255 * fl), u8(240 * fl), u8(180 * fl), false});
          }
        }
      });
}

void effectFilmBurn(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float ox = w * 0.40F, oy = h * 0.60F;
  const float maxR = std::hypot(std::max(ox, w - ox), std::max(oy * ya, (h - oy) * ya)) + 4.0F;
  auto noise = [](float a)
  {
    return 1.0F + 0.13F * std::sin(a * 6.0F + 1.3F) + 0.07F * std::sin(a * 13.0F) +
           0.05F * std::sin(a * 23.0F + 0.7F);
  };
  runFrames(
      renderer,
      w,
      h,
      4400,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float weave = std::sin(t * 55.0F) * 1.3F + std::sin(t * 12.0F) * 0.6F;
        const float flick = 0.82F + 0.18F * std::sin(t * 80.0F) * std::sin(t * 37.0F);
        const float burn = std::clamp((t - 0.16F) / 0.66F, 0.0F, 1.0F);
        const float R = burn * burn * maxR;
        const float edge = 0.05F * maxR + 1.0F, heatBand = 0.14F * maxR;
        const float whiteOut = std::clamp((t - 0.82F) / 0.10F, 0.0F, 1.0F);
        const float toBlack = std::clamp((t - 0.92F) / 0.08F, 0.0F, 1.0F);
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb s = sample(src, w, h, static_cast<float>(x), static_cast<float>(y) + weave);
            float r = s.transparent ? 14.0F : s.r, g = s.transparent ? 16.0F : s.g,
                  b = s.transparent ? 22.0F : s.b;
            r *= flick;
            g *= flick;
            b *= flick;
            const float dx = x - ox, dy = (y - oy) * ya;
            const float d = std::hypot(dx, dy);
            const float rr = R * noise(std::atan2(dy, dx));
            Rgb out;
            if (d < rr - edge)
            {
              const float wv = 232.0F * flick;
              out = Rgb{u8(wv + 20), u8(wv + 8), u8(wv - 22), false};
            }
            else if (d < rr)
            {
              const float e = (rr - d) / edge;
              out = Rgb{255, u8(110 + 125 * e), u8(15 + 85 * e), false};
            }
            else
            {
              const float heat = std::clamp(1.0F - (d - rr) / heatBand, 0.0F, 1.0F);
              out = Rgb{u8(r * (1 - heat) + 72 * heat),
                        u8(g * (1 - heat) + 40 * heat),
                        u8(b * (1 - heat) + 18 * heat),
                        false};
              if (heat > 0.82F)
              {
                const float k = (heat - 0.82F) / 0.18F;
                out = Rgb{u8(out.r * (1 - k)), u8(out.g * (1 - k)), u8(out.b * (1 - k)), false};
              }
            }
            if (whiteOut > 0.0F)
              out = Rgb{u8(out.r + (255 - out.r) * whiteOut),
                        u8(out.g + (255 - out.g) * whiteOut),
                        u8(out.b + (255 - out.b) * whiteOut),
                        false};
            if (toBlack > 0.0F)
              out = Rgb{u8(out.r * (1 - toBlack)),
                        u8(out.g * (1 - toBlack)),
                        u8(out.b * (1 - toBlack)),
                        false};
            dst[static_cast<std::size_t>(y) * w + x] = out;
          }
      });
}

void effectET(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float moonR = 0.30F * mn;
  const float moonCx = w * 0.60F, moonCy = h * 0.40F;
  runFrames(renderer,
            w,
            h,
            5200,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float nf = std::clamp(1.0F - t * 3.0F, 0.0F, 1.0F);
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l =
                      s.transparent ? 0.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b) * nf;
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(8 + l * 0.05F), u8(12 + l * 0.06F), u8(28 + l * 0.08F), false};
                }
              for (int i = 0; i < 60; ++i)
              {
                const int sx = static_cast<int>(hash(i * 2) * w),
                          sy = static_cast<int>(hash(i * 2 + 1) * h * 0.85F);
                if (sx < 0 || sx >= w || sy < 0 || sy >= h)
                  continue;
                const float tw = 0.55F + 0.45F * std::sin(t * 7.0F + i);
                dst[static_cast<std::size_t>(sy) * w + sx] =
                    Rgb{u8(185 * tw), u8(185 * tw), u8(205 * tw), false};
              }
              const float mi = std::clamp(t * 4.0F, 0.0F, 1.0F);
              plotDot(dst,
                      w,
                      h,
                      moonCx,
                      moonCy,
                      moonR * 1.16F,
                      ya,
                      Rgb{u8(38 * mi), u8(42 * mi), u8(58 * mi), false});
              drawDataDisk(dst,
                           w,
                           h,
                           src,
                           moonCx,
                           moonCy,
                           moonR,
                           ya,
                           0.70F,
                           t * 0.25F,
                           Rgb{u8(236 * mi), u8(232 * mi), u8(206 * mi), false});
              plotDot(dst,
                      w,
                      h,
                      moonCx - moonR * 0.30F,
                      moonCy - moonR * 0.22F,
                      moonR * 0.16F,
                      ya,
                      Rgb{u8(208 * mi), u8(204 * mi), u8(180 * mi), false});
              plotDot(dst,
                      w,
                      h,
                      moonCx + moonR * 0.26F,
                      moonCy + moonR * 0.18F,
                      moonR * 0.12F,
                      ya,
                      Rgb{u8(210 * mi), u8(206 * mi), u8(182 * mi), false});
              auto drawBike = [&](float cx, float cy, float s, const Rgb& c)
              {
                const float rw = 0.30F * s, th = std::max(1.0F, s * 0.05F);
                auto ring = [&](float wx)
                {
                  for (int k = 0; k < 28; ++k)
                  {
                    const float a = k / 28.0F * 6.2832F;
                    plotDot(dst,
                            w,
                            h,
                            wx + std::cos(a) * rw,
                            cy + std::sin(a) * rw,
                            std::max(1.0F, s * 0.045F),
                            ya,
                            c);
                  }
                };
                const float rearX = cx - 0.72F * s, frontX = cx + 0.72F * s;
                ring(rearX);
                ring(frontX);
                const float seatX = cx - 0.28F * s, seatY = cy - 0.60F * s;
                const float barX = cx + 0.55F * s, barY = cy - 0.52F * s;
                drawSeg(dst, w, h, rearX, cy, cx, cy, th, ya, c);
                drawSeg(dst, w, h, cx, cy, seatX, seatY, th, ya, c);
                drawSeg(dst, w, h, rearX, cy, seatX, seatY, th, ya, c);
                drawSeg(dst, w, h, seatX, seatY, barX, barY, th, ya, c);
                drawSeg(dst, w, h, cx, cy, barX, barY, th, ya, c);
                drawSeg(dst, w, h, barX, barY, frontX, cy, th, ya, c);
                const float shX = cx + 0.16F * s, shY = cy - 1.00F * s;
                drawSeg(dst, w, h, seatX, seatY, shX, shY, s * 0.08F, ya, c);
                plotDot(dst, w, h, cx + 0.32F * s, cy - 1.14F * s, s * 0.13F, ya, c);
                drawSeg(dst, w, h, shX, shY, barX, barY, s * 0.05F, ya, c);
                drawSeg(dst, w, h, seatX, seatY, cx, cy, s * 0.05F, ya, c);
                plotDot(dst, w, h, frontX, cy - 0.40F * s, s * 0.13F, ya, c);
                plotDot(dst, w, h, frontX + 0.02F * s, cy - 0.58F * s, s * 0.09F, ya, c);
              };
              const float bp = std::clamp((t - 0.06F) / 0.86F, 0.0F, 1.0F);
              const float bx = (-0.18F + 1.36F * bp) * w;
              const float by = h * 0.62F - std::sin(bp * 3.14159F) * h * 0.24F - bp * h * 0.05F;
              drawBike(bx, by, 0.085F * mn, Rgb{6, 8, 16, false});
            });
}

void effectThelma(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float cliffX = w * 0.46F, groundY = h * 0.72F;
  const float startX = w * 0.05F, endX = w * 0.74F, arcH = h * 0.18F;
  const float freezeT = 0.74F;
  runFrames(
      renderer,
      w,
      h,
      5000,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float dim = std::clamp(1.0F - t * 1.8F, 0.30F, 1.0F);
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l =
                (s.transparent ? 60.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b)) * dim;
            Rgb c;
            if (y < groundY)  // sky: deep blue up top -> orange at the horizon
            {
              const float sf = y / groundY;
              c = Rgb{u8(40 + 200 * sf + l * 0.10F),
                      u8(50 + 110 * sf + l * 0.10F),
                      u8(110 - 40 * sf + l * 0.10F),
                      false};
            }
            else if (x <= cliffX)  // mesa
              c = Rgb{u8(150 + l * 0.20F), u8(90 + l * 0.15F), u8(50 + l * 0.10F), false};
            else  // canyon void
              c = Rgb{u8(28 + l * 0.06F), u8(20 + l * 0.05F), u8(18 + l * 0.05F), false};
            dst[static_cast<std::size_t>(y) * w + x] = c;
          }
        const float p = std::clamp(t / freezeT, 0.0F, 1.0F);
        const float carX = startX + (endX - startX) * p * p;
        float carY = groundY, tilt = 0.0F;
        if (carX > cliffX)
        {
          const float a = (carX - cliffX) / (endX - cliffX);
          carY = groundY - arcH * (1.3F * a - 0.45F * a * a);
          tilt = 0.55F * a;
        }
        if (carX <= cliffX)  // dust trail on the mesa
          for (int k = 1; k <= 5; ++k)
            plotDot(dst,
                    w,
                    h,
                    carX - k * mn * 0.05F,
                    groundY + mn * 0.01F,
                    mn * 0.02F * k,
                    ya,
                    Rgb{u8(150 - k * 12), u8(120 - k * 10), u8(90 - k * 8), false});
        const float s = 0.085F * mn;
        const Rgb car{20, 18, 26, false};
        const float bw = 1.5F * s, bh = 0.45F * s;
        drawSeg(dst, w, h, carX - bw, carY, carX + bw, carY - tilt * bw, 0.42F * s, ya, car);
        plotDot(dst, w, h, carX - bw * 0.7F, carY + bh, 0.34F * s, ya, Rgb{14, 14, 18, false});
        plotDot(dst,
                w,
                h,
                carX + bw * 0.7F,
                carY - tilt * bw * 0.7F + bh,
                0.34F * s,
                ya,
                Rgb{14, 14, 18, false});
        drawSeg(dst,
                w,
                h,
                carX,
                carY - bh,
                carX + bw * 0.5F,
                carY - tilt * bw - bh * 1.8F,
                0.12F * s,
                ya,
                car);
        plotDot(dst, w, h, carX - bw * 0.3F, carY - bh * 1.7F, 0.22F * s, ya, car);
        plotDot(dst, w, h, carX + bw * 0.1F, carY - tilt * bw - bh * 1.9F, 0.22F * s, ya, car);
        if (t > freezeT)  // freeze-frame bleach to white
        {
          const float bl = std::clamp((t - freezeT) / 0.22F, 0.0F, 1.0F) * 0.9F;
          for (std::size_t k = 0; k < dst.size(); ++k)
            dst[k] = Rgb{u8(dst[k].r + (255 - dst[k].r) * bl),
                         u8(dst[k].g + (250 - dst[k].g) * bl),
                         u8(dst[k].b + (235 - dst[k].b) * bl),
                         false};
        }
      });
}

void effectDeLorean(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float vanX = w * 0.5F, vanY = h * 0.46F, roadY = h * 0.78F;
  const float startCX = w * 0.32F, flashT = 0.55F;
  runFrames(
      renderer,
      w,
      h,
      4400,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float speed = std::clamp(t / flashT, 0.0F, 1.0F);
        const float smear = speed * mn * 0.14F;
        const float vdim = 1.0F - 0.55F * speed;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb a = sample(src, w, h, x - smear, static_cast<float>(y));
            const Rgb b = sample(src, w, h, static_cast<float>(x), static_cast<float>(y));
            const float ar = a.transparent ? 10 : a.r, ag = a.transparent ? 12 : a.g,
                        ab = a.transparent ? 18 : a.b;
            const float br = b.transparent ? 10 : b.r, bg = b.transparent ? 12 : b.g,
                        bb = b.transparent ? 18 : b.b;
            dst[static_cast<std::size_t>(y) * w + x] = Rgb{u8((ar * 0.5F + br * 0.5F) * vdim),
                                                           u8((ag * 0.5F + bg * 0.5F) * vdim),
                                                           u8((ab * 0.5F + bb * 0.5F) * vdim),
                                                           false};
          }
        if (t < flashT)  // DeLorean receding (we see its tail)
        {
          const float p = t / flashT;
          const float cs = 0.16F * mn * (1.0F - 0.8F * p);
          const float ccx = startCX + (vanX - startCX) * p, ccy = roadY + (vanY - roadY) * p;
          drawSeg(dst, w, h, ccx - cs, ccy, ccx + cs, ccy, cs * 0.5F, ya, Rgb{30, 30, 40, false});
          drawSeg(dst,
                  w,
                  h,
                  ccx - cs * 0.7F,
                  ccy - cs * 0.5F,
                  ccx + cs * 0.7F,
                  ccy - cs * 0.5F,
                  cs * 0.28F,
                  ya,
                  Rgb{40, 40, 52, false});
          plotDot(dst, w, h, ccx - cs * 0.6F, ccy, cs * 0.2F, ya, Rgb{255, 40, 30, false});
          plotDot(dst, w, h, ccx + cs * 0.6F, ccy, cs * 0.2F, ya, Rgb{255, 40, 30, false});
        }
        const float flash = std::exp(-std::pow((t - flashT) / 0.05F, 2.0F));
        if (t > flashT)  // two burning tire trails fanning toward the camera
        {
          const float ft = std::clamp((t - flashT) / 0.45F, 0.0F, 1.0F);
          for (int side = -1; side <= 1; side += 2)
            for (int k = 0; k <= 20; ++k)
            {
              const float f = k / 20.0F;
              const float tx = vanX + side * (mn * 0.02F + f * mn * 0.22F);
              const float tyy = vanY + (roadY - vanY) * f;
              const float flick = 0.6F + 0.4F * std::sin(t * 40.0F + k);
              const float life = (1.0F - ft) * flick * (0.4F + 0.6F * f);
              if (life < 0.05F)
                continue;
              plotDot(dst,
                      w,
                      h,
                      tx,
                      tyy,
                      mn * 0.025F * life + 0.6F,
                      ya,
                      Rgb{u8(255 * life), u8((150 + 80 * hash(k)) * life), u8(20 * life), false});
            }
        }
        if (flash > 0.01F)
          for (std::size_t k = 0; k < dst.size(); ++k)
            dst[k] = Rgb{u8(dst[k].r + (255 - dst[k].r) * flash),
                         u8(dst[k].g + (255 - dst[k].g) * flash),
                         u8(dst[k].b + (255 - dst[k].b) * flash),
                         false};
      });
}

void effectUp(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float hw = 0.12F * mn, hh = 0.12F * mn;
  static const Rgb kBalloon[6] = {{235, 60, 60, false},
                                  {245, 180, 40, false},
                                  {70, 150, 235, false},
                                  {80, 200, 90, false},
                                  {200, 90, 210, false},
                                  {240, 240, 90, false}};
  runFrames(
      renderer,
      w,
      h,
      5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float vf = std::clamp(1.0F - t * 2.5F, 0.0F, 1.0F) * 0.30F;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 0.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b) * vf;
            const float sf = static_cast<float>(y) / h;
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(120 + 70 * sf + l), u8(170 + 50 * sf + l), u8(225 - 10 * sf + l), false};
          }
        const float form = std::clamp(t / 0.30F, 0.0F, 1.0F);
        const float rise = (t < 0.32F) ? 0.0F : std::pow((t - 0.32F) / 0.68F, 1.6F);
        const float liftY = -rise * (h * 1.4F);
        const float sway =
            std::sin(t * 3.0F) * h * 0.03F * std::clamp((t - 0.30F) / 0.2F, 0.0F, 1.0F);
        const float hx = w * 0.5F + sway, hy = h * 0.66F + liftY;
        const float nb = std::clamp((t - 0.18F) / 0.20F, 0.0F, 1.0F);
        const int balloons = static_cast<int>(nb * 34);
        const float bcx = hx, bcy = hy - hh * 1.1F - mn * 0.18F;
        for (int i = 0; i < 4; ++i)  // strings
          drawSeg(dst,
                  w,
                  h,
                  hx + (i - 1.5F) * hw * 0.3F,
                  hy - hh,
                  bcx + (hash(i) - 0.5F) * mn * 0.1F,
                  bcy,
                  std::max(1.0F, mn * 0.006F),
                  ya,
                  Rgb{210, 210, 210, false});
        for (int i = 0; i < balloons; ++i)
        {
          const float a = hash(i * 3) * 6.2832F, rr = std::sqrt(hash(i * 3 + 1)) * mn * 0.16F;
          const float bx = bcx + std::cos(a) * rr, by = bcy + std::sin(a) * rr * 0.9F;
          drawDataDisk(dst,
                       w,
                       h,
                       src,
                       bx,
                       by,
                       mn * 0.03F,
                       ya,
                       0.85F,
                       hash(i * 5) * 6.2832F + t,
                       kBalloon[i % 6]);
        }
        const float s = form;
        auto fillRect = [&](float x0, float y0, float x1, float y1, const Rgb& c)
        {
          for (int y = std::max(0, static_cast<int>(y0));
               y <= std::min(h - 1, static_cast<int>(y1));
               ++y)
            for (int x = std::max(0, static_cast<int>(x0));
                 x <= std::min(w - 1, static_cast<int>(x1));
                 ++x)
              dst[static_cast<std::size_t>(y) * w + x] = c;
        };
        const float bwd = hw * s, bht = hh * s;
        // Walls and roof carry the data: each pixel samples the underlying
        // view and modulates the tan/red tint, so as the house rises into the
        // air it visibly wears the weather instead of a flat fill.
        {
          const int wx0 = std::max(0, static_cast<int>(hx - bwd));
          const int wx1 = std::min(w - 1, static_cast<int>(hx + bwd));
          const int wy0 = std::max(0, static_cast<int>(hy - bht));
          const int wy1 = std::min(h - 1, static_cast<int>(hy));
          for (int y = wy0; y <= wy1; ++y)
            for (int x = wx0; x <= wx1; ++x)
            {
              const Rgb d = sample(src, w, h, x, y);
              const float dr = d.transparent ? 120.0F : d.r;
              const float dg = d.transparent ? 120.0F : d.g;
              const float db = d.transparent ? 120.0F : d.b;
              dst[static_cast<std::size_t>(y) * w + x] = Rgb{u8(225 * 0.62F + dr * 0.30F),
                                                             u8(210 * 0.62F + dg * 0.30F),
                                                             u8(175 * 0.62F + db * 0.28F),
                                                             false};
            }
        }
        for (int y = 0; y <= static_cast<int>(bht * 0.9F); ++y)  // roof triangle
        {
          const float frac = 1.0F - y / (bht * 0.9F);
          const float rwd = bwd * 1.15F * frac;
          const float ry = hy - bht - y;
          const int ryi = static_cast<int>(ry);
          if (ryi < 0 || ryi >= h)
            continue;
          const int rx0 = std::max(0, static_cast<int>(hx - rwd));
          const int rx1 = std::min(w - 1, static_cast<int>(hx + rwd));
          for (int x = rx0; x <= rx1; ++x)
          {
            const Rgb d = sample(src, w, h, x, ry);
            const float dr = d.transparent ? 100.0F : d.r;
            const float dg = d.transparent ? 100.0F : d.g;
            const float db = d.transparent ? 100.0F : d.b;
            dst[static_cast<std::size_t>(ryi) * w + x] = Rgb{u8(170 * 0.65F + dr * 0.32F),
                                                             u8(70 * 0.65F + dg * 0.25F),
                                                             u8(55 * 0.65F + db * 0.22F),
                                                             false};
          }
        }
        if (s > 0.5F)
        {
          fillRect(
              hx - bwd * 0.22F, hy - bht * 0.5F, hx + bwd * 0.22F, hy, Rgb{120, 80, 50, false});
          fillRect(hx - bwd * 0.7F,
                   hy - bht * 0.75F,
                   hx - bwd * 0.4F,
                   hy - bht * 0.45F,
                   Rgb{250, 240, 150, false});
          fillRect(hx + bwd * 0.4F,
                   hy - bht * 0.75F,
                   hx + bwd * 0.7F,
                   hy - bht * 0.45F,
                   Rgb{250, 240, 150, false});
        }
      });
}

void effectLawrence(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float cutT = 0.46F, horizonY = h * 0.60F;
  runFrames(
      renderer,
      w,
      h,
      5000,
      [&](float t, std::vector<Rgb>& dst)
      {
        if (t < cutT)  // the match in the dark
        {
          const float flame = 1.0F - std::clamp((t - (cutT - 0.12F)) / 0.12F, 0.0F, 1.0F);
          const float fx = w * 0.5F + std::sin(t * 40.0F) * w * 0.004F, fy = h * 0.5F;
          for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
              const float gd = std::hypot(x - fx, (y - fy) * ya);
              const float glow = flame * std::exp(-gd / (0.18F * mn));
              dst[static_cast<std::size_t>(y) * w + x] =
                  Rgb{u8(10 + glow * 230), u8(8 + glow * 150), u8(6 + glow * 60), false};
            }
          drawSeg(dst,
                  w,
                  h,
                  fx,
                  fy + h * 0.02F,
                  w * 0.46F,
                  h * 0.80F,
                  mn * 0.012F,
                  ya,
                  Rgb{90, 60, 35, false});
          if (flame > 0.02F)
          {
            const float fl = flame * (0.9F + 0.1F * std::sin(t * 60.0F));
            plotDot(dst, w, h, fx, fy, mn * 0.05F * fl, ya, Rgb{240, 130, 30, false});
            plotDot(
                dst, w, h, fx, fy - h * 0.018F * fl, mn * 0.03F * fl, ya, Rgb{255, 210, 90, false});
            plotDot(dst,
                    w,
                    h,
                    fx,
                    fy - h * 0.03F * fl,
                    mn * 0.014F * fl,
                    ya,
                    Rgb{255, 250, 230, false});
          }
        }
        else  // the desert sunrise
        {
          const float sr = std::clamp((t - cutT) / 0.5F, 0.0F, 1.0F);
          const float sunX = w * 0.5F, sunY = horizonY + h * 0.12F - sr * h * 0.24F;
          for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
              const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
              const float l = s.transparent ? 70.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
              const float sd = std::hypot(x - sunX, (y - sunY) * ya);
              const float glow = std::exp(-sd / (0.45F * mn));
              Rgb c;
              if (y < horizonY)  // sky
              {
                const float sf = y / horizonY;  // 0 at top -> 1 at horizon
                c = Rgb{u8(55 + 200 * sf + 70 * glow),
                        u8(70 + 100 * sf + 80 * glow),
                        u8(140 - 70 * sf + 50 * glow),
                        false};
              }
              else  // desert dunes from the view luma
                c = Rgb{u8(120 + l * 0.25F + 60 * glow),
                        u8(70 + l * 0.18F + 40 * glow),
                        u8(35 + l * 0.10F),
                        false};
              dst[static_cast<std::size_t>(y) * w + x] = c;
            }
          drawDataDisk(
              dst, w, h, src, sunX, sunY, 0.17F * mn, ya, 0.55F, t * 0.3F, Rgb{255, 232, 165, false});
          plotDot(dst, w, h, sunX, sunY, 0.12F * mn, ya, Rgb{255, 250, 225, false});
        }
      });
}

void effectJurassic(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float cx = w * 0.5F, cy = h * 0.42F;
  const float mn = std::min(static_cast<float>(w), h * ya);
  constexpr int kThuds = 6;
  runFrames(renderer,
            w,
            h,
            5200,
            [&](float t, std::vector<Rgb>& dst)
            {
              float shake = 0.0F;
              for (int i = 0; i < kThuds; ++i)
              {
                const float ti = (i + 1.0F) / (kThuds + 1.0F);
                if (t > ti)
                {
                  const float dt = t - ti, amp = 0.25F + 0.75F * i / (kThuds - 1.0F);
                  shake += amp * std::exp(-dt * 26.0F) * std::sin(dt * 90.0F);
                }
              }
              const float shakePx = shake * h * 0.05F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const float dx = x - cx, dy = (y - cy) * ya, d = std::hypot(dx, dy);
                  float disp = 0.0F;
                  for (int i = 0; i < kThuds; ++i)
                  {
                    const float ti = (i + 1.0F) / (kThuds + 1.0F);
                    if (t > ti)
                    {
                      const float age = t - ti, ringR = age * mn * 2.4F;
                      const float amp = 0.15F + 0.85F * i / (kThuds - 1.0F);
                      const float band = std::exp(-std::pow((d - ringR) / (0.12F * mn), 2.0F));
                      disp += amp * band * std::sin((d - ringR) * 0.7F) * mn * 0.05F;
                    }
                  }
                  const float ux = d > 0.1F ? dx / d : 0.0F, uy = d > 0.1F ? dy / d : 0.0F;
                  const Rgb s = sample(src, w, h, x + ux * disp, y + uy * disp / ya + shakePx);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      s.transparent ? Rgb{8, 10, 14, false} : s;
                }
              const float endDark = std::clamp((t - 0.93F) / 0.07F, 0.0F, 1.0F);
              if (endDark > 0.0F)
                for (std::size_t k = 0; k < dst.size(); ++k)
                  dst[k] = Rgb{u8(dst[k].r * (1 - endDark)),
                               u8(dst[k].g * (1 - endDark)),
                               u8(dst[k].b * (1 - endDark)),
                               false};
            });
}

void effectStandoff(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  static const Rgb kSkin[3] = {
      {185, 142, 100, false}, {150, 120, 96, false}, {205, 172, 132, false}};
  runFrames(
      renderer,
      w,
      h,
      5000,
      [&](float t, std::vector<Rgb>& dst)
      {
        const int cut = static_cast<int>(t * t * 17.0F);
        const int who = cut % 3;
        const float zoom = 1.0F + t * 1.7F;
        const Rgb skin = kSkin[who];
        // The whole-screen close-up: skin tone modulated by the data so the
        // weather rides the cheek instead of a flat fill — soft enough that
        // the face still reads.
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const float vg = 1.0F - 0.4F * std::hypot((x - w * 0.5F) / w, (y - h * 0.5F) / h);
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 80.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            const float k = (l - 128.0F) / 255.0F;  // -0.5..+0.5
            dst[static_cast<std::size_t>(y) * w + x] = Rgb{u8(skin.r * vg + k * 60.0F),
                                                           u8(skin.g * vg + k * 50.0F),
                                                           u8(skin.b * vg + k * 30.0F),
                                                           false};
          }
        const float browY = h * (0.34F - 0.05F * t);
        const float eyeY = h * 0.54F;
        const float eyeDX = w * 0.18F / std::max(1.0F, zoom * 0.6F);
        const float eyeR = mn * 0.10F * zoom * 0.6F;
        const float dart = std::sin(t * 26.0F + who * 2.0F) * eyeR * 0.9F;
        // heavy brow shadow
        for (int y = 0; y < static_cast<int>(browY); ++y)
          for (int x = 0; x < w; ++x)
          {
            const float k = 1.0F - static_cast<float>(y) / browY * 0.6F;
            Rgb& p = dst[static_cast<std::size_t>(y) * w + x];
            p = Rgb{u8(p.r * (1 - 0.7F * k)),
                    u8(p.g * (1 - 0.7F * k)),
                    u8(p.b * (1 - 0.7F * k)),
                    false};
          }
        for (int s = -1; s <= 1; s += 2)
        {
          const float ex = w * 0.5F + s * eyeDX;
          drawSeg(dst,
                  w,
                  h,
                  ex - eyeR,
                  eyeY,
                  ex + eyeR,
                  eyeY,
                  eyeR * 0.55F,
                  ya,
                  Rgb{235, 230, 220, false});  // eye white
          plotDot(dst, w, h, ex + dart, eyeY, eyeR * 0.5F, ya, Rgb{70, 45, 30, false});   // iris
          plotDot(dst, w, h, ex + dart, eyeY, eyeR * 0.22F, ya, Rgb{10, 10, 10, false});  // pupil
        }
        if (t > 0.88F)  // muzzle flash, then black
        {
          const float f = (t - 0.88F) / 0.12F;
          if (f < 0.45F)
          {
            const float wf = f / 0.45F;
            for (std::size_t k = 0; k < dst.size(); ++k)
              dst[k] = Rgb{u8(dst[k].r + (255 - dst[k].r) * wf),
                           u8(dst[k].g + (255 - dst[k].g) * wf),
                           u8(dst[k].b + (255 - dst[k].b) * wf),
                           false};
          }
          else
            for (std::size_t k = 0; k < dst.size(); ++k)
              dst[k] = Rgb{0, 0, 0, false};
        }
      });
}

void effectJaws(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float surfaceY = h * 0.5F;
  runFrames(
      renderer,
      w,
      h,
      5200,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float beat = std::pow(t, 1.6F) * 30.0F;
        const float pulse = std::pow(0.5F + 0.5F * std::sin(beat), 3.0F);
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 0.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            const float shim = 0.5F + 0.5F * std::sin(x * 0.3F + t * 6.0F);
            const float depth = std::clamp((y - surfaceY) / (h * 0.5F), 0.0F, 1.0F);
            dst[static_cast<std::size_t>(y) * w + x] = Rgb{u8(10 + l * 0.05F + pulse * 40 * depth),
                                                           u8(30 + l * 0.06F + shim * 14),
                                                           u8(60 + l * 0.08F),
                                                           false};
          }
        const float fp = std::clamp(t / 0.85F, 0.0F, 1.0F);
        const float finX = -0.1F * w + 1.2F * w * fp;
        const float submerge = std::clamp((t - 0.80F) / 0.18F, 0.0F, 1.0F);
        const float finH = mn * 0.13F * (1.0F - submerge);
        if (finH > 1.0F)
        {
          for (int k = 1; k <= 6; ++k)  // V-wake foam behind (moving right)
          {
            const float f = k / 6.0F;
            const Rgb foam{u8(180 * (1 - f)), u8(200 * (1 - f)), u8(210 * (1 - f)), false};
            plotDot(dst,
                    w,
                    h,
                    finX - f * mn * 0.22F,
                    surfaceY - f * finH * 0.5F,
                    mn * 0.012F,
                    ya,
                    foam);
            plotDot(dst,
                    w,
                    h,
                    finX - f * mn * 0.22F,
                    surfaceY + f * finH * 0.5F,
                    mn * 0.012F,
                    ya,
                    foam);
          }
          const float ax = finX + mn * 0.06F, ay = surfaceY;  // fin: leaned-back triangle
          const float bx = finX - mn * 0.08F, by = surfaceY;
          const float ttx = finX - mn * 0.02F, tty = surfaceY - finH;
          const int x0 = static_cast<int>(finX - mn * 0.10F),
                    x1 = static_cast<int>(finX + mn * 0.08F);
          const int y0 = static_cast<int>(surfaceY - finH - 1), y1 = static_cast<int>(surfaceY + 1);
          for (int y = std::max(0, y0); y <= std::min(h - 1, y1); ++y)
            for (int x = std::max(0, x0); x <= std::min(w - 1, x1); ++x)
              if (inTri(static_cast<float>(x), static_cast<float>(y), ax, ay, bx, by, ttx, tty))
                dst[static_cast<std::size_t>(y) * w + x] = Rgb{18, 22, 30, false};
        }
        const float endDark = std::clamp((t - 0.90F) / 0.10F, 0.0F, 1.0F);
        if (endDark > 0.0F)
          for (std::size_t k = 0; k < dst.size(); ++k)
            dst[k] = Rgb{u8(dst[k].r * (1 - endDark)),
                         u8(dst[k].g * (1 - endDark)),
                         u8(dst[k].b * (1 - endDark)),
                         false};
      });
}

void effectStarGate(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hsv = [&](float hh, float s, float v) -> Rgb
  {
    hh -= std::floor(hh);
    const float H = hh * 6.0F;
    const int i = static_cast<int>(H);
    const float f = H - i, p = v * (1 - s), q = v * (1 - s * f), tt = v * (1 - s * (1 - f));
    float r, g, b;
    switch (i % 6)
    {
      case 0:
        r = v;
        g = tt;
        b = p;
        break;
      case 1:
        r = q;
        g = v;
        b = p;
        break;
      case 2:
        r = p;
        g = v;
        b = tt;
        break;
      case 3:
        r = p;
        g = q;
        b = v;
        break;
      case 4:
        r = tt;
        g = p;
        b = v;
        break;
      default:
        r = v;
        g = p;
        b = q;
        break;
    }
    return Rgb{u8(r * 255), u8(g * 255), u8(b * 255), false};
  };
  const float cx = w * 0.5F, cy = h * 0.5F;
  const float mn = std::min(static_cast<float>(w), h * ya);
  runFrames(renderer,
            w,
            h,
            5200,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float speed = 1.0F + t * 3.0F;
              const float onset = std::clamp(t * 3.0F, 0.0F, 1.0F);
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const float dx = x - cx, dy = (y - cy) * ya, r = std::hypot(dx, dy);
                  const float depth = (mn * 0.5F) / (r + 1.0F);
                  const float ang = std::atan2(dy, dx);
                  const float hue = depth * 0.5F + ang * 0.16F - t * speed * 0.5F;
                  const float flick = 0.7F + 0.3F * std::sin(depth * 22.0F - t * speed * 6.0F);
                  const float v = std::clamp(0.18F + depth * 0.9F, 0.0F, 1.0F) * flick * onset;
                  Rgb c = (r < mn * 0.05F) ? Rgb{255, 255, 255, false} : hsv(hue, 0.85F, v);
                  dst[static_cast<std::size_t>(y) * w + x] = c;
                }
            });
}

void effectCloseEncounters(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float ridgeY = h * 0.74F;
  static const Rgb kTone[5] = {{235, 80, 80, false},
                               {245, 160, 50, false},
                               {240, 235, 90, false},
                               {90, 210, 110, false},
                               {90, 140, 240, false}};
  static const int kSeq[8] = {2, 3, 1, 0, 4, 2, 3, 1};  // a five-tone-ish phrase
  runFrames(
      renderer,
      w,
      h,
      5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float rise = std::clamp((t - 0.50F) / 0.45F, 0.0F, 1.0F);
        const float shipCy = ridgeY + mn * 0.30F - rise * mn * 0.42F;
        const float shipR = mn * 0.42F;
        const float halo = rise;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const float ridge =
                ridgeY + std::sin(x * 0.10F) * h * 0.02F + std::sin(x * 0.031F + 1.0F) * h * 0.03F;
            const float gd = std::hypot(x - w * 0.5F, (y - shipCy) * ya);
            const float g = halo * std::exp(-gd / (0.7F * mn)) * 0.6F;
            Rgb c;
            if (y > ridge)
              c = Rgb{u8(12 + g * 200), u8(10 + g * 200), u8(14 + g * 160), false};  // dark ridge
            else
            {
              const float sf = y / ridgeY;
              c = Rgb{u8(10 + 18 * sf + g * 220),
                      u8(12 + 22 * sf + g * 220),
                      u8(26 + 40 * sf + g * 180),
                      false};
            }
            dst[static_cast<std::size_t>(y) * w + x] = c;
          }
        for (int i = 0; i < 70; ++i)  // stars
        {
          const int sx = static_cast<int>(hash(i * 2) * w),
                    sy = static_cast<int>(hash(i * 2 + 1) * ridgeY);
          if (sx >= 0 && sx < w && sy >= 0 && sy < h)
            dst[static_cast<std::size_t>(sy) * w + sx] = Rgb{150, 150, 175, false};
        }
        const int step = static_cast<int>(t * 14.0F);  // tone sequence
        const float local = t * 14.0F - step;
        const int lit = kSeq[step % 8];
        const float env = std::sin(std::clamp(local, 0.0F, 1.0F) * 3.14159F);
        for (int i = 0; i < 5; ++i)  // five coloured tone-lights on the ridge
        {
          const float px = w * (0.22F + 0.14F * i), py = ridgeY - mn * 0.02F;
          const float on = (i == lit) ? env : 0.12F;
          plotDot(dst,
                  w,
                  h,
                  px,
                  py,
                  mn * 0.055F,
                  ya,
                  Rgb{u8(kTone[i].r * on), u8(kTone[i].g * on), u8(kTone[i].b * on), false});
        }
        if (rise > 0.0F)  // the mothership
        {
          drawDataDisk(dst,
                       w,
                       h,
                       src,
                       w * 0.5F,
                       shipCy,
                       shipR,
                       ya,
                       0.85F,
                       t * 0.20F,
                       Rgb{50, 54, 70, false});  // hull, data-wrapped
          drawDataDisk(dst,
                       w,
                       h,
                       src,
                       w * 0.5F,
                       shipCy,
                       shipR * 0.34F,
                       ya,
                       0.30F,
                       t * 0.60F,
                       Rgb{u8(220 * halo + 35),
                           u8(225 * halo + 35),
                           u8(255 * halo + 45),
                           false});  // bright core
          for (int i = 0; i < 26; ++i)  // studded lights around the rim
          {
            const float a = i / 26.0F * 3.14159F;  // lower hemisphere
            const float lx = w * 0.5F + std::cos(a) * shipR * 0.86F;
            const float lyy = shipCy + std::sin(a) * shipR * 0.86F / ya * ya;
            const float fl = 0.5F + 0.5F * std::sin(t * 12.0F + i);
            const Rgb& k = kTone[i % 5];
            plotDot(dst,
                    w,
                    h,
                    lx,
                    lyy,
                    mn * 0.012F,
                    ya,
                    Rgb{u8(k.r * fl), u8(k.g * fl), u8(k.b * fl), false});
          }
        }
        const float fin = std::clamp((t - 0.93F) / 0.07F, 0.0F, 1.0F);
        if (fin > 0.0F)
          for (std::size_t k = 0; k < dst.size(); ++k)
            dst[k] = Rgb{u8(dst[k].r + (255 - dst[k].r) * fin),
                         u8(dst[k].g + (255 - dst[k].g) * fin),
                         u8(dst[k].b + (255 - dst[k].b) * fin),
                         false};
      });
}

void effectTitanic(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  const float pivotX = w * 0.5F, pivotY = h * 0.92F, waterY = h * 0.66F;
  runFrames(
      renderer,
      w,
      h,
      5600,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float ang = std::pow(t, 1.3F) * 1.05F;  // tilt, stern rising
        const float sink = std::pow(std::clamp((t - 0.28F) / 0.72F, 0.0F, 1.0F), 1.4F) * h * 1.15F;
        const float ca = std::cos(-ang), sa = std::sin(-ang);
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const float rx = x - pivotX, ry = (y - sink) - pivotY;
            const float sxv = pivotX + (rx * ca - ry * sa);
            const float syv = pivotY + (rx * sa + ry * ca);
            const Rgb s = sample(src, w, h, sxv, syv);
            Rgb out;
            if (!s.transparent && sxv >= 0 && sxv < w && syv >= 0 && syv < h)
            {
              if (y > waterY)  // ship gone under: deep, dark, blue
              {
                const float dd = std::clamp((y - waterY) / (h - waterY), 0.0F, 1.0F);
                out = Rgb{u8(s.r * 0.10F * (1 - dd)),
                          u8(s.g * 0.14F * (1 - dd) + 8),
                          u8(s.b * 0.20F * (1 - dd) + 18),
                          false};
              }
              else
                out = Rgb{u8(s.r * 0.55F), u8(s.g * 0.55F), u8(s.b * 0.6F), false};  // night ship
            }
            else if (y > waterY)  // sea
            {
              const float dd = (y - waterY) / (h - waterY);
              out =
                  Rgb{u8(6 + 6 * (1 - dd)), u8(12 + 10 * (1 - dd)), u8(28 + 18 * (1 - dd)), false};
              if (hash(x * 13 + y * 7) > 0.992F)
                out = Rgb{60, 70, 90, false};  // debris / froth fleck
            }
            else  // night sky
            {
              out = Rgb{6, 8, 18, false};
              if (hash(x * 7 + y * 131) > 0.985F)
                out = Rgb{150, 150, 180, false};  // star
            }
            dst[static_cast<std::size_t>(y) * w + x] = out;
          }
      });
}

void effectInception(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float cx = w * 0.5F, tableY = h * 0.66F;
  runFrames(
      renderer,
      w,
      h,
      5200,
      [&](float t, std::vector<Rgb>& dst)
      {
        for (int y = 0; y < h; ++y)  // dim the data into a shadowy room + tabletop
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float dim = (y > tableY) ? 0.22F : 0.34F;
            dst[static_cast<std::size_t>(y) * w + x] =
                s.transparent ? Rgb{10, 10, 14, false}
                              : Rgb{u8(s.r * dim), u8(s.g * dim), u8(s.b * dim + 4), false};
          }
        const float form = std::clamp(t / 0.22F, 0.0F, 1.0F);
        const float bodyR = mn * 0.12F * form;
        const float spin = t * t * 80.0F;
        const float wob = std::pow(std::clamp((t - 0.40F) / 0.55F, 0.0F, 1.0F), 1.5F);
        const float prec = t * 13.0F;
        const float pivotX = cx + std::sin(prec) * wob * mn * 0.07F;
        const float tiltX = std::sin(prec) * wob * bodyR * 0.9F;
        const float bodyCx = pivotX + tiltX, bodyCy = tableY - bodyR * 1.25F;
        if (bodyR > 1.0F)
        {
          const int x0 = static_cast<int>(std::min(bodyCx - bodyR, pivotX) - 1);
          const int x1 = static_cast<int>(std::max(bodyCx + bodyR, pivotX) + 1);
          for (int y = static_cast<int>(bodyCy); y <= static_cast<int>(tableY); ++y)
            for (int x = std::max(0, x0); x <= std::min(w - 1, x1); ++x)
              if (inTri(static_cast<float>(x),
                        static_cast<float>(y),
                        bodyCx - bodyR * 0.5F,
                        bodyCy,
                        bodyCx + bodyR * 0.5F,
                        bodyCy,
                        pivotX,
                        tableY))
              {
                const Rgb s = sample(src, w, h, x, y);
                dst[static_cast<std::size_t>(y) * w + x] =
                    s.transparent ? Rgb{40, 40, 48, false}
                                  : Rgb{u8(s.r * 0.5F), u8(s.g * 0.5F), u8(s.b * 0.5F), false};
              }
          drawSphere(dst, w, h, src, bodyCx, bodyCy, bodyR, bodyR, 0.85F, spin, 0.92F, 0.92F, 1.0F);
          drawSeg(dst,
                  w,
                  h,
                  bodyCx,
                  bodyCy - bodyR,
                  bodyCx + tiltX * 0.2F,
                  bodyCy - bodyR * 1.4F,
                  std::max(1.0F, bodyR * 0.08F),
                  ya,
                  Rgb{180, 180, 190, false});
        }
        if (t > 0.93F)  // the smash cut to black
          for (std::size_t k = 0; k < dst.size(); ++k)
            dst[k] = Rgb{0, 0, 0, false};
      });
}

void effectVertigo(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float cx = w * 0.5F, cy = h * 0.5F;
  runFrames(
      renderer,
      w,
      h,
      5200,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float rot = t * 3.5F;
        const float twist = (1.5F + t * 7.0F) * 0.1F;
        const float zoom = 1.0F + 0.5F * std::sin(t * 3.0F) + t * 0.9F;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const float dx = x - cx, dy = (y - cy) * ya, r = std::hypot(dx, dy);
            const float th = std::atan2(dy, dx);
            const float th2 = th + rot + std::log(r + 1.0F) * twist;
            const float r2 = r / zoom;
            const Rgb s = sample(src, w, h, cx + std::cos(th2) * r2, cy + std::sin(th2) * r2 / ya);
            const float arm = std::sin(th * 6.0F + std::log(r + 1.0F) * 4.0F - t * 6.0F);
            const float red = std::clamp(arm, 0.0F, 1.0F) * 0.5F * std::clamp(1.3F - t, 0.0F, 1.0F);
            const float br = s.transparent ? 10 : s.r, bg = s.transparent ? 10 : s.g,
                        bb = s.transparent ? 14 : s.b;
            dst[static_cast<std::size_t>(y) * w + x] = Rgb{u8(br + (255 - br) * red * 0.7F),
                                                           u8(bg * (1 - red * 0.5F)),
                                                           u8(bb * (1 - red * 0.5F)),
                                                           false};
          }
        const float fin = std::clamp((t - 0.90F) / 0.10F, 0.0F, 1.0F);
        if (fin > 0.0F)
          for (std::size_t k = 0; k < dst.size(); ++k)
            dst[k] = Rgb{u8(dst[k].r * (1 - fin)),
                         u8(dst[k].g * (1 - fin)),
                         u8(dst[k].b * (1 - fin)),
                         false};
      });
}

void effectPsycho(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float cx = w * 0.5F, cy = h * 0.5F, mn = std::min(static_cast<float>(w), h * ya);
  runFrames(renderer,
            w,
            h,
            4800,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float pull = 1.0F + t * 0.5F;
              const float swirl = 0.8F + t * 3.2F;
              const float holeR = std::pow(t, 1.9F) * mn * 0.58F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const float dx = x - cx, dy = (y - cy) * ya, r = std::hypot(dx, dy);
                  if (r < holeR)
                  {
                    dst[static_cast<std::size_t>(y) * w + x] = Rgb{0, 0, 0, false};
                    continue;
                  }
                  const float th = std::atan2(dy, dx) - swirl / (r / mn + 0.18F);
                  const float r2 = r * pull;
                  const Rgb s =
                      sample(src, w, h, cx + std::cos(th) * r2, cy + std::sin(th) * r2 / ya);
                  const float fade = std::clamp((r - holeR) / (mn * 0.25F), 0.0F, 1.0F);
                  const float br = s.transparent ? 6 : s.r, bg = s.transparent ? 8 : s.g,
                              bb = s.transparent ? 12 : s.b;
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(br * fade), u8(bg * fade), u8(bb * fade), false};
                }
            });
}

void effectFeather(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  runFrames(renderer,
            w,
            h,
            5600,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float vf = std::clamp(1.0F - t * 1.5F, 0.12F, 1.0F);
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 200.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  const float sf = static_cast<float>(y) / h;
                  dst[static_cast<std::size_t>(y) * w + x] = Rgb{u8(196 - 26 * sf + l * 0.10F * vf),
                                                                 u8(208 - 16 * sf + l * 0.10F * vf),
                                                                 u8(226 + l * 0.08F * vf),
                                                                 false};
                }
              const float fall = std::pow(t, 1.1F);
              const float fx = w * (0.5F + 0.24F * std::sin(t * 2.2F));
              const float fy = h * (0.06F + 0.86F * fall);
              const float rot = 0.5F + std::sin(t * 2.6F) * 0.6F;
              const float L = mn * 0.42F, maxW = mn * 0.085F;
              const float ca = std::cos(rot), sa = std::sin(rot);
              for (int i = 0; i <= static_cast<int>(L); ++i)
              {
                const float sps = i / L - 0.5F;  // -0.5 (tip) .. 0.5 (base)
                const float halfw = maxW * std::sin((sps + 0.5F) * 3.14159F);
                for (int j = -static_cast<int>(halfw); j <= static_cast<int>(halfw); ++j)
                {
                  const float lx = static_cast<float>(j), ly = sps * L;
                  const bool rachis = std::fabs(lx) < std::max(1.0F, maxW * 0.12F);
                  const bool gap = (!rachis) &&
                                   (static_cast<int>(ly * 0.9F + std::fabs(lx) * 1.3F) % 3 == 0) &&
                                   std::fabs(lx) > halfw * 0.35F;
                  if (gap)
                    continue;  // feathery barb gaps at the vane edge
                  const float px = fx + lx * ca - ly * sa;
                  const float py = fy + (lx * sa + ly * ca) / ya;
                  if (px < 0 || px >= w || py < 0 || py >= h)
                    continue;
                  const Rgb d = sample(src, w, h, px, py);
                  const float dr = d.transparent ? 210 : d.r, dg = d.transparent ? 210 : d.g,
                              db = d.transparent ? 220 : d.b;
                  dst[static_cast<std::size_t>(py) * w + static_cast<std::size_t>(px)] =
                      rachis ? Rgb{205, 200, 188, false}
                             : Rgb{u8(248 * 0.68F + dr * 0.32F),
                                   u8(248 * 0.68F + dg * 0.32F),
                                   u8(250 * 0.68F + db * 0.30F),
                                   false};
                }
              }
            });
}

void effectRedBalloon(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  runFrames(
      renderer,
      w,
      h,
      5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float vf = std::clamp(1.0F - t * 1.6F, 0.15F, 1.0F);
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 0.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b) * vf;
            const float sf = static_cast<float>(y) / h;
            dst[static_cast<std::size_t>(y) * w + x] = Rgb{u8(182 - 20 * sf + l * 0.12F),
                                                           u8(196 - 14 * sf + l * 0.12F),
                                                           u8(216 + l * 0.10F),
                                                           false};
          }
        const float form = std::clamp(t / 0.22F, 0.0F, 1.0F);
        const float rise = std::clamp((t - 0.20F) / 0.80F, 0.0F, 1.0F);
        const float r = mn * 0.16F * form * (1.0F - 0.6F * rise);
        const float bx = w * 0.5F + std::sin(t * 2.0F) * mn * 0.06F * rise;
        const float by = h * 0.60F - rise * h * 0.95F;
        if (r > 1.0F)
        {
          for (int k = 1; k <= 8; ++k)  // dangling wavy string
          {
            const float sy = by + r * 1.15F + k * r * 0.22F;
            const float sx = bx + std::sin(k * 0.7F + t * 3.0F) * r * 0.18F;
            plotDot(dst, w, h, sx, sy, std::max(1.0F, r * 0.05F), ya, Rgb{60, 50, 50, false});
          }
          drawSphere(dst, w, h, src, bx, by, r, r * 1.12F, 0.85F, t * 0.5F, 1.0F, 0.32F, 0.30F);
          plotDot(dst, w, h, bx, by + r * 1.06F, r * 0.13F, ya, Rgb{150, 30, 30, false});
        }
      });
}

void effectMaryPoppins(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  const float mn = std::min(static_cast<float>(w), h * ya);
  runFrames(
      renderer,
      w,
      h,
      5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        for (int y = 0; y < h; ++y)  // sky + drifting clouds (from data)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 0.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            const float sf = static_cast<float>(y) / h;
            const float cloud = std::clamp((l / 255.0F - 0.45F) * 2.0F, 0.0F, 1.0F) * 0.5F;
            dst[static_cast<std::size_t>(y) * w + x] = Rgb{u8(135 + 70 * sf + cloud * 110),
                                                           u8(170 + 55 * sf + cloud * 90),
                                                           u8(220 - 10 * sf + cloud * 40),
                                                           false};
          }
        const float rise = std::clamp((t - 0.10F) / 0.90F, 0.0F, 1.0F);
        const float sway = std::sin(t * 2.2F) * mn * 0.04F;
        const float cxp = w * 0.5F + sway;
        const float cyp = h * 0.72F - rise * h * 0.92F;  // canopy rim height
        const float r = mn * 0.20F;
        const int x0 = static_cast<int>(cxp - r), x1 = static_cast<int>(cxp + r);
        const int y0 = static_cast<int>(cyp - r * 0.85F), y1 = static_cast<int>(cyp);
        for (int y = std::max(0, y0); y <= std::min(h - 1, y1); ++y)
          for (int x = std::max(0, x0); x <= std::min(w - 1, x1); ++x)
          {
            const float nx = (x - cxp) / r, ny = (y - cyp) / (r * 0.85F);
            if (ny > 0 || nx * nx + ny * ny > 1.0F)
              continue;                                         // upper half-dome only
            const float scallop = 0.06F * std::sin(nx * 9.0F);  // scalloped rim
            if (ny > -scallop - 0.02F && y > cyp - 2)
              continue;
            const Rgb s = sample(src, w, h, x, y);
            const float dr = s.transparent ? 120 : s.r, dg = s.transparent ? 120 : s.g,
                        db = s.transparent ? 130 : s.b;
            const bool rib = std::fabs(std::fmod(std::atan2(-ny, nx) * 6.0F, 1.0F)) < 0.12F;
            dst[static_cast<std::size_t>(y) * w + x] =
                rib ? Rgb{40, 30, 40, false}
                    : Rgb{u8(dr * 0.55F + 30), u8(dg * 0.55F + 20), u8(db * 0.55F + 30), false};
          }
        drawSeg(dst,
                w,
                h,
                cxp,
                cyp - r * 0.85F,
                cxp,
                cyp - r * 1.0F,
                std::max(1.0F, mn * 0.01F),
                ya,
                Rgb{60, 50, 40, false});  // ferrule
        drawSeg(dst,
                w,
                h,
                cxp,
                cyp,
                cxp,
                cyp + r * 0.9F,
                std::max(1.0F, mn * 0.012F),
                ya,
                Rgb{90, 60, 40, false});  // handle shaft
        drawSeg(dst,
                w,
                h,
                cxp,
                cyp + r * 0.9F,
                cxp + r * 0.18F,
                cyp + r * 0.95F,
                std::max(1.0F, mn * 0.012F),
                ya,
                Rgb{90, 60, 40, false});                                 // crook
        const float fcx = cxp, ftop = cyp + r * 0.55F, fh = mn * 0.20F;  // the figure
        const Rgb coat{30, 32, 48, false};
        // Head + bowler hat brim.
        plotDot(dst, w, h, fcx, ftop, fh * 0.16F, ya, coat);
        drawSeg(dst, w, h, fcx - fh * 0.20F, ftop + fh * 0.02F, fcx + fh * 0.20F,
                ftop + fh * 0.02F, std::max(1.0F, mn * 0.005F), ya, coat);
        // Narrow bodice from neck to waist.
        const float waistY = ftop + fh * 0.36F;
        drawSeg(dst, w, h, fcx, ftop + fh * 0.16F, fcx, waistY, fh * 0.10F, ya, coat);
        // Bell skirt — almost a half-balloon, billowing from the waist down to
        // the hem. Carries the data: each interior pixel samples src and is
        // tinted toward a warm Poppins-skirt blue-grey so the cloud field
        // shows through the dress. A few darker vertical pleats add structure.
        const float skirtR = fh * 0.55F;            // rim half-width
        const float skirtH = fh * 0.55F;            // top-to-hem
        const float hemY = waistY + skirtH;
        for (int yy = static_cast<int>(waistY); yy <= static_cast<int>(hemY); ++yy)
        {
          const float v = (yy - waistY) / skirtH;
          // Half-balloon profile: starts narrow at the waist, bulges to ~0.7r
          // around v=0.7, settles to skirtR at the hem.
          const float bulge = std::sin(v * 3.14159F) * skirtR * 0.20F;
          const float halfW = fh * (0.10F + 0.45F * v) + bulge;
          for (int xo = -static_cast<int>(halfW); xo <= static_cast<int>(halfW); ++xo)
          {
            const int xx = static_cast<int>(fcx + xo);
            if (xx < 0 || xx >= w || yy < 0 || yy >= h) continue;
            const Rgb s = sample(src, w, h, xx, yy);
            const float sr = s.transparent ? 80.0F : s.r;
            const float sg = s.transparent ? 90.0F : s.g;
            const float sb = s.transparent ? 130.0F : s.b;
            // Vertical pleats — every ~12% of the rim get a darker stripe.
            const float pleat = std::fmod((xo / halfW + 1.0F) * 5.0F, 1.0F);
            const float pl = (pleat < 0.10F || pleat > 0.90F) ? 0.55F : 0.85F;
            dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{
                u8((40 + sr * 0.30F) * pl), u8((50 + sg * 0.30F) * pl),
                u8((80 + sb * 0.30F) * pl), false};
          }
        }
        // Tiny black booties poking out from below the hem.
        plotDot(dst, w, h, fcx - fh * 0.18F, hemY + fh * 0.03F, fh * 0.06F, ya, coat);
        plotDot(dst, w, h, fcx + fh * 0.18F, hemY + fh * 0.03F, fh * 0.06F, ya, coat);
        // Arm grasping the umbrella handle.
        drawSeg(dst, w, h, fcx, ftop + fh * 0.24F, cxp, cyp + r * 0.9F, fh * 0.05F, ya, coat);
        // Carpet bag in the other hand.
        plotDot(dst, w, h, fcx - fh * 0.30F, waistY + fh * 0.08F, fh * 0.10F, ya,
                Rgb{120, 80, 40, false});
        (void)hash;
      });
}

// Marionette: ONE large figure cycling through 12 Carnegie Mellon
// mocap motions (walk, run, sneak, ladder, jump, cartwheel, sit, wave,
// punch, kick, salsa, throw). Captured at 120 Hz, played back at the
// terminal redraw rate with linear inter-frame interpolation — at any
// terminal frame rate the motion stays buttery smooth because we sample
// the underlying 120 Hz dense trajectory.
//
// Each motion plays for ~3 seconds, with the motion name fading in at
// the start and fading out near the end. The figure is drawn as a
// capsule-silhouette puppet over the dimmed weather data.
void effectMarionette(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };

  struct Motion
  {
    const char* file;
    const char* label;
    float seconds;  // how long to display this motion in the cycle
  };
  static const std::array<Motion, 12> kMotions = {{
      {"walk",      "Walking",        3.0F},
      {"run",       "Running",        2.5F},
      {"sneak",     "Sneaking",       3.5F},
      {"ladder",    "Climbing",       3.5F},
      {"jump",      "Jumping",        2.5F},
      {"cartwheel", "Cartwheel",      2.0F},
      {"sit",       "Sitting down",   3.5F},
      {"wave",      "Waving",         2.5F},
      {"punch",     "Punching",       3.5F},
      {"kick",      "Kicking",        3.5F},
      {"salsa",     "Salsa",          4.0F},
      {"throw",     "Throwing",       3.0F},
  }};

  // Locate each BVH file via the same data-path resolution path the
  // image loader uses (findDataImage), then parse once.
  struct Loaded
  {
    BvhAnimation anim;
    double refH = 1.0;
    bool ok = false;
  };
  std::vector<Loaded> loaded(kMotions.size());
  for (std::size_t i = 0; i < kMotions.size(); ++i)
  {
    char rel[64];
    std::snprintf(rel, sizeof(rel), "cmu/%s.bvh", kMotions[i].file);
    const std::string path = findDataImage(rel);
    if (path.empty())
      continue;
    try
    {
      loaded[i].anim = loadBvhFile(path);
      loaded[i].refH = bvhReferenceHeight(loaded[i].anim);
      loaded[i].ok = true;
    }
    catch (const std::exception&)
    {
      loaded[i].ok = false;
    }
  }

  float totalSec = 0;
  for (const auto& m : kMotions)
    totalSec += m.seconds;
  const int totalMs = static_cast<int>(totalSec * 1000.0F);

  runFrames(
      renderer,
      w,
      h,
      totalMs,
      [&](float t, std::vector<Rgb>& dst)
      {
        // Find which motion is active at time t (0..1 over totalSec).
        const float secNow = t * totalSec;
        float acc = 0;
        std::size_t mi = 0;
        float local = secNow;
        for (std::size_t i = 0; i < kMotions.size(); ++i)
        {
          if (secNow < acc + kMotions[i].seconds)
          {
            mi = i;
            local = secNow - acc;
            break;
          }
          acc += kMotions[i].seconds;
        }
        const float motionT = std::clamp(local / kMotions[mi].seconds, 0.0F, 1.0F);

        // Dim weather data backdrop in cool slate-grey so the figure pops.
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 40.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(35 + l * 0.18F), u8(40 + l * 0.20F), u8(55 + l * 0.22F), false};
          }

        // Render the figure.
        const Loaded& L = loaded[mi];
        if (L.ok)
        {
          // Map motionT to a frame index. The animation plays once per
          // slot (no loop) — for cyclic motions this still shows several
          // full cycles since they're 1-2 s each. For one-shot motions
          // (jump, throw) the captured arc plays start-to-end.
          const float fIdx = motionT * (L.anim.frameCount - 1);
          const int frame = static_cast<int>(std::round(fIdx));
          const float figH = h * 0.78F;
          const float cx = w * 0.5F;
          const float cy = h * 0.93F;  // foot baseline near bottom
          drawMarionette(dst, w, h, ya, L.anim, frame,
                         cx, cy, figH, L.refH,
                         Rgb{240, 230, 215, false});
        }

        // Caption with motion name. Fade in over the first 0.25 of the
        // slot, hold, fade out over the last 0.20.
        float capAlpha = 1.0F;
        if (motionT < 0.25F) capAlpha = motionT / 0.25F;
        else if (motionT > 0.80F) capAlpha = (1.0F - motionT) / 0.20F;
        capAlpha = std::clamp(capAlpha, 0.0F, 1.0F);
        const Rgb capCol{u8(255 * capAlpha + 35 * (1 - capAlpha)),
                         u8(240 * capAlpha + 40 * (1 - capAlpha)),
                         u8(200 * capAlpha + 55 * (1 - capAlpha)), false};
        // Draw the label, scaled so longer words still fit. We pick the
        // largest integer pixel scale that keeps the rendered string
        // within 70 % of the screen width.
        const char* label = kMotions[mi].label;
        const int slen = static_cast<int>(std::strlen(label));
        constexpr int kFW = 5, kFH = 7, kGap = 1;
        const int totalFx = slen * kFW + (slen - 1) * kGap;
        const int scale = std::max(1, std::min(static_cast<int>(0.7F * w / totalFx),
                                              static_cast<int>(0.10F * h / kFH)));
        const int ox = (w - totalFx * scale) / 2;
        const int oy = static_cast<int>(h * 0.07F);
        for (int ci = 0; ci < slen; ++ci)
        {
          const auto g = glyph5x7(static_cast<char>(
              std::toupper(static_cast<unsigned char>(label[ci]))));
          const int charOx = ci * (kFW + kGap) * scale;
          for (int fy = 0; fy < kFH; ++fy)
            for (int fx = 0; fx < kFW; ++fx)
            {
              if (g[fy][fx] != '1') continue;
              for (int sy = 0; sy < scale; ++sy)
                for (int sx = 0; sx < scale; ++sx)
                {
                  const int xx = ox + charOx + fx * scale + sx;
                  const int yy = oy + fy * scale + sy;
                  if (xx >= 0 && xx < w && yy >= 0 && yy < h)
                    dst[static_cast<std::size_t>(yy) * w + xx] = capCol;
                }
            }
        }
      });
}

void effectMonolith(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float horizonY = h * 0.74F, monoCx = w * 0.5F;
  runFrames(renderer,
            w,
            h,
            5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float rise = std::clamp((t - 0.10F) / 0.42F, 0.0F, 1.0F);
              const float align = std::clamp((t - 0.52F) / 0.40F, 0.0F, 1.0F);
              const float sunY = horizonY - h * 0.10F - align * h * 0.34F;
              const float flare = std::pow(std::clamp((t - 0.88F) / 0.12F, 0.0F, 1.0F), 0.6F);
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 40.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  const float gd = std::hypot(x - monoCx, (y - sunY) * ya);
                  const float glow = std::exp(-gd / (0.5F * mn)) * (0.3F + 0.7F * align);
                  Rgb c;
                  if (y > horizonY)  // dim lunar ground from the data
                    c = Rgb{u8(28 + l * 0.18F + glow * 60),
                            u8(26 + l * 0.16F + glow * 60),
                            u8(30 + l * 0.14F + glow * 50),
                            false};
                  else  // dark sky
                    c = Rgb{u8(6 + glow * 230), u8(7 + glow * 210), u8(16 + glow * 150), false};
                  dst[static_cast<std::size_t>(y) * w + x] = c;
                }
              drawDataDisk(dst,
                           w,
                           h,
                           src,
                           monoCx,
                           sunY,
                           mn * 0.13F,
                           ya,
                           0.50F,
                           t * 0.3F,
                           Rgb{u8(255), u8(238), u8(180 + 60 * flare), false});  // sun
              const float crY = sunY - mn * 0.36F;                               // crescent above
              drawDataDisk(dst,
                           w,
                           h,
                           src,
                           monoCx,
                           crY,
                           mn * 0.075F,
                           ya,
                           0.75F,
                           t * 0.4F,
                           Rgb{220, 222, 210, false});
              plotDot(dst,
                      w,
                      h,
                      monoCx + mn * 0.028F,
                      crY - mn * 0.008F,
                      mn * 0.07F,
                      ya,
                      Rgb{8, 9, 18, false});
              const float mh = rise * h * 0.56F, mw = mn * 0.055F;  // the black slab
              for (int y = static_cast<int>(horizonY - mh); y <= static_cast<int>(horizonY); ++y)
                for (int x = static_cast<int>(monoCx - mw); x <= static_cast<int>(monoCx + mw); ++x)
                  if (x >= 0 && x < w && y >= 0 && y < h)
                    dst[static_cast<std::size_t>(y) * w + x] = Rgb{2, 2, 4, false};
              if (flare > 0.0F)
                for (std::size_t k = 0; k < dst.size(); ++k)
                  dst[k] = Rgb{u8(dst[k].r + (255 - dst[k].r) * flare * 0.7F),
                               u8(dst[k].g + (255 - dst[k].g) * flare * 0.7F),
                               u8(dst[k].b + (255 - dst[k].b) * flare * 0.7F),
                               false};
            });
}

void effectTron(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float horizon = h * 0.40F, cxw = w * 0.5F, A = h - horizon;
  runFrames(
      renderer,
      w,
      h,
      4800,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float scroll = t * 6.0F;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            Rgb out;
            if (y <= horizon)  // dark sky with a glow band at the horizon
            {
              const float gb = std::exp(-(horizon - y) / (h * 0.18F)) * 0.6F;
              out = Rgb{u8(8 + gb * 30), u8(14 + gb * 120), u8(26 + gb * 150), false};
            }
            else
            {
              const float v = A / (y - horizon);
              const float wu = (x - cxw) * v / (w * 0.5F);
              const float wv = v + scroll;
              const Rgb s = sample(src, w, h, (wu * 0.25F + 0.5F) * w, std::fmod(wv * 12.0F, h));
              const float du = std::fabs(wu / 0.5F - std::round(wu / 0.5F));
              const float dv = std::fabs(wv - std::round(wv));
              const float line = std::max(std::exp(-du * 18.0F), std::exp(-dv * 8.0F));
              const float base = s.transparent ? 20.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
              out = Rgb{u8(8 + base * 0.06F + line * 60),
                        u8(20 + base * 0.10F + line * 200),
                        u8(36 + base * 0.14F + line * 230),
                        false};
            }
            dst[static_cast<std::size_t>(y) * w + x] = out;
          }
        for (int c = 0; c < 2; ++c)  // light-cycle ribbons racing toward the viewer
        {
          const float lu = (c == 0 ? -0.32F : 0.30F);
          const Rgb col = (c == 0) ? Rgb{90, 230, 255, false} : Rgb{255, 170, 40, false};
          const float headV = 0.6F + (1.0F - t) * 5.0F;
          for (int k = 0; k <= 24; ++k)
          {
            const float v = headV + k * 0.5F;
            const float sy = horizon + A / v;
            const float sx = cxw + lu * w * 0.5F / v;
            if (sy > horizon && sy < h)
              plotDot(dst, w, h, sx, sy, std::max(1.0F, (h - sy) * 0.04F), ya, col);
          }
        }
        const float derez = std::pow(std::clamp((t - 0.9F) / 0.1F, 0.0F, 1.0F), 0.6F);
        if (derez > 0.0F)
          for (std::size_t k = 0; k < dst.size(); ++k)
            dst[k] = Rgb{u8(dst[k].r + (120 - dst[k].r) * derez),
                         u8(dst[k].g + (240 - dst[k].g) * derez),
                         u8(dst[k].b + (255 - dst[k].b) * derez),
                         false};
      });
}

void effectBoulder(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  const float mn = std::min(static_cast<float>(w), h * ya);
  runFrames(renderer,
            w,
            h,
            4800,
            [&](float t, std::vector<Rgb>& dst)
            {
              for (int y = 0; y < h; ++y)  // dim corridor from the data
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  dst[static_cast<std::size_t>(y) * w + x] =
                      s.transparent ? Rgb{14, 12, 10, false}
                                    : Rgb{u8(s.r * 0.28F), u8(s.g * 0.26F), u8(s.b * 0.22F), false};
                }
              const float grow = std::pow(t, 1.7F);
              const float r = mn * (0.10F + grow * 0.95F);
              const float bx = w * (0.5F + 0.18F * std::sin(t * 3.0F) * (1.0F - grow));
              const float by = h * (0.34F + grow * 0.30F);
              const float spin = t * 9.0F;
              for (int k = 0; k < 14; ++k)  // dust kicked up around the base
              {
                const float a = hash(k) * 6.2832F, dd = r * (0.8F + hash(k * 3) * 0.5F);
                plotDot(dst,
                        w,
                        h,
                        bx + std::cos(a) * dd,
                        by + r * 0.6F + std::sin(a) * r * 0.3F,
                        r * 0.18F,
                        ya,
                        Rgb{u8(90 + hash(k * 7) * 40), u8(82), u8(70), false});
              }
              drawSphere(dst, w, h, src, bx, by, r, r, 0.9F, spin, 0.62F, 0.55F, 0.48F);
              const float crush = std::clamp((t - 0.88F) / 0.12F, 0.0F, 1.0F);
              if (crush > 0.0F)
                for (std::size_t k = 0; k < dst.size(); ++k)
                  dst[k] = Rgb{u8(dst[k].r * (1 - crush) + 60 * crush),
                               u8(dst[k].g * (1 - crush) + 54 * crush),
                               u8(dst[k].b * (1 - crush) + 46 * crush),
                               false};
            });
}

void effectApocalypse(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float treeY = h * 0.78F, sunX = w * 0.5F, sunY = h * 0.62F;
  runFrames(
      renderer,
      w,
      h,
      5200,
      [&](float t, std::vector<Rgb>& dst)
      {
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 70.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            const float sd = std::hypot(x - sunX, (y - sunY) * ya);
            const float glow = std::exp(-sd / (0.55F * mn));
            Rgb c;
            if (y > treeY + std::sin(x * 0.20F) * h * 0.02F)  // dark jungle treeline
              c = Rgb{u8(14 + l * 0.04F), u8(10 + l * 0.03F), u8(8), false};
            else  // sunset haze from the data
            {
              const float sf = static_cast<float>(y) / treeY;
              c = Rgb{u8(120 + 120 * sf + glow * 110 + l * 0.12F),
                      u8(50 + 70 * sf + glow * 90 + l * 0.10F),
                      u8(30 + 20 * sf + glow * 40),
                      false};
            }
            dst[static_cast<std::size_t>(y) * w + x] = c;
          }
        drawDataDisk(dst,
                     w,
                     h,
                     src,
                     sunX,
                     sunY,
                     0.16F * mn,
                     ya,
                     0.55F,
                     t * 0.25F,
                     Rgb{255, 180, 90, false});  // hazy sun
        const Rgb heli{12, 8, 6, false};
        for (int i = 0; i < 4; ++i)  // choppers crossing at different depths
        {
          const float dep = 0.4F + 0.6F * (i / 3.0F);
          const float hx = std::fmod(t * (0.5F + dep) + i * 0.27F, 1.3F) * w - w * 0.15F;
          const float hy = h * (0.26F + 0.10F * i) + std::sin(t * 3.0F + i) * h * 0.01F;
          const float s = mn * 0.05F * dep;
          drawSeg(dst, w, h, hx - s * 1.6F, hy, hx + s * 0.8F, hy, s * 0.4F, ya, heli);  // body
          drawSeg(dst, w, h, hx + s * 0.8F, hy, hx + s * 2.0F, hy - s * 0.3F, s * 0.12F, ya, heli);
          const float rot = (0.6F + 0.4F * std::sin(t * 50.0F + i)) * s * 2.2F;  // blurred rotor
          drawSeg(dst, w, h, hx - rot, hy - s * 0.7F, hx + rot, hy - s * 0.7F, s * 0.1F, ya, heli);
          drawSeg(dst,
                  w,
                  h,
                  hx - s * 1.2F,
                  hy + s * 0.5F,
                  hx + s * 0.4F,
                  hy + s * 0.5F,
                  s * 0.1F,
                  ya,
                  heli);  // skids
        }
      });
}

void effectKong(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float towerX = w * 0.5F, towerTop = h * 0.34F;
  runFrames(
      renderer,
      w,
      h,
      5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        for (int y = 0; y < h; ++y)  // night sky + skyline silhouette from the data
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 0.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            const float sky = static_cast<float>(y) / h;
            const float skylineY =
                h * 0.62F + std::sin(x * 0.21F) * h * 0.04F + std::sin(x * 0.07F) * h * 0.05F;
            if (y > skylineY)  // buildings: dark with lit windows from data
            {
              const bool win =
                  (static_cast<int>(x) % 3 == 0) && (static_cast<int>(y) % 2 == 0) && l > 120;
              dst[static_cast<std::size_t>(y) * w + x] =
                  win ? Rgb{200, 190, 120, false} : Rgb{u8(10 + l * 0.04F), u8(10), u8(16), false};
            }
            else
              dst[static_cast<std::size_t>(y) * w + x] = Rgb{
                  u8(12 + 16 * (1 - sky)), u8(14 + 16 * (1 - sky)), u8(28 + 22 * (1 - sky)), false};
          }
        // pale moon behind the perch, so the ape reads as a silhouette
        plotDot(dst, w, h, towerX, towerTop - mn * 0.02F, mn * 0.26F, ya, Rgb{55, 58, 74, false});
        drawDataDisk(dst,
                     w,
                     h,
                     src,
                     towerX,
                     towerTop - mn * 0.02F,
                     mn * 0.17F,
                     ya,
                     0.70F,
                     t * 0.2F,
                     Rgb{206, 206, 198, false});
        {  // the tower (Empire State) rising to the perch
          const float tw = mn * 0.05F;
          for (int y = static_cast<int>(towerTop); y < h; ++y)
          {
            const float taper = (y < towerTop + mn * 0.14F) ? 0.45F : 1.0F;
            for (int x = static_cast<int>(towerX - tw * taper);
                 x <= static_cast<int>(towerX + tw * taper);
                 ++x)
              if (x >= 0 && x < w)
                dst[static_cast<std::size_t>(y) * w + x] =
                    ((static_cast<int>(y) % 2 == 0) && (static_cast<int>(x) % 2 == 0))
                        ? Rgb{120, 110, 80, false}
                        : Rgb{34, 32, 42, false};
          }
        }
        const float fall = std::clamp((t - 0.82F) / 0.18F, 0.0F, 1.0F);
        const float kongY = towerTop + fall * h * 0.8F;
        const float kongX = towerX + fall * fall * w * 0.12F;
        const float s = mn * 0.10F;
        const float roll = fall * 6.0F;
        if (fall < 1.0F)
        {
          const float ca = std::cos(roll), sa = std::sin(roll);
          auto rp = [&](float lx, float ly, float& ox, float& oy)
          {
            ox = kongX + lx * ca - ly * sa;
            oy = kongY + (lx * sa + ly * ca) / ya;
          };
          float bx, by, hx, hy, ax, ay;
          rp(0, 0, bx, by);
          rp(0, -s * 1.3F, hx, hy);
          const float swat = std::sin(t * 12.0F) * 0.6F;  // swatting arm
          rp(s * 1.1F, -s * (1.0F + swat), ax, ay);
          plotDot(dst, w, h, bx, by, s * 1.05F, ya, Rgb{50, 42, 36, false});  // body
          plotDot(dst, w, h, hx, hy, s * 0.55F, ya, Rgb{46, 38, 32, false});  // head
          drawSeg(dst, w, h, bx, by - s * 0.4F, ax, ay, s * 0.3F, ya, Rgb{50, 42, 36, false});
          drawSeg(dst,
                  w,
                  h,
                  bx,
                  by - s * 0.4F,
                  bx - s * 1.0F,
                  by - s * 0.2F,
                  s * 0.3F,
                  ya,
                  Rgb{50, 42, 36, false});  // gripping arm
        }
        for (int i = 0; i < 3; ++i)  // circling biplanes
        {
          const float a = t * 4.0F + i * 2.094F;
          const float px = towerX + std::cos(a) * mn * 0.34F;
          const float py = towerTop + std::sin(a) * mn * 0.20F - mn * 0.04F;
          const Rgb pl{20, 20, 26, false};
          drawSeg(dst, w, h, px - mn * 0.03F, py, px + mn * 0.03F, py, mn * 0.012F, ya, pl);
          drawSeg(dst, w, h, px, py - mn * 0.018F, px, py + mn * 0.018F, mn * 0.01F, ya, pl);
          if (static_cast<int>(t * 20.0F + i) % 4 == 0)  // muzzle flicker
            plotDot(dst, w, h, px - mn * 0.03F, py, mn * 0.012F, ya, Rgb{255, 220, 120, false});
        }
      });
}

void effectOz(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float cx = w * 0.5F, cy = h * 0.5F, mn = std::min(static_cast<float>(w), h * ya);
  runFrames(renderer,
            w,
            h,
            5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float desat = std::clamp(t / 0.35F, 0.0F, 1.0F);
              const float swirl = std::clamp((t - 0.45F) / 0.55F, 0.0F, 1.0F);
              const float spin = swirl * swirl * 12.0F;
              const float pull = 1.0F + swirl * 2.6F;
              const float vy = cy - swirl * cy * 1.1F;  // funnel climbs to the top
              const float fade = 1.0F - swirl * 0.7F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const float dx = x - cx, dy = (y - vy) * ya, r = std::hypot(dx, dy);
                  const float th = std::atan2(dy, dx) - spin / (r / mn + 0.2F);
                  const float r2 = r * pull;
                  const Rgb s =
                      sample(src, w, h, cx + std::cos(th) * r2, vy + std::sin(th) * r2 / ya);
                  const float l = s.transparent ? 0.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  const float br = s.transparent ? 0 : s.r, bg = s.transparent ? 0 : s.g,
                              bb = s.transparent ? 0 : s.b;
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8((br * (1 - desat) + l * 1.07F * desat) * fade),
                          u8((bg * (1 - desat) + l * 0.88F * desat) * fade),
                          u8((bb * (1 - desat) + l * 0.62F * desat) * fade),
                          false};
                }
              for (int c = 0; c < 3; ++c)  // ruby-slipper heel clicks
              {
                const float fl = std::exp(-std::pow((t - (0.36F + c * 0.045F)) / 0.018F, 2.0F));
                if (fl > 0.02F)
                  plotDot(dst,
                          w,
                          h,
                          cx,
                          h * 0.82F,
                          mn * 0.06F * fl,
                          ya,
                          Rgb{u8(255 * fl), u8(120 * fl), u8(150 * fl), false});
              }
            });
}

void effectNosferatu(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  runFrames(renderer,
            w,
            h,
            5200,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float climb = std::pow(t, 1.15F);
              const float shadowTop = h * (1.08F - climb * 1.2F);
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 0.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  const float edge = shadowTop + std::sin(x * 0.18F) * h * 0.015F;
                  dst[static_cast<std::size_t>(y) * w + x] =
                      (y > edge)
                          ? Rgb{3, 3, 6, false}
                          : Rgb{u8(l * 0.40F + 22), u8(l * 0.42F + 26), u8(l * 0.52F + 36), false};
                }
              const float hx = w * 0.5F, handBase = shadowTop + mn * 0.10F;  // clawed hand
              const Rgb shadow{2, 2, 5, false};
              plotDot(dst, w, h, hx, handBase, mn * 0.13F, ya, shadow);  // palm
              for (int f = 0; f < 5; ++f)
              {
                const float ang = (f - 2) * 0.34F + std::sin(t * 3.0F + f) * 0.05F;
                const float fl = mn * (0.42F + (f == 2 ? 0.06F : 0.0F));  // middle finger longest
                const float bx = hx + std::sin(ang) * mn * 0.05F;
                const float by = handBase - mn * 0.04F;
                const float tx = hx + std::sin(ang) * fl;
                const float tyy = handBase - std::cos(ang) * fl;
                // Tapered finger: thick at the knuckle, narrowing to a needle
                // point. Stamping shrinking dots along the line gives a smooth
                // bevel that drawSeg's uniform stroke can't.
                constexpr int kSteps = 24;
                for (int k = 0; k <= kSteps; ++k)
                {
                  const float u = static_cast<float>(k) / kSteps;
                  const float px = bx + (tx - bx) * u;
                  const float py = by + (tyy - by) * u;
                  const float rad = std::max(0.6F, mn * 0.028F * (1.0F - u * 0.95F));
                  plotDot(dst, w, h, px, py, rad, ya, shadow);
                }
                // Sharp continuation past the tip — a hairline barb that gives
                // the claw a distinct point rather than ending in a stub.
                const float barb = mn * 0.045F;
                drawSeg(dst,
                        w,
                        h,
                        tx,
                        tyy,
                        tx + std::sin(ang) * barb,
                        tyy - std::cos(ang) * barb,
                        std::max(1.0F, mn * 0.006F),
                        ya,
                        shadow);
              }
            });
}

void effectBirds(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  const float mn = std::min(static_cast<float>(w), h * ya);
  runFrames(
      renderer,
      w,
      h,
      5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float d = 0.72F - 0.32F * t;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            dst[static_cast<std::size_t>(y) * w + x] =
                s.transparent ? Rgb{6, 6, 10, false}
                              : Rgb{u8(s.r * d), u8(s.g * d), u8(s.b * d), false};
          }
        const int N = static_cast<int>(t * t * 420);
        const Rgb bird{6, 6, 8, false};
        for (int i = 0; i < N; ++i)
        {
          const float drift =
              (hash(i * 5) < 0.45F) ? std::sin(t * 2.0F + i) * mn * 0.06F : 0.0F;  // some in flight
          const float bx = hash(i * 2) * w + drift, by = hash(i * 2 + 1) * h;
          const float sz = mn * 0.018F * (0.7F + hash(i * 7) * 0.7F);
          drawSeg(dst, w, h, bx - sz, by, bx, by - sz * 0.5F, std::max(1.0F, sz * 0.4F), ya, bird);
          drawSeg(dst, w, h, bx, by - sz * 0.5F, bx + sz, by, std::max(1.0F, sz * 0.4F), ya, bird);
        }
      });
}

void effectPulp(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float caseX = w * 0.5F, caseY = h * 0.60F;
  const float cw = mn * 0.16F, ch = mn * 0.11F;
  runFrames(
      renderer,
      w,
      h,
      5000,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float open = std::clamp((t - 0.15F) / 0.55F, 0.0F, 1.0F);
        const float glow = std::pow(open, 1.3F) * (0.85F + 0.15F * std::sin(t * 9.0F));
        const float whiteout = std::clamp((t - 0.85F) / 0.15F, 0.0F, 1.0F);
        const float gy = caseY - ch * 0.6F;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float br = s.transparent ? 14 : s.r * 0.4F, bg = s.transparent ? 12 : s.g * 0.4F,
                        bb = s.transparent ? 18 : s.b * 0.45F;
            const float gd = std::hypot(x - caseX, (y - gy) * ya);
            const float g = glow * std::exp(-gd / (0.5F * mn)) * 1.5F;
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(br + g * 255), u8(bg + g * 205), u8(bb + g * 70), false};
          }
        for (int y = static_cast<int>(caseY - ch); y <= static_cast<int>(caseY + ch); ++y)
          for (int x = static_cast<int>(caseX - cw); x <= static_cast<int>(caseX + cw); ++x)
            if (x >= 0 && x < w && y >= 0 && y < h)
              dst[static_cast<std::size_t>(y) * w + x] = Rgb{26, 22, 20, false};  // case body
        const float gap = open * ch * 1.1F;  // the glowing opening as the lid lifts
        for (int y = static_cast<int>(caseY - ch - gap); y <= static_cast<int>(caseY - ch); ++y)
          for (int x = static_cast<int>(caseX - cw * 0.9F);
               x <= static_cast<int>(caseX + cw * 0.9F);
               ++x)
            if (x >= 0 && x < w && y >= 0 && y < h)
              dst[static_cast<std::size_t>(y) * w + x] = Rgb{255, 220, 110, false};
        if (whiteout > 0.0F)
          for (std::size_t k = 0; k < dst.size(); ++k)
            dst[k] = Rgb{u8(dst[k].r + (255 - dst[k].r) * whiteout),
                         u8(dst[k].g + (250 - dst[k].g) * whiteout),
                         u8(dst[k].b + (210 - dst[k].b) * whiteout),
                         false};
      });
}

void effectStrangelove(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float flashT = 0.56F;
  runFrames(
      renderer,
      w,
      h,
      5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        if (t < flashT)  // the ride down
        {
          for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
              const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
              const float l = s.transparent ? 0.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
              const float sf = static_cast<float>(y) / h;
              const float cl = std::clamp((l / 255.0F - 0.5F) * 2.0F, 0.0F, 1.0F) * 0.5F;
              dst[static_cast<std::size_t>(y) * w + x] = Rgb{u8(70 + 60 * sf + cl * 130),
                                                             u8(110 + 60 * sf + cl * 110),
                                                             u8(175 - 20 * sf + cl * 40),
                                                             false};
            }
          const float p = t / flashT;
          const float bx = w * 0.5F, by = h * (0.26F + 0.55F * p);
          const float s = mn * 0.14F * (1.0F - 0.65F * p);
          for (int i = -6; i <= 6; ++i)  // bomb body (capsule), data-tinted steel
          {
            const float f = i / 6.0F, along = f * s * 1.4F;
            const float rad = s * 0.34F * std::sqrt(std::max(0.0F, 1.0F - f * f));
            const Rgb d = sample(src, w, h, bx, by + along);
            plotDot(dst,
                    w,
                    h,
                    bx,
                    by + along,
                    std::max(1.0F, rad),
                    ya,
                    Rgb{u8(120 + d.r * 0.2F), u8(122 + d.g * 0.2F), u8(128 + d.b * 0.2F), false});
          }
          drawSeg(dst,
                  w,
                  h,
                  bx - s * 0.4F,
                  by - s * 1.3F,
                  bx + s * 0.4F,
                  by - s * 1.3F,
                  s * 0.1F,
                  ya,
                  Rgb{90, 90, 96, false});                    // tail fins
          const float wave = std::sin(t * 18.0F) * s * 0.5F;  // rider waving his hat
          plotDot(dst, w, h, bx, by - s * 0.4F, s * 0.18F, ya, Rgb{30, 24, 20, false});  // rider
          plotDot(
              dst, w, h, bx + wave, by - s * 0.9F, s * 0.14F, ya, Rgb{60, 45, 30, false});  // hat
        }
        else  // flash and mushroom cloud
        {
          const float mp = std::clamp((t - flashT) / 0.44F, 0.0F, 1.0F);
          const float groundY = h * 0.86F;
          for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
              dst[static_cast<std::size_t>(y) * w + x] =
                  (y > groundY) ? Rgb{40, 30, 22, false} : Rgb{30, 24, 30, false};
          const float capY = groundY - mp * h * 0.55F, capR = mn * (0.10F + mp * 0.34F);
          const float stemW = capR * 0.3F;
          for (int y = static_cast<int>(capY); y < static_cast<int>(groundY); ++y)  // stem
            for (int x = static_cast<int>(w * 0.5F - stemW);
                 x <= static_cast<int>(w * 0.5F + stemW);
                 ++x)
              if (x >= 0 && x < w && y >= 0 && y < h)
              {
                const Rgb d = sample(src, w, h, x, y);
                dst[static_cast<std::size_t>(y) * w + x] =
                    Rgb{u8(150 + d.r * 0.2F), u8(90 + d.g * 0.2F), u8(50), false};
              }
          for (int i = 0; i < 26; ++i)  // billowing cap from the data
          {
            const float a = hash(i) * 6.2832F, rad = std::sqrt(hash(i * 3)) * capR;
            const float px = w * 0.5F + std::cos(a) * rad, py = capY + std::sin(a) * rad * 0.55F;
            const Rgb d = sample(src, w, h, px, py);
            const float core = 1.0F - rad / capR;
            plotDot(dst,
                    w,
                    h,
                    px,
                    py,
                    capR * 0.3F,
                    ya,
                    Rgb{u8(120 + d.r * 0.25F + core * 130),
                        u8(78 + d.g * 0.2F + core * 70),
                        u8(46 + d.b * 0.1F),
                        false});
          }
          const float flash = std::exp(-std::pow((t - flashT) / 0.05F, 2.0F));
          if (flash > 0.01F)
            for (std::size_t k = 0; k < dst.size(); ++k)
              dst[k] = Rgb{u8(dst[k].r + (255 - dst[k].r) * flash),
                           u8(dst[k].g + (255 - dst[k].g) * flash),
                           u8(dst[k].b + (255 - dst[k].b) * flash),
                           false};
        }
      });
}

void effectAkira(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float roadY = h * 0.58F;
  runFrames(
      renderer,
      w,
      h,
      4600,
      [&](float t, std::vector<Rgb>& dst)
      {
        for (int y = 0; y < h; ++y)  // dark night road from the data
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float road = (y > roadY) ? 0.22F : 0.12F;
            dst[static_cast<std::size_t>(y) * w + x] =
                s.transparent ? Rgb{8, 8, 14, false}
                              : Rgb{u8(s.r * road), u8(s.g * road), u8(s.b * road + 6), false};
          }
        const float p = 1.0F - std::pow(1.0F - t, 2.2F);  // decelerating slide
        const float startX = -0.12F * w, bx = startX + 1.2F * w * p;
        const int steps = static_cast<int>((bx - startX) / 1.2F) + 1;
        for (int k = 0; k <= steps; ++k)  // glowing red light-trail (data-smeared)
        {
          const float f = static_cast<float>(k) / steps;  // 0 tail .. 1 head
          const float tx = startX + (bx - startX) * f;
          const float tyy = roadY + std::sin(f * 6.0F + t) * mn * 0.015F;
          const Rgb d = sample(src, w, h, tx, tyy);
          const float dr = d.transparent ? 40 : d.r;
          const float glow = 0.3F + 0.7F * f;  // brighter toward the head
          plotDot(dst,
                  w,
                  h,
                  tx,
                  tyy,
                  mn * (0.02F + 0.03F * f),
                  ya,
                  Rgb{u8((150 + dr * 0.4F) * glow + 60),
                      u8(30 * glow + d.g * 0.1F),
                      u8(40 * glow),
                      false});
        }
        const float s = mn * 0.08F;  // the bike, leaning into the slide
        const Rgb bike{210, 40, 40, false};
        drawSeg(dst,
                w,
                h,
                bx - s * 1.4F,
                roadY - s * 0.2F,
                bx + s * 0.9F,
                roadY - s * 0.5F,
                s * 0.4F,
                ya,
                bike);
        plotDot(dst, w, h, bx - s * 1.1F, roadY + s * 0.2F, s * 0.5F, ya, Rgb{20, 20, 26, false});
        plotDot(dst, w, h, bx + s * 0.7F, roadY, s * 0.5F, ya, Rgb{20, 20, 26, false});
        plotDot(dst,
                w,
                h,
                bx - s * 0.2F,
                roadY - s * 0.9F,
                s * 0.32F,
                ya,
                Rgb{200, 70, 60, false});  // hunched rider
        for (int sp = 0; sp < 5; ++sp)     // skid sparks
          plotDot(dst,
                  w,
                  h,
                  bx - s * 1.2F - sp * mn * 0.02F,
                  roadY + s * 0.3F,
                  std::max(1.0F, mn * 0.008F),
                  ya,
                  Rgb{255, u8(180 - sp * 20), 60, false});
      });
}

void effectClockwork(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float cx = w * 0.5F, cy = h * 0.5F, mn = std::min(static_cast<float>(w), h * ya);
  const float irisR = mn * 0.27F, eyeR = mn * 0.46F;
  runFrames(
      renderer,
      w,
      h,
      4800,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float blink = std::clamp((t - 0.82F) / 0.18F, 0.0F, 1.0F);
        const float dil = 0.42F + 0.30F * std::sin(t * 3.0F);  // pupil dilation
        const float lidTop = cy - eyeR + blink * eyeR, lidBot = cy + eyeR - blink * eyeR;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const float dx = x - cx, dy = (y - cy) * ya, r = std::hypot(dx, dy);
            Rgb c;
            if (r < irisR)  // the iris carries the data
            {
              const float ang = std::atan2(dy, dx);
              const Rgb d =
                  sample(src, w, h, (dx / irisR * 0.5F + 0.5F) * w, (dy / irisR * 0.5F + 0.5F) * h);
              const float stri = 0.78F + 0.22F * std::sin(ang * 38.0F);
              const float rim = (r > irisR * 0.86F) ? 0.5F : 1.0F;  // dark limbal ring
              if (r < irisR * dil)
                c = Rgb{6, 5, 8, false};  // pupil
              else
                c = Rgb{u8(d.r * stri * rim), u8(d.g * stri * rim), u8(d.b * stri * rim), false};
            }
            else if (r < eyeR)  // sclera, with a little curvature shading and veins
            {
              const float sh = 0.82F + 0.18F * (1.0F - r / eyeR);
              const float vein = (std::fabs(dx) > eyeR * 0.7F) ? 0.85F : 1.0F;
              c = Rgb{u8(228 * sh), u8(222 * sh * vein), u8(214 * sh * vein), false};
            }
            else
              c = Rgb{150, 110, 95, false};  // surrounding skin
            if (y < lidTop || y > lidBot)
              c = Rgb{170, 128, 110, false};  // eyelid skin closing
            dst[static_cast<std::size_t>(y) * w + x] = c;
          }
        if (blink < 0.5F)  // the spiked lashes on the lower lid (Alex's eye)
          for (int i = -3; i <= 3; ++i)
          {
            const float lx = cx + i * eyeR * 0.26F;
            const float ly = cy + eyeR * 0.62F;
            drawSeg(dst,
                    w,
                    h,
                    lx,
                    ly,
                    lx + i * mn * 0.02F,
                    ly + mn * 0.12F,
                    std::max(1.0F, mn * 0.012F),
                    ya,
                    Rgb{12, 10, 12, false});
          }
        if (blink >= 1.0F)
          for (std::size_t k = 0; k < dst.size(); ++k)
            dst[k] = Rgb{0, 0, 0, false};
      });
}

void effectShining(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float horizon = h * 0.46F, cxw = w * 0.5F, A = h - horizon;
  const float wallA = horizon;
  runFrames(
      renderer,
      w,
      h,
      5200,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float openT = std::clamp((t - 0.20F) / 0.18F, 0.0F, 1.0F);
        const float flow = std::clamp((t - 0.38F) / 0.50F, 0.0F, 1.0F);
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            Rgb out;
            if (y > horizon)
            {
              const float v = A / (y - horizon);
              const float wu = (x - cxw) * v / (w * 0.5F);
              const Rgb d = sample(src, w, h, (wu * 0.25F + 0.5F) * w, std::fmod(v * 22.0F, h));
              const float l = d.transparent ? 50 : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
              out = Rgb{u8(80 + l * 0.18F), u8(40 + l * 0.10F), u8(50 + l * 0.10F), false};
            }
            else
            {
              const float du = std::fabs(x - cxw) / (w * 0.5F);
              const float vv = wallA / (horizon - y + 1.0F);
              const float wally = (1.0F - du);
              const Rgb d = sample(src, w, h, std::fmod(vv * 26.0F, w), (du * 0.5F + 0.25F) * h);
              const float l = d.transparent ? 60 : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
              out = Rgb{u8(130 + l * 0.18F * wally),
                        u8(110 + l * 0.16F * wally),
                        u8(90 + l * 0.12F * wally),
                        false};
            }
            dst[static_cast<std::size_t>(y) * w + x] = out;
          }
        const float dw = std::max(2.0F, w * 0.06F), dh = std::max(3.0F, h * 0.10F);
        const float ey = horizon - dh * 0.5F;
        for (int yo = 0; yo <= static_cast<int>(dh); ++yo)
          for (int xo = -static_cast<int>(dw); xo <= static_cast<int>(dw); ++xo)
          {
            const int x = static_cast<int>(cxw) + xo, y = static_cast<int>(ey) + yo;
            if (x < 0 || x >= w || y < 0 || y >= h)
              continue;
            const float opx = std::fabs(static_cast<float>(xo)) / dw;
            const Rgb door{20, 12, 12, false}, mouth{180, 8, 8, false};
            dst[static_cast<std::size_t>(y) * w + x] = (opx < openT) ? mouth : door;
          }
        if (flow > 0.0F)
        {
          // Perspective flood: the blood pours from the door at the
          // vanishing point and floods the corridor floor. Its footprint
          // is the 1-point-perspective wedge — apex at the door rim
          // (cxw, horizon+dh/2), widening linearly to the bottom screen
          // corners at the camera. As the front advances from the door
          // toward the camera (yFront from horizon → h), the wedge fills
          // out into a trapezoid.
          const float doorRimY = horizon + dh * 0.5F;
          const float floorH = std::max(1.0F, static_cast<float>(h) - doorRimY);
          const float doorHalfW = dw * 0.5F;
          const float yFront = doorRimY + (static_cast<float>(h) - doorRimY) * flow;
          const int yLo = static_cast<int>(doorRimY) + 1;
          const int yHi = std::min(h - 1, static_cast<int>(yFront));
          for (int y = yLo; y <= yHi; ++y)
          {
            const float t01 = std::clamp((y - doorRimY) / floorH, 0.0F, 1.0F);
            const float halfWedge = doorHalfW + t01 * (w * 0.5F - doorHalfW);
            const int xLo = std::max(0, static_cast<int>(std::round(cxw - halfWedge)));
            const int xHi = std::min(w - 1, static_cast<int>(std::round(cxw + halfWedge)));
            for (int x = xLo; x <= xHi; ++x)
            {
              const float turb = 0.85F + 0.15F * std::sin((x - y) * 0.3F + t * 8.0F);
              // Deeper red toward the leading edge, lighter pooled blood
              // toward the door — gives the flood a sense of direction.
              const float depthOfPool = std::clamp(t01, 0.0F, 1.0F);
              const Rgb& base = dst[static_cast<std::size_t>(y) * w + x];
              dst[static_cast<std::size_t>(y) * w + x] =
                  Rgb{u8((180 + 60 * depthOfPool) * turb + base.r * 0.10F),
                      u8(14 * turb + base.g * 0.05F),
                      u8(14 * turb + base.b * 0.05F),
                      false};
            }
          }
          // Bright leading edge along the front, also clipped to the
          // wedge so it never crosses into the wall region.
          if (yHi > yLo)
          {
            const float t01 = std::clamp((yFront - doorRimY) / floorH, 0.0F, 1.0F);
            const float halfWedgeF = doorHalfW + t01 * (w * 0.5F - doorHalfW);
            const int xLoF = std::max(0, static_cast<int>(std::round(cxw - halfWedgeF)));
            const int xHiF = std::min(w - 1, static_cast<int>(std::round(cxw + halfWedgeF)));
            for (int x = xLoF; x <= xHiF; ++x)
            {
              const int yj =
                  static_cast<int>(yFront + std::sin(x * 0.4F + t * 6.0F) * 1.2F);
              if (yj >= 0 && yj < h)
                dst[static_cast<std::size_t>(yj) * w + x] = Rgb{255, 130, 130, false};
            }
          }
        }
      });
}

void effectDialM(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float cx = w * 0.5F, cy = h * 0.52F;
  const float R = mn * 0.30F;
  const float holeR = mn * 0.045F;
  const float ringR = R + holeR + 2.0F;
  const int mPos = 5;
  const float aPer = 6.2832F / 10.0F;
  runFrames(
      renderer,
      w,
      h,
      4800,
      [&](float t, std::vector<Rgb>& dst)
      {
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            dst[static_cast<std::size_t>(y) * w + x] =
                s.transparent
                    ? Rgb{12, 10, 14, false}
                    : Rgb{u8(s.r * 0.22F + 4), u8(s.g * 0.20F + 4), u8(s.b * 0.18F + 6), false};
          }
        float ang = 0.0F;
        if (t < 0.18F)
          ang = 0.0F;
        else if (t < 0.48F)
          ang = ((t - 0.18F) / 0.30F) * (aPer * mPos);
        else if (t < 0.78F)
        {
          const float k = (t - 0.48F) / 0.30F;
          ang = (1.0F - std::pow(k, 0.85F)) * (aPer * mPos);
        }
        else
          ang = 0.0F;
        const float ringFlash = (t > 0.80F) ? std::exp(-std::pow((t - 0.84F) / 0.04F, 2.0F)) +
                                                  std::exp(-std::pow((t - 0.92F) / 0.04F, 2.0F))
                                            : 0.0F;
        plotDot(dst, w, h, cx, cy, ringR, ya, Rgb{42, 36, 32, false});
        drawDataDisk(dst,
                     w,
                     h,
                     src,
                     cx,
                     cy,
                     R + holeR * 0.2F,
                     ya,
                     0.55F,
                     0.0F,
                     Rgb{215, 208, 195, false});  // bakelite face wraps the data
        plotDot(dst, w, h, cx, cy, R * 0.32F, ya, Rgb{40, 32, 28, false});
        for (int k = 0; k < 10; ++k)
        {
          const float a = -1.5708F + k * aPer + ang;
          const float hx = cx + std::sin(a) * R, hy = cy - std::cos(a) * R;
          plotDot(dst, w, h, hx, hy, holeR, ya, Rgb{20, 16, 16, false});
          if (k == mPos)
          {
            const auto g = glyph5x7('M');
            const int sc = std::max(1, static_cast<int>(holeR / 4.0F));
            for (int fy = 0; fy < 7; ++fy)
              for (int fx = 0; fx < 5; ++fx)
                if (g[fy][fx] == '1')
                  for (int sy = 0; sy < sc; ++sy)
                    for (int sx = 0; sx < sc; ++sx)
                    {
                      const int xp = static_cast<int>(hx - 2.5F * sc) + fx * sc + sx;
                      const int yp = static_cast<int>(hy - 3.5F * sc) + fy * sc + sy;
                      if (xp >= 0 && xp < w && yp >= 0 && yp < h)
                        dst[static_cast<std::size_t>(yp) * w + xp] = Rgb{230, 220, 200, false};
                    }
          }
        }
        const float pa = -1.5708F + 7.5F * aPer;
        plotDot(dst,
                w,
                h,
                cx + std::sin(pa) * (R + holeR * 0.5F),
                cy - std::cos(pa) * (R + holeR * 0.5F),
                holeR * 0.5F,
                ya,
                Rgb{60, 50, 44, false});
        if (t > 0.18F && t < 0.50F)
        {
          const float a = -1.5708F + mPos * aPer + ang;
          plotDot(dst,
                  w,
                  h,
                  cx + std::sin(a) * R,
                  cy - std::cos(a) * R,
                  holeR * 0.75F,
                  ya,
                  Rgb{220, 180, 150, false});
        }
        if (ringFlash > 0.02F)
          for (std::size_t k = 0; k < dst.size(); ++k)
            dst[k] = Rgb{u8(dst[k].r + (255 - dst[k].r) * ringFlash * 0.7F),
                         u8(dst[k].g + (240 - dst[k].g) * ringFlash * 0.7F),
                         u8(dst[k].b + (200 - dst[k].b) * ringFlash * 0.7F),
                         false};
        if (t > 0.96F)
          for (std::size_t k = 0; k < dst.size(); ++k)
            dst[k] = Rgb{0, 0, 0, false};
      });
}

void effectLebowski(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float horizon = h * 0.40F, cxw = w * 0.5F, A = h - horizon;
  struct Pin
  {
    float wu;
    float wv;
  };
  static const Pin pins[] = {{0.0F, 0.0F},
                             {-0.18F, 0.22F},
                             {0.18F, 0.22F},
                             {-0.34F, 0.44F},
                             {0.0F, 0.44F},
                             {0.34F, 0.44F},
                             {-0.50F, 0.66F},
                             {-0.17F, 0.66F},
                             {0.17F, 0.66F},
                             {0.50F, 0.66F}};
  runFrames(
      renderer,
      w,
      h,
      4800,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float approach = t * 3.2F;
        const float strike = std::clamp((t - 0.85F) / 0.10F, 0.0F, 1.0F);
        const float scroll = t * 4.0F;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            Rgb out;
            if (y <= horizon)
            {
              const float gb = std::exp(-(horizon - y) / (h * 0.2F)) * 0.3F;
              out = Rgb{u8(12 + gb * 80), u8(10 + gb * 60), u8(20 + gb * 40), false};
            }
            else
            {
              const float v = A / (y - horizon);
              const float wu = (x - cxw) * v / (w * 0.5F);
              const float wv = v + scroll;
              const bool gutter = std::fabs(wu) > 0.55F;
              const Rgb d = sample(src, w, h, (wu * 0.30F + 0.5F) * w, std::fmod(wv * 18.0F, h));
              const float l = d.transparent ? 70 : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
              if (gutter)
                out = Rgb{30, 22, 18, false};
              else
              {
                const float plank = std::sin(wv * 8.0F) * 0.08F;
                out = Rgb{u8(180 + l * 0.20F + plank * 30),
                          u8(120 + l * 0.16F + plank * 20),
                          u8(60 + l * 0.10F),
                          false};
              }
            }
            dst[static_cast<std::size_t>(y) * w + x] = out;
          }
        for (int i = 0; i < 10; ++i)
        {
          float pwv = 3.5F + pins[i].wv - approach;
          float pwu = pins[i].wu;
          if (strike > 0.0F)
          {
            const float spd = 0.4F + hash(i * 3) * 0.6F;
            pwu += (pins[i].wu < 0 ? -1.0F : (pins[i].wu > 0 ? 1.0F : (hash(i) - 0.5F))) * strike *
                   spd;
            pwv += strike * 0.6F;
          }
          if (pwv < 0.05F)
            continue;
          const float sy = horizon + A / pwv;
          const float sx = cxw + pwu * w * 0.5F / pwv;
          const float sz = mn * 0.18F / pwv;
          if (sy < 0 || sy > h + sz || sx < -sz || sx > w + sz)
            continue;
          plotDot(dst, w, h, sx, sy, sz * 0.5F, ya, Rgb{245, 240, 230, false});
          plotDot(dst, w, h, sx, sy - sz * 0.6F, sz * 0.3F, ya, Rgb{245, 240, 230, false});
          plotDot(dst, w, h, sx, sy - sz * 0.35F, sz * 0.18F, ya, Rgb{200, 30, 30, false});
        }
        const float flash = (t > 0.83F) ? std::exp(-std::pow((t - 0.86F) / 0.04F, 2.0F)) : 0.0F;
        if (flash > 0.01F)
          for (std::size_t k = 0; k < dst.size(); ++k)
            dst[k] = Rgb{u8(dst[k].r + (255 - dst[k].r) * flash),
                         u8(dst[k].g + (255 - dst[k].g) * flash),
                         u8(dst[k].b + (255 - dst[k].b) * flash),
                         false};
      });
}

void effectRocky(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float botX = w * 0.5F, botY = h * 0.96F;
  const float topY = h * 0.42F;
  constexpr int kSteps = 10;
  runFrames(
      renderer,
      w,
      h,
      5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float sun = std::clamp((t - 0.10F) / 0.8F, 0.0F, 1.0F);
        const float sunY = h * 0.50F - sun * h * 0.24F, sunX = w * 0.5F;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 40.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            const float sf = static_cast<float>(y) / h;
            const float sd = std::hypot(x - sunX, (y - sunY) * ya);
            const float glow = std::exp(-sd / (mn * 0.55F)) * sun;
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(60 + 90 * sf + l * 0.16F + glow * 120),
                    u8(70 + 60 * sf + l * 0.14F + glow * 90),
                    u8(120 - 60 * sf + l * 0.10F + glow * 30),
                    false};
          }
        drawDataDisk(dst,
                     w,
                     h,
                     src,
                     sunX,
                     sunY,
                     mn * 0.10F * (0.6F + 0.4F * sun),
                     ya,
                     0.55F,
                     t * 0.3F,
                     Rgb{u8(255), u8(220), u8(160), false});
        for (int i = 0; i < kSteps; ++i)
        {
          const float f0 = static_cast<float>(i) / kSteps;
          const float f1 = static_cast<float>(i + 1) / kSteps;
          const float sw0 = (1.0F - f0) * (w * 0.42F);
          const float sw1 = (1.0F - f1) * (w * 0.42F);
          const float sy = botY + (topY - botY) * f1;
          const Rgb stone{u8(95 - i * 3), u8(85 - i * 2), u8(75 - i * 2), false};
          const Rgb edge{u8(150 - i * 4), u8(140 - i * 4), u8(125 - i * 3), false};
          const int y0 = static_cast<int>(sy);
          const int y1 = static_cast<int>(botY + (topY - botY) * f0);
          for (int y = std::min(y0, y1); y <= std::max(y0, y1); ++y)
          {
            const float ff =
                static_cast<float>(y - y1) / std::max(1.0F, static_cast<float>(y0 - y1));
            const float halfw = sw1 + (sw0 - sw1) * (1.0F - ff);
            for (int x = static_cast<int>(botX - halfw); x <= static_cast<int>(botX + halfw); ++x)
              if (x >= 0 && x < w && y >= 0 && y < h)
                dst[static_cast<std::size_t>(y) * w + x] = stone;
          }
          for (int x = static_cast<int>(botX - sw1); x <= static_cast<int>(botX + sw1); ++x)
            if (x >= 0 && x < w && static_cast<int>(sy) >= 0 && static_cast<int>(sy) < h)
              dst[static_cast<std::size_t>(static_cast<int>(sy)) * w + x] = edge;
        }
        const float climb = std::clamp(t / 0.72F, 0.0F, 1.0F);
        const float victory = std::clamp((t - 0.72F) / 0.28F, 0.0F, 1.0F);
        const float fx = botX, fy = botY + (topY - botY) * climb;
        const float ht = mn * 0.14F;
        const Rgb body{30, 26, 30, false};
        plotDot(dst, w, h, fx, fy - ht * 1.0F, ht * 0.13F, ya, body);
        drawSeg(dst, w, h, fx, fy - ht * 0.92F, fx, fy - ht * 0.40F, ht * 0.12F, ya, body);
        if (victory < 0.05F)
        {
          const float stride = std::sin(t * 16.0F) * ht * 0.20F;
          drawSeg(dst, w, h, fx, fy - ht * 0.40F, fx - stride, fy, ht * 0.10F, ya, body);
          drawSeg(dst, w, h, fx, fy - ht * 0.40F, fx + stride, fy, ht * 0.10F, ya, body);
          const float armSwing = std::sin(t * 16.0F + 1.5708F) * ht * 0.18F;
          drawSeg(
              dst, w, h, fx, fy - ht * 0.78F, fx + armSwing, fy - ht * 0.52F, ht * 0.08F, ya, body);
          drawSeg(
              dst, w, h, fx, fy - ht * 0.78F, fx - armSwing, fy - ht * 0.52F, ht * 0.08F, ya, body);
        }
        else
        {
          const float vh = victory * ht * 0.6F;
          drawSeg(dst, w, h, fx, fy - ht * 0.40F, fx - ht * 0.10F, fy, ht * 0.10F, ya, body);
          drawSeg(dst, w, h, fx, fy - ht * 0.40F, fx + ht * 0.10F, fy, ht * 0.10F, ya, body);
          drawSeg(dst,
                  w,
                  h,
                  fx,
                  fy - ht * 0.78F,
                  fx - ht * 0.22F,
                  fy - ht * 1.10F - vh,
                  ht * 0.08F,
                  ya,
                  body);
          drawSeg(dst,
                  w,
                  h,
                  fx,
                  fy - ht * 0.78F,
                  fx + ht * 0.22F,
                  fy - ht * 1.10F - vh,
                  ht * 0.08F,
                  ya,
                  body);
        }
      });
}

void effectBoneCut(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float cutT = 0.56F;
  auto drawCapsule =
      [&](std::vector<Rgb>& dst, float cx, float cy, float L, float R, float ang, const Rgb& col)
  {
    const float dx = std::cos(ang) * L * 0.5F, dy = std::sin(ang) * L * 0.5F;
    drawSeg(dst, w, h, cx - dx, cy - dy, cx + dx, cy + dy, R, ya, col);
    plotDot(dst, w, h, cx - dx, cy - dy, R * 1.25F, ya, col);
    plotDot(dst, w, h, cx + dx, cy + dy, R * 1.25F, ya, col);
  };
  runFrames(
      renderer,
      w,
      h,
      5200,
      [&](float t, std::vector<Rgb>& dst)
      {
        if (t < cutT)  // the throw across the savanna sky
        {
          for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
              const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
              const float l = s.transparent ? 60.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
              const float sf = static_cast<float>(y) / h;
              dst[static_cast<std::size_t>(y) * w + x] =
                  (y > h * 0.78F)
                      ? Rgb{u8(60 + l * 0.22F), u8(40 + l * 0.16F), u8(28 + l * 0.10F), false}
                      : Rgb{u8(140 + 60 * sf + l * 0.15F),
                            u8(95 + 60 * sf + l * 0.12F),
                            u8(70 + 40 * sf + l * 0.10F),
                            false};
            }
          const float p = t / cutT;
          const float bx = w * (0.30F + 0.45F * p);
          const float by = h * 0.78F - std::sin(p * 3.14159F) * h * 0.55F;
          const float ang = p * 18.0F + 0.6F;  // spinning fast
          const float L = mn * 0.22F, R = mn * 0.022F;
          drawCapsule(dst, bx, by, L, R, ang, Rgb{240, 235, 222, false});
          plotDot(dst,
                  w,
                  h,
                  bx + std::cos(ang) * L * 0.5F,
                  by + std::sin(ang) * L * 0.5F,
                  R * 1.6F,
                  ya,
                  Rgb{240, 235, 222, false});  // knob ends slightly bigger
          plotDot(dst,
                  w,
                  h,
                  bx - std::cos(ang) * L * 0.5F,
                  by - std::sin(ang) * L * 0.5F,
                  R * 1.6F,
                  ya,
                  Rgb{240, 235, 222, false});
        }
        else  // the cut: same axis, now a spaceship in space
        {
          for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
              dst[static_cast<std::size_t>(y) * w + x] = Rgb{2, 3, 7, false};
          for (int i = 0; i < 90; ++i)  // stars seeded from the data's bright pixels
          {
            const int sx = static_cast<int>(hash(i * 2) * w),
                      sy = static_cast<int>(hash(i * 2 + 1) * h);
            if (sx < 0 || sx >= w || sy < 0 || sy >= h)
              continue;
            const Rgb& s = src[static_cast<std::size_t>(sy) * w + sx];
            const float l = s.transparent ? 100.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            if (l < 90.0F)
              continue;
            const float tw = 0.5F + 0.5F * std::sin(t * 6.0F + i);
            dst[static_cast<std::size_t>(sy) * w + sx] =
                Rgb{u8(190 + tw * 50), u8(190 + tw * 50), u8(210 + tw * 40), false};
          }
          const float p = (t - cutT) / (1.0F - cutT);
          const float bx = w * (0.74F + 0.08F * p), by = h * 0.20F + 6.0F * std::sin(p * 4.0F);
          const float ang = 0.6F + p * 0.4F;  // slow spin
          const float L = mn * 0.30F, R = mn * 0.030F;
          drawCapsule(dst, bx, by, L, R, ang, Rgb{200, 200, 215, false});
          // two fins near the tail
          const float fdx = std::cos(ang), fdy = std::sin(ang);
          const float tx = bx - fdx * L * 0.40F, ty = by - fdy * L * 0.40F;
          drawSeg(dst,
                  w,
                  h,
                  tx,
                  ty,
                  tx - fdy * mn * 0.04F,
                  ty + fdx * mn * 0.04F,
                  R * 0.6F,
                  ya,
                  Rgb{160, 160, 180, false});
          drawSeg(dst,
                  w,
                  h,
                  tx,
                  ty,
                  tx + fdy * mn * 0.04F,
                  ty - fdx * mn * 0.04F,
                  R * 0.6F,
                  ya,
                  Rgb{160, 160, 180, false});
          plotDot(dst,
                  w,
                  h,
                  bx + fdx * L * 0.5F,
                  by + fdy * L * 0.5F,
                  R * 1.6F,
                  ya,
                  Rgb{255, 245, 200, false});  // bright nose lamp
        }
      });
}

void effectPrideRock(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float horizonY = h * 0.66F, rockTop = h * 0.32F;
  runFrames(renderer,
            w,
            h,
            5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float sun = std::clamp((t - 0.12F) / 0.70F, 0.0F, 1.0F);
              const float sunX = w * 0.50F, sunY = horizonY - sun * h * 0.34F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 80.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  const float sf = static_cast<float>(y) / horizonY;
                  const float gd = std::hypot(x - sunX, (y - sunY) * ya);
                  const float glow = std::exp(-gd / (mn * 0.55F)) * sun;
                  Rgb c;
                  if (y > horizonY)  // dark savanna foreground
                    c = Rgb{u8(30 + l * 0.10F + glow * 60),
                            u8(22 + l * 0.08F + glow * 50),
                            u8(18 + l * 0.06F),
                            false};
                  else
                    c = Rgb{u8(140 + 110 * (1 - sf) + glow * 110 + l * 0.10F),
                            u8(70 + 90 * (1 - sf) + glow * 80 + l * 0.10F),
                            u8(40 + 30 * sf + glow * 30),
                            false};
                  dst[static_cast<std::size_t>(y) * w + x] = c;
                }
              drawDataDisk(dst,
                           w,
                           h,
                           src,
                           sunX,
                           sunY,
                           mn * 0.13F,
                           ya,
                           0.55F,
                           t * 0.3F,
                           Rgb{255, 220, 140, false});  // sun
              // Pride Rock silhouette: a tilted boulder rising from the horizon
              const float rx0 = w * 0.32F, rx1 = w * 0.68F;  // base
              const float rtx = w * 0.56F;                   // tip
              for (int y = static_cast<int>(rockTop); y <= static_cast<int>(horizonY) + 2; ++y)
              {
                const float f = (y - rockTop) / (horizonY - rockTop);
                const float lx = rx0 + (rtx - mn * 0.04F) * (1.0F - f) * 0.0F;
                (void)lx;
                // left silhouette edge ramps from tip to base
                const float xl = rtx - (rtx - rx0) * f;
                const float xr = rtx + (rx1 - rtx) * f;
                for (int x = static_cast<int>(xl); x <= static_cast<int>(xr); ++x)
                  if (x >= 0 && x < w)
                    dst[static_cast<std::size_t>(y) * w + x] = Rgb{18, 14, 10, false};
              }
              // figure on the tip with the cub raised
              const float lift = std::clamp((t - 0.35F) / 0.45F, 0.0F, 1.0F);
              const float fcx = rtx, fy = rockTop;
              const Rgb fig{6, 6, 8, false};
              plotDot(dst, w, h, fcx, fy - mn * 0.020F, mn * 0.018F, ya, fig);  // head
              drawSeg(
                  dst, w, h, fcx, fy - mn * 0.010F, fcx, fy + mn * 0.030F, mn * 0.012F, ya, fig);
              // arms reaching up holding the cub
              const float ay = fy - mn * 0.020F - lift * mn * 0.10F;
              drawSeg(dst,
                      w,
                      h,
                      fcx - mn * 0.015F,
                      fy - mn * 0.005F,
                      fcx - mn * 0.020F,
                      ay,
                      std::max(1.0F, mn * 0.010F),
                      ya,
                      fig);
              drawSeg(dst,
                      w,
                      h,
                      fcx + mn * 0.015F,
                      fy - mn * 0.005F,
                      fcx + mn * 0.020F,
                      ay,
                      std::max(1.0F, mn * 0.010F),
                      ya,
                      fig);
              if (lift > 0.05F)
                plotDot(dst,
                        w,
                        h,
                        fcx,
                        ay - mn * 0.012F,
                        mn * 0.022F * lift + 1.0F,
                        ya,
                        Rgb{u8(45), u8(34), u8(22), false});  // cub
            });
}

void effectPleasantville(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float seedX = w * 0.50F, seedY = h * 0.58F;
  runFrames(renderer,
            w,
            h,
            5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float desat = std::clamp(t / 0.28F, 0.0F, 1.0F);
              const float spread = std::clamp((t - 0.42F) / 0.46F, 0.0F, 1.0F);
              const float cR = spread * std::hypot(static_cast<float>(w), h * ya);
              const float fade = std::clamp((t - 0.92F) / 0.08F, 0.0F, 1.0F);
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 0.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  const float br = s.transparent ? 0 : s.r, bg = s.transparent ? 0 : s.g,
                              bb = s.transparent ? 0 : s.b;
                  const float d = std::hypot(x - seedX, (y - seedY) * ya);
                  // colour mix: how much colour does this pixel keep?
                  const float colorAmt = std::max(
                      1.0F - desat, std::clamp((cR - d) / (mn * 0.10F + 0.5F), 0.0F, 1.0F));
                  float r = l * (1 - colorAmt) + br * colorAmt;
                  float g = l * (1 - colorAmt) + bg * colorAmt;
                  float b = l * (1 - colorAmt) + bb * colorAmt;
                  if (desat >= 1.0F && spread > 0.0F && d < mn * 0.025F)  // the red seed bloom
                  {
                    const float k = 1.0F - d / (mn * 0.025F);
                    r = r + (255 - r) * k;
                    g = g * (1 - k);
                    b = b * (1 - k);
                  }
                  r *= (1 - fade);
                  g *= (1 - fade);
                  b *= (1 - fade);
                  dst[static_cast<std::size_t>(y) * w + x] = Rgb{u8(r), u8(g), u8(b), false};
                }
            });
}

void effectTatooine(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float horizonY = h * 0.62F;
  runFrames(
      renderer,
      w,
      h,
      5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float setp = std::clamp(t / 0.85F, 0.0F, 1.0F);
        const float sunY = horizonY - mn * 0.10F + setp * mn * 0.18F;  // both descend
        const float sepX = mn * 0.20F;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 70.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            const float gd1 = std::hypot(x - (w * 0.5F - sepX), (y - sunY) * ya);
            const float gd2 = std::hypot(x - (w * 0.5F + sepX), (y - sunY + mn * 0.02F) * ya);
            const float glow =
                std::exp(-gd1 / (mn * 0.40F)) + std::exp(-gd2 / (mn * 0.32F)) * 0.85F;
            Rgb c;
            if (y > horizonY + std::sin(x * 0.08F) * h * 0.012F)  // sand
              c = Rgb{u8(120 + l * 0.16F + glow * 60),
                      u8(75 + l * 0.12F + glow * 40),
                      u8(40 + l * 0.06F),
                      false};
            else  // dusk sky
            {
              const float sf = static_cast<float>(y) / horizonY;
              c = Rgb{u8(80 + 175 * sf + glow * 70 + l * 0.10F),
                      u8(50 + 90 * sf + glow * 70 + l * 0.08F),
                      u8(95 - 35 * sf + glow * 25 + l * 0.06F),
                      false};
            }
            dst[static_cast<std::size_t>(y) * w + x] = c;
          }
        drawDataDisk(dst,
                     w,
                     h,
                     src,
                     w * 0.5F - sepX,
                     sunY,
                     mn * 0.11F,
                     ya,
                     0.55F,
                     t * 0.3F,
                     Rgb{255, 215, 130, false});
        drawDataDisk(dst,
                     w,
                     h,
                     src,
                     w * 0.5F + sepX,
                     sunY + mn * 0.02F,
                     mn * 0.085F,
                     ya,
                     0.55F,
                     t * 0.45F,
                     Rgb{255, 175, 90, false});  // smaller, slightly lower sister sun
        // a small figure silhouette in the foreground
        const float fx = w * 0.30F, fy = horizonY + mn * 0.08F, ht = mn * 0.10F;
        const Rgb fig{6, 6, 10, false};
        plotDot(dst, w, h, fx, fy - ht * 1.0F, ht * 0.12F, ya, fig);
        drawSeg(dst, w, h, fx, fy - ht * 0.92F, fx, fy - ht * 0.40F, ht * 0.12F, ya, fig);
        drawSeg(dst, w, h, fx, fy - ht * 0.40F, fx - ht * 0.10F, fy, ht * 0.10F, ya, fig);
        drawSeg(dst, w, h, fx, fy - ht * 0.40F, fx + ht * 0.10F, fy, ht * 0.10F, ya, fig);
        drawSeg(
            dst, w, h, fx, fy - ht * 0.78F, fx - ht * 0.16F, fy - ht * 0.50F, ht * 0.07F, ya, fig);
        drawSeg(
            dst, w, h, fx, fy - ht * 0.78F, fx + ht * 0.16F, fy - ht * 0.50F, ht * 0.07F, ya, fig);
      });
}

void effectMemento(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float pw = mn * 0.42F, ph = mn * 0.52F;
  const float imW = pw * 0.86F, imH = ph * 0.66F;
  runFrames(
      renderer,
      w,
      h,
      5000,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float jx = std::sin(t * 32.0F) * 1.4F, jy = std::cos(t * 23.0F) * 1.0F;
        const float cxp = w * 0.5F + jx, cyp = h * 0.5F + jy;
        const float reveal = 1.0F - std::clamp(t / 0.85F, 0.0F, 1.0F);  // image strength
        const float endFade = std::clamp((t - 0.90F) / 0.10F, 0.0F, 1.0F);
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
            dst[static_cast<std::size_t>(y) * w + x] = Rgb{6, 6, 10, false};
        const int x0 = static_cast<int>(cxp - pw * 0.5F), x1 = static_cast<int>(cxp + pw * 0.5F);
        const int y0 = static_cast<int>(cyp - ph * 0.5F), y1 = static_cast<int>(cyp + ph * 0.5F);
        for (int y = std::max(0, y0); y <= std::min(h - 1, y1); ++y)
          for (int x = std::max(0, x0); x <= std::min(w - 1, x1); ++x)
            dst[static_cast<std::size_t>(y) * w + x] = Rgb{240, 238, 228, false};  // Polaroid card
        const float iY0 = cyp - ph * 0.45F, iY1 = cyp - ph * 0.45F + imH;
        const float iX0 = cxp - imW * 0.5F, iX1 = cxp + imW * 0.5F;
        for (int y = std::max(0, static_cast<int>(iY0));
             y <= std::min(h - 1, static_cast<int>(iY1));
             ++y)
          for (int x = std::max(0, static_cast<int>(iX0));
               x <= std::min(w - 1, static_cast<int>(iX1));
               ++x)
          {
            const float u = (x - iX0) / imW, v = (y - iY0) / imH;
            const Rgb d = sample(src, w, h, u * w, v * h);
            const float dr = d.transparent ? 200.0F : d.r, dg = d.transparent ? 200.0F : d.g,
                        db = d.transparent ? 200.0F : d.b;
            // image fades to the Polaroid's blank-white as it un-develops
            dst[static_cast<std::size_t>(y) * w + x] = Rgb{u8(dr * reveal + 240 * (1 - reveal)),
                                                           u8(dg * reveal + 238 * (1 - reveal)),
                                                           u8(db * reveal + 228 * (1 - reveal)),
                                                           false};
          }
        if (endFade > 0.0F)
          for (std::size_t k = 0; k < dst.size(); ++k)
            dst[k] = Rgb{u8(dst[k].r * (1 - endFade)),
                         u8(dst[k].g * (1 - endFade)),
                         u8(dst[k].b * (1 - endFade)),
                         false};
      });
}

void effectStandByMe(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float horizonY = h * 0.54F;
  runFrames(
      renderer,
      w,
      h,
      5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 60.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            const float sd = std::hypot(x - w * 0.5F, (y - horizonY + h * 0.02F) * ya);
            const float glow = std::exp(-sd / (mn * 0.50F));
            const float sf = static_cast<float>(y) / horizonY;
            Rgb c;
            if (y > horizonY)  // dim ground / cinder bed
              c = Rgb{u8(28 + l * 0.10F + glow * 80),
                      u8(20 + l * 0.08F + glow * 50),
                      u8(18 + l * 0.06F),
                      false};
            else  // sunset sky
              c = Rgb{u8(160 + 90 * (1 - sf) + glow * 80 + l * 0.10F),
                      u8(90 + 80 * (1 - sf) + glow * 70 + l * 0.10F),
                      u8(60 + 20 * sf + glow * 20),
                      false};
            dst[static_cast<std::size_t>(y) * w + x] = c;
          }
        drawDataDisk(dst,
                     w,
                     h,
                     src,
                     w * 0.5F,
                     horizonY - mn * 0.02F,
                     mn * 0.10F,
                     ya,
                     0.50F,
                     t * 0.3F,
                     Rgb{255, 200, 130, false});  // setting sun
        // rails + sleepers, converging from bottom to horizon
        const float vx = w * 0.5F, vy = horizonY;
        const float gauge = mn * 0.18F;  // half-rail spacing at the bottom
        const Rgb rail{30, 22, 18, false}, slpr{50, 36, 28, false};
        for (int k = 0; k < 18; ++k)  // sleepers
        {
          const float q = static_cast<float>(k) / 18.0F;
          const float qd = 1.0F - std::pow(1.0F - q, 2.0F);  // bunch up toward vanishing
          const float py = h + (vy - h) * qd;
          const float pw = gauge * (1.0F - qd) * 1.4F;
          drawSeg(dst,
                  w,
                  h,
                  vx - pw,
                  py,
                  vx + pw,
                  py,
                  std::max(1.0F, mn * 0.012F * (1.0F - qd) + 0.4F),
                  ya,
                  slpr);
        }
        drawSeg(dst, w, h, vx - gauge, h - 1.0F, vx, vy, std::max(1.0F, mn * 0.012F), ya, rail);
        drawSeg(dst, w, h, vx + gauge, h - 1.0F, vx, vy, std::max(1.0F, mn * 0.012F), ya, rail);
        // figures: 4 boys walking, advancing into the distance
        const float adv = t * 0.30F;
        const Rgb body{8, 8, 10, false};
        for (int i = 0; i < 4; ++i)
        {
          const float q = std::clamp(0.18F + i * 0.10F + adv, 0.0F, 0.95F);
          const float qd = 1.0F - std::pow(1.0F - q, 2.0F);
          const float fy = h + (vy - h) * qd;
          const float fx = vx + (i % 2 == 0 ? -1 : 1) * gauge * (1.0F - qd) * 0.45F;
          const float ht = mn * 0.12F * (1.0F - qd);
          if (ht < 1.5F)
            continue;
          plotDot(dst, w, h, fx, fy - ht * 1.0F, ht * 0.14F, ya, body);
          drawSeg(dst, w, h, fx, fy - ht * 0.92F, fx, fy - ht * 0.40F, ht * 0.12F, ya, body);
          const float stride = std::sin(t * 8.0F + i) * ht * 0.14F;
          drawSeg(dst, w, h, fx, fy - ht * 0.40F, fx - stride, fy, ht * 0.10F, ya, body);
          drawSeg(dst, w, h, fx, fy - ht * 0.40F, fx + stride, fy, ht * 0.10F, ya, body);
        }
      });
}

void effectShawshank(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float horizonY = h * 0.50F, beachY = h * 0.84F;
  runFrames(
      renderer,
      w,
      h,
      5600,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float bob = std::sin(t * 1.7F) * 1.5F;  // boat bobbing
        const float sunX = w * 0.5F + std::sin(t * 0.4F) * mn * 0.02F, sunY = horizonY - mn * 0.02F;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 80.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            Rgb c;
            if (y > beachY)  // sand
              c = Rgb{u8(225 + l * 0.06F), u8(205 + l * 0.06F), u8(160 + l * 0.05F), false};
            else if (y > horizonY)  // sea: rippled, with a vertical sun glint column
            {
              const float ripple = 0.85F + 0.15F * std::sin(x * 0.40F + y * 0.30F + t * 6.0F);
              const float glint = std::exp(-std::fabs(x - sunX) / (mn * 0.04F)) *
                                  std::clamp(1.0F - (y - horizonY) / (h - horizonY), 0.0F, 1.0F);
              c = Rgb{u8(70 * ripple + glint * 200 + l * 0.06F),
                      u8(130 * ripple + glint * 200 + l * 0.06F),
                      u8(165 * ripple + glint * 120 + l * 0.06F),
                      false};
            }
            else  // warm sky
            {
              const float sf = static_cast<float>(y) / horizonY;
              c = Rgb{u8(180 + 60 * sf + l * 0.08F),
                      u8(195 + 40 * sf + l * 0.08F),
                      u8(225 - 20 * sf + l * 0.06F),
                      false};
            }
            dst[static_cast<std::size_t>(y) * w + x] = c;
          }
        drawDataDisk(dst,
                     w,
                     h,
                     src,
                     sunX,
                     sunY,
                     mn * 0.08F,
                     ya,
                     0.45F,
                     t * 0.3F,
                     Rgb{255, 245, 200, false});
        // a tiny boat moored offshore
        const float bx = w * 0.62F, by = horizonY + mn * 0.05F + bob;
        const Rgb hull{40, 30, 22, false};
        drawSeg(dst, w, h, bx - mn * 0.04F, by, bx + mn * 0.04F, by, mn * 0.012F, ya, hull);
        drawSeg(dst,
                w,
                h,
                bx - mn * 0.03F,
                by,
                bx - mn * 0.045F,
                by + mn * 0.012F,
                mn * 0.008F,
                ya,
                hull);
        drawSeg(dst,
                w,
                h,
                bx + mn * 0.03F,
                by,
                bx + mn * 0.045F,
                by + mn * 0.012F,
                mn * 0.008F,
                ya,
                hull);
        drawSeg(dst, w, h, bx, by, bx, by - mn * 0.06F, std::max(1.0F, mn * 0.006F), ya, hull);
        drawSeg(dst,
                w,
                h,
                bx,
                by - mn * 0.06F,
                bx + mn * 0.03F,
                by - mn * 0.02F,
                std::max(1.0F, mn * 0.005F),
                ya,
                Rgb{235, 230, 220, false});  // sail
      });
}

void effectGlobeDance(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float floorY = h * 0.86F;

  // Marionette puppet driven by the CMU salsa motion — gives the
  // figure continuous hip swing and arm sway like Chaplin's globe
  // ballet from The Great Dictator. The globe bounces off body parts
  // in a choreographed sequence including the iconic *behind* bounce.
  BvhAnimation anim;
  double animRefH = 1.0;
  bool animOk = false;
  const std::string animPath = findDataImage("cmu/salsa.bvh");
  if (!animPath.empty())
  {
    try
    {
      anim = loadBvhFile(animPath);
      animRefH = bvhReferenceHeight(anim);
      animOk = true;
    }
    catch (const std::exception&) { animOk = false; }
  }

  // Pre-cache joint indices used for ball-bouncing contact points.
  const int idxHead  = animOk ? anim.jointIndex("Head")      : -1;
  const int idxHips  = animOk ? anim.jointIndex("Hips")      : -1;
  const int idxLHand = animOk ? anim.jointIndex("LeftHand")  : -1;
  const int idxRHand = animOk ? anim.jointIndex("RightHand") : -1;
  const int idxLFoot = animOk ? anim.jointIndex("LeftFoot")  : -1;
  const int idxRFoot = animOk ? anim.jointIndex("RightFoot") : -1;

  runFrames(
      renderer,
      w,
      h,
      5600,
      [&](float t, std::vector<Rgb>& dst)
      {
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 40.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            dst[static_cast<std::size_t>(y) * w + x] =
                (y > floorY)
                    ? Rgb{40, 30, 22, false}
                    : Rgb{u8(28 + l * 0.18F), u8(22 + l * 0.16F), u8(38 + l * 0.18F), false};
          }

        // Figure: marionette in dark suit, roughly half the screen
        // tall (was full-height in v30 — too dominant). Feet stand on
        // the chancellery floor line.
        const float fcx = w * 0.5F;
        const float figH = h * 0.50F;
        const Rgb suit{30, 28, 32, false};
        std::vector<std::array<double, 2>> joints;
        if (animOk)
        {
          // Loop the salsa ~3× over the runtime so hips visibly sway.
          const float phase = t * 3.0F;
          const int fi = static_cast<int>(std::floor(phase * anim.frameCount)) %
                         std::max(1, anim.frameCount);
          drawMarionette(dst, w, h, ya, anim, fi, fcx, floorY, figH, animRefH,
                         suit, &joints);
        }

        // Mustache anchored to the head joint.
        if (idxHead >= 0 && idxHead < static_cast<int>(joints.size()))
        {
          const double hx = joints[idxHead][0];
          const double hy = joints[idxHead][1];
          plotDot(dst, w, h, hx, hy + mn * 0.006F, mn * 0.006F, ya,
                  Rgb{14, 12, 14, false});
        }

        // Globe choreography: bounce off six contact points in a loop —
        // Head → RightHand → RightFoot → Hips (behind) → LeftFoot →
        // LeftHand → back to Head. The Hips contact is offset slightly
        // below and behind the hip joint so the bounce reads as "ball
        // hitting the butt" the way Chaplin played it.
        struct Contact
        {
          int idx;        // joint index in the BVH skeleton
          double offX;    // screen-x offset from the joint, fraction of mn
          double offY;    // screen-y offset (negative = above joint)
        };
        const Contact contacts[] = {
            {idxHead,  0.00, -0.06},  // above the head
            {idxRHand, 0.04, -0.02},  // at the right hand
            {idxRFoot, 0.02, -0.02},  // foot kick
            {idxHips,  0.06,  0.04},  // BEHIND/below the hips — the butt bounce
            {idxLFoot,-0.02, -0.02},
            {idxLHand,-0.04, -0.02},
        };
        const int nC = static_cast<int>(sizeof(contacts) / sizeof(contacts[0]));

        // 6 contacts per ~2 s loop, so the ball goes through the full
        // cycle ~3 times over the 5.6 s effect — matches the salsa loop.
        const float beats = t * 18.0F;
        const int beatIdx = static_cast<int>(std::floor(beats)) % nC;
        const int nextIdx = (beatIdx + 1) % nC;
        const float frac = beats - std::floor(beats);

        auto contactScreen = [&](const Contact& c) -> std::pair<double, double>
        {
          double cx = fcx, cy = h * 0.5F;
          if (c.idx >= 0 && c.idx < static_cast<int>(joints.size()))
          {
            cx = joints[c.idx][0];
            cy = joints[c.idx][1];
          }
          return {cx + c.offX * mn, cy + c.offY * mn / ya};
        };
        const auto from = contactScreen(contacts[beatIdx]);
        const auto to   = contactScreen(contacts[nextIdx]);
        // Parabolic arc between contacts — peak rises above the segment
        // by ~10 % of mn so the ball clearly arcs rather than sliding.
        const double midX = from.first + (to.first - from.first) * frac;
        const double midY = from.second + (to.second - from.second) * frac;
        const double arcH = mn * 0.10F;
        const float ghX = static_cast<float>(midX);
        const float ghY = static_cast<float>(midY - arcH * 4.0 * frac * (1.0 - frac));
        const float gR = mn * 0.07F;  // globe was 0.10 — scaled down with figure
        drawSphere(dst, w, h, src, ghX, ghY, gR, gR, 0.45F, t * 1.2F, 0.45F, 0.65F, 0.55F);
        plotDot(dst,
                w,
                h,
                ghX - gR * 0.42F,
                ghY - gR * 0.42F,
                gR * 0.15F,
                ya,
                Rgb{255, 255, 250, false});  // specular highlight
      });
}

void effectSpiritedTrain(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float waterY = h * 0.54F, trackY = h * 0.62F;
  runFrames(renderer,
            w,
            h,
            5600,
            [&](float t, std::vector<Rgb>& dst)
            {
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 70.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  const float sf = static_cast<float>(y) / h;
                  Rgb c;
                  if (y > waterY)  // shallow sea (data reflected)
                  {
                    const float ripple = 0.86F + 0.14F * std::sin(x * 0.30F - t * 4.0F);
                    const float refl = sample(src, w, h, x, 2.0F * waterY - y).transparent
                                           ? 60.0F
                                           : (0.3F * sample(src, w, h, x, 2.0F * waterY - y).r +
                                              0.59F * sample(src, w, h, x, 2.0F * waterY - y).g +
                                              0.11F * sample(src, w, h, x, 2.0F * waterY - y).b);
                    c = Rgb{u8(50 + refl * 0.12F + ripple * 20),
                            u8(85 + refl * 0.16F + ripple * 30),
                            u8(120 + refl * 0.18F + ripple * 40),
                            false};
                  }
                  else  // sky
                    c = Rgb{u8(180 + 40 * (1 - sf) + l * 0.10F),
                            u8(165 + 50 * (1 - sf) + l * 0.10F),
                            u8(190 - 20 * sf + l * 0.08F),
                            false};
                  dst[static_cast<std::size_t>(y) * w + x] = c;
                }
              // rails just above the waterline
              drawSeg(dst,
                      w,
                      h,
                      0,
                      trackY,
                      w - 1,
                      trackY,
                      std::max(1.0F, mn * 0.006F),
                      ya,
                      Rgb{40, 30, 24, false});
              drawSeg(dst,
                      w,
                      h,
                      0,
                      trackY + mn * 0.012F,
                      w - 1,
                      trackY + mn * 0.012F,
                      std::max(1.0F, mn * 0.006F),
                      ya,
                      Rgb{40, 30, 24, false});
              // train: dark silhouette with lit windows, gliding across
              const float bx0 = -mn * 0.5F + t * (w + mn * 1.0F);
              const float bH = mn * 0.10F;
              const Rgb body{18, 16, 22, false};
              // engine
              const float ex = bx0;
              drawSeg(dst,
                      w,
                      h,
                      ex - mn * 0.10F,
                      trackY - bH * 0.4F,
                      ex + mn * 0.10F,
                      trackY - bH * 0.4F,
                      bH * 0.7F,
                      ya,
                      body);
              plotDot(
                  dst, w, h, ex - mn * 0.12F, trackY - bH * 0.55F, bH * 0.18F, ya, body);  // funnel
              plotDot(dst,
                      w,
                      h,
                      ex - mn * 0.12F,
                      trackY - bH * 0.78F,
                      bH * 0.12F,
                      ya,
                      Rgb{230, 220, 200, false});  // steam puff
              // carriages with lit windows
              for (int c = 1; c <= 3; ++c)
              {
                const float cx0 = bx0 - mn * (0.20F + c * 0.24F);
                drawSeg(dst,
                        w,
                        h,
                        cx0 - mn * 0.10F,
                        trackY - bH * 0.35F,
                        cx0 + mn * 0.10F,
                        trackY - bH * 0.35F,
                        bH * 0.65F,
                        ya,
                        body);
                for (int wnd = 0; wnd < 3; ++wnd)
                {
                  const float wx = cx0 - mn * 0.07F + wnd * mn * 0.07F;
                  plotDot(dst,
                          w,
                          h,
                          wx,
                          trackY - bH * 0.4F,
                          mn * 0.012F,
                          ya,
                          Rgb{255, 220, 130, false});  // window
                }
              }
            });
}

void effectTotoro(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float groundY = h * 0.88F;
  runFrames(
      renderer,
      w,
      h,
      5600,
      [&](float t, std::vector<Rgb>& dst)
      {
        for (int y = 0; y < h; ++y)  // forest dusk from data
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 50.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            const float sf = static_cast<float>(y) / h;
            dst[static_cast<std::size_t>(y) * w + x] = (y > groundY)
                                                           ? Rgb{20, 24, 18, false}
                                                           : Rgb{u8(40 + 30 * sf + l * 0.14F),
                                                                 u8(50 + 30 * sf + l * 0.16F),
                                                                 u8(58 + 20 * sf + l * 0.14F),
                                                                 false};
          }
        const float tx = w * 0.58F, ty = groundY - mn * 0.20F;  // Totoro centre-right
        const Rgb fur{24, 26, 30, false};
        // belly: the data wrapped on a softly-lit sphere, kept dark by the
        // low-luminance tint so Totoro still reads as a night silhouette.
        drawDataDisk(dst, w, h, src, tx, ty, mn * 0.20F, ya, 0.55F, 0.10F, Rgb{60, 64, 72, false});
        plotDot(dst,
                w,
                h,
                tx - mn * 0.06F,
                ty - mn * 0.18F,
                mn * 0.03F,
                ya,
                Rgb{240, 240, 240, false});  // big eye L
        plotDot(dst,
                w,
                h,
                tx + mn * 0.06F,
                ty - mn * 0.18F,
                mn * 0.03F,
                ya,
                Rgb{240, 240, 240, false});  // big eye R
        plotDot(dst,
                w,
                h,
                tx - mn * 0.06F,
                ty - mn * 0.18F,
                mn * 0.012F,
                ya,
                Rgb{8, 8, 10, false});  // pupil L
        plotDot(dst, w, h, tx + mn * 0.06F, ty - mn * 0.18F, mn * 0.012F, ya, Rgb{8, 8, 10, false});
        plotDot(dst, w, h, tx, ty - mn * 0.14F, mn * 0.014F, ya, Rgb{40, 28, 22, false});  // nose
        // ears
        drawSeg(dst,
                w,
                h,
                tx - mn * 0.10F,
                ty - mn * 0.18F,
                tx - mn * 0.14F,
                ty - mn * 0.30F,
                mn * 0.02F,
                ya,
                fur);
        drawSeg(dst,
                w,
                h,
                tx + mn * 0.10F,
                ty - mn * 0.18F,
                tx + mn * 0.14F,
                ty - mn * 0.30F,
                mn * 0.02F,
                ya,
                fur);
        // umbrella held over Totoro
        const float uHx = tx + mn * 0.18F, uHy = ty - mn * 0.10F;
        drawSeg(dst,
                w,
                h,
                uHx,
                uHy,
                uHx,
                uHy - mn * 0.22F,
                std::max(1.0F, mn * 0.010F),
                ya,
                Rgb{60, 44, 30, false});
        for (int k = -5; k <= 5; ++k)  // umbrella canopy dome
        {
          const float a = k / 5.0F;
          plotDot(dst,
                  w,
                  h,
                  uHx + a * mn * 0.18F,
                  uHy - mn * 0.22F - (1.0F - a * a) * mn * 0.06F,
                  mn * 0.022F,
                  ya,
                  Rgb{60, 44, 30, false});
        }
        // little Mei beside him
        const float mcx = w * 0.32F, mcy = groundY - mn * 0.08F;
        drawSeg(
            dst, w, h, mcx, mcy - mn * 0.08F, mcx, mcy, mn * 0.018F, ya, Rgb{200, 60, 50, false});
        plotDot(dst, w, h, mcx, mcy - mn * 0.10F, mn * 0.014F, ya, Rgb{200, 160, 120, false});
        // rain
        for (int i = 0; i < 150; ++i)
        {
          const float rx = std::fmod(hash(i * 2) * w + t * w * 0.05F + i * 0.8F, (float)w);
          const float ry = std::fmod(hash(i * 2 + 1) * h + t * h * 2.0F + i * 0.4F, (float)h);
          drawSeg(dst,
                  w,
                  h,
                  rx,
                  ry,
                  rx - mn * 0.012F,
                  ry + mn * 0.03F,
                  std::max(1.0F, mn * 0.004F),
                  ya,
                  Rgb{170, 200, 220, false});
        }
        // raindrops bouncing off Totoro's belly
        for (int b = 0; b < 8; ++b)
        {
          const float a = hash(b * 13 + static_cast<int>(t * 6.0F));
          const float bx = tx + (a - 0.5F) * mn * 0.36F;
          const float by =
              ty - mn * 0.04F + std::fmod(t * 4.0F + b * 0.13F, 1.0F) * mn * 0.10F - mn * 0.05F;
          plotDot(dst, w, h, bx, by, std::max(1.0F, mn * 0.008F), ya, Rgb{220, 230, 240, false});
        }
      });
}

void effectBigKeyboard(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float keysTop = h * 0.40F, keysBot = h * 0.95F;
  constexpr int kKeys = 12;
  runFrames(
      renderer,
      w,
      h,
      5600,
      [&](float t, std::vector<Rgb>& dst)
      {
        for (int y = 0; y < h; ++y)  // toy-store room dimly tinted by the data
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 30.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            dst[static_cast<std::size_t>(y) * w + x] =
                (y < keysTop)
                    ? Rgb{u8(34 + l * 0.18F), u8(28 + l * 0.16F), u8(40 + l * 0.20F), false}
                    : Rgb{6, 6, 10, false};
          }
        // the white-key floor (12 keys across) — each key's ivory carries the
        // data, so the dance floor is the weather you've been looking at
        const float keyW = static_cast<float>(w) / kKeys;
        const float lit = std::fmod(t * 5.0F, kKeys);
        const int litI = static_cast<int>(lit) % kKeys;
        for (int k = 0; k < kKeys; ++k)
        {
          const float x0 = k * keyW + 1.0F, x1 = (k + 1) * keyW - 1.0F;
          const bool litKey = (k == litI);
          const float k_lit = litKey ? (1.0F - (lit - litI)) : 0.0F;
          for (int y = static_cast<int>(keysTop); y <= static_cast<int>(keysBot); ++y)
            for (int x = static_cast<int>(x0); x <= static_cast<int>(x1); ++x)
            {
              if (x < 0 || x >= w || y < 0 || y >= h)
                continue;
              const Rgb d = sample(src, w, h, x, y);
              const float dr = d.transparent ? 130.0F : d.r;
              const float dg = d.transparent ? 130.0F : d.g;
              const float db = d.transparent ? 130.0F : d.b;
              const float baseR = litKey ? (235.0F + 20.0F * k_lit) : 235.0F;
              const float baseG = litKey ? (220.0F + 30.0F * k_lit) : 230.0F;
              const float baseB = litKey ? (110.0F + 90.0F * k_lit) : 220.0F;
              const float ivR = baseR * 0.68F + dr * 0.28F;
              const float ivG = baseG * 0.68F + dg * 0.28F;
              const float ivB = baseB * 0.68F + db * 0.25F;
              dst[static_cast<std::size_t>(y) * w + x] = Rgb{u8(ivR), u8(ivG), u8(ivB), false};
            }
        }
        // black keys (groups of 2 and 3 between whites)
        for (int k = 0; k < kKeys - 1; ++k)
        {
          const int p = k % 7;
          if (p == 2 || p == 6)
            continue;  // skip the gaps E-F and B-C
          const float bx = (k + 1) * keyW;
          const float bw = keyW * 0.55F;
          for (int y = static_cast<int>(keysTop);
               y <= static_cast<int>(keysTop + (keysBot - keysTop) * 0.62F);
               ++y)
            for (int x = static_cast<int>(bx - bw * 0.5F); x <= static_cast<int>(bx + bw * 0.5F);
                 ++x)
              if (x >= 0 && x < w && y >= 0 && y < h)
                dst[static_cast<std::size_t>(y) * w + x] = Rgb{14, 14, 18, false};
        }
        // the feet hopping
        const float beat = t * 5.0F;
        const float bx = (litI + 0.5F) * keyW;
        const float by = keysTop + (keysBot - keysTop) * 0.28F -
                         std::sin(std::fmod(beat, 1.0F) * 3.14159F) * mn * 0.10F;
        plotDot(dst, w, h, bx - mn * 0.025F, by, mn * 0.018F, ya, Rgb{60, 40, 30, false});
        plotDot(dst, w, h, bx + mn * 0.025F, by, mn * 0.018F, ya, Rgb{60, 40, 30, false});
        // music notes drifting up
        for (int n = 0; n < 6; ++n)
        {
          const float nt = std::fmod(t * 1.2F + n * 0.17F, 1.0F);
          const float nx = hash(n * 3) * w;
          const float ny = (keysTop)*nt + 0.0F * (1 - nt) - mn * 0.05F + nt * (-mn * 0.20F);
          const float alpha = 1.0F - nt;
          if (alpha < 0.05F)
            continue;
          plotDot(dst,
                  w,
                  h,
                  nx,
                  ny,
                  mn * 0.012F,
                  ya,
                  Rgb{u8(220 * alpha), u8(220 * alpha), u8(160 * alpha), false});  // notehead
          drawSeg(dst,
                  w,
                  h,
                  nx + mn * 0.012F,
                  ny,
                  nx + mn * 0.012F,
                  ny - mn * 0.05F * alpha,
                  std::max(1.0F, mn * 0.005F),
                  ya,
                  Rgb{u8(220 * alpha), u8(220 * alpha), u8(160 * alpha), false});  // stem
        }
      });
}

void effectHalStare(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  const int corner = static_cast<int>(hash(static_cast<int>(w + h)) * 4.0F) % 4;
  const float cx =
      (corner == 0 || corner == 2) ? mn * 0.18F : w - mn * 0.18F;
  const float cy = (corner < 2) ? mn * 0.18F : h - mn * 0.18F;
  const float R = mn * 0.14F;
  runFrames(renderer, w, h, 4800,
            [&](float t, std::vector<Rgb>& dst)
            {
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float gd = std::hypot(x - cx, (y - cy) * ya);
                  const float vd = 1.0F - std::clamp(gd / (R * 2.5F), 0.0F, 1.0F);
                  const float dim = 0.5F + 0.5F * (1.0F - vd);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      s.transparent ? Rgb{4, 4, 8, false}
                                    : Rgb{u8(s.r * dim), u8(s.g * dim), u8(s.b * dim), false};
                }
              const float pulse = 0.5F + 0.5F * std::sin(t * 4.0F * 3.14159F);
              for (int yy = static_cast<int>(cy - R); yy <= static_cast<int>(cy + R); ++yy)
                for (int xx = static_cast<int>(cx - R); xx <= static_cast<int>(cx + R); ++xx)
                {
                  if (xx < 0 || xx >= w || yy < 0 || yy >= h) continue;
                  const float dx = xx - cx, dy = (yy - cy) * ya;
                  const float d = std::hypot(dx, dy);
                  if (d > R) continue;
                  const float q = d / R;
                  Rgb c;
                  if (q < 0.18F)
                    c = Rgb{255, 220, 150, false};
                  else
                  {
                    const float rr = 1.0F - q;
                    c = Rgb{u8(50 + 200 * rr), u8(20 * rr * rr), 0, false};
                  }
                  dst[static_cast<std::size_t>(yy) * w + xx] = c;
                }
              const float pupilR = R * (0.10F + 0.06F * pulse);
              plotDot(dst, w, h, cx, cy, pupilR, ya, Rgb{20, 0, 0, false});
            });
}

void effectHitchcock(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(
      renderer, w, h, 5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float dim = std::clamp(1.0F - t * 1.2F, 0.20F, 1.0F);
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 50.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(210 + l * 0.10F * dim), u8(196 + l * 0.10F * dim),
                    u8(180 + l * 0.08F * dim), false};
          }
        const float slide = std::clamp(t / 0.50F, 0.0F, 1.0F);
        const float cx = -mn * 0.4F + slide * (w * 0.5F + mn * 0.4F);
        const float baseY = h * 0.58F;
        const Rgb ink{14, 12, 10, false};
        const float S = mn * 0.40F;
        // Hitchcock profile via stacked dots: crown, brow, nose, chin, body.
        struct Node { float dx, dy, r; };
        static const std::vector<Node> nodes = {
            {-0.20F, -0.90F, 0.30F},  // crown
            {0.05F, -0.85F, 0.27F},   // back of head bulge
            {-0.30F, -0.65F, 0.20F},  // brow region forward
            {-0.42F, -0.50F, 0.10F},  // nose tip
            {-0.32F, -0.40F, 0.14F},  // upper lip / mustache area
            {-0.28F, -0.28F, 0.16F},  // jowls
            {-0.20F, -0.15F, 0.20F},  // double chin
            {-0.05F, 0.05F, 0.32F},   // neck/collar
            {0.00F, 0.40F, 0.55F},    // shoulders/body
        };
        for (const auto& n : nodes)
          plotDot(dst, w, h, cx + n.dx * S, baseY + n.dy * S, n.r * S, ya, ink);
        if (slide >= 1.0F)
        {
          const std::string line = "GOOD EVENING";
          const int sc = std::max(2, static_cast<int>(mn / 80.0F));
          const float lineW = static_cast<float>(line.size()) * 6 * sc;
          const float fadeT = std::clamp((t - 0.55F) / 0.20F, 0.0F, 1.0F);
          for (int ci = 0; ci < static_cast<int>(line.size()); ++ci)
          {
            const char ch = line[ci];
            const auto g = glyph5x7(ch);
            for (int fy = 0; fy < 7; ++fy)
              for (int fx = 0; fx < 5; ++fx)
                if (g[fy][fx] == '1')
                {
                  const float px = cx + S * 1.0F + ((ci - 6) * 6 + fx) * sc;
                  const float py = baseY + S * 0.7F + fy * sc;
                  const Rgb tc{u8(20 * fadeT), u8(20 * fadeT), u8(20 * fadeT), false};
                  plotDot(dst, w, h, px, py, std::max(1.0F, static_cast<float>(sc) * 0.5F), ya,
                          tc);
                }
            (void)lineW;
          }
        }
      });
}

void effectUFO(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(
      renderer, w, h, 5200,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float dim = 0.20F;
        for (int y = 0; y < h; ++y)
        {
          const float sf = static_cast<float>(y) / h;
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 30.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(6 + 18 * (1 - sf) + l * 0.05F * dim),
                    u8(8 + 20 * (1 - sf) + l * 0.06F * dim),
                    u8(28 + 36 * (1 - sf) + l * 0.10F * dim), false};
          }
        }
        // Stars
        for (int i = 0; i < 80; ++i)
        {
          const int sx = static_cast<int>(hash(i * 3) * w);
          const int sy = static_cast<int>(hash(i * 3 + 1) * h * 0.7F);
          const float tw = 0.5F + 0.5F * std::sin(t * 10.0F + i);
          dst[static_cast<std::size_t>(sy) * w + sx] =
              Rgb{u8(170 * tw), u8(170 * tw), u8(200 * tw), false};
        }
        const float p = std::clamp((t - 0.1F) / 0.5F, 0.0F, 1.0F);
        const float ux = -mn * 0.3F + p * (w + mn * 0.6F);
        const float uy = h * 0.32F + std::sin(p * 3.14F * 2.0F) * mn * 0.04F;
        const float us = mn * 0.10F;
        // Disc body
        for (int yo = -static_cast<int>(us * 0.18F); yo <= static_cast<int>(us * 0.18F); ++yo)
        {
          const float yf = std::fabs(yo) / (us * 0.18F);
          const int half = static_cast<int>(us * (1.0F - 0.3F * yf));
          for (int xo = -half; xo <= half; ++xo)
          {
            const int xx = static_cast<int>(ux + xo), yy = static_cast<int>(uy + yo);
            if (xx >= 0 && xx < w && yy >= 0 && yy < h)
              dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{120, 120, 140, false};
          }
        }
        // Dome
        for (float a = 0; a < 3.14F; a += 0.05F)
        {
          const float dx = std::cos(a) * us * 0.4F;
          const float dy = -std::sin(a) * us * 0.25F;
          plotDot(dst, w, h, ux + dx, uy + dy, std::max(1.0F, mn * 0.005F), ya,
                  Rgb{200, 200, 220, false});
        }
        // Lights ring
        for (int k = 0; k < 5; ++k)
        {
          const float lx = ux + (k - 2) * us * 0.30F;
          const Rgb lc =
              (static_cast<int>(t * 20) + k) % 2 == 0 ? Rgb{255, 200, 80, false} : Rgb{80, 200, 255, false};
          plotDot(dst, w, h, lx, uy + us * 0.10F, std::max(1.0F, mn * 0.008F), ya, lc);
        }
        // Beam
        if (p > 0.3F && p < 0.7F)
        {
          const float bf = (p - 0.3F) / 0.4F * 3.14F;
          const float intensity = std::sin(bf);
          for (float v = 0; v <= 1.0F; v += 0.02F)
          {
            const float bw = us * 0.3F * v;
            for (float u = -bw; u <= bw; u += mn * 0.01F)
            {
              const float xx = ux + u;
              const float yy = uy + us * 0.18F + v * mn * 0.4F;
              if (xx >= 0 && xx < w && yy >= 0 && yy < h)
              {
                Rgb& c = dst[static_cast<std::size_t>(yy) * w + static_cast<int>(xx)];
                const float k = intensity * (1.0F - v);
                c = Rgb{u8(c.r + (255 - c.r) * k * 0.6F),
                        u8(c.g + (240 - c.g) * k * 0.6F),
                        u8(c.b + (180 - c.b) * k * 0.6F), false};
              }
            }
          }
        }
        // Subtitle
        if (t > 0.7F)
        {
          const std::string line = "I WANT TO BELIEVE";
          const int sc = std::max(2, static_cast<int>(mn / 80.0F));
          const float lineW = static_cast<float>(line.size()) * 6 * sc;
          for (int ci = 0; ci < static_cast<int>(line.size()); ++ci)
          {
            const char ch = line[ci];
            if (ch == ' ') continue;
            const auto g = glyph5x7(ch);
            for (int fy = 0; fy < 7; ++fy)
              for (int fx = 0; fx < 5; ++fx)
                if (g[fy][fx] == '1')
                  plotDot(dst, w, h, w * 0.5F - lineW * 0.5F + (ci * 6 + fx) * sc,
                          h * 0.90F + fy * sc,
                          std::max(1.0F, static_cast<float>(sc) * 0.5F), ya,
                          Rgb{240, 240, 240, false});
          }
        }
      });
}

void effectIndyIdol(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
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
          const float dl = d.transparent ? 40.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(60 + dl * 0.20F), u8(40 + dl * 0.15F), u8(20 + dl * 0.10F), false};
        }
      // Pedestal.
      const float cx = w * 0.5F, top = h * 0.58F;
      for (int yo = 0; yo <= static_cast<int>(mn * 0.10F); ++yo)
        for (int xo = -static_cast<int>(mn * 0.08F); xo <= static_cast<int>(mn * 0.08F); ++xo) {
          const int xx = static_cast<int>(cx + xo), yy = static_cast<int>(top + yo);
          if (xx >= 0 && xx < w && yy >= 0 && yy < h)
            dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{160, 120, 60, false};
        }
      // Foot — initially on pedestal; swap progress lifts foot, drops sandbag.
      const float swap = std::clamp(t * 1.3F, 0.0F, 1.0F);
      const float footX = cx - swap * mn * 0.15F;
      // Foot rotated so the sole sits on the pedestal nicely.
      drawFootSprite(dst, w, h, ya, f.img.get(), f.fw, f.fh, f.needChromaKey,
                     footX, top - mn * 0.06F - swap * mn * 0.05F, mn * 0.14F, 0.1F);
      // Sandbag descending into place.
      const float bagX = cx + (1 - swap) * mn * 0.15F;
      const float bagY = top - mn * 0.04F * swap;
      plotDot(dst, w, h, bagX, bagY, mn * 0.045F * swap, ya, Rgb{120, 90, 60, false});
      // Indy silhouette.
      const float ix = w * 0.20F + swap * w * 0.10F;
      drawSeg(dst, w, h, ix, h * 0.60F, ix, h * 0.85F, mn * 0.025F, ya, Rgb{40, 30, 20, false});
      plotDot(dst, w, h, ix, h * 0.56F, mn * 0.025F, ya, Rgb{200, 160, 110, false});
      // Hat (fedora).
      drawSeg(dst, w, h, ix - mn * 0.04F, h * 0.53F, ix + mn * 0.04F, h * 0.53F,
              std::max(1.0F, mn * 0.010F), ya, Rgb{60, 40, 20, false});
      // Trigger flash after the swap.
      if (swap > 0.9F) {
        const float flash = (swap - 0.9F) / 0.1F;
        for (int yy = 0; yy < h; ++yy)
          for (int xx = 0; xx < w; ++xx) {
            Rgb& p = dst[static_cast<std::size_t>(yy) * w + xx];
            p.r = u8(p.r + 120 * flash);
            p.g = u8(p.g + 100 * flash);
            p.b = u8(p.b + 60 * flash);
          }
      }
    });
}

void effectOscarStatue(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
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
              Rgb{u8(60 + dl * 0.15F), u8(20 + dl * 0.10F), u8(20 + dl * 0.10F), false};
        }
      // Red carpet at the bottom.
      for (int yy = static_cast<int>(h * 0.80F); yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx)
          dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{160, 20, 30, false};
      // Pedestal.
      const float cx = w * 0.5F, top = h * 0.55F;
      for (int yo = 0; yo <= static_cast<int>(mn * 0.10F); ++yo)
        for (int xo = -static_cast<int>(mn * 0.06F); xo <= static_cast<int>(mn * 0.06F); ++xo) {
          const int xx = static_cast<int>(cx + xo), yy = static_cast<int>(top + yo);
          if (xx >= 0 && xx < w && yy >= 0 && yy < h)
            dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{120, 80, 30, false};
        }
      // Gold-foot trophy on top, with golden tint applied via a quick post-pass.
      // Draw to a scratch then tint? Simpler: place foot, then overlay gold tint
      // on the foot's pixels by sampling alpha.
      drawFootSprite(dst, w, h, ya, f.img.get(), f.fw, f.fh, f.needChromaKey,
                     cx, top - mn * 0.12F, mn * 0.20F, 0.0F);
      // Gold-cast: re-walk the bounding box, blend toward gold where the foot was painted.
      const int x0 = static_cast<int>(cx - mn * 0.20F);
      const int x1 = static_cast<int>(cx + mn * 0.20F);
      const int y0 = static_cast<int>(top - mn * 0.22F);
      const int y1 = static_cast<int>(top - mn * 0.02F);
      for (int yy = std::max(0, y0); yy <= std::min(h - 1, y1); ++yy)
        for (int xx = std::max(0, x0); xx <= std::min(w - 1, x1); ++xx) {
          Rgb& p = dst[static_cast<std::size_t>(yy) * w + xx];
          // Skin-ish? gild it.
          if (p.r > 140 && p.g > 80 && p.b > 80 && p.r > p.b) {
            p.r = u8(p.r * 0.4F + 220 * 0.6F);
            p.g = u8(p.g * 0.4F + 170 * 0.6F);
            p.b = u8(p.b * 0.4F + 40 * 0.6F);
          }
        }
      // Flashbulbs.
      for (int i = 0; i < 6; ++i) {
        const float age = std::fmod(t * 3.0F + hash(i), 1.0F);
        const float fx = hash(i * 7) * w;
        const float fy = h * (0.20F + 0.30F * hash(i * 11));
        if (age < 0.15F)
          plotDot(dst, w, h, fx, fy, mn * 0.030F * (1 - age / 0.15F), ya,
                  Rgb{255, 255, 240, false});
      }
    });
}

void effectPulpBriefcase(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  FootSprite f = loadPythonFootSprite();
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      const float open = std::clamp(t * 1.5F, 0.0F, 1.0F);
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb d = sample(src, w, h, x, y);
          const float dl = d.transparent ? 30.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          // Glow lights the scene from below as the case opens.
          const float glow = open * std::max(0.0F, 1.0F - std::fabs(static_cast<float>(x) / w - 0.5F) * 2.0F)
                                 * std::max(0.0F, 1.0F - static_cast<float>(y) / h);
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(20 + dl * 0.20F + glow * 200), u8(15 + dl * 0.10F + glow * 130),
                  u8(15 + dl * 0.10F + glow * 40), false};
        }
      // Table.
      const float tableY = h * 0.78F;
      for (int yy = static_cast<int>(tableY); yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx)
          dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{40, 30, 20, false};
      // Briefcase body.
      const float cx = w * 0.5F;
      for (int yo = 0; yo <= static_cast<int>(mn * 0.10F); ++yo)
        for (int xo = -static_cast<int>(mn * 0.18F); xo <= static_cast<int>(mn * 0.18F); ++xo) {
          const int xx = static_cast<int>(cx + xo), yy = static_cast<int>(tableY - mn * 0.04F + yo);
          if (xx >= 0 && xx < w && yy >= 0 && yy < h)
            dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{60, 40, 30, false};
        }
      // Lid open.
      const float lidA = -open * 1.3F;
      drawSeg(dst, w, h, cx - mn * 0.18F, tableY - mn * 0.04F,
              cx - mn * 0.18F + std::cos(lidA) * mn * 0.36F,
              tableY - mn * 0.04F + std::sin(lidA) * mn * 0.36F,
              std::max(1.0F, mn * 0.012F), ya, Rgb{80, 60, 40, false});
      // Foot inside, glowing.
      drawFootSprite(dst, w, h, ya, f.img.get(), f.fw, f.fh, f.needChromaKey,
                     cx, tableY - mn * 0.08F, mn * 0.18F * open, 0.0F);
      // Actor face above (silhouette + glow lighting).
      const float fxx = w * 0.30F, fyy = h * 0.35F;
      plotDot(dst, w, h, fxx, fyy, mn * 0.06F, ya,
              Rgb{u8(120 + open * 130), u8(80 + open * 80), u8(40 + open * 20), false});
    });
}

void effectYorick(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
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
          const float dl = d.transparent ? 30.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          const float sf = static_cast<float>(y) / h;
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(40 + dl * 0.15F + 30 * (1 - sf)), u8(50 + dl * 0.15F + 30 * (1 - sf)),
                  u8(60 + dl * 0.15F + 30 * (1 - sf)), false};
        }
      // Ground + headstones.
      const float groundY = h * 0.85F;
      for (int xx = 0; xx < w; ++xx)
        dst[static_cast<std::size_t>(groundY) * w + xx] = Rgb{40, 50, 40, false};
      for (int k = 0; k < 4; ++k) {
        const float hsX = w * (0.20F + 0.20F * k);
        for (int yo = 0; yo <= static_cast<int>(mn * 0.05F); ++yo)
          for (int xo = -static_cast<int>(mn * 0.015F); xo <= static_cast<int>(mn * 0.015F); ++xo) {
            const int xx = static_cast<int>(hsX + xo), yy = static_cast<int>(groundY - mn * 0.05F + yo);
            if (xx >= 0 && xx < w && yy >= 0 && yy < h)
              dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{120, 120, 130, false};
          }
      }
      // Hamlet figure (kneeling).
      const float hX = w * 0.55F;
      const Rgb cloak{30, 20, 30, false};
      drawSeg(dst, w, h, hX, groundY, hX, groundY - mn * 0.18F, mn * 0.030F, ya, cloak);
      plotDot(dst, w, h, hX, groundY - mn * 0.21F, mn * 0.025F, ya, Rgb{220, 180, 140, false});
      // Arm extended — holding foot.
      drawSeg(dst, w, h, hX, groundY - mn * 0.15F, hX + mn * 0.12F,
              groundY - mn * 0.25F, mn * 0.012F, ya, Rgb{220, 180, 140, false});
      // Foot held at arm's length.
      const float hold = std::sin(t * 1.5F) * 0.05F;
      drawFootSprite(dst, w, h, ya, f.img.get(), f.fw, f.fh, f.needChromaKey,
                     hX + mn * 0.18F, groundY - mn * 0.27F + hold * mn * 0.01F,
                     mn * 0.13F, 0.0F);
      // Word "ALAS" floats above.
      const std::string label = "ALAS";
      const int sc = std::max(2, static_cast<int>(mn / 35.0F));
      const float lineW = static_cast<float>(label.size()) * 6 * sc;
      const float fadeIn = std::clamp(t * 1.5F, 0.0F, 1.0F);
      for (int ci = 0; ci < static_cast<int>(label.size()); ++ci) {
        const auto g = glyph5x7(label[ci]);
        for (int fy = 0; fy < 7; ++fy)
          for (int fx = 0; fx < 5; ++fx)
            if (g[fy][fx] == '1')
              plotDot(dst, w, h, hX + mn * 0.04F - lineW * 0.5F + (ci * 6 + fx) * sc,
                      h * 0.18F + fy * sc, std::max(1.0F, static_cast<float>(sc) * 0.5F * fadeIn), ya,
                      Rgb{u8(240 * fadeIn), u8(220 * fadeIn), u8(200 * fadeIn), false});
      }
    });
}

}  // namespace ee_detail
}  // namespace Qdless
