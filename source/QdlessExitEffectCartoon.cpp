#include "QdlessExitEffectCommon.h"
#include "QdlessMarionette.h"

namespace Qdless
{
namespace ee_detail
{

// Kenney gallery: 4×4 grid of Kenney.nl "Platformer Art Deluxe" (CC0)
// character animations playing simultaneously over the dimmed data
// backdrop. The 11-frame player walk cycle is the showpiece (top-left
// cell); the rest of the grid covers jumps, climbs, swims, and small
// creature loops for breadth. Sprites are pre-extracted to
// data/kenney/<motion>/frame_NN.png by scripts/kenney2sprites.py — they
// arrive as alpha-clean colour PNGs and composite directly through
// drawKenneyFrame which preserves the original cartoon palette.
void effectKenney(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto motions = loadAllKenneyMotions();

  constexpr int cols = 4;
  constexpr int rows = 4;
  const float cellW = static_cast<float>(w) / cols;
  const float cellH = static_cast<float>(h) / rows;
  const float spriteH = cellH * 0.78F;

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

  runFrames(
      renderer,
      w,
      h,
      6400,
      [&](float t, std::vector<Rgb>& dst)
      {
        // Cool blue-grey scrapbook backdrop so the cartoon colours pop.
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 40.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(40 + l * 0.20F), u8(60 + l * 0.22F), u8(95 + l * 0.20F), false};
          }
        // Thin grid dividers.
        const Rgb divCol{20, 30, 50, false};
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

        for (int idx = 0; idx < kKenneyMotionCount; ++idx)
        {
          const int r = idx / cols;
          const int c = idx % cols;
          const float cx = (c + 0.5F) * cellW;
          // Anchor sprites slightly above centre so the caption strip has
          // room below; the walking sprites read better with the feet
          // near the implied ground anyway.
          const float cy = (r + 0.5F) * cellH - cellH * 0.06F;

          const auto& m = motions[idx];
          if (m.frames.empty())
            continue;
          const int nf = static_cast<int>(m.frames.size());
          // Cycle ~3.5× over the runtime so the 11-frame walk plays
          // properly (else it looks like a slow march); short 2-frame
          // cycles just blink at a similar visual rate.
          const float phase = t * 3.5F + idx * 0.13F;
          const int fi = static_cast<int>(std::floor(phase * nf)) % std::max(1, nf);

          drawKenneyFrame(dst, w, h, ya, m, fi, cx, cy, spriteH);

          const char* label = kKenneyLabels[idx];
          const std::size_t llen = std::strlen(label);
          const float capW = llen * 6.0F;
          const float capX = cx - capW * 0.5F;
          const float capY = (r + 1) * cellH - cellH * 0.10F;
          drawText(dst, label, capX, capY, Rgb{230, 240, 250, false});
        }

        const char* title = "KENNEY.NL PLATFORMER PACK (CC0)";
        const std::size_t tlen = std::strlen(title);
        const float tw = tlen * 6.0F;
        drawText(dst, title, w * 0.5F - tw * 0.5F, 4.0F, Rgb{240, 250, 255, false});
      });
}

void effectPacman(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float cy = (h - 1) * 0.5F;
  const float radius = std::max(4.0F, h * 0.32F);  // body radius (x sub-pixels)
  runFrames(renderer,
            w,
            h,
            3000,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float pacX = -radius + t * (w + 2.0F * radius);
              // Base: blank behind Pac-Man, frame ahead of him.
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const std::size_t idx = static_cast<std::size_t>(y) * w + x;
                  dst[idx] = (x < pacX) ? kBlank : src[idx];
                }
              // Body: yellow disc minus an animated mouth wedge opening +x.
              const float chomp = (0.05F + 0.30F * std::fabs(std::sin(t * 22.0F))) * 3.14159F;
              const Rgb body{255, 220, 0, false};
              const int x0 = std::max(0, static_cast<int>(pacX - radius));
              const int x1 = std::min(w - 1, static_cast<int>(pacX + radius));
              const int y0 = std::max(0, static_cast<int>(cy - radius / ya));
              const int y1 = std::min(h - 1, static_cast<int>(cy + radius / ya));
              for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                {
                  const float dx = x - pacX;
                  const float dy = (y - cy) * ya;
                  if (dx * dx + dy * dy > radius * radius)
                    continue;
                  if (std::fabs(std::atan2(dy, dx)) < chomp)
                    continue;  // mouth: leave the base layer showing
                  dst[static_cast<std::size_t>(y) * w + x] = body;
                }
              // Eye: a small dark dot, up and slightly back from the mouth.
              plotDot(dst,
                      w,
                      h,
                      pacX - radius * 0.15F,
                      cy - radius / ya * 0.45F,
                      std::max(1.0F, radius * 0.10F),
                      ya,
                      Rgb{30, 30, 30, false});
            });
}

void effectFoot(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float fcx = (w - 1) * 0.5F;
  // Wide flat foot: spans ~0.92*w across, squashed vertically so the whole
  // print still fits the height (and so it reads as flat-footed).
  const float sx = 0.77F * w;
  const float sy = std::min(sx / ya, 0.43F * h);
  const float footBottom = 1.02F * sy;  // the heel reaches this far below centre

  // Five toes (sole view): big toe to little toe, in a shallow arc above the
  // ball. {centre x, centre y, radius} in foot-local coords.
  struct Toe
  {
    float x, y, r;
  };
  static const Toe kToes[5] = {{-0.34F, -0.82F, 0.170F},
                               {-0.11F, -0.93F, 0.135F},
                               {0.10F, -0.96F, 0.120F},
                               {0.29F, -0.93F, 0.105F},
                               {0.46F, -0.86F, 0.090F}};

  auto inEllipse = [](float nx, float ny, float cx, float cy, float rx, float ry)
  {
    const float dx = (nx - cx) / rx;
    const float dy = (ny - cy) / ry;
    return dx * dx + dy * dy <= 1.0F;
  };
  // Solid foot silhouette in local coords (nx across, ny along; -1 ~ toe tips,
  // +1 ~ heel): a wide ball, a full (flat-footed) arch, a rounded heel, toes.
  auto inFoot = [&](float nx, float ny)
  {
    if (inEllipse(nx, ny, 0.0F, -0.42F, 0.60F, 0.46F))
      return true;
    if (inEllipse(nx, ny, 0.0F, 0.60F, 0.52F, 0.42F))
      return true;
    if (ny > -0.42F && ny < 0.60F && std::fabs(nx) <= 0.52F)  // filled arch (flat-footed)
      return true;
    for (const auto& toe : kToes)
    {
      const float dx = nx - toe.x;
      const float dy = ny - toe.y;
      if (dx * dx + dy * dy <= toe.r * toe.r)
        return true;
    }
    return false;
  };

  const Rgb skin{226, 190, 158, false};
  const Rgb pad{240, 205, 188, false};  // fleshy sole / toe pads (pinker, lighter)
  const Rgb rim{150, 112, 84, false};   // darkened outline
  // Paint the foot at (cx,cy) with half-extents (sx,sy); `alpha` fades it out.
  auto drawFoot = [&](std::vector<Rgb>& dst, float cx, float cy, float sx, float sy, float alpha)
  {
    const int x0 = std::max(0, static_cast<int>(std::floor(cx - 0.66F * sx)));
    const int x1 = std::min(w - 1, static_cast<int>(std::ceil(cx + 0.66F * sx)));
    const int y0 = std::max(0, static_cast<int>(std::floor(cy - 1.14F * sy)));
    const int y1 = std::min(h - 1, static_cast<int>(std::ceil(cy + 1.07F * sy)));
    constexpr float e = 0.05F;  // neighbour offset for the rim test
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x)
      {
        const float nx = (x - cx) / sx;
        const float ny = (y - cy) / sy;
        if (!inFoot(nx, ny))
          continue;
        Rgb c = skin;
        // Fleshy pads (ball, heel, toe tips) read as a sole.
        bool isPad = inEllipse(nx, ny, 0.0F, -0.40F, 0.42F, 0.30F) ||
                     inEllipse(nx, ny, 0.0F, 0.58F, 0.34F, 0.26F);
        if (!isPad)
          for (const auto& toe : kToes)
            if (inEllipse(nx, ny, toe.x, toe.y, toe.r * 0.72F, toe.r * 0.72F))
            {
              isPad = true;
              break;
            }
        if (isPad)
          c = pad;
        // Top-lit roundness, and a darkened rim where a neighbour falls outside.
        float lit = 0.82F + 0.18F * (1.0F - std::min(1.0F, std::fabs(nx) / 0.62F));
        if (!inFoot(nx - e, ny) || !inFoot(nx + e, ny) || !inFoot(nx, ny - e) ||
            !inFoot(nx, ny + e))
        {
          c = rim;
          lit = 1.0F;
        }
        const float k = lit * alpha;
        dst[static_cast<std::size_t>(y) * w + x] = Rgb{static_cast<std::uint8_t>(c.r * k),
                                                       static_cast<std::uint8_t>(c.g * k),
                                                       static_cast<std::uint8_t>(c.b * k),
                                                       false};
      }
  };

  constexpr float tDown = 0.5F;  // the sole slams to the floor by here
  runFrames(renderer,
            w,
            h,
            1400,
            [&](float t, std::vector<Rgb>& dst)
            {
              // Stomp: the sole accelerates down, squishing the whole view into
              // the shrinking strip beneath it; once down it stays planted.
              const float p = std::min(1.0F, t / tDown);
              const float contactY = p * p * h;
              for (int y = static_cast<int>(std::ceil(contactY)); y < h; ++y)
              {
                const float frac = (y - contactY) / std::max(1.0F, h - contactY);
                const int row = std::min(h - 1, static_cast<int>(frac * (h - 1)));
                for (int x = 0; x < w; ++x)
                  dst[static_cast<std::size_t>(y) * w + x] =
                      src[static_cast<std::size_t>(row) * w + x];
              }
              drawFoot(dst, fcx, contactY - footBottom, sx, sy, 1.0F);
            });
}

void effectMontyPython(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  // fw/fh are only needed so loadFootImage can reject a zero-sized decode; the
  // stomp scales the painting to the screen, so the source aspect is unused.
  [[maybe_unused]] std::size_t fw = 0;
  [[maybe_unused]] std::size_t fh = 0;
  std::unique_ptr<ImageSource> foot = loadFootImage(fw, fh);
  if (!foot)
  {
    effectMontyPythonProcedural(renderer, src, w, h);
    return;
  }

  // Sample the painting at (u,v) in [0,1]^2 — v=0 is the top (leg cut off the
  // frame), v=1 the sole; u=0 the toes, u=1 the heel. Bilinear, full-res.
  auto footPixel = [&](float u, float v) -> Rgb
  { return foot->pixelAtUV(std::clamp(u, 0.0F, 0.999F), std::clamp(v, 0.0F, 0.999F)); };

  // The sole accelerates down from the top (gravity: p^2) and slams in the
  // first third of the run (~3x faster than a full-length drop); the foot then
  // stays planted for a brief hold before the effect exits.
  runFrames(renderer,
            w,
            h,
            450,
            [&](float t, std::vector<Rgb>& dst)
            { stompFrame(dst, src, w, h, std::min(1.0F, t / 0.33F), footPixel); });
}

void effectPythonWars(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  int bw = 0;
  int bh = 0;
  const std::vector<char> text =
      buildTextBitmap({"A LONG TIME AGO IN A GALAXY FAR,", "FAR AWAY..."}, bw, bh);
  const Rgb blue{90, 140, 255, false};

  // The crawl's near edge recedes from 0.5 to ~2.4 over the build-up; freeze the
  // frame at that final position as the view the foot then stomps. The recede
  // speed (kFrontSpan / build-up time) matches the slower, readable pace, but
  // the build-up is ~1 s shorter than the full recede would be so the foot
  // stomps before the text drifts off into dead air.
  constexpr float kRecede = 0.753F;  // the build-up ends here (fraction of t)
  constexpr float kFrontStart = 0.5F;
  constexpr float kFrontSpan = 1.87F;
  std::vector<Rgb> view(static_cast<std::size_t>(w) * h, Rgb{0, 0, 0, false});
  drawCrawl(view, w, h, text, bw, bh, kFrontStart + kFrontSpan, blue, 1.0F, 1.5F);

  [[maybe_unused]] std::size_t fw = 0;
  [[maybe_unused]] std::size_t fh = 0;
  std::unique_ptr<ImageSource> foot = loadFootImage(fw, fh);
  if (!foot)
  {
    // No foot art: let the hand-drawn foot stomp the receded prologue instead.
    effectMontyPythonProcedural(renderer, view, w, h);
    return;
  }
  auto footPixel = [&](float u, float v) -> Rgb
  { return foot->pixelAtUV(std::clamp(u, 0.0F, 0.999F), std::clamp(v, 0.0F, 0.999F)); };

  runFrames(renderer,
            w,
            h,
            2200,
            [&](float t, std::vector<Rgb>& dst)
            {
              if (t < kRecede)
              {
                // The prologue appears and recedes toward the vanishing point.
                std::fill(dst.begin(), dst.end(), Rgb{0, 0, 0, false});
                const float tA = t / kRecede;
                const float gFade = std::min(1.0F, tA / 0.2F);  // quick fade-in
                drawCrawl(
                    dst, w, h, text, bw, bh, kFrontStart + tA * kFrontSpan, blue, gFade, 1.5F);
                return;
              }
              // ...then the foot slams in fast and stays planted briefly.
              const float p = std::min(1.0F, ((t - kRecede) / (1.0F - kRecede)) / 0.30F);
              stompFrame(dst, view, w, h, p, footPixel);
            });
}

void effectPacmanDuel(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float smallR = 0.055F * mn;  // a small regular Pac-Man
  const float bigR = 0.22F * mn;     // ...much larger
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto angdiff = [](float a, float b)
  {
    float d = a - b;
    while (d > 3.14159F)
      d -= 6.28318F;
    while (d < -3.14159F)
      d += 6.28318F;
    return d;
  };
  auto drawPac = [&](std::vector<Rgb>& dst,
                     float cx,
                     float cy,
                     float r,
                     float dir,
                     float mouth,
                     bool textured,
                     float alpha)
  {
    const int x0 = std::max(0, static_cast<int>(cx - r));
    const int x1 = std::min(w - 1, static_cast<int>(cx + r));
    const int y0 = std::max(0, static_cast<int>(cy - r / ya));
    const int y1 = std::min(h - 1, static_cast<int>(cy + r / ya));
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x)
      {
        const float dx = x - cx;
        const float dy = (y - cy) * ya;
        const float rr = std::sqrt(dx * dx + dy * dy);
        if (rr > r)
          continue;
        if (std::fabs(angdiff(std::atan2(dy, dx), dir)) < mouth)
          continue;  // the open mouth wedge
        const float z = std::sqrt(std::max(0.0F, 1.0F - (rr / r) * (rr / r)));
        float yr = 255.0F;
        float yg = 225.0F;
        float yb = 45.0F;
        if (textured)
        {
          const Rgb v = sample(src, w, h, static_cast<float>(x), static_cast<float>(y));
          yr = (v.transparent ? 200.0F : v.r) * 0.4F + 255.0F * 0.6F;
          yg = (v.transparent ? 200.0F : v.g) * 0.4F + 225.0F * 0.6F;
          yb = (v.transparent ? 60.0F : v.b) * 0.4F + 45.0F * 0.6F;
        }
        const float sh = 0.55F + 0.45F * z;
        const std::size_t idx = static_cast<std::size_t>(y) * w + x;
        const Rgb cur = dst[idx];  // alpha-blend so the giant fades in as it forms
        dst[idx] = Rgb{u8(cur.r * (1.0F - alpha) + yr * sh * alpha),
                       u8(cur.g * (1.0F - alpha) + yg * sh * alpha),
                       u8(cur.b * (1.0F - alpha) + yb * sh * alpha),
                       false};
      }
  };
  const float bx = w * 0.70F;
  const float by = (h - 1) * 0.5F;
  runFrames(
      renderer,
      w,
      h,
      5500,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float mouth =
            0.08F + 0.42F * (0.5F + 0.5F * std::sin(t * 180.0F));  // very fast chomp
        float sx = 0;
        float sy = (h - 1) * 0.5F;
        float sr = smallR;
        float sdir = 0.0F;
        float eatFront = 0;
        float gather = 0.0F;  // data-implosion progress
        float bigAlpha = 0.0F;
        float blx = bx;  // big Pac-Man x (lunges in the duel)
        const float eatEnd = 0.42F * w;
        if (t < 0.18F)  // eat across (~1 s before the giant forms)
        {
          const float p = t / 0.18F;
          sx = -smallR + p * (eatEnd + smallR);
          eatFront = sx;
        }
        else if (t < 0.32F)  // SUDDEN: the leftover data flies together into the giant
        {
          const float p = (t - 0.18F) / 0.14F;
          sx = eatEnd;
          eatFront = eatEnd;
          gather = p;
          bigAlpha = std::clamp(p * 1.3F, 0.0F, 1.0F);
        }
        else  // the duel: giant lunges, little one flees
        {
          const float p = (t - 0.32F) / 0.68F;
          eatFront = eatEnd;
          gather = 1.0F;
          bigAlpha = 1.0F;
          const float lunge = 0.5F + 0.5F * std::sin(t * 14.0F);
          blx = bx - p * 0.16F * w - lunge * 0.04F * w;               // advance + menacing snaps
          sx = eatEnd - p * p * (0.7F * w);                           // flee left, accelerating
          sy = (h - 1) * 0.5F + std::sin(t * 34.0F) * h * 0.05F * p;  // terror wobble
          sr = smallR * (1.0F - 0.25F * p);
          sdir = 3.14159F;
        }
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const std::size_t idx = static_cast<std::size_t>(y) * w + x;
            if (x < eatFront)
            {
              dst[idx] = Rgb{0, 0, 0, false};  // eaten
              continue;
            }
            if (gather <= 0.0F)
            {
              const Rgb& s0 = src[idx];  // intact data ahead of the eater
              dst[idx] = s0.transparent ? Rgb{0, 0, 0, false} : s0;
              continue;
            }
            // The leftover data implodes toward the giant's centre.
            const float k = std::max(0.05F, 1.0F - 0.85F * gather);
            const float ssx = bx + (x - bx) / k;
            const float ssy = by + (y - by) / k;
            if (ssx < eatFront)
            {
              dst[idx] = Rgb{0, 0, 0, false};
              continue;
            }
            const Rgb v = sample(src, w, h, ssx, ssy);
            const float dd = 1.0F - 0.55F * gather;  // loose data dims as it gathers
            dst[idx] = v.transparent ? Rgb{0, 0, 0, false}
                                     : Rgb{u8(v.r * dd), u8(v.g * dd), u8(v.b * dd), false};
          }
        if (bigAlpha > 0.01F)
          drawPac(
              dst, blx, by, bigR, 3.14159F, mouth * 1.15F, true, bigAlpha);  // giant, faces left
        if (sr > 1.0F)
          drawPac(dst, sx, sy, sr, sdir, mouth, false, 1.0F);
      });
}

void effectPythonCut(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float cutT = 0.45F;
  // Prefer the alpha-clean transfoot.png; otherwise chroma-key foot.png.
  std::size_t fw = 0, fh = 0;
  std::unique_ptr<ImageSource> foot = loadDataImage("transfoot.png", fw, fh);
  bool needChromaKey = false;
  if (!foot)
  {
    foot = loadFootImage(fw, fh);
    needChromaKey = true;
  }
  if (!foot)
  {
    effectBoneCut(renderer, src, w, h);
    return;
  }
  // Skin-tone chroma key for the renaissance-background fallback.
  auto isFootColour = [](const Rgb& c)
  { return !c.transparent && c.r > c.g + 12 && c.r > c.b + 24 && c.r > 110; };
  auto drawCapsule =
      [&](std::vector<Rgb>& dst, float cx, float cy, float L, float R, float ang, const Rgb& col)
  {
    const float dx = std::cos(ang) * L * 0.5F, dy = std::sin(ang) * L * 0.5F;
    drawSeg(dst, w, h, cx - dx, cy - dy, cx + dx, cy + dy, R, ya, col);
    plotDot(dst, w, h, cx - dx, cy - dy, R * 1.25F, ya, col);
    plotDot(dst, w, h, cx + dx, cy + dy, R * 1.25F, ya, col);
  };
  runFrames(
      renderer, w, h, 5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        if (t < cutT)  // savanna throw — copied from Bone Cut so the cut is exact
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
                      : Rgb{u8(140 + 60 * sf + l * 0.15F), u8(95 + 60 * sf + l * 0.12F),
                            u8(70 + 40 * sf + l * 0.10F), false};
            }
          // The bone rises monotonically to its apex right at the cut, so
          // its on-screen position is the match point for the foot. (The
          // original Bone Cut's parabola lands back on the ground at p=1 —
          // fine for a 2001 jump cut, wrong when the next shot is meant to
          // start at the bone's screen position.)
          const float p = t / cutT;
          const float bx = w * (0.30F + 0.40F * p);
          const float by = h * 0.78F - std::sin(p * 1.5708F) * h * 0.55F;
          const float ang = p * 18.0F + 0.6F;
          const float L = mn * 0.22F, R = mn * 0.022F;
          drawCapsule(dst, bx, by, L, R, ang, Rgb{240, 235, 222, false});
          plotDot(dst, w, h, bx + std::cos(ang) * L * 0.5F, by + std::sin(ang) * L * 0.5F,
                  R * 1.6F, ya, Rgb{240, 235, 222, false});
          plotDot(dst, w, h, bx - std::cos(ang) * L * 0.5F, by - std::sin(ang) * L * 0.5F,
                  R * 1.6F, ya, Rgb{240, 235, 222, false});
        }
        else  // the cut: foot tumbles in space where the bone reached apex
        {
          for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
              dst[static_cast<std::size_t>(y) * w + x] = Rgb{2, 3, 7, false};
          // Stars seeded from data so the field still carries weather pixels.
          for (int i = 0; i < 120; ++i)
          {
            const int sx = static_cast<int>(hash(i * 2) * w);
            const int sy = static_cast<int>(hash(i * 2 + 1) * h);
            if (sx < 0 || sx >= w || sy < 0 || sy >= h) continue;
            const Rgb& s = src[static_cast<std::size_t>(sy) * w + sx];
            const float l = s.transparent ? 100.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            if (l < 90.0F) continue;
            const float tw = 0.5F + 0.5F * std::sin(t * 6.0F + i);
            dst[static_cast<std::size_t>(sy) * w + sx] =
                Rgb{u8(190 + tw * 50), u8(190 + tw * 50), u8(210 + tw * 40), false};
          }
          // The foot picks up exactly where the bone reached apex.
          // Bone end-point: bx = w*0.70, by = h*0.23 (sin(π/2)=1, with the
          // multipliers above). Drift gently toward screen centre afterwards.
          const float p = (t - cutT) / (1.0F - cutT);
          const float startCx = w * 0.70F;
          const float startCy = h * 0.23F;
          const float targetCx = w * 0.55F;
          const float targetCy = h * 0.42F;
          // Ease in to the drift target (1 - (1 - p)^2 — fast at first, soft
          // arrival), so the cut moment lands precisely on the bone.
          const float ease = 1.0F - (1.0F - p) * (1.0F - p);
          const float cx = startCx + (targetCx - startCx) * ease;
          const float cy = startCy + (targetCy - startCy) * ease;
          // Bone's spin rate continued, decaying as the foot slows down.
          const float ang = 18.0F + 0.6F + p * 4.0F;
          // Image aspect-correct sizing — preserve the PNG's pixel aspect.
          const float scaleH = mn * 0.45F;
          const float aspect = (fh > 0) ? static_cast<float>(fw) / static_cast<float>(fh) : 1.5F;
          const float scaleW = scaleH * aspect;
          const float cs = std::cos(-ang), sn = std::sin(-ang);
          // Bounding circle of the rotated sprite (longest half-diagonal).
          const float halfDiag = 0.5F * std::sqrt(scaleW * scaleW + scaleH * scaleH);
          const int x0 = static_cast<int>(std::max(0.0F, cx - halfDiag));
          const int x1 = static_cast<int>(std::min(static_cast<float>(w - 1), cx + halfDiag));
          const int y0 = static_cast<int>(std::max(0.0F, cy - halfDiag / ya));
          const int y1 = static_cast<int>(std::min(static_cast<float>(h - 1), cy + halfDiag / ya));
          for (int yy = y0; yy <= y1; ++yy)
            for (int xx = x0; xx <= x1; ++xx)
            {
              const float dx = static_cast<float>(xx) - cx;
              const float dy = (static_cast<float>(yy) - cy) * ya;
              // Rotate into image-local axes.
              const float lx = dx * cs - dy * sn;
              const float lyL = dx * sn + dy * cs;
              const float u = lx / scaleW + 0.5F;
              const float v = lyL / scaleH + 0.5F;
              if (u < 0.0F || u > 1.0F || v < 0.0F || v > 1.0F) continue;
              const Rgb pix = foot->pixelAtUV(std::clamp(u, 0.0F, 0.999F),
                                              std::clamp(v, 0.0F, 0.999F));
              if (pix.transparent) continue;
              if (needChromaKey && !isFootColour(pix)) continue;
              dst[static_cast<std::size_t>(yy) * w + xx] = pix;
            }
        }
      });
}

void effectPinkPanther(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(
      renderer, w, h, 5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float dim = 0.45F;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 90.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(190 + l * 0.15F * dim), u8(160 + l * 0.13F * dim),
                    u8(190 + l * 0.15F * dim), false};
          }
        const float walkY = h * 0.78F;
        const float cx = w * 1.15F - t * w * 1.4F;  // right -> left
        const Rgb pink{232, 92, 168, false};
        const Rgb darkPink{170, 50, 120, false};
        // Magenta ink trail behind the cat (everything to the right of cx).
        for (int sx = std::max(0, static_cast<int>(cx) + 1); sx < w; sx += 1)
        {
          const float yy = walkY + std::sin(sx * 0.05F + t * 4.0F) * mn * 0.008F;
          plotDot(dst, w, h, static_cast<float>(sx), yy, std::max(1.0F, mn * 0.008F), ya, darkPink);
        }
        if (cx > -mn * 0.6F && cx < w + mn * 0.6F)
        {
          const float s = mn * 0.10F;
          const float bobY = walkY + std::sin(t * 18.0F) * mn * 0.012F;
          // Body — elongated horizontal oval.
          for (int yo = -static_cast<int>(s * 0.4F); yo <= static_cast<int>(s * 0.4F); ++yo)
            for (int xo = -static_cast<int>(s * 0.95F); xo <= static_cast<int>(s * 0.95F); ++xo)
            {
              const float nx = xo / (s * 0.95F);
              const float ny = yo / (s * 0.4F);
              if (nx * nx + ny * ny <= 1.0F)
              {
                const int xx = static_cast<int>(cx + xo);
                const int yy = static_cast<int>(bobY + yo);
                if (xx >= 0 && xx < w && yy >= 0 && yy < h)
                  dst[static_cast<std::size_t>(yy) * w + xx] = pink;
              }
            }
          // Tail — sweeping curl above the body.
          for (int k = 0; k < 18; ++k)
          {
            const float u = k / 17.0F;
            const float tx = cx + s * 0.85F + std::cos(u * 3.2F) * s * 0.35F;
            const float tyy = bobY - s * 0.4F - u * s * 0.7F + std::sin(u * 4.0F) * s * 0.08F;
            plotDot(dst, w, h, tx, tyy, std::max(1.0F, mn * 0.008F), ya, pink);
          }
          // Head, ears, eye, nose.
          plotDot(dst, w, h, cx - s * 0.95F, bobY - s * 0.45F, s * 0.30F, ya, pink);
          plotDot(dst, w, h, cx - s * 1.10F, bobY - s * 0.80F, s * 0.11F, ya, pink);
          plotDot(dst, w, h, cx - s * 0.78F, bobY - s * 0.80F, s * 0.11F, ya, pink);
          plotDot(dst, w, h, cx - s * 0.95F, bobY - s * 0.45F, std::max(1.0F, mn * 0.006F), ya,
                  Rgb{30, 12, 30, false});
          plotDot(dst, w, h, cx - s * 1.15F, bobY - s * 0.35F, std::max(1.0F, mn * 0.008F), ya,
                  Rgb{210, 70, 140, false});
          // Walking legs.
          for (int leg = 0; leg < 4; ++leg)
          {
            const float ph = t * 18.0F + leg * 1.5F;
            const float legX = cx + (leg - 1.5F) * s * 0.32F;
            const float lift = std::max(0.0F, std::sin(ph)) * s * 0.18F;
            drawSeg(dst, w, h, legX, bobY - s * 0.05F, legX + std::cos(ph) * s * 0.10F,
                    bobY + s * 0.30F - lift, std::max(1.0F, s * 0.07F), ya, pink);
          }
        }
      });
}

void effectAcmeAnvil(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 4400,
            [&](float t, std::vector<Rgb>& dst)
            {
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                  dst[static_cast<std::size_t>(y) * w + x] =
                      src[static_cast<std::size_t>(y) * w + x];
              const float dropT = 0.62F;
              const float p = std::clamp(t / dropT, 0.0F, 1.0F);
              const float fall = p * p;
              const float anvilCx = w * 0.5F;
              const float anvilCy = -mn * 0.4F + fall * h * 0.62F;
              const float anvilW = mn * 0.42F;
              const float anvilH = mn * 0.24F;
              const float squash = t > dropT ? std::clamp((t - dropT) / 0.06F, 0.0F, 1.0F) : 0.0F;
              const float aWf = anvilW * (1.0F + 0.45F * squash);
              const float aHf = anvilH * (1.0F - 0.5F * squash);
              const Rgb steel{55, 58, 68, false};
              const Rgb dark{16, 18, 22, false};
              // Anvil silhouette: classic hourglass-ish profile, dark outline.
              for (int yo = -static_cast<int>(aHf * 0.5F); yo <= static_cast<int>(aHf * 0.5F);
                   ++yo)
              {
                const int yy = static_cast<int>(anvilCy + yo);
                if (yy < 0 || yy >= h) continue;
                const float yf = static_cast<float>(yo) / (aHf * 0.5F);
                float scale;
                if (yf < -0.6F) scale = 1.05F + 0.15F * (-yf - 0.6F);  // top crown
                else if (yf < 0.1F) scale = 0.55F + 0.3F * (yf + 0.6F);  // narrow waist
                else scale = 0.85F + 0.20F * yf;  // base
                const int half = static_cast<int>(aWf * 0.5F * scale);
                for (int xo = -half; xo <= half; ++xo)
                {
                  const int xx = static_cast<int>(anvilCx + xo);
                  if (xx < 0 || xx >= w) continue;
                  const float xf = std::fabs(xo) / static_cast<float>(std::max(1, half));
                  dst[static_cast<std::size_t>(yy) * w + xx] = (xf > 0.93F) ? dark : steel;
                }
              }
              // "ACME" centred on the body (5x7 stencil glyphs via glyph5x7).
              if (anvilCy > -mn * 0.05F)
              {
                const int sc = std::max(1, static_cast<int>(aWf * 0.04F));
                const char* str = "ACME";
                const int W = (5 + 1) * 4 - 1;
                const float startX = anvilCx - W * sc * 0.5F;
                const float startY = anvilCy - aHf * 0.18F;
                for (int ci = 0; ci < 4; ++ci)
                {
                  const auto g = glyph5x7(str[ci]);
                  for (int fy = 0; fy < 7; ++fy)
                    for (int fx = 0; fx < 5; ++fx)
                      if (g[fy][fx] == '1')
                        plotDot(dst, w, h, startX + (ci * 6 + fx) * sc, startY + fy * sc,
                                std::max(1.0F, static_cast<float>(sc) * 0.55F), ya,
                                Rgb{210, 210, 220, false});
                }
              }
              // Dust kick-up on impact.
              if (t > dropT)
              {
                const float du = std::clamp((t - dropT) / 0.30F, 0.0F, 1.0F);
                const Rgb dust{190, 178, 156, false};
                for (int i = 0; i < 28; ++i)
                {
                  const float a = i / 28.0F * 6.2832F + hash(i) * 0.3F;
                  const float rr = du * mn * 0.35F * (0.7F + 0.6F * hash(i * 3));
                  plotDot(dst, w, h, anvilCx + std::cos(a) * rr,
                          anvilCy + aHf * 0.5F + std::sin(a) * rr * 0.25F,
                          mn * 0.028F * (1.0F - du), ya, dust);
                }
              }
            });
}

void effectSillyWalk(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };

  // Load the CMU walk capture once. Marionette puppet replaces the
  // ad-hoc stick figure; the walk cycle reads as a "normal" walk so
  // the silliness comes from the exaggerated body bob + tilt + the
  // bowler hat + briefcase staples of the Python sketch.
  BvhAnimation walk;
  double walkRefH = 1.0;
  bool walkOk = false;
  const std::string walkPath = findDataImage("cmu/walk.bvh");
  if (!walkPath.empty())
  {
    try
    {
      walk = loadBvhFile(walkPath);
      walkRefH = bvhReferenceHeight(walk);
      walkOk = true;
    }
    catch (const std::exception&) { walkOk = false; }
  }

  runFrames(
      renderer, w, h, 5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float dim = 0.35F;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 60.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(180 + l * 0.10F * dim), u8(170 + l * 0.10F * dim),
                    u8(150 + l * 0.10F * dim), false};
          }
        const float groundY = h * 0.90F;
        for (int x = 0; x < w; ++x)
          dst[static_cast<std::size_t>(groundY) * w + x] = Rgb{50, 40, 30, false};

        // Scroll the figure across the screen left → right.
        const float cx = -mn * 0.3F + t * (w + mn * 0.6F);
        // Exaggerated bob and forward-lean so the Cleese silliness reads
        // even though the underlying gait is a normal CMU walk.
        const float bob = std::sin(t * 14.0F) * h * 0.015F;
        const float cy = groundY + bob;
        const float figH = h * 0.55F;
        const Rgb ink{20, 20, 30, false};

        std::vector<std::array<double, 2>> joints;
        if (walkOk)
        {
          // Cycle through the walk a few times across the scroll.
          const float phase = t * 5.0F;
          const int fi = static_cast<int>(std::floor(phase * walk.frameCount)) %
                         std::max(1, walk.frameCount);
          drawMarionette(dst, w, h, ya, walk, fi, cx, cy, figH, walkRefH,
                         ink, &joints);
        }

        // Bowler hat sits above the head joint.
        if (!joints.empty())
        {
          const int headI = walk.jointIndex("Head");
          const int rWristI = walk.jointIndex("RightHand");
          if (headI >= 0 && headI < static_cast<int>(joints.size()))
          {
            const double hx = joints[headI][0];
            const double hy = joints[headI][1];
            const double bodyUnit = std::max(1.0, static_cast<double>(figH) / 30.0);
            // Brim: a thin horizontal slab just above the head disc.
            const double brimY = hy - bodyUnit * 2.0;
            const double brimHalfW = bodyUnit * 2.6;
            drawCapsule(dst, w, h, ya,
                        hx - brimHalfW, brimY, hx + brimHalfW, brimY,
                        bodyUnit * 0.35, bodyUnit * 0.35, ink);
            // Crown: a rounded dome above the brim.
            for (int yo = -static_cast<int>(bodyUnit * 2.4); yo <= 0; ++yo)
            {
              const int yy = static_cast<int>(brimY) + yo;
              const double w0 = bodyUnit * 1.6 *
                                std::sqrt(std::max(0.0, 1.0 - std::pow(yo / (bodyUnit * 2.4), 2)));
              for (int xo = -static_cast<int>(w0); xo <= static_cast<int>(w0); ++xo)
              {
                const int xx = static_cast<int>(hx) + xo;
                if (xx >= 0 && xx < w && yy >= 0 && yy < h)
                  dst[static_cast<std::size_t>(yy) * w + xx] = ink;
              }
            }
          }
          if (rWristI >= 0 && rWristI < static_cast<int>(joints.size()))
          {
            // Briefcase dangles below the right hand.
            const double bx = joints[rWristI][0];
            const double by = joints[rWristI][1];
            const double bodyUnit = std::max(1.0, static_cast<double>(figH) / 30.0);
            const Rgb leather{80, 60, 40, false};
            const double caseH = bodyUnit * 2.2;
            const double caseW = bodyUnit * 1.6;
            for (int yo = 0; yo < static_cast<int>(caseH / ya); ++yo)
            {
              const int yy = static_cast<int>(by) + yo + static_cast<int>(bodyUnit * 0.5);
              for (int xo = -static_cast<int>(caseW); xo <= static_cast<int>(caseW); ++xo)
              {
                const int xx = static_cast<int>(bx) + xo;
                if (xx >= 0 && xx < w && yy >= 0 && yy < h)
                  dst[static_cast<std::size_t>(yy) * w + xx] = leather;
              }
            }
          }
        }
      });
}

void effectThatsAllFolks(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(
      renderer, w, h, 5200,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float cx = w * 0.5F, cy = h * 0.5F;
        const float maxR =
            std::hypot(std::max(cx, w - cx), std::max(cy * ya, (h - cy) * ya));
        const float irisR = std::max(0.0F, (1.0F - t * 1.05F) * maxR);
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const float gd = std::hypot(x - cx, (y - cy) * ya);
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            if (gd > irisR)
              dst[static_cast<std::size_t>(y) * w + x] =
                  Rgb{u8(s.r * 0.20F), u8(s.g * 0.20F + 4), u8(s.b * 0.20F + 12), false};
            else
            {
              const float wob = std::sin(t * 8.0F + std::atan2((y - cy) * ya, x - cx) * 6.0F) * mn * 0.012F;
              if (gd > irisR + wob - mn * 0.003F && gd < irisR + wob)
                dst[static_cast<std::size_t>(y) * w + x] = Rgb{220, 180, 80, false};
              else
                dst[static_cast<std::size_t>(y) * w + x] = s;
            }
          }
        const std::string text = "THAT'S ALL FOLKS!";
        const int sc = std::max(2, static_cast<int>(mn / 60.0F));
        const float lineW = static_cast<float>(text.size()) * 6 * sc;
        const float fadeT = std::clamp(t * 1.6F - 0.5F, 0.0F, 1.0F);
        for (int ci = 0; ci < static_cast<int>(text.size()); ++ci)
        {
          const char ch = text[ci];
          if (ch == ' ') continue;
          const auto g = glyph5x7(ch);
          const float wobY = std::sin(ci * 1.1F + t * 6.0F) * mn * 0.015F;
          for (int fy = 0; fy < 7; ++fy)
            for (int fx = 0; fx < 5; ++fx)
              if (g[fy][fx] == '1')
                plotDot(dst, w, h, cx - lineW * 0.5F + (ci * 6 + fx) * sc,
                        cy + fy * sc + wobY,
                        std::max(1.0F, static_cast<float>(sc) * 0.6F * fadeT), ya,
                        Rgb{u8(220 * fadeT), u8(40 * fadeT), u8(60 * fadeT), false});
        }
      });
}

void effectCrystalBall(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
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
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(50 + dl * 0.20F), u8(20 + dl * 0.10F), u8(70 + dl * 0.20F), false};
        }
      // Pedestal.
      const float cx = w * 0.5F, cy = h * 0.55F;
      const float R = mn * 0.22F;
      drawSeg(dst, w, h, cx, cy + R / ya, cx, h * 0.90F, mn * 0.020F, ya, Rgb{80, 40, 20, false});
      // Crystal ball — translucent disc.
      for (int yo = -static_cast<int>(R / ya); yo <= static_cast<int>(R / ya); ++yo)
        for (int xo = -static_cast<int>(R); xo <= static_cast<int>(R); ++xo) {
          const float nx = xo / R, nyN = yo * ya / R;
          const float r2 = nx * nx + nyN * nyN;
          if (r2 > 1.0F) continue;
          const int xx = static_cast<int>(cx + xo), yy = static_cast<int>(cy + yo);
          if (xx < 0 || xx >= w || yy < 0 || yy >= h) continue;
          const float shade = 0.6F + 0.4F * std::sqrt(1.0F - r2);
          const Rgb& orig = dst[static_cast<std::size_t>(yy) * w + xx];
          dst[static_cast<std::size_t>(yy) * w + xx] =
              Rgb{u8(orig.r * 0.30F + 80 * shade), u8(orig.g * 0.30F + 100 * shade),
                  u8(orig.b * 0.30F + 140 * shade), false};
        }
      // Foot floating in the ball.
      drawFootSprite(dst, w, h, ya, f.img.get(), f.fw, f.fh, f.needChromaKey,
                     cx, cy, R * 0.95F, t * 1.5F);
      // Highlight on the ball.
      plotDot(dst, w, h, cx - R * 0.35F, cy - R * 0.35F / ya, R * 0.10F, ya, Rgb{240, 240, 255, false});
    });
}

void effectTopHat(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
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
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(50 + dl * 0.20F), u8(20 + dl * 0.10F), u8(40 + dl * 0.10F), false};
        }
      // Stage (red curtain backdrop already in tint).
      const float cx = w * 0.50F, hatY = h * 0.70F;
      // Hat body (upturned, opening up).
      const float crownW = mn * 0.16F, brimW = mn * 0.22F;
      for (int yo = -static_cast<int>(mn * 0.15F); yo <= 0; ++yo)
        for (int xo = -static_cast<int>(crownW); xo <= static_cast<int>(crownW); ++xo) {
          const int xx = static_cast<int>(cx + xo), yy = static_cast<int>(hatY + yo);
          if (xx >= 0 && xx < w && yy >= 0 && yy < h)
            dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{15, 15, 25, false};
        }
      // Brim.
      drawSeg(dst, w, h, cx - brimW, hatY, cx + brimW, hatY,
              std::max(1.0F, mn * 0.015F), ya, Rgb{15, 15, 25, false});
      // Hat opening (inside).
      drawSeg(dst, w, h, cx - crownW * 0.85F, hatY - mn * 0.15F, cx + crownW * 0.85F, hatY - mn * 0.15F,
              std::max(1.0F, mn * 0.010F), ya, Rgb{60, 30, 30, false});
      // Foot rising out.
      const float rise = std::clamp(t * 1.4F, 0.0F, 1.0F);
      const float fcy = hatY - mn * 0.15F - rise * mn * 0.25F;
      drawFootSprite(dst, w, h, ya, f.img.get(), f.fw, f.fh, f.needChromaKey,
                     cx, fcy, mn * 0.16F, std::sin(t * 1.5F) * 0.15F);
      // Magician's gloved hand reaching above.
      drawSeg(dst, w, h, cx + mn * 0.08F, hatY - mn * 0.30F, cx + mn * 0.02F,
              hatY - mn * 0.18F, mn * 0.015F, ya, Rgb{240, 240, 240, false});
      // Sparkles around the foot.
      for (int k = 0; k < 12; ++k) {
        const float a = k / 12.0F * 6.2832F + t * 2.0F;
        plotDot(dst, w, h, cx + std::cos(a) * mn * 0.12F, fcy + std::sin(a) * mn * 0.10F,
                std::max(1.0F, mn * 0.003F), ya, Rgb{255, 240, 120, false});
      }
    });
}

void effectTrophy(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
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
          const float dl = d.transparent ? 50.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
          dst[static_cast<std::size_t>(y) * w + x] =
              Rgb{u8(20 + dl * 0.15F), u8(20 + dl * 0.15F), u8(60 + dl * 0.20F), false};
        }
      // Podium tiers (1, 2, 3).
      const float cx = w * 0.5F;
      const float baseY = h * 0.92F;
      const float tier1Y = h * 0.65F;
      const float tier1W = mn * 0.10F;
      for (int yo = 0; yo <= static_cast<int>(baseY - tier1Y); ++yo)
        for (int xo = -static_cast<int>(tier1W); xo <= static_cast<int>(tier1W); ++xo) {
          const int xx = static_cast<int>(cx + xo), yy = static_cast<int>(tier1Y + yo);
          if (xx >= 0 && xx < w && yy >= 0 && yy < h)
            dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{200, 160, 60, false};
        }
      // Tier "1" label.
      const int sc = std::max(2, static_cast<int>(mn / 30.0F));
      const auto g1 = glyph5x7('1');
      for (int fy = 0; fy < 7; ++fy)
        for (int fx = 0; fx < 5; ++fx)
          if (g1[fy][fx] == '1')
            plotDot(dst, w, h, cx - 2 * sc + fx * sc, tier1Y + mn * 0.06F + fy * sc,
                    std::max(1.0F, static_cast<float>(sc) * 0.5F), ya, Rgb{40, 30, 10, false});
      // Winner silhouette on podium.
      const float wy = tier1Y - mn * 0.10F;
      drawSeg(dst, w, h, cx, wy, cx, tier1Y, mn * 0.030F, ya, Rgb{30, 30, 60, false});
      plotDot(dst, w, h, cx, wy - mn * 0.03F, mn * 0.030F, ya, Rgb{220, 180, 140, false});
      // Arms raised, foot held aloft.
      const float armAngle = std::sin(t * 4.0F) * 0.08F;
      drawSeg(dst, w, h, cx, wy, cx + std::cos(-1.5708F + armAngle) * mn * 0.10F,
              wy + std::sin(-1.5708F + armAngle) * mn * 0.10F, mn * 0.020F, ya,
              Rgb{220, 180, 140, false});
      drawFootSprite(dst, w, h, ya, f.img.get(), f.fw, f.fh, f.needChromaKey,
                     cx, wy - mn * 0.18F, mn * 0.15F, std::sin(t * 4.0F) * 0.10F);
      // Confetti.
      for (int i = 0; i < 60; ++i) {
        const float fa = std::fmod(t * 0.7F + hash(i), 1.0F);
        const float fx = hash(i * 3) * w;
        const float fy = fa * h;
        const Rgb col = (i & 1) ? Rgb{255, 80, 80, false}
                                : ((i & 2) ? Rgb{80, 255, 80, false}
                                            : Rgb{80, 80, 255, false});
        plotDot(dst, w, h, fx, fy, std::max(1.0F, mn * 0.003F), ya, col);
      }
    });
}

}  // namespace ee_detail
}  // namespace Qdless
