#include <cmath>

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
    float x0 = 0.0F;
    float y0 = 0.0F;
    float r0 = 0.0F;
    float drift = 0.0F;
    float phase = 0.0F;
    float fall = 0.0F;
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

namespace
{
// Clamp a channel value to 0..255
std::uint8_t clampU8(float v)
{
  return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F));
}

constexpr int kAuroraCurtains = 4;
constexpr int kAuroraStars = 50;

// One swaying aurora curtain
struct AuroraCurtain
{
  float base = 0.0F;
  float ampX = 0.0F;
  float ky = 0.0F;
  float spd = 0.0F;
  float phase = 0.0F;
  float width = 0.0F;
  Rgb col;
};

using AuroraCurtains = std::array<AuroraCurtain, kAuroraCurtains>;

// Horizontal centre of each curtain on the row at relative height fy
std::array<float, kAuroraCurtains> curtainCentres(const AuroraCurtains& cs, float fy, float t, int w)
{
  std::array<float, kAuroraCurtains> cxw{};
  for (int c = 0; c < kAuroraCurtains; ++c)
    cxw[c] =
        (cs[c].base + cs[c].ampX * std::sin(fy * cs[c].ky + t * 6.2832F * cs[c].spd + cs[c].phase)) *
        w;
  return cxw;
}

// Night-sky aurora pass state shared by every pixel of one frame
struct AuroraFrame
{
  float t;
  float vf;     // remaining view fraction
  float night;  // 1 - vf
  float env;    // aurora swell/fade envelope
};

// One pixel: the dimmed view on a deep-blue night sky plus the light of every curtain
Rgb auroraPixel(const Rgb& s0,
                int x,
                int w,
                float fy,
                float vp,
                const AuroraFrame& fr,
                const AuroraCurtains& cs,
                const std::array<float, kAuroraCurtains>& cxw)
{
  float r = s0.transparent ? 0.0F : s0.r * fr.vf;
  float g = s0.transparent ? 0.0F : s0.g * fr.vf;
  float b = s0.transparent ? 0.0F : s0.b * fr.vf;
  r += 3.0F * fr.night;
  g += 5.0F * fr.night;
  b += 14.0F * fr.night;  // deep-blue night sky
  for (int c = 0; c < kAuroraCurtains; ++c)
  {
    const float d = (x - cxw[c]) / (cs[c].width * w);
    const float band = std::exp(-d * d);
    const float ray = 0.55F + 0.45F * std::sin(x * 0.5F + fy * 9.0F + fr.t * 4.0F + cs[c].phase);
    const float inten = band * vp * ray * fr.env * 1.1F;
    r += cs[c].col.r * inten;
    g += cs[c].col.g * inten;
    b += cs[c].col.b * inten;
  }
  return Rgb{clampU8(r), clampU8(g), clampU8(b), false};
}

// Stars twinkling in as the night falls
void drawTwinklingStars(std::vector<Rgb>& dst,
                        int w,
                        int h,
                        const std::array<float, kAuroraStars>& stx,
                        const std::array<float, kAuroraStars>& sty,
                        const std::array<float, kAuroraStars>& stp,
                        float t,
                        float night)
{
  for (int i = 0; i < kAuroraStars; ++i)
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
    dst[idx] = Rgb{clampU8(cur.r + bri), clampU8(cur.g + bri), clampU8(cur.b + bri), false};
  }
}

// A falling rain streak
struct RainDrop
{
  float x;
  float y0;
  float spd;
  float len;
};

// A lightning polyline (main bolt or a fork) and the time it strikes
struct BoltStrike
{
  float t0;
  std::vector<std::pair<float, float>> seg;
};

// Append a jagged bolt striking at t0, and with 80% probability a branching fork
void addLightningStrike(std::vector<BoltStrike>& strikes,
                        float t0,
                        int w,
                        int h,
                        std::mt19937& rng,
                        std::uniform_real_distribution<float>& u01)
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
}

// The view darkening into a storm-tinted sky as vf goes 1..0
void drawStormTint(std::vector<Rgb>& dst, const std::vector<Rgb>& src, float vf)
{
  for (std::size_t i = 0; i < dst.size(); ++i)
  {
    const Rgb& s0 = src[i];
    float r = s0.transparent ? 0.0F : s0.r * vf * 0.7F;
    float g = s0.transparent ? 0.0F : s0.g * vf * 0.7F;
    float b = s0.transparent ? 0.0F : s0.b * vf * 0.7F;
    r += 8.0F * (1.0F - vf);
    g += 10.0F * (1.0F - vf);
    b += 18.0F * (1.0F - vf);  // storm tint
    dst[i] = Rgb{clampU8(r), clampU8(g), clampU8(b), false};
  }
}

// Diagonal rain streaks added onto dst
void drawDiagonalRain(std::vector<Rgb>& dst,
                      int w,
                      int h,
                      const std::array<RainDrop, 150>& drops,
                      float t,
                      const Rgb& rainCol)
{
  for (const auto& d : drops)
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
      dst[idx] = Rgb{clampU8(cur.r + rainCol.r * a),
                     clampU8(cur.g + rainCol.g * a),
                     clampU8(cur.b + rainCol.b * a),
                     false};
    }
  }
}

// Brighten every pixel towards white by 255 * fk
void flashScreen(std::vector<Rgb>& dst, float fk)
{
  for (std::size_t i = 0; i < dst.size(); ++i)
  {
    const Rgb c = dst[i];
    dst[i] = Rgb{
        clampU8(c.r + 255.0F * fk), clampU8(c.g + 255.0F * fk), clampU8(c.b + 255.0F * fk), false};
  }
}

// Plot the polyline densely with dots of the bolt colour
void drawBoltPolyline(std::vector<Rgb>& dst,
                      int w,
                      int h,
                      float ya,
                      const std::vector<std::pair<float, float>>& seg,
                      const Rgb& bolt)
{
  for (std::size_t i = 0; i + 1 < seg.size(); ++i)
  {
    const auto& p0 = seg[i];
    const auto& p1 = seg[i + 1];
    const int steps =
        static_cast<int>(std::max(2.0F, std::hypot(p1.first - p0.first, p1.second - p0.second)));
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

// Is (bx, by) inside the stingray outline kRay (even-odd rule)?
bool insideStingray(float bx, float by)
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
}

// Is body-frame point (bx, by) on the swaying whip tail behind the body?
bool onStingrayTail(float bx, float by, float tailPhase)
{
  if (by >= -0.55F || by <= -1.8F)
    return false;
  const float c0 = 0.42F * std::sin(by * 2.4F + tailPhase);  // sway
  const float wtail = 0.012F + 0.05F * std::clamp((by + 1.8F) / 1.25F, 0.0F, 1.0F);
  return std::fabs(bx - c0) < wtail;
}

// Pose of the stingray drawn by drawStingray
struct StingrayPose
{
  float cx;
  float cy;
  float scaleX;
  float scaleY;
  float rot;
  float flapPhase;
  float tailPhase;
  float dim;
};

// The view-textured stingray, flapping its wings and swaying its tail
void drawStingray(std::vector<Rgb>& dst,
                  const std::vector<Rgb>& src,
                  int w,
                  int h,
                  float ya,
                  const StingrayPose& p)
{
  const float cr = std::cos(p.rot);
  const float sr = std::sin(p.rot);
  const float ext = std::max(p.scaleX * 1.05F, p.scaleY * 1.85F);  // wings vs tail reach
  const int x0 = std::max(0, static_cast<int>(p.cx - ext));
  const int x1 = std::min(w - 1, static_cast<int>(p.cx + ext));
  const int y0 = std::max(0, static_cast<int>(p.cy - ext / ya));
  const int y1 = std::min(h - 1, static_cast<int>(p.cy + ext / ya));
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x)
    {
      const float dx = x - p.cx;
      const float dyp = (y - p.cy) * ya;
      const float bx = (dx * cr + dyp * sr) / p.scaleX;  // lateral
      const float by = (dx * sr - dyp * cr) / p.scaleY;  // longitudinal
      const bool body =
          insideStingray(bx, by - 0.12F * bx * bx * std::sin(p.flapPhase));  // flap bend
      const bool tail = !body && onStingrayTail(bx, by, p.tailPhase);
      if (!body && !tail)
        continue;
      const float tu = std::clamp((bx + 1.0F) * 0.5F, 0.0F, 1.0F);
      const float tv = std::clamp((1.0F - by) * 0.5F, 0.0F, 1.0F);
      const Rgb tex = sample(src, w, h, tu * (w - 1), tv * (h - 1));
      const float tr = tex.transparent ? 70.0F : tex.r;
      const float tg = tex.transparent ? 80.0F : tex.g;
      const float tb = tex.transparent ? 95.0F : tex.b;
      const float edge = 1.0F - 0.35F * std::clamp(std::fabs(bx) - 0.25F, 0.0F, 1.0F);
      const float flap = 1.0F + 0.30F * std::sin(std::fabs(bx) * 5.0F - p.flapPhase);
      const float sh = (tail ? 0.45F : edge * flap) * p.dim;
      dst[static_cast<std::size_t>(y) * w + x] =
          Rgb{clampU8(tr * sh), clampU8(tg * sh), clampU8(tb * sh), false};
    }
}

// Accumulated funnel colour of one pixel
struct FunnelColour
{
  float r;
  float g;
  float b;
};

// Blend the spinning funnel texture into c for a pixel dx from the funnel axis,
// where r is the funnel radius on row v. False if the pixel is outside the funnel.
bool blendFunnel(const std::vector<Rgb>& src,
                 int w,
                 int h,
                 float dx,
                 float r,
                 float spin,
                 float v,
                 float dim,
                 float f,
                 FunnelColour& c)
{
  if (std::fabs(dx) > r)
    return false;
  const float s = dx / r;
  float u = std::asin(std::clamp(s, -1.0F, 1.0F)) / 3.14159F * 0.5F + spin * 0.16F +
            v * 2.2F;  // angle + spin + helix
  u -= std::floor(u);
  const Rgb tex = sample(src, w, h, u * (w - 1), v * (h - 1));
  const float z = std::sqrt(std::max(0.0F, 1.0F - s * s));
  const float sh = (0.35F + 0.65F * z) * dim;
  const float tr = (tex.transparent ? 60.0F : tex.r) * sh;
  const float tg = (tex.transparent ? 70.0F : tex.g) * sh;
  const float tb = (tex.transparent ? 90.0F : tex.b) * sh;
  c.r = c.r * (1.0F - f) + tr * f;
  c.g = c.g * (1.0F - f) + tg * f;
  c.b = c.b * (1.0F - f) + tb * f;
  return true;
}

// The view colour of a pixel as a funnel colour accumulator
FunnelColour funnelBase(const Rgb& base)
{
  return {base.transparent ? 0.0F : base.r,
          base.transparent ? 0.0F : base.g,
          base.transparent ? 0.0F : base.b};
}

// Final pixel: the funnel colour, or the surroundings fading to dark as the funnel forms
Rgb funnelPixel(FunnelColour c, bool drew, float f)
{
  if (!drew)
  {
    const float k = 1.0F - f;  // surroundings fade to dark as the funnel forms
    c.r *= k;
    c.g *= k;
    c.b *= k;
  }
  return Rgb{clampU8(c.r), clampU8(c.g), clampU8(c.b), false};
}

// One row of the tornado duel: two funnels, or the merged one
struct DuelRow
{
  float v;
  bool inBand;
  float rL;
  float rR;
  float rM;
  float cxfL;
  float cxfR;
  float cxfM;
};

// Per-frame state of the tornado duel
struct DuelFrame
{
  float f;
  float dim;
  float spinL;
  float spinR;
  bool merged;
};

// Colour of pixel x on a tornado duel row
Rgb duelPixel(const std::vector<Rgb>& src, int w, int h, int x, const Rgb& base, const DuelRow& row, const DuelFrame& fr)
{
  FunnelColour c = funnelBase(base);
  bool drew = false;
  auto paintFunnel = [&](float cxf, float r, float spin)
  {
    if (!row.inBand || r <= 1.0F)
      return false;
    return blendFunnel(src, w, h, x - cxf, r, spin, row.v, fr.dim, fr.f, c);
  };
  if (!fr.merged)
  {
    if (paintFunnel(row.cxfL, row.rL, fr.spinL))
      drew = true;
    if (paintFunnel(row.cxfR, row.rR, fr.spinR))
      drew = true;
  }
  else
  {
    if (paintFunnel(row.cxfM, row.rM, fr.spinL))
      drew = true;  // merged spin = left's
  }
  return funnelPixel(c, drew, fr.f);
}

// Warm radial flash around the collision point of the two funnels
void drawCollisionFlash(std::vector<Rgb>& dst, int w, int h, float flashA)
{
  for (int yy = 0; yy < h; ++yy)
    for (int xx = 0; xx < w; ++xx)
    {
      const float dx = xx - w * 0.5F;
      const float dy = (yy - h * 0.6F);
      const float r2 = dx * dx + dy * dy;
      const float boost = std::exp(-r2 / (w * w * 0.04F)) * 120.0F * flashA;
      Rgb& p = dst[static_cast<std::size_t>(yy) * w + xx];
      p.r = clampU8(p.r + boost);
      p.g = clampU8(p.g + boost * 0.9F);
      p.b = clampU8(p.b + boost * 0.7F);
    }
}

// Supercell anvil and stem: a data-textured mushroom profile from anvilY to baseY
void drawSupercellBody(std::vector<Rgb>& dst,
                       const std::vector<Rgb>& src,
                       int w,
                       int h,
                       float cx,
                       float anvilY,
                       float baseY,
                       float mn)
{
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
          Rgb{clampU8(70 + dr * 0.30F), clampU8(70 + dg * 0.30F), clampU8(85 + db * 0.30F), false};
    }
  }
}

// Three forked bolts from the wall cloud down towards the ground
template <typename Hash>
void drawWallCloudBolts(std::vector<Rgb>& dst, int w, int h, float cx, float baseY, float mn, float ya, Hash&& hash)
{
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

// Fill the full-width rows y0..h-1 with one colour
void fillRowsBelow(std::vector<Rgb>& dst, int w, int h, int y0, const Rgb& c)
{
  for (int yy = y0; yy < h; ++yy)
    for (int xx = 0; xx < w; ++xx)
      dst[static_cast<std::size_t>(yy) * w + xx] = c;
}

// Snow plume rising where the toppled tree hits the ground
template <typename Hash>
void drawSnowPlume(std::vector<Rgb>& dst, int w, int h, float pivotX, float groundY, float crash, float mn, float ya, Hash&& hash)
{
  for (int i = 0; i < 60; ++i) {
    const float age = std::clamp(crash + hash(i * 5) * 0.3F, 0.0F, 1.0F);
    const float ph = (hash(i) - 0.5F) * 1.2F;
    const float px = pivotX + std::cos(ph) * mn * 0.15F + age * mn * 0.08F * std::sin(ph);
    const float py = groundY - age * mn * 0.10F + hash(i * 11) * mn * 0.02F;
    plotDot(dst, w, h, px, py, mn * 0.020F * (1 - age), ya,
            Rgb{clampU8(240 * (1 - age * 0.3F)), clampU8(240 * (1 - age * 0.3F)), clampU8(255 * (1 - age * 0.3F)), false});
  }
}
}  // namespace

void effectAurora(
    const Renderer& renderer, const std::vector<Rgb>& src, int w, int h, std::mt19937& rng)
{
  constexpr int kC = kAuroraCurtains;
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
  AuroraCurtains cs{};
  for (int i = 0; i < kC; ++i)
    cs[i] = {ubase(rng), uamp(rng), uky(rng), uspd(rng), uph(rng), uwid(rng), kHue[i]};

  constexpr int kStars = kAuroraStars;
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
        const AuroraFrame frame{t, vf, night, env};
        for (int y = 0; y < h; ++y)
        {
          const float fy = static_cast<float>(y) / h;
          const float vp = std::clamp((fy - 0.03F) / 0.18F, 0.0F, 1.0F) *
                           std::clamp((0.9F - fy) / 0.55F, 0.0F, 1.0F);  // rays hang from the top
          const auto cxw = curtainCentres(cs, fy, t, w);
          for (int x = 0; x < w; ++x)
          {
            const std::size_t idx = static_cast<std::size_t>(y) * w + x;
            dst[idx] = auroraPixel(src[idx], x, w, fy, vp, frame, cs, cxw);
          }
        }
        drawTwinklingStars(dst, w, h, stx, sty, stp, t, night);
      });
}

void effectThunderstorm(
    const Renderer& renderer, const std::vector<Rgb>& src, int w, int h, std::mt19937& rng)
{
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float ya = yAspectFor(renderer);
  constexpr int kRain = 150;
  std::uniform_real_distribution<float> u01(0.0F, 1.0F);
  std::array<RainDrop, kRain> drops{};
  for (auto& d : drops)
  {
    d.x = u01(rng) * w;
    d.y0 = u01(rng) * h;
    d.spd = 0.8F + u01(rng) * 0.9F;
    d.len = 3.0F + u01(rng) * 4.0F;
  }
  std::vector<BoltStrike> strikes;
  auto buildBolt = [&](float t0) { addLightningStrike(strikes, t0, w, h, rng, u01); };
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
              drawStormTint(dst, src, vf);
              drawDiagonalRain(dst, w, h, drops, t, rainCol);  // diagonal rain
              for (const auto& s : strikes)
              {
                const float age = t - s.t0;
                if (age < 0.0F || age > 0.22F)
                  continue;
                const float k = 1.0F - age / 0.22F;
                if (age < 0.06F)  // full-screen flash
                  flashScreen(dst, (1.0F - age / 0.06F) * 0.7F);
                const Rgb bolt{u8(200.0F + 55.0F * k), u8(220.0F + 35.0F * k), 255, false};
                drawBoltPolyline(dst, w, h, ya, s.seg, bolt);
              }
            });
}

void effectStingray(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float cx0 = (w - 1) * 0.5F;
  const float cy0 = (h - 1) * 0.5F;
  const float mn = std::min(static_cast<float>(w), h * ya);

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
          drawStingray(dst, src, w, h, ya, {cx0, cy0, s * 1.15F, s, 0.0F, flapPhase, tailPhase, 1.0F});
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
        drawStingray(dst,
                     src,
                     w,
                     h,
                     ya,
                     {px, py, s * 1.15F, s * foreLong, rot, flapPhase, tailPhase, 1.0F - 0.65F * tau});
      });
}

void effectTornado(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float cx0 = (w - 1) * 0.5F;
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
                  FunnelColour c = funnelBase(src[idx]);
                  const bool drew = inBand && r > 1.0F &&
                                    blendFunnel(src, w, h, x - cxf, r, spin, v, dim, f, c);
                  dst[idx] = funnelPixel(c, drew, f);
                }
              }
            });
}

void effectTornadoDuel(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
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
              const DuelFrame frame{f, dim, spinL, spinR, merged};
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
                const DuelRow row{v, inBand, rL, rR, rM, cxfL, cxfR, cxfM};
                for (int x = 0; x < w; ++x)
                {
                  const std::size_t idx = static_cast<std::size_t>(y) * w + x;
                  dst[idx] = duelPixel(src, w, h, x, src[idx], row, frame);
                }
              }
              // Brief flash at the collision moment.
              if (merge > 0.0F && merge < 0.3F)
                drawCollisionFlash(dst, w, h, std::sin(merge * 3.14159F / 0.3F));
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
      shadePixels(dst, src, w, h, [&](const Rgb& /*s*/, int x, int y) {
        const Rgb d = sample(src, w, h, x, y);
        const float l = d.transparent ? 60.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
        const float sf = static_cast<float>(y) / h;
        return Rgb{u8(120 + 40 * (1 - sf) + l * 0.20F), u8(130 + 40 * (1 - sf) + l * 0.20F),
                   u8(150 + 40 * (1 - sf) + l * 0.22F), false};
      });
      const float groundY = h * 0.90F;
      // Snow on ground accumulates.
      const float ground = std::clamp(t * 0.6F, 0.0F, 0.6F);
      fillRowsBelow(dst, w, h, static_cast<int>(groundY),
                    Rgb{u8(240 - 20 * ground), u8(245 - 15 * ground), u8(255), false});
      // Topple kicks in around t = 0.65 → 0.80.
      const float fall = std::clamp((t - 0.65F) / 0.15F, 0.0F, 1.0F);
      const float crash = std::clamp((t - 0.80F) / 0.05F, 0.0F, 1.0F);
      const float angle = fall * 1.40F;  // radians
      const float cs = std::cos(angle);
      const float sn = std::sin(angle);
      const float pivotX = w * 0.50F;
      const float pivotY = groundY;
      auto rot = [&](float x, float y, float& rx, float& ry)
      {
        const float dx = x - pivotX;
        const float dy = y - pivotY;
        rx = pivotX + dx * cs - dy * sn;
        ry = pivotY + dx * sn + dy * cs;
      };
      auto drawRotSeg = [&](float x0, float y0, float x1, float y1, float thk, Rgb col)
      {
        float rx0 = NAN;
        float ry0 = NAN;
        float rx1 = NAN;
        float ry1 = NAN;
        rot(x0, y0, rx0, ry0);
        rot(x1, y1, rx1, ry1);
        drawSeg(dst, w, h, rx0, ry0, rx1, ry1, std::max(1.0F, thk), ya, col);
      };
      auto rotPlot = [&](float x, float y, float r, Rgb col)
      {
        float rx = NAN;
        float ry = NAN;
        rot(x, y, rx, ry);
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
      if (crash > 0)
        drawSnowPlume(dst, w, h, pivotX, groundY, crash, mn, ya, hash);
      (void)load;
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
      shadePixels(dst, src, w, h, [&](const Rgb& s, int x, int y) {
        const float l = s.transparent ? 50.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
        const float sf = static_cast<float>(y) / h;
        const float fb = flash ? 80.0F : 0.0F;
        return Rgb{u8(40 + 30 * sf + l * 0.08F + fb), u8(40 + 30 * sf + l * 0.08F + fb),
                   u8(60 + 30 * sf + l * 0.08F + fb), false};
      });
      // Anvil top spread and stem — data-textured.
      const float cx = w * 0.5F;
      const float anvilY = h * 0.18F;
      const float baseY = h * 0.70F;
      drawSupercellBody(dst, src, w, h, cx, anvilY, baseY, mn);
      // Wall cloud (the data-rich pouch under the storm).
      drawDataDisk(dst, w, h, src, cx, baseY + mn * 0.04F, mn * 0.12F, ya, 0.75F, 0.0F,
                   Rgb{60, 50, 70, false});
      // Lightning bolts (random forks).
      if (flash)
        drawWallCloudBolts(dst, w, h, cx, baseY, mn, ya, hash);
    });
}

}  // namespace ee_detail
}  // namespace Qdless
