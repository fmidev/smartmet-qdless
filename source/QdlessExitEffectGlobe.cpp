// Globe exit effects: phenomena drawn on a lit orthographic Earth with real
// coastlines. The land mask is rasterised once from the crude GSHHS file
// (the same data qdless draws coastlines from) and coloured by a coarse
// biome model; each effect then paints its phenomenon in latitude/longitude.
// Every globe effect opens the same way: the current view wraps onto the
// sphere and turns into the Earth.

#include "QdlessCoastline.h"
#include "QdlessExitEffectKit.h"
#include "QdlessLandSea.h"

namespace Qdless
{
namespace ee_detail
{
using namespace kit;

namespace
{
constexpr float kDeg = kPi / 180.0F;

struct V3
{
  float x = 0.0F;
  float y = 0.0F;
  float z = 0.0F;
};

float dot(const V3& a, const V3& b)
{
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

V3 unit(float latDeg, float lonDeg)
{
  const float la = latDeg * kDeg;
  const float lo = lonDeg * kDeg;
  return {std::cos(la) * std::cos(lo), std::cos(la) * std::sin(lo), std::sin(la)};
}

// Signed longitude difference a - b folded into [-180, 180).
float dlon(float a, float b)
{
  float d = std::fmod(a - b + 540.0F, 360.0F);
  if (d < 0.0F)
    d += 360.0F;
  return d - 180.0F;
}

// 3D value noise on the unit sphere, so clouds have no dateline seam and
// no pinching at the poles.
float vnoise3(float x, float y, float z, int s)
{
  const float xf = std::floor(x);
  const float yf = std::floor(y);
  const float zf = std::floor(z);
  const int xi = static_cast<int>(xf);
  const int yi = static_cast<int>(yf);
  const int zi = static_cast<int>(zf);
  auto sm = [](float a) { return a * a * (3.0F - 2.0F * a); };
  const float ax = sm(x - xf);
  const float ay = sm(y - yf);
  const float az = sm(z - zf);
  auto h = [&](int i, int j, int k) { return hash01(xi + i, yi + j, (zi + k) * 7919 + s); };
  const float x00 = h(0, 0, 0) + (h(1, 0, 0) - h(0, 0, 0)) * ax;
  const float x10 = h(0, 1, 0) + (h(1, 1, 0) - h(0, 1, 0)) * ax;
  const float x01 = h(0, 0, 1) + (h(1, 0, 1) - h(0, 0, 1)) * ax;
  const float x11 = h(0, 1, 1) + (h(1, 1, 1) - h(0, 1, 1)) * ax;
  const float y0 = x00 + (x10 - x00) * ay;
  const float y1 = x01 + (x11 - x01) * ay;
  return y0 + (y1 - y0) * az;
}

float fbm3(const V3& p, float scale, int octaves, int s)
{
  float sum = 0.0F;
  float amp = 0.5F;
  float norm = 0.0F;
  float k = scale;
  for (int i = 0; i < octaves; ++i)
  {
    sum += amp * vnoise3(p.x * k + 11.0F, p.y * k + 3.0F, p.z * k + 7.0F, s + i * 31);
    norm += amp;
    amp *= 0.5F;
    k *= 2.03F;
  }
  return sum / norm;
}

// Colour ramp through evenly spaced stops, s in [0, 1].
template <std::size_t N>
F3 ramp(const std::array<F3, N>& stops, float s)
{
  s = std::clamp(s, 0.0F, 1.0F) * (N - 1);
  const int i = std::min(static_cast<int>(s), static_cast<int>(N) - 2);
  return mix(stops[i], stops[i + 1], s - i);
}

// ---------------------------------------------------------------------------
// The Earth: land mask and surface colours on a 0.5 degree grid

constexpr int kMapRows = 360;
constexpr int kMapCols = 720;

float softBox(float lat, float lon, float la0, float la1, float lo0, float lo1, float edge)
{
  return smooth01(la0 - edge, la0 + edge, lat) * smooth01(la1 + edge, la1 - edge, lat) *
         smooth01(lo0 - edge, lo0 + edge, lon) * smooth01(lo1 + edge, lo1 - edge, lon);
}

F3 biome(float lat, float lon, float n)
{
  const float a = std::fabs(lat);
  // Hot deserts and dry interiors, roughly.
  float desert = 0.0F;
  desert = std::max(desert, softBox(lat, lon, 15, 32, -17, 35, 9));     // Sahara
  desert = std::max(desert, softBox(lat, lon, 14, 32, 35, 60, 9));      // Arabia
  desert = std::max(desert, softBox(lat, lon, 25, 38, 55, 72, 9));      // Iran, Thar
  desert = std::max(desert, softBox(lat, lon, 36, 47, 75, 112, 9));     // Taklamakan, Gobi
  desert = std::max(desert, softBox(lat, lon, -31, -18, 117, 145, 9));  // Australia
  desert = std::max(desert, softBox(lat, lon, -29, -18, 14, 26, 9));    // Kalahari, Namib
  desert = std::max(desert, softBox(lat, lon, 28, 38, -118, -104, 9));  // US south-west
  desert = std::max(desert, softBox(lat, lon, -30, -15, -72, -68, 9));  // Atacama
  // Ragged edges: the boxes only say where deserts are, the noise shapes them.
  desert = smooth01(0.5F, 0.62F, desert + (n - 0.5F) * 1.2F);
  F3 c;
  if (a < 12.0F)
    c = F3{35, 95, 40};  // rain forest
  else if (a < 22.0F)
    c = mix(F3{35, 95, 40}, F3{140, 135, 75}, smooth01(12, 22, a));  // savanna
  else if (a < 45.0F)
    c = mix(F3{120, 130, 70}, F3{75, 115, 55}, smooth01(25, 45, a));  // temperate
  else if (a < 62.0F)
    c = mix(F3{75, 115, 55}, F3{40, 75, 45}, smooth01(45, 60, a));  // boreal forest
  else
    c = mix(F3{40, 75, 45}, F3{125, 120, 100}, smooth01(62, 72, a));  // tundra
  c = mix(c, F3{215, 185, 130}, desert);
  // Ice sheets: Antarctica, Greenland, and the high Arctic islands.
  const float greenland = softBox(lat, lon, 60, 84, -73, -12, 2);
  const float ice = std::max({smooth01(-62, -66, lat), greenland, smooth01(76, 80, lat)});
  c = mix(c, F3{232, 238, 245}, ice);
  return mul(c, 0.85F + 0.3F * n);
}

constexpr int kNoiseFields = 3;
constexpr int kNoiseRows = 180;  // 1 degree: finer than the finest noise feature
constexpr int kNoiseCols = 360;

struct EarthMap
{
  bool haveLand = false;
  std::vector<unsigned char> land;  // 1 = land
  std::vector<F3> colour;
  // Seamless fbm on the sphere at three scales (coarse, medium, fine), so
  // effects get cloud and plume texture with a bilinear lookup per pixel.
  std::array<std::vector<float>, kNoiseFields> noise;
};

// Bilinear lookup of noise field k (0 coarse, 1 medium, 2 fine), in [0, 1].
float noiseAt(const EarthMap& m, int k, float lat, float lon)
{
  const float fr =
      std::clamp((90.0F - lat) * kNoiseRows / 180.0F - 0.5F, 0.0F, kNoiseRows - 1.001F);
  float fc = (lon + 180.0F) * kNoiseCols / 360.0F - 0.5F;
  fc -= std::floor(fc / kNoiseCols) * kNoiseCols;
  const int r0 = static_cast<int>(fr);
  const int c0 = std::min(static_cast<int>(fc), kNoiseCols - 1);
  const int c1 = (c0 + 1) % kNoiseCols;
  const float ar = fr - r0;
  const float ac = fc - c0;
  const auto& f = m.noise[k];
  const auto at = [&](int r, int c) { return f[static_cast<std::size_t>(r) * kNoiseCols + c]; };
  const float top = at(r0, c0) + (at(r0, c1) - at(r0, c0)) * ac;
  const float bot = at(r0 + 1, c0) + (at(r0 + 1, c1) - at(r0 + 1, c0)) * ac;
  return top + (bot - top) * ar;
}

// Run body(r) for r in [0, rows) on a few threads.
template <typename Body>
void parallelRows(int rows, Body&& body)
{
  const int nt = std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, 8);
  std::vector<std::thread> pool;
  for (int k = 0; k < nt; ++k)
    pool.emplace_back(
        [&, k]
        {
          for (int r = k; r < rows; r += nt)
            body(r);
        });
  for (auto& th : pool)
    th.join();
}

// Built on first use and kept for the life of the process.
const EarthMap& earthMap()
{
  static EarthMap map = []
  {
    EarthMap m;
    m.land.assign(static_cast<std::size_t>(kMapRows) * kMapCols, 0);
    m.colour.resize(m.land.size());
    for (int k = 0; k < kNoiseFields; ++k)
      m.noise[k].resize(static_cast<std::size_t>(kNoiseRows) * kNoiseCols);
    // The noise fields are independent of the land: build them while the
    // GSHHS file is read and rasterised.
    std::thread noise(
        [&]
        {
          static const std::array<float, kNoiseFields> kScale = {3.0F, 7.0F, 16.0F};
          parallelRows(kNoiseRows,
                       [&](int r)
                       {
                         for (int c = 0; c < kNoiseCols; ++c)
                         {
                           const float lat = 90.0F - (r + 0.5F) * 180.0F / kNoiseRows;
                           const float lon = -180.0F + (c + 0.5F) * 360.0F / kNoiseCols;
                           const V3 p = unit(lat, lon);
                           for (int k = 0; k < kNoiseFields; ++k)
                             m.noise[k][static_cast<std::size_t>(r) * kNoiseCols + c] =
                                 fbm3(p, kScale[k], 4, 211 + 17 * k);
                         }
                       });
        });
    LandSea ls;
    const auto path = Coastline::pickFile(g_coastlineDir, "GSHHS", 1.0F);
    m.haveLand = !path.empty() && ls.build(path, kMapRows);
    for (int r = 0; r < kMapRows; ++r)
      for (int c = 0; c < kMapCols; ++c)
      {
        const float lat = 90.0F - (r + 0.5F) * 180.0F / kMapRows;
        const float lon = -180.0F + (c + 0.5F) * 360.0F / kMapCols;
        m.land[static_cast<std::size_t>(r) * kMapCols + c] =
            m.haveLand && ls.isLand(lat, lon) ? 1 : 0;
      }
    noise.join();
    parallelRows(kMapRows,
                 [&](int r)
                 {
                   for (int c = 0; c < kMapCols; ++c)
                   {
                     const std::size_t i = static_cast<std::size_t>(r) * kMapCols + c;
                     const float lat = 90.0F - (r + 0.5F) * 180.0F / kMapRows;
                     const float lon = -180.0F + (c + 0.5F) * 360.0F / kMapCols;
                     const float n = noiseAt(m, 1, lat, lon);
                     if (m.land[i] != 0)
                       m.colour[i] = biome(lat, lon, n);
                     else
                     {
                       // Shelf seas are lighter: count land within two cells.
                       int near = 0;
                       for (int dr = -2; dr <= 2; ++dr)
                         for (int dc = -2; dc <= 2; ++dc)
                         {
                           const int rr = std::clamp(r + dr, 0, kMapRows - 1);
                           const int cc = (c + dc + kMapCols) % kMapCols;
                           near += m.land[static_cast<std::size_t>(rr) * kMapCols + cc];
                         }
                       F3 sea = mix(F3{10, 34, 82}, F3{28, 90, 128}, std::min(1.0F, near / 8.0F));
                       sea = mix(sea, F3{30, 45, 70}, smooth01(55, 80, std::fabs(lat)) * 0.5F);
                       m.colour[i] = mul(sea, 0.92F + 0.16F * n);
                     }
                   }
                 });
    return m;
  }();
  return map;
}

std::size_t mapIndex(float lat, float lon)
{
  const int r = std::clamp(static_cast<int>((90.0F - lat) * kMapRows / 180.0F), 0, kMapRows - 1);
  int c = static_cast<int>((lon + 180.0F) * kMapCols / 360.0F) % kMapCols;
  if (c < 0)
    c += kMapCols;
  return static_cast<std::size_t>(r) * kMapCols + c;
}

// ---------------------------------------------------------------------------
// Orthographic view

struct Globe
{
  float cx = 0.0F;
  float cy = 0.0F;
  float R = 1.0F;  // isotropic pixels
  float ya = 1.0F;
  V3 f;  // towards the viewer
  V3 e;  // screen right
  V3 n;  // screen up

  Globe(int w, int h, float yaspect, float radiusFrac)
  {
    ya = yaspect;
    cx = (w - 1) * 0.5F;
    cy = (h - 1) * 0.5F;
    R = std::min(w * 0.5F, h * 0.5F * ya) * radiusFrac;
    look(0.0F, 0.0F);
  }

  void look(float latDeg, float lonDeg)
  {
    const float la = latDeg * kDeg;
    const float lo = lonDeg * kDeg;
    f = unit(latDeg, lonDeg);
    e = {-std::sin(lo), std::cos(lo), 0.0F};
    n = {-std::sin(la) * std::cos(lo), -std::sin(la) * std::sin(lo), std::cos(la)};
  }

  // Screen position -> point on the sphere; nz is the depth towards the viewer.
  bool locate(float x, float y, V3& p, float& nz) const
  {
    const float u = (x - cx) / R;
    const float v = -(y - cy) * ya / R;
    const float r2 = u * u + v * v;
    if (r2 > 1.0F)
      return false;
    nz = std::sqrt(1.0F - r2);
    p = {u * e.x + v * n.x + nz * f.x, u * e.y + v * n.y + nz * f.y, u * e.z + v * n.z + nz * f.z};
    return true;
  }

  bool project(float latDeg, float lonDeg, float& x, float& y) const
  {
    const V3 p = unit(latDeg, lonDeg);
    if (dot(p, f) < 0.0F)
      return false;
    x = cx + dot(p, e) * R;
    y = cy - dot(p, n) * R / ya;
    return true;
  }
};

void latLonOf(const V3& p, float& lat, float& lon)
{
  lat = std::asin(std::clamp(p.z, -1.0F, 1.0F)) / kDeg;
  lon = std::atan2(p.y, p.x) / kDeg;
}

// What an effect can change at a surface point before lighting.
struct Surface
{
  F3 albedo;
  float cloud = 0.0F;  // 0..1 cover
  bool land = false;
};

struct GlobeLook
{
  float sunLat = 20.0F;  // subsolar point
  float sunLon = 0.0F;
  float clouds = 0.25F;     // background cloud cover, 0 for none
  float cloudDrift = 0.0F;  // advances the background clouds
  float intro = 1.0F;       // 0 = the view, 1 = the Earth
  float fade = 1.0F;
  float night = 0.07F;  // brightness of the unlit side
};

F3 starfield(int x, int y)
{
  const float s = hash01(x, y, 991);
  if (s > 0.994F)
    return F3{150 + 100 * (s - 0.994F) / 0.006F, 150 + 100 * (s - 0.994F) / 0.006F, 175};
  return F3{2, 3, 10};
}

// Render the globe: space and stars, the Earth lit by the sun, clouds, an
// atmospheric rim and an ocean glint. `paint(lat, lon, p, s)` adjusts the
// surface; `glow(lat, lon, x, y, c)` adds light after shading (fires,
// lightning, aurora), and is also called off the disc with lat = NaN.
template <typename Paint, typename Glow>
void renderGlobe(std::vector<Rgb>& dst,
                 int w,
                 int h,
                 const std::vector<Rgb>& src,
                 const Globe& g,
                 const GlobeLook& look,
                 Paint&& paint,
                 Glow&& glow)
{
  const EarthMap& map = earthMap();
  const V3 sun = unit(look.sunLat, look.sunLon);
  // Glint: half-way between the sun and the viewer.
  V3 half{sun.x + g.f.x, sun.y + g.f.y, sun.z + g.f.z};
  const float hl = std::sqrt(dot(half, half));
  half = {half.x / hl, half.y / hl, half.z / hl};
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
    {
      const std::size_t i = static_cast<std::size_t>(y) * w + x;
      V3 p;
      float nz = 0.0F;
      F3 c;
      if (!g.locate(static_cast<float>(x), static_cast<float>(y), p, nz))
      {
        c = starfield(x, y);
        const float r = std::hypot(x - g.cx, (y - g.cy) * g.ya) / g.R;
        if (r < 1.2F)
          c = add(c, mul(F3{70, 120, 230}, 0.6F * std::exp(-(r - 1.0F) * 30.0F)));
        glow(std::numeric_limits<float>::quiet_NaN(), 0.0F, x, y, c);
        if (look.intro < 1.0F)
          c = mix(texel(src, w, x, y), c, look.intro);
      }
      else
      {
        float lat = 0.0F;
        float lon = 0.0F;
        latLonOf(p, lat, lon);
        const std::size_t mi = mapIndex(lat, lon);
        Surface s{map.colour[mi], 0.0F, map.land[mi] != 0};
        if (look.clouds > 0.0F)
        {
          // Background weather: drifting with the westerlies / trades.
          float f1 = 0.6F * noiseAt(map, 0, lat, lon - look.cloudDrift) +
                     0.4F * noiseAt(map, 2, lat, lon - look.cloudDrift * 1.3F);
          f1 = 0.5F + (f1 - 0.5F) * 2.6F;  // averaged fbm is narrow: stretch to ~[0, 1]
          const float thr = 0.9F - 0.6F * look.clouds;  // look.clouds ~ fraction covered
          s.cloud = smooth01(thr - 0.1F, thr + 0.2F, f1) * 0.8F;
        }
        paint(lat, lon, p, s);
        const float d = dot(p, sun);
        const float diffuse = std::max(0.0F, d);
        const float day = smooth01(-0.1F, 0.12F, d);
        c = mul(s.albedo, look.night + (0.95F - look.night) * diffuse);
        const float cl = std::clamp(s.cloud, 0.0F, 1.0F);
        c = mix(c, mul(F3{238, 240, 245}, look.night * 0.8F + 0.92F * diffuse), cl);
        if (!s.land && cl < 0.5F)
        {
          const float spec = std::pow(std::max(0.0F, dot(p, half)), 400.0F);
          c = add(c, mul(F3{255, 240, 210}, spec * 0.15F * (1.0F - 2.0F * cl) * day));
        }
        const float rim = std::pow(1.0F - nz, 3.0F);
        c = mix(c, mul(F3{110, 160, 255}, 0.25F + 0.75F * day), rim * 0.55F);
        glow(lat, lon, x, y, c);
        if (look.intro < 1.0F)
        {
          // The view, pasted onto the sphere and shaded like it.
          const float u = (x - g.cx) / g.R;
          const float v = (y - g.cy) * g.ya / g.R;
          const F3 view =
              mul(bilinear(src, w, h, (u + 1.0F) * 0.5F * (w - 1), (v + 1.0F) * 0.5F * (h - 1)),
                  0.35F + 0.65F * nz);
          const float k = smooth01(0.0F, 0.5F, look.intro);
          c = mix(mix(texel(src, w, x, y), view, k), c, smooth01(0.4F, 1.0F, look.intro));
        }
      }
      dst[i] = toRgb(mul(c, look.fade));
    }
}

void blendPx(std::vector<Rgb>& dst, int w, int h, float fx, float fy, const F3& c, float a)
{
  const int x = static_cast<int>(fx);
  const int y = static_cast<int>(fy);
  if (x < 0 || x >= w || y < 0 || y >= h || a <= 0.0F)
    return;
  Rgb& d = dst[static_cast<std::size_t>(y) * w + x];
  const F3 o{static_cast<float>(d.r), static_cast<float>(d.g), static_cast<float>(d.b)};
  d = toRgb(mix(o, c, std::min(1.0F, a)));
}

// A soft dot at (lat, lon), `alt` globe radii above the surface, hidden
// behind the planet. sizePx is the radius in isotropic pixels.
void splat(std::vector<Rgb>& dst,
           int w,
           int h,
           const Globe& g,
           float lat,
           float lon,
           float alt,
           float sizePx,
           const F3& col,
           float a)
{
  const V3 p = unit(lat, lon);
  const float k = g.R * (1.0F + alt);
  const float X = dot(p, g.e) * k;
  const float Y = dot(p, g.n) * k;
  if (dot(p, g.f) < 0.0F && X * X + Y * Y < g.R * g.R)
    return;
  const float px = g.cx + X;
  const float py = g.cy - Y / g.ya;
  const int r = static_cast<int>(std::ceil(sizePx));
  const int ry = static_cast<int>(std::ceil(sizePx / g.ya));
  for (int y = static_cast<int>(py) - ry; y <= static_cast<int>(py) + ry; ++y)
    for (int x = static_cast<int>(px) - r; x <= static_cast<int>(px) + r; ++x)
    {
      if (x < 0 || x >= w || y < 0 || y >= h)
        continue;
      const float d = std::hypot(x + 0.5F - px, (y + 0.5F - py) * g.ya) / std::max(sizePx, 0.5F);
      if (d < 1.0F)
        blendPx(dst, w, h, static_cast<float>(x), static_cast<float>(y), col, a * (1.0F - d * d));
    }
}

struct LatLon
{
  float lat;
  float lon;
};

// Position a fraction u (0..1) of the way along a lat/lon polyline,
// measured by segment count (segments are of similar length).
LatLon alongPath(const std::vector<LatLon>& pts, float u)
{
  const float f = std::clamp(u, 0.0F, 1.0F) * (pts.size() - 1);
  const std::size_t i = std::min(static_cast<std::size_t>(f), pts.size() - 2);
  const float a = f - i;
  return {pts[i].lat + (pts[i + 1].lat - pts[i].lat) * a,
          pts[i].lon + dlon(pts[i + 1].lon, pts[i].lon) * a};
}

// Local offsets in degrees of arc from (lat0, lon0): x east, y north.
void localDeg(float lat, float lon, float lat0, float lon0, float& x, float& y)
{
  x = dlon(lon, lon0) * std::cos(lat0 * kDeg);
  y = lat - lat0;
}

// Opening and closing: the view wraps onto the Earth, and the end fades out.
void phase(float t, GlobeLook& look, float introEnd = 0.16F)
{
  look.intro = smooth01(0.0F, introEnd, t);
  look.fade = 1.0F - smooth01(0.9F, 1.0F, t);
}

}  // namespace

// El Niño. The Pacific seen from space, coloured by sea-surface temperature.
// A Kelvin wave runs east along the equator, the cold tongue off Peru
// warms, and the warm pool's thunderstorms follow the warm water east.
void effectElNino(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  Globe g(w, h, ya, 0.92F);
  runFrames(
      renderer,
      w,
      h,
      8000,
      [&](float t, std::vector<Rgb>& dst)
      {
        GlobeLook look;
        phase(t, look);
        g.look(2.0F, -150.0F - 25.0F * (1.0F - look.intro));  // turns to face the Pacific
        look.sunLat = 18.0F;
        look.sunLon = -175.0F;
        look.clouds = 0.12F;
        look.cloudDrift = t * 25.0F;
        const float kelvin = -200.0F + 120.0F * smooth01(0.15F, 0.55F, t);  // 160E -> 80W
        const float nino = smooth01(0.4F, 0.75F, t);
        const float convLon = 150.0F + 70.0F * nino;  // 150E -> 140W
        renderGlobe(
            dst,
            w,
            h,
            src,
            g,
            look,
            [&](float lat, float lon, const V3& p, Surface& s)
            {
              if (s.land || std::fabs(lat) > 30.0F)
                return;
              // Sea-surface temperature anomaly, as on an ENSO monitoring map.
              const float pac = lon > 120.0F || lon < -75.0F ? 1.0F : 0.0F;
              const float eq = std::exp(-lat * lat / 50.0F);
              const float east = std::exp(-std::pow(dlon(lon, -110.0F) / 35.0F, 2.0F));
              float anom = pac * eq *
                           (-2.0F * east * (1.0F - nino) +
                            3.0F * nino * std::exp(-std::pow(dlon(lon, -120.0F) / 40.0F, 2.0F)));
              // The downwelling Kelvin wave: a warm bump running east.
              if (t > 0.15F && t < 0.7F)
                anom += 2.5F * pac *
                        std::exp(-std::pow(dlon(lon, kelvin) / 12.0F, 2.0F) - lat * lat / 16.0F) *
                        (1.0F - smooth01(0.55F, 0.7F, t));
              anom += (noiseAt(earthMap(), 1, lat, lon) - 0.5F) * 0.8F;
              const F3 hue =
                  anom > 0.0F
                      ? mix(F3{240, 200, 60}, F3{210, 30, 25}, smooth01(0.5F, 3.0F, anom))
                      : mix(F3{90, 200, 230}, F3{30, 60, 200}, smooth01(-0.5F, -2.5F, anom));
              s.albedo = mix(s.albedo, hue, smooth01(0.3F, 1.5F, std::fabs(anom)) * 0.9F);
              // Deep convection over the warmest water.
              const float conv =
                  std::exp(-std::pow(dlon(lon, convLon) / 28.0F, 2.0F) - lat * lat / 120.0F);
              s.cloud =
                  std::max(s.cloud,
                           smooth01(0.45F, 0.7F, noiseAt(earthMap(), 2, lat, lon - t * 20.0F)) *
                               conv * 1.3F);
            },
            [](float, float, int, int, F3&) {});
      });
}

// Monsoon. India from space before the summer monsoon: parched land and
// dusty skies. The onset front sweeps north from Kerala, the south-west
// monsoon flow streams cloud in off the Arabian Sea and the Bay of Bengal,
// and the land turns green behind it.
void effectMonsoon(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  Globe g(w, h, ya, 0.92F);
  const EarthMap& map = earthMap();
  runFrames(
      renderer,
      w,
      h,
      8000,
      [&](float t, std::vector<Rgb>& dst)
      {
        GlobeLook look;
        phase(t, look);
        g.look(18.0F, 82.0F);
        look.sunLat = 22.0F;
        look.sunLon = 75.0F;
        look.clouds = 0.15F;
        look.cloudDrift = -t * 20.0F;
        // Onset front: 8N at the start of the rains, 32N when it covers India.
        const float front = 6.0F + 28.0F * smooth01(0.18F, 0.8F, t);
        renderGlobe(
            dst,
            w,
            h,
            src,
            g,
            look,
            [&](float lat, float lon, const V3&, Surface& s)
            {
              if (lon < 40.0F || lon > 125.0F || lat < -15.0F || lat > 45.0F)
                return;
              // The Bay of Bengal branch runs ahead of the Arabian Sea branch.
              const float f = front + 0.12F * (lon - 75.0F);
              const float wet = smooth01(f + 2.0F, f - 2.0F, lat);
              const float region = smooth01(40.0F, 55.0F, lon) * smooth01(120.0F, 105.0F, lon) *
                                   smooth01(-12.0F, 0.0F, lat);
              if (s.land)
              {
                const float dry = region * (1.0F - wet) * smooth01(36.0F, 30.0F, lat);
                s.albedo = mix(s.albedo, F3{185, 150, 95}, 0.7F * dry);
                s.albedo = mix(
                    s.albedo, F3{30, 120, 45}, 0.75F * wet * region * smooth01(34.0F, 28.0F, lat));
              }
              // Cloud carried north-east by the monsoon flow.
              const float n = noiseAt(map, 1, lat - t * 25.0F, lon - t * 25.0F);
              const float n2 = noiseAt(map, 2, lat - t * 30.0F, lon - t * 35.0F);
              const float conv = smooth01(0.42F, 0.62F, 0.6F * n + 0.4F * n2) * wet * region;
              s.cloud = std::max(s.cloud, conv);
              // Pre-monsoon dust over Arabia and the Thar.
              const float dust = (1.0F - wet) * softBox(lat, lon, 15, 32, 50, 75, 5) * 0.4F;
              s.albedo = mix(s.albedo, F3{200, 165, 115}, dust);
            },
            [](float, float, int, int, F3&) {});
      });
}

// The ITCZ through a year. The Earth turns beneath the sun as it moves
// between the tropics; the band of tropical thunderstorms follows it, far
// over the continents and only a little over the oceans.
void effectItcz(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  Globe g(w, h, ya, 0.92F);
  const EarthMap& map = earthMap();
  runFrames(
      renderer,
      w,
      h,
      9000,
      [&](float t, std::vector<Rgb>& dst)
      {
        GlobeLook look;
        phase(t, look);
        const float year = smooth01(0.1F, 0.95F, t);
        const float sunLat = 23.4F * std::sin(2.0F * kPi * year);
        // Lags the sun by about a month.
        const float lagged = 23.4F * std::sin(2.0F * kPi * (year - 0.08F));
        g.look(8.0F, -40.0F + 300.0F * t);
        look.sunLat = sunLat;
        look.sunLon = -40.0F + 300.0F * t;
        look.clouds = 0.2F;
        look.cloudDrift = t * 60.0F;
        renderGlobe(
            dst,
            w,
            h,
            src,
            g,
            look,
            [&](float lat, float lon, const V3&, Surface& s)
            {
              if (std::fabs(lat) > 35.0F)
                return;
              // Continental longitudes swing the band further.
              float landFrac = 0.0F;
              landFrac = std::max(landFrac, softBox(0.0F, lon, -1, 1, -15, 40, 12));   // Africa
              landFrac = std::max(landFrac, softBox(0.0F, lon, -1, 1, -75, -40, 10));  // S America
              landFrac =
                  std::max(landFrac, softBox(0.0F, lon, -1, 1, 70, 125, 12));  // monsoon Asia
              const float axis = 5.0F + lagged * (0.25F + 0.55F * landFrac);
              const float band = std::exp(-std::pow((lat - axis) / 6.0F, 2.0F));
              const float n = noiseAt(map, 2, lat, lon + t * 90.0F);
              const float m = noiseAt(map, 1, lat, lon + t * 50.0F);
              s.cloud = std::max(s.cloud, smooth01(0.38F, 0.6F, 0.5F * n + 0.5F * m) * band * 1.2F);
              // Subtropical highs on both sides are clear.
              s.cloud *=
                  1.0F - 0.8F * std::exp(-std::pow((std::fabs(lat - axis) - 18.0F) / 6.0F, 2.0F));
            },
            [](float, float, int, int, F3&) {});
      });
}

// The Madden-Julian Oscillation. An envelope of enhanced thunderstorms is
// born over the Indian Ocean and creeps east across the Maritime Continent
// into the Pacific, with clear, suppressed skies before and after it. The
// tint is the outgoing-longwave anomaly: green where it rains.
void effectMjo(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  Globe g(w, h, ya, 0.92F);
  const EarthMap& map = earthMap();
  runFrames(renderer,
            w,
            h,
            9000,
            [&](float t, std::vector<Rgb>& dst)
            {
              GlobeLook look;
              phase(t, look);
              const float env = 60.0F + 130.0F * smooth01(0.12F, 0.92F, t);  // 60E -> 170W
              g.look(0.0F, 85.0F + 0.6F * (env - 60.0F));
              look.sunLat = 5.0F;
              look.sunLon = std::atan2(g.f.y, g.f.x) / kDeg;
              look.clouds = 0.15F;
              look.cloudDrift = t * 15.0F;
              const float strength = smooth01(0.1F, 0.3F, t);
              renderGlobe(
                  dst,
                  w,
                  h,
                  src,
                  g,
                  look,
                  [&](float lat, float lon, const V3&, Surface& s)
                  {
                    if (std::fabs(lat) > 40.0F)
                      return;
                    const float d = dlon(lon, env);
                    const float y = std::exp(-lat * lat / 200.0F);
                    // Wavenumber-1 pattern: wet core, dry flanks 60-90 degrees away.
                    const float wetness =
                        strength * y *
                        (std::exp(-d * d / 500.0F) -
                         0.6F * std::exp(-std::pow((std::fabs(d) - 75.0F) / 30.0F, 2.0F)));
                    const F3 tint = wetness > 0.0F ? F3{40, 200, 120} : F3{210, 130, 50};
                    s.albedo = mix(s.albedo, tint, std::min(0.4F, std::fabs(wetness) * 0.6F));
                    const float n = noiseAt(map, 2, lat, lon - t * 120.0F);
                    const float m = noiseAt(map, 1, lat, lon - t * 60.0F);
                    if (wetness > 0.0F)
                      s.cloud = std::max(s.cloud,
                                         smooth01(0.45F, 0.62F, 0.6F * n + 0.4F * m) *
                                             std::min(1.0F, wetness * 1.6F));
                    else
                      s.cloud *= 1.0F + wetness * 1.5F;
                  },
                  [](float, float, int, int, F3&) {});
            });
}

// The Walker circulation. Air rises in towering thunderstorms over the warm
// Maritime Continent, flows east high above the Pacific, sinks over the cold
// waters off Peru and returns west with the trade winds — drawn as tracers
// in a loop above the equator, outside the planet's rim where it is seen
// edge on.
void effectWalkerCell(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  Globe g(w, h, ya, 0.8F);
  const EarthMap& map = earthMap();
  // The loop: surface from Peru west to Indonesia, up, aloft back east, down.
  constexpr float kWest = 120.0F;
  constexpr float kEast = -82.0F;
  const float span = dlon(kWest, kEast) + 360.0F;  // 202 degrees going west
  const float top = 0.16F;
  const auto loopAt = [&](float u, float& lat, float& lon, float& alt)
  {
    u = fract(u);
    lat = 0.0F;
    if (u < 0.4F)  // trades: east to west near the surface
    {
      lon = kEast - span * (u / 0.4F);
      alt = 0.012F;
    }
    else if (u < 0.5F)  // rising over the warm pool
    {
      lon = kWest;
      alt = 0.012F + (top - 0.012F) * (u - 0.4F) / 0.1F;
    }
    else if (u < 0.9F)  // aloft back east
    {
      lon = kWest + span * ((u - 0.5F) / 0.4F);
      alt = top;
    }
    else  // sinking off South America
    {
      lon = kEast;
      alt = top - (top - 0.012F) * (u - 0.9F) / 0.1F;
    }
  };
  runFrames(
      renderer,
      w,
      h,
      8500,
      [&](float t, std::vector<Rgb>& dst)
      {
        GlobeLook look;
        phase(t, look);
        g.look(18.0F, -165.0F + 15.0F * std::sin(t * 2.0F));
        look.sunLat = 15.0F;
        look.sunLon = -165.0F;
        look.clouds = 0.12F;
        look.cloudDrift = -t * 30.0F;
        renderGlobe(
            dst,
            w,
            h,
            src,
            g,
            look,
            [&](float lat, float lon, const V3&, Surface& s)
            {
              if (std::fabs(lat) > 30.0F)
                return;
              const float warm =
                  std::exp(-std::pow(dlon(lon, 135.0F) / 25.0F, 2.0F) - lat * lat / 150.0F);
              const float cold =
                  std::exp(-std::pow(dlon(lon, -95.0F) / 25.0F, 2.0F) - lat * lat / 30.0F);
              if (!s.land)
                s.albedo =
                    mix(mix(s.albedo, F3{40, 120, 150}, 0.6F * cold), F3{20, 60, 140}, 0.3F * warm);
              const float n = noiseAt(map, 2, lat, lon - t * 40.0F);
              s.cloud = std::max(s.cloud, smooth01(0.35F, 0.55F, n) * warm * 1.3F);
              s.cloud *= 1.0F - 0.9F * cold;
              // Trade-wind cumulus streets.
              s.cloud = std::max(
                  s.cloud,
                  0.35F * smooth01(0.62F, 0.7F, noiseAt(map, 2, lat * 3.0F, lon + t * 80.0F)) *
                      std::exp(-lat * lat / 120.0F) * (1.0F - warm));
            },
            [](float, float, int, int, F3&) {});
        if (look.intro < 0.9F)
          return;
        const float a = smooth01(0.9F, 1.0F, look.intro) * look.fade;
        constexpr int kTracers = 90;
        for (int k = 0; k < kTracers; ++k)
        {
          const float u0 = static_cast<float>(k) / kTracers + t * 0.9F;
          for (int tail = 0; tail < 4; ++tail)
          {
            float lat = 0.0F;
            float lon = 0.0F;
            float alt = 0.0F;
            loopAt(u0 - tail * 0.004F, lat, lon, alt);
            lat += (hash01(k, 1, 5) - 0.5F) * 10.0F;
            const bool aloft = alt > 0.05F;
            const F3 col = aloft ? F3{255, 235, 190} : F3{140, 230, 255};
            splat(dst, w, h, g, lat, lon, alt, 1.3F, col, a * (0.9F - 0.2F * tail));
          }
        }
      });
}

// AMOC. Warm water rides the Gulf Stream and the North Atlantic Drift to the
// Nordic Seas, cools, sinks and returns south at depth. Then the overturning
// stalls: the tracers slow, sea ice spreads south and Europe freezes.
void effectAmoc(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  Globe g(w, h, ya, 0.92F);
  const std::vector<LatLon> warm = {{18, -85},
                                    {24, -82},
                                    {26, -79},
                                    {32, -77},
                                    {37, -72},
                                    {41, -60},
                                    {45, -45},
                                    {50, -30},
                                    {55, -18},
                                    {60, -8},
                                    {64, 0},
                                    {68, 5},
                                    {72, 2}};
  const std::vector<LatLon> deep = {{72, -8},
                                    {65, -28},
                                    {58, -45},
                                    {50, -50},
                                    {40, -62},
                                    {30, -70},
                                    {15, -55},
                                    {0, -35},
                                    {-15, -30},
                                    {-30, -40}};
  float phaseWarm = 0.0F;
  float phaseDeep = 0.0F;
  runFrames(
      renderer,
      w,
      h,
      9000,
      [&](float t, std::vector<Rgb>& dst)
      {
        GlobeLook look;
        phase(t, look);
        g.look(42.0F, -38.0F);
        look.sunLat = 18.0F;
        look.sunLon = -30.0F;
        look.clouds = 0.22F;
        look.cloudDrift = -t * 40.0F;
        const float stall = smooth01(0.55F, 0.8F, t);
        phaseWarm += 0.008F * (1.0F - 0.9F * stall);
        phaseDeep += 0.004F * (1.0F - 0.9F * stall);
        renderGlobe(
            dst,
            w,
            h,
            src,
            g,
            look,
            [&](float lat, float lon, const V3&, Surface& s)
            {
              // Sea ice: Arctic and Labrador now, spreading south as the AMOC fails.
              const float edge =
                  74.0F - 12.0F * stall -
                  6.0F * smooth01(-70.0F, -40.0F, lon) * smooth01(-20.0F, -45.0F, lon);
              const float n = noiseAt(earthMap(), 2, lat, lon);
              if (!s.land)
                s.albedo = mix(s.albedo,
                               F3{225, 235, 245},
                               smooth01(edge - 1.0F, edge + 1.0F, lat + (n - 0.5F) * 6.0F));
              // Gulf Stream warmth while it runs.
              else if (lon > -15.0F && lon < 45.0F && lat > 40.0F && lat < 72.0F)  // European snow
                s.albedo = mix(s.albedo,
                               F3{235, 240, 248},
                               stall * smooth01(58.0F - 14.0F * stall,
                                                62.0F - 14.0F * stall,
                                                lat + (n - 0.5F) * 8.0F));
            },
            [](float, float, int, int, F3&) {});
        if (look.intro < 0.9F)
          return;
        const float a = smooth01(0.9F, 1.0F, look.intro) * look.fade;
        constexpr int kWarm = 70;
        for (int k = 0; k < kWarm; ++k)
        {
          const float u = fract(static_cast<float>(k) / kWarm + phaseWarm);
          const LatLon q = alongPath(warm, u);
          const float off = (hash01(k, 2, 9) - 0.5F) * 3.0F;
          const F3 col = mix(F3{255, 150, 40}, F3{90, 170, 255}, smooth01(0.75F, 1.0F, u));
          splat(dst, w, h, g, q.lat + off, q.lon + off, 0.0F, 1.6F, col, a * (1.0F - 0.6F * stall));
        }
        constexpr int kDeep = 45;
        for (int k = 0; k < kDeep; ++k)
        {
          const float u = fract(static_cast<float>(k) / kDeep + phaseDeep);
          const LatLon q = alongPath(deep, u);
          if (fract(u * 40.0F) > 0.6F)
            continue;  // dashed: it runs at depth
          splat(dst,
                w,
                h,
                g,
                q.lat,
                q.lon,
                0.0F,
                1.5F,
                F3{70, 140, 255},
                a * 0.85F * (1.0F - 0.6F * stall));
        }
      });
}

// Saharan dust. A dust outbreak lifts off the Sahara in the Saharan Air
// Layer and rides the easterlies across the Atlantic to the Caribbean,
// a tan veil over the blue ocean thinning as it goes.
void effectSaharanDust(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  Globe g(w, h, ya, 0.92F);
  const EarthMap& map = earthMap();
  runFrames(renderer,
            w,
            h,
            8500,
            [&](float t, std::vector<Rgb>& dst)
            {
              GlobeLook look;
              phase(t, look);
              g.look(18.0F, -28.0F);
              look.sunLat = 20.0F;
              look.sunLon = -25.0F;
              look.clouds = 0.25F;
              look.cloudDrift = t * 30.0F;
              const float front = 20.0F - 110.0F * smooth01(0.15F, 0.85F, t);  // 20E -> 90W
              renderGlobe(
                  dst,
                  w,
                  h,
                  src,
                  g,
                  look,
                  [&](float lat, float lon, const V3&, Surface& s)
                  {
                    if (lat < -10.0F || lat > 40.0F || lon < -100.0F || lon > 40.0F)
                      return;
                    // The plume: between the front and the source, wobbling in latitude.
                    const float axis = 17.0F + 4.0F * std::sin((lon + t * 60.0F) * 0.05F);
                    const float width = 6.0F + 4.0F * smooth01(10.0F, -60.0F, lon);
                    const float across = std::exp(-std::pow((lat - axis) / width, 2.0F));
                    const float reach =
                        smooth01(front - 6.0F, front + 6.0F, lon) * smooth01(32.0F, 20.0F, lon);
                    const float thin = 0.35F + 0.65F * smooth01(-80.0F, 0.0F, lon);
                    const float tex = noiseAt(map, 1, lat, lon + t * 70.0F);
                    const float dust =
                        std::clamp(across * reach * thin * (0.6F + 1.4F * tex), 0.0F, 0.9F);
                    s.albedo = mix(s.albedo, F3{215, 170, 110}, dust);
                    s.cloud *= 1.0F - 0.7F * dust;  // the dry layer suppresses cloud beneath
                  },
                  [](float, float, int, int, F3&) {});
            });
}

// Wildfire smoke. Fires burn across Canada into the evening, glowing on the
// night side; their smoke is lifted into the westerlies and streams east
// over North America and out across the Atlantic.
void effectWildfireSmoke(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  Globe g(w, h, ya, 0.92F);
  const EarthMap& map = earthMap();
  const std::vector<LatLon> fires = {
      {56, -120}, {58, -112}, {53, -105}, {50, -78}, {62, -125}, {55, -96}};
  runFrames(
      renderer,
      w,
      h,
      8500,
      [&](float t, std::vector<Rgb>& dst)
      {
        GlobeLook look;
        phase(t, look);
        g.look(48.0F, -88.0F);
        look.sunLat = 15.0F;
        look.sunLon = -40.0F - 40.0F * t;  // evening falls from the west
        look.clouds = 0.2F;
        look.cloudDrift = -t * 40.0F;
        const float reach = 75.0F * smooth01(0.12F, 0.85F, t);  // degrees downstream
        renderGlobe(
            dst,
            w,
            h,
            src,
            g,
            look,
            [&](float lat, float lon, const V3&, Surface& s)
            {
              if (lat < 25.0F || lat > 75.0F || lon < -140.0F || lon > 10.0F)
                return;
              float smoke = 0.0F;
              for (const auto& f : fires)
              {
                const float down = dlon(lon, f.lon);
                if (down < -2.0F || down > reach + 10.0F)
                  continue;
                // Plumes drift east and a little south, spreading with distance.
                const float axis = f.lat - 0.12F * down + 3.0F * std::sin(down * 0.08F + t * 3.0F);
                const float width = 1.5F + 0.12F * down;
                const float a = std::exp(-std::pow((lat - axis) / width, 2.0F)) *
                                smooth01(reach + 8.0F, reach - 8.0F, down) / (1.0F + down / 60.0F);
                smoke += a;
                // Burn scars around the fire.
                if (s.land)
                {
                  float x = 0.0F;
                  float y = 0.0F;
                  localDeg(lat, lon, f.lat, f.lon, x, y);
                  const float scar =
                      smooth01(2.5F * smooth01(0.0F, 0.6F, t), 0.0F, std::hypot(x, y));
                  s.albedo = mix(s.albedo, F3{45, 35, 30}, 0.8F * scar);
                }
              }
              const float tex = noiseAt(map, 1, lat, lon - t * 60.0F);
              smoke = std::clamp(smoke * (0.5F + tex), 0.0F, 0.85F);
              s.albedo = mix(s.albedo, F3{150, 130, 110}, smoke);
              s.cloud *= 1.0F - 0.5F * smoke;
            },
            [&](float lat, float lon, int, int, F3& c)
            {
              if (std::isnan(lat) || t < 0.12F)
                return;
              for (std::size_t k = 0; k < fires.size(); ++k)
              {
                float x = 0.0F;
                float y = 0.0F;
                localDeg(lat, lon, fires[k].lat, fires[k].lon, x, y);
                const float d2 = x * x + y * y;
                if (d2 > 9.0F)
                  continue;
                const float flicker = 0.7F + 0.3F * std::sin(t * 70.0F + k * 2.3F);
                c = add(c,
                        mul(F3{255, 120, 30},
                            1.1F * flicker * std::exp(-d2 / 0.8F) * smooth01(0.12F, 0.2F, t)));
              }
            });
      });
}

// Krakatoa, 1883. The eruption flashes, its pressure wave races around the
// planet — out to the antipode and back — and the stratospheric veil of
// sulphate aerosol spreads west along the equator, turning the sunsets red.
void effectKrakatoa(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  Globe g(w, h, ya, 0.92F);
  const EarthMap& map = earthMap();
  constexpr float kLat = -6.1F;
  constexpr float kLon = 105.4F;
  constexpr float kT0 = 0.18F;
  runFrames(renderer,
            w,
            h,
            9000,
            [&](float t, std::vector<Rgb>& dst)
            {
              GlobeLook look;
              phase(t, look);
              const float te = std::max(0.0F, t - kT0);
              // The camera follows the veil west.
              g.look(-4.0F, kLon + 10.0F - 40.0F * smooth01(0.0F, 0.9F, t));
              look.sunLat = 10.0F;
              look.sunLon = kLon - 60.0F;  // the terminator falls across the view
              look.clouds = 0.22F;
              look.cloudDrift = t * 30.0F;
              const float ring = te * 520.0F;  // the pressure wave, degrees from the volcano
              const float ringPos = std::fmod(ring, 360.0F);
              const float ringDeg = ringPos < 180.0F ? ringPos : 360.0F - ringPos;
              const float veilWest = 220.0F * smooth01(0.0F, 0.75F, te);
              const V3 kv = unit(kLat, kLon);
              const V3 sun = unit(look.sunLat, look.sunLon);
              renderGlobe(
                  dst,
                  w,
                  h,
                  src,
                  g,
                  look,
                  [&](float lat, float lon, const V3& p, Surface& s)
                  {
                    if (te <= 0.0F)
                      return;
                    // Pressure wave: a bright refraction line, fading as it travels.
                    const float d = std::acos(std::clamp(dot(p, kv), -1.0F, 1.0F)) / kDeg;
                    const float wave =
                        std::exp(-std::pow((d - ringDeg) / 2.0F, 2.0F)) * std::exp(-ring / 900.0F);
                    s.albedo = add(s.albedo, mul(F3{120, 150, 190}, wave));
                    s.cloud = std::max(s.cloud, 0.4F * wave);
                    // The veil: west of Krakatoa, a few degrees either side of the equator.
                    const float west = -dlon(lon, kLon);
                    const float span = smooth01(-15.0F, -3.0F, west) *
                                       smooth01(veilWest + 15.0F, veilWest - 15.0F, west);
                    const float band = std::exp(
                        -std::pow((lat + 3.0F) / (10.0F + 0.08F * std::max(0.0F, west)), 2.0F));
                    const float veil =
                        span * band * (0.5F + 0.6F * noiseAt(map, 1, lat, lon + t * 80.0F));
                    // Red sunsets where the veil meets the terminator.
                    const float sunset = std::exp(-std::pow(dot(p, sun) / 0.12F, 2.0F));
                    s.albedo = mix(s.albedo, F3{205, 195, 180}, std::min(0.7F, veil * 0.8F));
                    s.albedo = add(s.albedo, mul(F3{230, 70, 30}, veil * sunset * 2.0F));
                    s.cloud *= 1.0F - 0.5F * std::min(1.0F, veil);
                  },
                  [&](float lat, float lon, int, int, F3& c)
                  {
                    if (std::isnan(lat))
                      return;
                    float x = 0.0F;
                    float y = 0.0F;
                    localDeg(lat, lon, kLat, kLon, x, y);
                    const float d2 = x * x + y * y;
                    if (d2 > 60.0F)
                      return;
                    const float flash = std::exp(-std::pow((t - kT0) / 0.02F, 2.0F));
                    const float glow = smooth01(kT0 - 0.01F, kT0, t) * std::exp(-te * 6.0F);
                    c = add(c, mul(F3{255, 200, 120}, flash * 2.5F * std::exp(-d2 / 30.0F)));
                    c = add(c, mul(F3{255, 90, 30}, glow * 1.5F * std::exp(-d2 / 2.0F)));
                  });
            });
}

// Hurricane season. Tropical cyclones are born off Africa, spiral west
// across the Atlantic, strengthen, recurve north-east and die, each leaving
// its track coloured by Saffir-Simpson category.
void effectHurricaneTracks(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  Globe g(w, h, ya, 0.92F);
  const EarthMap& map = earthMap();
  struct Storm
  {
    std::vector<LatLon> path;
    float t0;
    float t1;
    float peak;  // category 0..5
  };
  const std::vector<Storm> storms = {
      {{{12, -22},
        {14, -35},
        {17, -50},
        {21, -62},
        {26, -72},
        {32, -76},
        {39, -70},
        {45, -55},
        {50, -38}},
       0.12F,
       0.7F,
       4.5F},
      {{{11, -30}, {13, -45}, {16, -58}, {19, -70}, {22, -80}, {25, -88}, {29, -91}, {33, -88}},
       0.25F,
       0.8F,
       5.0F},
      {{{14, -25}, {17, -38}, {22, -48}, {28, -55}, {35, -55}, {42, -48}, {48, -35}},
       0.38F,
       0.85F,
       2.5F},
      {{{10, -40}, {12, -52}, {15, -63}, {17, -74}, {18, -84}}, 0.5F, 0.92F, 3.5F}};
  static const std::array<F3, 6> kCat = {F3{80, 170, 255},
                                         F3{250, 240, 120},
                                         F3{255, 200, 80},
                                         F3{255, 140, 60},
                                         F3{240, 70, 50},
                                         F3{220, 60, 200}};
  struct Now
  {
    LatLon at;
    float strength;  // 0..1
    float u;
  };
  std::vector<Now> now(storms.size());
  runFrames(
      renderer,
      w,
      h,
      9000,
      [&](float t, std::vector<Rgb>& dst)
      {
        GlobeLook look;
        phase(t, look);
        g.look(26.0F, -55.0F);
        look.sunLat = 18.0F;
        look.sunLon = -50.0F;
        look.clouds = 0.2F;
        look.cloudDrift = -t * 30.0F;
        for (std::size_t k = 0; k < storms.size(); ++k)
        {
          const float u = (t - storms[k].t0) / (storms[k].t1 - storms[k].t0);
          now[k].u = u;
          now[k].at = alongPath(storms[k].path, u);
          now[k].strength = u > 0.0F && u < 1.0F ? std::sin(kPi * std::pow(u, 0.8F)) : 0.0F;
        }
        renderGlobe(
            dst,
            w,
            h,
            src,
            g,
            look,
            [&](float lat, float lon, const V3&, Surface& s)
            {
              for (std::size_t k = 0; k < storms.size(); ++k)
              {
                if (now[k].strength <= 0.0F)
                  continue;
                float x = 0.0F;
                float y = 0.0F;
                localDeg(lat, lon, now[k].at.lat, now[k].at.lon, x, y);
                const float size = 5.0F + 6.0F * now[k].strength;
                const float r = std::hypot(x, y);
                if (r > size * 1.6F)
                  continue;
                const float ang = std::atan2(y, x);
                // Cyclonic (counter-clockwise) arms, a dense core and a clear eye.
                const float arms =
                    0.5F +
                    0.5F * std::sin(2.0F * ang + 3.0F * std::log(r / size + 0.05F) + t * 20.0F);
                const float tex = noiseAt(map, 2, lat * 2.0F, lon * 2.0F);
                float c = std::exp(-r / (size * 0.45F)) * 1.4F +
                          arms * smooth01(size * 1.6F, size * 0.5F, r) * 0.8F;
                c *= (0.6F + 0.6F * tex) * now[k].strength;
                c *= smooth01(0.25F, 0.55F, r / (0.12F * size + 0.3F));  // eye
                s.cloud = std::max(s.cloud, std::min(1.0F, c));
              }
            },
            [](float, float, int, int, F3&) {});
        if (look.intro < 0.9F)
          return;
        const float a = smooth01(0.9F, 1.0F, look.intro) * look.fade;
        for (std::size_t k = 0; k < storms.size(); ++k)
        {
          const float u1 = std::min(1.0F, now[k].u);
          for (float u = 0.0F; u < u1; u += 0.006F)
          {
            const LatLon q = alongPath(storms[k].path, u);
            const float strength = std::sin(kPi * std::pow(u, 0.8F));
            const int cat = std::clamp(static_cast<int>(strength * storms[k].peak + 0.5F), 0, 5);
            splat(dst, w, h, g, q.lat, q.lon, 0.0F, 0.9F, kCat[cat], a * 0.9F);
          }
        }
      });
}

// Sea ice through a year. The Arctic seen from above: winter ice reaching
// the Sea of Okhotsk and Labrador, the polar night lifting, the melt season
// opening the coasts, the September minimum, and the freeze-up again.
void effectSeaIce(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  Globe g(w, h, ya, 0.92F);
  const EarthMap& map = earthMap();
  runFrames(renderer,
            w,
            h,
            9000,
            [&](float t, std::vector<Rgb>& dst)
            {
              GlobeLook look;
              phase(t, look);
              // From the March maximum through the year.
              const float year = smooth01(0.12F, 0.95F, t);
              const float season = std::sin(2.0F * kPi * year);  // 1 in June
              // Ice lags the sun: minimum in September, maximum in March.
              const float melt = 0.5F - 0.5F * std::cos(2.0F * kPi * year);  // 1 in September
              g.look(78.0F, -40.0F + 50.0F * t);
              look.sunLat = 23.4F * season;
              look.sunLon = -40.0F + 50.0F * t;
              look.clouds = 0.15F;
              look.cloudDrift = t * 50.0F;
              look.night = 0.12F;
              renderGlobe(
                  dst,
                  w,
                  h,
                  src,
                  g,
                  look,
                  [&](float lat, float lon, const V3&, Surface& s)
                  {
                    if (lat < 40.0F)
                      return;
                    const float n = noiseAt(map, 2, lat, lon) - 0.5F;
                    if (s.land)
                    {
                      // Snow cover retreats north in spring and returns in autumn.
                      const float snowLine = 50.0F + 22.0F * melt;
                      s.albedo = mix(s.albedo,
                                     F3{235, 240, 248},
                                     smooth01(snowLine - 2.0F, snowLine + 2.0F, lat + n * 8.0F));
                      return;
                    }
                    // Winter edge: far south in the Pacific sector, held north by the
                    // Atlantic inflow; summer: the central Arctic only.
                    const float atlantic = std::exp(-std::pow(dlon(lon, 10.0F) / 40.0F, 2.0F));
                    const float maxEdge = 58.0F + 16.0F * atlantic;
                    const float minEdge = 77.0F + 4.0F * atlantic;
                    const float edge = maxEdge + (minEdge - maxEdge) * melt;
                    const float ice = smooth01(edge - 1.0F, edge + 1.5F, lat + n * 7.0F);
                    const float floe = 0.85F + 0.3F * noiseAt(map, 2, lat * 2.0F, lon * 2.0F);
                    s.albedo = mix(s.albedo, mul(F3{230, 238, 248}, floe), ice);
                    // Melt ponds darken the summer pack.
                    s.albedo =
                        mix(s.albedo, F3{120, 160, 200}, ice * 0.3F * std::max(0.0F, season));
                  },
                  [](float, float, int, int, F3&) {});
            });
}

// The ozone hole. Antarctica in the southern spring, coloured by total
// column ozone in the familiar TOMS palette: the hole deepens and spreads
// inside the spinning polar vortex, stretches, then breaks up into filaments
// as the vortex weakens.
void effectOzoneHole(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  Globe g(w, h, ya, 0.92F);
  const EarthMap& map = earthMap();
  static const std::array<F3, 8> kToms = {F3{110, 30, 150},
                                          F3{60, 40, 200},
                                          F3{40, 120, 230},
                                          F3{40, 200, 200},
                                          F3{70, 200, 80},
                                          F3{230, 230, 60},
                                          F3{240, 140, 40},
                                          F3{220, 40, 40}};
  runFrames(renderer,
            w,
            h,
            9000,
            [&](float t, std::vector<Rgb>& dst)
            {
              GlobeLook look;
              phase(t, look);
              g.look(-75.0F, 20.0F * t);
              look.sunLat = -5.0F - 15.0F * t;  // spring sun returning
              look.sunLon = 0.0F;
              look.clouds = 0.15F;
              look.cloudDrift = t * 40.0F;
              look.night = 0.25F;
              const float deepen = smooth01(0.15F, 0.55F, t);
              const float breakup = smooth01(0.68F, 0.95F, t);
              const float spin = t * 140.0F;  // vortex rotation, degrees
              const float show = smooth01(0.12F, 0.25F, t);
              renderGlobe(
                  dst,
                  w,
                  h,
                  src,
                  g,
                  look,
                  [&](float lat, float lon, const V3&, Surface& s)
                  {
                    if (lat > -25.0F)
                      return;
                    // Elongated vortex: colatitude edge varies with wavenumber 2.
                    const float colat = 90.0F + lat;
                    const float stretch =
                        1.0F + (0.15F + 0.35F * breakup) * std::cos(2.0F * (lon - spin) * kDeg);
                    float edge = (16.0F + 8.0F * deepen) * stretch * (1.0F - 0.4F * breakup);
                    edge += (noiseAt(map, 1, lat, lon - spin) - 0.5F) * (6.0F + 20.0F * breakup);
                    const float inside = smooth01(edge + 3.0F, edge - 3.0F, colat);
                    float du = 330.0F + 70.0F * std::exp(-std::pow((colat - edge - 12.0F) / 8.0F,
                                                                   2.0F));  // collar
                    du -= (200.0F * deepen) * inside * (1.0F - 0.5F * breakup);
                    du += (noiseAt(map, 2, lat, lon - spin) - 0.5F) * 30.0F;
                    const F3 col = ramp(kToms, (du - 100.0F) / 400.0F);
                    s.albedo = mix(s.albedo, col, 0.75F * show * smooth01(-25.0F, -40.0F, lat));
                    // The 220 DU contour that defines the hole.
                    if (std::fabs(du - 220.0F) < 6.0F)
                      s.albedo = mix(s.albedo, F3{255, 255, 255}, 0.8F * show);
                    s.cloud *= 1.0F - 0.6F * show;
                  },
                  [](float, float, int, int, F3&) {});
            });
}

// The auroral oval. The night side of the Arctic from space: a ring of
// green light around the geomagnetic pole, quiet at first, until a
// substorm breaks out near midnight, brightens, surges west and expands
// poleward in rippling curtains topped with red.
void effectAuroralOval(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  Globe g(w, h, ya, 0.92F);
  constexpr float kPoleLat = 80.7F;
  constexpr float kPoleLon = -72.7F;
  const V3 pole = unit(kPoleLat, kPoleLon);
  runFrames(
      renderer,
      w,
      h,
      8500,
      [&](float t, std::vector<Rgb>& dst)
      {
        GlobeLook look;
        phase(t, look);
        g.look(62.0F, -60.0F);
        look.sunLat = -12.0F;
        look.sunLon = 110.0F;  // midnight over North America
        look.clouds = 0.2F;
        look.cloudDrift = t * 30.0F;
        look.night = 0.1F;  // moonlit
        const V3 sun = unit(look.sunLat, look.sunLon);
        const float onset = smooth01(0.42F, 0.55F, t);
        const float recover = smooth01(0.75F, 0.95F, t);
        const float storm = onset * (1.0F - 0.6F * recover);
        const float surge = 70.0F * smooth01(0.45F, 0.8F, t);  // westward surge, degrees
        const float show = smooth01(0.12F, 0.3F, t);
        renderGlobe(
            dst,
            w,
            h,
            src,
            g,
            look,
            [](float, float, const V3&, Surface&) {},
            [&](float lat, float lon, int, int, F3& c)
            {
              if (std::isnan(lat) || lat < 45.0F)
                return;
              const V3 p = unit(lat, lon);
              // Geomagnetic colatitude and magnetic local time (0 = noon side).
              const float mcol = std::acos(std::clamp(dot(p, pole), -1.0F, 1.0F)) / kDeg;
              // Direction away from the sun, projected: night side has dot < 0.
              const float night = -dot(p, sun);
              const float nightSide = std::clamp(0.5F + 0.6F * night, 0.0F, 1.0F);
              // The oval sits further from the pole on the night side, more so in a storm.
              const float az = std::atan2(p.y, p.x) / kDeg;
              const float fold = 1.2F * std::sin(az * 0.12F * kPi + t * 6.0F) +
                                 0.6F * std::sin(az * 0.31F * kPi - t * 11.0F);
              const float centre = 15.0F + 6.0F * nightSide + 3.0F * storm + fold * (0.5F + storm);
              // Discrete arcs: a thin bright one and a fainter second arc poleward,
              // over a broad diffuse glow.
              const float d = mcol - centre;
              const float arc1 = std::exp(-std::pow(d / (0.9F + 0.6F * storm), 2.0F));
              const float arc2 = 0.5F *
                                 std::exp(-std::pow((d + 2.5F + 2.0F * storm) / 0.7F, 2.0F)) *
                                 (0.3F + storm);
              const float diffuse = 0.25F * std::exp(-std::pow(d / (4.0F + 3.0F * storm), 2.0F));
              const float ring = arc1 + arc2 + diffuse;
              if (ring < 0.01F)
                return;
              // Bulge near magnetic midnight, surging west after onset.
              const float mid = std::exp(
                  -std::pow(dlon(az, 110.0F + 180.0F + surge * 0.3F) / (30.0F + surge), 2.0F));
              // Rayed structure along the arcs, drifting.
              const float rays = 0.6F + 0.4F * std::sin(az * 9.0F + t * 30.0F +
                                                        3.0F * std::sin(az * 0.7F - t * 7.0F));
              const float bright = ring * (0.4F + 0.4F * nightSide + 1.6F * storm * mid) *
                                   (diffuse > 0.0F ? rays : 1.0F) * show;
              c = add(c, mul(F3{60, 255, 120}, bright));
              // Red oxygen tops on the poleward edge in the storm.
              c = add(
                  c,
                  mul(F3{200, 40, 70},
                      std::exp(-std::pow((d + 1.5F) / 1.2F, 2.0F)) * storm * mid * 0.8F * show));
            });
      });
}

// The jet stream. The northern hemisphere seen from above Iceland: tracers
// race east in the polar-front jet while a Rossby wave grows in it, the
// meanders deepen, cold air plunges south beneath the troughs and the
// warm ridges bulge north, with cloud along the jet's poleward flank.
void effectJetStream(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  Globe g(w, h, ya, 0.92F);
  const EarthMap& map = earthMap();
  float drift = 0.0F;
  const auto axisAt = [&](float lon, float amp, float ph)
  {
    // A wave packet: wavenumbers 4 and 6 beating, so the meanders differ.
    const float env = 0.6F + 0.4F * std::sin((lon + ph * 0.3F) * kDeg);
    return 52.0F +
           amp * env *
               (0.7F * std::sin((4.0F * lon + ph) * kDeg) +
                0.45F * std::sin((6.0F * lon - 0.7F * ph + 50.0F) * kDeg)) +
           4.0F * (noiseAt(earthMap(), 0, 40.0F, lon) - 0.5F);
  };
  runFrames(renderer,
            w,
            h,
            8500,
            [&](float t, std::vector<Rgb>& dst)
            {
              GlobeLook look;
              phase(t, look);
              g.look(68.0F, -20.0F);
              look.sunLat = 20.0F;
              look.sunLon = -20.0F;
              look.clouds = 0.15F;
              look.cloudDrift = t * 60.0F;
              const float amp = 3.0F + 11.0F * smooth01(0.15F, 0.75F, t);
              const float ph = -t * 200.0F;  // the wave drifts slowly east
              drift += 1.0F;
              renderGlobe(
                  dst,
                  w,
                  h,
                  src,
                  g,
                  look,
                  [&](float lat, float lon, const V3&, Surface& s)
                  {
                    if (lat < 15.0F)
                      return;
                    const float axis = axisAt(lon, amp, ph);
                    const float side = (lat - axis) / 6.0F;
                    // Cold poleward, warm equatorward, as a temperature tint on land
                    // (over the dark ocean the tint only muddies it).
                    const float tint = std::clamp(side, -1.0F, 1.0F) * smooth01(0.0F, 4.0F, amp);
                    if (s.land)
                      s.albedo = mix(s.albedo,
                                     tint > 0.0F ? F3{150, 185, 255} : F3{255, 160, 80},
                                     0.3F * std::fabs(tint));
                    // Jet-stream cirrus and frontal cloud on the poleward flank.
                    const float flank = std::exp(-std::pow(side - 0.4F, 2.0F) * 2.0F);
                    const float n = noiseAt(map, 2, lat, lon - t * 150.0F);
                    s.cloud = std::max(s.cloud, smooth01(0.4F, 0.6F, n) * flank * 0.85F);
                  },
                  [](float, float, int, int, F3&) {});
              if (look.intro < 0.9F)
                return;
              const float a = smooth01(0.9F, 1.0F, look.intro) * look.fade;
              constexpr int kTracers = 70;
              for (int k = 0; k < kTracers; ++k)
              {
                const float off = (hash01(k, 3, 7) - 0.5F) * 6.0F;
                const float speed = 2.2F * (1.0F - std::fabs(off) / 4.0F);  // fastest in the core
                const float lon0 = hash01(k, 4, 7) * 360.0F + drift * speed;
                for (int tail = 0; tail < 8; ++tail)
                {
                  const float lon = lon0 - tail * 1.1F;
                  const float lat = axisAt(lon, amp, ph) + off;
                  const F3 col = mix(F3{255, 255, 255}, F3{120, 200, 255}, tail / 8.0F);
                  splat(dst, w, h, g, lat, lon, 0.02F, 0.9F, col, a * (0.8F - 0.09F * tail));
                }
              }
            });
}

}  // namespace ee_detail
}  // namespace Qdless
