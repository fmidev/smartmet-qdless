// Earth-science exit effects in which the current view itself is the medium:
// the map is advected by an analytic flow, swept by a radar beam, contoured,
// seen through wet glass, frozen, torn by a fault, or laid out as a sea
// surface under a setting sun.

#include "QdlessExitEffectKit.h"

namespace Qdless
{
namespace ee_detail
{
using namespace kit;

// Kelvin-Helmholtz billows. The view shears along its midline (top half to
// the right, bottom half to the left) and the interface rolls up into a row
// of cat's-eye vortices: the Stuart vortex solution of the Euler equations,
// psi = U/k ln(cosh ky + eps cos kx), with eps growing from pure tanh shear.
// Fluid that has overturned across the interface condenses into billow cloud.
void effectKelvinHelmholtz(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float k = 2.0F * kPi / (static_cast<float>(w) / 3.0F);  // three billows
  const float U = static_cast<float>(w) / 200.0F;
  const float yi = h * 0.5F;
  FlowMap flow(w, h, Boundary::Wrap);
  runFrames(renderer,
            w,
            h,
            7000,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float eps = 0.8F * smooth01(0.08F, 0.5F, t);
              const float drive = U * smooth01(0.0F, 0.12F, t);
              // Secondary instabilities: the billows break down into turbulence.
              const float turb = U * 1.6F * smooth01(0.5F, 0.8F, t);
              flow.step(
                  [&](float x, float y, float& vx, float& vy)
                  {
                    const float X = k * x;
                    const float Y = k * (y - yi) * ya;
                    const float D = std::cosh(Y) + eps * std::cos(X);
                    vx = drive * std::sinh(Y) / D;
                    vy = drive * eps * std::sin(X) / D / ya;
                    if (turb > 0.0F)
                    {
                      const float band = std::exp(-0.25F * Y * Y);
                      const float nx = x * 0.06F;
                      const float ny = y * ya * 0.06F + t * 3.0F;
                      vx += turb * band * (vnoise(nx, ny, 41) - 0.5F) * 2.0F;
                      vy += turb * band * (vnoise(nx, ny, 43) - 0.5F) * 2.0F / ya;
                    }
                  });
              const float fade = 1.0F - smooth01(0.72F, 1.0F, t);
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const std::size_t i = static_cast<std::size_t>(y) * w + x;
                  const float sv = flow.v(i);
                  F3 c = bilinear(src, w, h, flow.u(i), sv);
                  // Overturned fluid: came from the other side of the interface.
                  const float side = (sv - yi) * (static_cast<float>(y) - yi);
                  if (side < 0.0F)
                  {
                    const float depth = std::min(std::fabs(sv - yi), std::fabs(y - yi));
                    const float cloud = 0.55F * smooth01(0.0F, 3.0F, depth);
                    c = mix(c, F3{245, 240, 235}, cloud);
                  }
                  // Sunlit crests, shaded troughs: light from above the shear layer.
                  const float X = k * x;
                  const float Y = k * (y - yi) * ya;
                  const float lit = 1.0F + 0.18F * eps * std::sin(X) * std::exp(-0.5F * Y * Y);
                  dst[i] = toRgb(mul(c, lit * fade));
                }
            });
}

// Kármán vortex street. The view streams past a mountainous island and sheds
// an alternating double row of vortices (Strouhal number 0.3), as in satellite
// pictures of stratocumulus downwind of Jan Mayen or Guadalupe.
void effectKarmanVortex(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float R = mn * 0.075F;  // island radius, isotropic pixels
  const float xc = w * 0.18F;
  const float yc = h * 0.5F;
  const float U = static_cast<float>(w) / 140.0F;
  const float halfPeriod = (2.0F * R / (0.3F * U)) * 0.5F;  // frames between sheds
  constexpr float kFirstShed = 12.0F;
  FlowMap flow(w, h, Boundary::Wrap);
  int frame = 0;

  struct Vortex
  {
    float x;
    float y;  // screen rows
    float gamma;
    float rc2;
  };
  std::vector<Vortex> active;

  runFrames(
      renderer,
      w,
      h,
      7500,
      [&](float t, std::vector<Rgb>& dst)
      {
        ++frame;
        const float f = static_cast<float>(frame);
        const float ramp = smooth01(0.0F, 0.1F, t);
        active.clear();
        for (int n = 0;; ++n)
        {
          const float born = kFirstShed + n * halfPeriod;
          if (born > f)
            break;
          const float age = f - born;
          const float x = xc + 1.4F * R + 0.8F * U * age;
          if (x > w + 4.0F * R)
            continue;
          const float sgn = (n % 2 == 0) ? 1.0F : -1.0F;  // +1 = upper row
          const float off = (0.3F + 0.55F * std::min(1.0F, age / 50.0F)) * R;
          const float rc = 0.5F * R * (1.0F + age / 250.0F);
          const float grow = std::min(1.0F, age / 10.0F);
          active.push_back({x, yc - sgn * off / ya, sgn * 6.5F * U * rc * grow, rc * rc});
        }
        flow.step(
            [&](float x, float y, float& vx, float& vy)
            {
              const float X = x - xc;
              const float Y = (y - yc) * ya;
              const float r2 = X * X + Y * Y;
              if (r2 < R * R)
              {
                vx = vy = 0.0F;
                return;
              }
              const float R2 = R * R;
              const float r4 = r2 * r2;
              float ux = U * (1.0F - R2 * (X * X - Y * Y) / r4);
              float uy = -U * 2.0F * R2 * X * Y / r4;
              for (const auto& v : active)
              {
                const float dx = x - v.x;
                const float dy = (y - v.y) * ya;
                const float d2 = dx * dx + dy * dy + 1e-3F;
                const float fac = v.gamma / (2.0F * kPi * d2) * (1.0F - std::exp(-d2 / v.rc2));
                ux += -fac * dy;
                uy += fac * dx;
              }
              vx = ux * ramp;
              vy = uy * ramp / ya;
            });

        const float fade = 1.0F - smooth01(0.86F, 1.0F, t);
        const float lx = -0.55F, ly = -0.55F, lz = 0.63F;  // light from the upper left
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const std::size_t i = static_cast<std::size_t>(y) * w + x;
            const float X = (x - xc) / R;
            const float Y = (y - yc) * ya / R;
            const float q2 = X * X + Y * Y;
            F3 c;
            if (q2 < 1.0F)  // the island, with a ragged coast and an off-centre peak
            {
              const float q = std::sqrt(q2);
              const float ca = X / std::max(q, 1e-3F);
              const float sa = Y / std::max(q, 1e-3F);
              const float coast = 0.8F + 0.25F * fbm(ca * 1.6F + 3.0F, sa * 1.6F + 3.0F, 3, 67);
              const float px = X + 0.3F;  // the peak sits upwind of the centre
              const float py = Y + 0.15F;
              const float hq = std::sqrt(px * px + py * py) / coast;
              const float ridge = fbm(X * 3.0F + 7.0F, Y * 3.0F, 3, 61);
              if (q > coast)  // shallow water ringing the shore
                c = mix(bilinear(src, w, h, flow.u(i), flow.v(i)), F3{120, 200, 190}, 0.6F);
              else
              {
                const float height = std::max(0.0F, 1.0F - hq) * (0.8F + 0.4F * ridge);
                // Lambert light on the height field's local slope.
                const float e = 0.05F;
                const float hx = (std::max(0.0F, 1.0F - std::hypot(px + e, py) / coast) -
                                  std::max(0.0F, 1.0F - hq)) /
                                 e;
                const float hy = (std::max(0.0F, 1.0F - std::hypot(px, py + e) / coast) -
                                  std::max(0.0F, 1.0F - hq)) /
                                 e;
                const float nx = -hx * 0.8F;
                const float ny = -hy * 0.8F;
                const float lit = std::clamp(
                    (nx * lx + ny * ly + lz) / std::sqrt(nx * nx + ny * ny + 1.0F), 0.1F, 1.0F);
                F3 ground = mix(F3{175, 160, 110}, F3{65, 105, 55}, smooth01(0.0F, 0.12F, height));
                ground = mix(ground, F3{100, 85, 70}, smooth01(0.35F, 0.6F, height));
                ground =
                    mix(ground, F3{245, 248, 255}, smooth01(0.62F, 0.75F, height + 0.1F * ridge));
                c = mul(ground, (0.6F + 0.6F * lit) * (0.85F + 0.3F * ridge));
              }
            }
            else
            {
              c = bilinear(src, w, h, flow.u(i), flow.v(i));
              // Lee shadow just downwind of the island.
              if (X > 0.0F && std::fabs(Y) < 1.0F)
                c = mul(c, 1.0F - 0.35F * std::exp(-(X - 1.0F) * 1.5F) * std::sqrt(1.0F - Y * Y));
            }
            dst[i] = toRgb(mul(c, fade));
          }
      });
}

// Weather radar PPI scope. A rotating beam reveals the view in true colour,
// the phosphor afterglow fades it to green, and storm cells, refreshed only
// as the beam passes, drift through in reflectivity colours.
void effectRadarSweep(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float cx = (w - 1) * 0.5F;
  const float cy = (h - 1) * 0.5F;
  const float Rp = std::min(w * 0.5F, h * 0.5F * ya) * 0.97F;
  constexpr float kRevs = 3.0F;
  constexpr float kSweepEnd = 0.9F;  // the last revolution finishes here
  const std::size_t n = static_cast<std::size_t>(w) * h;
  std::vector<float> theta(n);
  std::vector<float> rr(n);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
    {
      const float dx = x - cx;
      const float dy = (y - cy) * ya;
      const std::size_t i = static_cast<std::size_t>(y) * w + x;
      theta[i] = fract(std::atan2(dx, -dy) / (2.0F * kPi) + 1.0F);  // clockwise from north
      rr[i] = std::sqrt(dx * dx + dy * dy) / Rp;
    }
  runFrames(renderer,
            w,
            h,
            7000,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float beam = std::min(t / kSweepEnd, 1.0F) * kRevs;
              const float fade = 1.0F - smooth01(0.9F, 1.0F, t);
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const std::size_t i = static_cast<std::size_t>(y) * w + x;
                  const float r = rr[i];
                  if (r > 1.0F)
                  {
                    const float bezel = r < 1.02F ? 90.0F : 22.0F;
                    dst[i] = toRgb(mul(F3{bezel * 0.8F, bezel, bezel * 0.8F}, fade));
                    continue;
                  }
                  F3 c{0, 8, 2};
                  const float since = beam - theta[i];
                  if (since >= 0.0F)
                  {
                    const float scan = std::floor(since);
                    const float age = since - scan;  // revolutions since the beam passed
                    const float glow = std::exp(-age * 3.0F);
                    const float level = std::max(0.0F, 1.0F - 0.3F * scan);
                    const F3 view = texel(src, w, x, y);
                    const float l = luma(view);
                    const F3 phosphor{l * 0.15F, l * 0.9F, l * 0.35F};
                    c = mul(mix(view, phosphor, smooth01(0.0F, 0.6F, age + 0.25F * scan)),
                            (0.2F + 0.8F * glow) * level);
                    // Storm cells as they were when the beam last passed.
                    const float tSweep = (beam - age) / kRevs;
                    const float cell = fbm(x * 0.035F - tSweep * 3.0F, y * ya * 0.035F, 4, 77);
                    const float thr = 0.66F - 0.06F * std::min(scan, 2.0F);
                    if (cell > thr && r > 0.06F)
                    {
                      const float s = (cell - thr) / (1.0F - thr);
                      c = mix(c,
                              dbzColour(s * 1.6F),
                              std::min(1.0F, 0.4F + s * 2.0F) * (0.3F + 0.7F * glow));
                    }
                    // The beam itself and its bright leading edge.
                    if (age < 0.006F)
                      c = add(c, F3{120, 255, 150});
                  }
                  // Range rings every quarter radius and spokes every 30 degrees.
                  const float ring = std::fabs(fract(r * 4.0F + 0.5F) - 0.5F) * Rp / 4.0F;
                  const float spoke = std::fabs(fract(theta[i] * 12.0F + 0.5F) - 0.5F) * 2.0F *
                                      kPi / 12.0F * r * Rp;
                  if (ring < 0.6F || (spoke < 0.5F && r > 0.04F))
                    c = add(c, F3{10, 55, 20});
                  dst[i] = toRgb(mul(c, fade));
                }
            });
}

// The view dissolves into its own contour lines. Isolines of the smoothed
// brightness field march across the map and grow denser, then lift off and
// drift away like smoke.
void effectIsolines(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const std::size_t n = static_cast<std::size_t>(w) * h;
  const std::vector<F3> smooth = blurred(src, w, h, 2, 2);
  std::vector<float> L(n);
  for (std::size_t i = 0; i < n; ++i)
    L[i] = luma(smooth[i]) / 255.0F;
  std::vector<float> grad(n, 0.0F);
  for (int y = 1; y + 1 < h; ++y)
    for (int x = 1; x + 1 < w; ++x)
    {
      const std::size_t i = static_cast<std::size_t>(y) * w + x;
      const float gx = 0.5F * (L[i + 1] - L[i - 1]);
      const float gy = 0.5F * (L[i + w] - L[i - w]) / ya;
      grad[i] = std::sqrt(gx * gx + gy * gy);
    }
  // Line colour: the view's own palette colour, lifted.
  std::vector<F3> ink(n);
  for (std::size_t i = 0; i < n; ++i)
  {
    const F3 c = smooth[i];
    ink[i] = add(mul(c, 1.15F), F3{40, 40, 40});
  }
  runFrames(renderer,
            w,
            h,
            7000,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float levels = 10.0F + 22.0F * smooth01(0.12F, 0.62F, t);
              const float offset = t * 3.0F;
              const float bg = 1.0F - 0.85F * smooth01(0.0F, 0.3F, t);
              const float lift = smooth01(0.62F, 1.0F, t);
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const std::size_t i = static_cast<std::size_t>(y) * w + x;
                  float sx = static_cast<float>(x);
                  float sy = static_cast<float>(y);
                  float breakup = 1.0F;
                  if (lift > 0.0F)
                  {
                    const float nz = fbm(x * 0.03F, y * 0.03F * ya + t * 2.0F, 3, 11);
                    sy += lift * h * (0.35F + 0.9F * nz);
                    sx += lift * w * 0.08F * std::sin(y * 0.07F + t * 9.0F + nz * 4.0F);
                    breakup =
                        smooth01(lift - 0.05F, lift + 0.25F, fbm(x * 0.05F, y * 0.08F, 3, 23));
                  }
                  F3 c = mul(texel(src, w, x, y), bg * (1.0F - lift));
                  c = add(c, F3{4, 6, 16});
                  if (sy < h - 1)
                  {
                    const float lv = bilinear(L, w, h, sx, sy) * levels + offset;
                    const float g = bilinear(grad, w, h, sx, sy) * levels;
                    const float d = std::fabs(fract(lv + 0.5F) - 0.5F);  // in level units
                    const float px = d / std::max(g, 1e-4F);
                    float inten = std::clamp(1.1F - px, 0.0F, 1.0F);
                    const int idx = static_cast<int>(std::floor(lv + 0.5F));
                    if (idx % 5 == 0)
                      inten = std::min(1.0F, inten * 1.4F);  // index contours
                    inten *= smooth01(0.0F, 0.12F, t) * breakup;
                    if (inten > 0.0F)
                    {
                      const std::size_t si =
                          static_cast<std::size_t>(std::clamp(static_cast<int>(sy), 0, h - 1)) * w +
                          std::clamp(static_cast<int>(sx), 0, w - 1);
                      c = mix(c, ink[si], inten);
                    }
                  }
                  dst[i] = toRgb(c);
                }
            });
}

// Rain running down a window in front of the view. Each drop is a little
// lens showing the scene behind it upside down; big drops stick, slip and
// leave trails, swallowing the droplets in their way. Finally the glass
// fogs over, except where the water has cleared it.
void effectRainOnGlass(
    const Renderer& renderer, const std::vector<Rgb>& src, int w, int h, std::mt19937& rng)
{
  const float ya = yAspectFor(renderer);
  const std::vector<F3> soft = blurred(src, w, h, 2, 2);
  std::uniform_real_distribution<float> u01(0.0F, 1.0F);
  struct Drop
  {
    float x;
    float y;
    float r;  // isotropic pixels
    float born;
    float vy = 0.0F;
    float stick = 0.0F;
    bool big = false;
    bool alive = true;
    float drift = 0.0F;  // sideways meander, pixels per frame
  };
  std::vector<Drop> drops;
  const int nSmall = w * h / 140;
  for (int i = 0; i < nSmall; ++i)
    drops.push_back(
        {u01(rng) * w, u01(rng) * h, 0.8F + 1.6F * u01(rng) * u01(rng), u01(rng) * 0.7F});
  const int nBig = 10 + w / 30;
  for (int i = 0; i < nBig; ++i)
  {
    Drop d{u01(rng) * w, u01(rng) * h * 0.7F, 3.5F + 3.0F * u01(rng), 0.04F + 0.55F * u01(rng)};
    d.big = true;
    d.stick = 5.0F + 30.0F * u01(rng);
    drops.push_back(d);
  }
  // Glass wiped clear by the water: fog cannot form on the drops' paths.
  std::vector<float> clear(static_cast<std::size_t>(w) * h, 0.0F);

  auto drawDrop = [&](std::vector<F3>& img, const Drop& d)
  {
    const float r = d.r;
    const float stretch = d.big ? 1.0F + std::min(0.4F, d.vy * 0.25F) : 1.0F;  // runs long
    const int x0 = std::max(0, static_cast<int>(d.x - r - 1));
    const int x1 = std::min(w - 1, static_cast<int>(d.x + r + 1));
    const int y0 = std::max(0, static_cast<int>(d.y - r * stretch / ya - 1));
    const int y1 = std::min(h - 1, static_cast<int>(d.y + r / ya + 1));
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x)
      {
        const float dx = (x - d.x) / r;
        float dy = (y - d.y) * ya / r;
        if (dy < 0.0F)
          dy /= stretch;
        const float q2 = dx * dx + dy * dy;
        if (q2 > 1.0F)
          continue;
        const float q = std::sqrt(q2);
        // Inverted, wide-angle image of the scene behind the drop.
        F3 c = bilinear(src, w, h, d.x - dx * r * 3.2F, d.y - dy * r * 3.2F / ya);
        c = mul(c, 0.9F + 0.25F * (1.0F - q2));
        if (q > 0.65F)
          c = mul(c, 1.0F - 0.75F * (q - 0.65F) / 0.35F);  // dark refracting rim
        if (dy > 0.55F && q > 0.6F && q < 0.9F)
          c = add(c, F3{45, 45, 50});  // caustic under the drop
        const float hx = dx + 0.35F;
        const float hy = dy + 0.4F;
        if (hx * hx + hy * hy < 0.05F)
          c = add(c, F3{150, 150, 150});  // specular highlight
        const float a = std::clamp((1.0F - q) * r * 1.5F, 0.0F, 1.0F);
        const std::size_t i = static_cast<std::size_t>(y) * w + x;
        img[i] = mix(img[i], c, a);
        if (q < 0.6F)  // the water wipes a path narrower than the drop
          clear[i] = 1.0F;
      }
  };

  std::vector<F3> img(clear.size());
  runFrames(renderer,
            w,
            h,
            8000,
            [&](float t, std::vector<Rgb>& dst)
            {
              // Big drops: stick, then slip downward in fits, leaving droplets behind.
              std::vector<Drop> trail;
              for (auto& d : drops)
              {
                if (!d.big || !d.alive || d.born > t)
                  continue;
                if (d.stick > 0.0F)
                {
                  d.stick -= 1.0F;
                  continue;
                }
                d.vy = std::min(3.0F, d.vy + 0.08F + 0.02F * d.r);
                if (u01(rng) < 0.04F)  // snags on the glass
                {
                  d.vy = 0.0F;
                  d.stick = 3.0F + 10.0F * u01(rng);
                }
                const float oy = d.y;
                d.y += d.vy / ya;
                d.drift = std::clamp(d.drift + (u01(rng) - 0.5F) * 0.25F, -0.6F, 0.6F);
                d.x += d.drift;
                if (static_cast<int>(oy / 3.0F) != static_cast<int>(d.y / 3.0F) && u01(rng) < 0.6F)
                {
                  Drop s{d.x + (u01(rng) - 0.5F) * d.r * 0.6F,
                         oy,
                         0.4F + 0.5F * u01(rng) * d.r * 0.35F,
                         0.0F};
                  trail.push_back(s);
                  d.r = std::max(1.8F, d.r - 0.02F);
                }
                for (auto& s : drops)  // swallow the small droplets it runs into
                  if (!s.big && s.alive && s.born <= t)
                  {
                    const float dx = s.x - d.x;
                    const float dy = (s.y - d.y) * ya;
                    if (dx * dx + dy * dy < d.r * d.r)
                    {
                      s.alive = false;
                      d.r = std::min(8.0F, std::sqrt(d.r * d.r + s.r * s.r));
                    }
                  }
                if (d.y - d.r > h)
                  d.alive = false;
              }
              drops.insert(drops.end(), trail.begin(), trail.end());

              const float gloom = smooth01(0.0F, 0.3F, t);
              const float fog = smooth01(0.62F, 0.9F, t);
              const float fade = 1.0F - smooth01(0.93F, 1.0F, t);
              for (std::size_t i = 0; i < img.size(); ++i)
              {
                const int x = static_cast<int>(i % w);
                const int y = static_cast<int>(i / w);
                F3 c = mix(texel(src, w, x, y), soft[i], 0.4F + 0.4F * gloom);
                c = mul(c, 1.0F - 0.3F * gloom);
                c = add(c, mul(F3{8, 10, 16}, gloom));
                const float mist = fog * (1.0F - 0.85F * clear[i]) *
                                   (0.75F + 0.25F * fbm(x * 0.05F, y * 0.05F * ya, 3, 5));
                img[i] = mix(c, add(mul(soft[i], 0.25F), F3{150, 158, 168}), mist);
              }
              for (const auto& d : drops)
                if (d.alive && d.born <= t && !d.big)
                  drawDrop(img, d);
              for (const auto& d : drops)
                if (d.alive && d.born <= t && d.big)
                  drawDrop(img, d);
              for (std::size_t i = 0; i < img.size(); ++i)
                dst[i] = toRgb(mul(img[i], fade));
            });
}

// Hoarfrost. Dendritic ice crystals grow in from the window edges and from
// a few interior seeds, branching at 60 degrees; the frost creeps out from
// every branch and freezes the view under a glittering pale-blue rime.
void effectHoarfrost(
    const Renderer& renderer, const std::vector<Rgb>& src, int w, int h, std::mt19937& rng)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  std::uniform_real_distribution<float> u01(0.0F, 1.0F);
  const std::size_t n = static_cast<std::size_t>(w) * h;
  constexpr float kNever = 1e9F;
  std::vector<float> branch(n, kNever);  // time a crystal branch reaches the pixel
  constexpr float kGrowth = 0.0024F;     // time per isotropic pixel of dendrite

  auto stamp = [&](float x, float y, float tt)
  {
    const int ix = static_cast<int>(x);
    const int iy = static_cast<int>(y);
    if (ix < 0 || ix >= w || iy < 0 || iy >= h)
      return;
    float& b = branch[static_cast<std::size_t>(iy) * w + ix];
    b = std::min(b, tt);
  };
  int budget = 16000;  // growth steps, keeps pathological seeds bounded
  // A frost fern: a nearly straight stem sprouting paired side branches at
  // 60 degrees whose length tapers towards the tip; the side branches do the
  // same one level down.
  std::function<void(float, float, float, float, float, int)> grow =
      [&](float x, float y, float ang, float len, float t0, int depth)
  {
    float tt = t0;
    const float spacing = depth == 0 ? 7.0F : 5.0F;
    float next = spacing * (0.5F + u01(rng));
    for (float s = 0.0F; s < len && budget > 0; s += 1.0F, --budget)
    {
      x += std::cos(ang);
      y += std::sin(ang) / ya;
      ang += (u01(rng) - 0.5F) * (depth == 0 ? 0.03F : 0.06F);
      tt += kGrowth;
      stamp(x, y, tt);
      if (x < -2 || x > w + 1 || y < -2 || y > h + 1)
        return;
      if (depth < 2 && s >= next)
      {
        next += spacing * (0.7F + 0.6F * u01(rng));
        const float taper = (len - s) / len;
        const float sub = len * (depth == 0 ? 0.3F : 0.35F) * taper * (0.6F + 0.5F * u01(rng));
        if (sub > 3.0F)
        {
          grow(x, y, ang + kPi / 3.0F, sub, tt, depth + 1);
          grow(x, y, ang - kPi / 3.0F, sub * (0.7F + 0.5F * u01(rng)), tt, depth + 1);
        }
      }
    }
  };
  const int nEdge = 7 + w / 45;
  for (int i = 0; i < nEdge; ++i)
  {
    const int edge = static_cast<int>(u01(rng) * 4.0F);
    const float p = u01(rng);
    float x = 0;
    float y = 0;
    float ang = 0;
    switch (edge)
    {
      case 0:
        x = p * w;
        y = 0;
        ang = kPi / 2;
        break;
      case 1:
        x = p * w;
        y = h - 1.0F;
        ang = -kPi / 2;
        break;
      case 2:
        x = 0;
        y = p * h;
        ang = 0;
        break;
      default:
        x = w - 1.0F;
        y = p * h;
        ang = kPi;
        break;
    }
    ang += (u01(rng) - 0.5F) * 1.2F;
    grow(x, y, ang, mn * (0.45F + 0.4F * u01(rng)), 0.02F + 0.3F * u01(rng), 0);
  }
  for (int i = 0; i < 3; ++i)  // six-armed stars on the pane
  {
    const float x = (0.15F + 0.7F * u01(rng)) * w;
    const float y = (0.15F + 0.7F * u01(rng)) * h;
    const float t0 = 0.12F + 0.3F * u01(rng);
    const float a0 = u01(rng) * kPi;
    for (int a = 0; a < 6; ++a)
      grow(x, y, a0 + a * kPi / 3.0F, mn * (0.07F + 0.04F * u01(rng)), t0, 1);
  }
  // Frost creeps out from the branches: chamfer distance in arrival time.
  constexpr float kCreep = 0.008F;
  std::vector<float> frost = branch;
  const float dd = kCreep * std::sqrt(1.0F + ya * ya);
  const float dv = kCreep * ya;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
    {
      float& f = frost[static_cast<std::size_t>(y) * w + x];
      if (x > 0)
        f = std::min(f, frost[static_cast<std::size_t>(y) * w + x - 1] + kCreep);
      if (y > 0)
      {
        f = std::min(f, frost[static_cast<std::size_t>(y - 1) * w + x] + dv);
        if (x > 0)
          f = std::min(f, frost[static_cast<std::size_t>(y - 1) * w + x - 1] + dd);
        if (x + 1 < w)
          f = std::min(f, frost[static_cast<std::size_t>(y - 1) * w + x + 1] + dd);
      }
    }
  for (int y = h - 1; y >= 0; --y)
    for (int x = w - 1; x >= 0; --x)
    {
      float& f = frost[static_cast<std::size_t>(y) * w + x];
      if (x + 1 < w)
        f = std::min(f, frost[static_cast<std::size_t>(y) * w + x + 1] + kCreep);
      if (y + 1 < h)
      {
        f = std::min(f, frost[static_cast<std::size_t>(y + 1) * w + x] + dv);
        if (x + 1 < w)
          f = std::min(f, frost[static_cast<std::size_t>(y + 1) * w + x + 1] + dd);
        if (x > 0)
          f = std::min(f, frost[static_cast<std::size_t>(y + 1) * w + x - 1] + dd);
      }
    }
  // Distance to the nearest branch, isotropic pixels: rime is densest there.
  std::vector<float> near(n);
  for (std::size_t i = 0; i < n; ++i)
    near[i] = branch[i] < kNever ? 0.0F : kNever;
  const float ddist = std::sqrt(1.0F + ya * ya);
  for (int pass = 0; pass < 2; ++pass)
    for (int yy = 0; yy < h; ++yy)
      for (int xx = 0; xx < w; ++xx)
      {
        const int y = pass == 0 ? yy : h - 1 - yy;
        const int x = pass == 0 ? xx : w - 1 - xx;
        const int sx = pass == 0 ? -1 : 1;
        float& f = near[static_cast<std::size_t>(y) * w + x];
        if (x + sx >= 0 && x + sx < w)
          f = std::min(f, near[static_cast<std::size_t>(y) * w + x + sx] + 1.0F);
        const int yp = y + sx;
        if (yp >= 0 && yp < h)
        {
          f = std::min(f, near[static_cast<std::size_t>(yp) * w + x] + ya);
          if (x - 1 >= 0)
            f = std::min(f, near[static_cast<std::size_t>(yp) * w + x - 1] + ddist);
          if (x + 1 < w)
            f = std::min(f, near[static_cast<std::size_t>(yp) * w + x + 1] + ddist);
        }
      }
  runFrames(renderer,
            w,
            h,
            7500,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float cold = smooth01(0.0F, 0.6F, t);
              const float fade = 1.0F - smooth01(0.9F, 1.0F, t);
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const std::size_t i = static_cast<std::size_t>(y) * w + x;
                  F3 c = texel(src, w, x, y);
                  const float l = luma(c);
                  c = mix(c, F3{l * 0.8F, l * 0.9F, l * 1.05F}, 0.4F * cold);  // chill
                  const float a = smooth01(frost[i], frost[i] + 0.14F, t);
                  if (a > 0.0F)
                  {
                    const float tex = fbm(x * 0.35F, y * 0.35F * ya, 3, 9);
                    F3 rime{185 + 0.25F * l + 40 * tex, 212 + 0.15F * l + 25 * tex, 240 + 15 * tex};
                    const float tw = hash01(x, y, 3);
                    if (tw > 0.975F && std::sin(t * 25.0F + tw * 400.0F) > 0.4F)
                      rime = add(rime, F3{90, 90, 90});  // glints
                    const float feather = 0.18F + 0.5F * std::exp(-near[i] / 3.0F);
                    c = mix(c, rime, std::min(0.9F, a * feather * (0.7F + 0.6F * tex)));
                  }
                  if (branch[i] <= t)
                    c = mix(c, F3{250, 253, 255}, 0.95F);
                  else if (near[i] < 2.5F && frost[i] <= t)
                    c = mix(c, F3{235, 245, 255}, 0.45F * (1.0F - near[i] / 2.5F));  // glow
                  dst[i] = toRgb(mul(c, fade));
                }
            });
}

// Earthquake. P waves compress the map, slower S waves shear it, then the
// fault ruptures outward from the epicentre and the two sides slip past
// each other. A seismograph along the bottom records the whole event.
void effectEarthquake(
    const Renderer& renderer, const std::vector<Rgb>& src, int w, int h, std::mt19937& rng)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  std::uniform_real_distribution<float> u01(0.0F, 1.0F);
  const float ang = (u01(rng) < 0.5F ? -1.0F : 1.0F) * (0.35F + 0.5F * u01(rng));
  const float Tx = std::cos(ang);
  const float Ty = std::sin(ang);
  const float Nx = -Ty;
  const float Ny = Tx;
  const float p0x = w * (0.4F + 0.2F * u01(rng));
  const float p0y = h * ya * (0.35F + 0.2F * u01(rng));
  const float se = (u01(rng) - 0.5F) * 0.3F * mn;  // epicentre along the fault
  const float ex = p0x + Tx * se;
  const float ey = p0y + Ty * se;
  const float slipSign = u01(rng) < 0.5F ? -1.0F : 1.0F;
  const int seed = static_cast<int>(rng() % 1000U);
  const float far = std::hypot(static_cast<float>(w), h * ya);
  constexpr float kT0 = 0.06F;  // origin time
  constexpr float kRupture = 0.34F;
  const float vp = far / 0.22F;  // screen-crossing speeds, per unit t
  const float vs = vp / 1.73F;
  const float vr = mn * 1.5F / 0.12F;  // rupture front along the fault
  const float slipMax = mn * 0.11F;

  auto jag = [&](float s)
  {
    return (vnoise(s * 0.04F, 0.5F, seed) - 0.5F) * mn * 0.16F +
           (vnoise(s * 0.25F, 2.5F, seed + 1) - 0.5F) * mn * 0.025F;
  };
  auto wavelet = [](float u, float width)
  {
    const float a = u / width;
    return std::sin(a * 2.0F * kPi) * std::exp(-a * a);
  };
  // Ground displacement at isotropic position (X, Y) and time t.
  float shakeX = 0.0F;  // per-frame shake direction, set before the pixel loop
  float shakeY = 0.0F;
  // `along` and `d` are the position along and across the (jagged) fault.
  auto displacement = [&](float X, float Y, float along, float d, float t, float& dx, float& dy)
  {
    dx = dy = 0.0F;
    const float rx = X - ex;
    const float ry = Y - ey;
    const float r = std::sqrt(rx * rx + ry * ry) + 1e-3F;
    const float te = t - kT0;
    if (te > 0.0F)
    {
      const float att = 1.0F / (1.0F + r / mn);
      const float p = mn * 0.03F * att * wavelet(r - te * vp, mn * 0.08F);
      const float s = mn * 0.045F * att * wavelet(r - te * vs, mn * 0.12F);
      dx += rx / r * p - ry / r * s;
      dy += ry / r * p + rx / r * s;
    }
    const float tr = t - kRupture;
    if (tr > 0.0F)
    {
      const float passed = tr - std::fabs(along - se) / vr;
      const float slip = slipMax * smooth01(0.0F, 0.12F, passed);
      const float sideSign = d >= 0.0F ? 1.0F : -1.0F;
      dx += Tx * sideSign * slip * 0.5F * slipSign;
      dy += Ty * sideSign * slip * 0.5F * slipSign;
      // Surface waves and shaking radiating from the rupture.
      const float shake = mn * 0.012F * std::exp(-tr * 7.0F);
      dx += shake * shakeX;  // camera shake: the same for every pixel
      dy += shake * shakeY;
      const float roll = mn * 0.015F * std::exp(-tr * 4.0F) *
                         std::sin((r - tr * vs * 0.8F) / (mn * 0.1F) * 2.0F * kPi) *
                         smooth01(0.0F, mn * 0.2F, tr * vs * 0.8F - r + mn * 0.2F);
      dx += rx / r * roll * 0.3F;
      dy += roll;
    }
  };

  // Seismograph at the bottom of the screen: one sample per frame.
  const float stX = w * 0.5F;
  const float stY = h * ya * 0.95F;
  std::vector<float> trace;
  const int stripH = std::max(8, h / 6);
  const int stripTop = h - stripH;

  // The fault geometry per pixel never changes: compute it once.
  std::vector<float> alongA(static_cast<std::size_t>(w) * h);
  std::vector<float> acrossA(alongA.size());
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
    {
      const float X = static_cast<float>(x);
      const float Y = y * ya;
      const float along = (X - p0x) * Tx + (Y - p0y) * Ty;
      alongA[static_cast<std::size_t>(y) * w + x] = along;
      acrossA[static_cast<std::size_t>(y) * w + x] = (X - p0x) * Nx + (Y - p0y) * Ny - jag(along);
    }

  runFrames(renderer,
            w,
            h,
            7000,
            [&](float t, std::vector<Rgb>& dst)
            {
              shakeX = (vnoise(t * 90.0F, 0.0F, seed + 7) - 0.5F) * 2.0F;
              shakeY = (vnoise(t * 90.0F, 9.0F, seed + 7) - 0.5F) * 2.0F;
              const float tr = t - kRupture;
              const float fade = 1.0F - smooth01(0.88F, 1.0F, t);
              for (int y = 0; y < stripTop; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const float X = static_cast<float>(x);
                  const float Y = y * ya;
                  float dx = 0.0F;
                  float dy = 0.0F;
                  const std::size_t gi = static_cast<std::size_t>(y) * w + x;
                  const float along = alongA[gi];
                  const float d = acrossA[gi];
                  displacement(X, Y, along, d, t, dx, dy);
                  F3 c = bilinear(src, w, h, X - dx, (Y - dy) / ya);
                  if (tr > 0.0F)  // the open crack and its crushed edges
                  {
                    const float passed = tr - std::fabs(along - se) / vr;
                    if (passed > 0.0F)
                    {
                      const float gap = 0.8F + mn * 0.006F * smooth01(0.0F, 0.1F, passed);
                      const float ad = std::fabs(d);
                      if (ad < gap)
                        c = F3{22, 16, 12};
                      else if (ad < gap + 1.5F)
                        c = mix(c, F3{120, 95, 70}, 0.7F);
                      // Dust thrown up along the fresh scarp.
                      const float near = std::exp(-ad / (mn * 0.04F)) * std::exp(-passed * 5.0F);
                      const float dust =
                          near > 0.02F ? near * fbm(X * 0.08F, Y * 0.08F - t * 6.0F, 3, seed + 3)
                                       : 0.0F;
                      c = mix(c, F3{170, 150, 120}, std::clamp(dust * 1.6F, 0.0F, 0.8F));
                    }
                  }
                  dst[static_cast<std::size_t>(y) * w + x] = toRgb(mul(c, fade));
                }
              // Record the station's vertical ground motion.
              float sdx = 0.0F;
              float sdy = 0.0F;
              {
                const float along = (stX - p0x) * Tx + (stY - p0y) * Ty;
                displacement(
                    stX, stY, along, (stX - p0x) * Nx + (stY - p0y) * Ny - jag(along), t, sdx, sdy);
              }
              float amp = (sdx + sdy) / (mn * 0.03F);
              amp += (vnoise(t * 400.0F, 3.0F, seed) - 0.5F) * 0.08F;
              if (tr > 0.0F)
                amp += (vnoise(t * 250.0F, 5.0F, seed) - 0.5F) * 2.5F * std::exp(-tr * 5.0F);
              trace.push_back(std::clamp(amp, -1.0F, 1.0F));
              // Paper, grid and ink.
              const float mid = stripTop + stripH * 0.5F;
              for (int y = stripTop; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  F3 paper{232, 226, 208};
                  if (x % 20 == 0 || y == stripTop)
                    paper = F3{200, 180, 170};
                  dst[static_cast<std::size_t>(y) * w + x] = toRgb(mul(paper, fade));
                }
              const int penX = static_cast<int>(w * 0.85F);
              const int count = static_cast<int>(trace.size());
              for (int k = 1; k < count; ++k)
              {
                const int x = penX - (count - 1 - k) * 2;
                if (x < 1)
                  continue;
                const float ya0 = mid - trace[k - 1] * stripH * 0.45F;
                const float ya1 = mid - trace[k] * stripH * 0.45F;
                const int lo = static_cast<int>(std::min(ya0, ya1));
                const int hi = static_cast<int>(std::max(ya0, ya1));
                for (int y = std::max(stripTop, lo); y <= std::min(h - 1, hi); ++y)
                  for (int xx = x - 2; xx <= x; ++xx)
                    if (xx >= 0)
                      dst[static_cast<std::size_t>(y) * w + xx] = toRgb(mul(F3{40, 30, 90}, fade));
              }
            });
}

// Green flash. The view becomes a sea surface stretching to the horizon;
// the sun, flattened by refraction, sets into it with a glitter path, and
// in the last instant its upper rim turns green. Then dusk and stars.
void effectGreenFlash(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float Yh = h * 0.56F * ya;  // horizon, isotropic units
  const float Rs = mn * 0.075F;
  const float Xs = w * 0.5F;
  constexpr float kSet = 0.7F;  // sun fully set
  auto sunY = [&](float t) { return Yh - 2.8F * Rs + (t / kSet) * 3.9F * Rs; };
  auto squash = [&](float t) { return 1.0F - 0.3F * smooth01(3.0F * Rs, 0.0F, Yh - sunY(t)); };
  // Moment the flattened upper limb touches the horizon.
  float tFlash = kSet;
  for (float tt = 0.0F; tt < 1.0F; tt += 0.001F)
    if (sunY(tt) - Rs * squash(tt) >= Yh)
    {
      tFlash = tt;
      break;
    }
  runFrames(
      renderer,
      w,
      h,
      8000,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float Ys = sunY(t);
        const float sq = squash(t);
        const float dusk = smooth01(tFlash - 0.25F, tFlash + 0.2F, t);
        const float flash = std::exp(-std::pow((t - tFlash) / 0.018F, 2.0F));
        const float intro = smooth01(0.0F, 0.14F, t);
        const float fade = 1.0F - smooth01(0.9F, 1.0F, t);
        const float visible = std::clamp((Yh - (Ys - Rs * sq)) / (2.0F * Rs * sq), 0.0F, 1.0F);
        const F3 zen = mix(F3{50, 100, 185}, F3{8, 12, 40}, dusk);
        const F3 hor = mix(F3{255, 165, 85}, F3{110, 45, 70}, dusk);
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const float X = static_cast<float>(x);
            const float Y = y * ya;
            F3 c;
            if (Y < Yh)  // sky
            {
              const float v = std::pow(Y / Yh, 2.2F);
              c = mix(zen, hor, v);
              const float dsx = X - Xs;
              const float dsy = Y - Ys;
              const float halo = std::exp(-(dsx * dsx + dsy * dsy) / (Rs * Rs * 30.0F));
              c = add(c, mul(F3{255, 140, 60}, halo * 0.6F * (1.0F - dusk * 0.7F)));
              c = add(
                  c,
                  mul(F3{60, 255, 120},
                      flash * 0.5F * std::exp(-(dsx * dsx) / (Rs * Rs * 8.0F) - (Yh - Y) / Rs)));
              if (dusk > 0.5F && Y < Yh * 0.8F && hash01(x, y, 17) > 0.992F)
                c = add(c,
                        mul(F3{200, 200, 220},
                            (dusk - 0.5F) * 2.0F * (0.6F + 0.4F * std::sin(t * 30.0F + x))));
              const float sy = dsy / sq;
              const float q2 = (dsx * dsx + sy * sy) / (Rs * Rs);
              if (q2 < 1.0F)
              {
                const float low = smooth01(4.0F * Rs, 0.0F, Yh - Ys);
                F3 disk = mix(F3{255, 245, 205}, F3{255, 110, 45}, low);
                const float rimTop = sy < 0.0F ? smooth01(0.8F, 1.0F, q2) : 0.0F;
                const float rimBot = sy > 0.0F ? smooth01(0.8F, 1.0F, q2) : 0.0F;
                disk = mix(disk, F3{150, 255, 150}, rimTop * 0.5F * low);  // dispersion
                disk = mix(disk, F3{200, 30, 30}, rimBot * 0.5F * low);
                disk = mix(disk, F3{70, 255, 110}, flash);
                c = mix(c, disk, std::clamp((1.0F - q2) * 8.0F, 0.0F, 1.0F));
              }
            }
            else  // the view as a sea surface in perspective
            {
              const float dy = (Y - Yh) / (h * ya - Yh);
              const float z = 1.0F / (dy + 0.06F);
              const float mv = std::clamp(1.0F - (z - 0.95F) / 17.0F, 0.0F, 1.0F);
              float mu = 0.5F + (X / w - 0.5F) * z * 0.3F;
              mu += 0.004F * z * std::sin(mv * 90.0F + t * 12.0F);
              mu = fract(mu);
              F3 sea = bilinear(src, w, h, mu * (w - 1), mv * (h - 1));
              const F3 light = mix(F3{1.0F, 0.72F, 0.55F}, F3{0.22F, 0.22F, 0.38F}, dusk);
              sea = F3{sea.r * light.r, sea.g * light.g, sea.b * light.b};
              sea = mul(sea, 0.45F + 0.45F * dy);
              sea = mix(sea, mix(mul(zen, 0.7F), hor, 1.0F - dy), 0.35F);  // sky reflected
              sea = mix(sea, hor, std::pow(1.0F - dy, 6.0F) * 0.8F);       // haze
              // Glitter path under the sun.
              const float width = Rs * (0.6F + 3.0F * dy);
              const float g = std::exp(-std::pow((X - Xs) / width, 2.0F));
              const float sparkle =
                  fbm(X * 0.15F / (0.3F + dy), Y * 1.2F / (0.1F + dy) - t * 4.0F, 2, 31);
              if (sparkle > 0.62F)
                sea = add(sea,
                          mul(mix(F3{255, 200, 120}, F3{80, 255, 130}, flash),
                              g * visible * (sparkle - 0.62F) * 5.0F));
              c = sea;
            }
            if (intro < 1.0F)  // the view tilts away into the sea
              c = mix(texel(src, w, x, y), c, intro);
            dst[static_cast<std::size_t>(y) * w + x] = toRgb(mul(c, fade));
          }
      });
}

// Rayleigh-Bénard convection. The view breaks into hexagonal cells: fluid
// wells up in each centre and spreads to the rims where it sinks, so the
// map inside every cell streams outward from its middle, like open-cell
// stratocumulus seen from a satellite.
void effectBenardCells(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float cell = mn / 6.5F;  // lattice spacing, isotropic pixels
  const float rowH = cell * 0.8660254F;
  const std::size_t n = static_cast<std::size_t>(w) * h;
  // Voronoi of a jittered hexagonal lattice: nearest centre and rim distance.
  std::vector<float> ccx(n), ccy(n), rim(n), rad(n);
  const auto centre = [&](int i, int j, float& cx, float& cy)
  {
    cx = (i + ((j & 1) != 0 ? 0.5F : 0.0F)) * cell + (hash01(i, j, 5) - 0.5F) * cell * 0.3F;
    cy = j * rowH + (hash01(i, j, 6) - 0.5F) * cell * 0.3F;
  };
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
    {
      const float X = static_cast<float>(x);
      const float Y = y * ya;
      const int j0 = static_cast<int>(std::floor(Y / rowH));
      const int i0 = static_cast<int>(std::floor(X / cell));
      float d1 = 1e9F, d2 = 1e9F, bx = 0.0F, by = 0.0F;
      for (int j = j0 - 1; j <= j0 + 2; ++j)
        for (int i = i0 - 1; i <= i0 + 2; ++i)
        {
          float cx = 0.0F, cy = 0.0F;
          centre(i, j, cx, cy);
          const float d = std::hypot(X - cx, Y - cy);
          if (d < d1)
          {
            d2 = d1;
            d1 = d;
            bx = cx;
            by = cy;
          }
          else if (d < d2)
            d2 = d;
        }
      const std::size_t k = static_cast<std::size_t>(y) * w + x;
      ccx[k] = bx;
      ccy[k] = by;
      rim[k] = 0.5F * (d2 - d1);  // approximate distance to the cell wall
      rad[k] = d1;
    }
  runFrames(renderer,
            w,
            h,
            7000,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float onset = smooth01(0.0F, 0.25F, t);
              const float spread = std::exp(-2.2F * std::max(0.0F, t - 0.05F));  // outflow
              const float fade = 1.0F - smooth01(0.86F, 1.0F, t);
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const std::size_t k = static_cast<std::size_t>(y) * w + x;
                  const float X = static_cast<float>(x);
                  const float Y = y * ya;
                  // Each cell magnifies what welled up at its centre.
                  const float f = 1.0F + (spread - 1.0F) * onset;
                  const float sx = ccx[k] + (X - ccx[k]) * f;
                  const float sy = (ccy[k] + (Y - ccy[k]) * f) / ya;
                  F3 c = bilinear(src, w, h, sx, sy);
                  const float l = luma(c);
                  // Rising centres are bright cloud, sinking rims are clear and dark.
                  const float up = std::clamp(1.0F - rad[k] / (cell * 0.6F), 0.0F, 1.0F);
                  c = mix(
                      c, F3{l * 0.6F + 110, l * 0.6F + 115, l * 0.6F + 125}, 0.35F * onset * up);
                  const float wall = smooth01(cell * 0.06F, 0.0F, rim[k]) * onset;
                  c = mix(c, F3{20, 32, 55}, 0.7F * wall);
                  c = mul(c, 1.0F + 0.25F * onset * (up - 0.5F));
                  dst[k] = toRgb(mul(c, fade));
                }
            });
}

// Red sprites. Night falls and the view becomes the top of a thunderstorm,
// lit from within by lightning; above it red sprites trail blue tendrils,
// a blue jet shoots up, and an elve ring races across the ionosphere.
void effectRedSprites(
    const Renderer& renderer, const std::vector<Rgb>& src, int w, int h, std::mt19937& rng)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  std::uniform_real_distribution<float> u01(0.0F, 1.0F);
  struct Event
  {
    float t0;
    float x;
    int kind;  // 0 sprite, 1 blue jet, 2 elve
    int seed;
  };
  std::vector<Event> events;
  const std::array<float, 6> times = {0.2F, 0.33F, 0.45F, 0.56F, 0.68F, 0.78F};
  for (std::size_t i = 0; i < times.size(); ++i)
  {
    const int kind = i == 2 ? 2 : (i == 4 ? 1 : 0);
    events.push_back({times[i] + 0.03F * u01(rng),
                      (0.15F + 0.7F * u01(rng)) * w,
                      kind,
                      static_cast<int>(i * 17 + 3)});
  }
  // Cloud top: an anvil plateau with overshooting turrets.
  const auto cloudTop = [&](float x)
  {
    const float turrets = std::pow(fbm(x * 0.02F, 0.5F, 4, 91), 2.0F);
    const float anvil = std::exp(-std::pow((x - w * 0.5F) / (w * 0.3F), 2.0F));
    return h * (0.66F - 0.1F * anvil - 0.3F * turrets);
  };
  std::vector<float> tops(static_cast<std::size_t>(w));
  for (int x = 0; x < w; ++x)
    tops[static_cast<std::size_t>(x)] = cloudTop(static_cast<float>(x));
  runFrames(
      renderer,
      w,
      h,
      7500,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float night = smooth01(0.0F, 0.18F, t);
        const float fade = 1.0F - smooth01(0.9F, 1.0F, t);
        // Lightning inside the cloud: the parent strokes of the sprites.
        float flash = 0.0F;
        float fx = w * 0.5F;
        for (const auto& e : events)
        {
          const float a = t - (e.t0 - 0.015F);
          if (a > 0.0F && a < 0.05F)
          {
            const float k = std::exp(-a / 0.012F) * (0.6F + 0.4F * std::sin(a * 900.0F));
            if (k > flash)
            {
              flash = k;
              fx = e.x;
            }
          }
        }
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const std::size_t k = static_cast<std::size_t>(y) * w + x;
            const F3 view = texel(src, w, x, y);
            const float top = tops[static_cast<std::size_t>(x)];
            F3 c;
            if (y >= top)  // storm cloud, textured by the view
            {
              const float depth = (y - top) / (h - top);
              const float billow = fbm(x * 0.06F, y * ya * 0.06F, 4, 93);
              const float rimLit = std::exp(-(y - top) / 2.5F);  // starlit tops of the turrets
              c = mul(view, 0.12F + 0.25F * billow * billow);
              c = add(c, mul(F3{30, 34, 55}, (1.0F - depth) * (0.5F + billow)));
              c = add(c, mul(F3{45, 50, 75}, rimLit));
              const float dx = (x - fx) / (mn * 0.35F);
              const float dy = (y - top) * ya / (mn * 0.3F);
              const float glow = flash * std::exp(-(dx * dx + dy * dy));
              c = add(c, mul(add(mul(view, 0.8F), F3{120, 120, 160}), glow));
            }
            else  // night sky
            {
              c = F3{3, 4, 14};
              if (hash01(x, y, 29) > 0.993F)
                c = add(c, F3{150, 150, 170});
              for (const auto& e : events)
              {
                const float a = t - e.t0;
                if (a < 0.0F || a > 0.14F)
                  continue;
                const float life = std::exp(-a / 0.04F) * smooth01(0.0F, 0.008F, a);
                const float X = (x - e.x) * 1.0F;
                const float Y = y * ya;
                const float Yt = top * ya;
                if (e.kind == 0)  // sprite: diffuse red crown, tendrils beneath
                {
                  const float core = Yt - mn * 0.42F;
                  const float headR = mn * 0.14F;
                  const float hd = (X * X) / (headR * headR * 2.0F) +
                                   std::pow((Y - core) / (headR * 0.8F), 2.0F);
                  float red = std::exp(-hd) * 1.2F;
                  if (Y > core && Y < Yt)
                  {
                    const float down = (Y - core) / (Yt - core);
                    for (int tt = 0; tt < 7; ++tt)
                    {
                      const float off =
                          (tt - 3) * headR * 0.28F +
                          (vnoise(Y * 0.08F, tt * 3.0F, e.seed) - 0.5F) * headR * 0.8F * down;
                      const float wdt = 1.2F + 1.5F * (1.0F - down);
                      red +=
                          0.8F * std::exp(-std::pow((X - off) / wdt, 2.0F)) * (1.0F - down * 0.7F);
                    }
                    const F3 col = mix(F3{255, 40, 70}, F3{120, 60, 255}, down);
                    c = add(c, mul(col, red * life * 0.9F));
                  }
                  else if (Y <= core)
                    c = add(c, mul(F3{255, 50, 80}, red * life));
                }
                else if (e.kind == 1)  // blue jet: a cone rising from the cloud top
                {
                  const float rise = std::min(1.0F, a / 0.04F) * mn * 0.35F;
                  const float up = Yt - Y;
                  if (up > 0.0F && up < rise)
                  {
                    const float wdt = 1.0F + up * 0.18F;
                    const float b = std::exp(-std::pow(X / wdt, 2.0F)) * (1.0F - up / (mn * 0.4F));
                    c = add(c, mul(F3{70, 110, 255}, b * std::exp(-a / 0.05F) * 1.3F));
                  }
                }
                else  // elve: an expanding ring at the edge of space
                {
                  const float Ye = h * 0.06F * ya;
                  const float rr = a * mn * 18.0F;
                  const float d = std::hypot(X, (Y - Ye) * 4.0F);
                  const float ring = std::exp(-std::pow((d - rr) / (mn * 0.03F), 2.0F));
                  c = add(c, mul(F3{255, 60, 40}, ring * std::exp(-a / 0.04F) * 0.9F));
                }
              }
            }
            dst[k] = toRgb(mul(mix(view, c, night), fade));
          }
      });
}

// The water cycle. The map dries out and evaporates in rising wisps that
// carry its colours; the vapour condenses into a cloud tinted with the
// view's own palette, the cloud darkens, and rain re-forms the map from the
// ground up, freshly wet.
void effectWaterCycle(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  F3 mean;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      mean = add(mean, texel(src, w, x, y));
  mean = mul(mean, 1.0F / (static_cast<float>(w) * h));
  const float cloudBase = h * 0.3F;
  runFrames(
      renderer,
      w,
      h,
      8000,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float dry = smooth01(0.04F, 0.4F, t);
        const float evap = smooth01(0.04F, 0.16F, t) * (1.0F - smooth01(0.42F, 0.58F, t));
        const float condense = smooth01(0.15F, 0.5F, t);
        const float darken = smooth01(0.48F, 0.62F, t);
        const float rain = smooth01(0.58F, 0.64F, t) * (1.0F - smooth01(0.88F, 0.96F, t));
        const float front = h - (h - cloudBase) * smooth01(0.62F, 0.9F, t);  // wet front
        const float fade = 1.0F - smooth01(0.93F, 1.0F, t);
        const float rise = t * h * 1.6F;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const std::size_t k = static_cast<std::size_t>(y) * w + x;
            const F3 view = texel(src, w, x, y);
            const float l = luma(view);
            // Parched ground: bleached and dim.
            F3 c = mix(view, F3{l * 0.5F + 40, l * 0.45F + 35, l * 0.4F + 30}, dry * 0.85F);
            // Wisps of vapour, coloured by the ground they rose from.
            const float v =
                evap > 0.0F
                    ? smooth01(0.6F, 0.8F, fbm(x * 0.045F, (y + rise) * ya * 0.035F, 4, 71)) * evap
                    : 0.0F;
            if (v > 0.0F)
            {
              const F3 below = bilinear(src,
                                        w,
                                        h,
                                        x + 3.0F * std::sin(y * 0.15F + t * 8.0F),
                                        std::min(h - 1.0F, y + h * 0.12F));
              c = mix(c, add(mul(below, 0.5F), F3{120, 125, 130}), v * 0.6F);
            }
            // The cloud deck, in the view's own colours.
            // The deck never reaches below 1.3 cloudBase, so skip the noise there.
            const float billow =
                y < cloudBase * 1.35F ? fbm(x * 0.03F + t * 0.6F, y * ya * 0.045F, 4, 73) : 0.0F;
            const float edge = cloudBase * (0.75F + 0.55F * billow) * (0.3F + 0.7F * condense);
            if (y < edge)
            {
              const float dens = condense * smooth01(edge, edge - h * 0.1F, static_cast<float>(y));
              F3 cloud = mix(add(mul(mean, 0.45F), F3{140, 145, 155}), F3{65, 70, 88}, darken);
              cloud = mul(cloud, 0.75F + 0.45F * billow);
              c = mix(c, cloud, dens);
            }
            // Rain; below the wet front the map is back, glistening at first.
            if (y >= edge)
            {
              const float ripple = h * 0.03F * (vnoise(x * 0.08F, t * 3.0F, 75) - 0.5F);
              const float wet =
                  smooth01(front + ripple - 3.0F, front + ripple + 3.0F, static_cast<float>(y));
              const float sheen = std::exp(-std::max(0.0F, y - front) / (h * 0.08F));
              c = mix(c, add(view, F3{25 * sheen, 30 * sheen, 40 * sheen}), wet);
              const float streak = fract(hash01(x, 0, 77) +
                                         (y * 0.02F - t * 9.0F) * (0.8F + 0.4F * hash01(x, 1, 77)));
              if (hash01(x, 2, 77) > 0.55F && streak < 0.1F)
                c = mix(c, F3{175, 190, 220}, 0.55F * rain);
            }
            dst[k] = toRgb(mul(c, fade));
          }
      });
}

// Mirage. Heat shimmer rises off the bottom of the view, an inferior mirage
// floods it with an upside-down image of the sky, and the view towers and
// wavers until it melts into haze.
void effectMirage(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  runFrames(
      renderer,
      w,
      h,
      7000,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float yh = h * (1.0F - 0.5F * smooth01(0.1F, 0.6F, t));  // mirage line
        const float tower = 1.0F + 0.6F * smooth01(0.45F, 0.85F, t);   // looming stretch
        const float haze = smooth01(0.7F, 0.97F, t);
        const float heat = smooth01(0.0F, 0.3F, t);
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const float below = std::max(0.0F, 1.0F - (yh - y) / (h * 0.25F));
            const float amp = heat * (0.6F + 3.0F * below * below);
            const float sx =
                x + amp * std::sin(y * 0.9F - t * 30.0F + vnoise(x * 0.05F, y * 0.2F, 3) * 6.0F);
            const float sjit = amp * 0.4F * std::sin(x * 0.25F + t * 17.0F);
            F3 c;
            if (y < yh)  // the view, stretched upward from the mirage line
            {
              const float sy = yh - (yh - y) / tower + sjit;
              c = bilinear(src, w, h, sx, sy);
            }
            else  // mirror image, compressed, tinted by the sky it really shows
            {
              const float sy = yh - (y - yh) * 1.6F + sjit;
              c = bilinear(src, w, h, sx, sy);
              c = mix(c, F3{150, 180, 220}, 0.35F + 0.3F * std::min(1.0F, (y - yh) / (h * 0.2F)));
              c = mul(c, 0.9F);
            }
            c = mix(
                c, F3{215, 205, 185}, haze * (0.7F + 0.3F * fbm(x * 0.02F, y * 0.05F - t, 3, 7)));
            c = mul(c, 1.0F - smooth01(0.93F, 1.0F, t));
            dst[static_cast<std::size_t>(y) * w + x] = toRgb(c);
          }
      });
}

// Hurricane. The map spins up into a tropical cyclone: a Rankine vortex with
// inflow drags it into a spiral, cloud bands carried by the same flow wrap
// around a bright eyewall, and the eye stays clear so the map shows through.
// Finally the camera dives into the eye.
void effectHurricaneEye(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float cx = (w - 1) * 0.5F;
  const float cy = (h - 1) * 0.5F;
  const float Rm = mn * 0.1F;  // radius of maximum wind
  const float Vmax = mn * 0.013F;
  FlowMap flow(w, h, Boundary::Open);
  runFrames(renderer,
            w,
            h,
            8000,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float spin = smooth01(0.0F, 0.3F, t);
              flow.step(
                  [&](float x, float y, float& vx, float& vy)
                  {
                    const float dx = x - cx;
                    const float dy = (y - cy) * ya;
                    const float r = std::sqrt(dx * dx + dy * dy) + 1e-3F;
                    const float vt = spin * Vmax * (r < Rm ? r / Rm : std::sqrt(Rm / r));
                    const float vr = r < Rm ? 0.0F : -0.3F * vt;  // inflow
                    // Counter-clockwise on screen (northern hemisphere).
                    vx = vt * dy / r + vr * dx / r;
                    vy = (-vt * dx / r + vr * dy / r) / ya;
                  });
              const float dive = smooth01(0.72F, 0.98F, t);
              const float zoom = 1.0F - 0.8F * dive;
              const float cover = smooth01(0.05F, 0.5F, t);
              const float fade = 1.0F - smooth01(0.93F, 1.0F, t);
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const float px = cx + (x - cx) * zoom;
                  const float py = cy + (y - cy) * zoom;
                  float su = 0.0F;
                  float sv = 0.0F;
                  flow.sampleAt(px, py, su, sv);
                  F3 c = bilinearMirrored(src, w, h, su, sv);
                  const float dx = px - cx;
                  const float dy = (py - cy) * ya;
                  const float r = std::sqrt(dx * dx + dy * dy);
                  // Clouds ride on the flow: texture them by where the air came from.
                  const float sdx = su - cx;
                  const float sdy = (sv - cy) * ya;
                  const float r0 = std::sqrt(sdx * sdx + sdy * sdy) + 1e-3F;
                  const float a0 = std::atan2(sdy, sdx);
                  const float bands = 0.5F + 0.5F * std::sin(3.0F * a0 + 2.5F * std::log(r0 / Rm));
                  const float tex = fbm(su * 0.05F, sv * ya * 0.05F, 3, 51);
                  const float wall = std::exp(-std::pow((r - Rm * 1.15F) / (Rm * 0.45F), 2.0F));
                  const float outer = smooth01(mn * 0.95F, mn * 0.3F, r) * (0.3F + 0.7F * bands);
                  float dens = std::max(wall * 1.2F, outer * 0.9F) * (0.55F + 0.7F * tex);
                  dens *= smooth01(Rm * 0.55F, Rm * 0.95F, r);  // the clear eye
                  dens = std::clamp(dens * cover * 1.3F, 0.0F, 1.0F);
                  // Stadium effect: the inner wall is lit, the outer anvil is shadowed.
                  const float lit = 0.75F + 0.35F * smooth01(Rm * 1.6F, Rm * 0.9F, r) + 0.2F * tex;
                  const F3 cloud = mul(F3{235, 238, 245}, lit);
                  c = mul(c, 1.0F - 0.25F * cover * smooth01(Rm, Rm * 3.0F, r));  // storm gloom
                  c = mix(c, cloud, dens);
                  dst[static_cast<std::size_t>(y) * w + x] = toRgb(mul(c, fade));
                }
            });
}

// Coriolis. The map breaks up into air parcels that are drawn towards a low
// and deflected to the right of their motion, so they wind into it in a
// counter-clockwise spiral, leaving trails, and vanish into the rising core.
void effectCoriolis(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float cx = (w - 1) * 0.5F;
  const float cy = (h - 1) * 0.5F * ya;  // isotropic
  struct Parcel
  {
    float x;
    float y;  // isotropic
    float vx = 0.0F;
    float vy = 0.0F;
    F3 c;
    bool alive = true;
  };
  std::vector<Parcel> parcels;
  for (int y = 0; y < h; y += 2)
    for (int x = 0; x < w; x += 2)
      parcels.push_back({x + 0.5F, (y + 0.5F) * ya, 0.0F, 0.0F, texel(src, w, x, y)});
  const float G = mn * 0.0005F;  // pressure-gradient acceleration, px/frame^2
  constexpr float kF = 0.06F;    // Coriolis parameter, 1/frame
  constexpr float kDrag = 0.018F;
  std::vector<F3> trail(static_cast<std::size_t>(w) * h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      trail[static_cast<std::size_t>(y) * w + x] = texel(src, w, x, y);
  runFrames(renderer,
            w,
            h,
            7500,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float go = smooth01(0.03F, 0.15F, t);
              const float decay = 0.93F - 0.13F * go;
              for (auto& p : trail)
                p = mul(p, decay);
              for (auto& p : parcels)
              {
                if (!p.alive)
                  continue;
                const float dx = p.x - cx;
                const float dy = p.y - cy;
                const float r = std::sqrt(dx * dx + dy * dy) + 1e-3F;
                const float pull = G * go * std::min(1.0F, r / (mn * 0.08F));
                // Northern hemisphere: acceleration f(-vy, vx) in y-down coordinates.
                const float ax = -pull * dx / r - kF * p.vy - kDrag * p.vx;
                const float ay = -pull * dy / r + kF * p.vx - kDrag * p.vy;
                p.vx += ax;
                p.vy += ay;
                p.x += p.vx;
                p.y += p.vy;
                if (r < mn * 0.02F)
                  p.alive = false;
                // Each parcel stands for a 2x2 block of the view: splat it as one.
                const int ix = static_cast<int>(std::floor(p.x - 0.5F));
                const int iy = static_cast<int>(std::floor(p.y / ya - 0.5F));
                const float core = std::exp(-r / (mn * 0.05F));
                const F3 col = add(p.c, mul(F3{200, 200, 220}, core));
                for (int oy = 0; oy < 2; ++oy)
                  for (int ox = 0; ox < 2; ++ox)
                  {
                    const int qx = ix + ox;
                    const int qy = iy + oy;
                    if (qx < 0 || qx >= w || qy < 0 || qy >= h)
                      continue;
                    F3& q = trail[static_cast<std::size_t>(qy) * w + qx];
                    q = mix(q, col, 0.8F);
                  }
              }
              const float fade = 1.0F - smooth01(0.9F, 1.0F, t);
              for (std::size_t i = 0; i < trail.size(); ++i)
                dst[i] = toRgb(mul(trail[i], fade));
            });
}

// Hadley, Ferrel and polar cells. The view becomes a pole-to-pole slice of
// the troposphere and is overturned by the six meridional cells: rising at
// the ITCZ, where thunderstorms tower, sinking over the subtropical deserts.
void effectHadleyCell(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float speed = static_cast<float>(w) / 150.0F;
  // Cell edges in latitude (south to north) and signed strengths.
  static const std::array<float, 7> kEdge = {-90, -60, -30, 0, 30, 60, 90};
  static const std::array<float, 6> kAmp = {0.35F, -0.5F, 1.0F, -1.0F, 0.5F, -0.35F};
  const auto psi = [&](float x, float y)
  {
    const float lat = x / (w - 1) * 180.0F - 90.0F;
    const float z = std::clamp(1.0F - y / (h - 1), 0.0F, 1.0F);
    int c = std::clamp(static_cast<int>((lat + 90.0F) / 30.0F), 0, 5);
    const float f = (lat - kEdge[c]) / 30.0F;
    return kAmp[c] * std::sin(kPi * f) * std::sin(kPi * z);
  };
  const float scale = speed * h / kPi;
  FlowMap flow(w, h, Boundary::Clamp);
  runFrames(
      renderer,
      w,
      h,
      7500,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float go = scale * smooth01(0.0F, 0.12F, t);
        flow.step(
            [&](float x, float y, float& vx, float& vy)
            {
              // u = dpsi/dz, w = -dpsi/dx; screen y runs downward.
              const float u = (psi(x, y - 1.0F) - psi(x, y + 1.0F)) * 0.5F;
              const float wz = -(psi(x + 1.0F, y) - psi(x - 1.0F, y)) * 0.5F;
              vx = go * u * (static_cast<float>(w) / h) * 0.35F;
              vy = -go * wz * 0.8F;
            });
        const float storms = smooth01(0.1F, 0.5F, t);
        const float fade = 1.0F - smooth01(0.88F, 1.0F, t);
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const std::size_t i = static_cast<std::size_t>(y) * w + x;
            F3 c = bilinear(src, w, h, flow.u(i), flow.v(i));
            const float lat = static_cast<float>(x) / (w - 1) * 180.0F - 90.0F;
            const float z = 1.0F - static_cast<float>(y) / (h - 1);
            c = mul(c, 0.75F + 0.25F * z);  // a little darker near the ground
            // ITCZ thunderstorms and storm-track clouds in the rising branches.
            const float itcz = std::exp(-lat * lat / 30.0F);
            const float track = std::exp(-std::pow(std::fabs(lat) - 60.0F, 2.0F) / 40.0F);
            const float tower =
                itcz + track > 0.01F ? fbm(x * 0.08F, y * ya * 0.05F - t * 3.0F, 4, 57) : 0.0F;
            const float cb = itcz * smooth01(0.35F, 0.6F, tower + 0.3F * (1.0F - z)) * storms;
            const float st = track * smooth01(0.5F, 0.7F, tower) * storms * smooth01(0.6F, 0.2F, z);
            c = mix(c, F3{245, 245, 250}, std::clamp(cb + st * 0.7F, 0.0F, 0.95F));
            // The ground: deserts under the sinking branches, green where it rains.
            if (y >= h - 3)
            {
              const float desert = std::exp(-std::pow(std::fabs(lat) - 25.0F, 2.0F) / 60.0F);
              c = mix(F3{50, 110, 50}, F3{215, 180, 110}, desert);
              if (std::fabs(lat) > 70.0F)
                c = F3{235, 240, 250};
            }
            dst[i] = toRgb(mul(c, fade));
          }
      });
}

// Mammatus. The view becomes the underside of an anvil cloud, sagging into
// rows of pouches lit from the side by the setting sun, which slowly fades.
void effectMammatus(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float cell = mn / 6.0F;
  const float rowH = cell * 0.8660254F;
  const std::size_t n = static_cast<std::size_t>(w) * h;
  std::vector<float> H(n);
  runFrames(
      renderer,
      w,
      h,
      7000,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float sag = smooth01(0.05F, 0.5F, t);
        const float dusk = smooth01(0.55F, 0.95F, t);
        // Height field: a hemispherical lobe per jittered hex-lattice site,
        // splatted over its own bounding box.
        std::fill(H.begin(), H.end(), 0.0F);
        const int nj = static_cast<int>(h * ya / rowH) + 2;
        const int ni = static_cast<int>(w / cell) + 2;
        for (int j = -1; j <= nj; ++j)
          for (int i = -1; i <= ni; ++i)
          {
            const float jx = hash01(i, j, 81) - 0.5F;
            const float jy = hash01(i, j, 82) - 0.5F;
            const float px = (i + ((j & 1) != 0 ? 0.5F : 0.0F) + 0.55F * jx) * cell +
                             std::sin(t * 2.0F + i + j) * cell * 0.04F;
            const float py = (j + 0.55F * jy) * rowH;
            const float rad = cell * (0.35F + 0.3F * hash01(i, j, 83));
            const float amp = (0.5F + 0.8F * hash01(i, j, 84)) * rad / cell * sag * cell * 0.9F;
            const int x0 = std::max(0, static_cast<int>(px - rad));
            const int x1 = std::min(w - 1, static_cast<int>(px + rad) + 1);
            const int y0 = std::max(0, static_cast<int>((py - rad) / ya));
            const int y1 = std::min(h - 1, static_cast<int>((py + rad) / ya) + 1);
            for (int y = y0; y <= y1; ++y)
              for (int x = x0; x <= x1; ++x)
              {
                const float dx = x - px;
                const float dy = y * ya - py;
                const float q = (dx * dx + dy * dy) / (rad * rad);
                if (q < 1.0F)
                  H[static_cast<std::size_t>(y) * w + x] += amp * std::pow(1.0F - q, 1.5F);
              }
          }
        const float fade = 1.0F - smooth01(0.93F, 1.0F, t);
        const float lx = 0.75F, ly = 0.35F, lz = 0.56F;  // low sun from the lower right
        const F3 sun = mix(F3{255, 185, 120}, F3{190, 90, 120}, dusk);
        const F3 shade = mix(F3{110, 80, 120}, F3{35, 25, 55}, dusk);
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const std::size_t i = static_cast<std::size_t>(y) * w + x;
            const float hx = 0.5F * (H[i + (x + 1 < w ? 1 : 0)] - H[i - (x > 0 ? 1 : 0)]);
            const float hy = 0.5F * (H[i + (y + 1 < h ? w : 0)] - H[i - (y > 0 ? w : 0)]) / ya;
            const float nlen = std::sqrt(hx * hx + hy * hy + 1.0F);
            const float lit = std::max(0.0F, (-hx * lx - hy * ly + lz) / nlen);
            // The pouches bulge towards the viewer, magnifying the map a little.
            const F3 base = bilinear(src, w, h, x - hx * 2.0F, y - hy * 2.0F / ya);
            const float l = luma(base) / 255.0F;
            F3 c = add(mul(sun, lit * (0.55F + 0.6F * l)),
                       mul(shade, (1.0F - lit) * (0.6F + 0.5F * l)));
            c = mix(c, base, 0.35F * (1.0F - sag));
            c = mix(base, c, smooth01(0.0F, 0.2F, t));
            dst[i] = toRgb(mul(c, fade));
          }
      });
}

// Derecho. A bow echo races across the view: behind its gust front the
// rear-inflow jet blows the map downstream in streaks, a shelf cloud
// darkens the leading edge, and radar colours mark the squall line.
void effectDerecho(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float yc = h * 0.5F;
  // Slower than the front (1.1 w per 0.8 of the run), or the air behind it
  // would pile up against the gust front and crush the map into the line.
  const float V = static_cast<float>(w) / 230.0F;
  FlowMap flow(w, h, Boundary::Open);  // fresh map streams in from upwind
  float frontX = 0.0F;
  const auto front = [&](float y, float fx)
  {
    const float q = (y - yc) / (h * 0.38F);
    return fx + w * 0.2F * std::exp(-q * q);  // the bow's apex leads
  };
  runFrames(renderer,
            w,
            h,
            6500,
            [&](float t, std::vector<Rgb>& dst)
            {
              frontX = w * (-0.3F + 1.1F * t / 0.8F);
              flow.step(
                  [&](float x, float y, float& vx, float& vy)
                  {
                    const float behind = front(y, frontX) - x;
                    const float q = (y - yc) / (h * 0.3F);
                    const float jet = 0.6F + 0.4F * std::exp(-q * q);
                    // Gusts peak just behind the front and subside further back.
                    const float g =
                        smooth01(-4.0F, 12.0F, behind) *
                        (0.25F + 0.75F * std::exp(-std::max(0.0F, behind) / (w * 0.2F)));
                    vx = V * jet * g;
                    vy = V * 0.1F * q * g * std::exp(-std::max(0.0F, behind) / (w * 0.15F)) / ya;
                  });
              const float fade = 1.0F - smooth01(0.88F, 1.0F, t);
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const std::size_t i = static_cast<std::size_t>(y) * w + x;
                  F3 c = bilinearMirrored(src, w, h, flow.u(i), flow.v(i));
                  const float behind = front(static_cast<float>(y), frontX) - x;
                  if (behind > 0.0F)
                  {
                    // Leading convective line, then trailing stratiform rain.
                    const float cellTex = fbm(x * 0.06F - t * 20.0F, y * ya * 0.06F, 4, 63);
                    const float line = std::exp(-behind / (w * 0.035F)) * (0.6F + 0.6F * cellTex);
                    const float strat =
                        smooth01(w * 0.6F, w * 0.05F, behind) * smooth01(0.42F, 0.7F, cellTex);
                    const float s = std::max(line, 0.35F * strat);
                    if (s > 0.12F)
                      c = mix(mul(c, 0.7F), dbzColour(s), std::min(0.85F, s * 1.2F));
                    else
                      c = mul(c, 0.8F);  // gloom under the storm
                  }
                  else if (behind > -w * 0.08F)  // the shelf cloud overhanging the front
                  {
                    const float a = 1.0F + behind / (w * 0.08F);
                    const float striation = 0.8F + 0.2F * std::sin(behind * 0.9F + y * 0.15F);
                    c = mix(c, mul(F3{70, 78, 90}, striation), 0.75F * a * a);
                  }
                  dst[i] = toRgb(mul(c, fade));
                }
            });
}

// Polar vortex. The view wraps around the North Pole as a spinning disc of
// cold air; a planetary wave stretches it, and in a sudden stratospheric
// warming it splits into two daughter vortices that drift apart.
void effectPolarVortex(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float cx = (w - 1) * 0.5F;
  const float cy = (h - 1) * 0.5F;
  const float R = mn * 0.47F;  // the disc, isotropic
  const float Rv = R * 0.45F;  // edge of the vortex
  const float V = mn * 0.011F;
  // Disc position -> view pixel: the view's top row sits at the pole.
  const auto polar = [&](float x, float y, float& sx, float& sy)
  {
    const float dx = x - cx;
    const float dy = (y - cy) * ya;
    const float r = std::sqrt(dx * dx + dy * dy) / R;
    const float a = std::atan2(dx, -dy) / (2.0F * kPi) + 0.5F;
    sx = a * (w - 1);
    sy = std::min(r, 1.0F) * (h - 1);
  };
  FlowMap flow(w, h, Boundary::Clamp);
  runFrames(renderer,
            w,
            h,
            8000,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float split = smooth01(0.4F, 0.72F, t);
              const float sep = Rv * 1.1F * split;
              const float turn = t * 5.0F;
              const float stretch = smooth01(0.2F, 0.45F, t) * (1.0F - split);
              const float c0x = std::cos(turn) * sep;
              const float c0y = std::sin(turn) * sep;
              flow.step(
                  [&](float x, float y, float& vx, float& vy)
                  {
                    const float X = x - cx;
                    const float Y = (y - cy) * ya;
                    float ux = 0.0F;
                    float uy = 0.0F;
                    for (int k = 0; k < 2; ++k)  // two half-vortices, together until the split
                    {
                      const float sgn = k == 0 ? 1.0F : -1.0F;
                      const float dx = X - sgn * c0x;
                      const float dy = Y - sgn * c0y;
                      const float r = std::sqrt(dx * dx + dy * dy) + 1e-3F;
                      const float core = Rv * (1.0F - 0.3F * split);
                      const float vt = 0.5F * V * (r < core ? r / core : core / r);
                      ux += vt * dy / r;
                      uy += -vt * dx / r;
                    }
                    // Wavenumber-2 strain that elongates the vortex before it splits.
                    const float a = turn;
                    const float ca = std::cos(2.0F * a);
                    const float sa = std::sin(2.0F * a);
                    ux += stretch * 0.012F * (X * ca + Y * sa);
                    uy += stretch * 0.012F * (X * sa - Y * ca);
                    const float edge = smooth01(R * 1.02F, R * 0.9F, std::sqrt(X * X + Y * Y));
                    vx = ux * edge;
                    vy = uy * edge / ya;
                  });
              const float intro = smooth01(0.0F, 0.15F, t);
              const float fade = 1.0F - smooth01(0.9F, 1.0F, t);
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const std::size_t i = static_cast<std::size_t>(y) * w + x;
                  const float dx = x - cx;
                  const float dy = (y - cy) * ya;
                  const float r = std::sqrt(dx * dx + dy * dy);
                  F3 c{4, 6, 18};
                  if (r < R)
                  {
                    const float su = flow.u(i);
                    const float sv = flow.v(i);
                    float mx = 0.0F;
                    float my = 0.0F;
                    polar(su, sv, mx, my);
                    c = bilinear(src, w, h, mx, my);
                    // Air that started inside the vortex stays cold.
                    const float sdx = su - cx;
                    const float sdy = (sv - cy) * ya;
                    const float r0 = std::sqrt(sdx * sdx + sdy * sdy);
                    const float cold = smooth01(Rv * 1.05F, Rv * 0.85F, r0);
                    c = mix(c, add(mul(c, 0.4F), F3{60, 90, 190}), 0.55F * cold * intro);
                    c = mix(c, add(mul(c, 0.5F), F3{150, 90, 40}), 0.3F * (1.0F - cold) * split);
                    if (std::fabs(r0 - Rv) < 1.2F)  // the polar-night jet at the vortex edge
                      c = mix(c, F3{255, 240, 200}, 0.6F * intro);
                    for (const float lat : {60.0F, 75.0F})  // graticule
                      if (std::fabs(r - (90.0F - lat) / 90.0F * R * 2.0F) < 0.5F)
                        c = mix(c, F3{200, 210, 230}, 0.35F);
                    c = mul(c, 0.8F + 0.2F * std::sqrt(std::max(0.0F, 1.0F - r * r / (R * R))));
                  }
                  c = mix(texel(src, w, x, y), c, intro);
                  dst[i] = toRgb(mul(c, fade));
                }
            });
}

// Tsunami. The view is the sea surface seen from above. The sea floor
// jolts up, the dome collapses into a ring of long waves, dispersive
// trailing waves follow, and the moving surface refracts the map beneath,
// breaking into foam where it steepens.
void effectTsunami(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float ex = w * 0.38F;
  const float ey = h * 0.42F * ya;
  const float far = std::hypot(std::max(ex, w - ex), std::max(ey, h * ya - ey));
  const std::size_t n = static_cast<std::size_t>(w) * h;
  std::vector<float> H(n);
  // Surface height at distance r from the epicentre, time t.
  const auto height = [&](float r, float ang, float t)
  {
    const float lam = mn * 0.12F;
    const float dome = mn * 0.9F * std::exp(-r * r / (lam * lam)) * smooth01(0.02F, 0.06F, t) *
                       (1.0F - smooth01(0.06F, 0.14F, t));
    const float te = std::max(0.0F, t - 0.08F);
    const float front = te * far * 1.25F;
    const float u = (r - front) / lam;
    const float spread = 1.0F / std::sqrt(1.0F + r / (mn * 0.15F));
    float z = mn * 0.35F * spread * std::exp(-u * u) * (te > 0.0F ? 1.0F : 0.0F);
    // Dispersive tail: shorter waves lagging behind the leading crest.
    if (r < front)
    {
      const float behind = (front - r) / lam;
      z += mn * 0.12F * spread * std::sin(behind * behind * 0.6F + behind * 1.5F) *
           std::exp(-behind * 0.35F) *
           (1.0F + (fbm(std::cos(ang) * 2.0F + 5.0F, std::sin(ang) * 2.0F, 3, 47) - 0.5F) *
                       smooth01(0.0F, 2.0F * lam, r));
    }
    return (dome + z) * 0.011F;
  };
  runFrames(renderer,
            w,
            h,
            7000,
            [&](float t, std::vector<Rgb>& dst)
            {
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const float r = std::hypot(x - ex, y * ya - ey);
                  const float ang = std::atan2(y * ya - ey, x - ex);
                  H[static_cast<std::size_t>(y) * w + x] = height(r, ang, t);
                }
              const float fade = 1.0F - smooth01(0.9F, 1.0F, t);
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const std::size_t i = static_cast<std::size_t>(y) * w + x;
                  const float hx = 0.5F * (H[i + (x + 1 < w ? 1 : 0)] - H[i - (x > 0 ? 1 : 0)]);
                  const float hy =
                      0.5F * (H[i + (y + 1 < h ? w : 0)] - H[i - (y > 0 ? w : 0)]) / ya;
                  // Refraction through the tilted surface.
                  F3 c = bilinearMirrored(src, w, h, x - hx * 220.0F, y - hy * 220.0F / ya);
                  c = mix(c, F3{20, 60, 110}, 0.15F);
                  const float lit = std::clamp(1.0F + (-hx * 0.6F - hy * 0.6F) * 14.0F, 0.5F, 1.6F);
                  c = mul(c, lit);
                  const float slope = std::sqrt(hx * hx + hy * hy);
                  const float foam = smooth01(0.06F, 0.1F, slope) *
                                     (0.6F + 0.4F * fbm(x * 0.2F, y * ya * 0.2F, 2, 45));
                  c = mix(c, F3{240, 248, 255}, std::min(0.9F, foam));
                  dst[i] = toRgb(mul(c, fade));
                }
            });
}

// Fogbow. Fog banks roll in over the view and grey it out; with the sun at
// the observer's back a broad white bow forms, faintly red outside and blue
// inside, and a glory rings the observer's shadow at the antisolar point.
void effectFogbow(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float ax = w * 0.5F;  // antisolar point, isotropic
  const float ay = h * 0.78F * ya;
  const float bowR = mn * 0.62F;  // 41 degrees scaled to the screen
  runFrames(
      renderer,
      w,
      h,
      7500,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float roll = smooth01(0.0F, 0.45F, t);
        const float bow = smooth01(0.35F, 0.6F, t);
        const float fade = 1.0F - smooth01(0.9F, 1.0F, t);
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const float X = static_cast<float>(x);
            const float Y = y * ya;
            F3 c = texel(src, w, x, y);
            // Fog advancing from the right in drifting banks.
            const float drift = fbm(X * 0.02F - t * 2.5F, Y * 0.04F, 4, 21);
            const float edge = w * (1.1F - 1.5F * roll);
            const float dens =
                std::clamp(smooth01(edge - 30.0F, edge + 30.0F, X + (drift - 0.5F) * 80.0F) *
                               (0.75F + 0.35F * drift),
                           0.0F,
                           0.92F);
            const F3 fogCol{205 + 20 * drift, 210 + 20 * drift, 218 + 15 * drift};
            c = mix(c, fogCol, dens);
            // The bow: droplets too small for colours, so white with faint fringes.
            const float r = std::hypot(X - ax, Y - ay);
            const float d = (r - bowR) / (mn * 0.06F);
            const float band = std::exp(-d * d);
            if (band > 0.01F && Y < ay)
            {
              F3 tint{255, 255, 255};
              tint = mix(tint, F3{255, 190, 170}, smooth01(0.3F, 1.2F, d) * 0.6F);
              tint = mix(tint, F3{175, 200, 255}, smooth01(-0.3F, -1.2F, d) * 0.6F);
              const float super = 0.5F + 0.5F * std::cos(std::max(0.0F, -d - 1.2F) * 5.0F);
              const float a = band * bow * dens * (d < -1.2F ? 0.4F * super : 0.75F) *
                              smooth01(ay, ay - mn * 0.15F, Y);  // the bow's feet fade into the fog
              c = mix(c, tint, a);
            }
            // Glory: coloured rings around the observer's shadow.
            const float g = std::hypot(X - ax, Y - ay) / (mn * 0.05F);
            if (g < 3.2F)
            {
              const float hue = fract(g * 0.9F);
              const F3 ring = hue < 0.33F ? F3{150, 190, 255}
                                          : (hue < 0.66F ? F3{190, 255, 190} : F3{255, 180, 150});
              const float a = bow * dens * 0.5F * smooth01(3.2F, 1.0F, g) * smooth01(0.6F, 1.0F, g);
              c = mix(c, ring, a);
              if (g < 0.8F)  // the observer's shadow cast on the fog
                c = mix(c, F3{90, 95, 105}, bow * dens * 0.6F * smooth01(0.8F, 0.3F, g));
            }
            dst[static_cast<std::size_t>(y) * w + x] = toRgb(mul(c, fade));
          }
      });
}

}  // namespace ee_detail
}  // namespace Qdless
