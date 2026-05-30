#include "QdlessExitEffectCommon.h"

namespace Qdless
{
namespace ee_detail
{

void effectSnowflakes(
    const Renderer& renderer, const std::vector<Rgb>& src, int w, int h, std::mt19937& rng)
{
  const float ya = yAspectFor(renderer);
  constexpr int kN = 16;
  struct Flake
  {
    float x0;
    float y0;
    float r0;
    float drift;
    float phase;
    float fall;
    Rgb col;
  };
  std::uniform_real_distribution<float> ux(0.05F, 0.95F);
  std::uniform_real_distribution<float> uy(0.05F, 0.60F);
  std::uniform_real_distribution<float> ur(0.30F, 0.55F);
  std::uniform_real_distribution<float> ud(-1.0F, 1.0F);
  std::uniform_real_distribution<float> up(0.0F, 6.2832F);
  std::uniform_real_distribution<float> uf(0.8F, 1.4F);
  std::array<Flake, kN> flakes{};
  for (auto& f : flakes)
  {
    f.x0 = ux(rng) * w;
    f.y0 = uy(rng) * h;
    f.r0 = ur(rng) * h;  // giant: a big fraction of the screen height
    f.drift = ud(rng);
    f.phase = up(rng);
    f.fall = uf(rng);
    Rgb b = sample(src, w, h, f.x0, f.y0);
    if (b.transparent)
      b = Rgb{120, 150, 200, false};
    f.col = Rgb{static_cast<std::uint8_t>(b.r + (255 - b.r) * 0.55F),
                static_cast<std::uint8_t>(b.g + (255 - b.g) * 0.55F),
                static_cast<std::uint8_t>(b.b + (255 - b.b) * 0.62F),
                false};
  }
  runFrames(renderer,
            w,
            h,
            3200,
            [&](float t, std::vector<Rgb>& dst)
            {
              for (const auto& f : flakes)
              {
                const float R = f.r0 * (1.0F - t);  // shrink to nothing
                if (R < 1.0F)
                  continue;
                const float fx =
                    f.x0 + f.drift * w * 0.15F * t + std::sin(f.phase + t * 6.0F) * w * 0.02F;
                const float fy = f.y0 + f.fall * (t * t) * h * 1.2F;  // accelerating fall
                const int x0 = std::max(0, static_cast<int>(fx - R));
                const int x1 = std::min(w - 1, static_cast<int>(fx + R));
                const int y0 = std::max(0, static_cast<int>(fy - R / ya));
                const int y1 = std::min(h - 1, static_cast<int>(fy + R / ya));
                for (int y = y0; y <= y1; ++y)
                  for (int x = x0; x <= x1; ++x)
                    if (inSnowflake(x - fx, y - fy, R, ya))
                      dst[static_cast<std::size_t>(y) * w + x] = f.col;
              }
            });
}

void effectAurora(
    const Renderer& renderer, const std::vector<Rgb>& src, int w, int h, std::mt19937& rng)
{
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  constexpr int kC = 4;
  struct Curtain
  {
    float base;
    float ampX;
    float ky;
    float spd;
    float phase;
    float width;
    Rgb col;
  };
  static const std::array<Rgb, kC> kHue = {Rgb{60, 255, 140, false},
                                           Rgb{110, 255, 90, false},
                                           Rgb{40, 230, 200, false},
                                           Rgb{150, 110, 255, false}};
  std::uniform_real_distribution<float> ubase(0.2F, 0.8F);
  std::uniform_real_distribution<float> uamp(0.05F, 0.13F);
  std::uniform_real_distribution<float> uky(2.5F, 5.5F);
  std::uniform_real_distribution<float> uspd(0.5F, 1.3F);
  std::uniform_real_distribution<float> uph(0.0F, 6.2832F);
  std::uniform_real_distribution<float> uwid(0.05F, 0.10F);
  std::array<Curtain, kC> cs{};
  for (int i = 0; i < kC; ++i)
    cs[i] = {ubase(rng), uamp(rng), uky(rng), uspd(rng), uph(rng), uwid(rng), kHue[i]};

  constexpr int kStars = 50;
  std::array<float, kStars> stx{};
  std::array<float, kStars> sty{};
  std::array<float, kStars> stp{};
  std::uniform_real_distribution<float> u01(0.0F, 1.0F);
  for (int i = 0; i < kStars; ++i)
  {
    stx[i] = u01(rng) * w;
    sty[i] = u01(rng) * h * 0.7F;
    stp[i] = uph(rng);
  }

  runFrames(
      renderer,
      w,
      h,
      3800,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float vf = std::clamp(1.0F - t * 2.4F, 0.0F, 1.0F);  // view -> night
        const float night = 1.0F - vf;
        const float env = std::sin(std::clamp(t, 0.0F, 1.0F) * 3.14159F);  // aurora swells/fades
        for (int y = 0; y < h; ++y)
        {
          const float fy = static_cast<float>(y) / h;
          const float vp = std::clamp((fy - 0.03F) / 0.18F, 0.0F, 1.0F) *
                           std::clamp((0.9F - fy) / 0.55F, 0.0F, 1.0F);  // rays hang from the top
          std::array<float, kC> cxw{};
          for (int c = 0; c < kC; ++c)
            cxw[c] = (cs[c].base + cs[c].ampX * std::sin(fy * cs[c].ky + t * 6.2832F * cs[c].spd +
                                                         cs[c].phase)) *
                     w;
          for (int x = 0; x < w; ++x)
          {
            const std::size_t idx = static_cast<std::size_t>(y) * w + x;
            const Rgb& s0 = src[idx];
            float r = s0.transparent ? 0.0F : s0.r * vf;
            float g = s0.transparent ? 0.0F : s0.g * vf;
            float b = s0.transparent ? 0.0F : s0.b * vf;
            r += 3.0F * night;
            g += 5.0F * night;
            b += 14.0F * night;  // deep-blue night sky
            for (int c = 0; c < kC; ++c)
            {
              const float d = (x - cxw[c]) / (cs[c].width * w);
              const float band = std::exp(-d * d);
              const float ray =
                  0.55F + 0.45F * std::sin(x * 0.5F + fy * 9.0F + t * 4.0F + cs[c].phase);
              const float inten = band * vp * ray * env * 1.1F;
              r += cs[c].col.r * inten;
              g += cs[c].col.g * inten;
              b += cs[c].col.b * inten;
            }
            dst[idx] = Rgb{u8(r), u8(g), u8(b), false};
          }
        }
        for (int i = 0; i < kStars; ++i)
        {
          const float bri = 220.0F * (0.5F + 0.5F * std::sin(t * 9.0F + stp[i])) * night;
          if (bri < 8.0F)
            continue;
          const int sx = static_cast<int>(stx[i]);
          const int sy = static_cast<int>(sty[i]);
          if (sx < 0 || sx >= w || sy < 0 || sy >= h)
            continue;
          const std::size_t idx = static_cast<std::size_t>(sy) * w + sx;
          const Rgb cur = dst[idx];
          dst[idx] = Rgb{u8(cur.r + bri), u8(cur.g + bri), u8(cur.b + bri), false};
        }
      });
}

void effectThunderstorm(
    const Renderer& renderer, const std::vector<Rgb>& src, int w, int h, std::mt19937& rng)
{
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float ya = yAspectFor(renderer);
  constexpr int kRain = 150;
  struct Drop
  {
    float x;
    float y0;
    float spd;
    float len;
  };
  std::uniform_real_distribution<float> u01(0.0F, 1.0F);
  std::array<Drop, kRain> drops{};
  for (auto& d : drops)
  {
    d.x = u01(rng) * w;
    d.y0 = u01(rng) * h;
    d.spd = 0.8F + u01(rng) * 0.9F;
    d.len = 3.0F + u01(rng) * 4.0F;
  }
  struct Strike
  {
    float t0;
    std::vector<std::pair<float, float>> seg;  // a polyline (main bolt or a fork)
  };
  std::vector<Strike> strikes;
  auto buildBolt = [&](float t0)
  {
    float x = (0.2F + u01(rng) * 0.6F) * w;
    float y = 0.0F;
    const float endY = h * (0.6F + u01(rng) * 0.25F);
    const int steps = 14;
    std::vector<std::pair<float, float>> main;
    for (int i = 0; i <= steps; ++i)
    {
      main.emplace_back(x, y);
      y += endY / steps;
      x = std::clamp(x + (u01(rng) - 0.5F) * w * 0.10F, 0.0F, static_cast<float>(w - 1));
    }
    strikes.push_back({t0, main});
    if (u01(rng) < 0.8F)  // a branching fork
    {
      const int mi = steps / 3 + static_cast<int>(u01(rng) * steps / 3);
      float fx = main[static_cast<std::size_t>(mi)].first;
      float fy = main[static_cast<std::size_t>(mi)].second;
      std::vector<std::pair<float, float>> fork;
      const int fsteps = 6;
      for (int i = 0; i <= fsteps; ++i)
      {
        fork.emplace_back(fx, fy);
        fy += (h * 0.25F) / fsteps;
        fx = std::clamp(fx + (u01(rng) - 0.3F) * w * 0.12F, 0.0F, static_cast<float>(w - 1));
      }
      strikes.push_back({t0, fork});
    }
  };
  buildBolt(0.18F);
  buildBolt(0.45F + u01(rng) * 0.10F);
  buildBolt(0.76F);
  const Rgb rainCol{120, 140, 180, false};

  runFrames(renderer,
            w,
            h,
            3200,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float vf = std::clamp(1.0F - t * 1.7F, 0.0F, 1.0F);
              for (std::size_t i = 0; i < dst.size(); ++i)
              {
                const Rgb& s0 = src[i];
                float r = s0.transparent ? 0.0F : s0.r * vf * 0.7F;
                float g = s0.transparent ? 0.0F : s0.g * vf * 0.7F;
                float b = s0.transparent ? 0.0F : s0.b * vf * 0.7F;
                r += 8.0F * (1.0F - vf);
                g += 10.0F * (1.0F - vf);
                b += 18.0F * (1.0F - vf);  // storm tint
                dst[i] = Rgb{u8(r), u8(g), u8(b), false};
              }
              for (const auto& d : drops)  // diagonal rain
              {
                const float y = std::fmod(d.y0 + d.spd * t * h * 1.5F, static_cast<float>(h));
                const int n = static_cast<int>(d.len);
                for (int k = 0; k < n; ++k)
                {
                  const int xx = static_cast<int>(d.x - k * 0.3F);
                  const int yy = static_cast<int>(y - k);
                  if (xx < 0 || xx >= w || yy < 0 || yy >= h)
                    continue;
                  const std::size_t idx = static_cast<std::size_t>(yy) * w + xx;
                  const Rgb cur = dst[idx];
                  const float a = 0.5F * (1.0F - static_cast<float>(k) / n);
                  dst[idx] = Rgb{u8(cur.r + rainCol.r * a),
                                 u8(cur.g + rainCol.g * a),
                                 u8(cur.b + rainCol.b * a),
                                 false};
                }
              }
              for (const auto& s : strikes)
              {
                const float age = t - s.t0;
                if (age < 0.0F || age > 0.22F)
                  continue;
                const float k = 1.0F - age / 0.22F;
                if (age < 0.06F)  // full-screen flash
                {
                  const float fk = (1.0F - age / 0.06F) * 0.7F;
                  for (std::size_t i = 0; i < dst.size(); ++i)
                  {
                    const Rgb c = dst[i];
                    dst[i] = Rgb{
                        u8(c.r + 255.0F * fk), u8(c.g + 255.0F * fk), u8(c.b + 255.0F * fk), false};
                  }
                }
                const Rgb bolt{u8(200.0F + 55.0F * k), u8(220.0F + 35.0F * k), 255, false};
                for (std::size_t i = 0; i + 1 < s.seg.size(); ++i)
                {
                  const auto& p0 = s.seg[i];
                  const auto& p1 = s.seg[i + 1];
                  const int steps = static_cast<int>(
                      std::max(2.0F, std::hypot(p1.first - p0.first, p1.second - p0.second)));
                  for (int j = 0; j <= steps; ++j)
                  {
                    const float f = static_cast<float>(j) / steps;
                    plotDot(dst,
                            w,
                            h,
                            p0.first + (p1.first - p0.first) * f,
                            p0.second + (p1.second - p0.second) * f,
                            1.4F,
                            ya,
                            bolt);
                  }
                }
              }
            });
}

void effectStingray(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float cx0 = (w - 1) * 0.5F;
  const float cy0 = (h - 1) * 0.5F;
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };

  auto inRay = [&](float bx, float by)
  {
    bool inside = false;
    for (std::size_t i = 0, j = kRay.size() - 1; i < kRay.size(); j = i++)
    {
      const float xi = kRay[i].first;
      const float yi = kRay[i].second;
      const float xj = kRay[j].first;
      const float yj = kRay[j].second;
      if (((yi > by) != (yj > by)) && (bx < (xj - xi) * (by - yi) / (yj - yi + 1e-9F) + xi))
        inside = !inside;
    }
    return inside;
  };

  auto drawRay = [&](std::vector<Rgb>& dst,
                     float cx,
                     float cy,
                     float scaleX,
                     float scaleY,
                     float rot,
                     float flapPhase,
                     float tailPhase,
                     float dim)
  {
    const float cr = std::cos(rot);
    const float sr = std::sin(rot);
    const float ext = std::max(scaleX * 1.05F, scaleY * 1.85F);  // wings vs tail reach
    const int x0 = std::max(0, static_cast<int>(cx - ext));
    const int x1 = std::min(w - 1, static_cast<int>(cx + ext));
    const int y0 = std::max(0, static_cast<int>(cy - ext / ya));
    const int y1 = std::min(h - 1, static_cast<int>(cy + ext / ya));
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x)
      {
        const float dx = x - cx;
        const float dyp = (y - cy) * ya;
        const float bx = (dx * cr + dyp * sr) / scaleX;                           // lateral
        const float by = (dx * sr - dyp * cr) / scaleY;                           // longitudinal
        const bool body = inRay(bx, by - 0.12F * bx * bx * std::sin(flapPhase));  // flap bend
        bool tail = false;
        if (!body && by < -0.55F && by > -1.8F)
        {
          const float c0 = 0.42F * std::sin(by * 2.4F + tailPhase);  // sway
          const float wtail = 0.012F + 0.05F * std::clamp((by + 1.8F) / 1.25F, 0.0F, 1.0F);
          tail = std::fabs(bx - c0) < wtail;
        }
        if (!body && !tail)
          continue;
        const float tu = std::clamp((bx + 1.0F) * 0.5F, 0.0F, 1.0F);
        const float tv = std::clamp((1.0F - by) * 0.5F, 0.0F, 1.0F);
        const Rgb tex = sample(src, w, h, tu * (w - 1), tv * (h - 1));
        const float tr = tex.transparent ? 70.0F : tex.r;
        const float tg = tex.transparent ? 80.0F : tex.g;
        const float tb = tex.transparent ? 95.0F : tex.b;
        const float edge = 1.0F - 0.35F * std::clamp(std::fabs(bx) - 0.25F, 0.0F, 1.0F);
        const float flap = 1.0F + 0.30F * std::sin(std::fabs(bx) * 5.0F - flapPhase);
        const float sh = (tail ? 0.45F : edge * flap) * dim;
        dst[static_cast<std::size_t>(y) * w + x] =
            Rgb{u8(tr * sh), u8(tg * sh), u8(tb * sh), false};
      }
  };

  const float sBig = w / 2.3F;     // collapse: wings fill the screen
  const float sSwim = 0.18F * mn;  // size as it sets off
  constexpr float kCollapse = 0.16F;
  // Curved swim path: up first, then bending to the right (so the ray turns).
  const float p0x = cx0;
  const float p0y = cy0;
  const float p1x = w * 0.5F;
  const float p1y = h * 0.30F;
  const float p2x = w * 0.76F;
  const float p2y = h * 0.18F;

  runFrames(
      renderer,
      w,
      h,
      5200,
      [&](float t, std::vector<Rgb>& dst)
      {
        std::fill(dst.begin(), dst.end(), Rgb{0, 0, 0, false});
        const float flapPhase = t * 26.0F;
        const float tailPhase = t * 14.0F;
        if (t < kCollapse)  // the view gathers into the ray
        {
          const float p = t / kCollapse;
          const float s = sBig + (sSwim - sBig) * p;
          drawRay(dst, cx0, cy0, s * 1.15F, s, 0.0F, flapPhase, tailPhase, 1.0F);
          return;
        }
        const float tau = (t - kCollapse) / (1.0F - kCollapse);
        const float om = 1.0F - tau;
        const float px = om * om * p0x + 2.0F * om * tau * p1x + tau * tau * p2x;
        const float py = om * om * p0y + 2.0F * om * tau * p1y + tau * tau * p2y;
        const float vx = 2.0F * om * (p1x - p0x) + 2.0F * tau * (p2x - p1x);
        const float vy = (2.0F * om * (p1y - p0y) + 2.0F * tau * (p2y - p1y)) * ya;
        const float rot = std::atan2(vx, -vy);         // nose follows the path
        const float s = sSwim * (1.0F - 0.85F * tau);  // recede
        const float foreLong = 1.0F - 0.5F * tau;      // foreshorten away
        drawRay(
            dst, px, py, s * 1.15F, s * foreLong, rot, flapPhase, tailPhase, 1.0F - 0.65F * tau);
      });
}

void effectTornado(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float cx0 = (w - 1) * 0.5F;
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer,
            w,
            h,
            5200,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float f = std::clamp(t / 0.18F, 0.0F, 1.0F);  // wrap into the funnel
              const float rcParam = std::clamp((t - 0.18F) / 0.82F, 0.0F, 1.0F);
              const float recede = 1.0F - 0.62F * rcParam;
              const float dim = 1.0F - 0.5F * rcParam;
              const float spin = t * 22.0F;  // rapid rotation
              const float vp = h * 0.32F;    // recedes toward this vanishing point
              const float yTop = vp + (h * 0.04F - vp) * recede;
              const float yBot = vp + (h * 0.98F - vp) * recede;
              const float rMax = 0.42F * w * recede;
              for (int y = 0; y < h; ++y)
              {
                const float v = (yBot > yTop) ? (y - yTop) / (yBot - yTop) : -1.0F;
                const bool inBand = v >= 0.0F && v <= 1.0F;
                const float r = inBand ? rMax * std::pow(std::max(0.0F, 1.0F - v), 0.55F) : 0.0F;
                const float cxf = cx0 + std::sin(t * 3.0F + v * 3.5F) * rMax * 0.35F;  // sway
                for (int x = 0; x < w; ++x)
                {
                  const std::size_t idx = static_cast<std::size_t>(y) * w + x;
                  const Rgb& base = src[idx];
                  float fr = base.transparent ? 0.0F : base.r;
                  float fg = base.transparent ? 0.0F : base.g;
                  float fb = base.transparent ? 0.0F : base.b;
                  bool drew = false;
                  if (inBand && r > 1.0F)
                  {
                    const float dx = x - cxf;
                    if (std::fabs(dx) <= r)
                    {
                      const float s = dx / r;
                      float u = std::asin(std::clamp(s, -1.0F, 1.0F)) / 3.14159F * 0.5F +
                                spin * 0.16F + v * 2.2F;  // angle + spin + helix
                      u -= std::floor(u);
                      const Rgb tex = sample(src, w, h, u * (w - 1), v * (h - 1));
                      const float z = std::sqrt(std::max(0.0F, 1.0F - s * s));
                      const float sh = (0.35F + 0.65F * z) * dim;
                      const float tr = (tex.transparent ? 60.0F : tex.r) * sh;
                      const float tg = (tex.transparent ? 70.0F : tex.g) * sh;
                      const float tb = (tex.transparent ? 90.0F : tex.b) * sh;
                      fr = fr * (1.0F - f) + tr * f;
                      fg = fg * (1.0F - f) + tg * f;
                      fb = fb * (1.0F - f) + tb * f;
                      drew = true;
                    }
                  }
                  if (!drew)
                  {
                    const float k = 1.0F - f;  // surroundings fade to dark as the funnel forms
                    fr *= k;
                    fg *= k;
                    fb *= k;
                  }
                  dst[idx] = Rgb{u8(fr), u8(fg), u8(fb), false};
                }
              }
            });
}

void effectTornadoDuel(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 6400,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float f = std::clamp(t / 0.15F, 0.0F, 1.0F);       // wrap-in
              const float merge = std::clamp((t - 0.45F) / 0.15F, 0.0F, 1.0F);
              const float dissipate = std::clamp((t - 0.80F) / 0.20F, 0.0F, 1.0F);
              const float dim = 1.0F - 0.5F * dissipate;
              const float spinL = t * 22.0F;
              const float spinR = -t * 22.0F;  // opposite rotation
              const float vp = h * 0.32F;
              const float yTop = vp + (h * 0.04F - vp);
              const float yBot = vp + (h * 0.98F - vp);
              const float baseRmax = 0.22F * w * (1.0F - 0.3F * dissipate);
              const float mergedRmax = baseRmax * (1.0F + merge * 0.6F);  // bigger when merged
              // Funnel centres approach the middle over time, then merge.
              const float cxLeftBase = w * 0.30F;
              const float cxRightBase = w * 0.70F;
              const float drift = std::clamp(t / 0.55F, 0.0F, 1.0F);
              const float cxLeft = cxLeftBase + drift * (w * 0.50F - cxLeftBase);
              const float cxRight = cxRightBase + drift * (w * 0.50F - cxRightBase);
              const bool merged = merge > 0.5F;
              for (int y = 0; y < h; ++y)
              {
                const float v = (yBot > yTop) ? (y - yTop) / (yBot - yTop) : -1.0F;
                const bool inBand = v >= 0.0F && v <= 1.0F;
                const float swayL = std::sin(t * 3.0F + v * 3.5F) * baseRmax * 0.30F;
                const float swayR = std::sin(t * 3.0F + v * 3.5F + 1.57F) * baseRmax * 0.30F;
                const float rL = inBand ? baseRmax * std::pow(std::max(0.0F, 1.0F - v), 0.55F) : 0.0F;
                const float rR = rL;
                const float rM = inBand ? mergedRmax * std::pow(std::max(0.0F, 1.0F - v), 0.55F) : 0.0F;
                const float cxfL = cxLeft + swayL;
                const float cxfR = cxRight + swayR;
                const float cxfM = (cxLeft + cxRight) * 0.5F + swayL * 0.5F;
                for (int x = 0; x < w; ++x)
                {
                  const std::size_t idx = static_cast<std::size_t>(y) * w + x;
                  const Rgb& base = src[idx];
                  float fr = base.transparent ? 0.0F : base.r;
                  float fg = base.transparent ? 0.0F : base.g;
                  float fb = base.transparent ? 0.0F : base.b;
                  bool drew = false;
                  auto paintFunnel = [&](float cxf, float r, float spin) {
                    if (!inBand || r <= 1.0F) return false;
                    const float dx = x - cxf;
                    if (std::fabs(dx) > r) return false;
                    const float s = dx / r;
                    float u = std::asin(std::clamp(s, -1.0F, 1.0F)) / 3.14159F * 0.5F +
                              spin * 0.16F + v * 2.2F;
                    u -= std::floor(u);
                    const Rgb tex = sample(src, w, h, u * (w - 1), v * (h - 1));
                    const float z = std::sqrt(std::max(0.0F, 1.0F - s * s));
                    const float sh = (0.35F + 0.65F * z) * dim;
                    const float tr = (tex.transparent ? 60.0F : tex.r) * sh;
                    const float tg = (tex.transparent ? 70.0F : tex.g) * sh;
                    const float tb = (tex.transparent ? 90.0F : tex.b) * sh;
                    fr = fr * (1.0F - f) + tr * f;
                    fg = fg * (1.0F - f) + tg * f;
                    fb = fb * (1.0F - f) + tb * f;
                    return true;
                  };
                  if (!merged) {
                    if (paintFunnel(cxfL, rL, spinL)) drew = true;
                    if (paintFunnel(cxfR, rR, spinR)) drew = true;
                  } else {
                    if (paintFunnel(cxfM, rM, spinL)) drew = true;  // merged spin = left's
                  }
                  if (!drew)
                  {
                    const float k = 1.0F - f;
                    fr *= k;
                    fg *= k;
                    fb *= k;
                  }
                  dst[idx] = Rgb{u8(fr), u8(fg), u8(fb), false};
                }
              }
              // Brief flash at the collision moment.
              if (merge > 0.0F && merge < 0.3F) {
                const float flashA = std::sin(merge * 3.14159F / 0.3F);
                for (int yy = 0; yy < h; ++yy)
                  for (int xx = 0; xx < w; ++xx) {
                    const float dx = xx - w * 0.5F;
                    const float dy = (yy - h * 0.6F);
                    const float r2 = dx * dx + dy * dy;
                    const float boost = std::exp(-r2 / (w * w * 0.04F)) * 120.0F * flashA;
                    Rgb& p = dst[static_cast<std::size_t>(yy) * w + xx];
                    p.r = u8(p.r + boost);
                    p.g = u8(p.g + boost * 0.9F);
                    p.b = u8(p.b + boost * 0.7F);
                  }
              }
            });
}

void effectCoriolis(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      // Two hemispheres tinted differently so the equator is unmissable.
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
          const float l = s.transparent ? 60.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
          const float sf = static_cast<float>(y) / h;
          const bool north = y < h * 0.5F;
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8((north ? 100 : 60) + l * 0.18F - 20 * sf),
                  u8((north ? 130 : 80) + l * 0.20F - 20 * sf),
                  u8((north ? 180 : 130) + l * 0.20F + 20 * sf), false};
        }
      // Equator line + N/S labels.
      const int eqY = static_cast<int>(h * 0.5F);
      for (int x = 0; x < w; ++x) {
        if (eqY > 0 && eqY < h)
          dst[static_cast<std::size_t>(eqY) * w + x] = Rgb{220, 200, 140, false};
        if (eqY - 1 >= 0)
          dst[static_cast<std::size_t>(eqY - 1) * w + x] = Rgb{180, 160, 100, false};
      }
      auto drawLabel = [&](const std::string& s, float cx, float cy, Rgb col) {
        const int sc = std::max(2, static_cast<int>(mn / 30.0F));
        const float lineW = static_cast<float>(s.size()) * 6 * sc;
        for (int ci = 0; ci < static_cast<int>(s.size()); ++ci) {
          if (s[ci] == ' ') continue;
          const auto g = glyph5x7(s[ci]);
          for (int fy = 0; fy < 7; ++fy)
            for (int fx = 0; fx < 5; ++fx)
              if (g[fy][fx] == '1')
                plotDot(dst, w, h, cx - lineW * 0.5F + (ci * 6 + fx) * sc, cy + fy * sc,
                        std::max(1.0F, static_cast<float>(sc) * 0.5F), ya, col);
        }
      };
      drawLabel("N", w * 0.06F, h * 0.06F, Rgb{240, 240, 240, false});
      drawLabel("S", w * 0.06F, h * 0.86F, Rgb{240, 240, 240, false});
      drawLabel("CORIOLIS", w * 0.5F, h * 0.04F, Rgb{240, 220, 140, false});
      // Cyclone in each hemisphere — a tight spiral of data-textured arms,
      // CCW in the north (spinSign = +1), CW in the south (-1).
      auto drawCyclone = [&](float cx, float cy, float spinSign) {
        const int nArms = 3;
        const int nPts = 50;
        for (int arm = 0; arm < nArms; ++arm) {
          const float armPh = arm * 6.2832F / nArms;
          for (int k = 0; k < nPts; ++k) {
            const float f = k / static_cast<float>(nPts - 1);
            const float r = mn * (0.02F + 0.14F * f);
            const float ang = armPh + spinSign * (f * 6.2832F * 1.2F + t * 6.2832F * 0.5F);
            const float px = cx + std::cos(ang) * r;
            const float py = cy + std::sin(ang) * r / ya;
            plotDot(dst, w, h, px, py, mn * 0.014F * (1.0F - 0.6F * f), ya,
                    Rgb{220, 240, 255, false});
          }
        }
        // Eye.
        drawDataDisk(dst, w, h, src, cx, cy, mn * 0.025F, ya, 0.6F, t * 0.4F,
                     Rgb{200, 220, 255, false});
      };
      drawCyclone(w * 0.30F, h * 0.28F, +1.0F);  // N hemisphere: CCW
      drawCyclone(w * 0.70F, h * 0.72F, -1.0F);  // S hemisphere: CW
      // Curving wind arrows: a straight-line tracer bends right-of-motion
      // in the north and left-of-motion in the south. Drawn as a series
      // of dots with each segment rotated slightly so the deflection is
      // visible at a glance.
      auto drawDeflectedArrow = [&](float x0, float y0, float dx0, float dy0, float curl) {
        float x = x0, y = y0, dx = dx0, dy = dy0;
        for (int s = 0; s < 24; ++s) {
          plotDot(dst, w, h, x, y, mn * 0.006F, ya, Rgb{255, 220, 80, false});
          // Rotate (dx, dy) by `curl` per step.
          const float c = std::cos(curl), si = std::sin(curl);
          const float ndx = dx * c - dy * si;
          const float ndy = dx * si + dy * c;
          dx = ndx; dy = ndy;
          x += dx; y += dy;
        }
      };
      // N hemisphere: launch eastward, bends to the south (right of motion).
      drawDeflectedArrow(w * 0.55F, h * 0.18F, -mn * 0.012F, 0, +0.05F);
      drawDeflectedArrow(w * 0.55F, h * 0.32F, -mn * 0.012F, 0, +0.05F);
      // S hemisphere: launch westward, bends to the north (left of motion).
      drawDeflectedArrow(w * 0.45F, h * 0.68F, +mn * 0.012F, 0, -0.05F);
      drawDeflectedArrow(w * 0.45F, h * 0.82F, +mn * 0.012F, 0, -0.05F);
    });
}

void effectDerecho(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
          const float l = s.transparent ? 80.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
          const float sf = static_cast<float>(y) / h;
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(180 - 80 * sf + l * 0.10F), u8(190 - 90 * sf + l * 0.10F),
                  u8(180 - 60 * sf + l * 0.10F), false};
        }
      // Shelf cloud bottom edge.
      const float frontX = w * (1.2F - t * 1.6F);
      const float shelfY = h * 0.40F;
      for (int yy = 0; yy <= static_cast<int>(shelfY); ++yy)
        for (int xx = static_cast<int>(frontX); xx < w; ++xx) {
          if (xx < 0 || xx >= w || yy < 0 || yy >= h) continue;
          const Rgb d = sample(src, w, h, xx, yy);
          const float dr = d.transparent ? 80.0F : d.r;
          const float dg = d.transparent ? 80.0F : d.g;
          const float db = d.transparent ? 90.0F : d.b;
          const float yf = static_cast<float>(yy) / shelfY;
          dst[static_cast<std::size_t>(yy) * w + xx] =
              Rgb{u8(40 + 60 * (1 - yf) + dr * 0.20F), u8(40 + 60 * (1 - yf) + dg * 0.20F),
                  u8(50 + 70 * (1 - yf) + db * 0.20F), false};
        }
      // Sharp leading edge.
      drawSeg(dst, w, h, frontX, 0, frontX, shelfY, std::max(1.0F, mn * 0.004F), ya,
              Rgb{20, 20, 30, false});
      // Trees bending — data-textured trunks.
      for (int i = 0; i < 12; ++i) {
        const float bx = w * (0.05F + 0.08F * i);
        const float bend = (bx > frontX) ? 0.6F : 0.0F;
        drawSeg(dst, w, h, bx, h * 0.88F, bx + bend * mn * 0.05F, h * 0.70F,
                std::max(1.0F, mn * 0.005F), ya, Rgb{60, 40, 20, false});
        plotDot(dst, w, h, bx + bend * mn * 0.05F, h * 0.70F, mn * 0.025F, ya,
                Rgb{30, 90, 40, false});
      }
    });
}

void effectElNino(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      // Space background.
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          dst[static_cast<std::size_t>(y) * w + x] = Rgb{6, 6, 16, false};
      const float cx = w * 0.5F, cy = h * 0.5F;
      const float R = mn * 0.42F;
      // Pacific-centred orthographic globe (center ~ 0°N, 170°W).
      const float centerLatDeg = 0.0F;
      const float centerLonDeg = -170.0F;
      const float cLat = std::cos(centerLatDeg * 3.14159F / 180.0F);
      const float sLat = std::sin(centerLatDeg * 3.14159F / 180.0F);
      const float grow = std::clamp(t * 1.5F, 0.0F, 1.0F);
      for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx) {
          const float u = (xx - cx) / R;
          const float v = (yy - cy) * ya / R;
          const float r2 = u * u + v * v;
          if (r2 > 1.0F) continue;
          const float nz = std::sqrt(1.0F - r2);
          // Convert (u, v, nz) → (lat, lon).
          const float yE = v * cLat + nz * sLat;       // not used directly
          const float zE = -v * sLat + nz * cLat;
          const float lat = std::asin(std::clamp(yE, -1.0F, 1.0F)) * 180.0F / 3.14159F;
          const float lon = centerLonDeg + std::atan2(u, zE) * 180.0F / 3.14159F;
          // Base SST: cool everywhere, with limb darkening.
          const Rgb d = sample(src, w, h, xx, yy);
          const float dl = d.transparent ? 80.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          const float shade = 0.65F + 0.35F * nz;
          float r0 = 40 + dl * 0.20F, g0 = 100 + dl * 0.25F, b0 = 180 + dl * 0.25F;
          // Warm anomaly: Gaussian centred on (0°N, ~95°W = -95°), broad zonal.
          const float dLon1 = lon + 95.0F;
          const float warm = std::exp(-(dLon1 * dLon1) / 1500.0F - lat * lat / 200.0F) * grow;
          // Cold tongue (western Pacific) fades.
          const float dLon2 = lon + 160.0F;
          const float cold = std::exp(-(dLon2 * dLon2) / 1500.0F - lat * lat / 200.0F) * (1.0F - grow);
          r0 += warm * 200; g0 += warm * 30 - cold * 20; b0 += -warm * 80 + cold * 50;
          dst[static_cast<std::size_t>(yy) * w + xx] =
              Rgb{u8(r0 * shade), u8(g0 * shade), u8(b0 * shade), false};
        }
      // Continent silhouettes — orthographic projections of crude polygons.
      auto plotLatLon = [&](float lat, float lon, Rgb col) {
        const float latR = lat * 3.14159F / 180.0F;
        const float lonR = (lon - centerLonDeg) * 3.14159F / 180.0F;
        const float x3 = std::cos(latR) * std::sin(lonR);
        const float y3 = std::sin(latR);
        const float z3 = std::cos(latR) * std::cos(lonR);
        if (z3 < 0.0F) return;  // back of globe
        // Un-rotate by centerLat about x-axis:
        const float yV = y3 * cLat - z3 * sLat;
        const float zV = y3 * sLat + z3 * cLat;
        (void)zV;
        const float px = cx + x3 * R;
        const float py = cy + yV * R / ya;
        if (px >= 0 && px < w && py >= 0 && py < h)
          dst[static_cast<std::size_t>(static_cast<int>(py)) * w + static_cast<int>(px)] = col;
      };
      // South America (rough): long, west coast around -75 lon.
      for (float la = -55.0F; la <= 10.0F; la += 0.5F) {
        const float westLon = -82.0F + 5.0F * std::sin(la * 0.1F);
        const float eastLon = -35.0F - 5.0F * std::sin(la * 0.1F);
        for (float lo = westLon; lo <= eastLon; lo += 1.0F)
          plotLatLon(la, lo, Rgb{120, 130, 60, false});
      }
      // Australia (rough rectangle, lon 113..153, lat -38..-12).
      for (float la = -38.0F; la <= -12.0F; la += 0.5F)
        for (float lo = 113.0F; lo <= 153.0F; lo += 1.0F)
          plotLatLon(la, lo, Rgb{180, 140, 60, false});
      // North America west coast (rough).
      for (float la = 10.0F; la <= 60.0F; la += 0.5F)
        for (float lo = -120.0F + 8.0F * std::sin(la * 0.05F); lo <= -90.0F; lo += 1.0F)
          plotLatLon(la, lo, Rgb{100, 130, 80, false});
      // Equator graticule.
      for (float lo = -180.0F; lo <= 180.0F; lo += 1.0F)
        plotLatLon(0.0F, lo, Rgb{220, 220, 140, false});
      // Label.
      const std::string label = grow > 0.6F ? "EL NINO" : "NEUTRAL";
      const int sc = std::max(2, static_cast<int>(mn / 30.0F));
      const float lineW = static_cast<float>(label.size()) * 6 * sc;
      for (int ci = 0; ci < static_cast<int>(label.size()); ++ci) {
        if (label[ci] == ' ') continue;
        const auto g = glyph5x7(label[ci]);
        for (int fy = 0; fy < 7; ++fy)
          for (int fx = 0; fx < 5; ++fx)
            if (g[fy][fx] == '1')
              plotDot(dst, w, h, w * 0.5F - lineW * 0.5F + (ci * 6 + fx) * sc, h * 0.05F + fy * sc,
                      std::max(1.0F, static_cast<float>(sc) * 0.5F), ya, Rgb{240, 220, 100, false});
      }
    });
}

void effectFogbow(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb d = sample(src, w, h, x, y);
          const float l = d.transparent ? 70.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          // Fog: desaturate strongly, lift to mid-grey.
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(160 + l * 0.20F), u8(170 + l * 0.20F), u8(180 + l * 0.18F), false};
        }
      // Bow arc — white-grey, faint.
      const float cx = w * 0.5F;
      const float cy = h * 0.95F;
      const float rOuter = mn * 0.55F;
      const float rInner = mn * 0.50F;
      const float fade = std::clamp(t * 1.5F, 0.0F, 1.0F);
      for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx) {
          const float dx = xx - cx;
          const float dy = (yy - cy) / ya;
          const float d = std::sqrt(dx * dx + dy * dy);
          if (d < rOuter && d > rInner && (yy - cy) < 0) {
            const float band = (d - rInner) / (rOuter - rInner);
            const float a = std::sin(band * 3.14159F) * 0.6F * fade;
            const Rgb& orig = dst[static_cast<std::size_t>(yy) * w + xx];
            dst[static_cast<std::size_t>(yy) * w + xx] =
                Rgb{u8(orig.r * (1 - a) + 250 * a), u8(orig.g * (1 - a) + 250 * a),
                    u8(orig.b * (1 - a) + 250 * a), false};
          }
        }
      // Sun behind fog — data-textured disk.
      drawDataDisk(dst, w, h, src, cx, h * 0.25F, mn * 0.08F, ya, 0.30F, t * 0.2F, Rgb{240, 230, 200, false});
    });
}

void effectHurricaneEye(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      const float cx = w * 0.5F, cy = h * 0.5F;
      for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx) {
          const float dx = xx - cx;
          const float dy = (yy - cy) / ya;
          const float r = std::sqrt(dx * dx + dy * dy);
          const float ang = std::atan2(dy, dx);
          const float spiralPh = ang + r * 0.04F - t * 6.0F;
          const float bandIntensity = std::cos(spiralPh * 3.0F);
          const Rgb d = sample(src, w, h, xx, yy);
          const float dl = d.transparent ? 80.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          const float fade = std::clamp(r / (mn * 0.5F), 0.0F, 1.0F);
          const float cloudy = std::max(0.0F, bandIntensity) * (1.0F - fade);
          dst[static_cast<std::size_t>(yy) * w + xx] =
              Rgb{u8(30 + 200 * cloudy + dl * 0.15F * (1 - cloudy)),
                  u8(40 + 200 * cloudy + dl * 0.20F * (1 - cloudy)),
                  u8(60 + 210 * cloudy + dl * 0.25F * (1 - cloudy)), false};
        }
      // Eye — clear, data-textured disk.
      drawDataDisk(dst, w, h, src, cx, cy, mn * 0.08F, ya, 0.5F, t * 0.5F, Rgb{120, 180, 220, false});
      // Eye wall ring.
      for (float a = 0; a < 6.2832F; a += 0.03F)
        plotDot(dst, w, h, cx + std::cos(a) * mn * 0.085F, cy + std::sin(a) * mn * 0.085F / ya,
                std::max(1.0F, mn * 0.004F), ya, Rgb{20, 20, 30, false});
    });
}

void effectSnowTree(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 6500,
    [&](float t, std::vector<Rgb>& dst) {
      // Cold-grey winter sky.
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb d = sample(src, w, h, x, y);
          const float l = d.transparent ? 60.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          const float sf = static_cast<float>(y) / h;
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(120 + 40 * (1 - sf) + l * 0.20F), u8(130 + 40 * (1 - sf) + l * 0.20F),
                  u8(150 + 40 * (1 - sf) + l * 0.22F), false};
        }
      const float groundY = h * 0.90F;
      // Snow on ground accumulates.
      const float ground = std::clamp(t * 0.6F, 0.0F, 0.6F);
      for (int yy = static_cast<int>(groundY); yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx)
          dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{u8(240 - 20 * ground), u8(245 - 15 * ground),
                                                          u8(255), false};
      // Topple kicks in around t = 0.65 → 0.80.
      const float fall = std::clamp((t - 0.65F) / 0.15F, 0.0F, 1.0F);
      const float crash = std::clamp((t - 0.80F) / 0.05F, 0.0F, 1.0F);
      const float angle = fall * 1.40F;  // radians
      const float cs = std::cos(angle), sn = std::sin(angle);
      const float pivotX = w * 0.50F, pivotY = groundY;
      auto rot = [&](float x, float y, float& rx, float& ry) {
        const float dx = x - pivotX, dy = y - pivotY;
        rx = pivotX + dx * cs - dy * sn;
        ry = pivotY + dx * sn + dy * cs;
      };
      auto drawRotSeg = [&](float x0, float y0, float x1, float y1, float thk, Rgb col) {
        float rx0, ry0, rx1, ry1;
        rot(x0, y0, rx0, ry0); rot(x1, y1, rx1, ry1);
        drawSeg(dst, w, h, rx0, ry0, rx1, ry1, std::max(1.0F, thk), ya, col);
      };
      auto rotPlot = [&](float x, float y, float r, Rgb col) {
        float rx, ry; rot(x, y, rx, ry);
        plotDot(dst, w, h, rx, ry, r, ya, col);
      };
      // Trunk + branches.
      const Rgb bark{50, 35, 20, false};
      drawRotSeg(pivotX, pivotY, pivotX, h * 0.30F, mn * 0.014F, bark);
      // Branches off the trunk.
      struct Br { float yr; float lenSign; float scale; };
      const Br brs[] = {{0.30F, +1, 1.0F},  {0.32F, -1, 0.9F}, {0.40F, +1, 1.2F},
                       {0.42F, -1, 1.1F}, {0.55F, +1, 1.4F}, {0.57F, -1, 1.3F},
                       {0.70F, +1, 1.1F}, {0.72F, -1, 1.2F}};
      const int nBr = static_cast<int>(sizeof(brs) / sizeof(brs[0]));
      for (int i = 0; i < nBr; ++i) {
        const float yy = h * brs[i].yr;
        const float dx = brs[i].lenSign * mn * 0.18F * brs[i].scale;
        const float dy = -mn * 0.04F * brs[i].scale;
        drawRotSeg(pivotX, yy, pivotX + dx, yy + dy, mn * 0.006F, bark);
        // Sub-branchlets.
        for (int j = 0; j < 3; ++j) {
          const float jf = (j + 1) / 4.0F;
          drawRotSeg(pivotX + dx * jf, yy + dy * jf,
                     pivotX + dx * jf + brs[i].lenSign * mn * 0.04F,
                     yy + dy * jf - mn * 0.03F, mn * 0.002F, bark);
        }
      }
      // Snow accumulating on branches — opacity scales with t (before fall).
      const float load = std::clamp(t * 1.4F, 0.0F, 1.0F);
      for (int i = 0; i < nBr; ++i) {
        const float yy = h * brs[i].yr;
        const float dx = brs[i].lenSign * mn * 0.18F * brs[i].scale;
        const float dy = -mn * 0.04F * brs[i].scale;
        const int n = 12;
        for (int k = 0; k <= n; ++k) {
          const float f = k / static_cast<float>(n);
          const float px = pivotX + dx * f;
          const float py = yy + dy * f;
          const Rgb d = sample(src, w, h, static_cast<int>(px), static_cast<int>(py));
          const float dl = d.transparent ? 220.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          rotPlot(px, py - mn * 0.012F * load, mn * 0.014F * load,
                  Rgb{u8(220 + dl * 0.15F), u8(230 + dl * 0.10F), u8(255), false});
        }
      }
      // Falling snowflakes (data-tinted).
      for (int i = 0; i < 200; ++i) {
        const float fx = std::fmod(hash(i) * w + t * w * 0.10F, static_cast<float>(w));
        const float fy = std::fmod(hash(i * 3) * h + t * h * 0.50F, static_cast<float>(h * 0.90F));
        plotDot(dst, w, h, fx, fy, mn * 0.005F, ya, Rgb{240, 245, 255, false});
      }
      // Crash puff: snow plume rising near impact point.
      if (crash > 0) {
        const float impactX = pivotX + mn * 0.18F * 1.4F;  // tip of upper branch after rotation
        const float impactY = groundY - mn * 0.02F;
        (void)impactX; (void)impactY;
        for (int i = 0; i < 60; ++i) {
          const float age = std::clamp(crash + hash(i * 5) * 0.3F, 0.0F, 1.0F);
          const float ph = (hash(i) - 0.5F) * 1.2F;
          const float px = pivotX + std::cos(ph) * mn * 0.15F + age * mn * 0.08F * std::sin(ph);
          const float py = groundY - age * mn * 0.10F + hash(i * 11) * mn * 0.02F;
          plotDot(dst, w, h, px, py, mn * 0.020F * (1 - age), ya,
                  Rgb{u8(240 * (1 - age * 0.3F)), u8(240 * (1 - age * 0.3F)), u8(255 * (1 - age * 0.3F)), false});
        }
      }
      (void)load;
    });
}

void effectJetStream(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 6000,
    [&](float t, std::vector<Rgb>& dst) {
      // Space background.
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          dst[static_cast<std::size_t>(y) * w + x] = Rgb{6, 6, 18, false};
      const float cx = w * 0.5F, cy = h * 0.5F;
      const float R = mn * 0.40F;
      const float centerLatDeg = 35.0F;  // tilt the globe so the mid-latitude jet sits across it
      const float cLat = std::cos(centerLatDeg * 3.14159F / 180.0F);
      const float sLat = std::sin(centerLatDeg * 3.14159F / 180.0F);
      // Fill the disk with sampled data.
      for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx) {
          const float u = (xx - cx) / R;
          const float v = (yy - cy) * ya / R;
          const float r2 = u * u + v * v;
          if (r2 > 1.0F) continue;
          const float nz = std::sqrt(1.0F - r2);
          const float shade = 0.55F + 0.45F * nz;
          const Rgb d = sample(src, w, h, xx, yy);
          const float dr = d.transparent ? 90.0F : d.r;
          const float dg = d.transparent ? 100.0F : d.g;
          const float db = d.transparent ? 130.0F : d.b;
          dst[static_cast<std::size_t>(yy) * w + xx] =
              Rgb{u8((40 + dr * 0.40F) * shade), u8((80 + dg * 0.40F) * shade),
                  u8((140 + db * 0.30F) * shade), false};
        }
      // Latitude graticule (40°N, 50°N, 60°N).
      auto plotLatLon = [&](float lat, float lon, Rgb col) {
        const float latR = lat * 3.14159F / 180.0F;
        const float lonR = lon * 3.14159F / 180.0F;
        const float x3 = std::cos(latR) * std::sin(lonR);
        const float y3 = std::sin(latR);
        const float z3 = std::cos(latR) * std::cos(lonR);
        const float yV = y3 * cLat - z3 * sLat;
        const float zV = y3 * sLat + z3 * cLat;
        if (zV < 0.0F) return;
        const float px = cx + x3 * R;
        const float py = cy - yV * R / ya;
        if (px >= 0 && px < w && py >= 0 && py < h)
          dst[static_cast<std::size_t>(static_cast<int>(py)) * w + static_cast<int>(px)] = col;
      };
      for (float lat : {30.0F, 40.0F, 50.0F, 60.0F, 70.0F})
        for (float lon = -180.0F; lon <= 180.0F; lon += 1.0F)
          plotLatLon(lat, lon, Rgb{120, 130, 160, false});
      // The polar-front jet sits near ~45°N with Rossby-wave meanders.
      // Multiple harmonics: A1*sin(x) + A2*sin(2x) + A3*sin(3x) + A4*sin(4x).
      auto jetLat = [&](float lon) {
        const float ph = lon * 3.14159F / 180.0F;
        return 45.0F + 8.0F * std::sin(ph * 1 - t * 3) +
                       5.0F * std::sin(ph * 2 + t * 2) +
                       3.0F * std::sin(ph * 3 - t * 1.5F) +
                       2.0F * std::sin(ph * 4 + t * 1.0F);
      };
      // Draw the ribbon by sampling along longitudes.
      for (float lon = -180.0F; lon <= 180.0F; lon += 0.5F) {
        const float lat = jetLat(lon);
        for (float dlat = -3.0F; dlat <= 3.0F; dlat += 0.5F) {
          const float intensity = std::exp(-(dlat * dlat) / 4.0F);
          plotLatLon(lat + dlat, lon,
                     Rgb{u8(220 + 35 * intensity), u8(120 + 100 * intensity),
                         u8(40 + 50 * intensity), false});
        }
      }
      // Particles flowing along the jet (fastest at the core).
      for (int i = 0; i < 80; ++i) {
        const float baseLon = std::fmod(hash(i) * 360.0F + t * 200.0F, 360.0F) - 180.0F;
        const float dlat = (hash(i * 7) - 0.5F) * 4.0F;
        plotLatLon(jetLat(baseLon) + dlat, baseLon, Rgb{255, 240, 200, false});
      }
    });
}

void effectMammatus(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
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
          // Stormy orange-pink dusk above, dark below.
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(220 - 140 * sf + l * 0.10F), u8(140 - 100 * sf + l * 0.10F),
                  u8(100 - 80 * sf + l * 0.10F), false};
        }
      // Cloud base line at y = h*0.45.
      const float baseY = h * 0.45F;
      for (int yy = 0; yy <= static_cast<int>(baseY); ++yy)
        for (int xx = 0; xx < w; ++xx) {
          const Rgb d = sample(src, w, h, xx, yy);
          const float dr = d.transparent ? 80.0F : d.r;
          const float yf = static_cast<float>(yy) / baseY;
          dst[static_cast<std::size_t>(yy) * w + xx] =
              Rgb{u8(80 + 60 * yf + dr * 0.15F), u8(60 + 50 * yf), u8(70 + 50 * yf), false};
        }
      // Pouches.
      const float spread = std::clamp(t * 1.2F, 0.0F, 1.0F);
      for (int i = 0; i < 8; ++i) {
        const float px = w * (0.05F + 0.12F * i);
        const float pr = mn * (0.04F + 0.015F * std::sin(i * 3.0F)) * spread;
        drawDataDisk(dst, w, h, src, px, baseY + pr * 0.5F, pr, ya, 0.80F, 0.0F,
                     Rgb{160, 120, 100, false});
      }
    });
}

void effectMonsoon(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 6000,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          dst[static_cast<std::size_t>(y) * w + x] = Rgb{6, 6, 16, false};
      const float cx = w * 0.5F, cy = h * 0.5F;
      const float R = mn * 0.42F;
      const float centerLatDeg = 15.0F;
      const float centerLonDeg = 90.0F;     // Bay of Bengal
      const float cLat = std::cos(centerLatDeg * 3.14159F / 180.0F);
      const float sLat = std::sin(centerLatDeg * 3.14159F / 180.0F);
      const float advance = std::clamp(t * 1.2F, 0.0F, 1.0F);  // monsoon front latitude
      const float frontLat = -5.0F + advance * 30.0F;          // 5°S → 25°N
      for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx) {
          const float u = (xx - cx) / R;
          const float v = (yy - cy) * ya / R;
          const float r2 = u * u + v * v;
          if (r2 > 1.0F) continue;
          const float nz = std::sqrt(1.0F - r2);
          const float yE = v * cLat + nz * sLat;
          const float lat = std::asin(std::clamp(yE, -1.0F, 1.0F)) * 180.0F / 3.14159F;
          const Rgb d = sample(src, w, h, xx, yy);
          const float dl = d.transparent ? 70.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          const float shade = 0.55F + 0.45F * nz;
          // Below front: dry warm; above front: monsoon wet (blue-grey).
          const bool wet = lat < frontLat;
          if (wet)
            dst[static_cast<std::size_t>(yy) * w + xx] =
                Rgb{u8((50 + dl * 0.25F) * shade), u8((80 + dl * 0.25F) * shade),
                    u8((120 + dl * 0.25F) * shade), false};
          else
            dst[static_cast<std::size_t>(yy) * w + xx] =
                Rgb{u8((200 + dl * 0.20F) * shade), u8((160 + dl * 0.20F) * shade),
                    u8((90 + dl * 0.15F) * shade), false};
        }
      // Continent outlines.
      auto plotLatLon = [&](float lat, float lon, Rgb col) {
        const float latR = lat * 3.14159F / 180.0F;
        const float lonR = (lon - centerLonDeg) * 3.14159F / 180.0F;
        const float x3 = std::cos(latR) * std::sin(lonR);
        const float y3 = std::sin(latR);
        const float z3 = std::cos(latR) * std::cos(lonR);
        const float yV = y3 * cLat - z3 * sLat;
        const float zV = y3 * sLat + z3 * cLat;
        if (zV < 0.0F) return;
        const float px = cx + x3 * R;
        const float py = cy - yV * R / ya;
        if (px >= 0 && px < w && py >= 0 && py < h)
          dst[static_cast<std::size_t>(static_cast<int>(py)) * w + static_cast<int>(px)] = col;
      };
      // Indian subcontinent (rough).
      for (float la = 8.0F; la <= 35.0F; la += 0.4F)
        for (float lo = 68.0F + 7.0F * std::sin(la * 0.1F); lo <= 88.0F - 4.0F * std::sin(la * 0.1F); lo += 0.8F)
          plotLatLon(la, lo, Rgb{180, 140, 70, false});
      // Indochina (rough).
      for (float la = 5.0F; la <= 25.0F; la += 0.4F)
        for (float lo = 95.0F; lo <= 110.0F; lo += 0.8F)
          plotLatLon(la, lo, Rgb{160, 130, 60, false});
      // Sri Lanka, Indonesia, Himalayas (chips).
      for (float la = 5.0F; la <= 10.0F; la += 0.5F)
        for (float lo = 79.0F; lo <= 82.0F; lo += 0.5F) plotLatLon(la, lo, Rgb{160, 130, 60, false});
      // Equator + monsoon front line.
      for (float lo = -180.0F; lo <= 180.0F; lo += 1.0F) {
        plotLatLon(0.0F, lo, Rgb{220, 220, 140, false});
        plotLatLon(frontLat, lo, Rgb{160, 200, 255, false});
      }
      // Rain streaks raining over the wet half.
      for (int i = 0; i < 250; ++i) {
        const float fx = hash(i) * w;
        const float fy = std::fmod(hash(i * 3) * h + t * h * 0.5F, static_cast<float>(h));
        const float u = (fx - cx) / R, v = (fy - cy) * ya / R;
        if (u * u + v * v > 1.0F) continue;
        const float nz = std::sqrt(1.0F - u * u - v * v);
        const float yE = v * cLat + nz * sLat;
        const float lat = std::asin(std::clamp(yE, -1.0F, 1.0F)) * 180.0F / 3.14159F;
        if (lat >= frontLat) continue;
        drawSeg(dst, w, h, fx, fy, fx + mn * 0.010F, fy + mn * 0.030F,
                std::max(1.0F, mn * 0.002F), ya, Rgb{200, 220, 240, false});
      }
    });
}

void effectPolarVortex(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 6000,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          dst[static_cast<std::size_t>(y) * w + x] = Rgb{6, 6, 18, false};
      const float cx = w * 0.5F, cy = h * 0.5F;
      const float R = mn * 0.42F;
      // The "wave" defines the polar-mid-latitude boundary in r (radius
      // from the pole). Multi-harmonic: r0 + sum A_k * cos(k*theta).
      auto boundaryR = [&](float theta) {
        return mn * (0.25F + 0.06F * std::cos(theta * 3 + t * 2.0F) +
                            0.04F * std::cos(theta * 4 - t * 1.5F) +
                            0.03F * std::cos(theta * 5 + t * 1.0F));
      };
      // Fill the polar cap. Inside the boundary = cold polar; outside =
      // warmer mid-latitudes (still data-tinted).
      for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx) {
          const float u = (xx - cx);
          const float v = (yy - cy) * ya;
          const float r = std::sqrt(u * u + v * v);
          if (r > R) continue;
          const float th = std::atan2(v, u);
          const float bR = boundaryR(th);
          const Rgb d = sample(src, w, h, xx, yy);
          const float dl = d.transparent ? 100.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          const float inside = (r < bR) ? 1.0F : 0.0F;
          // Polar inside: deep blue/violet, mid-latitudes outside: warmer.
          if (inside > 0.5F)
            dst[static_cast<std::size_t>(yy) * w + xx] =
                Rgb{u8(40 + dl * 0.20F), u8(70 + dl * 0.25F), u8(160 + dl * 0.30F), false};
          else
            dst[static_cast<std::size_t>(yy) * w + xx] =
                Rgb{u8(150 + dl * 0.25F), u8(120 + dl * 0.20F), u8(100 + dl * 0.15F), false};
        }
      // Latitude rings (every 10°: 80°, 70°, 60° → r = (90-lat)/90 * R).
      for (float lat : {60.0F, 70.0F, 80.0F}) {
        const float rr = (90.0F - lat) / 90.0F * R;
        for (float a = 0; a < 6.2832F; a += 0.01F) {
          const float xx = cx + std::cos(a) * rr;
          const float yy = cy + std::sin(a) * rr / ya;
          if (xx >= 0 && xx < w && yy >= 0 && yy < h)
            dst[static_cast<std::size_t>(static_cast<int>(yy)) * w + static_cast<int>(xx)] =
                Rgb{160, 180, 200, false};
        }
      }
      // Meridians every 30°.
      for (int m = 0; m < 12; ++m) {
        const float a = m * 0.5236F;
        for (float rr = 0; rr <= R; rr += 0.5F) {
          const int xx = static_cast<int>(cx + std::cos(a) * rr);
          const int yy = static_cast<int>(cy + std::sin(a) * rr / ya);
          if (xx >= 0 && xx < w && yy >= 0 && yy < h && static_cast<int>(rr) % 6 < 2)
            dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{140, 160, 180, false};
        }
      }
      // Highlight the wave boundary.
      for (float a = 0; a < 6.2832F; a += 0.005F) {
        const float rr = boundaryR(a);
        const float xx = cx + std::cos(a) * rr;
        const float yy = cy + std::sin(a) * rr / ya;
        if (xx >= 0 && xx < w && yy >= 0 && yy < h)
          plotDot(dst, w, h, xx, yy, std::max(1.0F, mn * 0.003F), ya, Rgb{255, 240, 200, false});
      }
      // Pole marker (+ N label).
      plotDot(dst, w, h, cx, cy, mn * 0.012F, ya, Rgb{255, 255, 255, false});
      const int sc = std::max(2, static_cast<int>(mn / 35.0F));
      const auto g = glyph5x7('N');
      for (int fy = 0; fy < 7; ++fy)
        for (int fx = 0; fx < 5; ++fx)
          if (g[fy][fx] == '1')
            plotDot(dst, w, h, cx + (fx - 2) * sc, cy + mn * 0.04F + fy * sc,
                    std::max(1.0F, static_cast<float>(sc) * 0.5F), ya, Rgb{240, 240, 240, false});
    });
}

void effectSupercell(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      const bool flash = std::fmod(t * 5.0F, 1.0F) < 0.04F;
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
          const float l = s.transparent ? 50.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
          const float sf = static_cast<float>(y) / h;
          const float fb = flash ? 80.0F : 0.0F;
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(40 + 30 * sf + l * 0.08F + fb), u8(40 + 30 * sf + l * 0.08F + fb),
                  u8(60 + 30 * sf + l * 0.08F + fb), false};
        }
      // Anvil top spread and stem — data-textured.
      const float cx = w * 0.5F;
      const float anvilY = h * 0.18F;
      const float baseY = h * 0.70F;
      for (int yy = static_cast<int>(anvilY); yy <= static_cast<int>(baseY); ++yy) {
        const float yf = (yy - anvilY) / (baseY - anvilY);
        // Mushroom profile: wide top, narrow neck, swelling base.
        const float halfW = mn * (0.40F - 0.30F * std::sin(yf * 3.14159F * 0.5F) + 0.15F * yf);
        for (int xo = -static_cast<int>(halfW); xo <= static_cast<int>(halfW); ++xo) {
          const int xx = static_cast<int>(cx + xo);
          if (xx < 0 || xx >= w || yy < 0 || yy >= h) continue;
          const Rgb d = sample(src, w, h, xx, yy);
          const float dr = d.transparent ? 100.0F : d.r;
          const float dg = d.transparent ? 100.0F : d.g;
          const float db = d.transparent ? 110.0F : d.b;
          dst[static_cast<std::size_t>(yy) * w + xx] =
              Rgb{u8(70 + dr * 0.30F), u8(70 + dg * 0.30F), u8(85 + db * 0.30F), false};
        }
      }
      // Wall cloud (the data-rich pouch under the storm).
      drawDataDisk(dst, w, h, src, cx, baseY + mn * 0.04F, mn * 0.12F, ya, 0.75F, 0.0F,
                   Rgb{60, 50, 70, false});
      // Lightning bolts (random forks).
      if (flash) {
        for (int b = 0; b < 3; ++b) {
          float bx = cx + (hash(b) - 0.5F) * mn * 0.15F;
          float by = baseY + mn * 0.04F;
          for (int s = 0; s < 6; ++s) {
            const float nx = bx + (hash(b * 7 + s) - 0.5F) * mn * 0.05F;
            const float ny = by + mn * 0.04F;
            drawSeg(dst, w, h, bx, by, nx, ny, std::max(1.0F, mn * 0.003F), ya,
                    Rgb{240, 240, 255, false});
            bx = nx; by = ny;
            if (by > h * 0.92F) break;
          }
        }
      }
    });
}

void effectAmoc(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 6000,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          dst[static_cast<std::size_t>(y) * w + x] = Rgb{6, 6, 18, false};
      const float cx = w * 0.5F, cy = h * 0.5F;
      const float R = mn * 0.42F;
      const float centerLatDeg = 30.0F, centerLonDeg = -30.0F;
      const float cLat = std::cos(centerLatDeg * 3.14159F / 180.0F);
      const float sLat = std::sin(centerLatDeg * 3.14159F / 180.0F);
      for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx) {
          float lat, lon;
          if (!globePxToLatLon(cx, cy, R, ya, cLat, sLat, centerLonDeg, xx, yy, lat, lon)) continue;
          const float u = (xx - cx) / R;
          const float v = (yy - cy) * ya / R;
          const float nz = std::sqrt(std::max(0.0F, 1.0F - u * u - v * v));
          const float shade = 0.55F + 0.45F * nz;
          const Rgb d = sample(src, w, h, xx, yy);
          const float dl = d.transparent ? 60.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          dst[static_cast<std::size_t>(yy) * w + xx] =
              Rgb{u8((20 + dl * 0.20F) * shade), u8((60 + dl * 0.25F) * shade),
                  u8((110 + dl * 0.30F) * shade), false};
        }
      // Rough continent outlines (Americas, Europe/Africa).
      auto plotLandPoly = [&](const std::vector<std::pair<float,float>>& pts, Rgb col) {
        for (size_t i = 0; i + 1 < pts.size(); ++i) {
          const int n = 30;
          for (int k = 0; k <= n; ++k) {
            const float f = k / static_cast<float>(n);
            const float la = pts[i].first + f * (pts[i+1].first - pts[i].first);
            const float lo = pts[i].second + f * (pts[i+1].second - pts[i].second);
            float px, py;
            if (globeLatLonToPx(cx, cy, R, ya, cLat, sLat, centerLonDeg, la, lo, px, py))
              plotDot(dst, w, h, px, py, std::max(1.0F, mn * 0.003F), ya, col);
          }
        }
      };
      // Americas (rough east coast).
      plotLandPoly({{60,-65},{50,-60},{40,-75},{30,-80},{25,-80},{10,-78},{0,-50},{-30,-50},{-50,-70}},
                   Rgb{120, 130, 80, false});
      // Europe + Africa (rough west coast).
      plotLandPoly({{65,15},{45,-10},{35,-10},{20,-18},{0,-10},{-20,15},{-35,18}},
                   Rgb{140, 130, 70, false});
      // Surface warm conveyor: lat/lon waypoints from Gulf to Norwegian Sea.
      const std::vector<std::pair<float,float>> warm = {
          {25,-80},{30,-75},{35,-72},{40,-65},{45,-50},{50,-35},{55,-20},{60,-5},{65,5}};
      for (int seg = 0; seg + 1 < static_cast<int>(warm.size()); ++seg) {
        const int npt = 14;
        for (int k = 0; k <= npt; ++k) {
          const float f = k / static_cast<float>(npt);
          const float phaseOff = (seg * npt + k) / static_cast<float>(warm.size() * npt);
          const float pulse = 0.6F + 0.4F * std::sin((phaseOff - t) * 12.0F);
          const float la = warm[seg].first + f * (warm[seg+1].first - warm[seg].first);
          const float lo = warm[seg].second + f * (warm[seg+1].second - warm[seg].second);
          float px, py;
          if (globeLatLonToPx(cx, cy, R, ya, cLat, sLat, centerLonDeg, la, lo, px, py))
            plotDot(dst, w, h, px, py, mn * 0.010F * pulse, ya,
                    Rgb{u8(255 * pulse), u8(140 * pulse), u8(60), false});
        }
      }
      // Cold deep return: south-bound dashes a bit further east.
      const std::vector<std::pair<float,float>> cold = {
          {65,-25},{55,-30},{45,-30},{30,-25},{10,-30},{-10,-25},{-30,-15}};
      for (int seg = 0; seg + 1 < static_cast<int>(cold.size()); ++seg) {
        const int npt = 10;
        for (int k = 0; k <= npt; ++k) {
          const float f = k / static_cast<float>(npt);
          const float la = cold[seg].first + f * (cold[seg+1].first - cold[seg].first);
          const float lo = cold[seg].second + f * (cold[seg+1].second - cold[seg].second);
          const float phaseOff = (seg * npt + k) / static_cast<float>(cold.size() * npt);
          const float dashOn = std::fmod(phaseOff + t * 0.5F, 0.15F) < 0.1F;
          if (!dashOn) continue;
          float px, py;
          if (globeLatLonToPx(cx, cy, R, ya, cLat, sLat, centerLonDeg, la, lo, px, py))
            plotDot(dst, w, h, px, py, mn * 0.006F, ya, Rgb{60, 120, 220, false});
        }
      }
    });
}

void effectAuroralOval(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 6000,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          dst[static_cast<std::size_t>(y) * w + x] = Rgb{4, 6, 22, false};
      const float cx = w * 0.5F, cy = h * 0.5F;
      const float R = mn * 0.42F;
      // Polar projection (centre = North Pole). Lat = 90 - r * 90 / R.
      for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx) {
          const float u = xx - cx;
          const float v = (yy - cy) * ya;
          const float r = std::sqrt(u * u + v * v);
          if (r > R) continue;
          const Rgb d = sample(src, w, h, xx, yy);
          const float dl = d.transparent ? 60.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          const float lat = 90.0F - r / R * 90.0F;
          // Ocean blue baseline; whiter near pole.
          const float ice = std::clamp((lat - 70.0F) / 20.0F, 0.0F, 1.0F);
          dst[static_cast<std::size_t>(yy) * w + xx] =
              Rgb{u8(20 + 200 * ice + dl * 0.15F), u8(40 + 200 * ice + dl * 0.15F),
                  u8(80 + 80 * ice + dl * 0.20F), false};
        }
      // Auroral oval around magnetic pole (offset to ~80°N, 72°W).
      const float magLat = 80.0F, magLon = -72.0F;
      // Translate magnetic pole into screen pixel via polar projection.
      const float mlonR = magLon * 3.14159F / 180.0F;
      const float mr = (90.0F - magLat) / 90.0F * R;
      const float mpx = cx + std::cos(mlonR + 1.5708F) * mr;
      const float mpy = cy + std::sin(mlonR + 1.5708F) * mr / ya;
      // Pulsing oval.
      const float pulse = 0.7F + 0.3F * std::sin(t * 4.0F);
      const float ovalR = mn * 0.18F * pulse;
      const float ovalE = 0.7F;  // ellipticity
      for (float a = 0; a < 6.2832F; a += 0.01F) {
        const float ang = a + magLon * 3.14159F / 180.0F;
        const float xx = mpx + std::cos(ang) * ovalR;
        const float yy = mpy + std::sin(ang) * ovalR * ovalE / ya;
        // Aurora: green inner, violet outer.
        for (float dR = -mn * 0.020F; dR <= mn * 0.020F; dR += mn * 0.004F) {
          const float xx2 = mpx + std::cos(ang) * (ovalR + dR);
          const float yy2 = mpy + std::sin(ang) * (ovalR + dR) * ovalE / ya;
          const float fadeR = 1.0F - std::fabs(dR) / (mn * 0.020F);
          const bool inner = dR < 0;
          plotDot(dst, w, h, xx2, yy2, std::max(1.0F, mn * 0.002F), ya,
                  inner ? Rgb{u8(60 * fadeR * pulse), u8(255 * fadeR * pulse), u8(120 * fadeR * pulse), false}
                        : Rgb{u8(180 * fadeR * pulse), u8(80 * fadeR * pulse), u8(220 * fadeR * pulse), false});
        }
        (void)xx; (void)yy;
      }
      // Latitude rings.
      for (float lat : {60.0F, 70.0F, 80.0F}) {
        const float rr = (90.0F - lat) / 90.0F * R;
        for (float a = 0; a < 6.2832F; a += 0.02F) {
          const float xx = cx + std::cos(a) * rr;
          const float yy = cy + std::sin(a) * rr / ya;
          if (xx >= 0 && xx < w && yy >= 0 && yy < h)
            dst[static_cast<std::size_t>(static_cast<int>(yy)) * w + static_cast<int>(xx)] =
                Rgb{100, 140, 180, false};
        }
      }
    });
}

void effectHadleyCell(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 6000,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb d = sample(src, w, h, x, y);
          const float dl = d.transparent ? 50.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          const float sf = static_cast<float>(y) / h;
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(40 + 40 * (1 - sf) + dl * 0.20F), u8(60 + 60 * (1 - sf) + dl * 0.25F),
                  u8(120 + 60 * (1 - sf) + dl * 0.25F), false};
        }
      // Earth's curved surface across the bottom + sides — half-disc.
      const float cx = w * 0.5F, cy = h * 1.20F;
      const float R = mn * 1.10F;
      for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx) {
          const float dx = xx - cx;
          const float dy = (yy - cy) * ya;
          const float r = std::sqrt(dx * dx + dy * dy);
          if (r > R) continue;
          const Rgb d = sample(src, w, h, xx, yy);
          const float dl = d.transparent ? 80.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          dst[static_cast<std::size_t>(yy) * w + xx] =
              Rgb{u8(60 + dl * 0.30F), u8(120 + dl * 0.30F), u8(80), false};
        }
      // Latitude markers.
      const float eqY = h * 0.65F;
      const float nh30Y = h * 0.45F;
      const float sh30Y = h * 0.85F;
      const float topY = h * 0.18F;
      for (int x = 0; x < w; ++x) {
        if (eqY > 0 && eqY < h) dst[static_cast<std::size_t>(eqY) * w + x] = Rgb{220, 200, 100, false};
        if (nh30Y > 0 && nh30Y < h) dst[static_cast<std::size_t>(nh30Y) * w + x] = Rgb{180, 160, 120, false};
        if (sh30Y > 0 && sh30Y < h) dst[static_cast<std::size_t>(sh30Y) * w + x] = Rgb{180, 160, 120, false};
        if (topY > 0 && topY < h) dst[static_cast<std::size_t>(topY) * w + x] = Rgb{100, 120, 160, false};
      }
      // Arrows for each cell.
      auto cellArrows = [&](float botY, float topY2, float side) {
        const float advance = std::fmod(t * 0.6F, 1.0F);
        // 1: rising at equator. 2: poleward aloft. 3: sinking at 30°.
        // 4: equatorward at surface.
        const std::vector<std::tuple<float,float,float,float>> segs = {
            {w * 0.5F, botY, w * 0.5F, topY2},                 // rise
            {w * 0.5F, topY2, w * 0.5F + side * w * 0.30F, topY2 + side * 0},  // poleward aloft
            {w * 0.5F + side * w * 0.30F, topY2, w * 0.5F + side * w * 0.30F, botY},  // sink
            {w * 0.5F + side * w * 0.30F, botY, w * 0.5F, botY}  // return
        };
        for (size_t i = 0; i < segs.size(); ++i) {
          const float x0 = std::get<0>(segs[i]), y0 = std::get<1>(segs[i]);
          const float x1 = std::get<2>(segs[i]), y1 = std::get<3>(segs[i]);
          // Animated dots flowing along this segment.
          for (int k = 0; k < 12; ++k) {
            const float f = std::fmod(k / 12.0F + advance + i * 0.10F, 1.0F);
            const float fx = x0 + f * (x1 - x0);
            const float fy = y0 + f * (y1 - y0);
            plotDot(dst, w, h, fx, fy, mn * 0.008F, ya, Rgb{255, 240, 120, false});
          }
        }
      };
      cellArrows(eqY, nh30Y, +1.0F);
      cellArrows(eqY, sh30Y, -1.0F);
      // Don't forget the ascending branch at the equator (both cells share).
    });
}

void effectHurricaneTracks(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 6000,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          dst[static_cast<std::size_t>(y) * w + x] = Rgb{6, 6, 18, false};
      const float cx = w * 0.5F, cy = h * 0.5F;
      const float R = mn * 0.42F;
      const float centerLatDeg = 25.0F, centerLonDeg = -55.0F;
      const float cLat = std::cos(centerLatDeg * 3.14159F / 180.0F);
      const float sLat = std::sin(centerLatDeg * 3.14159F / 180.0F);
      for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx) {
          float lat, lon;
          if (!globePxToLatLon(cx, cy, R, ya, cLat, sLat, centerLonDeg, xx, yy, lat, lon)) continue;
          const Rgb d = sample(src, w, h, xx, yy);
          const float dl = d.transparent ? 50.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          dst[static_cast<std::size_t>(yy) * w + xx] =
              Rgb{u8(20 + dl * 0.20F), u8(60 + dl * 0.25F), u8(110 + dl * 0.30F), false};
        }
      // Continents (rough).
      auto plotLand = [&](const std::vector<std::pair<float,float>>& pts) {
        for (size_t i = 0; i + 1 < pts.size(); ++i) {
          for (int k = 0; k <= 20; ++k) {
            const float f = k / 20.0F;
            const float la = pts[i].first + f * (pts[i+1].first - pts[i].first);
            const float lo = pts[i].second + f * (pts[i+1].second - pts[i].second);
            float px, py;
            if (globeLatLonToPx(cx, cy, R, ya, cLat, sLat, centerLonDeg, la, lo, px, py))
              plotDot(dst, w, h, px, py, std::max(1.0F, mn * 0.003F), ya, Rgb{130, 130, 80, false});
          }
        }
      };
      plotLand({{50,-80},{40,-75},{35,-80},{30,-82},{25,-80},{20,-90},{15,-85},{10,-78}});  // N+C America
      plotLand({{30,-15},{15,-18},{0,-10},{-15,5}});                                         // Africa west
      // Multiple tracks (lat, lon) sequences, each progressively drawn.
      const std::vector<std::vector<std::pair<float,float>>> tracks = {
          {{15,-25},{18,-40},{22,-55},{26,-65},{32,-72},{38,-72},{43,-65},{45,-55}},
          {{12,-20},{15,-35},{20,-50},{25,-60},{30,-65},{35,-65}},
          {{10,-30},{14,-45},{18,-60},{22,-70},{27,-75},{30,-78}},
          {{18,-55},{22,-70},{28,-75},{35,-72},{40,-65},{45,-55},{50,-45}},
      };
      const float reveal = std::clamp(t * 1.3F, 0.0F, 1.0F);
      for (size_t i = 0; i < tracks.size(); ++i) {
        const float startDelay = i * 0.15F;
        const float trackProg = std::clamp((reveal - startDelay) / 0.5F, 0.0F, 1.0F);
        if (trackProg <= 0) continue;
        const int nSeg = static_cast<int>(tracks[i].size()) - 1;
        const int nSeg_drawn = static_cast<int>(trackProg * nSeg);
        for (int seg = 0; seg <= nSeg_drawn && seg < nSeg; ++seg) {
          for (int k = 0; k <= 12; ++k) {
            const float f = k / 12.0F;
            const float la = tracks[i][seg].first + f * (tracks[i][seg+1].first - tracks[i][seg].first);
            const float lo = tracks[i][seg].second + f * (tracks[i][seg+1].second - tracks[i][seg].second);
            float px, py;
            if (globeLatLonToPx(cx, cy, R, ya, cLat, sLat, centerLonDeg, la, lo, px, py))
              plotDot(dst, w, h, px, py, std::max(1.0F, mn * 0.003F), ya, Rgb{255, 80, 60, false});
          }
        }
        // Head: cyclone symbol at current end.
        const int seg = std::min(nSeg - 1, nSeg_drawn);
        const float la = tracks[i][seg].first;
        const float lo = tracks[i][seg].second;
        float px, py;
        if (globeLatLonToPx(cx, cy, R, ya, cLat, sLat, centerLonDeg, la, lo, px, py))
          drawDataDisk(dst, w, h, src, px, py, mn * 0.015F, ya, 0.8F, t * 4.0F, Rgb{255, 200, 80, false});
      }
    });
}

void effectItcz(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 6500,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          dst[static_cast<std::size_t>(y) * w + x] = Rgb{4, 4, 14, false};
      const float cx = w * 0.5F, cy = h * 0.5F;
      const float R = mn * 0.42F;
      const float centerLonDeg = std::fmod(t * 60.0F, 360.0F) - 180.0F;
      const float cLat = 1.0F, sLat = 0.0F;
      // ITCZ latitude oscillates +/- 10° with season.
      const float itczLat = 5.0F + 10.0F * std::sin(t * 6.2832F);
      for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx) {
          float lat, lon;
          if (!globePxToLatLon(cx, cy, R, ya, cLat, sLat, centerLonDeg, xx, yy, lat, lon)) continue;
          const Rgb d = sample(src, w, h, xx, yy);
          const float dl = d.transparent ? 60.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          // Cloud band intensity: gaussian centred on itczLat, ±5° wide.
          const float band = std::exp(-((lat - itczLat) * (lat - itczLat)) / 30.0F);
          const float u2 = (xx - cx) / R, v2 = (yy - cy) * ya / R;
          const float nz = std::sqrt(std::max(0.0F, 1.0F - u2 * u2 - v2 * v2));
          const float shade = 0.55F + 0.45F * nz;
          // Ocean + bright cloud band.
          dst[static_cast<std::size_t>(yy) * w + xx] =
              Rgb{u8((30 + 200 * band + dl * 0.20F) * shade),
                  u8((60 + 200 * band + dl * 0.25F) * shade),
                  u8((120 + 200 * band + dl * 0.30F) * shade), false};
          // Sprinkle convective spots inside the band.
          if (band > 0.5F && hash(xx * 13 + yy * 7 + static_cast<int>(t * 20)) > 0.985F)
            dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{255, 255, 255, false};
        }
    });
}

void effectKrakatoa(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 6000,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          dst[static_cast<std::size_t>(y) * w + x] = Rgb{6, 6, 18, false};
      const float cx = w * 0.5F, cy = h * 0.5F;
      const float R = mn * 0.42F;
      const float centerLatDeg = -5.0F, centerLonDeg = 105.0F;
      const float cLat = std::cos(centerLatDeg * 3.14159F / 180.0F);
      const float sLat = std::sin(centerLatDeg * 3.14159F / 180.0F);
      // Globe baseline.
      for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx) {
          float lat, lon;
          if (!globePxToLatLon(cx, cy, R, ya, cLat, sLat, centerLonDeg, xx, yy, lat, lon)) continue;
          const Rgb d = sample(src, w, h, xx, yy);
          const float dl = d.transparent ? 60.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          const float u = (xx - cx) / R, v = (yy - cy) * ya / R;
          const float nz = std::sqrt(std::max(0.0F, 1.0F - u * u - v * v));
          const float shade = 0.55F + 0.45F * nz;
          // Plume: at latitude ~-6, longitude band growing eastward and westward from 105°E.
          const float plumeLat = -6.0F;
          const float dLat = lat - plumeLat;
          // Angular distance from eruption longitude.
          float dLon = std::fmod(lon - 105.0F + 540.0F, 360.0F) - 180.0F;
          const float spread = std::clamp(t * 180.0F, 0.0F, 180.0F);
          const float band = (std::fabs(dLon) < spread) ? std::exp(-(dLat * dLat) / 50.0F) : 0.0F;
          const float ash = band * std::clamp(t * 1.5F, 0.0F, 1.0F);
          dst[static_cast<std::size_t>(yy) * w + xx] =
              Rgb{u8(((30 + 100 * ash) + dl * 0.20F) * shade),
                  u8(((50 + 80 * ash) + dl * 0.20F) * shade),
                  u8(((110 + 60 * ash) + dl * 0.20F) * shade), false};
        }
      // Initial volcanic flash + plume vertical at Krakatoa location.
      const float erupt = std::clamp(1.0F - t * 3.0F, 0.0F, 1.0F);
      if (erupt > 0) {
        float px, py;
        if (globeLatLonToPx(cx, cy, R, ya, cLat, sLat, centerLonDeg, -6.0F, 105.0F, px, py))
          plotDot(dst, w, h, px, py, mn * 0.030F * erupt, ya, Rgb{255, 160, 60, false});
      }
      // Ash particles drifting.
      for (int i = 0; i < 60; ++i) {
        const float lon = std::fmod(105.0F - 180.0F * t + (hash(i) - 0.5F) * 90.0F * t + 540.0F, 360.0F) - 180.0F;
        const float lat = -6.0F + (hash(i * 3) - 0.5F) * 10.0F;
        float px, py;
        if (globeLatLonToPx(cx, cy, R, ya, cLat, sLat, centerLonDeg, lat, lon, px, py))
          plotDot(dst, w, h, px, py, mn * 0.006F, ya, Rgb{120, 100, 90, false});
      }
    });
}

void effectMjo(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 6500,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          dst[static_cast<std::size_t>(y) * w + x] = Rgb{4, 4, 14, false};
      const float cx = w * 0.5F, cy = h * 0.5F;
      const float R = mn * 0.42F;
      const float centerLonDeg = std::fmod(t * 40.0F, 360.0F) - 180.0F;
      const float cLat = 1.0F, sLat = 0.0F;
      // Envelope longitude marches east.
      const float envLon = std::fmod(t * 720.0F + 540.0F, 360.0F) - 180.0F;
      for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx) {
          float lat, lon;
          if (!globePxToLatLon(cx, cy, R, ya, cLat, sLat, centerLonDeg, xx, yy, lat, lon)) continue;
          const Rgb d = sample(src, w, h, xx, yy);
          const float dl = d.transparent ? 60.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          const float u = (xx - cx) / R, v = (yy - cy) * ya / R;
          const float nz = std::sqrt(std::max(0.0F, 1.0F - u * u - v * v));
          const float shade = 0.55F + 0.45F * nz;
          // Wrap-aware longitude distance.
          float dLon = std::fmod(lon - envLon + 540.0F, 360.0F) - 180.0F;
          const float env = std::exp(-(dLon * dLon) / 1000.0F - lat * lat / 150.0F);
          dst[static_cast<std::size_t>(yy) * w + xx] =
              Rgb{u8(((40 + 180 * env) + dl * 0.25F) * shade),
                  u8(((70 + 160 * env) + dl * 0.25F) * shade),
                  u8(((130 + 100 * env) + dl * 0.20F) * shade), false};
        }
    });
}

void effectOzoneHole(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 6000,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          dst[static_cast<std::size_t>(y) * w + x] = Rgb{4, 4, 14, false};
      const float cx = w * 0.5F, cy = h * 0.5F;
      const float R = mn * 0.42F;
      // South pole projection: lat = -90 + r/R * 90.
      for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx) {
          const float u = xx - cx;
          const float v = (yy - cy) * ya;
          const float r = std::sqrt(u * u + v * v);
          if (r > R) continue;
          const Rgb d = sample(src, w, h, xx, yy);
          const float dl = d.transparent ? 60.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          const float lat = -90.0F + r / R * 90.0F;
          const bool ice = lat < -70.0F;
          // Default: cold blue ocean / white Antarctica.
          float r0, g0, b0;
          if (ice) {
            r0 = 220 + dl * 0.10F;
            g0 = 230 + dl * 0.10F;
            b0 = 240;
          } else {
            r0 = 40 + dl * 0.20F;
            g0 = 80 + dl * 0.25F;
            b0 = 130 + dl * 0.30F;
          }
          // Ozone hole: pulsing dark patch over the pole, max in October-Nov.
          const float pulse = 0.6F + 0.4F * std::sin(t * 6.2832F);
          const float holeR = mn * 0.25F * pulse;
          if (r < holeR) {
            const float hole = 1.0F - r / holeR;
            r0 = r0 * (1 - 0.7F * hole) + 30 * hole;
            g0 = g0 * (1 - 0.7F * hole) + 0 * hole;
            b0 = b0 * (1 - 0.7F * hole) + 80 * hole;
          }
          dst[static_cast<std::size_t>(yy) * w + xx] =
              Rgb{u8(r0), u8(g0), u8(b0), false};
        }
      // Latitude rings.
      for (float lat : {-60.0F, -70.0F, -80.0F}) {
        const float rr = (90.0F + lat) / 90.0F * R;
        for (float a = 0; a < 6.2832F; a += 0.02F) {
          const float xx = cx + std::cos(a) * rr;
          const float yy = cy + std::sin(a) * rr / ya;
          if (xx >= 0 && xx < w && yy >= 0 && yy < h)
            dst[static_cast<std::size_t>(static_cast<int>(yy)) * w + static_cast<int>(xx)] =
                Rgb{120, 140, 180, false};
        }
      }
    });
}

void effectSaharanDust(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 6000,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          dst[static_cast<std::size_t>(y) * w + x] = Rgb{6, 6, 18, false};
      const float cx = w * 0.5F, cy = h * 0.5F;
      const float R = mn * 0.42F;
      const float centerLatDeg = 15.0F, centerLonDeg = -30.0F;
      const float cLat = std::cos(centerLatDeg * 3.14159F / 180.0F);
      const float sLat = std::sin(centerLatDeg * 3.14159F / 180.0F);
      for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx) {
          float lat, lon;
          if (!globePxToLatLon(cx, cy, R, ya, cLat, sLat, centerLonDeg, xx, yy, lat, lon)) continue;
          const Rgb d = sample(src, w, h, xx, yy);
          const float dl = d.transparent ? 60.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          // Africa: simplified band on the east; SA on the west.
          const bool africa = lon > -20.0F && lon < 50.0F && lat > -30.0F && lat < 30.0F;
          const bool amazonia = lon > -80.0F && lon < -45.0F && lat > -15.0F && lat < 10.0F;
          float r0, g0, b0;
          if (africa) { r0 = 200 + dl * 0.15F; g0 = 160 + dl * 0.10F; b0 = 80; }
          else if (amazonia) { r0 = 60 + dl * 0.15F; g0 = 140 + dl * 0.20F; b0 = 50; }
          else { r0 = 30 + dl * 0.20F; g0 = 70 + dl * 0.25F; b0 = 130 + dl * 0.30F; }
          dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{u8(r0), u8(g0), u8(b0), false};
        }
      // Dust plume particles: drift from Sahara (~20°N, 10°E) westward to Caribbean (~15°N, -65°E).
      for (int i = 0; i < 200; ++i) {
        const float lifeT = std::fmod(t + hash(i), 1.0F);
        const float lat = 20.0F + (hash(i * 7) - 0.5F) * 10.0F;
        const float lonStart = 10.0F, lonEnd = -65.0F;
        const float lon = lonStart + lifeT * (lonEnd - lonStart);
        float px, py;
        if (globeLatLonToPx(cx, cy, R, ya, cLat, sLat, centerLonDeg, lat, lon, px, py))
          plotDot(dst, w, h, px, py, mn * 0.006F, ya, Rgb{220, 180, 100, false});
      }
    });
}

void effectSeaIce(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 6500,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          dst[static_cast<std::size_t>(y) * w + x] = Rgb{4, 4, 14, false};
      const float cx = w * 0.5F, cy = h * 0.5F;
      const float R = mn * 0.42F;
      // North-pole projection. lat = 90 - r/R * 90.
      // Ice cap radius oscillates with t (winter wider, summer narrower).
      const float icePulse = 0.55F + 0.35F * std::sin(t * 6.2832F);  // 0.20..0.90
      const float iceR = R * icePulse;
      for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx) {
          const float u = xx - cx;
          const float v = (yy - cy) * ya;
          const float r = std::sqrt(u * u + v * v);
          if (r > R) continue;
          const Rgb d = sample(src, w, h, xx, yy);
          const float dl = d.transparent ? 60.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          // Continents (rough): Russia, Alaska, Canada, Greenland — sketch as
          // sectors of the disk.
          const float theta = std::atan2(v, u);
          const float lat = 90.0F - r / R * 90.0F;
          const bool ice = r < iceR;
          const bool island = (lat < 70.0F) && (
              (theta > -2.0F && theta < -1.0F) ||  // Russia sector
              (theta > 1.0F && theta < 2.0F) ||    // North America sector
              (theta > 2.5F || theta < -2.5F));    // Greenland sector
          if (ice) {
            const float fade = 1.0F - r / iceR;
            dst[static_cast<std::size_t>(yy) * w + xx] =
                Rgb{u8(220 + 30 * fade + dl * 0.10F), u8(230 + 20 * fade + dl * 0.10F),
                    u8(250), false};
          } else if (island) {
            dst[static_cast<std::size_t>(yy) * w + xx] =
                Rgb{u8(100 + dl * 0.20F), u8(130 + dl * 0.20F), u8(80), false};
          } else {
            dst[static_cast<std::size_t>(yy) * w + xx] =
                Rgb{u8(20 + dl * 0.20F), u8(50 + dl * 0.25F), u8(110 + dl * 0.30F), false};
          }
        }
      // Latitude rings.
      for (float lat : {60.0F, 70.0F, 80.0F}) {
        const float rr = (90.0F - lat) / 90.0F * R;
        for (float a = 0; a < 6.2832F; a += 0.02F) {
          const float xx = cx + std::cos(a) * rr;
          const float yy = cy + std::sin(a) * rr / ya;
          if (xx >= 0 && xx < w && yy >= 0 && yy < h)
            dst[static_cast<std::size_t>(static_cast<int>(yy)) * w + static_cast<int>(xx)] =
                Rgb{160, 180, 210, false};
        }
      }
    });
}

void effectTsunami(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 6500,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          dst[static_cast<std::size_t>(y) * w + x] = Rgb{6, 6, 18, false};
      const float cx = w * 0.5F, cy = h * 0.5F;
      const float R = mn * 0.42F;
      const float centerLatDeg = 20.0F, centerLonDeg = 160.0F;
      const float cLat = std::cos(centerLatDeg * 3.14159F / 180.0F);
      const float sLat = std::sin(centerLatDeg * 3.14159F / 180.0F);
      // Pre-compute epicentre screen position.
      const float epiLat = 38.0F, epiLon = 143.0F;  // Tohoku
      float epiPx = 0, epiPy = 0;
      const bool epiVisible =
          globeLatLonToPx(cx, cy, R, ya, cLat, sLat, centerLonDeg, epiLat, epiLon, epiPx, epiPy);
      for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx) {
          float lat, lon;
          if (!globePxToLatLon(cx, cy, R, ya, cLat, sLat, centerLonDeg, xx, yy, lat, lon)) continue;
          const Rgb d = sample(src, w, h, xx, yy);
          const float dl = d.transparent ? 60.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          float r0 = 20 + dl * 0.20F, g0 = 60 + dl * 0.25F, b0 = 110 + dl * 0.30F;
          // Continents (rough land mask).
          const bool land = (lon > 100.0F && lon < 145.0F && lat > 0.0F && lat < 50.0F)  // Asia
                         || (lon > -130.0F && lon < -75.0F && lat > -60.0F && lat < 60.0F) // Americas
                         || (lon > 140.0F && lon < 180.0F && lat > -45.0F && lat < -10.0F); // Australia
          if (land) { r0 = 120 + dl * 0.10F; g0 = 130; b0 = 80; }
          // Concentric wave fronts.
          if (epiVisible) {
            const float dx = xx - epiPx;
            const float dy = (yy - epiPy) * ya;
            const float r = std::sqrt(dx * dx + dy * dy);
            const float speed = mn * 1.5F;
            const float wave = std::sin(r * 0.10F - t * speed * 0.05F);
            const float reach = std::clamp(t * speed - r, 0.0F, mn * 0.4F) / (mn * 0.4F);
            if (!land && reach > 0)
              { r0 += 80 * wave * reach; g0 += 60 * wave * reach; b0 += 100 * wave * reach; }
          }
          dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{u8(r0), u8(g0), u8(b0), false};
        }
      // Mark the epicentre.
      if (epiVisible)
        plotDot(dst, w, h, epiPx, epiPy, mn * 0.012F, ya, Rgb{255, 80, 80, false});
    });
}

void effectWalkerCell(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 6500,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          dst[static_cast<std::size_t>(y) * w + x] = Rgb{6, 6, 18, false};
      const float cx = w * 0.5F, cy = h * 0.5F;
      const float R = mn * 0.42F;
      const float centerLonDeg = -150.0F;
      const float cLat = 1.0F, sLat = 0.0F;
      for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx) {
          float lat, lon;
          if (!globePxToLatLon(cx, cy, R, ya, cLat, sLat, centerLonDeg, xx, yy, lat, lon)) continue;
          const Rgb d = sample(src, w, h, xx, yy);
          const float dl = d.transparent ? 50.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          dst[static_cast<std::size_t>(yy) * w + xx] =
              Rgb{u8(20 + dl * 0.20F), u8(60 + dl * 0.25F), u8(110 + dl * 0.30F), false};
        }
      // Convective rising at ~110°E (Indonesia): cloud cluster.
      for (int i = 0; i < 30; ++i) {
        const float a = i / 30.0F * 6.2832F;
        const float lat = 0.0F + std::sin(a + t * 3.0F) * 6.0F;
        const float lon = 110.0F + std::cos(a + t * 3.0F) * 6.0F;
        float px, py;
        if (globeLatLonToPx(cx, cy, R, ya, cLat, sLat, centerLonDeg, lat, lon, px, py))
          plotDot(dst, w, h, px, py, mn * 0.020F, ya, Rgb{240, 240, 250, false});
      }
      // Sinking at ~ -80°E (Peru): clear, dry-bright.
      for (int i = 0; i < 12; ++i) {
        const float a = i / 12.0F * 6.2832F;
        const float lat = -10.0F + std::sin(a) * 3.0F;
        const float lon = -80.0F + std::cos(a) * 4.0F;
        float px, py;
        if (globeLatLonToPx(cx, cy, R, ya, cLat, sLat, centerLonDeg, lat, lon, px, py))
          plotDot(dst, w, h, px, py, mn * 0.008F, ya, Rgb{220, 200, 140, false});
      }
      // East-west arrows along equator (surface easterlies + upper westerlies).
      const float adv = std::fmod(t * 1.5F, 1.0F);
      for (int k = 0; k < 12; ++k) {
        const float lonE = -180.0F + (k / 12.0F) * 360.0F + adv * 30.0F;
        float px, py;
        if (globeLatLonToPx(cx, cy, R, ya, cLat, sLat, centerLonDeg, 0.0F, lonE, px, py)) {
          plotDot(dst, w, h, px - mn * 0.005F, py, mn * 0.005F, ya, Rgb{255, 240, 120, false});
        }
      }
    });
}

void effectWildfireSmoke(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 6000,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          dst[static_cast<std::size_t>(y) * w + x] = Rgb{6, 6, 18, false};
      const float cx = w * 0.5F, cy = h * 0.5F;
      const float R = mn * 0.42F;
      const float centerLatDeg = 45.0F, centerLonDeg = -90.0F;
      const float cLat = std::cos(centerLatDeg * 3.14159F / 180.0F);
      const float sLat = std::sin(centerLatDeg * 3.14159F / 180.0F);
      for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx) {
          float lat, lon;
          if (!globePxToLatLon(cx, cy, R, ya, cLat, sLat, centerLonDeg, xx, yy, lat, lon)) continue;
          const Rgb d = sample(src, w, h, xx, yy);
          const float dl = d.transparent ? 60.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          // Crude land vs ocean: longitude band -130 .. -65 + latitude 25..70 = N America.
          const bool land = lon > -135.0F && lon < -65.0F && lat > 25.0F && lat < 70.0F;
          float r0, g0, b0;
          if (land) { r0 = 90 + dl * 0.20F; g0 = 130 + dl * 0.20F; b0 = 60; }
          else { r0 = 30 + dl * 0.20F; g0 = 70 + dl * 0.25F; b0 = 110 + dl * 0.30F; }
          dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{u8(r0), u8(g0), u8(b0), false};
        }
      // Fires + smoke trails.
      const std::vector<std::pair<float,float>> fires = {
          {45,-122}, {50,-115}, {40,-105}, {55,-100}};
      for (const auto& f : fires) {
        float fpx, fpy;
        if (!globeLatLonToPx(cx, cy, R, ya, cLat, sLat, centerLonDeg, f.first, f.second, fpx, fpy)) continue;
        plotDot(dst, w, h, fpx, fpy, mn * 0.012F, ya, Rgb{255, 80, 30, false});
        // Smoke particles drift east + north.
        for (int i = 0; i < 40; ++i) {
          const float age = std::fmod(t * 1.5F + hash(i + static_cast<int>(f.first * 7)), 1.0F);
          const float driftLon = f.second + age * 40.0F + (hash(i * 3) - 0.5F) * 8.0F;
          const float driftLat = f.first + age * 8.0F + (hash(i * 5) - 0.5F) * 4.0F;
          float spx, spy;
          if (globeLatLonToPx(cx, cy, R, ya, cLat, sLat, centerLonDeg, driftLat, driftLon, spx, spy))
            plotDot(dst, w, h, spx, spy, mn * 0.010F * (1 - age), ya,
                    Rgb{u8(160), u8(140), u8(130), false});
        }
      }
    });
}

}  // namespace ee_detail
}  // namespace Qdless
