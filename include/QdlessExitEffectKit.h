#pragma once

// Shared toolkit for the exit effects that use the view itself as the medium
// (QdlessExitEffectEarth.cpp, QdlessExitEffectGlobe.cpp): float colours,
// bilinear sampling, value noise, blurs, a semi-Lagrangian flow map and radar
// colours. Everything is inline so each effect TU optimises it in place.

#include "QdlessExitEffectCommon.h"

namespace Qdless
{
namespace ee_detail
{
namespace kit
{

constexpr float kPi = 3.14159265F;

struct F3
{
  float r = 0.0F;
  float g = 0.0F;
  float b = 0.0F;
};

inline std::uint8_t c8(float v)
{
  return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F));
}

inline Rgb toRgb(const F3& c)
{
  return Rgb{c8(c.r), c8(c.g), c8(c.b), false};
}

inline F3 mix(const F3& a, const F3& b, float t)
{
  return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

inline F3 mul(const F3& a, float k)
{
  return {a.r * k, a.g * k, a.b * k};
}

inline F3 add(const F3& a, const F3& b)
{
  return {a.r + b.r, a.g + b.g, a.b + b.b};
}

inline float luma(const F3& c)
{
  return 0.299F * c.r + 0.587F * c.g + 0.114F * c.b;
}

inline float smooth01(float e0, float e1, float x)
{
  const float t = std::clamp((x - e0) / (e1 - e0), 0.0F, 1.0F);
  return t * t * (3.0F - 2.0F * t);
}

inline float fract(float x)
{
  return x - std::floor(x);
}

// Pixel of the view; the terminal-default background reads as black.
inline F3 texel(const std::vector<Rgb>& src, int w, int x, int y)
{
  const Rgb& p = src[static_cast<std::size_t>(y) * w + x];
  if (p.transparent)
    return {};
  return {static_cast<float>(p.r), static_cast<float>(p.g), static_cast<float>(p.b)};
}

// Bilinear sample, clamped to the edges.
inline F3 bilinear(const std::vector<Rgb>& src, int w, int h, float fx, float fy)
{
  fx = std::clamp(fx, 0.0F, static_cast<float>(w - 1));
  fy = std::clamp(fy, 0.0F, static_cast<float>(h - 1));
  const int x0 = static_cast<int>(fx);
  const int y0 = static_cast<int>(fy);
  const int x1 = std::min(x0 + 1, w - 1);
  const int y1 = std::min(y0 + 1, h - 1);
  const float ax = fx - x0;
  const float ay = fy - y0;
  const F3 top = mix(texel(src, w, x0, y0), texel(src, w, x1, y0), ax);
  const F3 bot = mix(texel(src, w, x0, y1), texel(src, w, x1, y1), ax);
  return mix(top, bot, ay);
}

// Same for a scalar field.
inline float bilinear(const std::vector<float>& f, int w, int h, float fx, float fy)
{
  fx = std::clamp(fx, 0.0F, static_cast<float>(w - 1));
  fy = std::clamp(fy, 0.0F, static_cast<float>(h - 1));
  const int x0 = static_cast<int>(fx);
  const int y0 = static_cast<int>(fy);
  const int x1 = std::min(x0 + 1, w - 1);
  const int y1 = std::min(y0 + 1, h - 1);
  const float ax = fx - x0;
  const float ay = fy - y0;
  const auto at = [&](int x, int y) { return f[static_cast<std::size_t>(y) * w + x]; };
  const float top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * ax;
  const float bot = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * ax;
  return top + (bot - top) * ay;
}

inline std::uint32_t hashU(std::uint32_t x)
{
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}

inline float hash01(int x, int y, int s)
{
  const auto k = static_cast<std::uint32_t>(x) * 73856093U ^
                 static_cast<std::uint32_t>(y) * 19349663U ^
                 static_cast<std::uint32_t>(s) * 83492791U;
  return static_cast<float>(hashU(k)) / 4294967295.0F;
}

// Smooth value noise in [0, 1].
inline float vnoise(float x, float y, int s)
{
  const float xf = std::floor(x);
  const float yf = std::floor(y);
  const int xi = static_cast<int>(xf);
  const int yi = static_cast<int>(yf);
  float ax = x - xf;
  float ay = y - yf;
  ax = ax * ax * (3.0F - 2.0F * ax);
  ay = ay * ay * (3.0F - 2.0F * ay);
  const float a = hash01(xi, yi, s);
  const float b = hash01(xi + 1, yi, s);
  const float c = hash01(xi, yi + 1, s);
  const float d = hash01(xi + 1, yi + 1, s);
  return a + (b - a) * ax + (c - a) * ay + (a - b - c + d) * ax * ay;
}

// Fractal Brownian motion in [0, 1].
inline float fbm(float x, float y, int octaves, int s)
{
  float sum = 0.0F;
  float amp = 0.5F;
  float norm = 0.0F;
  for (int i = 0; i < octaves; ++i)
  {
    sum += amp * vnoise(x, y, s + i * 101);
    norm += amp;
    amp *= 0.5F;
    x = x * 2.03F + 17.1F;
    y = y * 2.03F + 5.3F;
  }
  return sum / norm;
}

// Separable box blur of an RGB image, repeated `passes` times.
inline std::vector<F3> blurred(const std::vector<Rgb>& src, int w, int h, int radius, int passes)
{
  std::vector<F3> a(static_cast<std::size_t>(w) * h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      a[static_cast<std::size_t>(y) * w + x] = texel(src, w, x, y);
  std::vector<F3> b(a.size());
  const float n = 2.0F * radius + 1.0F;
  for (int p = 0; p < passes; ++p)
  {
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
      {
        F3 s;
        for (int k = -radius; k <= radius; ++k)
          s = add(s, a[static_cast<std::size_t>(y) * w + std::clamp(x + k, 0, w - 1)]);
        b[static_cast<std::size_t>(y) * w + x] = mul(s, 1.0F / n);
      }
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
      {
        F3 s;
        for (int k = -radius; k <= radius; ++k)
          s = add(s, b[static_cast<std::size_t>(std::clamp(y + k, 0, h - 1)) * w + x]);
        a[static_cast<std::size_t>(y) * w + x] = mul(s, 1.0F / n);
      }
  }
  return a;
}

// Where each screen pixel's fluid came from: a map of source coordinates
// carried along by a velocity field, one semi-Lagrangian (RK2) step per
// frame. Sampling the view through it makes the picture flow.
//   Clamp: fluid entering the screen repeats the edge.
//   Wrap:  periodic in x; u is stored unwrapped (u(x + w) = u(x) + w) so
//          interpolation across the seam stays continuous.
//   Open:  fluid entering from off-screen brings its own (off-screen)
//          coordinates; pair it with mirrored() sampling of the view.
enum class Boundary : std::uint8_t
{
  Clamp,
  Wrap,
  Open,
};

// Mirror-extend a coordinate into [0, n - 1].
inline float mirrored(float v, int n)
{
  const float period = 2.0F * (n - 1);
  v = std::fabs(v - std::floor(v / period) * period);
  return v > n - 1 ? period - v : v;
}

inline F3 bilinearMirrored(const std::vector<Rgb>& src, int w, int h, float fx, float fy)
{
  return bilinear(src, w, h, mirrored(fx, w), mirrored(fy, h));
}

class FlowMap
{
 public:
  FlowMap(int w, int h, Boundary boundary)
      : itsW(w),
        itsH(h),
        itsWrap(boundary == Boundary::Wrap),
        itsOpen(boundary == Boundary::Open),
        itsU(static_cast<std::size_t>(w) * h),
        itsV(itsU.size()),
        itsTu(itsU.size()),
        itsTv(itsU.size())
  {
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
      {
        itsU[static_cast<std::size_t>(y) * w + x] = static_cast<float>(x);
        itsV[static_cast<std::size_t>(y) * w + x] = static_cast<float>(y);
      }
  }

  // vel(x, y, vx, vy): screen pixels per frame at screen position (x, y).
  // The field is evaluated on a grid of every other pixel and interpolated,
  // which keeps even many-vortex flows cheap.
  template <typename Vel>
  void step(Vel&& vel)
  {
    const int gw = itsW / 2 + 2;
    const int gh = itsH / 2 + 2;
    itsGx.resize(static_cast<std::size_t>(gw) * gh);
    itsGy.resize(itsGx.size());
    for (int j = 0; j < gh; ++j)
      for (int i = 0; i < gw; ++i)
      {
        const std::size_t k = static_cast<std::size_t>(j) * gw + i;
        vel(2.0F * i, 2.0F * j, itsGx[k], itsGy[k]);
      }
    const auto field = [&](float x, float y, float& vx, float& vy)
    {
      const float gx = std::clamp(x * 0.5F, 0.0F, gw - 1.001F);
      const float gy = std::clamp(y * 0.5F, 0.0F, gh - 1.001F);
      const int i0 = static_cast<int>(gx);
      const int j0 = static_cast<int>(gy);
      const float ax = gx - i0;
      const float ay = gy - j0;
      const std::size_t k = static_cast<std::size_t>(j0) * gw + i0;
      const auto lerp2 = [&](const std::vector<float>& g)
      {
        const float top = g[k] + (g[k + 1] - g[k]) * ax;
        const float bot = g[k + gw] + (g[k + gw + 1] - g[k + gw]) * ax;
        return top + (bot - top) * ay;
      };
      vx = lerp2(itsGx);
      vy = lerp2(itsGy);
    };
    for (int y = 0; y < itsH; ++y)
      for (int x = 0; x < itsW; ++x)
      {
        float vx = 0.0F;
        float vy = 0.0F;
        field(static_cast<float>(x), static_cast<float>(y), vx, vy);
        field(x - 0.5F * vx, y - 0.5F * vy, vx, vy);
        const std::size_t i = static_cast<std::size_t>(y) * itsW + x;
        sampleAt(x - vx, y - vy, itsTu[i], itsTv[i]);
      }
    itsU.swap(itsTu);
    itsV.swap(itsTv);
  }

  // Source coordinates of pixel i, with u folded back into [0, w).
  float u(std::size_t i) const
  {
    if (!itsWrap)
      return itsU[i];
    const float fw = static_cast<float>(itsW);
    return itsU[i] - std::floor(itsU[i] / fw) * fw;
  }
  float v(std::size_t i) const { return itsV[i]; }

  // Source coordinates at an arbitrary screen position (u unwrapped).
  void sampleAt(float fx, float fy, float& ou, float& ov) const
  {
    if (itsOpen && (fx < 0.0F || fx > itsW - 1 || fy < 0.0F || fy > itsH - 1))
    {
      ou = fx;  // fresh fluid from outside the screen
      ov = fy;
      return;
    }
    fy = std::clamp(fy, 0.0F, static_cast<float>(itsH - 1));
    float shift = 0.0F;
    if (itsWrap)
    {
      const float fw = static_cast<float>(itsW);
      const float k = std::floor(fx / fw);
      fx -= k * fw;
      shift = k * fw;
    }
    else
      fx = std::clamp(fx, 0.0F, static_cast<float>(itsW - 1));
    int x0 = static_cast<int>(fx);
    x0 = std::min(x0, itsW - 1);
    int x1 = x0 + 1;
    float x1shift = 0.0F;
    if (x1 >= itsW)
    {
      if (itsWrap)
      {
        x1 = 0;
        x1shift = static_cast<float>(itsW);
      }
      else
        x1 = itsW - 1;
    }
    const int y0 = static_cast<int>(fy);
    const int y1 = std::min(y0 + 1, itsH - 1);
    const float ax = fx - x0;
    const float ay = fy - y0;
    const auto at = [&](const std::vector<float>& f, int x, int y)
    { return f[static_cast<std::size_t>(y) * itsW + x]; };
    const float u00 = at(itsU, x0, y0);
    const float u10 = at(itsU, x1, y0) + x1shift;
    const float u01 = at(itsU, x0, y1);
    const float u11 = at(itsU, x1, y1) + x1shift;
    const float ut = u00 + (u10 - u00) * ax;
    const float ub = u01 + (u11 - u01) * ax;
    ou = ut + (ub - ut) * ay + shift;
    const float vt = at(itsV, x0, y0) + (at(itsV, x1, y0) - at(itsV, x0, y0)) * ax;
    const float vb = at(itsV, x0, y1) + (at(itsV, x1, y1) - at(itsV, x0, y1)) * ax;
    ov = vt + (vb - vt) * ay;
  }

 private:
  int itsW;
  int itsH;
  bool itsWrap;
  bool itsOpen;
  std::vector<float> itsU;
  std::vector<float> itsV;
  std::vector<float> itsTu;
  std::vector<float> itsTv;
  std::vector<float> itsGx;
  std::vector<float> itsGy;
};

// Radar reflectivity colours, weak to strong.
inline F3 dbzColour(float s)
{
  static const std::array<F3, 6> kStops = {F3{90, 170, 255},
                                           F3{30, 200, 70},
                                           F3{20, 140, 30},
                                           F3{250, 230, 40},
                                           F3{240, 60, 40},
                                           F3{230, 70, 230}};
  s = std::clamp(s, 0.0F, 1.0F) * (kStops.size() - 1);
  const int i = std::min(static_cast<int>(s), static_cast<int>(kStops.size()) - 2);
  return mix(kStops[i], kStops[i + 1], s - i);
}

}  // namespace kit
}  // namespace ee_detail
}  // namespace Qdless
