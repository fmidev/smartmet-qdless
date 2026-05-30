#include "QdlessExitEffectCommon.h"

namespace Qdless
{
namespace ee_detail
{

void effectRubik(
    const Renderer& renderer, const std::vector<Rgb>& src, int w, int h, std::mt19937& rng)
{
  const float ya = yAspectFor(renderer);
  const float ccx = (w - 1) * 0.5F;
  const float ccy = (h - 1) * 0.5F;

  // Render a solved isometric cube into a target buffer: three visible faces,
  // each a 3x3 grid of one colour separated by dark gridlines.
  std::vector<Rgb> cube(static_cast<std::size_t>(w) * h, kBlank);
  {
    const float a = std::min(0.22F * static_cast<float>(w), 0.34F * static_cast<float>(h));
    const float b = a * 0.42F;    // iso depth (rows)
    const float hgt = a * 0.85F;  // face height (rows)
    const float topY = ccy - hgt * 0.5F;
    const Rgb grid{15, 15, 15, false};
    auto face = [&](float ox, float oy, float ux, float uy, float vx, float vy, Rgb col)
    {
      const float det = ux * vy - uy * vx;
      if (std::fabs(det) < 1e-3F)
        return;
      const float xs[4] = {ox, ox + ux, ox + vx, ox + ux + vx};
      const float ys[4] = {oy, oy + uy, oy + vy, oy + uy + vy};
      int xmin = w;
      int xmax = -1;
      int ymin = h;
      int ymax = -1;
      for (int i = 0; i < 4; ++i)
      {
        xmin = std::min(xmin, static_cast<int>(std::floor(xs[i])));
        xmax = std::max(xmax, static_cast<int>(std::ceil(xs[i])));
        ymin = std::min(ymin, static_cast<int>(std::floor(ys[i])));
        ymax = std::max(ymax, static_cast<int>(std::ceil(ys[i])));
      }
      xmin = std::max(0, xmin);
      xmax = std::min(w - 1, xmax);
      ymin = std::max(0, ymin);
      ymax = std::min(h - 1, ymax);
      for (int y = ymin; y <= ymax; ++y)
        for (int x = xmin; x <= xmax; ++x)
        {
          const float px = x - ox;
          const float py = y - oy;
          const float s = (px * vy - py * vx) / det;
          const float u = (-px * uy + py * ux) / det;
          if (s < 0.0F || s > 1.0F || u < 0.0F || u > 1.0F)
            continue;
          const float gs = s * 3.0F - std::floor(s * 3.0F);
          const float gu = u * 3.0F - std::floor(u * 3.0F);
          constexpr float inset = 0.12F;
          const bool line = gs < inset || gs > 1 - inset || gu < inset || gu > 1 - inset;
          cube[static_cast<std::size_t>(y) * w + x] = line ? grid : col;
        }
    };
    face(ccx - a, topY - b, a, -b, a, b, Rgb{240, 240, 240, false});   // top  (white)
    face(ccx - a, topY - b, a, b, 0.0F, hgt, Rgb{0, 158, 72, false});  // left (green)
    face(ccx, topY, a, -b, 0.0F, hgt, Rgb{200, 30, 30, false});        // right(red)
  }

  // Each cube pixel becomes a particle: a random start, a colour picked from
  // the view there (so the view flies together), a juggle orbit, and its home
  // in the cube.
  struct P
  {
    float sx, sy, omega, phase, amp, tx, ty;
    Rgb view, cube;
  };
  std::vector<P> parts;
  std::uniform_real_distribution<float> dPx(0.0F, static_cast<float>(w));
  std::uniform_real_distribution<float> dPy(0.0F, static_cast<float>(h));
  std::uniform_real_distribution<float> dOm(4.0F, 9.0F);
  std::uniform_real_distribution<float> dPh(0.0F, 6.283F);
  std::uniform_real_distribution<float> dAmp(0.05F, 0.16F);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
    {
      const Rgb& cc = cube[static_cast<std::size_t>(y) * w + x];
      if (cc.transparent)
        continue;
      const float px = dPx(rng);
      const float py = dPy(rng);
      Rgb vc = sample(src, w, h, px, py);
      if (vc.transparent)
        vc = Rgb{200, 200, 200, false};
      parts.push_back(P{px,
                        py,
                        dOm(rng),
                        dPh(rng),
                        dAmp(rng) * w,
                        static_cast<float>(x),
                        static_cast<float>(y),
                        vc,
                        cc});
    }

  runFrames(renderer,
            w,
            h,
            5000,
            [&](float t, std::vector<Rgb>& dst)
            {
              // Base: the view, fading out over the first third.
              if (t < 0.35F)
              {
                const float k = 1.0F - t / 0.35F;
                for (std::size_t i = 0; i < src.size(); ++i)
                {
                  const Rgb& c = src[i];
                  if (c.transparent)
                    continue;
                  dst[i] = Rgb{static_cast<std::uint8_t>(c.r * k),
                               static_cast<std::uint8_t>(c.g * k),
                               static_cast<std::uint8_t>(c.b * k),
                               false};
                }
              }
              const float fade = t < 0.93F ? 1.0F : std::max(0.0F, 1.0F - (t - 0.93F) / 0.07F);
              for (const auto& p : parts)
              {
                float x = 0;
                float y = 0;
                Rgb col;
                if (t < 0.5F)
                {
                  // Juggle: arc around the start, amplitude swelling then settling.
                  const float amp = p.amp * std::sin(3.14159F * t / 0.5F);
                  const float ang = p.phase + t * p.omega;
                  x = p.sx + amp * std::cos(ang);
                  y = p.sy + amp * 0.6F * std::sin(2.0F * ang) / ya;
                  col = p.view;
                }
                else
                {
                  // Converge to the cube, crossfading view -> cube colour.
                  const float e0 = std::min(1.0F, (t - 0.5F) / 0.33F);
                  const float e = 1.0F - (1.0F - e0) * (1.0F - e0) * (1.0F - e0);
                  x = p.sx + (p.tx - p.sx) * e;
                  y = p.sy + (p.ty - p.sy) * e;
                  col = Rgb{static_cast<std::uint8_t>(p.view.r + (p.cube.r - p.view.r) * e),
                            static_cast<std::uint8_t>(p.view.g + (p.cube.g - p.view.g) * e),
                            static_cast<std::uint8_t>(p.view.b + (p.cube.b - p.view.b) * e),
                            false};
                }
                const int ix = static_cast<int>(std::lround(x));
                const int iy = static_cast<int>(std::lround(y));
                if (ix < 0 || ix >= w || iy < 0 || iy >= h)
                  continue;
                dst[static_cast<std::size_t>(iy) * w + ix] =
                    (fade >= 1.0F) ? col
                                   : Rgb{static_cast<std::uint8_t>(col.r * fade),
                                         static_cast<std::uint8_t>(col.g * fade),
                                         static_cast<std::uint8_t>(col.b * fade),
                                         false};
              }
            });
}

void effectFlatland(
    const Renderer& renderer, const std::vector<Rgb>& src, int w, int h, std::mt19937& rng)
{
  const float ya = yAspectFor(renderer);
  const float cx = (w - 1) * 0.5F;
  const float cy = (h - 1) * 0.5F;
  const float halfU = (w - 1) * 0.5F;
  const float halfV = (h - 1) * 0.5F;
  const float dCam = static_cast<float>(h);  // camera distance == focal: identity at theta 0

  // Inhabitants of the line: small dots that crawl along it at varied speeds.
  constexpr int kDots = 8;
  struct Dot
  {
    float x0;
    float dir;
    float spd;  // widths traversed over the flat phase
    Rgb col;
  };
  std::uniform_real_distribution<float> u01(0.0F, 1.0F);
  std::uniform_real_distribution<float> uspd(0.5F, 1.6F);
  std::array<Dot, kDots> dots{};
  for (auto& d : dots)
  {
    d.x0 = u01(rng) * w;
    d.dir = u01(rng) < 0.5F ? -1.0F : 1.0F;
    d.spd = uspd(rng);
    d.col = Rgb{static_cast<std::uint8_t>(205 + static_cast<int>(u01(rng) * 50)),
                static_cast<std::uint8_t>(205 + static_cast<int>(u01(rng) * 50)),
                static_cast<std::uint8_t>(205 + static_cast<int>(u01(rng) * 50)),
                false};
  }

  int tw = 0;
  int th = 0;
  const std::vector<char> title = buildTextBitmap({"FLATLAND"}, tw, th);

  constexpr float kTiltEnd = 0.40F;
  const float thetaMax = 1.52F;  // ~87 deg: thin enough to read as a line

  runFrames(renderer,
            w,
            h,
            3600,
            [&](float t, std::vector<Rgb>& dst)
            {
              std::fill(dst.begin(), dst.end(), Rgb{0, 0, 0, false});  // the flatland void

              if (t < kTiltEnd)
              {
                // Tilt the image back about the horizontal centre axis. Inverse map:
                // for each screen row solve the plane coord pv, then sample per column.
                const float theta = (t / kTiltEnd) * thetaMax;
                const float c = std::cos(theta);
                const float s = std::sin(theta);
                for (int y = 0; y < h; ++y)
                {
                  const float yPhys = (y - cy) * ya;
                  const float denom = c * dCam - yPhys * s;
                  if (std::fabs(denom) < 1e-3F)
                    continue;
                  const float pvPhys = yPhys * dCam / denom;
                  const float pvRow = pvPhys / ya;
                  if (std::fabs(pvRow) > halfV)
                    continue;  // off the tilted card -> void
                  const float depth = dCam + pvPhys * s;
                  if (depth <= 1e-3F)
                    continue;
                  const float sy = cy + pvRow;
                  for (int x = 0; x < w; ++x)
                  {
                    const float pu = (x - cx) * depth / dCam;
                    if (std::fabs(pu) > halfU)
                      continue;
                    const Rgb px = sample(src, w, h, cx + pu, sy);
                    if (!px.transparent)
                      dst[static_cast<std::size_t>(y) * w + x] = px;
                  }
                }
                return;
              }

              // Flat: a line (the collapsed world), crawling dots, and the title.
              const float q = (t - kTiltEnd) / (1.0F - kTiltEnd);
              const int lineY = static_cast<int>(std::lround(cy));
              // The line keeps the original centre row's colours, brightened so it
              // always reads as a lit edge.
              for (int dy = 0; dy <= 1; ++dy)
              {
                const int yy = lineY + dy;
                if (yy < 0 || yy >= h)
                  continue;
                for (int x = 0; x < w; ++x)
                {
                  Rgb c = sample(src, w, h, static_cast<float>(x), cy);
                  if (c.transparent)
                    c = Rgb{120, 120, 130, false};
                  dst[static_cast<std::size_t>(yy) * w + x] =
                      Rgb{static_cast<std::uint8_t>(c.r + (255 - c.r) * 0.4F),
                          static_cast<std::uint8_t>(c.g + (255 - c.g) * 0.4F),
                          static_cast<std::uint8_t>(c.b + (255 - c.b) * 0.4F),
                          false};
                }
              }
              // Dots crawl along the line (wrapping at the edges).
              for (const auto& d : dots)
              {
                float dx = d.x0 + d.dir * d.spd * q * w;
                dx = std::fmod(dx, static_cast<float>(w));
                if (dx < 0)
                  dx += w;
                const int ix = static_cast<int>(dx);
                for (int dy = -1; dy <= 1; ++dy)
                  for (int ddx = 0; ddx <= 1; ++ddx)
                  {
                    const int xx = (ix + ddx) % w;
                    const int yy = lineY + dy;
                    if (yy >= 0 && yy < h)
                      dst[static_cast<std::size_t>(yy) * w + xx] = d.col;
                  }
              }
              // "FLATLAND" fades in and settles above the line.
              const float appear = std::clamp((q - 0.10F) / 0.30F, 0.0F, 1.0F);
              if (appear > 0.0F && tw > 0)
              {
                const int scale = std::max(
                    1, std::min(static_cast<int>(w * 0.8F / tw), static_cast<int>(h * 0.18F / th)));
                const int ox = static_cast<int>(std::lround((w - tw * scale) * 0.5F));
                const int oyBase = static_cast<int>(std::lround(cy - h * 0.18F - th * scale));
                const int oy = oyBase + static_cast<int>(std::lround((1.0F - appear) * h * 0.06F));
                const auto b = static_cast<std::uint8_t>(std::lround(255.0F * appear));
                const Rgb tcol{b, b, b, false};
                for (int by = 0; by < th; ++by)
                  for (int bx = 0; bx < tw; ++bx)
                  {
                    if (title[static_cast<std::size_t>(by) * tw + bx] == 0)
                      continue;
                    for (int sy = 0; sy < scale; ++sy)
                      for (int sx = 0; sx < scale; ++sx)
                      {
                        const int xx = ox + bx * scale + sx;
                        const int yy = oy + by * scale + sy;
                        if (xx >= 0 && xx < w && yy >= 0 && yy < h)
                          dst[static_cast<std::size_t>(yy) * w + xx] = tcol;
                      }
                  }
              }
            });
}

void effectGameOfLife(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const int csz = 2;
  const int gw = std::max(8, w / csz);
  const int gh = std::max(8, h / csz);
  std::vector<char> cur(static_cast<std::size_t>(gw) * gh, 0);
  std::vector<char> nxt(cur.size(), 0);
  std::vector<Rgb> col(cur.size());
  auto luma = [](const Rgb& c) { return 0.30F * c.r + 0.59F * c.g + 0.11F * c.b; };
  for (int gy = 0; gy < gh; ++gy)
    for (int gx = 0; gx < gw; ++gx)
    {
      const Rgb c = sample(src, w, h, (gx + 0.5F) * w / gw, (gy + 0.5F) * h / gh);
      const std::size_t i = static_cast<std::size_t>(gy) * gw + gx;
      col[i] = c.transparent ? Rgb{120, 220, 140, false} : c;
      cur[i] = (!c.transparent && luma(c) > 135.0F) ? 1 : 0;
    }
  auto step = [&]()
  {
    for (int gy = 0; gy < gh; ++gy)
      for (int gx = 0; gx < gw; ++gx)
      {
        int n = 0;
        for (int dy = -1; dy <= 1; ++dy)
          for (int dx = -1; dx <= 1; ++dx)
          {
            if (dx == 0 && dy == 0)
              continue;
            n += cur[static_cast<std::size_t>((gy + dy + gh) % gh) * gw + (gx + dx + gw) % gw];
          }
        const std::size_t i = static_cast<std::size_t>(gy) * gw + gx;
        const bool alive = cur[i] != 0 ? (n == 2 || n == 3) : (n == 3);
        nxt[i] = alive ? 1 : 0;
      }
    cur.swap(nxt);
  };
  int lastGen = -1;
  constexpr int kGens = 70;
  runFrames(renderer,
            w,
            h,
            3600,
            [&](float t, std::vector<Rgb>& dst)
            {
              const int target = static_cast<int>(t * kGens);
              while (lastGen < target)
              {
                step();
                ++lastGen;
              }
              const float fade = std::clamp(1.0F - (t - 0.85F) / 0.15F, 0.0F, 1.0F);
              std::fill(dst.begin(), dst.end(), Rgb{0, 0, 0, false});
              for (int gy = 0; gy < gh; ++gy)
                for (int gx = 0; gx < gw; ++gx)
                {
                  const std::size_t i = static_cast<std::size_t>(gy) * gw + gx;
                  if (cur[i] == 0)
                    continue;
                  const Rgb c = col[i];
                  const Rgb lit{static_cast<std::uint8_t>(c.r * fade),
                                static_cast<std::uint8_t>(c.g * fade),
                                static_cast<std::uint8_t>(c.b * fade),
                                false};
                  for (int dy = 0; dy < csz; ++dy)
                    for (int dx = 0; dx < csz; ++dx)
                    {
                      const int xx = gx * csz + dx;
                      const int yy = gy * csz + dy;
                      if (xx < w && yy < h)
                        dst[static_cast<std::size_t>(yy) * w + xx] = lit;
                    }
                }
            });
}

void effectSolarSystem(
    const Renderer& renderer, const std::vector<Rgb>& src, int w, int h, std::mt19937& rng)
{
  const float ya = yAspectFor(renderer);
  const float cx = (w - 1) * 0.5F;
  const float cy = (h - 1) * 0.5F;
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float rSun = 0.11F * mn;
  const float tiltY = 0.34F;
  constexpr int kN = 6;
  struct Planet
  {
    float orbit;
    float size;
    float spd;
    float phase;
    float spin;
  };
  std::uniform_real_distribution<float> usz(0.22F, 0.5F);
  std::uniform_real_distribution<float> uph(0.0F, 6.2832F);
  std::uniform_real_distribution<float> uspin(0.5F, 2.5F);
  std::array<Planet, kN> p{};
  for (int i = 0; i < kN; ++i)
  {
    p[i].orbit = rSun * 2.0F + (mn * 0.5F - rSun * 2.0F) * ((i + 1.0F) / kN);
    p[i].size = rSun * usz(rng);
    p[i].spd = 1.0F / std::pow(p[i].orbit / rSun, 1.5F);  // Kepler: outer = slower
    p[i].phase = uph(rng);
    p[i].spin = uspin(rng);
  }
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };

  runFrames(
      renderer,
      w,
      h,
      7000,
      [&](float t, std::vector<Rgb>& dst)
      {
        std::fill(dst.begin(), dst.end(), Rgb{0, 0, 0, false});
        const float collapse = std::clamp(t / 0.14F, 0.0F, 1.0F);
        const float spawn = std::clamp((t - 0.14F) / 0.16F, 0.0F, 1.0F);
        const float orbT = std::max(0.0F, t - 0.14F);
        const float sunR = collapse < 1.0F ? (0.5F * w) + (rSun - 0.5F * w) * collapse : rSun;
        auto angle = [&](int i) { return p[i].phase + p[i].spd * orbT * 8.0F; };

        if (spawn > 0.05F)  // faint orbit rings
        {
          const Rgb orb{38, 40, 54, false};
          for (int i = 0; i < kN; ++i)
            for (float a = 0.0F; a < 6.2832F; a += 0.05F)
            {
              const int ox = static_cast<int>(cx + p[i].orbit * spawn * std::cos(a));
              const int oy = static_cast<int>(cy + p[i].orbit * spawn * std::sin(a) * tiltY);
              if (ox >= 0 && ox < w && oy >= 0 && oy < h)
                dst[static_cast<std::size_t>(oy) * w + ox] = orb;
            }
        }
        if (collapse > 0.5F)  // warm corona glow behind the sun
        {
          const float gR = sunR * 2.1F;
          const int gy0 = std::max(0, static_cast<int>(cy - gR / ya));
          const int gy1 = std::min(h - 1, static_cast<int>(cy + gR / ya));
          const int gx0 = std::max(0, static_cast<int>(cx - gR));
          const int gx1 = std::min(w - 1, static_cast<int>(cx + gR));
          for (int y = gy0; y <= gy1; ++y)
            for (int x = gx0; x <= gx1; ++x)
            {
              const float dx = x - cx;
              const float dy = (y - cy) * ya;
              const float d = std::sqrt(dx * dx + dy * dy);
              if (d > gR)
                continue;
              const float k = (1.0F - d / gR) * (1.0F - d / gR) * 0.6F * collapse;
              const std::size_t idx = static_cast<std::size_t>(y) * w + x;
              const Rgb c = dst[idx];
              dst[idx] =
                  Rgb{u8(c.r + 255.0F * k), u8(c.g + 200.0F * k), u8(c.b + 90.0F * k), false};
            }
        }
        auto drawPlanet = [&](int i)
        {
          const float th = angle(i);
          const float orb = p[i].orbit * spawn;
          const float px = cx + orb * std::cos(th);
          const float py = cy + orb * std::sin(th) * tiltY;
          const float sz = p[i].size * spawn;
          drawSphere(dst, w, h, src, px, py, sz, sz / ya, 1.0F, t * p[i].spin, 1.0F, 1.0F, 1.0F);
        };
        if (spawn > 0.0F)
          for (int i = 0; i < kN; ++i)
            if (std::sin(angle(i)) < 0.0F)  // behind the sun
              drawPlanet(i);
        drawSphere(
            dst, w, h, src, cx, cy, sunR, sunR / ya, 0.5F * collapse, t * 0.4F, 1.0F, 0.82F, 0.55F);
        if (spawn > 0.0F)
          for (int i = 0; i < kN; ++i)
            if (std::sin(angle(i)) >= 0.0F)  // in front of the sun
              drawPlanet(i);
      });
}

void effectSaturn(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float cx = (w - 1) * 0.5F;
  const float cy = (h - 1) * 0.5F;
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float rp = 0.15F * mn;
  const float rIn = 1.3F * rp;
  const float rOut = 2.4F * rp;
  const float gapC = 1.85F * rp;
  const float gapW = 0.08F * rp;
  const float tiltY = 0.42F;
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };

  runFrames(
      renderer,
      w,
      h,
      6000,
      [&](float t, std::vector<Rgb>& dst)
      {
        std::fill(dst.begin(), dst.end(), Rgb{0, 0, 0, false});
        const float reveal = std::clamp(t / 0.18F, 0.0F, 1.0F);
        const float spin = t * 6.0F;
        const float ringSpin = t * 2.5F;

        auto drawRing = [&](bool front)
        {
          const int rx = static_cast<int>(rOut);
          const int ry = static_cast<int>(rOut * tiltY / ya) + 1;
          for (int y = std::max(0, static_cast<int>(cy) - ry);
               y <= std::min(h - 1, static_cast<int>(cy) + ry);
               ++y)
            for (int x = std::max(0, static_cast<int>(cx) - rx);
                 x <= std::min(w - 1, static_cast<int>(cx) + rx);
                 ++x)
            {
              const float dyX = (y - cy) * ya;
              if (front ? (dyX < 0.0F) : (dyX >= 0.0F))
                continue;
              const float dxX = x - cx;
              const float planeZ = dyX / tiltY;
              const float rho = std::sqrt(dxX * dxX + planeZ * planeZ);
              if (rho < rIn || rho > rOut || std::fabs(rho - gapC) < gapW)
                continue;
              float az = (std::atan2(planeZ, dxX) + ringSpin) / 6.2831853F;
              az -= std::floor(az);
              const float rho01 = (rho - rIn) / (rOut - rIn);
              const Rgb tex = sample(src, w, h, az * (w - 1), rho01 * (h - 1));
              const float cr = tex.transparent ? 120.0F : tex.r;
              const float cg = tex.transparent ? 120.0F : tex.g;
              const float cb = tex.transparent ? 130.0F : tex.b;
              const float band = (0.5F + 0.5F * std::sin(rho * 0.55F)) * reveal;
              dst[static_cast<std::size_t>(y) * w + x] =
                  Rgb{u8(cr * band * 0.95F), u8(cg * band * 0.86F), u8(cb * band * 0.62F), false};
            }
        };

        if (reveal < 1.0F)  // collapse: the view shrinks into the planet
        {
          const float rdraw = (0.5F * w) + (rp - 0.5F * w) * reveal;
          drawSphere(dst, w, h, src, cx, cy, rdraw, rdraw / ya, reveal, spin, 0.95F, 0.85F, 0.62F);
          return;
        }
        drawRing(false);  // far half of the rings, behind the planet
        drawSphere(dst, w, h, src, cx, cy, rp, rp / ya, 1.0F, spin, 0.95F, 0.85F, 0.62F);
        drawRing(true);  // near half, in front
      });
}

void effectBlackHole(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float cx = (w - 1) * 0.5F;
  const float cy = (h - 1) * 0.5F;
  const float maxR = std::sqrt(cx * cx + (cy * ya) * (cy * ya));
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(
      renderer,
      w,
      h,
      4200,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float rh = t * t * maxR * 0.55F;                          // event horizon grows
        const float shrink = 0.06F + 0.94F * std::pow(1.0F - t, 1.5F);  // 1 -> 0.06: view falls in
        const float ringW = maxR * 0.035F + 1.0F;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const std::size_t i = static_cast<std::size_t>(y) * w + x;
            const float dx = x - cx;
            const float dy = (y - cy) * ya;
            const float r = std::sqrt(dx * dx + dy * dy);
            if (r < rh)
            {
              dst[i] = Rgb{0, 0, 0, false};  // past the event horizon
              continue;
            }
            const float ang = std::atan2(dy, dx);
            const float srcR = r / shrink;
            const float swirl = t * 5.0F * (1.0F + 1.8F / (srcR / maxR + 0.12F));  // inner faster
            const float a = ang - swirl;
            const Rgb c = sample(src, w, h, cx + srcR * std::cos(a), cy + srcR * std::sin(a) / ya);
            float cr = c.transparent ? 0.0F : c.r;
            float cg = c.transparent ? 0.0F : c.g;
            float cb = c.transparent ? 0.0F : c.b;
            float dim = std::clamp((r - rh) / (0.3F * maxR), 0.0F, 1.0F);  // redshift near horizon
            dim = 1.0F - (1.0F - dim) * t;
            const float e = (r - rh * 1.12F) / ringW;
            const float ring = std::exp(-e * e);  // accretion rim just outside the horizon
            dst[i] = Rgb{u8(cr * dim + 180.0F * ring),
                         u8(cg * dim + 200.0F * ring),
                         u8(cb * dim + 255.0F * ring),
                         false};
          }
      });
}

void effectButterfly(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float s0 = 0.17F * mn;
  const float sBig = w / 2.6F;
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer,
            w,
            h,
            5000,
            [&](float t, std::vector<Rgb>& dst)
            {
              std::fill(dst.begin(), dst.end(), Rgb{0, 0, 0, false});
              const float collapse = std::clamp(t / 0.14F, 0.0F, 1.0F);
              const float open = 0.12F + 0.88F * (0.5F + 0.5F * std::sin(t * 42.0F));  // fast flap
              float s = s0;
              float cx = (w - 1) * 0.5F;
              float cy = (h - 1) * 0.5F;
              float rot = 0.0F;
              if (collapse < 1.0F)
              {
                s = sBig + (s0 - sBig) * collapse;
              }
              else
              {
                const float tau = (t - 0.14F) / 0.86F;
                s = s0 * (1.0F - 0.2F * tau);
                cx = (w - 1) * 0.5F + std::sin(t * 2.3F) * w * 0.24F + tau * w * 0.12F;
                cy = (h - 1) * 0.55F - tau * h * 0.5F + std::sin(t * 3.1F) * h * 0.10F;
                rot = std::sin(t * 2.3F) * 0.4F;  // bank into the turns
              }
              const float cr = std::cos(rot);
              const float sr = std::sin(rot);
              const float ext = s * 1.25F;
              const int x0 = std::max(0, static_cast<int>(cx - ext));
              const int x1 = std::min(w - 1, static_cast<int>(cx + ext));
              const int y0 = std::max(0, static_cast<int>(cy - ext / ya));
              const int y1 = std::min(h - 1, static_cast<int>(cy + ext / ya));
              for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                {
                  const float dx = x - cx;
                  const float dyp = (y - cy) * ya;
                  const float lx = (dx * cr + dyp * sr) / s;  // lateral
                  const float ly = (dx * sr - dyp * cr) / s;  // up = +ly
                  const float bxw = std::fabs(lx) / open;     // wings flap shut
                  bool draw = false;
                  if (std::fabs(lx) < 0.05F && ly > -0.55F && ly < 0.66F)
                    draw = true;  // slim body
                  else
                  {
                    // Wing lobes sit well outboard of the body (clear thin-body
                    // gap), with pointed spikes at the fore- and hind-wing tips.
                    const float fe = (bxw - 0.66F) / 0.44F;
                    const float ff = (ly - 0.30F) / 0.50F;
                    const float he = (bxw - 0.60F) / 0.40F;
                    const float hf = (ly + 0.32F) / 0.38F;
                    if (fe * fe + ff * ff <= 1.0F || he * he + hf * hf <= 1.0F ||
                        inTri(bxw, ly, 0.88F, 0.52F, 0.98F, 0.26F, 1.32F, 0.66F) ||
                        inTri(bxw, ly, 0.72F, -0.50F, 0.94F, -0.60F, 1.20F, -1.00F))
                      draw = true;  // fore / hind wing + tip spikes
                  }
                  if (!draw)
                    continue;
                  const Rgb tex =
                      sample(src, w, h, (lx * 0.5F + 0.5F) * (w - 1), (0.5F - ly * 0.5F) * (h - 1));
                  const float r = tex.transparent ? 200.0F : tex.r;
                  const float g = tex.transparent ? 120.0F : tex.g;
                  const float b = tex.transparent ? 60.0F : tex.b;
                  const float sh = 0.55F + 0.45F * std::clamp(1.0F - std::fabs(lx), 0.0F, 1.0F);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(r * sh), u8(g * sh), u8(b * sh), false};
                }
            });
}

void effectLorenzAttractor(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  // Integrate the Lorenz system once at startup; render the trail per frame.
  constexpr int N = 1600;
  std::vector<std::array<float, 3>> path(N);
  {
    float x = 0.1F, y = 0.0F, z = 0.0F;
    constexpr float sigma = 10.0F, rho = 28.0F, beta = 8.0F / 3.0F;
    const float dt = 0.012F;
    for (int i = 0; i < N; ++i)
    {
      const float dx = sigma * (y - x);
      const float dy = x * (rho - z) - y;
      const float dz = x * y - beta * z;
      x += dx * dt;
      y += dy * dt;
      z += dz * dt;
      path[i] = {x, y, z};
    }
  }
  const float scale = mn / 50.0F;
  runFrames(renderer, w, h, 5600,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float dim = 0.10F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  dst[static_cast<std::size_t>(y) * w + x] =
                      s.transparent
                          ? Rgb{6, 8, 12, false}
                          : Rgb{u8(s.r * dim), u8(s.g * dim), u8(s.b * dim + 4), false};
                }
              const float yaw = t * 1.5F;
              const float cyy = std::cos(yaw), syy = std::sin(yaw);
              const int head = std::min(N - 1, static_cast<int>(t * N));
              const int tail = std::max(0, head - 300);
              for (int i = tail; i <= head; ++i)
              {
                const float px = path[i][0];
                const float py = path[i][1];
                const float pz = path[i][2] - 25.0F;
                const float rx = px * cyy + py * syy;
                const float ry = -px * syy + py * cyy;
                const float sx = w * 0.5F + rx * scale;
                const float sy = h * 0.55F - pz * scale * 0.55F + ry * scale * 0.15F;
                const float age = static_cast<float>(head - i) / 300.0F;
                const float br = 1.0F - age;
                Rgb c;
                if (i == head)
                  c = Rgb{255, 245, 220, false};
                else
                  c = Rgb{u8(255 * br), u8(140 * br), u8(60 * br), false};
                plotDot(dst, w, h, sx, sy, std::max(1.0F, mn * 0.006F), ya, c);
              }
            });
}

void effectMandelbrot(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(
      renderer, w, h, 5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float dim = 0.10F;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            dst[static_cast<std::size_t>(y) * w + x] =
                s.transparent ? Rgb{4, 6, 14, false}
                              : Rgb{u8(s.r * dim), u8(s.g * dim + 4), u8(s.b * dim + 10), false};
          }
        const float zoom = 1.0F + t * t * 6.0F;
        const float cxR = -0.74F, cyR = 0.10F;  // sweep toward seahorse valley
        const float xMin = cxR - 1.5F / zoom, xMax = cxR + 1.0F / zoom;
        const float yMin = cyR - 1.0F / zoom, yMax = cyR + 1.0F / zoom;
        const int sweep = std::min(w, static_cast<int>(t * w * 1.3F));
        for (int py = 0; py < h; ++py)
        {
          for (int px = 0; px < sweep; ++px)
          {
            const float a0 = xMin + (xMax - xMin) * px / static_cast<float>(w);
            const float b0 = yMin + (yMax - yMin) * py / static_cast<float>(h);
            float a = 0, b = 0;
            int iter = 0;
            constexpr int kMaxIter = 64;
            while (iter < kMaxIter && a * a + b * b < 4.0F)
            {
              const float aa = a * a - b * b + a0;
              const float bb = 2.0F * a * b + b0;
              a = aa;
              b = bb;
              ++iter;
            }
            Rgb c;
            if (iter == kMaxIter)
              c = Rgb{8, 4, 16, false};
            else
            {
              const float q = static_cast<float>(iter) / kMaxIter;
              c = Rgb{u8(40 + 215 * q), u8(20 + 60 * q * q), u8(80 + 120 * (1 - q)), false};
            }
            dst[static_cast<std::size_t>(py) * w + px] = c;
          }
        }
      });
}

void effectMobius(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(
      renderer, w, h, 5600,
      [&](float t, std::vector<Rgb>& dst)
      {
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
            dst[static_cast<std::size_t>(y) * w + x] = Rgb{6, 6, 12, false};
        const float yaw = t * 3.5F;
        const float cyw = std::cos(yaw), syw = std::sin(yaw);
        const float cxw = w * 0.5F, cyy = h * 0.5F;
        const float R = mn * 0.32F;
        for (float u = 0.0F; u < 6.2832F; u += 0.005F)
        {
          for (float v = -1.0F; v <= 1.0F; v += 0.06F)
          {
            const float half = u * 0.5F;
            const float pxw = (R + v * R * 0.25F * std::cos(half)) * std::cos(u);
            const float pyw = v * R * 0.25F * std::sin(half);
            const float pzw = (R + v * R * 0.25F * std::cos(half)) * std::sin(u);
            const float rx = pxw * cyw + pzw * syw;
            const float rz = -pxw * syw + pzw * cyw;
            const float sx = cxw + rx;
            const float sy = cyy + pyw - rz * 0.30F;
            const float texU = (u / 6.2832F) * w;
            const float texV = (v * 0.5F + 0.5F) * h;
            const Rgb d = sample(src, w, h, texU, texV);
            const float depth = (rz + R) / (2 * R);
            const float bright = 0.55F + 0.45F * depth;
            const Rgb c{u8(d.transparent ? 80 : d.r * bright),
                        u8(d.transparent ? 80 : d.g * bright),
                        u8(d.transparent ? 120 : d.b * bright), false};
            const int ix = static_cast<int>(sx), iy = static_cast<int>(sy);
            if (ix >= 0 && ix < w && iy >= 0 && iy < h)
              dst[static_cast<std::size_t>(iy) * w + ix] = c;
          }
        }
      });
}

void effectNewtonCradle(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(
      renderer, w, h, 5600,
      [&](float t, std::vector<Rgb>& dst)
      {
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 30.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(28 + l * 0.10F), u8(24 + l * 0.08F), u8(22 + l * 0.06F), false};
          }
        const float frameY = h * 0.18F;
        const float pivotY = h * 0.30F;
        const float ballY = h * 0.62F;
        const Rgb steel{200, 200, 215, false};
        const Rgb dark{30, 30, 40, false};
        // Top rail.
        drawSeg(dst, w, h, w * 0.18F, frameY, w * 0.82F, frameY, std::max(1.0F, mn * 0.010F), ya, dark);
        drawSeg(dst, w, h, w * 0.18F, frameY, w * 0.18F, h * 0.85F, std::max(1.0F, mn * 0.010F), ya, dark);
        drawSeg(dst, w, h, w * 0.82F, frameY, w * 0.82F, h * 0.85F, std::max(1.0F, mn * 0.010F), ya, dark);
        const float spacing = mn * 0.08F;
        const float L = ballY - pivotY;
        // Pendulum kinematics: a "phase" decides whether the left or right ball
        // is up. Use a half-cycle saw with damping.
        const float cycle = std::fmod(t * 1.6F, 2.0F);
        const float amp = std::max(0.0F, 0.6F * (1.0F - t * 0.4F));
        const float leftAng =
            cycle < 1.0F ? -amp * std::sin(cycle * 3.14159F) : 0.0F;
        const float rightAng =
            cycle >= 1.0F ? amp * std::sin((cycle - 1.0F) * 3.14159F) : 0.0F;
        for (int i = 0; i < 5; ++i)
        {
          const float baseX = w * 0.5F + (i - 2) * spacing;
          float ang = 0.0F;
          if (i == 0) ang = leftAng;
          else if (i == 4) ang = rightAng;
          const float bx = baseX + L * std::sin(ang);
          const float by = pivotY + L * std::cos(ang);
          drawSeg(dst, w, h, baseX, pivotY, bx, by, std::max(1.0F, mn * 0.004F), ya, steel);
          plotDot(dst, w, h, bx, by, mn * 0.030F, ya, steel);
          plotDot(dst, w, h, bx - mn * 0.010F, by - mn * 0.010F, mn * 0.008F, ya,
                  Rgb{245, 245, 250, false});
        }
      });
}

void effectPendulumWaves(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5600,
            [&](float t, std::vector<Rgb>& dst)
            {
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 24.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(14 + l * 0.06F), u8(14 + l * 0.06F), u8(20 + l * 0.06F), false};
                }
              constexpr int kN = 15;
              const float topY = h * 0.18F;
              const float xLeft = w * 0.12F, xRight = w * 0.88F;
              const float amp = (xRight - xLeft) * 0.0F;
              (void)amp;
              for (int i = 0; i < kN; ++i)
              {
                const float baseX = xLeft + (xRight - xLeft) * i / static_cast<float>(kN - 1);
                const float Lmin = mn * 0.28F, Lmax = mn * 0.60F;
                const float L = Lmin + (Lmax - Lmin) * (1.0F - i / static_cast<float>(kN - 1));
                const float freq = (40.0F + i) / 30.0F;
                const float ang = 0.7F * std::cos(t * freq * 6.2832F * 0.5F);
                const float bx = baseX + L * std::sin(ang) * 0.5F;
                const float by = topY + L * std::cos(ang);
                drawSeg(dst, w, h, baseX, topY, bx, by, std::max(1.0F, mn * 0.003F), ya,
                        Rgb{180, 180, 180, false});
                const Rgb ball{u8(120 + (255 - 120) * i / kN), u8(80 + 175 * (kN - i) / kN), 200,
                               false};
                plotDot(dst, w, h, bx, by, mn * 0.018F, ya, ball);
              }
            });
}

void effectPythagoras(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(
      renderer, w, h, 5600,
      [&](float t, std::vector<Rgb>& dst)
      {
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 80.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(220 + l * 0.08F), u8(212 + l * 0.08F), u8(200 + l * 0.06F), false};
          }
        // 3-4-5 triangle. Right angle at origin (Cx, Cy); legs going up and
        // right; hypotenuse closes the triangle.
        const float scale = mn * 0.08F;
        const float Cx = w * 0.42F, Cy = h * 0.65F;
        const float a = 3.0F * scale, b = 4.0F * scale;  // sides
        const Rgb ink{20, 20, 30, false};
        auto fillRect = [&](float x0, float y0, float x1, float y1, Rgb c)
        {
          const int xa = std::max(0, static_cast<int>(std::min(x0, x1)));
          const int xb = std::min(w - 1, static_cast<int>(std::max(x0, x1)));
          const int ya2 = std::max(0, static_cast<int>(std::min(y0, y1)));
          const int yb = std::min(h - 1, static_cast<int>(std::max(y0, y1)));
          for (int yy = ya2; yy <= yb; ++yy)
            for (int xx = xa; xx <= xb; ++xx)
              dst[static_cast<std::size_t>(yy) * w + xx] = c;
        };
        // Squares grow with t.
        const float gA = std::clamp(t / 0.20F, 0.0F, 1.0F);
        const float gB = std::clamp((t - 0.15F) / 0.20F, 0.0F, 1.0F);
        const float gC = std::clamp((t - 0.30F) / 0.20F, 0.0F, 1.0F);
        // Square on a (left, vertical leg).
        fillRect(Cx - a * gA, Cy, Cx, Cy - a * gA, Rgb{120, 180, 240, false});
        // Square on b (below, horizontal leg).
        fillRect(Cx, Cy, Cx + b * gB, Cy + b * gB, Rgb{240, 200, 120, false});
        // Square on c (the hypotenuse). Hypotenuse goes from (Cx-a, Cy) to
        // (Cx, Cy-b)... actually wait, legs go from (Cx, Cy) up (a) and right
        // (b)... let me re-set: A=(Cx,Cy), B=(Cx, Cy-a), C=(Cx+b, Cy).
        // Hypotenuse BC.
        // Place hypotenuse-square outward from BC.
        const float bx = Cx, by = Cy - a;
        const float cx = Cx + b, cy = Cy;
        const float vx = cx - bx, vy = cy - by;
        const float nx = vy, ny = -vx;  // outward normal
        // Square outline of hypotenuse, drawn via interpolated dots.
        const float gC2 = gC;
        for (float u = 0.0F; u <= 1.0F; u += 0.02F)
          for (float v = 0.0F; v <= 1.0F * gC2; v += 0.04F)
          {
            const float px = bx + vx * u + nx * v;
            const float py = by + vy * u + ny * v;
            plotDot(dst, w, h, px, py, std::max(1.0F, mn * 0.006F), ya,
                    Rgb{200, 120, 220, false});
          }
        // Triangle outline.
        drawSeg(dst, w, h, Cx, Cy, Cx, Cy - a, std::max(1.0F, mn * 0.005F), ya, ink);
        drawSeg(dst, w, h, Cx, Cy, Cx + b, Cy, std::max(1.0F, mn * 0.005F), ya, ink);
        drawSeg(dst, w, h, Cx, Cy - a, Cx + b, Cy, std::max(1.0F, mn * 0.005F), ya, ink);
        // "a² + b² = c²" label appears at the end.
        if (t > 0.55F)
        {
          const std::string text = "A2 + B2 = C2";
          const int sc = std::max(2, static_cast<int>(mn / 80.0F));
          const float fadeT = std::clamp((t - 0.55F) / 0.20F, 0.0F, 1.0F);
          for (int ci = 0; ci < static_cast<int>(text.size()); ++ci)
          {
            const char ch = text[ci];
            if (ch == ' ') continue;
            const auto g = glyph5x7(ch);
            for (int fy = 0; fy < 7; ++fy)
              for (int fx = 0; fx < 5; ++fx)
                if (g[fy][fx] == '1')
                  plotDot(dst, w, h, w * 0.18F + (ci * 6 + fx) * sc, h * 0.92F + fy * sc,
                          std::max(1.0F, static_cast<float>(sc) * 0.5F * fadeT), ya, ink);
          }
        }
      });
}

void effectSchrodinger(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  const bool alive = hash(static_cast<int>(w * 31 + h * 17)) > 0.5F;
  runFrames(
      renderer, w, h, 5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float dim = 0.35F;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 50.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(40 + l * 0.10F * dim), u8(40 + l * 0.10F * dim),
                    u8(56 + l * 0.10F * dim), false};
          }
        const float cx = w * 0.5F, cy = h * 0.55F;
        const float bw = mn * 0.22F, bh = mn * 0.18F;
        // Box outline.
        const Rgb wood{170, 110, 60, false};
        for (int yy = static_cast<int>(cy - bh); yy <= static_cast<int>(cy + bh); ++yy)
          for (int xo = -1; xo <= 1; ++xo)
          {
            const int xa = static_cast<int>(cx - bw) + xo, xb = static_cast<int>(cx + bw) + xo;
            if (yy >= 0 && yy < h)
            {
              if (xa >= 0 && xa < w) dst[static_cast<std::size_t>(yy) * w + xa] = wood;
              if (xb >= 0 && xb < w) dst[static_cast<std::size_t>(yy) * w + xb] = wood;
            }
          }
        for (int xx = static_cast<int>(cx - bw); xx <= static_cast<int>(cx + bw); ++xx)
          for (int yo = -1; yo <= 1; ++yo)
          {
            const int ya2 = static_cast<int>(cy - bh) + yo, yb = static_cast<int>(cy + bh) + yo;
            if (xx >= 0 && xx < w)
            {
              if (ya2 >= 0 && ya2 < h) dst[static_cast<std::size_t>(ya2) * w + xx] = wood;
              if (yb >= 0 && yb < h) dst[static_cast<std::size_t>(yb) * w + xx] = wood;
            }
          }
        // Superposition flicker before observation; collapse at t = 0.65.
        const float collapse = 0.65F;
        auto drawCat = [&](float alpha, Rgb body, Rgb eye, bool x_eyes)
        {
          const float ccx = cx, ccy = cy;
          (void)alpha;
          plotDot(dst, w, h, ccx, ccy - bh * 0.20F, bh * 0.40F, ya, body);
          plotDot(dst, w, h, ccx, ccy - bh * 0.55F, bh * 0.30F, ya, body);
          // Ears
          plotDot(dst, w, h, ccx - bh * 0.20F, ccy - bh * 0.75F, bh * 0.10F, ya, body);
          plotDot(dst, w, h, ccx + bh * 0.20F, ccy - bh * 0.75F, bh * 0.10F, ya, body);
          // Eyes
          if (x_eyes)
          {
            drawSeg(dst, w, h, ccx - bh * 0.18F, ccy - bh * 0.62F, ccx - bh * 0.08F,
                    ccy - bh * 0.52F, std::max(1.0F, mn * 0.006F), ya, eye);
            drawSeg(dst, w, h, ccx - bh * 0.08F, ccy - bh * 0.62F, ccx - bh * 0.18F,
                    ccy - bh * 0.52F, std::max(1.0F, mn * 0.006F), ya, eye);
            drawSeg(dst, w, h, ccx + bh * 0.08F, ccy - bh * 0.62F, ccx + bh * 0.18F,
                    ccy - bh * 0.52F, std::max(1.0F, mn * 0.006F), ya, eye);
            drawSeg(dst, w, h, ccx + bh * 0.18F, ccy - bh * 0.62F, ccx + bh * 0.08F,
                    ccy - bh * 0.52F, std::max(1.0F, mn * 0.006F), ya, eye);
          }
          else
          {
            plotDot(dst, w, h, ccx - bh * 0.13F, ccy - bh * 0.57F, std::max(1.0F, mn * 0.008F), ya, eye);
            plotDot(dst, w, h, ccx + bh * 0.13F, ccy - bh * 0.57F, std::max(1.0F, mn * 0.008F), ya, eye);
          }
        };
        if (t < collapse)
        {
          const float strobe = std::sin(t * 30.0F);
          if (strobe > 0)
            drawCat(1.0F, Rgb{210, 200, 180, false}, Rgb{30, 30, 40, false}, false);
          else
            drawCat(1.0F, Rgb{140, 100, 100, false}, Rgb{60, 0, 0, false}, true);
        }
        else
        {
          if (alive)
            drawCat(1.0F, Rgb{210, 200, 180, false}, Rgb{30, 30, 40, false}, false);
          else
            drawCat(1.0F, Rgb{140, 100, 100, false}, Rgb{60, 0, 0, false}, true);
        }
        // Label
        const std::string label = alive ? "ALIVE" : "DEAD";
        if (t > collapse + 0.05F)
        {
          const int sc = std::max(2, static_cast<int>(mn / 80.0F));
          for (int ci = 0; ci < static_cast<int>(label.size()); ++ci)
          {
            const char ch = label[ci];
            const auto g = glyph5x7(ch);
            for (int fy = 0; fy < 7; ++fy)
              for (int fx = 0; fx < 5; ++fx)
                if (g[fy][fx] == '1')
                  plotDot(dst, w, h, cx - label.size() * 3.0F * sc + (ci * 6 + fx) * sc,
                          cy + bh * 1.3F + fy * sc,
                          std::max(1.0F, static_cast<float>(sc) * 0.5F), ya,
                          alive ? Rgb{120, 220, 120, false} : Rgb{220, 80, 80, false});
          }
        }
      });
}

void effectDodecahedron(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  // 20 dodecahedron vertices: signs of (±1,±1,±1) + permutations of (0,±φ,±1/φ).
  constexpr float phi = 1.618033988F;
  constexpr float ip = 1.0F / phi;
  std::vector<std::array<float, 3>> verts = {
      {1, 1, 1},     {1, 1, -1},    {1, -1, 1},    {1, -1, -1},   {-1, 1, 1},
      {-1, 1, -1},   {-1, -1, 1},   {-1, -1, -1},  {0, phi, ip},  {0, phi, -ip},
      {0, -phi, ip}, {0, -phi, -ip},{ip, 0, phi},  {-ip, 0, phi}, {ip, 0, -phi},
      {-ip, 0, -phi},{phi, ip, 0},  {phi, -ip, 0}, {-phi, ip, 0}, {-phi, -ip, 0}};
  // 30 edges by closest-vertex pairs at distance 2/phi.
  std::vector<std::pair<int, int>> edges;
  for (int i = 0; i < 20; ++i)
    for (int j = i + 1; j < 20; ++j)
    {
      const float dx = verts[i][0] - verts[j][0];
      const float dy = verts[i][1] - verts[j][1];
      const float dz = verts[i][2] - verts[j][2];
      const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
      if (std::fabs(d - 2.0F / phi) < 0.05F) edges.emplace_back(i, j);
    }
  runFrames(
      renderer, w, h, 5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float dim = std::clamp(1.0F - t * 1.5F, 0.05F, 1.0F);
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 20.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(4 + l * 0.04F * dim), u8(4 + l * 0.04F * dim),
                    u8(14 + l * 0.08F * dim), false};
          }
        const float cyw = std::cos(t * 1.3F), syw = std::sin(t * 1.3F);
        const float cp = std::cos(t * 0.9F), sp = std::sin(t * 0.9F);
        const float scale = mn * 0.35F;
        auto project = [&](int v, float& sx, float& sy) -> float
        {
          const float x = verts[v][0], y = verts[v][1], z = verts[v][2];
          const float x1 = x * cyw - z * syw, z1 = x * syw + z * cyw;
          const float y1 = y * cp - z1 * sp, z2 = y * sp + z1 * cp;
          sx = w * 0.5F + x1 * scale;
          sy = h * 0.5F + y1 * scale;
          return z2;
        };
        for (const auto& e : edges)
        {
          float x0, y0, x1, y1;
          const float z0 = project(e.first, x0, y0);
          const float z1 = project(e.second, x1, y1);
          const float zAvg = (z0 + z1) * 0.5F;
          const float depth = std::clamp((zAvg + 1.6F) / 3.2F, 0.0F, 1.0F);
          const Rgb c{u8(60 + 150 * depth), u8(150 + 100 * depth), u8(220 + 30 * depth), false};
          drawSeg(dst, w, h, x0, y0, x1, y1, std::max(1.0F, mn * 0.005F), ya, c);
        }
      });
}

void effectEulerIdentity(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(
      renderer, w, h, 5600,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float dim = std::clamp(1.0F - t * 0.7F, 0.20F, 1.0F);
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 24.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(12 + l * 0.06F * dim), u8(12 + l * 0.06F * dim),
                    u8(20 + l * 0.10F * dim), false};
          }
        // Each "symbol" is a glyph string and a screen anchor + reveal time.
        struct Sym { std::string s; float at; };
        static const std::vector<Sym> syms = {
            {"E", 0.05F}, {"I*PI", 0.20F}, {"+1=0", 0.45F}};
        const int sc = std::max(2, static_cast<int>(mn / 28.0F));
        const Rgb ink{220, 220, 240, false};
        float cur = w * 0.20F;
        for (const auto& sym : syms)
        {
          const float rev = std::clamp((t - sym.at) / 0.10F, 0.0F, 1.0F);
          for (std::size_t ci = 0; ci < sym.s.size(); ++ci)
          {
            const char ch = sym.s[ci];
            if (ch == '*')
            {
              cur += 3 * sc;
              continue;
            }
            const auto g = glyph5x7(ch);
            for (int fy = 0; fy < 7; ++fy)
              for (int fx = 0; fx < 5; ++fx)
                if (g[fy][fx] == '1')
                  plotDot(dst, w, h, cur + fx * sc, h * 0.45F + fy * sc,
                          std::max(1.0F, static_cast<float>(sc) * 0.55F * rev), ya, ink);
            cur += 6 * sc;
          }
          cur += 4 * sc;
        }
      });
}

void effectBohrAtom(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(
      renderer, w, h, 5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float dim = 0.15F;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            dst[static_cast<std::size_t>(y) * w + x] =
                s.transparent ? Rgb{8, 8, 18, false}
                              : Rgb{u8(s.r * dim), u8(s.g * dim + 4), u8(s.b * dim + 10), false};
          }
        const float cx = w * 0.5F, cy = h * 0.5F;
        // Nucleus
        plotDot(dst, w, h, cx, cy, mn * 0.045F, ya, Rgb{255, 180, 80, false});
        plotDot(dst, w, h, cx - mn * 0.015F, cy - mn * 0.012F, mn * 0.018F, ya, Rgb{255, 220, 150, false});
        // Three elliptical orbits + electrons
        for (int k = 0; k < 3; ++k)
        {
          const float Rk = mn * (0.10F + 0.10F * k);
          const float ec = 0.55F + 0.10F * k;  // ellipticity per orbit
          const float tilt = (k * 1.05F);
          const float ct = std::cos(tilt), st = std::sin(tilt);
          for (float a = 0; a < 6.2832F; a += 0.04F)
          {
            const float ox = Rk * std::cos(a);
            const float oy = Rk * ec * std::sin(a);
            const float px = cx + ox * ct - oy * st;
            const float py = cy + ox * st + oy * ct;
            plotDot(dst, w, h, px, py, std::max(1.0F, mn * 0.004F), ya,
                    Rgb{60, 80, 130, false});
          }
          // Electron
          const float spd = 3.0F - k * 0.6F;
          const float ea = t * 6.2832F * spd;
          const float ox = Rk * std::cos(ea);
          const float oy = Rk * ec * std::sin(ea);
          const float px = cx + ox * ct - oy * st;
          const float py = cy + ox * st + oy * ct;
          plotDot(dst, w, h, px, py, mn * 0.015F, ya, Rgb{120, 180, 255, false});
          plotDot(dst, w, h, px - mn * 0.005F, py - mn * 0.005F, mn * 0.006F, ya,
                  Rgb{220, 230, 255, false});
        }
      });
}

void effectFoucaultPendulum(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(
      renderer, w, h, 5600,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float dim = 0.30F;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 40.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(40 + l * 0.08F * dim), u8(36 + l * 0.08F * dim),
                    u8(50 + l * 0.10F * dim), false};
          }
        const float topX = w * 0.5F, topY = h * 0.12F;
        const float pivotX = w * 0.5F, pivotY = h * 0.62F;
        const float radius = mn * 0.35F;
        // Compass ring on the floor.
        const Rgb ring{200, 180, 120, false};
        for (float a = 0; a < 6.2832F; a += 0.04F)
          plotDot(dst, w, h, pivotX + std::cos(a) * radius, pivotY + std::sin(a) * radius * 0.5F,
                  std::max(1.0F, mn * 0.003F), ya, ring);
        // Compass marks
        for (int k = 0; k < 16; ++k)
        {
          const float a = k / 16.0F * 6.2832F;
          drawSeg(dst, w, h, pivotX + std::cos(a) * radius * 0.95F,
                  pivotY + std::sin(a) * radius * 0.95F * 0.5F,
                  pivotX + std::cos(a) * radius * 1.05F,
                  pivotY + std::sin(a) * radius * 1.05F * 0.5F,
                  std::max(1.0F, mn * 0.004F), ya, ring);
        }
        // Pendulum plane rotates over time (Foucault precession).
        const float planeAng = t * 2.5F;
        // Bob swings within the plane.
        const float swingAng = std::sin(t * 8.0F) * 0.7F;
        const float L = pivotY - topY;
        const float swingR = std::sin(swingAng) * radius;
        const float bx = pivotX + std::cos(planeAng) * swingR;
        const float by = pivotY + std::sin(planeAng) * swingR * 0.5F;
        // String
        drawSeg(dst, w, h, topX, topY, bx, by, std::max(1.0F, mn * 0.004F), ya,
                Rgb{200, 200, 200, false});
        (void)L;
        // Bob
        plotDot(dst, w, h, bx, by, mn * 0.025F, ya, Rgb{40, 40, 60, false});
        plotDot(dst, w, h, bx - mn * 0.007F, by - mn * 0.007F, mn * 0.008F, ya,
                Rgb{170, 170, 200, false});
        // Top mount.
        drawSeg(dst, w, h, topX - mn * 0.04F, topY, topX + mn * 0.04F, topY,
                std::max(1.0F, mn * 0.008F), ya, Rgb{160, 130, 90, false});
      });
}

void effectGalileoTower(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(
      renderer, w, h, 5200,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float dim = 0.40F;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 80.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            const float sf = static_cast<float>(y) / h;
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(170 + 60 * sf + l * 0.10F * dim),
                    u8(170 + 50 * sf + l * 0.10F * dim),
                    u8(190 + 40 * sf + l * 0.10F * dim), false};
          }
        const float groundY = h * 0.92F;
        // Tower silhouette (leaning).
        const float towerCx = w * 0.5F;
        const float towerTop = h * 0.10F;
        const float towerH = groundY - towerTop;
        const float lean = 0.08F;
        const Rgb stone{225, 215, 195, false};
        const Rgb dark{160, 145, 120, false};
        for (int yy = static_cast<int>(towerTop); yy <= static_cast<int>(groundY); ++yy)
        {
          const float frac = (yy - towerTop) / towerH;
          const float cx = towerCx + lean * mn * frac;
          const int half = static_cast<int>(mn * (0.06F + 0.005F * frac));
          for (int xo = -half; xo <= half; ++xo)
          {
            const int xx = static_cast<int>(cx + xo);
            if (xx >= 0 && xx < w)
              dst[static_cast<std::size_t>(yy) * w + xx] = (std::abs(xo) > half - 2) ? dark : stone;
          }
        }
        // Drop: both balls fall same speed (equivalence principle).
        const float dropP = std::clamp(t / 0.85F, 0.0F, 1.0F);
        const float startY = towerTop + mn * 0.08F;
        const float endY = groundY - mn * 0.025F;
        const float yy = startY + (endY - startY) * dropP * dropP;
        const float xLight = towerCx + lean * mn * (startY - towerTop) / towerH - mn * 0.04F;
        const float xHeavy = towerCx + lean * mn * (startY - towerTop) / towerH + mn * 0.04F;
        plotDot(dst, w, h, xLight, yy, mn * 0.018F, ya, Rgb{60, 60, 80, false});  // small/iron
        plotDot(dst, w, h, xHeavy, yy, mn * 0.030F, ya, Rgb{60, 60, 80, false});  // big/lead
        // Ground.
        for (int x = 0; x < w; ++x)
          dst[static_cast<std::size_t>(groundY) * w + x] = Rgb{60, 50, 40, false};
        // Impact poof.
        if (dropP > 0.95F)
        {
          for (int i = 0; i < 8; ++i)
          {
            const float a = i / 8.0F * 6.2832F;
            plotDot(dst, w, h, xLight + std::cos(a) * mn * 0.03F,
                    endY + std::sin(a) * mn * 0.01F, mn * 0.010F, ya,
                    Rgb{200, 190, 170, false});
            plotDot(dst, w, h, xHeavy + std::cos(a) * mn * 0.03F,
                    endY + std::sin(a) * mn * 0.01F, mn * 0.012F, ya,
                    Rgb{200, 190, 170, false});
          }
        }
      });
}

void effectFourier(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(
      renderer, w, h, 5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float dim = 0.18F;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 24.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(14 + l * 0.06F * dim), u8(20 + l * 0.08F * dim),
                    u8(28 + l * 0.10F * dim), false};
          }
        const int nTerms = 1 + static_cast<int>(t * 30);
        const float midY = h * 0.5F;
        const float amp = mn * 0.30F;
        // Reference square wave guide.
        for (int x = 0; x < w; ++x)
        {
          const float phase = (x / static_cast<float>(w)) * 4.0F * 3.14159F;
          const float sq = std::sin(phase) >= 0 ? amp * 0.78F : -amp * 0.78F;
          plotDot(dst, w, h, static_cast<float>(x), midY + sq, std::max(1.0F, mn * 0.002F), ya,
                  Rgb{60, 80, 100, false});
        }
        // Fourier sum.
        for (int x = 0; x < w; ++x)
        {
          const float phase = (x / static_cast<float>(w)) * 4.0F * 3.14159F;
          float sum = 0;
          for (int n = 1; n <= 2 * nTerms - 1; n += 2)
            sum += std::sin(n * phase) / n;
          const float y = midY - sum * amp;
          plotDot(dst, w, h, static_cast<float>(x), y, std::max(1.0F, mn * 0.005F), ya,
                  Rgb{255, 200, 80, false});
        }
        // n indicator.
        const std::string label = "N=" + std::to_string(nTerms);
        const int sc = std::max(2, static_cast<int>(mn / 80.0F));
        for (std::size_t ci = 0; ci < label.size(); ++ci)
        {
          const auto g = glyph5x7(label[ci]);
          for (int fy = 0; fy < 7; ++fy)
            for (int fx = 0; fx < 5; ++fx)
              if (g[fy][fx] == '1')
                plotDot(dst, w, h, mn * 0.05F + (ci * 6 + fx) * sc, h * 0.08F + fy * sc,
                        std::max(1.0F, static_cast<float>(sc) * 0.5F), ya,
                        Rgb{255, 255, 255, false});
        }
      });
}

void effectPi(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  static const char* kPi =
      "31415926535897932384626433832795028841971693993751058209749445923078164062862089986280348253421170679";
  runFrames(
      renderer, w, h, 5200,
      [&](float t, std::vector<Rgb>& dst)
      {
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
            dst[static_cast<std::size_t>(y) * w + x] = Rgb{12, 16, 28, false};
        // Scrolling digit tape.
        const int sc = std::max(2, static_cast<int>(mn / 40.0F));
        const float scrollPx = t * mn * 0.6F;
        const Rgb amber{255, 180, 70, false};
        for (int row = 0; row < 6; ++row)
        {
          const float py = h * 0.20F + row * sc * 10;
          for (int i = 0; i < 100; ++i)
          {
            const float px = w * 0.05F + i * sc * 6 - scrollPx;
            if (px < -sc * 5 || px > w) continue;
            const int idx = (i + row * 33) % static_cast<int>(std::strlen(kPi));
            const char ch = kPi[idx];
            const auto g = glyph5x7(ch);
            for (int fy = 0; fy < 7; ++fy)
              for (int fx = 0; fx < 5; ++fx)
                if (g[fy][fx] == '1')
                  plotDot(dst, w, h, px + fx * sc, py + fy * sc,
                          std::max(1.0F, static_cast<float>(sc) * 0.5F), ya, amber);
          }
        }
        // Big π symbol fades in over the second half.
        if (t > 0.45F)
        {
          const float alpha = std::clamp((t - 0.45F) / 0.30F, 0.0F, 1.0F);
          const float pcx = w * 0.5F, pcy = h * 0.5F;
          const float S = mn * 0.30F;
          // Top bar
          for (int xo = -static_cast<int>(S * 0.6F); xo <= static_cast<int>(S * 0.6F); ++xo)
            for (int yo = -static_cast<int>(S * 0.42F); yo <= -static_cast<int>(S * 0.32F); ++yo)
            {
              const int xx = static_cast<int>(pcx + xo), yy = static_cast<int>(pcy + yo);
              if (xx >= 0 && xx < w && yy >= 0 && yy < h)
              {
                Rgb& c = dst[static_cast<std::size_t>(yy) * w + xx];
                c = Rgb{u8(c.r + (255 - c.r) * alpha), u8(c.g + (200 - c.g) * alpha),
                        u8(c.b + (80 - c.b) * alpha), false};
              }
            }
          // Left leg
          for (int xo = -static_cast<int>(S * 0.40F); xo <= -static_cast<int>(S * 0.28F); ++xo)
            for (int yo = -static_cast<int>(S * 0.32F); yo <= static_cast<int>(S * 0.5F); ++yo)
            {
              const int xx = static_cast<int>(pcx + xo), yy = static_cast<int>(pcy + yo);
              if (xx >= 0 && xx < w && yy >= 0 && yy < h)
              {
                Rgb& c = dst[static_cast<std::size_t>(yy) * w + xx];
                c = Rgb{u8(c.r + (255 - c.r) * alpha), u8(c.g + (200 - c.g) * alpha),
                        u8(c.b + (80 - c.b) * alpha), false};
              }
            }
          // Right leg
          for (int xo = static_cast<int>(S * 0.28F); xo <= static_cast<int>(S * 0.40F); ++xo)
            for (int yo = -static_cast<int>(S * 0.32F); yo <= static_cast<int>(S * 0.5F); ++yo)
            {
              const int xx = static_cast<int>(pcx + xo), yy = static_cast<int>(pcy + yo);
              if (xx >= 0 && xx < w && yy >= 0 && yy < h)
              {
                Rgb& c = dst[static_cast<std::size_t>(yy) * w + xx];
                c = Rgb{u8(c.r + (255 - c.r) * alpha), u8(c.g + (200 - c.g) * alpha),
                        u8(c.b + (80 - c.b) * alpha), false};
              }
            }
        }
      });
}

void effectGoldenSpiral(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(
      renderer, w, h, 5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float dim = 0.30F;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 50.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(40 + l * 0.10F * dim), u8(35 + l * 0.10F * dim),
                    u8(25 + l * 0.08F * dim), false};
          }
        // Start with a unit at the centre, grow rectangles in Fibonacci sequence
        // with quarter-circle arcs forming the spiral.
        const float cx = w * 0.5F, cy = h * 0.5F;
        const float scale = mn * 0.05F;
        const float fibs[8] = {1, 1, 2, 3, 5, 8, 13, 21};
        const int nReveal = std::min(8, static_cast<int>(t * 9.0F));
        // Walk the spiral.
        float x0 = cx, y0 = cy;
        int dir = 0;  // 0=right, 1=up, 2=left, 3=down
        const Rgb gold{220, 180, 100, false};
        for (int i = 0; i < nReveal; ++i)
        {
          const float L = fibs[i] * scale;
          float x1 = x0, y1 = y0;
          if (dir == 0) x1 = x0 + L;
          else if (dir == 1) y1 = y0 - L;
          else if (dir == 2) x1 = x0 - L;
          else y1 = y0 + L;
          // Rectangle outline.
          const float rx0 = std::min(x0, x1), ry0 = std::min(y0, y1);
          const float rx1 = std::max(x0, x1), ry1 = std::max(y0, y1);
          (void)rx0;
          (void)ry0;
          (void)rx1;
          (void)ry1;
          // Arc — quarter circle inside the rectangle.
          for (float a = 0; a <= 1.5708F; a += 0.04F)
          {
            float ax, ay;
            // Centre of arc depends on which direction.
            float ccx, ccy;
            float startAng;
            if (dir == 0) { ccx = x1; ccy = y0 + L; startAng = -1.5708F; }
            else if (dir == 1) { ccx = x1 - L; ccy = y1; startAng = 0; }
            else if (dir == 2) { ccx = x1; ccy = y0 - L; startAng = 1.5708F; }
            else { ccx = x1 + L; ccy = y1; startAng = 3.14159F; }
            ax = ccx + std::cos(startAng + a) * L;
            ay = ccy + std::sin(startAng + a) * L;
            plotDot(dst, w, h, ax, ay, std::max(1.0F, mn * 0.005F), ya, gold);
          }
          // Move to next position.
          x0 = x1;
          y0 = y1;
          dir = (dir + 1) % 4;
        }
      });
}

void effectDoubleSlit(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(
      renderer, w, h, 5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
            dst[static_cast<std::size_t>(y) * w + x] = Rgb{8, 10, 18, false};
        const float emitterX = w * 0.10F;
        const float slitX = w * 0.40F;
        const float screenX = w * 0.85F;
        const float slitGap = mn * 0.10F;
        const float slitY1 = h * 0.5F - slitGap;
        const float slitY2 = h * 0.5F + slitGap;
        const float slitH = mn * 0.04F;
        // Wall.
        for (int yy = 0; yy < h; ++yy)
        {
          const bool open1 = std::fabs(yy - slitY1) < slitH;
          const bool open2 = std::fabs(yy - slitY2) < slitH;
          if (!open1 && !open2)
            for (int xo = -2; xo <= 2; ++xo)
            {
              const int xx = static_cast<int>(slitX) + xo;
              if (xx >= 0 && xx < w)
                dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{100, 100, 110, false};
            }
        }
        // Emitter
        plotDot(dst, w, h, emitterX, h * 0.5F, mn * 0.025F, ya, Rgb{120, 200, 255, false});
        // Screen (far wall)
        for (int yy = 0; yy < h; ++yy)
        {
          const int xx = static_cast<int>(screenX);
          if (xx >= 0 && xx < w) dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{60, 60, 70, false};
        }
        // Fringe accumulation: deterministic pattern, increase intensity over t.
        const float intensity = std::clamp(t * 2.0F, 0.0F, 1.0F);
        for (int yy = 0; yy < h; ++yy)
        {
          const float dy1 = (yy - slitY1);
          const float dy2 = (yy - slitY2);
          const float d1 = std::hypot(screenX - slitX, dy1);
          const float d2 = std::hypot(screenX - slitX, dy2);
          const float wavelength = mn * 0.02F;
          const float ph = (d2 - d1) / wavelength * 6.2832F;
          const float amp = std::cos(ph * 0.5F);
          const float br = amp * amp * intensity;
          for (int xo = 1; xo <= static_cast<int>(mn * 0.04F * intensity); ++xo)
          {
            const int xx = static_cast<int>(screenX) - xo;
            if (xx >= 0 && xx < w)
            {
              const float fade = 1.0F - static_cast<float>(xo) / (mn * 0.04F);
              dst[static_cast<std::size_t>(yy) * w + xx] =
                  Rgb{u8(60 + 195 * br * fade), u8(150 + 100 * br * fade),
                      u8(220 + 30 * br * fade), false};
            }
          }
        }
        // Particle in flight
        const float pf = std::fmod(t * 2.5F, 1.0F);
        const float px = emitterX + pf * (screenX - emitterX);
        const float py = h * 0.5F + (hash(static_cast<int>(t * 30)) - 0.5F) * mn * 0.15F * pf;
        plotDot(dst, w, h, px, py, mn * 0.010F, ya, Rgb{255, 230, 150, false});
      });
}

void effectSierpinski(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(
      renderer, w, h, 5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float dim = 0.10F;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 14.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(8 + l * 0.04F * dim), u8(8 + l * 0.04F * dim),
                    u8(18 + l * 0.06F * dim), false};
          }
        const float cxw = w * 0.5F, midY = h * 0.55F;
        const float S = mn * 0.42F;
        const float v0x = cxw, v0y = midY - S * 0.9F;
        const float v1x = cxw - S, v1y = midY + S * 0.55F;
        const float v2x = cxw + S, v2y = midY + S * 0.55F;
        // Triangle outline.
        drawSeg(dst, w, h, v0x, v0y, v1x, v1y, std::max(1.0F, mn * 0.004F), ya,
                Rgb{200, 200, 220, false});
        drawSeg(dst, w, h, v0x, v0y, v2x, v2y, std::max(1.0F, mn * 0.004F), ya,
                Rgb{200, 200, 220, false});
        drawSeg(dst, w, h, v1x, v1y, v2x, v2y, std::max(1.0F, mn * 0.004F), ya,
                Rgb{200, 200, 220, false});
        // Chaos game.
        const int nPts = static_cast<int>(t * 12000);
        float px = cxw, py = midY;
        for (int i = 0; i < nPts; ++i)
        {
          const int k = static_cast<int>(hash(i * 7) * 3) % 3;
          float tx, tyv;
          if (k == 0) { tx = v0x; tyv = v0y; }
          else if (k == 1) { tx = v1x; tyv = v1y; }
          else { tx = v2x; tyv = v2y; }
          px = (px + tx) * 0.5F;
          py = (py + tyv) * 0.5F;
          if (i > 5)
          {
            const int ix = static_cast<int>(px), iy = static_cast<int>(py);
            if (ix >= 0 && ix < w && iy >= 0 && iy < h)
            {
              const Rgb c{u8(120 + (i & 0xFF) / 2), u8(255 - (i & 0xFF) / 2),
                          u8(180), false};
              dst[static_cast<std::size_t>(iy) * w + ix] = c;
            }
          }
        }
      });
}

void effectBrachistochrone(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(
      renderer, w, h, 5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float dim = 0.40F;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 60.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(40 + l * 0.10F * dim), u8(50 + l * 0.10F * dim),
                    u8(70 + l * 0.10F * dim), false};
          }
        const float Ax = w * 0.20F, Ay = h * 0.30F;
        const float Bx = w * 0.80F, By = h * 0.78F;
        // Straight ramp.
        drawSeg(dst, w, h, Ax, Ay, Bx, By, std::max(1.0F, mn * 0.005F), ya,
                Rgb{200, 80, 80, false});
        // Cycloid: parametric (a(θ-sin θ), a(1-cos θ)).
        const float Hh = By - Ay;
        const float L = Bx - Ax;
        // For simplicity sample a cycloid that passes through both points.
        std::vector<std::pair<float, float>> cyc;
        for (float th = 0; th <= 6.0F; th += 0.05F)
        {
          const float xt = th - std::sin(th);
          const float yt = 1.0F - std::cos(th);
          cyc.emplace_back(xt, yt);
        }
        // Scale to fit.
        const float maxX = cyc.back().first;
        const float maxY = 2.0F;
        for (auto& p : cyc)
        {
          p.first = Ax + (p.first / maxX) * L;
          p.second = Ay + (p.second / maxY) * Hh;
        }
        for (std::size_t i = 1; i < cyc.size(); ++i)
          drawSeg(dst, w, h, cyc[i - 1].first, cyc[i - 1].second, cyc[i].first, cyc[i].second,
                  std::max(1.0F, mn * 0.005F), ya, Rgb{80, 200, 120, false});
        // Markers.
        plotDot(dst, w, h, Ax, Ay, mn * 0.014F, ya, Rgb{240, 240, 240, false});
        plotDot(dst, w, h, Bx, By, mn * 0.014F, ya, Rgb{240, 240, 240, false});
        // Race: cycloid finishes first.
        const float cycT = std::clamp(t * 1.6F, 0.0F, 1.0F);
        const float strT = std::clamp(t * 1.3F, 0.0F, 1.0F);
        const std::size_t cycIdx = std::min(cyc.size() - 1, static_cast<std::size_t>(cycT * cyc.size()));
        const float strX = Ax + strT * (Bx - Ax), strY = Ay + strT * (By - Ay);
        plotDot(dst, w, h, cyc[cycIdx].first, cyc[cycIdx].second, mn * 0.018F, ya,
                Rgb{120, 255, 180, false});
        plotDot(dst, w, h, strX, strY, mn * 0.018F, ya, Rgb{255, 120, 120, false});
      });
}

void effectChladni(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  runFrames(
      renderer, w, h, 5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float cx = w * 0.5F, cy = h * 0.5F;
        const float plateR = mn * 0.40F;
        // Plate background.
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const float dx = x - cx, dy = (y - cy) * ya;
            const float r = std::hypot(dx, dy);
            if (r > plateR)
              dst[static_cast<std::size_t>(y) * w + x] = Rgb{20, 18, 18, false};
            else
              dst[static_cast<std::size_t>(y) * w + x] = Rgb{60, 50, 40, false};
          }
        // Mode (n, m) cycles over time.
        const int stage = static_cast<int>(t * 4.0F);
        int n, m;
        switch (stage % 4)
        {
          case 0: n = 3; m = 2; break;
          case 1: n = 4; m = 3; break;
          case 2: n = 5; m = 4; break;
          default: n = 6; m = 5; break;
        }
        // Plot sand along nodal lines: f(x,y) = cos(nπx/L)cos(mπy/L) - cos(mπx/L)cos(nπy/L) ≈ 0.
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const float dx = x - cx, dy = (y - cy) * ya;
            const float r = std::hypot(dx, dy);
            if (r > plateR) continue;
            const float u = (dx / plateR);
            const float v = (dy / plateR);
            const float val =
                std::cos(n * 3.14159F * u) * std::cos(m * 3.14159F * v) -
                std::cos(m * 3.14159F * u) * std::cos(n * 3.14159F * v);
            if (std::fabs(val) < 0.05F)
              dst[static_cast<std::size_t>(y) * w + x] = Rgb{240, 220, 180, false};
          }
        // Frequency label.
        const std::string lab = "(" + std::to_string(n) + "," + std::to_string(m) + ")";
        const int sc = std::max(2, static_cast<int>(mn / 50.0F));
        for (std::size_t ci = 0; ci < lab.size(); ++ci)
        {
          const auto g = glyph5x7(lab[ci]);
          for (int fy = 0; fy < 7; ++fy)
            for (int fx = 0; fx < 5; ++fx)
              if (g[fy][fx] == '1')
                plotDot(dst, w, h, mn * 0.05F + (ci * 6 + fx) * sc, h * 0.06F + fy * sc,
                        std::max(1.0F, static_cast<float>(sc) * 0.5F), ya,
                        Rgb{240, 220, 180, false});
        }
      });
}

void effectStandingWave(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(
      renderer, w, h, 5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float dim = 0.18F;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 22.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(14 + l * 0.06F * dim), u8(20 + l * 0.08F * dim),
                    u8(28 + l * 0.10F * dim), false};
          }
        const float midY = h * 0.55F;
        const float amp = mn * 0.20F;
        const float L = w * 0.84F;
        const float x0 = w * 0.08F;
        const float xN = x0 + L;
        // Pegs at ends.
        plotDot(dst, w, h, x0, midY, mn * 0.020F, ya, Rgb{180, 160, 110, false});
        plotDot(dst, w, h, xN, midY, mn * 0.020F, ya, Rgb{180, 160, 110, false});
        const float modeT = std::fmod(t * 1.4F, 1.0F);
        const int n = 1 + static_cast<int>(t * 1.4F) % 4;
        const float oscFreq = 8.0F * n;
        const float osc = std::sin(t * oscFreq);
        const Rgb str{220, 200, 150, false};
        for (int x = static_cast<int>(x0); x <= static_cast<int>(xN); ++x)
        {
          const float u = (x - x0) / L;
          const float y = midY + amp * std::sin(n * 3.14159F * u) * osc;
          plotDot(dst, w, h, static_cast<float>(x), y, std::max(1.0F, mn * 0.005F), ya, str);
        }
        // Nodes (every λ/2).
        for (int k = 0; k <= n; ++k)
        {
          const float nx = x0 + L * k / n;
          plotDot(dst, w, h, nx, midY, std::max(1.0F, mn * 0.008F), ya, Rgb{120, 200, 255, false});
        }
        // Label.
        const std::string lab = "N=" + std::to_string(n);
        const int sc = std::max(2, static_cast<int>(mn / 70.0F));
        for (std::size_t ci = 0; ci < lab.size(); ++ci)
        {
          const auto g = glyph5x7(lab[ci]);
          for (int fy = 0; fy < 7; ++fy)
            for (int fx = 0; fx < 5; ++fx)
              if (g[fy][fx] == '1')
                plotDot(dst, w, h, mn * 0.05F + (ci * 6 + fx) * sc, h * 0.10F + fy * sc,
                        std::max(1.0F, static_cast<float>(sc) * 0.5F), ya,
                        Rgb{220, 220, 240, false});
        }
        (void)modeT;
      });
}

void effectSunDogs(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
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
              Rgb{u8(220 - 40 * sf + l * 0.10F), u8(230 - 30 * sf + l * 0.08F),
                  u8(240 - 10 * sf + l * 0.08F), false};
        }
      // 22° halo arc.
      const float cx = w * 0.5F, cy = h * 0.45F;
      const float haloR = mn * 0.28F;
      for (float a = 3.14159F * 0.6F; a <= 3.14159F * 1.4F; a += 0.02F) {
        const float hx = cx + std::cos(a) * haloR;
        const float hy = cy - std::sin(a) * haloR / ya;
        plotDot(dst, w, h, hx, hy, std::max(1.0F, mn * 0.003F), ya, Rgb{255, 240, 200, false});
      }
      // Main sun + two parhelia (22° to either side along horizontal).
      drawDataDisk(dst, w, h, src, cx, cy, mn * 0.10F, ya, 0.40F, t * 0.3F, Rgb{255, 230, 160, false});
      drawDataDisk(dst, w, h, src, cx - haloR, cy, mn * 0.05F, ya, 0.5F, t * 0.2F, Rgb{255, 220, 180, false});
      drawDataDisk(dst, w, h, src, cx + haloR, cy, mn * 0.05F, ya, 0.5F, t * 0.2F, Rgb{255, 220, 180, false});
    });
}

void effectAccretionDisk(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
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
          if (r < mn * 0.05F) {
            dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{0, 0, 0, false};  // event horizon
            continue;
          }
          if (r > mn * 0.40F) {
            dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{4, 4, 12, false};
            continue;
          }
          const float a = std::atan2(dy, dx);
          // Kepler-ish: faster nearer in.
          const float spiral = a + r * 0.05F - t * 12.0F / (r / mn + 0.5F);
          const float arm = 0.5F + 0.5F * std::cos(spiral * 2.0F);
          const Rgb d = sample(src, w, h, xx, yy);
          const float dl = d.transparent ? 100.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          // Inner = hot orange, outer = cooler red, modulated by data.
          const float heat = 1.0F - (r - mn * 0.05F) / (mn * 0.35F);
          dst[static_cast<std::size_t>(yy) * w + xx] =
              Rgb{u8(80 + 170 * arm * heat + dl * 0.20F), u8(40 + 130 * arm * heat + dl * 0.10F),
                  u8(20 + 50 * arm * heat * (1 - heat)), false};
        }
    });
}

void effectBigBang(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          dst[static_cast<std::size_t>(y) * w + x] = Rgb{4, 4, 12, false};
      const float cx = w * 0.5F, cy = h * 0.5F;
      const float expansion = std::clamp(t * 1.6F, 0.0F, 1.0F);
      // Bright core fades as expansion.
      const float coreFade = std::clamp(1.0F - t * 1.5F, 0.0F, 1.0F);
      plotDot(dst, w, h, cx, cy, mn * 0.05F * (0.5F + coreFade), ya,
              Rgb{u8(240 * coreFade), u8(220 * coreFade), u8(180 * coreFade), false});
      // Particles flying outward.
      for (int i = 0; i < 250; ++i) {
        const float a = hash(i) * 6.2832F;
        const float v = 0.3F + 0.7F * hash(i * 3);
        const float r = expansion * mn * 0.55F * v;
        const float px = cx + std::cos(a) * r;
        const float py = cy + std::sin(a) * r / ya;
        if (px < 0 || px >= w || py < 0 || py >= h) continue;
        const Rgb d = sample(src, w, h, static_cast<int>(px), static_cast<int>(py));
        const float dr = d.transparent ? 100.0F : d.r;
        const float dg = d.transparent ? 100.0F : d.g;
        const float db = d.transparent ? 120.0F : d.b;
        plotDot(dst, w, h, px, py, mn * 0.008F, ya,
                Rgb{u8(150 + dr * 0.30F), u8(120 + dg * 0.30F), u8(180 + db * 0.30F), false});
      }
    });
}

void effectCmbGlow(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  (void)ya; (void)mn;
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb d = sample(src, w, h, x, y);
          const float dl = d.transparent ? 50.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          // Two-frequency noise via sin combinations.
          const float n1 = std::sin(x * 0.08F + y * 0.05F + t * 0.5F);
          const float n2 = std::sin(x * 0.20F - y * 0.13F + t * 0.7F);
          const float n = (n1 + n2) * 0.5F;
          // Map to Planck-style red/orange/yellow.
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(180 + 50 * n + dl * 0.10F), u8(80 + 80 * n + dl * 0.08F),
                  u8(40 + 60 * (-n) + dl * 0.05F), false};
        }
    });
}

void effectCometTail(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          dst[static_cast<std::size_t>(y) * w + x] = Rgb{6, 6, 16, false};
      // Stars.
      for (int i = 0; i < 80; ++i) {
        const int sx = static_cast<int>(hash(i) * w);
        const int sy = static_cast<int>(hash(i * 3) * h);
        const float tw = 0.5F + 0.5F * std::sin(t * 8.0F + i);
        dst[static_cast<std::size_t>(sy) * w + sx] = Rgb{u8(180 * tw), u8(180 * tw), u8(220 * tw), false};
      }
      // Comet head.
      const float cx = w * (0.15F + t * 0.70F);
      const float cy = h * (0.30F + t * 0.20F);
      drawDataDisk(dst, w, h, src, cx, cy, mn * 0.020F, ya, 0.5F, t * 2.0F, Rgb{220, 230, 255, false});
      // Dust tail (curved, yellow-white, away from motion direction).
      for (int i = 0; i < 80; ++i) {
        const float f = i / 80.0F;
        const float a = -1.0F + 0.6F * std::sin(f * 2.0F);  // curved dust
        const float dx = -std::cos(a) * mn * 0.30F * f;
        const float dy = std::sin(a) * mn * 0.15F * f;
        const Rgb d = sample(src, w, h, static_cast<int>(cx + dx), static_cast<int>(cy + dy));
        const float dr = d.transparent ? 220.0F : d.r;
        plotDot(dst, w, h, cx + dx, cy + dy, mn * 0.012F * (1 - f), ya,
                Rgb{u8(220 + dr * 0.1F * (1 - f)), u8(220 * (1 - f * 0.5F)), u8(180 * (1 - f)), false});
      }
      // Ion tail (straight, blue, anti-solar).
      for (int i = 0; i < 80; ++i) {
        const float f = i / 80.0F;
        const float dx = -mn * 0.40F * f;
        const float dy = -mn * 0.05F * f;
        plotDot(dst, w, h, cx + dx, cy + dy, mn * 0.008F * (1 - f), ya,
                Rgb{u8(80), u8(140 * (1 - f)), u8(255 * (1 - f)), false});
      }
    });
}

void effectDeepField(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          dst[static_cast<std::size_t>(y) * w + x] = Rgb{4, 4, 12, false};
      const int nGalaxies = static_cast<int>(std::clamp(t * 100.0F, 0.0F, 80.0F));
      for (int i = 0; i < nGalaxies; ++i) {
        const float gx = hash(i) * w;
        const float gy = hash(i * 7) * h;
        const float size = mn * 0.005F * (1.0F + 5.0F * hash(i * 13));
        const float tint = hash(i * 23);
        const Rgb col = tint > 0.66F ? Rgb{220, 180, 140, false}
                                      : (tint > 0.33F ? Rgb{180, 200, 230, false}
                                                       : Rgb{200, 180, 220, false});
        drawDataDisk(dst, w, h, src, gx, gy, size, ya, 0.7F, t * 0.5F + i, col);
      }
    });
}

void effectGalaxyCollision(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          dst[static_cast<std::size_t>(y) * w + x] = Rgb{4, 4, 12, false};
      const float approach = std::clamp(t * 1.5F, 0.0F, 1.0F);
      const float cx1 = w * (0.30F + 0.20F * approach);
      const float cy1 = h * 0.50F;
      const float cx2 = w * (0.70F - 0.20F * approach);
      const float cy2 = h * 0.50F;
      auto drawSpiral = [&](float cx, float cy, float r, float spinSign) {
        for (float a = 0; a < 6.2832F * 4; a += 0.05F) {
          const float rr = a * r / 25.0F;
          if (rr > r) break;
          const float ang = spinSign * a + t;
          const float px = cx + std::cos(ang) * rr;
          const float py = cy + std::sin(ang) * rr / ya;
          if (px < 0 || px >= w || py < 0 || py >= h) continue;
          const Rgb d = sample(src, w, h, static_cast<int>(px), static_cast<int>(py));
          const float dr = d.transparent ? 180.0F : d.r;
          plotDot(dst, w, h, px, py, std::max(1.0F, mn * 0.004F), ya,
                  Rgb{u8(180 + dr * 0.20F), u8(160), u8(220), false});
        }
      };
      drawSpiral(cx1, cy1, mn * 0.20F, 1.0F);
      drawSpiral(cx2, cy2, mn * 0.20F, -1.0F);
      // Central glow blob grows as they merge.
      const float mergeR = mn * 0.05F * approach;
      plotDot(dst, w, h, (cx1 + cx2) * 0.5F, (cy1 + cy2) * 0.5F, mergeR, ya,
              Rgb{220, 200, 240, false});
    });
}

void effectGasGiant(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          dst[static_cast<std::size_t>(y) * w + x] = Rgb{4, 4, 12, false};
      const float cx = w * 0.5F, cy = h * 0.5F;
      const float R = mn * 0.25F;
      // Disk fills (banded).
      for (int yy = static_cast<int>(cy - R / ya); yy <= static_cast<int>(cy + R / ya); ++yy)
        for (int xx = static_cast<int>(cx - R); xx <= static_cast<int>(cx + R); ++xx) {
          const float dx = (xx - cx) / R;
          const float dy = (yy - cy) * ya / R;
          if (dx * dx + dy * dy > 1.0F) continue;
          const Rgb d = sample(src, w, h, xx, yy);
          const float dr = d.transparent ? 200.0F : d.r;
          const float dg = d.transparent ? 160.0F : d.g;
          // Latitude band shading.
          const float band = std::sin(dy * 10.0F + t * 2.0F);
          const float r3 = std::sqrt(std::max(0.0F, 1.0F - dx * dx - dy * dy));  // for lighting
          dst[static_cast<std::size_t>(yy) * w + xx] =
              Rgb{u8((220 + 30 * band + dr * 0.10F) * (0.6F + 0.4F * r3)),
                  u8((160 + 30 * band + dg * 0.10F) * (0.6F + 0.4F * r3)),
                  u8((100 + 20 * band) * (0.6F + 0.4F * r3)), false};
        }
      // Ring system (ellipse).
      for (float a = 0; a < 6.2832F; a += 0.005F) {
        const float rR = R * 1.40F;
        const float rx = cx + std::cos(a) * rR;
        const float ry = cy + std::sin(a) * rR * 0.20F;
        if (rx >= 0 && rx < w && ry >= 0 && ry < h)
          plotDot(dst, w, h, rx, ry, std::max(1.0F, mn * 0.002F), ya,
                  Rgb{220, 200, 160, false});
      }
    });
}

void effectGravityLens(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          dst[static_cast<std::size_t>(y) * w + x] = Rgb{6, 6, 16, false};
      // Stars + distant galaxies behind.
      for (int i = 0; i < 40; ++i) {
        const float sx = hash(i) * w;
        const float sy = hash(i * 5) * h;
        plotDot(dst, w, h, sx, sy, mn * 0.005F, ya, Rgb{220, 220, 240, false});
      }
      const float cx = w * 0.5F, cy = h * 0.5F;
      // Foreground massive blob.
      drawDataDisk(dst, w, h, src, cx, cy, mn * 0.06F, ya, 0.90F, t * 0.2F, Rgb{160, 130, 80, false});
      // Einstein arcs at multiple radii.
      const float arcR1 = mn * 0.18F;
      const float arcR2 = mn * 0.25F;
      const float arcR3 = mn * 0.32F;
      const float intensity = std::clamp(t * 1.5F, 0.0F, 1.0F);
      for (float a = 0; a < 6.2832F; a += 0.01F) {
        for (float r : {arcR1, arcR2, arcR3}) {
          const float px = cx + std::cos(a) * r;
          const float py = cy + std::sin(a) * r / ya;
          if (px < 0 || px >= w || py < 0 || py >= h) continue;
          const Rgb d = sample(src, w, h, static_cast<int>(px), static_cast<int>(py));
          const float dr = d.transparent ? 220.0F : d.r;
          const float dg = d.transparent ? 200.0F : d.g;
          dst[static_cast<std::size_t>(static_cast<int>(py)) * w + static_cast<int>(px)] =
              Rgb{u8(dr * intensity * 0.7F + 60), u8(dg * intensity * 0.7F + 50), u8(140 * intensity), false};
        }
      }
    });
}

void effectJWST(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          dst[static_cast<std::size_t>(y) * w + x] = Rgb{4, 4, 14, false};
      const float open = std::clamp(t * 1.5F, 0.0F, 1.0F);
      // Hex mirror array (18 segments).
      const float cx = w * 0.5F, cy = h * 0.5F;
      const float hexR = mn * 0.06F;
      for (int ring = 0; ring < 3; ++ring) {
        const int n = ring == 0 ? 1 : (ring == 1 ? 6 : 11);
        for (int i = 0; i < n; ++i) {
          const float a = (ring == 0) ? 0 : (i / static_cast<float>(n)) * 6.2832F;
          const float dr = ring * hexR * 1.8F * open;
          const float hx = cx + std::cos(a) * dr;
          const float hy = cy + std::sin(a) * dr / ya;
          if (ring == 0 && i > 0) break;
          const Rgb d = sample(src, w, h, static_cast<int>(hx), static_cast<int>(hy));
          const float dr2 = d.transparent ? 220.0F : d.r;
          plotDot(dst, w, h, hx, hy, hexR * open, ya,
                  Rgb{u8(220 + dr2 * 0.1F), u8(200), u8(80), false});
        }
      }
      // Stars revealed as it opens.
      if (open > 0.4F) {
        for (int i = 0; i < 60; ++i) {
          const float sx = hash(i) * w;
          const float sy = hash(i * 3) * h;
          const float tw = std::clamp((open - 0.4F) * 1.7F, 0.0F, 1.0F);
          plotDot(dst, w, h, sx, sy, mn * 0.004F, ya,
                  Rgb{u8(220 * tw), u8(220 * tw), u8(240 * tw), false});
        }
      }
    });
}

void effectNeutronStar(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          dst[static_cast<std::size_t>(y) * w + x] = Rgb{4, 4, 12, false};
      // Stars.
      for (int i = 0; i < 60; ++i)
        dst[static_cast<std::size_t>(hash(i) * h) * w + static_cast<int>(hash(i * 3) * w)] =
            Rgb{180, 180, 220, false};
      const float cx = w * 0.5F, cy = h * 0.5F;
      // Tiny dense data sphere.
      drawDataDisk(dst, w, h, src, cx, cy, mn * 0.06F, ya, 0.95F, t * 3.0F, Rgb{220, 220, 255, false});
      // Magnetic field lines.
      for (int i = 0; i < 12; ++i) {
        const float ang0 = i / 12.0F * 6.2832F;
        for (float a = ang0 - 0.5F; a < ang0 + 0.5F; a += 0.01F) {
          const float r = mn * (0.08F + 0.05F * std::sin(a * 4));
          const float lx = cx + std::cos(a) * r;
          const float ly = cy + std::sin(a) * r / ya;
          plotDot(dst, w, h, lx, ly, std::max(1.0F, mn * 0.002F), ya, Rgb{120, 180, 255, false});
        }
      }
    });
}

void effectPulsar(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          dst[static_cast<std::size_t>(y) * w + x] = Rgb{4, 4, 12, false};
      for (int i = 0; i < 60; ++i)
        dst[static_cast<std::size_t>(hash(i) * h) * w + static_cast<int>(hash(i * 3) * w)] =
            Rgb{180, 180, 220, false};
      const float cx = w * 0.5F, cy = h * 0.5F;
      const float beamA = t * 6.2832F * 2.0F;  // 2 Hz pulsar
      for (int k = 0; k < 2; ++k) {
        const float ang = beamA + k * 3.14159F;
        for (float r = mn * 0.05F; r < mn * 0.50F; r += mn * 0.005F) {
          const float fade = 1.0F - r / (mn * 0.50F);
          const float bx = cx + std::cos(ang) * r;
          const float by = cy + std::sin(ang) * r / ya;
          plotDot(dst, w, h, bx, by, mn * 0.018F * fade, ya,
                  Rgb{u8(120 + 150 * fade), u8(180 + 60 * fade), u8(255), false});
        }
      }
      drawDataDisk(dst, w, h, src, cx, cy, mn * 0.04F, ya, 0.9F, t * 6.0F, Rgb{200, 220, 255, false});
    });
}

void effectSupernova(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          dst[static_cast<std::size_t>(y) * w + x] = Rgb{6, 6, 16, false};
      const float cx = w * 0.5F, cy = h * 0.5F;
      // Shockwave ring.
      const float ringR = mn * 0.50F * std::clamp(t * 1.5F, 0.0F, 1.0F);
      const float ringThk = mn * 0.025F * (1.0F + t);
      for (float a = 0; a < 6.2832F; a += 0.01F) {
        const float lx = cx + std::cos(a) * ringR;
        const float ly = cy + std::sin(a) * ringR / ya;
        if (lx >= 0 && lx < w && ly >= 0 && ly < h) {
          const Rgb d = sample(src, w, h, static_cast<int>(lx), static_cast<int>(ly));
          const float dr = d.transparent ? 220.0F : d.r;
          plotDot(dst, w, h, lx, ly, ringThk, ya,
                  Rgb{u8(240 + dr * 0.05F), u8(180), u8(100), false});
        }
      }
      // Ejecta dots within the ring.
      for (int i = 0; i < 80; ++i) {
        const float a = hash(i) * 6.2832F;
        const float r = ringR * (0.3F + 0.7F * hash(i * 3));
        const float px = cx + std::cos(a) * r;
        const float py = cy + std::sin(a) * r / ya;
        plotDot(dst, w, h, px, py, mn * 0.005F, ya, Rgb{u8(255), u8(220 * (1 - t)), u8(120), false});
      }
      // Bright central flash.
      const float flash = std::clamp(1.0F - t * 2.5F, 0.0F, 1.0F);
      plotDot(dst, w, h, cx, cy, mn * 0.08F * flash, ya,
              Rgb{u8(255 * flash), u8(240 * flash), u8(220 * flash), false});
    });
}

void effectSolarEclipse(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 6000,
    [&](float t, std::vector<Rgb>& dst) {
      // Daytime sky darkens dramatically at totality.
      const float total = 1.0F - std::exp(-((t - 0.50F) * (t - 0.50F)) / 0.005F);
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
          const float l = s.transparent ? 60.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
          const float sf = static_cast<float>(y) / h;
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8((180 - 100 * sf + l * 0.10F) * (1 - 0.85F * total)),
                  u8((200 - 90 * sf + l * 0.10F) * (1 - 0.80F * total)),
                  u8((230 - 70 * sf + l * 0.10F) * (1 - 0.50F * total)), false};
        }
      // Sun.
      const float cx = w * 0.5F, cy = h * 0.45F;
      const float R = mn * 0.12F;
      drawDataDisk(dst, w, h, src, cx, cy, R, ya, 0.4F, t * 0.3F, Rgb{255, 230, 160, false});
      // Corona during totality: streamers radiating data.
      if (total > 0.05F) {
        const int nRay = 40;
        for (int i = 0; i < nRay; ++i) {
          const float a = i * 6.2832F / nRay;
          for (float r = R; r < R * 2.5F; r += R * 0.04F) {
            const float fade = total * (1.0F - (r - R) / (R * 1.5F));
            if (fade <= 0) break;
            const float rx = cx + std::cos(a) * r;
            const float ry = cy + std::sin(a) * r / ya;
            const Rgb d = sample(src, w, h, static_cast<int>(rx), static_cast<int>(ry));
            const float dr = d.transparent ? 200.0F : d.r;
            plotDot(dst, w, h, rx, ry, std::max(1.0F, mn * 0.003F), ya,
                    Rgb{u8(220 * fade + dr * 0.1F * fade), u8(220 * fade), u8(180 * fade), false});
          }
        }
      }
      // Moon transits left → right, eclipsing at t ≈ 0.5.
      const float mx = cx + (t - 0.5F) * 2.0F * R * 1.4F;
      const float my = cy;
      // Moon = solid black disk (slightly larger than Sun for totality).
      const float Rm = R * 1.03F;
      for (int yo = -static_cast<int>(Rm / ya); yo <= static_cast<int>(Rm / ya); ++yo)
        for (int xo = -static_cast<int>(Rm); xo <= static_cast<int>(Rm); ++xo) {
          const float u = xo / Rm, vN = yo * ya / Rm;
          if (u * u + vN * vN > 1.0F) continue;
          const int xx = static_cast<int>(mx + xo), yy = static_cast<int>(my + yo);
          if (xx >= 0 && xx < w && yy >= 0 && yy < h)
            dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{8, 8, 12, false};
        }
    });
}

void effectLunarEclipse(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
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
      // Stars.
      for (int i = 0; i < 80; ++i) {
        const int sx = static_cast<int>(hash(i) * w);
        const int sy = static_cast<int>(hash(i * 3) * h);
        dst[static_cast<std::size_t>(sy) * w + sx] = Rgb{200, 200, 220, false};
      }
      // Umbra: a large faint reddish disk centred on screen.
      const float cx = w * 0.5F, cy = h * 0.5F;
      const float Ru = mn * 0.32F;
      for (int yo = -static_cast<int>(Ru / ya); yo <= static_cast<int>(Ru / ya); ++yo)
        for (int xo = -static_cast<int>(Ru); xo <= static_cast<int>(Ru); ++xo) {
          const float u = xo / Ru, vN = yo * ya / Ru;
          const float rN = u * u + vN * vN;
          if (rN > 1.0F) continue;
          const int xx = static_cast<int>(cx + xo), yy = static_cast<int>(cy + yo);
          if (xx >= 0 && xx < w && yy >= 0 && yy < h) {
            const float fade = 1.0F - rN;
            dst[static_cast<std::size_t>(yy) * w + xx] =
                Rgb{u8(60 * fade), u8(18 * fade), u8(10 * fade), false};
          }
        }
      // Moon drifts from left to right.
      const float mx = w * 0.15F + t * w * 0.70F;
      const float my = cy;
      const float Rm = mn * 0.07F;
      const float dCx = (mx - cx) / Ru;
      const float inside = std::clamp(1.0F - std::fabs(dCx), 0.0F, 1.0F);
      // Inside umbra: tint moon deep red; outside: data-textured grey.
      const Rgb tint = Rgb{u8(120 + 120 * inside), u8(120 - 80 * inside), u8(120 - 80 * inside), false};
      drawDataDisk(dst, w, h, src, mx, my, Rm, ya, 0.85F, t * 0.4F, tint);
    });
}

void effectSagittariusA(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 6000,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          dst[static_cast<std::size_t>(y) * w + x] = Rgb{4, 4, 14, false};
      for (int i = 0; i < 80; ++i)
        dst[static_cast<std::size_t>(hash(i) * h) * w + static_cast<int>(hash(i * 3) * w)] =
            Rgb{180, 180, 220, false};
      const float cx = w * 0.5F, cy = h * 0.5F;
      // Event-horizon shadow + photon ring.
      plotDot(dst, w, h, cx, cy, mn * 0.04F, ya, Rgb{0, 0, 0, false});
      for (float a = 0; a < 6.2832F; a += 0.01F)
        plotDot(dst, w, h, cx + std::cos(a) * mn * 0.05F, cy + std::sin(a) * mn * 0.05F / ya,
                std::max(1.0F, mn * 0.002F), ya, Rgb{240, 180, 80, false});
      // S-stars on elliptical orbits, period and eccentricity vary per star.
      struct SStar { float a, e, period, theta0, size; };
      const SStar stars[] = {
          {mn * 0.18F, 0.88F, 1.0F, 0.0F, mn * 0.012F},
          {mn * 0.25F, 0.45F, 1.6F, 1.5F, mn * 0.009F},
          {mn * 0.30F, 0.30F, 2.2F, 3.0F, mn * 0.008F},
          {mn * 0.38F, 0.20F, 3.0F, 4.5F, mn * 0.010F},
      };
      for (const auto& s : stars) {
        const float th = s.theta0 + t * 6.2832F / s.period;
        // Elliptical orbit (a = semi-major, e = eccentricity).
        const float r = s.a * (1.0F - s.e * s.e) / (1.0F + s.e * std::cos(th));
        const float sx = cx + r * std::cos(th);
        const float syp = cy + r * std::sin(th) / ya;
        drawDataDisk(dst, w, h, src, sx, syp, s.size, ya, 0.7F, t, Rgb{255, 240, 200, false});
        // Faint orbit ellipse.
        for (float a = 0; a < 6.2832F; a += 0.05F) {
          const float rr = s.a * (1.0F - s.e * s.e) / (1.0F + s.e * std::cos(a));
          const int ox = static_cast<int>(cx + rr * std::cos(a));
          const int oy = static_cast<int>(cy + rr * std::sin(a) / ya);
          if (ox >= 0 && ox < w && oy >= 0 && oy < h)
            dst[static_cast<std::size_t>(oy) * w + ox] = Rgb{60, 60, 100, false};
        }
      }
    });
}

void effectWormhole(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 6000,
    [&](float t, std::vector<Rgb>& dst) {
      const float cx = w * 0.5F, cy = h * 0.5F;
      const float Rmajor = mn * 0.30F;
      const float Rminor = mn * 0.10F;
      for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx) {
          const float dx = xx - cx;
          const float dy = (yy - cy) * ya;
          const float r = std::sqrt(dx * dx + dy * dy);
          const float ang = std::atan2(dy, dx);
          // Distance from the torus ring centre.
          const float dRing = std::fabs(r - Rmajor);
          if (dRing < Rminor) {
            // Inside the torus — sample data from a swirled angle.
            const float swirl = ang + (Rminor - dRing) / Rminor * 6.0F + t * 4.0F;
            const float sx = cx + std::cos(swirl) * Rmajor;
            const float syp = cy + std::sin(swirl) * Rmajor / ya;
            const Rgb d = sample(src, w, h, static_cast<int>(sx), static_cast<int>(syp));
            const float dr = d.transparent ? 100.0F : d.r;
            const float dg = d.transparent ? 100.0F : d.g;
            const float db = d.transparent ? 150.0F : d.b;
            const float bright = 1.0F - dRing / Rminor;
            dst[static_cast<std::size_t>(yy) * w + xx] =
                Rgb{u8(dr * bright + 60 * (1 - bright)), u8(dg * bright + 80 * (1 - bright)),
                    u8(db * bright + 160 * (1 - bright)), false};
          } else if (r < Rmajor - Rminor) {
            // Throat of the wormhole: data from the "other side".
            const Rgb d = sample(src, w, h, w - 1 - xx, h - 1 - yy);
            const float dr = d.transparent ? 60.0F : d.r;
            const float dg = d.transparent ? 60.0F : d.g;
            const float db = d.transparent ? 120.0F : d.b;
            const float fade = r / (Rmajor - Rminor);
            dst[static_cast<std::size_t>(yy) * w + xx] =
                Rgb{u8(dr * fade), u8(dg * fade), u8(db * fade), false};
          } else {
            const Rgb d = sample(src, w, h, xx, yy);
            const float dl = d.transparent ? 30.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
            dst[static_cast<std::size_t>(yy) * w + xx] =
                Rgb{u8(8 + dl * 0.10F), u8(8 + dl * 0.10F), u8(20 + dl * 0.10F), false};
          }
        }
    });
}

void effectMarsRover(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 6000,
    [&](float t, std::vector<Rgb>& dst) {
      // Mars sky (butterscotch) + ground (rusty).
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb d = sample(src, w, h, x, y);
          const float dl = d.transparent ? 60.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          const float sf = static_cast<float>(y) / h;
          const bool ground = y > h * 0.65F;
          dst[static_cast<std::size_t>(y) * w + x] =
              ground ? Rgb{u8(180 + dl * 0.30F - 30 * (sf - 0.65F) * 2),
                            u8(80 + dl * 0.20F), u8(50 + dl * 0.10F), false}
                     : Rgb{u8(220 + dl * 0.10F - 40 * sf), u8(160 + dl * 0.10F - 30 * sf),
                            u8(120 + dl * 0.05F), false};
        }
      // Olympus Mons silhouette.
      const float horizonY = h * 0.65F;
      for (int xx = 0; xx < w; ++xx) {
        const float xf = static_cast<float>(xx) / w;
        const float mountainBase = (xf > 0.20F && xf < 0.80F)
                                       ? std::sin((xf - 0.20F) / 0.60F * 3.14159F) * mn * 0.08F
                                       : 0.0F;
        const int peakY = static_cast<int>(horizonY - mountainBase);
        for (int yy = peakY; yy < static_cast<int>(horizonY); ++yy)
          if (yy >= 0 && yy < h)
            dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{160, 70, 40, false};
      }
      // Wheel tracks (data-tinted ruts left behind).
      for (int i = 0; i < 40; ++i) {
        const float trackX = w * 0.10F + i * w * 0.018F * t;
        if (trackX > w * 0.50F) break;
        for (int yo = 0; yo < 3; ++yo)
          plotDot(dst, w, h, trackX, h * 0.92F + yo, std::max(1.0F, mn * 0.003F), ya,
                  Rgb{120, 50, 30, false});
        plotDot(dst, w, h, trackX, h * 0.96F, std::max(1.0F, mn * 0.003F), ya,
                Rgb{120, 50, 30, false});
      }
      // Rover.
      const float rx = w * 0.10F + t * w * 0.40F;
      const float ry = h * 0.88F;
      const Rgb chassis{200, 200, 200, false};
      // Body (data-textured).
      for (int yo = -static_cast<int>(mn * 0.020F); yo <= 0; ++yo)
        for (int xo = -static_cast<int>(mn * 0.035F); xo <= static_cast<int>(mn * 0.035F); ++xo) {
          const int xx = static_cast<int>(rx + xo);
          const int yy = static_cast<int>(ry + yo);
          if (xx < 0 || xx >= w || yy < 0 || yy >= h) continue;
          const Rgb d = sample(src, w, h, xx, yy);
          const float dr = d.transparent ? 200.0F : d.r;
          dst[static_cast<std::size_t>(yy) * w + xx] =
              Rgb{u8(180 + dr * 0.20F), u8(180), u8(160), false};
        }
      // Wheels (6 visible).
      for (int wh = 0; wh < 3; ++wh) {
        const float wx = rx - mn * 0.030F + wh * mn * 0.030F;
        plotDot(dst, w, h, wx, ry + mn * 0.010F, mn * 0.012F, ya, Rgb{40, 40, 40, false});
      }
      // Mast + camera.
      drawSeg(dst, w, h, rx, ry - mn * 0.020F, rx, ry - mn * 0.050F, std::max(1.0F, mn * 0.004F), ya,
              chassis);
      plotDot(dst, w, h, rx, ry - mn * 0.054F, mn * 0.010F, ya, Rgb{120, 120, 140, false});
      // Solar panel above body.
      drawSeg(dst, w, h, rx - mn * 0.04F, ry - mn * 0.025F, rx + mn * 0.04F, ry - mn * 0.025F,
              std::max(1.0F, mn * 0.005F), ya, Rgb{30, 30, 80, false});
    });
}

void effectSolarFlare(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 5800,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          dst[static_cast<std::size_t>(y) * w + x] = Rgb{8, 6, 18, false};
      // Sun fills the left half.
      const float cx = w * 0.30F, cy = h * 0.50F;
      const float R = mn * 0.25F;
      drawDataDisk(dst, w, h, src, cx, cy, R, ya, 0.5F, t * 0.3F, Rgb{255, 180, 80, false});
      // Surface granulation: random small dots, data-tinted, on the disk.
      for (int i = 0; i < 120; ++i) {
        const float ang = hash(i) * 6.2832F;
        const float rr = hash(i * 3) * R * 0.95F;
        const float gx = cx + std::cos(ang) * rr;
        const float gy = cy + std::sin(ang) * rr / ya;
        const Rgb d = sample(src, w, h, static_cast<int>(gx), static_cast<int>(gy));
        const float dr = d.transparent ? 220.0F : d.r;
        plotDot(dst, w, h, gx, gy, mn * 0.010F, ya, Rgb{u8(255), u8(180 + dr * 0.10F), u8(40), false});
      }
      // Prominence arch: a loop of fire rising from the limb.
      const float erupt = std::clamp(t * 1.8F, 0.0F, 1.0F);
      const float pang0 = 2.6F, pang1 = 3.7F;  // limb angles in radians
      const float pX0 = cx + std::cos(pang0) * R;
      const float pY0 = cy + std::sin(pang0) * R / ya;
      const float pX1 = cx + std::cos(pang1) * R;
      const float pY1 = cy + std::sin(pang1) * R / ya;
      const int nP = 30;
      for (int k = 0; k <= nP; ++k) {
        const float f = k / static_cast<float>(nP);
        const float mxp = pX0 + f * (pX1 - pX0) + 4.0F * f * (1.0F - f) * (-mn * 0.20F) * erupt;
        const float myp = pY0 + f * (pY1 - pY0) - 4.0F * f * (1.0F - f) * mn * 0.05F;
        const Rgb d = sample(src, w, h, static_cast<int>(mxp), static_cast<int>(myp));
        const float dr = d.transparent ? 230.0F : d.r;
        plotDot(dst, w, h, mxp, myp, mn * 0.016F, ya,
                Rgb{u8(255), u8(150 + dr * 0.10F), u8(40), false});
      }
      // CME blast outward to the right at late t.
      const float blast = std::clamp((t - 0.40F) / 0.40F, 0.0F, 1.0F);
      for (int i = 0; i < 80 && blast > 0.05F; ++i) {
        const float ph = hash(i) * 6.2832F * 0.5F + 5.6F;  // direction cone, right-ish
        const float pr = R * 1.05F + blast * mn * 0.5F * (0.5F + hash(i * 5));
        const float px = cx + std::cos(ph) * pr;
        const float py = cy + std::sin(ph) * pr / ya;
        plotDot(dst, w, h, px, py, mn * 0.010F * blast, ya, Rgb{255, 120, 40, false});
      }
    });
}

void effectPillarsOfCreation(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 6000,
    [&](float t, std::vector<Rgb>& dst) {
      // Bright nebula background: hot pink + teal blend with data.
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb d = sample(src, w, h, x, y);
          const float dl = d.transparent ? 80.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          const float sf = static_cast<float>(y) / h;
          const float swirl = std::sin(x * 0.03F + y * 0.04F + t * 0.5F);
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(180 + 40 * swirl + dl * 0.30F - 50 * sf),
                  u8(80 + 30 * swirl + dl * 0.20F + 30 * sf),
                  u8(140 + 60 * (-swirl) + dl * 0.30F + 30 * sf), false};
        }
      // Stars dotted through.
      for (int i = 0; i < 60; ++i) {
        const int sx = static_cast<int>(hash(i) * w);
        const int sy = static_cast<int>(hash(i * 3) * h);
        const float tw = 0.6F + 0.4F * std::sin(t * 6.0F + i);
        dst[static_cast<std::size_t>(sy) * w + sx] = Rgb{u8(220 * tw), u8(220 * tw), u8(255 * tw), false};
      }
      // Three pillars, tapered, fingertips at top.
      auto drawPillar = [&](float cx, float baseW, float topW, float bottomY, float topY) {
        for (int yy = static_cast<int>(topY); yy <= static_cast<int>(bottomY); ++yy) {
          const float yf = (yy - topY) / (bottomY - topY);
          const float halfW = baseW * 0.5F * yf + topW * 0.5F * (1.0F - yf);
          // Add wavy edges.
          const float wave = std::sin(yy * 0.1F) * mn * 0.012F;
          for (int xo = -static_cast<int>(halfW + wave); xo <= static_cast<int>(halfW + wave); ++xo) {
            const int xx = static_cast<int>(cx + xo);
            if (xx >= 0 && xx < w && yy >= 0 && yy < h) {
              const float xf = static_cast<float>(xo) / halfW;
              const float rim = std::fabs(xf) > 0.85F ? 1.0F : 0.0F;
              const Rgb d = sample(src, w, h, xx, yy);
              const float dr = d.transparent ? 60.0F : d.r;
              dst[static_cast<std::size_t>(yy) * w + xx] =
                  Rgb{u8(30 + dr * 0.10F + rim * 80), u8(20 + rim * 40), u8(20 + rim * 60), false};
            }
          }
        }
        // Fingertip glow at top (rim of evaporating gas).
        for (int k = -5; k <= 5; ++k)
          plotDot(dst, w, h, cx + k, topY, std::max(1.0F, mn * 0.004F), ya,
                  Rgb{255, 200, 140, false});
      };
      drawPillar(w * 0.25F, mn * 0.08F, mn * 0.04F, h * 0.95F, h * 0.30F);
      drawPillar(w * 0.50F, mn * 0.10F, mn * 0.05F, h * 0.95F, h * 0.20F);
      drawPillar(w * 0.75F, mn * 0.06F, mn * 0.03F, h * 0.95F, h * 0.40F);
    });
}

void effectLigoChirp(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  runFrames(renderer, w, h, 6000,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          dst[static_cast<std::size_t>(y) * w + x] = Rgb{6, 6, 18, false};
      // Two black hole point masses spiralling inward. Separation r ∝ (1-t)^{1/4}.
      const float merge = std::clamp(t * 1.1F, 0.0F, 1.0F);
      const float r = mn * 0.20F * std::pow(std::max(0.05F, 1.0F - merge), 0.25F);
      const float omega = 4.0F * std::pow(std::max(0.05F, 1.0F - merge), -0.6F);  // chirp
      const float ang = omega * t * 6.2832F;
      const float cx = w * 0.5F, cy = h * 0.35F;
      const float sx1 = cx + std::cos(ang) * r, sy1 = cy + std::sin(ang) * r / ya;
      const float sx2 = cx - std::cos(ang) * r, sy2 = cy - std::sin(ang) * r / ya;
      if (merge < 0.95F) {
        drawDataDisk(dst, w, h, src, sx1, sy1, mn * 0.025F, ya, 0.95F, t * 2.0F, Rgb{180, 60, 20, false});
        drawDataDisk(dst, w, h, src, sx2, sy2, mn * 0.025F, ya, 0.95F, -t * 2.0F, Rgb{20, 60, 180, false});
      } else {
        // Single merged black hole + ringdown flash.
        drawDataDisk(dst, w, h, src, cx, cy, mn * 0.05F, ya, 0.95F, t * 4.0F, Rgb{255, 240, 220, false});
      }
      // Chirp waveform underneath: strain h(t) ∝ A(t) * cos(ωt). A grows
      // until merger, then ring-down dies.
      const float wy0 = h * 0.75F;
      const float wAmp = mn * 0.10F;
      for (int xx = 0; xx < w; ++xx) {
        const float xf = static_cast<float>(xx) / w;
        const float aT = xf * t;  // up to current "now" only — leading edge is blank
        const float amp = (aT < t) ? wAmp * std::pow(std::max(0.05F, 1.0F - aT), -0.25F) : 0.0F;
        const float w_x = 4.0F * std::pow(std::max(0.05F, 1.0F - aT), -0.6F);
        const float strain = (aT < t) ? amp * std::cos(w_x * aT * 12.0F) : 0.0F;
        const float yy = wy0 - strain;
        plotDot(dst, w, h, static_cast<float>(xx), yy, std::max(1.0F, mn * 0.003F), ya,
                Rgb{120, 220, 240, false});
      }
      // Axis line.
      for (int xx = 0; xx < w; ++xx)
        dst[static_cast<std::size_t>(wy0) * w + xx] = Rgb{60, 80, 100, false};
    });
}

void effectCosmicWeb(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 6000,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          dst[static_cast<std::size_t>(y) * w + x] = Rgb{4, 4, 14, false};
      // Anchor nodes (galaxy clusters): placed in a noisy grid.
      const int nNodes = 32;
      struct Node { float x, y, r; };
      Node nd[64];
      for (int i = 0; i < nNodes; ++i) {
        nd[i].x = (hash(i) * 0.9F + 0.05F) * w;
        nd[i].y = (hash(i * 3) * 0.9F + 0.05F) * h;
        nd[i].r = mn * (0.010F + 0.020F * hash(i * 7));
      }
      // Connect each node to its 2-3 nearest neighbours.
      const float fade = std::clamp(t * 1.5F, 0.0F, 1.0F);
      for (int i = 0; i < nNodes; ++i) {
        float best1 = 1e9F, best2 = 1e9F, best3 = 1e9F;
        int b1 = -1, b2 = -1, b3 = -1;
        for (int j = 0; j < nNodes; ++j) {
          if (j == i) continue;
          const float dx = nd[i].x - nd[j].x;
          const float dy = nd[i].y - nd[j].y;
          const float d2 = dx * dx + dy * dy;
          if (d2 < best1) { best3 = best2; b3 = b2; best2 = best1; b2 = b1; best1 = d2; b1 = j; }
          else if (d2 < best2) { best3 = best2; b3 = b2; best2 = d2; b2 = j; }
          else if (d2 < best3) { best3 = d2; b3 = j; }
        }
        for (int j : {b1, b2, b3}) {
          if (j < 0) continue;
          // Filament: draw a thin data-tinted line between (i, j).
          const int nSeg = 30;
          for (int k = 0; k <= nSeg; ++k) {
            const float f = k / static_cast<float>(nSeg);
            const float fx = nd[i].x + f * (nd[j].x - nd[i].x);
            const float fy = nd[i].y + f * (nd[j].y - nd[i].y);
            const Rgb d = sample(src, w, h, static_cast<int>(fx), static_cast<int>(fy));
            const float dr = d.transparent ? 120.0F : d.r;
            plotDot(dst, w, h, fx, fy, std::max(1.0F, mn * 0.002F), ya,
                    Rgb{u8((140 + dr * 0.20F) * fade), u8((120 + dr * 0.15F) * fade),
                        u8((200) * fade), false});
          }
        }
      }
      // Cluster nodes drawn on top — data-textured.
      for (int i = 0; i < nNodes; ++i)
        drawDataDisk(dst, w, h, src, nd[i].x, nd[i].y, nd[i].r * fade, ya, 0.75F, t,
                     Rgb{220, 200, 240, false});
    });
}

void effectSpaghettify(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  runFrames(renderer, w, h, 6000,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          dst[static_cast<std::size_t>(y) * w + x] = Rgb{6, 6, 16, false};
      // Black hole on the right.
      const float bhX = w * 0.75F, bhY = h * 0.5F;
      plotDot(dst, w, h, bhX, bhY, mn * 0.05F, ya, Rgb{0, 0, 0, false});
      for (float a = 0; a < 6.2832F; a += 0.01F)
        plotDot(dst, w, h, bhX + std::cos(a) * mn * 0.06F, bhY + std::sin(a) * mn * 0.06F / ya,
                std::max(1.0F, mn * 0.002F), ya, Rgb{240, 160, 60, false});
      // Star spaghettifies toward the BH.
      const float pull = std::clamp(t * 1.4F, 0.0F, 1.0F);
      const float starX0 = w * 0.20F, starY0 = h * 0.50F;
      const float starX = starX0 + pull * (bhX - mn * 0.10F - starX0);
      // Stretch length proportional to pull.
      const float stretchLen = pull * mn * 0.45F;
      const int nSeg = 30;
      for (int k = 0; k <= nSeg; ++k) {
        const float f = k / static_cast<float>(nSeg);
        const float sx = starX + f * stretchLen;
        const float ax = std::atan2(bhY - sx, bhX - sx);
        (void)ax;
        const float sw = mn * 0.05F * (1.0F - 0.6F * f) * (1.0F - 0.7F * pull * f);
        // Trailing noodle is data-textured.
        drawDataDisk(dst, w, h, src, sx, starY0, sw, ya, 0.7F, t + k, Rgb{255, 180, 80, false});
      }
    });
}

void effectMagnetar(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 5800,
    [&](float t, std::vector<Rgb>& dst) {
      const bool flare = std::fmod(t * 3.0F, 1.0F) < 0.06F;
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
          const float l = s.transparent ? 30.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
          const float fb = flare ? 100.0F : 0.0F;
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(20 + l * 0.10F + fb * 1.0F), u8(20 + l * 0.10F + fb * 0.6F),
                  u8(40 + l * 0.15F + fb * 0.3F), false};
        }
      // Stars.
      for (int i = 0; i < 50; ++i)
        dst[static_cast<std::size_t>(hash(i) * h) * w + static_cast<int>(hash(i * 3) * w)] =
            Rgb{180, 180, 220, false};
      const float cx = w * 0.5F, cy = h * 0.5F;
      // Magnetar (data-textured tiny sphere).
      drawDataDisk(dst, w, h, src, cx, cy, mn * 0.04F, ya, 0.95F, t * 5.0F, Rgb{200, 220, 255, false});
      // Magnetic field loops — dipole shape, twisted.
      const int nLoops = 16;
      for (int k = 0; k < nLoops; ++k) {
        const float baseAng = k * 6.2832F / nLoops + t * 0.5F;
        const int nPts = 40;
        for (int p = 0; p <= nPts; ++p) {
          const float pf = p / static_cast<float>(nPts);
          // Loop parameterisation: dipole curve.
          const float lat = (pf - 0.5F) * 3.14159F;
          const float r = mn * 0.14F * std::cos(lat) * std::cos(lat);
          const float lx = cx + std::cos(baseAng + lat * 0.5F) * r;
          const float ly = cy + std::sin(baseAng + lat * 0.5F) * r / ya;
          plotDot(dst, w, h, lx, ly, std::max(1.0F, mn * 0.002F), ya, Rgb{180, 160, 255, false});
        }
      }
      // X-ray burst beams when flare is on.
      if (flare) {
        for (int k = 0; k < 2; ++k) {
          const float ang = t * 8.0F + k * 3.14159F;
          for (float rr = mn * 0.05F; rr < mn * 0.45F; rr += mn * 0.005F) {
            const float fade = 1.0F - rr / (mn * 0.45F);
            const float bx = cx + std::cos(ang) * rr;
            const float by = cy + std::sin(ang) * rr / ya;
            plotDot(dst, w, h, bx, by, mn * 0.015F * fade, ya,
                    Rgb{255, u8(220 * fade), u8(120 * fade), false});
          }
        }
      }
    });
}

void effectHalley(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 6000,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          dst[static_cast<std::size_t>(y) * w + x] = Rgb{8, 8, 22, false};
      for (int i = 0; i < 80; ++i)
        dst[static_cast<std::size_t>(hash(i) * h) * w + static_cast<int>(hash(i * 3) * w)] =
            Rgb{180, 180, 220, false};
      // Sun at the right focus.
      const float sx = w * 0.70F, sy = h * 0.55F;
      drawDataDisk(dst, w, h, src, sx, sy, mn * 0.06F, ya, 0.5F, t * 0.3F, Rgb{255, 230, 160, false});
      // Comet on ellipse with high eccentricity. a = semi-major (in
      // screen units), e ≈ 0.97 for Halley.
      const float a = mn * 0.40F;
      const float e = 0.85F;
      const float theta = -3.0F + t * 6.2832F * 1.0F;  // sweep through perihelion
      const float r = a * (1.0F - e * e) / (1.0F + e * std::cos(theta));
      const float cx = sx + r * std::cos(theta);
      const float cy = sy + r * std::sin(theta) / ya;
      // Tail length scales with proximity to the Sun.
      const float dx = sx - cx, dy = sy - cy;
      const float dist = std::sqrt(dx * dx + dy * dy);
      const float tailLen = std::clamp(mn * 0.40F * (1.0F - dist / (a * 1.2F)), 0.0F, mn * 0.40F);
      // Dust tail (yellow, curved away from sun).
      const float ux = (cx - sx) / dist, uy = (cy - sy) / dist;
      for (int k = 0; k < 60; ++k) {
        const float f = k / 60.0F;
        const float tx = cx + ux * tailLen * f + (hash(k) - 0.5F) * mn * 0.02F;
        const float ty = cy + uy * tailLen * f + (hash(k * 3) - 0.5F) * mn * 0.02F;
        plotDot(dst, w, h, tx, ty, mn * 0.010F * (1 - f), ya,
                Rgb{u8(255 * (1 - f)), u8(220 * (1 - f * 0.8F)), u8(140 * (1 - f)), false});
      }
      // Comet head (data-textured).
      drawDataDisk(dst, w, h, src, cx, cy, mn * 0.018F, ya, 0.5F, t * 2.0F, Rgb{220, 230, 255, false});
      // Faint orbit ellipse.
      for (float a2 = 0; a2 < 6.2832F; a2 += 0.02F) {
        const float rr = a * (1.0F - e * e) / (1.0F + e * std::cos(a2));
        const int ox = static_cast<int>(sx + rr * std::cos(a2));
        const int oy = static_cast<int>(sy + rr * std::sin(a2) / ya);
        if (ox >= 0 && ox < w && oy >= 0 && oy < h)
          dst[static_cast<std::size_t>(oy) * w + ox] = Rgb{50, 50, 90, false};
      }
    });
}

void effectDayTerminator(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 6500,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          dst[static_cast<std::size_t>(y) * w + x] = Rgb{4, 4, 12, false};
      // Stars over the dark side.
      for (int i = 0; i < 100; ++i)
        dst[static_cast<std::size_t>(hash(i) * h) * w + static_cast<int>(hash(i * 3) * w)] =
            Rgb{180, 180, 220, false};
      const float cx = w * 0.5F, cy = h * 0.5F;
      const float R = mn * 0.42F;
      const float centerLatDeg = 15.0F;
      const float centerLonDeg = std::fmod(t * 360.0F, 360.0F) - 180.0F;  // rotate east
      const float cLat = std::cos(centerLatDeg * 3.14159F / 180.0F);
      const float sLat = std::sin(centerLatDeg * 3.14159F / 180.0F);
      // Solar direction (fixed in lon=0).
      const float sunLat = 23.5F * std::sin(t * 6.2832F);  // axial tilt seasonal
      const float sunLonAbs = 0.0F;
      const float slatR = sunLat * 3.14159F / 180.0F;
      const float slonR = sunLonAbs * 3.14159F / 180.0F;
      const float sunDx3 = std::cos(slatR) * std::sin(slonR);
      const float sunDy3 = std::sin(slatR);
      const float sunDz3 = std::cos(slatR) * std::cos(slonR);
      for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx) {
          float lat, lon;
          if (!globePxToLatLon(cx, cy, R, ya, cLat, sLat, centerLonDeg, xx, yy, lat, lon)) continue;
          const float latR = lat * 3.14159F / 180.0F;
          const float lonR = lon * 3.14159F / 180.0F;
          const float p3x = std::cos(latR) * std::sin(lonR);
          const float p3y = std::sin(latR);
          const float p3z = std::cos(latR) * std::cos(lonR);
          // Lit if surface normal dotted with sun direction > 0.
          const float lit = std::max(0.0F, p3x * sunDx3 + p3y * sunDy3 + p3z * sunDz3);
          const Rgb d = sample(src, w, h, xx, yy);
          const float dl = d.transparent ? 50.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          // Day = blue ocean + data; night = dim violet + city lights (random).
          if (lit > 0.05F) {
            dst[static_cast<std::size_t>(yy) * w + xx] =
                Rgb{u8((60 + dl * 0.25F) * lit), u8((100 + dl * 0.30F) * lit),
                    u8((160 + dl * 0.30F) * lit), false};
          } else {
            dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{18, 14, 30, false};
            if (hash(static_cast<int>(lat * 7 + lon * 11)) > 0.97F)
              dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{240, 200, 80, false};  // city lights
          }
        }
    });
}

void effectIssTrack(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
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
      const float centerLatDeg = 20.0F, centerLonDeg = 0.0F;
      const float cLat = std::cos(centerLatDeg * 3.14159F / 180.0F);
      const float sLat = std::sin(centerLatDeg * 3.14159F / 180.0F);
      for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx) {
          float lat, lon;
          if (!globePxToLatLon(cx, cy, R, ya, cLat, sLat, centerLonDeg, xx, yy, lat, lon)) continue;
          const Rgb d = sample(src, w, h, xx, yy);
          const float dl = d.transparent ? 50.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          dst[static_cast<std::size_t>(yy) * w + xx] =
              Rgb{u8(30 + dl * 0.25F), u8(70 + dl * 0.25F), u8(120 + dl * 0.30F), false};
        }
      // Ground track: lat = 51.6° * sin(2π * phase). Each orbit takes 90 min,
      // Earth rotates ~22.5° west per orbit. Draw 3 successive orbits.
      const float inc = 51.6F;
      for (int orbit = 0; orbit < 3; ++orbit) {
        for (int k = 0; k <= 200; ++k) {
          const float ph = k / 200.0F;
          const float lat = inc * std::sin(ph * 6.2832F);
          const float lon = std::fmod(ph * 360.0F - 22.5F * orbit + 540.0F, 360.0F) - 180.0F;
          float px, py;
          if (globeLatLonToPx(cx, cy, R, ya, cLat, sLat, centerLonDeg, lat, lon, px, py))
            plotDot(dst, w, h, px, py, std::max(1.0F, mn * 0.003F), ya,
                    Rgb{u8(180 - orbit * 50), u8(180 - orbit * 50), u8(220 - orbit * 30), false});
        }
      }
      // ISS dot on the current orbit (orbit 0).
      const float ph = std::fmod(t * 2.0F, 1.0F);
      const float lat = inc * std::sin(ph * 6.2832F);
      const float lon = std::fmod(ph * 360.0F + 540.0F, 360.0F) - 180.0F;
      float px, py;
      if (globeLatLonToPx(cx, cy, R, ya, cLat, sLat, centerLonDeg, lat, lon, px, py)) {
        plotDot(dst, w, h, px, py, mn * 0.012F, ya, Rgb{255, 240, 160, false});
        // Glint trail.
        for (int k = 1; k < 8; ++k) {
          const float ph2 = ph - k * 0.005F;
          const float la2 = inc * std::sin(ph2 * 6.2832F);
          const float lo2 = std::fmod(ph2 * 360.0F + 540.0F, 360.0F) - 180.0F;
          float px2, py2;
          if (globeLatLonToPx(cx, cy, R, ya, cLat, sLat, centerLonDeg, la2, lo2, px2, py2))
            plotDot(dst, w, h, px2, py2, mn * 0.006F * (1 - k * 0.12F), ya, Rgb{220, 200, 100, false});
        }
      }
    });
}

void effectMagnetosphere(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 6000,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          dst[static_cast<std::size_t>(y) * w + x] = Rgb{4, 4, 14, false};
      // Earth on the left-of-centre.
      const float cx = w * 0.35F, cy = h * 0.5F;
      const float Re = mn * 0.08F;
      drawDataDisk(dst, w, h, src, cx, cy, Re, ya, 0.85F, t * 0.2F, Rgb{80, 120, 200, false});
      // Solar wind from the left.
      for (int i = 0; i < 80; ++i) {
        const float swY = (hash(i) * h);
        const float swX = std::fmod(hash(i * 3) * w + t * w * 1.0F, static_cast<float>(w));
        plotDot(dst, w, h, swX, swY, mn * 0.005F, ya, Rgb{220, 220, 100, false});
      }
      // Bow shock: parabolic curve sunward of Earth (x < cx).
      for (float ang = -1.5F; ang <= 1.5F; ang += 0.02F) {
        const float bsX = cx - Re * 2.0F - std::sin(ang) * 0.5F * Re;
        const float bsY = cy + ang * mn * 0.35F;
        plotDot(dst, w, h, bsX, bsY, std::max(1.0F, mn * 0.003F), ya, Rgb{255, 180, 100, false});
      }
      // Magnetopause: similar but inside bow shock.
      for (float ang = -1.4F; ang <= 1.4F; ang += 0.02F) {
        const float mpX = cx - Re * 1.2F - std::sin(ang) * 0.2F * Re;
        const float mpY = cy + ang * mn * 0.30F;
        plotDot(dst, w, h, mpX, mpY, std::max(1.0F, mn * 0.002F), ya, Rgb{160, 180, 240, false});
      }
      // Magnetotail stretching to the right.
      for (float xx = cx + Re * 1.5F; xx < w; xx += 2.0F) {
        const float xf = (xx - cx) / (w - cx);
        const float tailH = mn * (0.04F + 0.20F * xf);
        for (int dy = -static_cast<int>(tailH); dy <= static_cast<int>(tailH); dy += 4) {
          const int yy = static_cast<int>(cy + dy);
          if (xx >= 0 && xx < w && yy >= 0 && yy < h)
            dst[static_cast<std::size_t>(yy) * w + static_cast<int>(xx)] =
                Rgb{u8(60 - 30 * xf), u8(60 + 60 * xf), u8(130 + 80 * xf), false};
        }
      }
      // Dipole field lines.
      for (int k = 0; k < 6; ++k) {
        const float baseAng = (k - 2.5F) * 0.4F;
        for (float pa = -1.5F; pa <= 1.5F; pa += 0.02F) {
          const float lr = Re * (1.0F + 1.5F * std::cos(pa));
          const float lx = cx + std::cos(pa + baseAng) * lr;
          const float ly = cy + std::sin(pa + baseAng) * lr / ya;
          plotDot(dst, w, h, lx, ly, std::max(1.0F, mn * 0.002F), ya, Rgb{180, 160, 240, false});
        }
      }
    });
}

void effectPangaea(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 7000,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          dst[static_cast<std::size_t>(y) * w + x] = Rgb{6, 6, 18, false};
      const float cx = w * 0.5F, cy = h * 0.5F;
      const float R = mn * 0.42F;
      const float centerLonDeg = 0.0F;
      const float cLat = 1.0F, sLat = 0.0F;
      // Ocean fill.
      for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx) {
          float lat, lon;
          if (!globePxToLatLon(cx, cy, R, ya, cLat, sLat, centerLonDeg, xx, yy, lat, lon)) continue;
          const Rgb d = sample(src, w, h, xx, yy);
          const float dl = d.transparent ? 60.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          dst[static_cast<std::size_t>(yy) * w + xx] =
              Rgb{u8(20 + dl * 0.20F), u8(60 + dl * 0.25F), u8(120 + dl * 0.30F), false};
        }
      // Continent fragments — drift outward from origin centre over t.
      struct Cont { float lat, lon; float r; float driftLat, driftLon; };
      const Cont conts[] = {
          // Each starts near (0, 0) when t=0 and drifts to its modern position.
          {30, -80, mn * 0.10F, 30, -80},    // North America
          {-15, -55, mn * 0.08F, -15, -55},  // South America
          {0, 20, mn * 0.10F, 0, 20},        // Africa
          {50, 10, mn * 0.08F, 50, 10},      // Europe
          {30, 100, mn * 0.15F, 30, 100},    // Asia
          {-25, 135, mn * 0.06F, -25, 135},  // Australia
      };
      const float drift = std::clamp(t * 1.2F, 0.0F, 1.0F);
      for (const auto& c : conts) {
        // Start: clumped near 0, 0 (Pangaea). End: at modern location.
        const float curLat = drift * c.driftLat;
        const float curLon = drift * c.driftLon;
        float px, py;
        if (globeLatLonToPx(cx, cy, R, ya, cLat, sLat, centerLonDeg, curLat, curLon, px, py))
          drawDataDisk(dst, w, h, src, px, py, c.r, ya, 0.75F, 0.0F,
                       Rgb{140, 120, 70, false});
      }
      // Label at start ("PANGAEA") fading to end ("TODAY").
      const std::string label = (drift < 0.5F) ? "PANGAEA" : "TODAY";
      const int sc = std::max(2, static_cast<int>(mn / 30.0F));
      const float lineW = static_cast<float>(label.size()) * 6 * sc;
      for (int ci = 0; ci < static_cast<int>(label.size()); ++ci) {
        const auto g = glyph5x7(label[ci]);
        for (int fy = 0; fy < 7; ++fy)
          for (int fx = 0; fx < 5; ++fx)
            if (g[fy][fx] == '1')
              plotDot(dst, w, h, w * 0.5F - lineW * 0.5F + (ci * 6 + fx) * sc, h * 0.08F + fy * sc,
                      std::max(1.0F, static_cast<float>(sc) * 0.5F), ya, Rgb{240, 220, 140, false});
      }
    });
}

void effectRingOfFire(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
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
              Rgb{u8(20 + dl * 0.20F), u8(50 + dl * 0.25F), u8(100 + dl * 0.30F), false};
        }
      // Ring of fire chain (rough Pacific rim).
      const std::vector<std::pair<float,float>> vol = {
          {65,-160},{60,-150},{55,-135},{45,-125},{38,-122},{30,-115},{20,-105},{10,-90},
          {-5,-80},{-20,-72},{-35,-72},{-45,-75},
          {-40,170},{-20,165},{0,150},{10,140},{20,140},{30,135},{40,140},{50,155},{55,160}};
      for (size_t i = 0; i < vol.size(); ++i) {
        float px, py;
        if (!globeLatLonToPx(cx, cy, R, ya, cLat, sLat, centerLonDeg, vol[i].first, vol[i].second, px, py)) continue;
        // Steady volcano marker.
        plotDot(dst, w, h, px, py, mn * 0.008F, ya, Rgb{180, 80, 40, false});
        // Erupts occasionally.
        const float eruptT = std::fmod(t + hash(i) * 0.7F, 0.3F);
        if (eruptT < 0.05F) {
          const float age = eruptT / 0.05F;
          plotDot(dst, w, h, px, py, mn * 0.025F * (1 - age), ya, Rgb{255, 200, 80, false});
          // Shockwave ring.
          for (float a = 0; a < 6.2832F; a += 0.05F)
            plotDot(dst, w, h, px + std::cos(a) * mn * 0.04F * age, py + std::sin(a) * mn * 0.04F * age / ya,
                    std::max(1.0F, mn * 0.002F), ya, Rgb{u8(255 * (1 - age)), u8(160 * (1 - age)), u8(40), false});
        }
      }
    });
}

void effectAndromeda(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 6000,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb d = sample(src, w, h, x, y);
          const float dl = d.transparent ? 30.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(8 + dl * 0.10F), u8(8 + dl * 0.10F), u8(18 + dl * 0.10F), false};
        }
      for (int i = 0; i < 80; ++i) {
        const int sx = static_cast<int>(hash(i) * w);
        const int sy = static_cast<int>(hash(i * 3) * h);
        const Rgb& s = src[static_cast<std::size_t>(sy) * w + sx];
        const float l = s.transparent ? 60.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
        if (l < 80.0F) continue;
        dst[static_cast<std::size_t>(sy) * w + sx] = Rgb{220, 220, 240, false};
      }
      const float cx = w * 0.5F, cy = h * 0.5F;
      const float grow = std::clamp(t * 1.4F, 0.0F, 1.0F);
      const float Rmax = mn * 0.45F * grow;
      const float tilt = 0.45F;
      drawDataDisk(dst, w, h, src, cx, cy, mn * 0.05F * grow, ya, 0.75F, t * 0.2F,
                   Rgb{255, 230, 180, false});
      for (int arm = 0; arm < 2; ++arm) {
        const float ph = arm * 3.14159F;
        for (float p = 0.05F; p < 1.0F; p += 0.005F) {
          const float r = Rmax * p;
          const float ang = p * 6.0F + ph + t * 0.3F;
          const float px = cx + std::cos(ang) * r;
          const float py = cy + std::sin(ang) * r * tilt / ya;
          const Rgb d = sample(src, w, h, static_cast<int>(px), static_cast<int>(py));
          const float dr = d.transparent ? 180.0F : d.r;
          const float dg = d.transparent ? 160.0F : d.g;
          plotDot(dst, w, h, px, py, std::max(1.0F, mn * 0.004F), ya,
                  Rgb{u8(220 + dr * 0.10F), u8(180 + dg * 0.10F), u8(220), false});
        }
      }
    });
}

void effectBokGlobule(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 6000,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb d = sample(src, w, h, x, y);
          const float dl = d.transparent ? 80.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          const float swirl = std::sin(x * 0.04F + y * 0.05F + t * 0.4F);
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(160 + 40 * swirl + dl * 0.20F), u8(80 + 30 * swirl + dl * 0.20F),
                  u8(140 + 40 * (-swirl) + dl * 0.20F), false};
        }
      const float collapse = std::clamp(t * 1.3F, 0.0F, 1.0F);
      const float cx = w * 0.5F, cy = h * 0.5F;
      const float R = mn * 0.25F * (1.0F - collapse * 0.85F);
      for (int yo = -static_cast<int>(R / ya); yo <= static_cast<int>(R / ya); ++yo)
        for (int xo = -static_cast<int>(R); xo <= static_cast<int>(R); ++xo) {
          const float nx = xo / R, nyN = yo * ya / R;
          const float r2 = nx * nx + nyN * nyN;
          if (r2 > 1.0F) continue;
          const int xx = static_cast<int>(cx + xo), yy = static_cast<int>(cy + yo);
          if (xx < 0 || xx >= w || yy < 0 || yy >= h) continue;
          const float fade = 1.0F - r2;
          const Rgb& o = dst[static_cast<std::size_t>(yy) * w + xx];
          dst[static_cast<std::size_t>(yy) * w + xx] =
              Rgb{u8(o.r * (1 - fade)), u8(o.g * (1 - fade)), u8(o.b * (1 - fade)), false};
        }
      if (collapse > 0.7F) {
        const float starSize = (collapse - 0.7F) / 0.3F * mn * 0.05F;
        drawDataDisk(dst, w, h, src, cx, cy, starSize, ya, 0.5F, t * 4.0F,
                     Rgb{255, 240, 200, false});
        for (int k = -1; k <= 1; k += 2)
          for (float j = 0; j < mn * 0.20F; j += mn * 0.005F)
            plotDot(dst, w, h, cx, cy + k * j, std::max(1.0F, mn * 0.003F), ya,
                    Rgb{u8(255 - j * 600 / mn), u8(180), u8(80), false});
      }
    });
}

void effectCassiniFinale(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 6000,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb d = sample(src, w, h, x, y);
          const float dl = d.transparent ? 30.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(6 + dl * 0.10F), u8(6 + dl * 0.10F), u8(18 + dl * 0.12F), false};
        }
      for (int i = 0; i < 50; ++i)
        dst[static_cast<std::size_t>(hash(i) * h) * w + static_cast<int>(hash(i * 3) * w)] =
            Rgb{180, 180, 220, false};
      const float cx = w * 0.50F, cy = h * 0.50F;
      const float R = mn * 0.18F;
      for (int yo = -static_cast<int>(R / ya); yo <= static_cast<int>(R / ya); ++yo)
        for (int xo = -static_cast<int>(R); xo <= static_cast<int>(R); ++xo) {
          const float nx = xo / R, nyN = yo * ya / R;
          if (nx * nx + nyN * nyN > 1.0F) continue;
          const int xx = static_cast<int>(cx + xo), yy = static_cast<int>(cy + yo);
          if (xx < 0 || xx >= w || yy < 0 || yy >= h) continue;
          const Rgb d = sample(src, w, h, xx, yy);
          const float dr = d.transparent ? 200.0F : d.r;
          const float dg = d.transparent ? 160.0F : d.g;
          const float band = std::sin(nyN * 12.0F);
          const float r3 = std::sqrt(std::max(0.0F, 1.0F - nx * nx - nyN * nyN));
          const float lit = 0.6F + 0.4F * r3;
          dst[static_cast<std::size_t>(yy) * w + xx] =
              Rgb{u8((220 + dr * 0.10F + 20 * band) * lit),
                  u8((180 + dg * 0.10F + 20 * band) * lit), u8(120 * lit), false};
        }
      for (float rr = R * 1.20F; rr < R * 2.0F; rr += R * 0.020F) {
        const float bandShade = (rr / R - 1.20F) / 0.80F;
        const bool gap = (rr / R > 1.55F && rr / R < 1.65F);
        if (gap) continue;
        for (float a = 0; a < 6.2832F; a += 0.005F) {
          const float rx = cx + std::cos(a) * rr;
          const float ry = cy + std::sin(a) * rr * 0.22F;
          if (rx >= 0 && rx < w && ry >= 0 && ry < h)
            plotDot(dst, w, h, rx, ry, std::max(1.0F, mn * 0.002F), ya,
                    Rgb{u8(220 - 50 * bandShade), u8(200 - 40 * bandShade), u8(160), false});
        }
      }
      const float pp = std::clamp(t * 1.1F, 0.0F, 1.0F);
      const float probeR = R * 1.10F;
      const float pang = -1.8F + pp * 2.2F;
      const float px = cx + std::cos(pang) * probeR;
      const float py = cy + std::sin(pang) * probeR * 0.22F;
      plotDot(dst, w, h, px, py, mn * 0.012F, ya, Rgb{240, 220, 140, false});
      drawSeg(dst, w, h, px - mn * 0.018F, py, px + mn * 0.018F, py,
              std::max(1.0F, mn * 0.005F), ya, Rgb{160, 160, 220, false});
    });
}

void effectCepheid(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 6000,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb d = sample(src, w, h, x, y);
          const float dl = d.transparent ? 20.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(8 + dl * 0.10F), u8(8 + dl * 0.10F), u8(22 + dl * 0.10F), false};
        }
      for (int i = 0; i < 50; ++i)
        dst[static_cast<std::size_t>(hash(i) * h) * w + static_cast<int>(hash(i * 3) * w)] =
            Rgb{160, 160, 200, false};
      const float ph = std::fmod(t * 4.0F, 1.0F);
      const float radius = mn * (0.12F + 0.04F * std::sin(ph * 6.2832F));
      const float bright = 0.7F + 0.3F * std::sin(ph * 6.2832F);
      drawDataDisk(dst, w, h, src, w * 0.35F, h * 0.40F, radius, ya, bright * 0.5F, t,
                   Rgb{u8(255 * bright), u8(220 * bright), u8(160), false});
      const float y0 = h * 0.78F;
      const float amp = mn * 0.08F;
      for (int xx = 0; xx < w; ++xx) {
        const float xf = static_cast<float>(xx) / w;
        const float strain = std::sin(xf * t * 4.0F * 6.2832F);
        plotDot(dst, w, h, static_cast<float>(xx), y0 - amp * strain,
                std::max(1.0F, mn * 0.003F), ya, Rgb{160, 220, 240, false});
      }
      for (int xx = 0; xx < w; ++xx)
        dst[static_cast<std::size_t>(y0) * w + xx] = Rgb{60, 80, 100, false};
    });
}

void effectCrabNebula(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 6000,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb d = sample(src, w, h, x, y);
          const float dl = d.transparent ? 30.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(8 + dl * 0.10F), u8(6 + dl * 0.10F), u8(20 + dl * 0.10F), false};
        }
      const float cx = w * 0.5F, cy = h * 0.5F;
      const int nFil = 40;
      const float expand = std::clamp(t * 1.4F, 0.0F, 1.0F);
      for (int i = 0; i < nFil; ++i) {
        const float a0 = i / static_cast<float>(nFil) * 6.2832F;
        const float twist = (hash(i) - 0.5F) * 2.0F;
        for (float r = mn * 0.05F; r < mn * 0.35F * expand; r += mn * 0.005F) {
          const float a = a0 + r * 0.05F * twist;
          const float fx = cx + std::cos(a) * r;
          const float fy = cy + std::sin(a) * r / ya;
          if (fx < 0 || fx >= w || fy < 0 || fy >= h) continue;
          const Rgb d = sample(src, w, h, static_cast<int>(fx), static_cast<int>(fy));
          const float dr = d.transparent ? 200.0F : d.r;
          const float dg = d.transparent ? 80.0F : d.g;
          plotDot(dst, w, h, fx, fy, std::max(1.0F, mn * 0.003F), ya,
                  Rgb{u8(180 + dr * 0.20F), u8(80 + dg * 0.15F), u8(80), false});
        }
      }
      drawDataDisk(dst, w, h, src, cx, cy, mn * 0.025F, ya, 0.9F, t * 6.0F,
                   Rgb{220, 240, 255, false});
      const float pulse = std::fmod(t * 3.0F, 1.0F);
      for (float a = 0; a < 6.2832F; a += 0.04F) {
        const float rr = mn * 0.04F + pulse * mn * 0.20F;
        plotDot(dst, w, h, cx + std::cos(a) * rr, cy + std::sin(a) * rr / ya,
                std::max(1.0F, mn * 0.002F), ya,
                Rgb{u8(180 * (1 - pulse)), u8(220 * (1 - pulse)), u8(255 * (1 - pulse)), false});
      }
    });
}

void effectDysonSphere(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 6000,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb d = sample(src, w, h, x, y);
          const float dl = d.transparent ? 30.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(8 + dl * 0.10F), u8(8 + dl * 0.10F), u8(18 + dl * 0.10F), false};
        }
      for (int i = 0; i < 60; ++i)
        dst[static_cast<std::size_t>(hash(i) * h) * w + static_cast<int>(hash(i * 3) * w)] =
            Rgb{180, 180, 220, false};
      const float cx = w * 0.5F, cy = h * 0.5F;
      drawDataDisk(dst, w, h, src, cx, cy, mn * 0.06F, ya, 0.5F, t * 0.3F,
                   Rgb{255, 220, 120, false});
      const int nRings = static_cast<int>(std::clamp(t * 8.0F, 0.0F, 7.0F));
      for (int k = 0; k <= nRings; ++k) {
        const float rr = mn * (0.10F + 0.04F * k);
        const float ringAng = k * 0.4F + t * 0.5F;
        const float tilt = 0.3F + 0.1F * std::sin(k * 1.5F);
        for (float a = 0; a < 6.2832F; a += 0.04F) {
          const float ang = a + ringAng;
          const float lx = cx + std::cos(ang) * rr;
          const float ly = cy + std::sin(ang) * rr * tilt / ya;
          if (lx < 0 || lx >= w || ly < 0 || ly >= h) continue;
          const Rgb d = sample(src, w, h, static_cast<int>(lx), static_cast<int>(ly));
          const float dr = d.transparent ? 160.0F : d.r;
          plotDot(dst, w, h, lx, ly, std::max(1.0F, mn * 0.004F), ya,
                  Rgb{u8(140 + dr * 0.20F), u8(140), u8(180), false});
        }
      }
    });
}

void effectEuropaIce(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 6000,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb d = sample(src, w, h, x, y);
          const float dl = d.transparent ? 80.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(200 + dl * 0.15F), u8(220 + dl * 0.15F), u8(240 + dl * 0.10F), false};
        }
      const float grow = std::clamp(t * 1.4F, 0.0F, 1.0F);
      const int nCracks = 12;
      for (int i = 0; i < nCracks; ++i) {
        const float a0 = hash(i) * 6.2832F;
        const float r0 = mn * 0.4F * hash(i * 3);
        const float cx0 = w * 0.5F + std::cos(a0) * r0;
        const float cy0 = h * 0.5F + std::sin(a0) * r0 / ya;
        const float endA = a0 + (hash(i * 7) - 0.5F) * 3.0F;
        const int nPt = static_cast<int>(40 * grow);
        for (int k = 0; k < nPt; ++k) {
          const float f = k / 40.0F;
          const float jitter = std::sin(k * 0.8F + i) * mn * 0.012F;
          const float lx = cx0 + std::cos(endA) * f * mn * 0.30F + jitter;
          const float ly = cy0 + std::sin(endA) * f * mn * 0.30F / ya;
          const Rgb d = sample(src, w, h, static_cast<int>(lx), static_cast<int>(ly));
          const float dl = d.transparent ? 50.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          plotDot(dst, w, h, lx, ly, std::max(1.0F, mn * 0.003F), ya,
                  Rgb{u8(140 + dl * 0.10F), u8(80 + dl * 0.10F), u8(40), false});
        }
      }
    });
}

void effectHawkingRadiation(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 6000,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb d = sample(src, w, h, x, y);
          const float dl = d.transparent ? 30.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(8 + dl * 0.10F), u8(8 + dl * 0.10F), u8(20 + dl * 0.12F), false};
        }
      for (int i = 0; i < 60; ++i)
        dst[static_cast<std::size_t>(hash(i) * h) * w + static_cast<int>(hash(i * 3) * w)] =
            Rgb{180, 180, 220, false};
      const float cx = w * 0.5F, cy = h * 0.5F;
      const float R = mn * (0.10F - 0.04F * t);
      plotDot(dst, w, h, cx, cy, R, ya, Rgb{0, 0, 0, false});
      for (float a = 0; a < 6.2832F; a += 0.01F)
        plotDot(dst, w, h, cx + std::cos(a) * R * 1.2F, cy + std::sin(a) * R * 1.2F / ya,
                std::max(1.0F, mn * 0.002F), ya, Rgb{240, 180, 80, false});
      for (int i = 0; i < 25; ++i) {
        const float age = std::fmod(t * 1.5F + hash(i), 1.0F);
        const float ang = hash(i * 3) * 6.2832F;
        const float r = R * 1.2F + age * mn * 0.35F;
        const float px = cx + std::cos(ang) * r;
        const float py = cy + std::sin(ang) * r / ya;
        const Rgb d = sample(src, w, h, static_cast<int>(px), static_cast<int>(py));
        const float dr = d.transparent ? 200.0F : d.r;
        plotDot(dst, w, h, px, py, mn * 0.008F * (1 - age), ya,
                Rgb{u8(220 * (1 - age) + dr * 0.05F), u8(220 * (1 - age)), u8(240 * (1 - age)), false});
      }
    });
}

void effectHelixNebula(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 6000,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb d = sample(src, w, h, x, y);
          const float dl = d.transparent ? 30.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(10 + dl * 0.10F), u8(8 + dl * 0.10F), u8(20 + dl * 0.10F), false};
        }
      for (int i = 0; i < 80; ++i)
        dst[static_cast<std::size_t>(hash(i) * h) * w + static_cast<int>(hash(i * 3) * w)] =
            Rgb{180, 180, 220, false};
      const float cx = w * 0.5F, cy = h * 0.5F;
      const float grow = std::clamp(t * 1.2F, 0.0F, 1.0F);
      auto drawRing = [&](float rMin, float rMax, Rgb cold, Rgb hot) {
        for (float r = rMin; r <= rMax; r += mn * 0.004F) {
          const float frac = (r - rMin) / std::max(0.001F, rMax - rMin);
          for (float a = 0; a < 6.2832F; a += 0.01F) {
            const float lx = cx + std::cos(a) * r;
            const float ly = cy + std::sin(a) * r * 0.85F / ya;
            if (lx < 0 || lx >= w || ly < 0 || ly >= h) continue;
            const Rgb d = sample(src, w, h, static_cast<int>(lx), static_cast<int>(ly));
            const float dr = d.transparent ? 100.0F : d.r;
            const float dg = d.transparent ? 100.0F : d.g;
            const float r0 = cold.r + (hot.r - cold.r) * frac;
            const float g0 = cold.g + (hot.g - cold.g) * frac;
            const float b0 = cold.b + (hot.b - cold.b) * frac;
            plotDot(dst, w, h, lx, ly, std::max(1.0F, mn * 0.002F), ya,
                    Rgb{u8(r0 + dr * 0.15F), u8(g0 + dg * 0.15F), u8(b0), false});
          }
        }
      };
      drawRing(mn * 0.10F * grow, mn * 0.22F * grow, Rgb{180, 40, 30, false},
               Rgb{220, 80, 60, false});
      drawRing(mn * 0.25F * grow, mn * 0.40F * grow, Rgb{60, 140, 180, false},
               Rgb{100, 200, 220, false});
      drawDataDisk(dst, w, h, src, cx, cy, mn * 0.020F, ya, 0.6F, t * 5.0F,
                   Rgb{240, 240, 255, false});
    });
}

void effectHRDiagram(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 6000,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb d = sample(src, w, h, x, y);
          const float dl = d.transparent ? 30.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(10 + dl * 0.10F), u8(10 + dl * 0.10F), u8(28 + dl * 0.12F), false};
        }
      const int axL = static_cast<int>(w * 0.10F);
      const int axR = static_cast<int>(w * 0.90F);
      const int axB = static_cast<int>(h * 0.88F);
      const int axT = static_cast<int>(h * 0.12F);
      for (int xx = axL; xx <= axR; ++xx)
        dst[static_cast<std::size_t>(axB) * w + xx] = Rgb{200, 200, 220, false};
      for (int yy = axT; yy <= axB; ++yy)
        dst[static_cast<std::size_t>(yy) * w + axL] = Rgb{200, 200, 220, false};
      const int reveal = static_cast<int>(std::clamp(t * 1.4F, 0.0F, 1.0F) * 60.0F);
      for (int i = 0; i < reveal; ++i) {
        const float xf = hash(i);
        const float scatter = (hash(i * 5) - 0.5F) * 0.10F;
        const float yf = xf + scatter;
        const int sx = axL + static_cast<int>(xf * (axR - axL));
        const int sy = axB - static_cast<int>(yf * (axB - axT));
        const Rgb d = sample(src, w, h, sx, sy);
        const float dr = d.transparent ? 200.0F : d.r;
        const float dg = d.transparent ? 200.0F : d.g;
        const float db = d.transparent ? 200.0F : d.b;
        const Rgb starCol{u8(60 + xf * 200 + dr * 0.10F), u8(80 + xf * 100 + dg * 0.10F),
                         u8(220 - xf * 130 + db * 0.10F), false};
        plotDot(dst, w, h, sx, sy, std::max(1.0F, mn * 0.005F * (0.8F + hash(i * 7) * 0.6F)), ya,
                starCol);
      }
      const int gReveal = std::max(0, reveal - 30);
      for (int i = 0; i < gReveal; ++i) {
        const float xf = 0.55F + hash(i * 13) * 0.35F;
        const float yf = 0.55F + hash(i * 23) * 0.35F;
        const int sx = axL + static_cast<int>(xf * (axR - axL));
        const int sy = axB - static_cast<int>(yf * (axB - axT));
        plotDot(dst, w, h, sx, sy, std::max(1.0F, mn * 0.006F), ya, Rgb{240, 120, 80, false});
      }
    });
}

void effectHubbleExpansion(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 6000,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb d = sample(src, w, h, x, y);
          const float dl = d.transparent ? 20.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(6 + dl * 0.08F), u8(6 + dl * 0.08F), u8(18 + dl * 0.08F), false};
        }
      const float cx = w * 0.5F, cy = h * 0.5F;
      for (int i = 0; i < 40; ++i) {
        const float a0 = hash(i) * 6.2832F;
        const float r0 = mn * 0.05F + hash(i * 3) * mn * 0.05F;
        const float v_h = hash(i * 7) + 0.5F;
        const float r = r0 + t * mn * 0.40F * v_h;
        if (r > mn * 0.55F) continue;
        const float gx = cx + std::cos(a0) * r;
        const float gy = cy + std::sin(a0) * r / ya;
        const float zshift = std::clamp(r / (mn * 0.55F), 0.0F, 1.0F);
        const Rgb tint{u8(200 + zshift * 55), u8(200 - zshift * 120), u8(220 - zshift * 200), false};
        drawDataDisk(dst, w, h, src, gx, gy, mn * 0.015F * (1.0F - zshift * 0.4F), ya,
                     0.7F, t + i, tint);
      }
    });
}

void effectHubbleTelescope(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 6000,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb d = sample(src, w, h, x, y);
          const float dl = d.transparent ? 30.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(6 + dl * 0.10F), u8(6 + dl * 0.10F), u8(22 + dl * 0.10F), false};
        }
      drawDataDisk(dst, w, h, src, w * 1.10F, h * 1.10F, mn * 0.50F, ya, 0.85F, 0.0F,
                   Rgb{60, 120, 180, false});
      const float tx = w * 0.30F, tcy = h * 0.50F;
      const float tubeL = mn * 0.30F, tubeR = mn * 0.05F;
      for (int yo = -static_cast<int>(tubeR / ya); yo <= static_cast<int>(tubeR / ya); ++yo)
        for (int xo = -static_cast<int>(tubeL); xo <= static_cast<int>(tubeL); ++xo) {
          const int xx = static_cast<int>(tx + xo), yy = static_cast<int>(tcy + yo);
          if (xx >= 0 && xx < w && yy >= 0 && yy < h)
            dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{200, 200, 220, false};
        }
      drawSeg(dst, w, h, tx - tubeL, tcy - tubeR / ya - mn * 0.15F, tx - tubeL,
              tcy - tubeR / ya, std::max(1.0F, mn * 0.005F), ya, Rgb{120, 120, 140, false});
      for (int yo = -static_cast<int>(mn * 0.15F); yo <= -static_cast<int>(tubeR / ya); ++yo)
        for (int xo = -static_cast<int>(mn * 0.15F); xo <= static_cast<int>(mn * 0.15F); ++xo) {
          const int xx = static_cast<int>(tx - tubeL + xo), yy = static_cast<int>(tcy + yo);
          if (xx >= 0 && xx < w && yy >= 0 && yy < h)
            dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{20, 40, 100, false};
        }
      const float mvx = w * 0.78F, mvy = h * 0.45F, mvR = mn * 0.20F;
      for (int yo = -static_cast<int>(mvR / ya); yo <= static_cast<int>(mvR / ya); ++yo)
        for (int xo = -static_cast<int>(mvR); xo <= static_cast<int>(mvR); ++xo) {
          const float nx = xo / mvR, nyN = yo * ya / mvR;
          if (nx * nx + nyN * nyN > 1.0F) continue;
          const int xx = static_cast<int>(mvx + xo), yy = static_cast<int>(mvy + yo);
          if (xx < 0 || xx >= w || yy < 0 || yy >= h) continue;
          const Rgb d = sample(src, w, h, xx, yy);
          const float dl = d.transparent ? 30.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          dst[static_cast<std::size_t>(yy) * w + xx] =
              Rgb{u8(8 + dl * 0.10F), u8(8 + dl * 0.10F), u8(22 + dl * 0.10F), false};
        }
      const int nG = static_cast<int>(std::clamp(t * 60.0F, 0.0F, 40.0F));
      for (int i = 0; i < nG; ++i) {
        const float ang = hash(i) * 6.2832F;
        const float rr = mvR * std::sqrt(hash(i * 3));
        const float gx = mvx + std::cos(ang) * rr;
        const float gy = mvy + std::sin(ang) * rr / ya;
        plotDot(dst, w, h, gx, gy, std::max(1.0F, mn * 0.003F), ya, Rgb{220, 200, 240, false});
      }
      drawSeg(dst, w, h, tx + tubeL, tcy, mvx - mvR, mvy, std::max(1.0F, mn * 0.001F), ya,
              Rgb{80, 100, 120, false});
    });
}

void effectLagrangePoints(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 6000,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb d = sample(src, w, h, x, y);
          const float dl = d.transparent ? 30.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(6 + dl * 0.10F), u8(6 + dl * 0.10F), u8(22 + dl * 0.10F), false};
        }
      const float sunX = w * 0.30F, sunY = h * 0.50F;
      const float earthOrbitR = mn * 0.30F;
      const float earthAng = t * 6.2832F * 0.5F;
      const float earthX = sunX + std::cos(earthAng) * earthOrbitR;
      const float earthY = sunY + std::sin(earthAng) * earthOrbitR / ya;
      for (float a = 0; a < 6.2832F; a += 0.02F) {
        const float ox = sunX + std::cos(a) * earthOrbitR;
        const float oy = sunY + std::sin(a) * earthOrbitR / ya;
        if (ox >= 0 && ox < w && oy >= 0 && oy < h)
          dst[static_cast<std::size_t>(static_cast<int>(oy)) * w + static_cast<int>(ox)] =
              Rgb{40, 50, 70, false};
      }
      drawDataDisk(dst, w, h, src, sunX, sunY, mn * 0.06F, ya, 0.4F, t * 0.3F,
                   Rgb{255, 220, 120, false});
      drawDataDisk(dst, w, h, src, earthX, earthY, mn * 0.020F, ya, 0.85F, earthAng,
                   Rgb{60, 120, 180, false});
      auto plotL = [&](float ang, float rr) {
        const float lx = sunX + std::cos(ang) * rr;
        const float ly = sunY + std::sin(ang) * rr / ya;
        plotDot(dst, w, h, lx, ly, std::max(1.0F, mn * 0.006F), ya, Rgb{240, 220, 80, false});
      };
      plotL(earthAng, earthOrbitR * 0.95F);
      plotL(earthAng, earthOrbitR * 1.05F);
      plotL(earthAng + 3.14159F, earthOrbitR);
      plotL(earthAng + 1.0472F, earthOrbitR);
      plotL(earthAng - 1.0472F, earthOrbitR);
      for (int side = -1; side <= 1; side += 2) {
        const float lAng = earthAng + side * 1.0472F;
        for (int i = 0; i < 6; ++i) {
          const float jitter = (hash(i * (side > 0 ? 7 : 13)) - 0.5F) * 0.30F;
          const float jr = (hash(i * (side > 0 ? 11 : 17)) - 0.5F) * mn * 0.03F;
          const float tx = sunX + std::cos(lAng + jitter) * (earthOrbitR + jr);
          const float ty = sunY + std::sin(lAng + jitter) * (earthOrbitR + jr) / ya;
          plotDot(dst, w, h, tx, ty, std::max(1.0F, mn * 0.003F), ya, Rgb{200, 200, 220, false});
        }
      }
    });
}

void effectMoonPhases(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 6000,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb d = sample(src, w, h, x, y);
          const float dl = d.transparent ? 30.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(8 + dl * 0.10F), u8(8 + dl * 0.10F), u8(24 + dl * 0.12F), false};
        }
      for (int i = 0; i < 50; ++i)
        dst[static_cast<std::size_t>(hash(i) * h) * w + static_cast<int>(hash(i * 3) * w)] =
            Rgb{180, 180, 220, false};
      const float cx = w * 0.5F, cy = h * 0.5F;
      const float R = mn * 0.25F;
      drawDataDisk(dst, w, h, src, cx, cy, R, ya, 0.75F, t * 0.4F, Rgb{200, 200, 200, false});
      const float ph = std::fmod(t * 6.2832F, 6.2832F);
      for (int yo = -static_cast<int>(R / ya); yo <= static_cast<int>(R / ya); ++yo)
        for (int xo = -static_cast<int>(R); xo <= static_cast<int>(R); ++xo) {
          const float nx = xo / R, nyN = yo * ya / R;
          const float r2 = nx * nx + nyN * nyN;
          if (r2 > 1.0F) continue;
          const int xx = static_cast<int>(cx + xo), yy = static_cast<int>(cy + yo);
          if (xx < 0 || xx >= w || yy < 0 || yy >= h) continue;
          const float z = std::sqrt(std::max(0.0F, 1.0F - r2));
          const float lit = nx * std::sin(ph) + z * std::cos(ph);
          if (lit < 0.0F) {
            Rgb& o = dst[static_cast<std::size_t>(yy) * w + xx];
            o.r = u8(o.r * 0.10F);
            o.g = u8(o.g * 0.10F);
            o.b = u8(o.b * 0.15F + 10);
          }
        }
    });
}

void effectOlympusMons(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb d = sample(src, w, h, x, y);
          const float dl = d.transparent ? 60.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          const float sf = static_cast<float>(y) / h;
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(220 + dl * 0.10F - 60 * sf), u8(160 + dl * 0.10F - 50 * sf),
                  u8(120 + dl * 0.05F), false};
        }
      const float groundY = h * 0.75F;
      for (int yy = static_cast<int>(groundY); yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx) {
          const Rgb d = sample(src, w, h, xx, yy);
          const float dr = d.transparent ? 180.0F : d.r;
          const float yf = (yy - groundY) / (h - groundY);
          dst[static_cast<std::size_t>(yy) * w + xx] =
              Rgb{u8(200 + dr * 0.20F - 60 * yf), u8(100 + dr * 0.10F), u8(50), false};
        }
      const float rise = std::clamp(t * 1.3F, 0.0F, 1.0F);
      const float peakX = w * 0.50F;
      const float peakY = groundY - mn * 0.30F * rise;
      for (int xx = 0; xx < w; ++xx) {
        const float xf = (xx - peakX) / (w * 0.45F);
        const float prof = std::exp(-xf * xf * 1.5F);
        const float mountY = groundY - prof * mn * 0.30F * rise;
        if (mountY < groundY)
          for (int yy = static_cast<int>(mountY); yy <= static_cast<int>(groundY); ++yy) {
            const Rgb d = sample(src, w, h, xx, yy);
            const float dr = d.transparent ? 180.0F : d.r;
            const float yfr = (yy - mountY) / std::max(1.0F, groundY - mountY);
            dst[static_cast<std::size_t>(yy) * w + xx] =
                Rgb{u8(160 + dr * 0.20F - 40 * yfr), u8(80 + dr * 0.10F), u8(40), false};
          }
      }
      plotDot(dst, w, h, peakX, peakY, mn * 0.030F * rise, ya, Rgb{60, 30, 20, false});
    });
}

void effectParkerProbe(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 5600,
    [&](float t, std::vector<Rgb>& dst) {
      const float sunX = w * 0.85F, sunY = h * 0.85F;
      const float R = mn * 0.75F;
      for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx) {
          const float dx = xx - sunX, dy = (yy - sunY) * ya;
          const float r = std::sqrt(dx * dx + dy * dy);
          const Rgb d = sample(src, w, h, xx, yy);
          const float dl = d.transparent ? 40.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          if (r < R) {
            const float surf = 1.0F - r / R;
            dst[static_cast<std::size_t>(yy) * w + xx] =
                Rgb{u8(255 * surf + dl * 0.20F * surf), u8(180 * surf + dl * 0.20F * surf),
                    u8(60 * surf), false};
          } else {
            dst[static_cast<std::size_t>(yy) * w + xx] =
                Rgb{u8(8 + dl * 0.10F), u8(8 + dl * 0.10F), u8(22 + dl * 0.10F), false};
          }
        }
      for (int i = 0; i < 60; ++i) {
        const float a = hash(i) * 6.2832F;
        const float rr = hash(i * 3) * R * 0.9F;
        const float gx = sunX + std::cos(a) * rr;
        const float gy = sunY + std::sin(a) * rr / ya;
        plotDot(dst, w, h, gx, gy, mn * 0.012F, ya, Rgb{255, 220, 100, false});
      }
      const float dive = std::clamp(t * 1.2F, 0.0F, 1.0F);
      const float px = w * 0.15F + dive * (sunX - mn * 0.20F - w * 0.15F);
      const float py = h * 0.20F + dive * (sunY - mn * 0.50F - h * 0.20F);
      plotDot(dst, w, h, px, py, mn * 0.035F, ya, Rgb{240, 240, 240, false});
      for (float a = 0; a < 6.2832F; a += 0.03F) {
        const float glow = 0.6F + 0.4F * std::sin(a * 3.0F + t * 12.0F);
        plotDot(dst, w, h, px + std::cos(a) * mn * 0.037F,
                py + std::sin(a) * mn * 0.037F / ya, std::max(1.0F, mn * 0.003F), ya,
                Rgb{u8(255 * glow), u8(140 * glow), u8(40 * glow), false});
      }
      drawSeg(dst, w, h, px - mn * 0.04F, py - mn * 0.02F, px - mn * 0.06F, py - mn * 0.05F,
              std::max(1.0F, mn * 0.004F), ya, Rgb{40, 60, 120, false});
      drawSeg(dst, w, h, px - mn * 0.04F, py + mn * 0.02F, px - mn * 0.06F, py + mn * 0.05F,
              std::max(1.0F, mn * 0.004F), ya, Rgb{40, 60, 120, false});
    });
}

void effectSaturnRings(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 6000,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb d = sample(src, w, h, x, y);
          const float dl = d.transparent ? 20.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(6 + dl * 0.10F), u8(6 + dl * 0.10F), u8(20 + dl * 0.10F), false};
        }
      const float cx = w * 0.20F, cy = h * 0.50F;
      drawDataDisk(dst, w, h, src, cx, cy, mn * 0.20F, ya, 0.75F, t * 0.2F,
                   Rgb{220, 180, 120, false});
      for (float rr = mn * 0.24F; rr < mn * 0.95F; rr += mn * 0.005F) {
        const float rNorm = (rr - mn * 0.24F) / (mn * 0.71F);
        const bool cassini = (rNorm > 0.42F && rNorm < 0.50F);
        const bool encke = (rNorm > 0.78F && rNorm < 0.79F);
        if (cassini || encke) continue;
        for (float a = 0; a < 6.2832F; a += 0.005F) {
          const float rx = cx + std::cos(a) * rr;
          const float ry = cy + std::sin(a) * rr * 0.12F;
          if (rx < 0 || rx >= w || ry < 0 || ry >= h) continue;
          const Rgb d = sample(src, w, h, static_cast<int>(rx), static_cast<int>(ry));
          const float dr = d.transparent ? 180.0F : d.r;
          const float bandShade = 0.6F + 0.4F * std::sin(rNorm * 80.0F);
          plotDot(dst, w, h, rx, ry, std::max(1.0F, mn * 0.002F), ya,
                  Rgb{u8((220 + dr * 0.15F) * bandShade), u8((200) * bandShade),
                      u8((160) * bandShade), false});
        }
      }
    });
}

void effectSunspotCycle(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 6000,
    [&](float t, std::vector<Rgb>& dst) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const Rgb d = sample(src, w, h, x, y);
          const float dl = d.transparent ? 30.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(6 + dl * 0.10F), u8(6 + dl * 0.10F), u8(22 + dl * 0.10F), false};
        }
      const float cx = w * 0.5F, cy = h * 0.5F;
      const float R = mn * 0.40F;
      drawDataDisk(dst, w, h, src, cx, cy, R, ya, 0.5F, t * 0.2F, Rgb{255, 220, 120, false});
      for (int i = 0; i < 80; ++i) {
        const float a = hash(i) * 6.2832F;
        const float rr = hash(i * 3) * R * 0.95F;
        const float gx = cx + std::cos(a) * rr;
        const float gy = cy + std::sin(a) * rr / ya;
        const Rgb d = sample(src, w, h, static_cast<int>(gx), static_cast<int>(gy));
        const float dr = d.transparent ? 220.0F : d.r;
        plotDot(dst, w, h, gx, gy, mn * 0.010F, ya,
                Rgb{u8(255), u8(180 + dr * 0.05F), u8(40), false});
      }
      const float cycle = std::sin(t * 3.14159F);
      const int nSpots = static_cast<int>(std::max(0.0F, cycle * 15.0F));
      for (int i = 0; i < nSpots; ++i) {
        const float drift = std::fmod(t * 1.5F + hash(i), 1.0F);
        const float bandLat = (hash(i * 3) - 0.5F) * 0.7F;
        const float sx = cx + (drift - 0.5F) * 2.0F * R * std::cos(bandLat);
        const float sy = cy + bandLat * R / ya;
        const float dx = sx - cx, dy = (sy - cy) * ya;
        if (dx * dx + dy * dy > R * R) continue;
        plotDot(dst, w, h, sx, sy, mn * 0.012F, ya, Rgb{30, 20, 10, false});
        for (float a = 0; a < 6.2832F; a += 0.1F)
          plotDot(dst, w, h, sx + std::cos(a) * mn * 0.018F, sy + std::sin(a) * mn * 0.018F / ya,
                  std::max(1.0F, mn * 0.002F), ya, Rgb{80, 50, 30, false});
      }
    });
}

void effectVoyager(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n) { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 6000,
    [&](float t, std::vector<Rgb>& dst) {
      const float sunX = w * 0.10F, sunY = h * 0.50F;
      for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx) {
          const Rgb d = sample(src, w, h, xx, yy);
          const float dl = d.transparent ? 30.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          const float dx = xx - sunX, dy = (yy - sunY) * ya;
          const float r = std::sqrt(dx * dx + dy * dy);
          const float helioR = mn * 0.50F;
          const bool inside = r < helioR;
          if (inside)
            dst[static_cast<std::size_t>(yy) * w + xx] =
                Rgb{u8(60 + dl * 0.15F), u8(40 + dl * 0.10F), u8(20 + dl * 0.05F), false};
          else
            dst[static_cast<std::size_t>(yy) * w + xx] =
                Rgb{u8(20 + dl * 0.10F), u8(20 + dl * 0.10F), u8(50 + dl * 0.15F), false};
        }
      const float helioR2 = mn * 0.50F;
      for (float a = 0; a < 6.2832F; a += 0.005F) {
        const float bx = sunX + std::cos(a) * helioR2;
        const float by = sunY + std::sin(a) * helioR2 / ya;
        if (bx >= 0 && bx < w && by >= 0 && by < h)
          plotDot(dst, w, h, bx, by, std::max(1.0F, mn * 0.002F), ya, Rgb{180, 140, 80, false});
      }
      drawDataDisk(dst, w, h, src, sunX, sunY, mn * 0.04F, ya, 0.5F, t * 0.3F,
                   Rgb{255, 220, 120, false});
      const float cross = std::clamp(t * 1.2F, 0.0F, 1.0F);
      const float px = sunX + cross * mn * 0.80F;
      const float py = sunY;
      plotDot(dst, w, h, px, py, mn * 0.015F, ya, Rgb{220, 220, 220, false});
      for (float a = -1.0F; a <= 1.0F; a += 0.2F)
        plotDot(dst, w, h, px + std::cos(a) * mn * 0.025F, py + std::sin(a) * mn * 0.025F / ya,
                std::max(1.0F, mn * 0.003F), ya, Rgb{200, 200, 220, false});
      for (int k = 0; k < 30; ++k) {
        const float f = k / 30.0F;
        plotDot(dst, w, h, px - f * mn * 0.20F, py - f * f * mn * 0.02F,
                std::max(1.0F, mn * 0.001F), ya, Rgb{120, 120, 160, false});
      }
      for (int i = 0; i < 40; ++i) {
        const float sxN = static_cast<float>(w) * 0.50F + hash(i) * w * 0.50F;
        const float syN = hash(i * 3) * h;
        dst[static_cast<std::size_t>(static_cast<int>(syN)) * w + static_cast<int>(sxN)] =
            Rgb{220, 220, 240, false};
      }
    });
}

}  // namespace ee_detail
}  // namespace Qdless
