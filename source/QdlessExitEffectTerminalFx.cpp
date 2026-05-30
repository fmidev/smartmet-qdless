#include "QdlessExitEffectCommon.h"

namespace Qdless
{
namespace ee_detail
{

void effectExplode(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float cx = (w - 1) * 0.5F;
  const float cy = (h - 1) * 0.5F;
  runFrames(renderer,
            w,
            h,
            1600,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float scale = 1.0F + 6.0F * t * t;  // ease-in: slow start, fast blow-out
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& c = src[static_cast<std::size_t>(y) * w + x];
                  if (c.transparent)
                    continue;
                  const int nx = static_cast<int>(std::lround(cx + (x - cx) * scale));
                  const int ny = static_cast<int>(std::lround(cy + (y - cy) * scale));
                  if (nx >= 0 && nx < w && ny >= 0 && ny < h)
                    dst[static_cast<std::size_t>(ny) * w + nx] = c;
                }
            });
}

void effectImplodeRing(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float cx = (w - 1) * 0.5F;
  const float cy = (h - 1) * 0.5F;
  const float ya = yAspectFor(renderer);
  const float maxR = std::sqrt(cx * cx + (cy * ya) * (cy * ya));
  const float ringW = std::max(2.0F, maxR * 0.10F);
  runFrames(renderer,
            w,
            h,
            1800,
            [&](float t, std::vector<Rgb>& dst)
            {
              if (t < 0.5F)
              {
                // Implode: scatter every sub-pixel towards the centre.
                const float s = 1.0F - t / 0.5F;  // 1 -> 0
                for (int y = 0; y < h; ++y)
                  for (int x = 0; x < w; ++x)
                  {
                    const Rgb& c = src[static_cast<std::size_t>(y) * w + x];
                    if (c.transparent)
                      continue;
                    const int nx = static_cast<int>(std::lround(cx + (x - cx) * s));
                    const int ny = static_cast<int>(std::lround(cy + (y - cy) * s));
                    if (nx >= 0 && nx < w && ny >= 0 && ny < h)
                      dst[static_cast<std::size_t>(ny) * w + nx] = c;
                  }
              }
              else
              {
                // Burst: a fading white ring sweeps outward.
                const float tb = (t - 0.5F) / 0.5F;  // 0 -> 1
                const float radius = tb * maxR;
                const auto v = static_cast<std::uint8_t>(std::lround(255.0F * (1.0F - tb)));
                const Rgb ring{v, v, v, false};
                for (int y = 0; y < h; ++y)
                  for (int x = 0; x < w; ++x)
                  {
                    const float dx = x - cx;
                    const float dy = (y - cy) * ya;
                    const float r = std::sqrt(dx * dx + dy * dy);
                    if (std::fabs(r - radius) <= ringW)
                      dst[static_cast<std::size_t>(y) * w + x] = ring;
                  }
              }
            });
}

void effectSpiral(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float cx = (w - 1) * 0.5F;
  const float cy = (h - 1) * 0.5F;
  const float ya = yAspectFor(renderer);
  runFrames(renderer,
            w,
            h,
            2200,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float scale = std::max(0.001F, 1.0F - t);  // shrink 1 -> 0
              const float spin = t * 9.0F;                     // total twist (radians)
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  // Inverse map: where in the source does this screen pixel come from?
                  const float dx = x - cx;
                  const float dy = (y - cy) * ya;
                  const float r = std::sqrt(dx * dx + dy * dy);
                  const float a = std::atan2(dy, dx) - spin;
                  const float sr = r / scale;  // shrinking image => source radius grows
                  const float sx = cx + sr * std::cos(a);
                  const float sy = cy + sr * std::sin(a) / ya;
                  dst[static_cast<std::size_t>(y) * w + x] = sample(src, w, h, sx, sy);
                }
            });
}

void effectDissolve(
    const Renderer& renderer, const std::vector<Rgb>& src, int w, int h, std::mt19937& rng)
{
  std::vector<int> order(static_cast<std::size_t>(w) * h);
  std::iota(order.begin(), order.end(), 0);
  std::shuffle(order.begin(), order.end(), rng);
  const auto n = static_cast<float>(order.size());
  runFrames(renderer,
            w,
            h,
            1600,
            [&](float t, std::vector<Rgb>& dst)
            {
              std::copy(src.begin(), src.end(), dst.begin());
              const auto gone = static_cast<std::size_t>(t * n);
              for (std::size_t k = 0; k < gone && k < order.size(); ++k)
                dst[order[k]] = kBlank;
            });
}

void effectCrtOff(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float cx = (w - 1) * 0.5F;
  const float cy = (h - 1) * 0.5F;
  const float halfH = h * 0.5F;
  const float halfW = w * 0.5F;
  runFrames(renderer,
            w,
            h,
            1400,
            [&](float t, std::vector<Rgb>& dst)
            {
              if (t < 0.6F)
              {
                // Vertical squash: map the full image into a shrinking centre band.
                const float band = std::max(0.5F, halfH * (1.0F - t / 0.6F));
                for (int y = 0; y < h; ++y)
                {
                  if (std::fabs(y - cy) > band)
                    continue;
                  const float sy = cy + (y - cy) / band * halfH;
                  for (int x = 0; x < w; ++x)
                    dst[static_cast<std::size_t>(y) * w + x] =
                        sample(src, w, h, static_cast<float>(x), sy);
                }
              }
              else if (t < 0.9F)
              {
                // Horizontal collapse of the centre line into a glowing white bar.
                const float tb = (t - 0.6F) / 0.3F;  // 0 -> 1
                const float halfBar = std::max(0.5F, halfW * (1.0F - tb));
                const int row = static_cast<int>(std::lround(cy));
                for (int x = 0; x < w; ++x)
                  if (std::fabs(x - cx) <= halfBar)
                    dst[static_cast<std::size_t>(row) * w + x] = Rgb{255, 255, 255, false};
              }
              else
              {
                // Final dot flash, then runFrames' next blank (and the caller's clear).
                const auto v = static_cast<std::uint8_t>(std::lround(255.0F * (1.0F - t) / 0.1F));
                const int row = static_cast<int>(std::lround(cy));
                const int col = static_cast<int>(std::lround(cx));
                dst[static_cast<std::size_t>(row) * w + col] = Rgb{v, v, v, false};
              }
            });
}

void effectFade(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  runFrames(renderer,
            w,
            h,
            1600,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float k = 1.0F - t;
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
            });
}

void effectFire(
    const Renderer& renderer, const std::vector<Rgb>& src, int w, int h, std::mt19937& rng)
{
  const float flameH = std::max(3.0F, h * 0.14F);  // thickness of the burning band
  // Per-column phase so the flame edge is ragged, not a flat line.
  std::vector<float> phase(w);
  std::uniform_real_distribution<float> dPhase(0.0F, 6.28318F);
  for (int x = 0; x < w; ++x)
    phase[x] = dPhase(rng);
  std::uniform_real_distribution<float> dFlick(0.6F, 1.0F);

  runFrames(renderer,
            w,
            h,
            2600,
            [&](float t, std::vector<Rgb>& dst)
            {
              // Front sweeps from below the bottom (t=0) to above the top (t=1).
              const float front = h + flameH - t * (h + 2.0F * flameH);
              for (int x = 0; x < w; ++x)
              {
                const float f = front + flameH * 0.5F * std::sin(phase[x] + t * 18.0F);
                for (int y = 0; y < h; ++y)
                {
                  const std::size_t idx = static_cast<std::size_t>(y) * w + x;
                  if (y < f)
                  {
                    dst[idx] = src[idx];  // intact, above the fire
                  }
                  else if (y < f + flameH)
                  {
                    // Flame band: hot yellow at the leading edge, red deeper.
                    const float d = (y - f) / flameH;  // 0 at front .. 1 deep
                    const float flick = dFlick(rng);
                    const auto r = static_cast<std::uint8_t>(std::lround(255.0F * flick));
                    const auto g = static_cast<std::uint8_t>(
                        std::lround(std::max(0.0F, 1.0F - d) * 200.0F * flick));
                    const auto b =
                        static_cast<std::uint8_t>(std::lround(std::max(0.0F, 0.3F - d) * 120.0F));
                    dst[idx] = Rgb{r, g, b, false};
                  }
                  // else: already burned -> stays blank
                }
              }
            });
}

void effectFireworks(
    const Renderer& renderer, const std::vector<Rgb>& src, int w, int h, std::mt19937& rng)
{
  const float ya = yAspectFor(renderer);
  struct Burst
  {
    float cx, cy, start, hue, maxR;
  };
  std::uniform_real_distribution<float> dX(w * 0.15F, w * 0.85F);
  std::uniform_real_distribution<float> dY(h * 0.12F, h * 0.55F);
  std::uniform_real_distribution<float> dStart(0.05F, 0.55F);
  std::uniform_real_distribution<float> dHue(0.0F, 1.0F);
  std::uniform_real_distribution<float> dR(0.30F, 0.48F);
  constexpr int kBursts = 5;
  std::vector<Burst> bursts(kBursts);
  for (auto& b : bursts)
    b = Burst{dX(rng), dY(rng), dStart(rng), dHue(rng), dR(rng) * w};

  constexpr int kRays = 30;
  runFrames(renderer,
            w,
            h,
            3000,
            [&](float t, std::vector<Rgb>& dst)
            {
              // Phase A: fade the captured frame to night over the first 30%.
              if (t < 0.3F)
              {
                const float k = 1.0F - t / 0.3F;
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
              for (const auto& b : bursts)
              {
                const float lt = (t - b.start) / 0.5F;  // local lifetime 0..1
                if (lt <= 0.0F || lt >= 1.0F)
                  continue;
                const float radius = lt * b.maxR;
                const float grav = 0.20F * b.maxR * lt * lt;  // particles droop
                const Rgb col = hsv2rgb(b.hue, 0.55F, std::min(1.0F, (1.0F - lt) + 0.2F));
                for (int k = 0; k < kRays; ++k)
                {
                  const float a = static_cast<float>(k) / kRays * 6.28318F;
                  plotDot(dst,
                          w,
                          h,
                          b.cx + radius * std::cos(a),
                          b.cy + (radius * std::sin(a)) / ya + grav,
                          1.4F,
                          ya,
                          col);
                }
              }
            });
}

void effectTheEnd(
    const Renderer& renderer, const std::vector<Rgb>& src, int w, int h, std::mt19937& rng)
{
  // 5x7 bitmap font for the glyphs we need (rows top -> bottom).
  auto glyph = [](char c) -> std::array<const char*, 7>
  {
    switch (c)
    {
      case 'T':
        return {"11111", "00100", "00100", "00100", "00100", "00100", "00100"};
      case 'H':
        return {"10001", "10001", "10001", "11111", "10001", "10001", "10001"};
      case 'E':
        return {"11111", "10000", "10000", "11110", "10000", "10000", "11111"};
      case 'N':
        return {"10001", "11001", "11001", "10101", "10011", "10011", "10001"};
      case 'D':
        return {"11110", "10001", "10001", "10001", "10001", "10001", "11110"};
      default:
        return {"00000", "00000", "00000", "00000", "00000", "00000", "00000"};  // space
    }
  };
  const std::string text = "THE END";
  constexpr int kFW = 5;
  constexpr int kFH = 7;
  constexpr int kGap = 1;
  const int cols = static_cast<int>(text.size());
  const int totalFx = cols * kFW + (cols - 1) * kGap;
  const int scale = std::max(1, static_cast<int>(std::min(0.60F * w / totalFx, 0.32F * h / kFH)));
  const float ox = (w - static_cast<float>(totalFx) * scale) * 0.5F;
  const float oy = (h - static_cast<float>(kFH) * scale) * 0.5F;

  // One particle per lit font sub-pixel: a random start, its target in the
  // text, and the colour it picks up from the frame at its start.
  struct Particle
  {
    float sx, sy, tx, ty;
    Rgb col;
  };
  std::vector<Particle> parts;
  std::uniform_real_distribution<float> dPx(0.0F, static_cast<float>(w));
  std::uniform_real_distribution<float> dPy(0.0F, static_cast<float>(h));
  for (int ci = 0; ci < cols; ++ci)
  {
    const auto g = glyph(text[static_cast<std::size_t>(ci)]);
    const int charOx = ci * (kFW + kGap);
    for (int fy = 0; fy < kFH; ++fy)
      for (int fx = 0; fx < kFW; ++fx)
      {
        if (g[fy][fx] != '1')
          continue;
        for (int sy = 0; sy < scale; ++sy)
          for (int sx = 0; sx < scale; ++sx)
          {
            const float px = dPx(rng);
            const float py = dPy(rng);
            Rgb col = sample(src, w, h, px, py);
            if (col.transparent)
              col = Rgb{220, 220, 220, false};
            parts.push_back(
                Particle{px, py, ox + (charOx + fx) * scale + sx, oy + fy * scale + sy, col});
          }
      }
  }

  runFrames(renderer,
            w,
            h,
            3800,
            [&](float t, std::vector<Rgb>& dst)
            {
              if (t < 0.55F)
              {
                // Fade the frame away while the particles fly in (ease-out).
                const float ta = t / 0.55F;
                const float k = 1.0F - ta;
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
                const float e = 1.0F - (1.0F - ta) * (1.0F - ta) * (1.0F - ta);
                for (const auto& p : parts)
                {
                  const int x = static_cast<int>(std::lround(p.sx + (p.tx - p.sx) * e));
                  const int y = static_cast<int>(std::lround(p.sy + (p.ty - p.sy) * e));
                  if (x >= 0 && x < w && y >= 0 && y < h)
                    dst[static_cast<std::size_t>(y) * w + x] = p.col;
                }
              }
              else
              {
                // Hold the assembled text, then fade it out over the last 20%.
                const float fade = t < 0.8F ? 1.0F : std::max(0.0F, 1.0F - (t - 0.8F) / 0.2F);
                for (const auto& p : parts)
                {
                  const int x = static_cast<int>(std::lround(p.tx));
                  const int y = static_cast<int>(std::lround(p.ty));
                  if (x < 0 || x >= w || y < 0 || y >= h)
                    continue;
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{static_cast<std::uint8_t>(p.col.r * fade),
                          static_cast<std::uint8_t>(p.col.g * fade),
                          static_cast<std::uint8_t>(p.col.b * fade),
                          false};
                }
              }
            });
}

void effectEyewink(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float cx = (w - 1) * 0.5F;
  const float cy = (h - 1) * 0.5F;
  const float halfW = w * 0.5F;
  const Rgb lash{20, 20, 24, false};
  runFrames(renderer,
            w,
            h,
            1600,
            [&](float t, std::vector<Rgb>& dst)
            {
              // Ease the close so the eye snaps shut near the end.
              const float openH = (h * 0.5F) * (1.0F - t) * (1.0F - t);
              for (int x = 0; x < w; ++x)
              {
                const float nx = (x - cx) / halfW;  // -1..1 across the eye
                if (std::fabs(nx) >= 1.0F)
                  continue;  // past the eye corners -> stays blank
                const float lid = openH * std::sqrt(std::max(0.0F, 1.0F - nx * nx));
                const int yTop = static_cast<int>(std::ceil(cy - lid));
                const int yBot = static_cast<int>(std::floor(cy + lid));
                for (int y = yTop; y <= yBot; ++y)
                {
                  if (y < 0 || y >= h)
                    continue;
                  const std::size_t idx = static_cast<std::size_t>(y) * w + x;
                  // Thin dark lash line along the lid edges; frame inside.
                  dst[idx] = (y == yTop || y == yBot) ? lash : src[idx];
                }
              }
            });
}

void effectTeardrop(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float cx = (w - 1) * 0.5F;
  const float cy = (h - 1) * 0.5F;
  const float halfW = w * 0.46F;  // eye half-width
  const float eyeH = h * 0.34F;   // open-eye half-height at the centre
  const Rgb lash{20, 20, 24, false};
  const Rgb tear{120, 185, 240, false};
  const Rgb tearDim{50, 80, 110, false};
  const float tx = cx - halfW * 0.33F;  // tear runs down a bit off-centre
  runFrames(renderer,
            w,
            h,
            3800,
            [&](float t, std::vector<Rgb>& dst)
            {
              // Eye half-height: close in from full screen -> open eye -> shut.
              float lensH = eyeH;
              if (t < 0.3F)
                lensH = h * 0.5F + (eyeH - h * 0.5F) * (t / 0.3F);
              else if (t >= 0.7F)
                lensH = eyeH * std::max(0.0F, 1.0F - (t - 0.7F) / 0.3F);
              // The eye's contents dissipate (fade) over the second half.
              const float fade = t < 0.5F ? 1.0F : std::max(0.0F, 1.0F - (t - 0.5F) / 0.5F);
              for (int x = 0; x < w; ++x)
              {
                const float nx = (x - cx) / halfW;
                if (std::fabs(nx) >= 1.0F)
                  continue;
                const float lid = lensH * std::sqrt(std::max(0.0F, 1.0F - nx * nx));
                const int yTop = static_cast<int>(std::ceil(cy - lid));
                const int yBot = static_cast<int>(std::floor(cy + lid));
                for (int y = yTop; y <= yBot; ++y)
                {
                  if (y < 0 || y >= h)
                    continue;
                  const std::size_t idx = static_cast<std::size_t>(y) * w + x;
                  if (y == yTop || y == yBot)
                  {
                    dst[idx] = lash;
                    continue;
                  }
                  const Rgb& c = src[idx];
                  if (c.transparent)
                    continue;
                  dst[idx] = Rgb{static_cast<std::uint8_t>(c.r * fade),
                                 static_cast<std::uint8_t>(c.g * fade),
                                 static_cast<std::uint8_t>(c.b * fade),
                                 false};
                }
              }
              // Teardrop forms at the lower lid and falls straight down.
              if (t > 0.34F)
              {
                const float tp = (t - 0.34F) / 0.66F;
                const float startY = cy + eyeH * 0.6F;
                const float tyc = startY + tp * (h - startY + h * 0.12F);
                const float rad = std::max(1.5F, h * 0.035F);
                for (int y = static_cast<int>(startY); y < static_cast<int>(tyc) && y < h; ++y)
                  if (y >= 0)
                    dst[static_cast<std::size_t>(y) * w + static_cast<int>(std::lround(tx))] =
                        tearDim;
                plotDot(dst, w, h, tx, tyc, rad, ya, tear);
                plotDot(dst, w, h, tx, tyc - rad / ya, rad * 0.55F, ya, tear);
                plotDot(dst, w, h, tx, tyc - 1.8F * rad / ya, rad * 0.25F, ya, tear);
              }
            });
}

void effectWordReveal(const Renderer& renderer,
                      const std::vector<Rgb>& src,
                      int w,
                      int h,
                      std::mt19937& rng,
                      const std::string& message)
{
  // When no message was supplied (the default), draw a closing line at random
  // from this anthology: mostly public-domain (Shakespeare and the like) with
  // a couple of famous short film lines. All A-Z + spaces — the bitmap font
  // carries no punctuation, so the lines are stored stripped. The draw uses
  // `rng`, so the repeat key (F10) replays the same line.
  const std::string chosen = message.empty() ? kExitWordlines[rng() % kExitWordlineCount] : message;

  // Tokenise. An optional " - <attribution>" becomes a "-" token plus the name
  // words, forced onto their own line so the credit reads as a credit.
  std::vector<std::string> tokens;
  int attribStart = -1;
  {
    std::string quote = chosen;
    std::string attrib;
    if (const auto sep = chosen.find(" - "); sep != std::string::npos)
    {
      quote = chosen.substr(0, sep);
      attrib = chosen.substr(sep + 3);
    }
    auto split = [&](const std::string& s)
    {
      std::string cur;
      for (char c : s)
      {
        if (c == ' ')
        {
          if (!cur.empty())
          {
            tokens.push_back(cur);
            cur.clear();
          }
        }
        else
          cur.push_back(c);
      }
      if (!cur.empty())
        tokens.push_back(cur);
    };
    split(quote);
    if (!attrib.empty())
    {
      attribStart = static_cast<int>(tokens.size());
      tokens.emplace_back("-");
      split(attrib);
    }
    if (tokens.empty())
      tokens.emplace_back("THE");
  }
  const int nTokens = static_cast<int>(tokens.size());

  constexpr int kFW = 5;
  constexpr int kFH = 7;
  constexpr int kCharGap = 1;  // cells between characters of a word
  constexpr int kWordGap = 4;  // cells between words
  constexpr int kLineGap = 3;  // cells between lines
  auto wordCells = [&](const std::string& s)
  { return static_cast<int>(s.size()) * kFW + (static_cast<int>(s.size()) - 1) * kCharGap; };

  // Greedy word-wrap at a given scale; forces a break before the attribution.
  auto wrap = [&](int scale)
  {
    std::vector<std::vector<int>> lines;
    std::vector<int> cur;
    int curW = 0;
    const int maxW = static_cast<int>(0.9F * w);
    for (int i = 0; i < nTokens; ++i)
    {
      const int tw = wordCells(tokens[static_cast<std::size_t>(i)]) * scale;
      const bool forced = (i == attribStart);
      const int next = cur.empty() ? tw : (curW + kWordGap * scale + tw);
      if (!cur.empty() && (forced || next > maxW))
      {
        lines.push_back(cur);
        cur.clear();
        curW = 0;
      }
      curW = cur.empty() ? tw : (curW + kWordGap * scale + tw);
      cur.push_back(i);
    }
    if (!cur.empty())
      lines.push_back(cur);
    return lines;
  };

  // Largest scale (capped) whose wrapped block fits ~0.72 of the height.
  int scale = 1;
  std::vector<std::vector<int>> lines;
  for (int s = 4; s >= 1; --s)
  {
    lines = wrap(s);
    scale = s;
    if (static_cast<int>(lines.size()) * (kFH + kLineGap) * s <= static_cast<int>(0.72F * h))
      break;
  }
  const int lineH = (kFH + kLineGap) * scale;
  const int oy0 = std::max(0, (h - static_cast<int>(lines.size()) * lineH) / 2);

  // Build letter pixels, each carrying the colour of the map directly beneath
  // it (so the words are literally drained out of the map), tagged with the
  // token index that drives the staggered reveal.
  struct LP
  {
    int x, y, word;
    Rgb col;
  };
  std::vector<LP> letters;
  for (int li = 0; li < static_cast<int>(lines.size()); ++li)
  {
    int lw = 0;
    for (std::size_t k = 0; k < lines[static_cast<std::size_t>(li)].size(); ++k)
    {
      lw += wordCells(tokens[static_cast<std::size_t>(lines[static_cast<std::size_t>(li)][k])]) *
            scale;
      if (k != 0)
        lw += kWordGap * scale;
    }
    int penX = std::max(0, (w - lw) / 2);
    const int y0 = oy0 + li * lineH;
    for (const int tok : lines[static_cast<std::size_t>(li)])
    {
      const std::string& word = tokens[static_cast<std::size_t>(tok)];
      for (int ci = 0; ci < static_cast<int>(word.size()); ++ci)
      {
        const auto g =
            glyph5x7(static_cast<char>(std::toupper(static_cast<unsigned char>(word[ci]))));
        for (int fy = 0; fy < kFH; ++fy)
          for (int fx = 0; fx < kFW; ++fx)
          {
            if (g[fy][fx] != '1')
              continue;
            for (int sy = 0; sy < scale; ++sy)
              for (int sx = 0; sx < scale; ++sx)
              {
                const int x = penX + (ci * (kFW + kCharGap) + fx) * scale + sx;
                const int y = y0 + fy * scale + sy;
                if (x < 0 || x >= w || y < 0 || y >= h)
                  continue;
                Rgb col = src[static_cast<std::size_t>(y) * w + x];
                if (col.transparent)
                  col = Rgb{210, 210, 225, false};
                letters.push_back(LP{x, y, tok, col});
              }
          }
      }
      penX += (wordCells(word) + kWordGap) * scale;
    }
  }

  // Per-column drain speeds for the map emptying downward.
  std::vector<float> dropSpeed(static_cast<std::size_t>(w));
  {
    std::uniform_real_distribution<float> d(0.8F, 1.5F);
    for (int x = 0; x < w; ++x)
      dropSpeed[static_cast<std::size_t>(x)] = d(rng);
  }

  auto dim = [](const Rgb& c, float a)
  {
    return Rgb{static_cast<std::uint8_t>(c.r * a),
               static_cast<std::uint8_t>(c.g * a),
               static_cast<std::uint8_t>(c.b * a),
               false};
  };

  // Timeline: the map drains over [0, drainEnd]; words form staggered (each
  // starting before the previous finishes); the full phrase holds, then fades.
  constexpr float drainEnd = 0.55F;
  constexpr float formDur = 0.20F;
  constexpr float lastStart = 0.45F;  // the last token begins forming by here
  constexpr float holdEnd = 0.90F;
  const float stagger = (nTokens > 1) ? (lastStart / static_cast<float>(nTokens - 1)) : 0.0F;
  const int durationMs = std::clamp(1500 + nTokens * 350, 2500, 7000);

  runFrames(renderer,
            w,
            h,
            durationMs,
            [&](float t, std::vector<Rgb>& dst)
            {
              // Draining map: each column slides down (varied speed) and fades.
              if (t < drainEnd)
              {
                const float dt = t / drainEnd;
                const float fade = 1.0F - dt;
                for (int x = 0; x < w; ++x)
                {
                  const int off =
                      static_cast<int>(std::lround(dt * dropSpeed[static_cast<std::size_t>(x)] *
                                                   (static_cast<float>(h) * 1.15F)));
                  for (int y = 0; y < h; ++y)
                  {
                    const Rgb& c = src[static_cast<std::size_t>(y) * w + x];
                    if (c.transparent)
                      continue;
                    const int ny = y + off;
                    if (ny >= h)
                      continue;
                    dst[static_cast<std::size_t>(ny) * w + x] = dim(c, fade);
                  }
                }
              }
              // Letters condensing out of the map, staggered, then the held
              // full phrase, then a final fade to black.
              for (const auto& p : letters)
              {
                const float ts = 0.05F + static_cast<float>(p.word) * stagger;
                float a = (t < ts) ? 0.0F : std::min(1.0F, (t - ts) / formDur);
                if (t > holdEnd)
                  a *= std::max(0.0F, 1.0F - (t - holdEnd) / (1.0F - holdEnd));
                if (a <= 0.0F)
                  continue;
                dst[static_cast<std::size_t>(p.y) * w + p.x] = dim(p.col, a);
              }
            });
}

void effectShiver(
    const Renderer& renderer, const std::vector<Rgb>& src, int w, int h, std::mt19937& rng)
{
  std::uniform_real_distribution<float> dJit(-1.0F, 1.0F);
  runFrames(renderer,
            w,
            h,
            2000,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float fade = std::max(0.0F, 1.0F - t);
              const float amp = 1.0F + t * static_cast<float>(std::max(w, h)) * 0.06F;
              const int ox = static_cast<int>(std::lround(dJit(rng) * amp));
              const int oy = static_cast<int>(std::lround(dJit(rng) * amp * 0.6F));
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const int sx = x - ox;
                  const int sy = y - oy;
                  if (sx < 0 || sx >= w || sy < 0 || sy >= h)
                    continue;
                  const Rgb& c = src[static_cast<std::size_t>(sy) * w + sx];
                  if (c.transparent)
                    continue;
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{static_cast<std::uint8_t>(c.r * fade),
                          static_cast<std::uint8_t>(c.g * fade),
                          static_cast<std::uint8_t>(c.b * fade),
                          false};
                }
            });
}

void effectTunnel(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float cx = (w - 1) * 0.5F;
  const float cy = (h - 1) * 0.5F;
  const float maxR = std::sqrt(cx * cx + (cy * ya) * (cy * ya));  // corner distance
  constexpr float kTwoPi = 6.2831853F;
  runFrames(renderer,
            w,
            h,
            2400,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float form = std::min(1.0F, t / 0.18F);  // flat view -> tunnel
              // The far end lights up (a small white disc), then the fly-in swells it.
              const float lightOn = std::clamp((t - 0.24F) / 0.12F, 0.0F, 1.0F) * 0.06F * maxR;
              const float fly = std::max(0.0F, (t - 0.40F) / 0.60F);  // 0 -> 1, accelerating in
              const float zoff = fly * fly * 5.5F;
              const float light = std::max(lightOn, fly * fly * maxR * 1.5F);
              const float glow = std::max(2.0F, maxR * 0.09F);  // soft halo around the light
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const std::size_t idx = static_cast<std::size_t>(y) * w + x;
                  const float dx = x - cx;
                  const float dy = (y - cy) * ya;
                  const float r = std::sqrt(dx * dx + dy * dy);
                  if (r <= light)
                  {
                    dst[idx] = Rgb{255, 255, 255, false};
                    continue;
                  }
                  // Bore wall: the view wrapped around by angle and tiled along depth;
                  // 1/r bunches the rings toward the end for the perspective look, and
                  // a gentle twist with zoff adds motion as we rush in.
                  const float a = std::atan2(dy, dx);
                  const float u = a / kTwoPi + 0.5F + zoff * 0.04F;
                  const float vv = 0.32F / std::max(r / maxR, 0.004F) + zoff;
                  const float sx = (u - std::floor(u)) * (w - 1);
                  const float sy = (vv - std::floor(vv)) * (h - 1);
                  Rgb c = sample(src, w, h, sx, sy);
                  if (c.transparent)
                    c = Rgb{18, 18, 22, false};
                  // Cross-fade the flat view into the tunnel mapping as it forms.
                  const Rgb& fc = src[idx];
                  const Rgb fb = fc.transparent ? Rgb{0, 0, 0, false} : fc;
                  c = Rgb{static_cast<std::uint8_t>(fb.r + (c.r - fb.r) * form),
                          static_cast<std::uint8_t>(fb.g + (c.g - fb.g) * form),
                          static_cast<std::uint8_t>(fb.b + (c.b - fb.b) * form),
                          false};
                  // Depth shade: black at the far end, bright at the mouth (ramped in
                  // with formation so the very first frame is the untouched view).
                  float shade = std::min(1.0F, r / maxR * 1.5F);
                  shade = 1.0F - form * (1.0F - shade);
                  Rgb px{static_cast<std::uint8_t>(c.r * shade),
                         static_cast<std::uint8_t>(c.g * shade),
                         static_cast<std::uint8_t>(c.b * shade),
                         false};
                  // Bloom: blend toward white just outside the light disc.
                  if (r < light + glow)
                  {
                    const float g = std::min(1.0F, (light + glow - r) / glow);
                    px = Rgb{static_cast<std::uint8_t>(px.r + (255 - px.r) * g),
                             static_cast<std::uint8_t>(px.g + (255 - px.g) * g),
                             static_cast<std::uint8_t>(px.b + (255 - px.b) * g),
                             false};
                  }
                  dst[idx] = px;
                }
            });
}

void effectGunshot(
    const Renderer& renderer, const std::vector<Rgb>& src, int w, int h, std::mt19937& rng)
{
  const float ya = yAspectFor(renderer);
  const float r = std::max(2.0F, h * 0.06F);  // bore radius
  std::uniform_real_distribution<float> jx(0.30F, 0.70F);
  std::uniform_real_distribution<float> jy(0.28F, 0.62F);
  struct Hole
  {
    float x;
    float y;
    float t;  // when it appears
  };
  const float popT[3] = {0.03F, 0.08F, 0.13F};
  std::array<Hole, 3> holes{};
  for (int i = 0; i < 3; ++i)
    holes[i] = {jx(rng) * w, jy(rng) * h, popT[i]};

  // Pre-build the fully-holed view for the pause / blink / fall phases.
  std::vector<Rgb> holed = src;
  for (const auto& hl : holes)
    drawBulletHole(holed, w, h, hl.x, hl.y, r, ya);

  const Rgb lid{18, 9, 8, false};  // closed eyelid

  runFrames(renderer,
            w,
            h,
            3200,
            [&](float t, std::vector<Rgb>& dst)
            {
              if (t < 0.16F)
              {
                // Shots: holes punch in one after another, each with a quick spark.
                std::copy(src.begin(), src.end(), dst.begin());
                for (const auto& hl : holes)
                {
                  if (t < hl.t)
                    continue;
                  if (t - hl.t < 0.025F)  // bright spark the hole then punches through
                    plotDot(dst, w, h, hl.x, hl.y, r * 3.0F, ya, Rgb{255, 238, 178, false});
                  drawBulletHole(dst, w, h, hl.x, hl.y, r, ya);
                }
                return;
              }
              if (t < 0.30F)  // brief pause: the holed view, held
              {
                std::copy(holed.begin(), holed.end(), dst.begin());
                return;
              }
              if (t < 0.42F)
              {
                // A single blink: the eyelids close from top and bottom, then reopen.
                std::copy(holed.begin(), holed.end(), dst.begin());
                const float local = (t - 0.30F) / 0.12F;                    // 0..1
                const float close = 1.0F - std::fabs(2.0F * local - 1.0F);  // 0->1->0
                const int lidH = static_cast<int>(close * (h * 0.5F));
                for (int y = 0; y < lidH; ++y)
                  for (int x = 0; x < w; ++x)
                  {
                    dst[static_cast<std::size_t>(y) * w + x] = lid;
                    dst[static_cast<std::size_t>(h - 1 - y) * w + x] = lid;
                  }
                return;
              }
              if (t < 0.50F)  // a beat after the blink, before keeling over
              {
                std::copy(holed.begin(), holed.end(), dst.begin());
                return;
              }
              // Collapse: the holed view topples to the left (top tilts left) about a
              // low-left pivot and drops away, dimming as vision fades to black.
              const float fp = (t - 0.50F) / 0.50F;
              const float e = fp * fp;  // accelerating fall
              const float ang = e * 1.45F;
              const float c = std::cos(ang);
              const float s = std::sin(ang);
              const float pivx = w * 0.30F;
              const float pivy = h * 0.82F;
              const float dropX = -e * w * 0.18F;
              const float dropY = e * h * 0.85F;
              const float dim = 1.0F - 0.75F * fp;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const float dxp = x - pivx - dropX;
                  const float dyp = (y - pivy - dropY) * ya;
                  const float relX = c * dxp - s * dyp;  // inverse rotation
                  const float relY = s * dxp + c * dyp;
                  Rgb px = sample(holed, w, h, pivx + relX, pivy + relY / ya);
                  if (!px.transparent)
                    px = Rgb{static_cast<std::uint8_t>(px.r * dim),
                             static_cast<std::uint8_t>(px.g * dim),
                             static_cast<std::uint8_t>(px.b * dim),
                             false};
                  dst[static_cast<std::size_t>(y) * w + x] = px;
                }
            });
}

void effectGotcha(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  static const std::vector<std::string> kLines = {
      "NOT THE EFFECT", "YOU WERE", "HOPING FOR,", "WAS IT?"};
  const Rgb white{235, 235, 235, false};
  runFrames(renderer,
            w,
            h,
            4200,
            [&](float t, std::vector<Rgb>& dst)
            {
              if (t < 0.5F)  // nothing happens: the frozen view
              {
                std::copy(src.begin(), src.end(), dst.begin());
                return;
              }
              std::fill(dst.begin(), dst.end(), Rgb{0, 0, 0, false});  // ...then, suddenly:
              drawLines(dst, w, h, kLines, white, 0.82F, 0.6F);
            });
}

void effectTestCard(
    const Renderer& renderer, const std::vector<Rgb>& src, int w, int h, std::mt19937& rng)
{
  static const std::array<Rgb, 7> kTop = {Rgb{191, 191, 191, false},
                                          Rgb{191, 191, 0, false},
                                          Rgb{0, 191, 191, false},
                                          Rgb{0, 191, 0, false},
                                          Rgb{191, 0, 191, false},
                                          Rgb{191, 0, 0, false},
                                          Rgb{0, 0, 191, false}};
  static const std::array<Rgb, 7> kMid = {Rgb{0, 0, 191, false},
                                          Rgb{19, 19, 19, false},
                                          Rgb{191, 0, 191, false},
                                          Rgb{19, 19, 19, false},
                                          Rgb{0, 191, 191, false},
                                          Rgb{19, 19, 19, false},
                                          Rgb{191, 191, 191, false}};
  const Rgb darkBlue{0, 33, 76, false};
  const Rgb white{255, 255, 255, false};
  const Rgb purple{50, 0, 106, false};
  const Rgb black{19, 19, 19, false};
  const Rgb superBlack{7, 7, 7, false};
  const Rgb lightBlack{31, 31, 31, false};

  const int topEnd = static_cast<int>(h * 0.66F);
  const int midEnd = static_cast<int>(h * 0.74F);
  std::uniform_int_distribution<int> noise(0, 255);

  // Colour of test-card row `r` at column `x`.
  auto cardPixel = [&](int r, int x) -> Rgb
  {
    const int bar = std::min(6, x * 7 / w);
    if (r < topEnd)
      return kTop[static_cast<std::size_t>(bar)];
    if (r < midEnd)
      return kMid[static_cast<std::size_t>(bar)];
    const float fx = static_cast<float>(x) / w;  // bottom strip
    if (fx < 1.0F / 6.0F)
      return darkBlue;
    if (fx < 2.0F / 6.0F)
      return white;
    if (fx < 3.0F / 6.0F)
      return purple;
    if (fx >= 0.72F && fx < 0.76F)
      return superBlack;  // PLUGE
    if (fx >= 0.76F && fx < 0.80F)
      return black;
    if (fx >= 0.80F && fx < 0.84F)
      return lightBlack;
    return black;
  };

  runFrames(renderer,
            w,
            h,
            3200,
            [&](float t, std::vector<Rgb>& dst)
            {
              if (t < 0.16F)
              {
                // Static snow: an untuned signal.
                for (int y = 0; y < h; ++y)
                  for (int x = 0; x < w; ++x)
                  {
                    const auto v = static_cast<std::uint8_t>(noise(rng));
                    dst[static_cast<std::size_t>(y) * w + x] = Rgb{v, v, v, false};
                  }
                return;
              }
              // The card locks in: a decaying vertical-hold wobble plus scanlines.
              const float q = (t - 0.16F) / 0.84F;
              const float inst = std::max(0.0F, 1.0F - q / 0.22F);  // instability -> 0
              const int roll = static_cast<int>(inst * h * 0.5F * std::sin(q * 26.0F));
              for (int y = 0; y < h; ++y)
              {
                const int r = ((y + roll) % h + h) % h;
                const bool scan = (y & 1) == 0;  // faint dark scanline on alternate rows
                for (int x = 0; x < w; ++x)
                {
                  Rgb c = cardPixel(r, x);
                  if (scan)
                    c = Rgb{static_cast<std::uint8_t>(c.r * 0.86F),
                            static_cast<std::uint8_t>(c.g * 0.86F),
                            static_cast<std::uint8_t>(c.b * 0.86F),
                            false};
                  dst[static_cast<std::size_t>(y) * w + x] = c;
                }
              }
            });
}

void effectUtopia(const Renderer& renderer,
                  const std::vector<Rgb>& src,
                  int w,
                  int h,
                  std::mt19937& rng,
                  const std::vector<Rgb>* linesFrame,
                  const std::vector<char>* swedenMask)
{
  const std::size_t n = static_cast<std::size_t>(w) * h;
  long cnt = 0;
  double sumX = 0;
  double sumY = 0;
  const bool haveGeo = linesFrame != nullptr && swedenMask != nullptr && linesFrame->size() == n &&
                       swedenMask->size() == n;
  if (haveGeo)
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
        if ((*swedenMask)[static_cast<std::size_t>(y) * w + x] != 0)
        {
          ++cnt;
          sumX += x;
          sumY += y;
        }
  if (cnt < 25)  // nothing recognisable to erase -> ordinary fade
  {
    effectFade(renderer, src, w, h);
    return;
  }
  const float cxm = static_cast<float>(sumX / cnt);
  const float cym = static_cast<float>(sumY / cnt);
  float maxR = 1.0F;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      if ((*swedenMask)[static_cast<std::size_t>(y) * w + x] != 0)
      {
        const float dx = x - cxm;
        const float dy = y - cym;
        maxR = std::max(maxR, std::sqrt(dx * dx + dy * dy));
      }
  std::uniform_real_distribution<float> u01(0.0F, 1.0F);
  std::vector<float> jit(n);  // per-pixel jitter for a ragged "eaten" frontier
  for (auto& j : jit)
    j = u01(rng);

  const std::vector<Rgb>& lines = *linesFrame;
  const std::vector<char>& mask = *swedenMask;
  const float frontier = maxR * 0.10F + 1.0F;
  runFrames(renderer,
            w,
            h,
            4000,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float reveal = std::clamp(t / 0.32F, 0.0F, 1.0F);       // data -> lines
              const float q = std::clamp((t - 0.32F) / 0.58F, 0.0F, 1.0F);  // Sweden erosion
              const float eraseR = q * (maxR + frontier);
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const std::size_t i = static_cast<std::size_t>(y) * w + x;
                  const Rgb& d0 = src[i];
                  const Rgb& l0 = lines[i];
                  // The data dims out while the line map fades in (they barely
                  // overlap, so a simple additive blend over black reads cleanly).
                  float r = (d0.transparent ? 0.0F : d0.r * (1.0F - reveal)) + l0.r * reveal;
                  float g = (d0.transparent ? 0.0F : d0.g * (1.0F - reveal)) + l0.g * reveal;
                  float b = (d0.transparent ? 0.0F : d0.b * (1.0F - reveal)) + l0.b * reveal;
                  if (q > 0.0F && mask[i] != 0)
                  {
                    const float dx = x - cxm;
                    const float dy = y - cym;
                    const float rad = std::sqrt(dx * dx + dy * dy);
                    if (rad < eraseR + (jit[i] - 0.5F) * frontier)  // eaten away -> void
                    {
                      r = 0.0F;
                      g = 0.0F;
                      b = 0.0F;
                    }
                  }
                  dst[i] = Rgb{static_cast<std::uint8_t>(std::clamp(r, 0.0F, 255.0F)),
                               static_cast<std::uint8_t>(std::clamp(g, 0.0F, 255.0F)),
                               static_cast<std::uint8_t>(std::clamp(b, 0.0F, 255.0F)),
                               false};
                }
            });
}

void effectBall(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float hx = h * ya;  // screen height in x-units
  const float R = std::max(4.0F, 0.13F * std::min(static_cast<float>(w), hx));  // ball x-radius
  const float Ry = R / ya;  // ball y-radius (rows), for round look

  // Texture + sphere shading of the ball: an ellipse (rxE x ryE) at (cx,cy)
  // sampled from the view, shaded as a lit sphere; `shade` ramps the 3D look
  // in during the collapse (0 = flat image, 1 = full sphere).
  auto drawBall = [&](std::vector<Rgb>& dst, float cx, float cy, float rxE, float ryE, float shade)
  {
    const int x0 = std::max(0, static_cast<int>(std::floor(cx - rxE)));
    const int x1 = std::min(w - 1, static_cast<int>(std::ceil(cx + rxE)));
    const int y0 = std::max(0, static_cast<int>(std::floor(cy - ryE)));
    const int y1 = std::min(h - 1, static_cast<int>(std::ceil(cy + ryE)));
    constexpr float lxd = -0.40F;
    constexpr float lyd = -0.55F;
    constexpr float lzd = 0.73F;  // light from upper-left, toward viewer
    auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x)
      {
        const float nx = (x - cx) / rxE;
        const float ny = (y - cy) / ryE;
        const float r2 = nx * nx + ny * ny;
        if (r2 > 1.0F)
          continue;
        const float z = std::sqrt(1.0F - r2);
        const Rgb tex =
            sample(src, w, h, (nx * 0.5F + 0.5F) * (w - 1), (ny * 0.5F + 0.5F) * (h - 1));
        const float tr = tex.transparent ? 70.0F : tex.r;
        const float tg = tex.transparent ? 72.0F : tex.g;
        const float tb = tex.transparent ? 82.0F : tex.b;
        const float ndotl = std::max(0.0F, nx * lxd + ny * lyd + z * lzd);
        const float lit = (1.0F - shade) + shade * (0.32F + 0.68F * ndotl);
        const float spec = shade * std::pow(ndotl, 22.0F) * 200.0F;
        dst[static_cast<std::size_t>(y) * w + x] =
            Rgb{u8(tr * lit + spec), u8(tg * lit + spec), u8(tb * lit + spec), false};
      }
  };

  // Physics state (sub-pixel units; +y is down).
  const float ccx = w * 0.5F;
  const float ccy = h * 0.32F;  // collapse / launch point, up in the frame
  float bx = ccx;
  float by = ccy;
  float vx = 0.0F;
  float vy = 0.0F;
  float cV = 0.0F;  // vertical squash spring (floor impacts): +ve = flatter
  float cVv = 0.0F;
  float cH = 0.0F;  // horizontal squash spring (wall impacts)
  float cHv = 0.0F;
  bool launched = false;
  const float dt = kFrameMs / 1000.0F;
  const float grav = 1.9F * h;  // gravity, rows/s^2
  const float eFloor = 0.78F;   // restitution
  const float eWall = 0.82F;
  const float springK = 240.0F;  // squash spring stiffness / damping
  const float springD = 12.0F;
  constexpr float kCollapse = 0.16F;

  runFrames(renderer,
            w,
            h,
            5600,
            [&](float t, std::vector<Rgb>& dst)
            {
              std::fill(dst.begin(), dst.end(), Rgb{0, 0, 0, false});
              float cx = ccx;
              float cy = ccy;
              float rxE = R;
              float ryE = Ry;
              float shade = 1.0F;
              if (t < kCollapse)
              {
                // Collapse: a big circle of the view shrinks to the ball, gaining its
                // 3D shading. (No physics yet.)
                const float p = t / kCollapse;
                const float rx = (w * 0.5F) + (R - w * 0.5F) * p;
                rxE = rx;
                ryE = rx / ya;
                shade = p;
              }
              else
              {
                if (!launched)
                {
                  launched = true;
                  vx = 0.42F * w;  // drift right
                  vy = 0.0F;       // released from rest -> falls
                }
                // Integrate.
                vy += grav * dt;
                vx *= 0.998F;  // gentle air drag
                vy *= 0.999F;
                bx += vx * dt;
                by += vy * dt;
                // Floor / ceiling.
                if (by + Ry > h)
                {
                  by = h - Ry;
                  cV = std::min(0.5F, std::fabs(vy) / (1.6F * h));  // squash on impact
                  cVv = 0.0F;
                  vy = -vy * eFloor;
                  vx *= 0.96F;  // rolling friction at the floor
                }
                else if (by - Ry < 0)
                {
                  by = Ry;
                  vy = -vy * eFloor;
                }
                // Side walls.
                if (bx - R < 0)
                {
                  bx = R;
                  cH = std::min(0.5F, std::fabs(vx) / (1.6F * w));
                  cHv = 0.0F;
                  vx = -vx * eWall;
                }
                else if (bx + R > w)
                {
                  bx = w - R;
                  cH = std::min(0.5F, std::fabs(vx) / (1.6F * w));
                  cHv = 0.0F;
                  vx = -vx * eWall;
                }
                // Squash springs relax back to round (damped).
                cVv += (-springK * cV - springD * cVv) * dt;
                cV += cVv * dt;
                cHv += (-springK * cH - springD * cHv) * dt;
                cH += cHv * dt;
                cx = bx;
                cy = by;
                // Soft deformation conserves bulk: flatter -> wider, and vice versa.
                rxE = R * (1.0F + 0.6F * cV - 0.6F * cH);
                ryE = Ry * (1.0F - 0.6F * cV + 0.6F * cH);
              }
              drawBall(dst, cx, cy, std::max(1.0F, rxE), std::max(1.0F, ryE), shade);
            });
}

void effectJellyfish(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float cx0 = (w - 1) * 0.5F;
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float s0 = 0.16F * mn;
  const float sBig = w / 2.4F;
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer,
            w,
            h,
            5200,
            [&](float t, std::vector<Rgb>& dst)
            {
              std::fill(dst.begin(), dst.end(), Rgb{0, 0, 0, false});
              const float collapse = std::clamp(t / 0.15F, 0.0F, 1.0F);
              const float pulse = 0.5F + 0.5F * std::sin(t * 7.0F);
              float bellW = 0;
              float bellH = 0;
              float cx = cx0;
              float cy = (h - 1) * 0.5F;
              float dim = 1.0F;
              if (collapse < 1.0F)
              {
                const float s = sBig + (s0 - sBig) * collapse;
                bellW = s;
                bellH = s * 0.85F;
              }
              else
              {
                const float tau = (t - 0.15F) / 0.85F;
                bellW = s0 * (1.0F - 0.22F * pulse);
                bellH = s0 * (0.78F + 0.34F * pulse);
                cx = cx0 + 0.05F * w * std::sin(t * 1.3F);
                cy = (h - 1) * 0.55F - tau * (h * 0.95F);  // drift up and out
                dim = 1.0F - 0.45F * tau;
              }
              const Rgb ctr = sample(src, w, h, cx0, (h - 1) * 0.5F);
              const Rgb tcol{u8((ctr.transparent ? 80 : ctr.r) * 0.5F),
                             u8((ctr.transparent ? 90 : ctr.g) * 0.5F),
                             u8((ctr.transparent ? 120 : ctr.b) * 0.6F),
                             false};
              const int nT = 7;
              for (int i = 0; i < nT; ++i)
              {
                const float base = (static_cast<float>(i) / (nT - 1) * 2.0F - 1.0F) * bellW * 0.7F;
                const float ph = i * 0.9F;
                const float maxd = bellH * 2.6F;
                for (float dd = 0.0F; dd < maxd; dd += 2.0F)
                {
                  const float fade = (1.0F - dd / maxd) * dim;
                  const float xx = cx + base + bellW * 0.16F * std::sin(dd * 0.05F + ph + t * 5.0F);
                  const Rgb cc{u8(tcol.r * fade), u8(tcol.g * fade), u8(tcol.b * fade), false};
                  plotDot(dst, w, h, xx, cy + dd / ya, std::max(1.0F, bellW * 0.035F), ya, cc);
                }
              }
              const int x0 = std::max(0, static_cast<int>(cx - bellW));
              const int x1 = std::min(w - 1, static_cast<int>(cx + bellW));
              const int y0 = std::max(0, static_cast<int>(cy - bellH / ya));
              const int y1 = std::min(h - 1, static_cast<int>(cy + bellH * 0.3F / ya));
              for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                {
                  const float bx = (x - cx) / bellW;
                  const float by = -(y - cy) * ya / bellH;
                  if (by < -0.05F || bx * bx + by * by > 1.0F)
                    continue;
                  const Rgb tex =
                      sample(src, w, h, (bx + 1) * 0.5F * (w - 1), (1.0F - by) * 0.5F * (h - 1));
                  const float r = tex.transparent ? 90.0F : tex.r;
                  const float g = tex.transparent ? 120.0F : tex.g;
                  const float b = tex.transparent ? 170.0F : tex.b;
                  const float sh = (0.55F + 0.45F * by) * dim;  // brighter toward the dome's top
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(r * sh), u8(g * sh), u8(b * sh), false};
                }
            });
}

void effectFish(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float s0 = 0.18F * mn;
  const float sBig = w / 2.3F;
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float p0x = (w - 1) * 0.55F;
  const float p0y = (h - 1) * 0.5F;
  const float p1x = w * 0.36F;
  const float p1y = h * 0.46F;
  const float p2x = w * 0.12F;
  const float p2y = h * 0.28F;
  const float head0 = std::atan2(p1x - p0x, -(p1y - p0y) * ya);  // initial heading

  auto drawFish = [&](std::vector<Rgb>& dst,
                      float cx,
                      float cy,
                      float scaleLong,
                      float scaleH,
                      float rot,
                      float wag,
                      float dim)
  {
    const float cr = std::cos(rot);
    const float sr = std::sin(rot);
    const float ext = std::max(scaleLong * 1.15F, scaleH * 1.2F);
    const int x0 = std::max(0, static_cast<int>(cx - ext));
    const int x1 = std::min(w - 1, static_cast<int>(cx + ext));
    const int y0 = std::max(0, static_cast<int>(cy - ext / ya));
    const int y1 = std::min(h - 1, static_cast<int>(cy + ext / ya));
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x)
      {
        const float dx = x - cx;
        const float dyp = (y - cy) * ya;
        const float by = (dx * sr - dyp * cr) / scaleLong;  // along body, +nose
        const float bv = (dx * cr + dyp * sr) / scaleH;     // vertical
        const float vc =
            0.5F * std::sin(by * 3.2F - wag) * std::clamp(0.5F - by * 0.5F, 0.0F, 1.0F);
        bool body = false;
        bool tail = false;
        if (by <= 1.0F && by >= -0.58F)
        {
          const float e = (by - 0.15F) / 0.85F;
          const float vv = 1.0F - e * e;
          const float hh = vv > 0 ? 0.95F * std::sqrt(vv) : 0.0F;
          body = std::fabs(bv - vc) <= hh;
        }
        else if (by < -0.58F && by > -1.08F)
        {
          tail = std::fabs(bv - vc) <= 0.95F * ((-0.58F - by) / 0.5F);
        }
        if (!body && !tail)
          continue;
        const bool eye = body && by > 0.6F && by < 0.78F && std::fabs(bv - vc + 0.2F) < 0.13F;
        const Rgb tex =
            sample(src, w, h, (by * 0.5F + 0.5F) * (w - 1), (bv * 0.5F + 0.5F) * (h - 1));
        float r = tex.transparent ? 150.0F : tex.r;
        float g = tex.transparent ? 170.0F : tex.g;
        float b = tex.transparent ? 200.0F : tex.b;
        float sh = (tail ? 0.6F : 1.0F) * dim;
        if (eye)
        {
          r = g = b = 18.0F;
          sh = 1.0F;
        }
        dst[static_cast<std::size_t>(y) * w + x] = Rgb{u8(r * sh), u8(g * sh), u8(b * sh), false};
      }
  };

  runFrames(renderer,
            w,
            h,
            5000,
            [&](float t, std::vector<Rgb>& dst)
            {
              std::fill(dst.begin(), dst.end(), Rgb{0, 0, 0, false});
              const float wag = t * 22.0F;
              if (t < 0.15F)
              {
                const float s = sBig + (s0 - sBig) * (t / 0.15F);
                drawFish(dst, (w - 1) * 0.5F, (h - 1) * 0.5F, s, s * 0.45F, head0, wag, 1.0F);
                return;
              }
              const float tau = (t - 0.15F) / 0.85F;
              const float om = 1.0F - tau;
              const float px = om * om * p0x + 2.0F * om * tau * p1x + tau * tau * p2x;
              const float py = om * om * p0y + 2.0F * om * tau * p1y + tau * tau * p2y;
              const float vx = 2.0F * om * (p1x - p0x) + 2.0F * tau * (p2x - p1x);
              const float vy = (2.0F * om * (p1y - p0y) + 2.0F * tau * (p2y - p1y)) * ya;
              const float rot = std::atan2(vx, -vy);
              const float s = s0 * (1.0F - 0.8F * tau);
              drawFish(dst, px, py, s, s * 0.45F, rot, wag, 1.0F - 0.6F * tau);
            });
}

void effectCrab(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float s0 = 0.15F * mn;
  const float sBig = w / 2.7F;
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(
      renderer,
      w,
      h,
      5000,
      [&](float t, std::vector<Rgb>& dst)
      {
        std::fill(dst.begin(), dst.end(), Rgb{0, 0, 0, false});
        const float collapse = std::clamp(t / 0.14F, 0.0F, 1.0F);
        const float dir = -1.0F;  // scuttle left
        float s = s0;
        float cx = (w - 1) * 0.5F;
        float cy = (h - 1) * 0.6F;
        if (collapse < 1.0F)
          s = sBig + (s0 - sBig) * collapse;
        else
        {
          const float tau = (t - 0.14F) / 0.86F;
          cx = (w - 1) * 0.5F + dir * tau * w * 0.95F;
          cy = (h - 1) * 0.62F + std::sin(t * 9.0F) * s * 0.10F / ya;
        }
        const float shW = s * 1.3F;
        const float shH = s * 0.78F;
        const Rgb ctr = sample(src, w, h, (w - 1) * 0.5F, (h - 1) * 0.55F);
        const Rgb leg{u8((ctr.transparent ? 150 : ctr.r) * 0.55F),
                      u8((ctr.transparent ? 80 : ctr.g) * 0.4F),
                      u8((ctr.transparent ? 60 : ctr.b) * 0.35F),
                      false};
        const float gait = t * 11.0F;
        for (int sgn = -1; sgn <= 1; sgn += 2)
          for (int j = 0; j < 4; ++j)
          {
            // Each leg traces a walking ellipse: the foot swings fore/aft while
            // lifting 90 deg out of phase; neighbouring legs and the two sides
            // are offset so the gait reads as stepping.
            const float phase = gait + j * 1.6F + (sgn > 0 ? 3.14159F : 0.0F);
            const float swing = std::sin(phase);
            const float lift = std::max(0.0F, std::cos(phase));
            const float basex = cx + (-0.6F + j * 0.4F) * shW * 0.7F;
            const float basey = cy + shH * 0.35F / ya;
            const float kneex = basex + sgn * shW * 0.4F + swing * shW * 0.12F;
            const float kneey = basey + shH * 0.45F / ya - lift * s * 0.12F / ya;
            const float footx = kneex + sgn * shW * 0.30F + swing * shW * 0.34F;
            const float footy = basey + shH * 1.2F / ya - lift * s * 0.55F / ya;
            drawSeg(dst, w, h, basex, basey, kneex, kneey, std::max(1.0F, s * 0.05F), ya, leg);
            drawSeg(dst, w, h, kneex, kneey, footx, footy, std::max(1.0F, s * 0.045F), ya, leg);
          }
        // Shell.
        const int x0 = std::max(0, static_cast<int>(cx - shW));
        const int x1 = std::min(w - 1, static_cast<int>(cx + shW));
        const int y0 = std::max(0, static_cast<int>(cy - shH / ya));
        const int y1 = std::min(h - 1, static_cast<int>(cy + shH / ya));
        for (int y = y0; y <= y1; ++y)
          for (int x = x0; x <= x1; ++x)
          {
            const float bx = (x - cx) / shW;
            const float by = (y - cy) * ya / shH;
            if (bx * bx + by * by > 1.0F)
              continue;
            const Rgb tex = sample(src, w, h, (bx + 1) * 0.5F * (w - 1), (by + 1) * 0.5F * (h - 1));
            const float r = tex.transparent ? 170.0F : tex.r;
            const float g = tex.transparent ? 90.0F : tex.g;
            const float b = tex.transparent ? 70.0F : tex.b;
            const float sh = 0.7F + 0.3F * std::clamp(1.0F - (bx * bx + by * by), 0.0F, 1.0F);
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(r * sh), u8(g * sh), u8(b * sh), false};
          }
        // Eyestalks.
        for (int c = -1; c <= 1; c += 2)
        {
          const float ex = cx + c * shW * 0.28F;
          const float ey = cy - shH * 0.9F / ya;
          drawSeg(dst,
                  w,
                  h,
                  cx + c * shW * 0.28F,
                  cy - shH * 0.3F / ya,
                  ex,
                  ey,
                  std::max(1.0F, s * 0.04F),
                  ya,
                  leg);
          plotDot(dst, w, h, ex, ey, std::max(1.0F, s * 0.07F), ya, Rgb{20, 20, 26, false});
        }
        // Two raised, waving claws on the leading edge.
        for (int c = -1; c <= 1; c += 2)
        {
          const float ax = cx + dir * shW * 0.5F + c * shW * 0.12F;
          const float ex = ax + dir * shW * 0.45F;
          const float ey = cy - shH * 0.7F / ya;
          const float wave = std::sin(t * 5.0F + c) * 0.3F;
          drawSeg(dst, w, h, ax, cy, ex, ey, std::max(1.0F, s * 0.09F), ya, leg);
          drawSeg(dst,
                  w,
                  h,
                  ex,
                  ey,
                  ex + dir * shW * 0.18F,
                  ey - shH * (0.25F + wave) / ya,
                  std::max(1.0F, s * 0.06F),
                  ya,
                  leg);
          drawSeg(dst,
                  w,
                  h,
                  ex,
                  ey,
                  ex + dir * shW * 0.18F,
                  ey + shH * 0.12F / ya,
                  std::max(1.0F, s * 0.06F),
                  ya,
                  leg);
        }
      });
}

void effectSpider(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float s0 = 0.12F * mn;
  const float sBig = w / 3.0F;
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(
      renderer,
      w,
      h,
      5200,
      [&](float t, std::vector<Rgb>& dst)
      {
        std::fill(dst.begin(), dst.end(), Rgb{0, 0, 0, false});
        const float collapse = std::clamp(t / 0.14F, 0.0F, 1.0F);
        float s = s0;
        float cx = (w - 1) * 0.5F;
        float cy = (h - 1) * 0.30F;
        if (collapse < 1.0F)
        {
          s = sBig + (s0 - sBig) * collapse;
          cy = (h - 1) * 0.5F + ((h - 1) * 0.18F - (h - 1) * 0.5F) * collapse;
        }
        else
        {
          const float tau = (t - 0.14F) / 0.86F;
          cx = (w - 1) * 0.5F + std::sin(t * 1.5F) * s * 0.4F;
          cy = (h - 1) * 0.18F + tau * h * 0.7F;  // drop on the thread
        }
        // Silk thread from the top down to the body.
        for (int yy = 0; yy < static_cast<int>(cy - s * 0.7F / ya); ++yy)
        {
          const int xx = static_cast<int>(cx + std::sin(yy * 0.05F) * 1.0F);
          if (xx >= 0 && xx < w)
            dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{150, 150, 165, false};
        }
        const Rgb ctr = sample(src, w, h, (w - 1) * 0.5F, (h - 1) * 0.3F);
        const Rgb leg{u8((ctr.transparent ? 60 : ctr.r) * 0.45F),
                      u8((ctr.transparent ? 60 : ctr.g) * 0.45F),
                      u8((ctr.transparent ? 70 : ctr.b) * 0.5F),
                      false};
        const float wig = t * 7.0F;
        for (int sgn = -1; sgn <= 1; sgn += 2)
          for (int j = 0; j < 4; ++j)
          {
            const float basex = cx + sgn * s * 0.5F;
            const float basey = cy + (j - 1.5F) * s * 0.22F / ya;
            const float wj = std::sin(wig + j * 1.1F + (sgn > 0 ? 2.0F : 0.0F)) * 0.2F;
            const float reach = s * 1.3F;
            const float kneex = basex + sgn * reach * 0.5F;
            const float kneey = basey - s * 0.65F / ya * (0.6F + wj);  // knee bent up
            const float footx = basex + sgn * reach * 0.95F;
            const float footy = basey + s * 0.55F / ya * (1.0F + wj);  // foot down-out
            drawSeg(dst, w, h, basex, basey, kneex, kneey, std::max(1.0F, s * 0.05F), ya, leg);
            drawSeg(dst, w, h, kneex, kneey, footx, footy, std::max(1.0F, s * 0.045F), ya, leg);
          }
        // Body: abdomen + smaller cephalothorax, both textured.
        auto blob = [&](float bcx, float bcy, float rx, float ry)
        {
          const int x0 = std::max(0, static_cast<int>(bcx - rx));
          const int x1 = std::min(w - 1, static_cast<int>(bcx + rx));
          const int y0 = std::max(0, static_cast<int>(bcy - ry / ya));
          const int y1 = std::min(h - 1, static_cast<int>(bcy + ry / ya));
          for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
            {
              const float ex = (x - bcx) / rx;
              const float ey = (y - bcy) * ya / ry;
              const float rr = ex * ex + ey * ey;
              if (rr > 1.0F)
                continue;
              const Rgb tex =
                  sample(src, w, h, (ex + 1) * 0.5F * (w - 1), (ey + 1) * 0.5F * (h - 1));
              const float r = tex.transparent ? 70.0F : tex.r;
              const float g = tex.transparent ? 70.0F : tex.g;
              const float b = tex.transparent ? 80.0F : tex.b;
              const float sh = 0.6F + 0.4F * std::clamp(1.0F - rr, 0.0F, 1.0F);
              dst[static_cast<std::size_t>(y) * w + x] =
                  Rgb{u8(r * sh), u8(g * sh), u8(b * sh), false};
            }
        };
        blob(cx, cy + s * 0.35F / ya, s * 0.85F, s * 1.0F);  // abdomen
        blob(cx, cy - s * 0.4F / ya, s * 0.55F, s * 0.5F);   // cephalothorax
      });
}

void effectSkeletonWave(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  constexpr int kN = 6;
  const float floorY = (h - 1) * 0.94F;
  const float H = h * 0.58F;
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const Limb lL{-0.01F, 0.23F, 0.02F, 0.46F};
  const Limb lR{0.01F, 0.23F, -0.02F, 0.46F};
  runFrames(
      renderer,
      w,
      h,
      5000,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float alpha = skeletonStage(dst, src, w, h, t, static_cast<int>(floorY));
        if (alpha <= 0.0F)
          return;
        const float beat = t * 6.0F * speed;
        for (int i = 0; i < kN; ++i)
        {
          const float cx = w * ((i + 0.5F) / kN);
          const float a = std::max(0.0F, std::sin(beat - i * 1.0F));
          const Limb aL = lerpLimb({0.0F, 0.16F, 0.0F, 0.32F}, {-0.20F, -0.35F, -0.45F, -0.70F}, a);
          const Limb aR = lerpLimb({0.0F, 0.16F, 0.0F, 0.32F}, {0.20F, -0.35F, 0.45F, -0.70F}, a);
          drawSkeleton(dst,
                       w,
                       h,
                       ya,
                       cx,
                       floorY,
                       H,
                       a * 0.06F * H,
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

void effectFallToPieces(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  constexpr int kN = 5;
  const float floorY = (h - 1) * 0.94F;
  const float H = h * 0.58F;
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  struct Bone
  {
    float cx;
    float cy;
    float ox;
    float oy;
    float rad;
    bool dot;
    float detach;
    float vx;
    float spin;
    float stack;
  };
  std::array<std::vector<Bone>, kN> bones;
  std::array<Rgb, kN> tint;
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  for (int i = 0; i < kN; ++i)
  {
    const float cx = w * ((i + 0.5F) / kN);
    tint[i] = boneTint(src, w, h, cx, 1.0F);
    const float hy = floorY - 0.46F * H;
    const float neckY = floorY - 0.78F * H;
    const float skullY = floorY - 0.88F * H;
    const float shY = floorY - 0.73F * H;
    const float bR = std::max(1.0F, H * 0.016F);
    const float shW = H * 0.13F;
    const float rad = H * 0.085F;
    auto& v = bones[i];
    int bi = 0;
    auto seg = [&](float ax, float ay, float bx, float by, float r)
    {
      Bone b;
      b.cx = (ax + bx) * 0.5F;
      b.cy = (ay + by) * 0.5F;
      b.ox = (bx - ax) * 0.5F;
      b.oy = (by - ay) * 0.5F;
      b.rad = r;
      b.dot = false;
      b.detach = hash(i * 53 + bi) * 0.12F;
      b.vx = (hash(i * 31 + bi * 7) - 0.5F) * w * 0.05F;
      b.spin = (hash(i * 17 + bi * 5) - 0.5F) * 7.0F;
      b.stack = (bi % 4) * r * 1.4F;
      ++bi;
      v.push_back(b);
    };
    seg(cx, hy, cx, neckY, bR);  // spine
    for (int r = 0; r < 3; ++r)
    {
      const float ry = neckY + (hy - neckY) * (0.28F + 0.22F * r);
      const float rw = H * 0.11F * (1.0F - 0.13F * r);
      seg(cx - rw, ry, cx + rw, ry, bR * 0.7F);
    }
    seg(cx - shW, shY, cx + shW, shY, bR);            // shoulders
    seg(cx - H * 0.07F, hy, cx + H * 0.07F, hy, bR);  // pelvis
    for (int s = -1; s <= 1; s += 2)                  // arms (hanging)
    {
      const float sx = cx + s * shW;
      seg(sx, shY, sx, shY + 0.16F * H, bR * 0.9F);
      seg(sx, shY + 0.16F * H, sx, shY + 0.32F * H, bR * 0.85F);
    }
    for (int s = -1; s <= 1; s += 2)  // legs
    {
      const float hx = cx + s * H * 0.06F;
      seg(hx, hy, hx, hy + 0.23F * H, bR);
      seg(hx, hy + 0.23F * H, hx, hy + 0.46F * H, bR * 0.9F);
    }
    Bone sk;  // skull
    sk.cx = cx;
    sk.cy = skullY;
    sk.ox = sk.oy = 0.0F;
    sk.rad = rad;
    sk.dot = true;
    sk.detach = hash(i * 53 + bi) * 0.12F;
    sk.vx = (hash(i * 31 + bi * 7) - 0.5F) * w * 0.05F;
    sk.spin = 0.0F;
    sk.stack = rad * 1.5F;
    v.push_back(sk);
  }
  const float g = 5.4F * h;  // 1.5x faster collapse than a natural fall
  constexpr float kFall = 0.15F;
  runFrames(
      renderer,
      w,
      h,
      4600,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float alpha = skeletonStage(dst, src, w, h, t, static_cast<int>(floorY));
        if (alpha <= 0.0F)
          return;
        for (int i = 0; i < kN; ++i)
        {
          const Rgb bone{
              u8(tint[i].r * alpha), u8(tint[i].g * alpha), u8(tint[i].b * alpha), false};
          const Rgb eye{u8(18 * alpha), u8(12 * alpha), u8(12 * alpha), false};
          for (const auto& b : bones[i])
          {
            const float pieceT = std::max(0.0F, (t - kFall) - b.detach);
            if (pieceT <= 0.0F)  // still standing
            {
              if (b.dot)
              {
                plotDot(dst, w, h, b.cx, b.cy, b.rad, ya, bone);
                plotDot(dst,
                        w,
                        h,
                        b.cx - b.rad * 0.42F,
                        b.cy - b.rad * 0.05F,
                        std::max(1.0F, b.rad * 0.26F),
                        ya,
                        eye);
                plotDot(dst,
                        w,
                        h,
                        b.cx + b.rad * 0.42F,
                        b.cy - b.rad * 0.05F,
                        std::max(1.0F, b.rad * 0.26F),
                        ya,
                        eye);
              }
              else
                drawSeg(
                    dst, w, h, b.cx - b.ox, b.cy - b.oy, b.cx + b.ox, b.cy + b.oy, b.rad, ya, bone);
              continue;
            }
            const float maxCY = floorY - b.stack;
            const float tl = std::sqrt(std::max(0.0F, 2.0F * (maxCY - b.cy) / g));
            const float te = std::min(pieceT, tl);
            const float cyN = std::min(maxCY, b.cy + 0.5F * g * pieceT * pieceT);
            const float cxN = b.cx + b.vx * te;
            const float delta = b.spin * te;
            const float ca = std::cos(delta);
            const float sa = std::sin(delta);
            const float oxp = b.ox * ca - (b.oy * ya) * sa;
            const float oyp = (b.ox * sa + (b.oy * ya) * ca) / ya;
            if (b.dot)
              plotDot(dst, w, h, cxN, cyN, b.rad, ya, bone);
            else
              drawSeg(dst, w, h, cxN - oxp, cyN - oyp, cxN + oxp, cyN + oyp, b.rad, ya, bone);
          }
        }
      });
}

void effectCountdown(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  static const char* const kDig[5][7] = {
      {"00100", "01100", "00100", "00100", "00100", "00100", "01110"},   // 1
      {"01110", "10001", "00001", "00110", "01000", "10000", "11111"},   // 2
      {"11110", "00001", "00001", "01110", "00001", "00001", "11110"},   // 3
      {"00010", "00110", "01010", "10010", "11111", "00010", "00010"},   // 4
      {"11111", "10000", "11110", "00001", "00001", "10001", "01110"}};  // 5
  const float cx = w * 0.5F, cy = h * 0.5F;
  const float R = 0.44F * std::min(static_cast<float>(w), h * ya);
  runFrames(
      renderer,
      w,
      h,
      5200,
      [&](float t, std::vector<Rgb>& dst)
      {
        const int seg = std::min(4, static_cast<int>(t / 0.20F));
        const int number = 5 - seg;
        const float local = (t - seg * 0.20F) / 0.20F;
        const float sweep = local * 6.2832F;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 0.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            const float grain = (hash(x * 7 + y * 131 + static_cast<int>(t * 600)) - 0.5F) * 24.0F;
            float base = 150 + l * 0.10F + grain;
            const float dx = x - cx, dy = (y - cy) * ya;
            float ang = std::atan2(dx, -dy);  // 0 at top, clockwise
            if (ang < 0)
              ang += 6.2832F;
            if (ang <= sweep)
              base *= 0.55F;  // the swept wedge darkens
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(base), u8(base * 1.02F), u8(base * 0.9F), false};
          }
        const Rgb ink{18, 20, 16, false};
        for (int k = 0; k < 200; ++k)  // ring
        {
          const float a = k / 200.0F * 6.2832F;
          plotDot(dst,
                  w,
                  h,
                  cx + std::cos(a) * R,
                  cy + std::sin(a) * R,
                  std::max(1.0F, R * 0.012F),
                  ya,
                  ink);
        }
        drawSeg(dst, w, h, 0, cy, w - 1, cy, std::max(1.0F, R * 0.008F), ya, ink);  // crosshairs
        drawSeg(dst, w, h, cx, 0, cx, h - 1, std::max(1.0F, R * 0.008F), ya, ink);
        drawSeg(dst,
                w,
                h,
                cx,
                cy,
                cx + std::sin(sweep) * R,
                cy - std::cos(sweep) * R,
                std::max(1.0F, R * 0.02F),
                ya,
                ink);                // sweep hand
        const float ds = R * 0.16F;  // big number
        const char* const* g = kDig[number - 1];
        for (int fy = 0; fy < 7; ++fy)
          for (int fx = 0; fx < 5; ++fx)
            if (g[fy][fx] == '1')
              plotDot(dst, w, h, cx + (fx - 2) * ds, cy + (fy - 3) * ds, ds * 0.55F, ya, ink);
        if (t > 0.985F)  // black
          for (std::size_t k = 0; k < dst.size(); ++k)
            dst[k] = Rgb{0, 0, 0, false};
      });
}

void effectIrisOut(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float cx = w * 0.5F, cy = h * 0.5F;
  const float maxR = std::hypot(std::max(cx, w - cx), std::max(cy, h - cy) * ya) + 2.0F;
  runFrames(renderer,
            w,
            h,
            3400,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float irisR = (1.0F - std::pow(t, 1.4F)) * maxR;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const float d = std::hypot(x - cx, (y - cy) * ya);
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  if (d > irisR || s.transparent)
                    dst[static_cast<std::size_t>(y) * w + x] = Rgb{0, 0, 0, false};
                  else
                  {
                    const float e = std::clamp((irisR - d) / (0.07F * maxR), 0.0F, 1.0F);
                    dst[static_cast<std::size_t>(y) * w + x] =
                        Rgb{u8(s.r * e), u8(s.g * e), u8(s.b * e), false};
                  }
                }
            });
}

void effectWarp(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  const float cx = w * 0.5F, cy = h * 0.5F, mn = std::min(static_cast<float>(w), h * ya);
  constexpr int kStars = 150;
  runFrames(
      renderer,
      w,
      h,
      4400,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float dim = (1.0F - std::clamp(t / 0.7F, 0.0F, 1.0F)) * 0.22F;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            dst[static_cast<std::size_t>(y) * w + x] =
                s.transparent ? Rgb{2, 2, 6, false}
                              : Rgb{u8(s.r * dim), u8(s.g * dim), u8(s.b * dim + 4), false};
          }
        const float speed = std::pow(std::clamp(t / 0.72F, 0.0F, 1.0F), 2.0F);
        for (int i = 0; i < kStars; ++i)
        {
          const float a = hash(i * 2) * 6.2832F;
          const float rb = (0.04F + hash(i * 2 + 1) * 0.5F) * mn;
          const float rr = rb * (1.0F + speed * 6.0F);
          const float streak = speed * mn * 0.55F;
          const Rgb c = sample(src, w, h, cx + std::cos(a) * rb, cy + std::sin(a) * rb / ya);
          const Rgb col =
              c.transparent
                  ? Rgb{210, 210, 230, false}
                  : Rgb{u8(120 + c.r * 0.6F), u8(120 + c.g * 0.6F), u8(140 + c.b * 0.6F), false};
          drawSeg(dst,
                  w,
                  h,
                  cx + std::cos(a) * rr,
                  cy + std::sin(a) * rr / ya,
                  cx + std::cos(a) * (rr + streak),
                  cy + std::sin(a) * (rr + streak) / ya,
                  std::max(0.6F, speed * mn * 0.012F + 0.5F),
                  ya,
                  col);
        }
        const float flash = std::exp(-std::pow((t - 0.72F) / 0.05F, 2.0F));
        if (flash > 0.01F)
          for (std::size_t k = 0; k < dst.size(); ++k)
            dst[k] = Rgb{u8(dst[k].r + (255 - dst[k].r) * flash),
                         u8(dst[k].g + (255 - dst[k].g) * flash),
                         u8(dst[k].b + (255 - dst[k].b) * flash),
                         false};
      });
}

void effectNewspaper(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float cx = w * 0.5F, cy = h * 0.5F;
  const int scale = std::max(1, static_cast<int>(std::lround(w / 130.0F)));
  runFrames(
      renderer,
      w,
      h,
      4800,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float sa = std::pow(1.0F - t, 2.0F) * 9.4F;  // spins, decaying to 0
        const float latScale = std::cos(sa);               // facing when sa->0
        const float fit = 0.20F + 0.78F * t;               // grows to fit the view
        const float halfW = w * 0.42F * fit, halfH = h * 0.42F * fit;
        for (int y = 0; y < h; ++y)  // dim the room behind the spinning page
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            dst[static_cast<std::size_t>(y) * w + x] =
                s.transparent ? Rgb{8, 8, 12, false}
                              : Rgb{u8(s.r * 0.25F), u8(s.g * 0.24F), u8(s.b * 0.22F), false};
          }
        const float swW = std::fabs(halfW * latScale);
        if (swW < 0.6F)
          return;
        const int x0 = static_cast<int>(cx - swW), x1 = static_cast<int>(cx + swW);
        const int y0 = static_cast<int>(cy - halfH), y1 = static_cast<int>(cy + halfH);
        for (int y = std::max(0, y0); y <= std::min(h - 1, y1); ++y)
          for (int x = std::max(0, x0); x <= std::min(w - 1, x1); ++x)
          {
            const float lx = (x - cx) / swW;                   // local -1..1
            const float ly = (y - cy) / halfH;                 // local -1..1
            const float sgn = latScale < 0.0F ? -1.0F : 1.0F;  // back side mirrors x
            const float ulx = lx * sgn;
            const Rgb d = sample(src, w, h, (ulx * 0.5F + 0.5F) * w, (ly * 0.5F + 0.5F) * h);
            const float l = d.transparent ? 80.0F : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
            float r = 222 + l * 0.10F, g = 214 + l * 0.10F, b = 192 + l * 0.08F;
            const float col = std::fabs(std::fmod(ulx * 2.5F + 5.0F, 1.0F) - 0.5F);
            if (col < 0.04F)
            {
              r *= 0.55F;
              g *= 0.55F;
              b *= 0.55F;
            }
            if (std::fabs(lx) > 0.985F || std::fabs(ly) > 0.985F)
            {
              r = 30;
              g = 30;
              b = 36;
            }
            dst[static_cast<std::size_t>(y) * w + x] = Rgb{u8(r), u8(g), u8(b), false};
          }
        if (latScale > 0.55F)  // headline appears as the card faces us
        {
          auto stamp = [&](const std::string& str, float leftX, float topY, int sc, const Rgb& c)
          {
            constexpr int kFW = 5, kFH = 7, kGap = 1;
            for (int ci = 0; ci < static_cast<int>(str.size()); ++ci)
            {
              const auto g =
                  glyph5x7(static_cast<char>(std::toupper(static_cast<unsigned char>(str[ci]))));
              const int cox = ci * (kFW + kGap) * sc;
              for (int fy = 0; fy < kFH; ++fy)
                for (int fx = 0; fx < kFW; ++fx)
                {
                  if (g[fy][fx] != '1')
                    continue;
                  for (int sy = 0; sy < sc; ++sy)
                    for (int sx = 0; sx < sc; ++sx)
                    {
                      const int xp = static_cast<int>(leftX) + cox + fx * sc + sx;
                      const int yp = static_cast<int>(topY) + fy * sc + sy;
                      if (xp >= 0 && xp < w && yp >= 0 && yp < h)
                        dst[static_cast<std::size_t>(yp) * w + xp] = c;
                    }
                }
            }
          };
          const int bigS = std::max(scale, 2);
          const std::string head = "EXTRA  EXTRA";
          const float bigW = static_cast<float>(head.size()) * 6 * bigS - bigS;
          stamp(head, cx - bigW * 0.5F, cy - halfH * 0.78F, bigS, Rgb{18, 16, 14, false});
          const std::string sub = "QDLESS EXITS";
          const float subW = static_cast<float>(sub.size()) * 6 * scale - scale;
          stamp(sub, cx - subW * 0.5F, cy - halfH * 0.48F, scale, Rgb{40, 36, 30, false});
          (void)mn;
        }
      });
}

void effectEndCard(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const int scale = std::max(2, static_cast<int>(std::lround(w / 60.0F)));
  runFrames(
      renderer,
      w,
      h,
      4600,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float desat = std::clamp(t / 0.35F, 0.0F, 1.0F);
        const float vign = std::clamp((t - 0.20F) / 0.50F, 0.0F, 1.0F);
        const float card = std::clamp((t - 0.45F) / 0.30F, 0.0F, 1.0F);
        const float fade = std::clamp((t - 0.88F) / 0.12F, 0.0F, 1.0F);
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 0.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            const float br = s.transparent ? 0 : s.r, bg = s.transparent ? 0 : s.g,
                        bb = s.transparent ? 0 : s.b;
            float r = br * (1 - desat) + l * 1.08F * desat;
            float g = bg * (1 - desat) + l * 0.86F * desat;
            float b = bb * (1 - desat) + l * 0.60F * desat;
            const float vd = std::hypot((x - w * 0.5F) / w, (y - h * 0.5F) / h);
            const float vm = 1.0F - vign * std::clamp(vd * 1.4F - 0.2F, 0.0F, 0.85F);
            r *= vm;
            g *= vm;
            b *= vm;
            dst[static_cast<std::size_t>(y) * w + x] = Rgb{u8(r), u8(g), u8(b), false};
          }
        const float cw = mn * 0.42F, ch = mn * 0.20F;
        const float cxp = w * 0.5F, cyp = h * 0.5F;
        for (int yo = -static_cast<int>(ch); yo <= static_cast<int>(ch); ++yo)
          for (int xo = -static_cast<int>(cw); xo <= static_cast<int>(cw); ++xo)
          {
            const int x = static_cast<int>(cxp) + xo, y = static_cast<int>(cyp) + yo;
            if (x < 0 || x >= w || y < 0 || y >= h)
              continue;
            const float ax = std::fabs(xo) / cw, ay = std::fabs(yo) / ch;
            const bool outer = (ax > 0.96F || ay > 0.92F);
            const bool inner = (ax > 0.86F && ax < 0.90F) || (ay > 0.78F && ay < 0.84F);
            if (outer || inner)
            {
              const Rgb b{u8(40), u8(34), u8(28), false};
              const Rgb& d = dst[static_cast<std::size_t>(y) * w + x];
              dst[static_cast<std::size_t>(y) * w + x] = Rgb{u8(d.r + (b.r - d.r) * card),
                                                             u8(d.g + (b.g - d.g) * card),
                                                             u8(d.b + (b.b - d.b) * card),
                                                             false};
            }
          }
        if (card > 0.0F)
        {
          auto stamp = [&](const std::string& str, float leftX, float topY, int sc, const Rgb& c)
          {
            constexpr int kFW = 5, kFH = 7, kGap = 1;
            for (int ci = 0; ci < static_cast<int>(str.size()); ++ci)
            {
              const auto g =
                  glyph5x7(static_cast<char>(std::toupper(static_cast<unsigned char>(str[ci]))));
              const int cox = ci * (kFW + kGap) * sc;
              for (int fy = 0; fy < kFH; ++fy)
                for (int fx = 0; fx < kFW; ++fx)
                {
                  if (g[fy][fx] != '1')
                    continue;
                  for (int sy = 0; sy < sc; ++sy)
                    for (int sx = 0; sx < sc; ++sx)
                    {
                      const int xp = static_cast<int>(leftX) + cox + fx * sc + sx;
                      const int yp = static_cast<int>(topY) + fy * sc + sy;
                      if (xp >= 0 && xp < w && yp >= 0 && yp < h)
                      {
                        const Rgb& d = dst[static_cast<std::size_t>(yp) * w + xp];
                        dst[static_cast<std::size_t>(yp) * w + xp] =
                            Rgb{u8(d.r + (c.r - d.r) * card),
                                u8(d.g + (c.g - d.g) * card),
                                u8(d.b + (c.b - d.b) * card),
                                false};
                      }
                    }
                }
            }
          };
          const std::string s = "THE END";
          const float tw = static_cast<float>(s.size()) * 6 * scale - scale;
          stamp(s, cxp - tw * 0.5F, cyp - 3.5F * scale, scale, Rgb{22, 18, 14, false});
        }
        if (fade > 0.0F)
          for (std::size_t k = 0; k < dst.size(); ++k)
            dst[k] = Rgb{u8(dst[k].r * (1 - fade)),
                         u8(dst[k].g * (1 - fade)),
                         u8(dst[k].b * (1 - fade)),
                         false};
      });
}

}  // namespace ee_detail
}  // namespace Qdless
