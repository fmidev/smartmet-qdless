#include "QdlessExitEffect.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <poll.h>
#include <unistd.h>

namespace Qdless
{
namespace
{
// A sub-pixel left at the terminal's default background — what every effect
// fades / scatters towards. Same convention as Palette::missingColor().
constexpr Rgb kBlank{0, 0, 0, true};

// Animation cadence. Effects specify a wall-clock *duration*; runFrames turns
// that into frames at ~kFps, so a longer effect just gets more frames and
// stays smooth rather than choppy. Per-effect durations live at each effect's
// runFrames call site; they range from ~1.4 s (CRT off) to ~10 s (tears in
// rain). The duration is the artistic budget — there is no global cap.
constexpr int kFps = 30;
constexpr int kFrameMs = 1000 / kFps;  // ~33 ms

// Nearest-neighbour read. Out-of-bounds samples return a blank sub-pixel so
// resampling effects (spiral, CRT squash) naturally reveal "nothing" past
// the original image edges.
inline Rgb sample(const std::vector<Rgb>& src, int w, int h, float fx, float fy)
{
  const int x = static_cast<int>(std::lround(fx));
  const int y = static_cast<int>(std::lround(fy));
  if (x < 0 || x >= w || y < 0 || y >= h)
    return kBlank;
  return src[static_cast<std::size_t>(y) * w + x];
}

// Composite one finished frame to the terminal. DEC mode 2026 brackets the
// write so the user sees a whole frame at once (terminals lacking 2026
// ignore it). Autowrap is assumed already disabled by the caller.
void present(const Renderer& renderer, const std::vector<Rgb>& buf, int w, int h)
{
  std::ostringstream os;
  renderer.render(os, buf, w, h, 0, 0);
  const std::string body = os.str();
  std::fputs("\x1b[?2026h", stdout);
  std::fwrite(body.data(), 1, body.size(), stdout);
  std::fputs("\x1b[?2026l", stdout);
  std::fflush(stdout);
}

// True if a keystroke is waiting on stdin. ncurses leaves the terminal in
// cbreak/noecho and isn't reading during an effect, so a key's bytes sit in
// the tty buffer; we poll (non-blocking), consume them, and report — so any
// key aborts the animation.
bool exitKeyPressed()
{
  struct pollfd pfd{STDIN_FILENO, POLLIN, 0};
  if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN) != 0)
  {
    char buf[32];
    return read(STDIN_FILENO, buf, sizeof(buf)) > 0;
  }
  return false;
}

// Runs an effect for `durationMs` wall-clock milliseconds at ~kFps. For frame
// i it calls fill(t, dst) with t = i / nframes (so t ends at 1.0) after
// blanking dst, then presents and sleeps until the frame's deadline. Effects
// that resample or scatter rely on the pre-blank; effects that overwrite every
// pixel ignore it harmlessly. Deadline-based pacing keeps the cadence (and
// thus the total wall time) stable even when per-frame render cost grows on
// big terminals.
template <typename Fill>
void runFrames(const Renderer& renderer, int w, int h, int durationMs, Fill&& fill)
{
  const int nframes = std::max(8, durationMs / kFrameMs);
  std::vector<Rgb> dst(static_cast<std::size_t>(w) * h, kBlank);
  const auto start = std::chrono::steady_clock::now();
  const auto frame = std::chrono::milliseconds(kFrameMs);
  for (int i = 1; i <= nframes; ++i)
  {
    const float t = static_cast<float>(i) / static_cast<float>(nframes);
    std::fill(dst.begin(), dst.end(), kBlank);
    fill(t, dst);
    present(renderer, dst, w, h);
    if (exitKeyPressed())
      return;  // any key aborts -> caller clears the screen and exits/returns
    std::this_thread::sleep_until(start + frame * i);
  }
}

// Cell aspect correction for the circular effects (ring / spiral). Terminal
// cells are ~1:2 (w:h); a sub-pixel is cellW/2 wide and cellH/subRows tall,
// so its height:width ratio is 4/subRows. Weighting vertical sub-pixel
// distance by this makes a "circle" in index space look round on screen.
float yAspectFor(const Renderer& renderer)
{
  return 4.0F / static_cast<float>(subRowsForStyle(renderer.cornerStyle()));
}

// Compact 5x7 uppercase font (rows top->bottom, '1' = lit). Covers A-Z and
// space; anything else renders blank. Used by the text effects (word reveal,
// rosebud). Callers uppercase via wordTargets.
std::array<const char*, 7> glyph5x7(char c)
{
  switch (c)
  {
    case 'A':
      return {"01110", "10001", "10001", "11111", "10001", "10001", "10001"};
    case 'B':
      return {"11110", "10001", "11110", "10001", "10001", "10001", "11110"};
    case 'C':
      return {"01110", "10001", "10000", "10000", "10000", "10001", "01110"};
    case 'D':
      return {"11110", "10001", "10001", "10001", "10001", "10001", "11110"};
    case 'E':
      return {"11111", "10000", "11110", "10000", "10000", "10000", "11111"};
    case 'F':
      return {"11111", "10000", "11110", "10000", "10000", "10000", "10000"};
    case 'G':
      return {"01110", "10001", "10000", "10111", "10001", "10001", "01111"};
    case 'H':
      return {"10001", "10001", "11111", "10001", "10001", "10001", "10001"};
    case 'I':
      return {"11111", "00100", "00100", "00100", "00100", "00100", "11111"};
    case 'J':
      return {"00111", "00010", "00010", "00010", "00010", "10010", "01100"};
    case 'K':
      return {"10001", "10010", "10100", "11000", "10100", "10010", "10001"};
    case 'L':
      return {"10000", "10000", "10000", "10000", "10000", "10000", "11111"};
    case 'M':
      return {"10001", "11011", "10101", "10101", "10001", "10001", "10001"};
    case 'N':
      return {"10001", "11001", "10101", "10011", "10001", "10001", "10001"};
    case 'O':
      return {"01110", "10001", "10001", "10001", "10001", "10001", "01110"};
    case 'P':
      return {"11110", "10001", "10001", "11110", "10000", "10000", "10000"};
    case 'Q':
      return {"01110", "10001", "10001", "10001", "10101", "10010", "01101"};
    case 'R':
      return {"11110", "10001", "10001", "11110", "10100", "10010", "10001"};
    case 'S':
      return {"01111", "10000", "10000", "01110", "00001", "00001", "11110"};
    case 'T':
      return {"11111", "00100", "00100", "00100", "00100", "00100", "00100"};
    case 'U':
      return {"10001", "10001", "10001", "10001", "10001", "10001", "01110"};
    case 'V':
      return {"10001", "10001", "10001", "10001", "10001", "01010", "00100"};
    case 'W':
      return {"10001", "10001", "10001", "10101", "10101", "11011", "10001"};
    case 'X':
      return {"10001", "10001", "01010", "00100", "01010", "10001", "10001"};
    case 'Y':
      return {"10001", "10001", "01010", "00100", "00100", "00100", "00100"};
    case 'Z':
      return {"11111", "00001", "00010", "00100", "01000", "10000", "11111"};
    case '?':
      return {"01110", "10001", "00001", "00010", "00100", "00000", "00100"};
    case '!':
      return {"00100", "00100", "00100", "00100", "00100", "00000", "00100"};
    case '.':
      return {"00000", "00000", "00000", "00000", "00000", "00000", "00100"};
    case '-':
      return {"00000", "00000", "00000", "01110", "00000", "00000", "00000"};
    default:
      return {"00000", "00000", "00000", "00000", "00000", "00000", "00000"};
  }
}

// Lit sub-pixel coordinates for `word` rendered centred, scaled to fit
// widthFrac × heightFrac of the screen. Shared by the text effects.
std::vector<std::pair<int, int>> wordTargets(
    const std::string& word, int w, int h, float widthFrac, float heightFrac)
{
  constexpr int kFW = 5;
  constexpr int kFH = 7;
  constexpr int kGap = 1;
  const int cols = static_cast<int>(word.size());
  const int totalFx = std::max(1, cols * kFW + (cols - 1) * kGap);
  const int scale =
      std::max(1, static_cast<int>(std::min(widthFrac * w / totalFx, heightFrac * h / kFH)));
  const float ox = (w - static_cast<float>(totalFx) * scale) * 0.5F;
  const float oy = (h - static_cast<float>(kFH) * scale) * 0.5F;
  std::vector<std::pair<int, int>> pts;
  for (int ci = 0; ci < cols; ++ci)
  {
    const auto g = glyph5x7(static_cast<char>(std::toupper(static_cast<unsigned char>(word[ci]))));
    const int charOx = ci * (kFW + kGap);
    for (int fy = 0; fy < kFH; ++fy)
      for (int fx = 0; fx < kFW; ++fx)
      {
        if (g[fy][fx] != '1')
          continue;
        for (int sy = 0; sy < scale; ++sy)
          for (int sx = 0; sx < scale; ++sx)
          {
            const int x = static_cast<int>(std::lround(ox + (charOx + fx) * scale + sx));
            const int y = static_cast<int>(std::lround(oy + fy * scale + sy));
            if (x >= 0 && x < w && y >= 0 && y < h)
              pts.emplace_back(x, y);
          }
      }
  }
  return pts;
}

// --- Effects --------------------------------------------------------------

// The view bursts apart: every sub-pixel flies radially outward from the
// centre by a factor that grows past the screen, so the image expands into
// sparse fragments and is gone by the end.
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

// The view collapses to a point, then a bright shockwave ring expands out
// of it and off the screen.
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

// The view swirls as it shrinks into the centre and vanishes.
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

// Matrix rain: each column slides straight down and off the bottom, with a
// random per-column head start and speed so the screen empties unevenly.
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

// Classic disintegration: sub-pixels wink out in a random order until the
// screen is blank.
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

// Old-CRT power-off: the image squashes to a thin horizontal band, then that
// line collapses to a bright dot which flashes out.
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

// Gentle fade of every colour towards black.
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

// HSV (h in [0,1), s/v in [0,1]) -> Rgb. Used for the fireworks burst hues.
Rgb hsv2rgb(float hue, float sat, float val)
{
  const float hh = (hue - std::floor(hue)) * 6.0F;
  const int i = static_cast<int>(hh);
  const float f = hh - static_cast<float>(i);
  const float p = val * (1.0F - sat);
  const float q = val * (1.0F - sat * f);
  const float u = val * (1.0F - sat * (1.0F - f));
  float r = val;
  float g = p;
  float b = q;
  switch (i % 6)
  {
    case 0:
      r = val, g = u, b = p;
      break;
    case 1:
      r = q, g = val, b = p;
      break;
    case 2:
      r = p, g = val, b = u;
      break;
    case 3:
      r = p, g = q, b = val;
      break;
    case 4:
      r = u, g = p, b = val;
      break;
    default:
      r = val, g = p, b = q;
      break;
  }
  return Rgb{static_cast<std::uint8_t>(std::lround(r * 255.0F)),
             static_cast<std::uint8_t>(std::lround(g * 255.0F)),
             static_cast<std::uint8_t>(std::lround(b * 255.0F)),
             false};
}

// Filled round sub-pixel dot of radius `rad` (x sub-pixels; vertical extent
// shrunk by `ya` so it stays round) centred at (px,py). Used by fireworks
// particles and the train's wheels / headlight / smoke.
inline void plotDot(
    std::vector<Rgb>& dst, int w, int h, float px, float py, float rad, float ya, const Rgb& c)
{
  const int x0 = std::max(0, static_cast<int>(std::floor(px - rad)));
  const int x1 = std::min(w - 1, static_cast<int>(std::ceil(px + rad)));
  const int y0 = std::max(0, static_cast<int>(std::floor(py - rad / ya)));
  const int y1 = std::min(h - 1, static_cast<int>(std::ceil(py + rad / ya)));
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x)
    {
      const float dx = x - px;
      const float dy = (y - py) * ya;
      if (dx * dx + dy * dy <= rad * rad)
        dst[static_cast<std::size_t>(y) * w + x] = c;
    }
}

// Burn-up: the frame catches fire along the bottom and a ragged flame front
// climbs to the top, leaving blackened (blank) screen behind it.
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

// Fireworks: the frame dims to night, then several colour bursts pop from
// random points, each a ring of fading particles that droop under gravity.
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

// Pac-Man wipe: a chomping Pac-Man crosses left to right; everything he has
// passed is eaten (blank), everything ahead is still the frame.
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

// Train wipe: a tiny locomotive chuffs across left to right, clearing the
// screen behind it. Deliberately crude — boiler, cab, stack, wheels, smoke.
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

// Coalesce: the screen's pixels stream together to spell "THE END", hold
// briefly, then fade out.
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

// Eye wink: the frame is what the eye sees; the lids close over it (a
// shrinking lens-shaped opening) until the eye is shut and the screen black.
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

// Fill the screen below `waterY` with a bluish ocean: a vertical gradient
// (lighter at the surface, darker with depth) under a gently wavy surface.
// `fade` in [0,1] dims the whole thing (used to drain the ocean at the end).
void drawOcean(std::vector<Rgb>& dst, int w, int h, float waterY, float fade)
{
  for (int x = 0; x < w; ++x)
  {
    const float wy = waterY + 2.0F * std::sin(x * 0.15F);  // gentle surface ripple
    const float deep = std::max(1.0F, h - wy);
    for (int y = static_cast<int>(std::ceil(wy)); y < h; ++y)
    {
      if (y < 0)
        continue;
      const float d = (y - wy) / deep;  // 0 surface .. 1 deep
      dst[static_cast<std::size_t>(y) * w + x] =
          Rgb{static_cast<std::uint8_t>(std::lround((30.0F - 25.0F * d) * fade)),
              static_cast<std::uint8_t>(std::lround((90.0F - 70.0F * d) * fade)),
              static_cast<std::uint8_t>(std::lround((160.0F - 100.0F * d) * fade)),
              false};
    }
  }
}

// Stamp a little yellow submarine centred at (scx,scy) in sub-pixel coords.
void drawSubmarine(std::vector<Rgb>& dst, int w, int h, float ya, float scx, float scy)
{
  const float hh = std::max(3.0F, h * 0.10F);  // hull half-height
  const float hw = hh * 4.5F;                  // hull half-width (elongated)
  const Rgb hull{240, 205, 40, false};
  const Rgb tower{215, 180, 30, false};
  const Rgb dark{60, 50, 10, false};
  auto box = [&](float ax, float bx, float ay, float by, const Rgb& c)
  {
    const int x0 = std::max(0, static_cast<int>(std::lround(std::min(ax, bx))));
    const int x1 = std::min(w - 1, static_cast<int>(std::lround(std::max(ax, bx))));
    const int y0 = std::max(0, static_cast<int>(std::lround(std::min(ay, by))));
    const int y1 = std::min(h - 1, static_cast<int>(std::lround(std::max(ay, by))));
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x)
        dst[static_cast<std::size_t>(y) * w + x] = c;
  };
  // Conning tower + periscope first; the hull is drawn over their base.
  box(scx - hw * 0.20F, scx + hw * 0.05F, scy - 2.0F * hh, scy, tower);
  box(scx - hw * 0.10F, scx - hw * 0.06F, scy - 2.8F * hh, scy - hh, dark);
  // Hull ellipse.
  const int x0 = std::max(0, static_cast<int>(scx - hw));
  const int x1 = std::min(w - 1, static_cast<int>(scx + hw));
  const int y0 = std::max(0, static_cast<int>(scy - hh));
  const int y1 = std::min(h - 1, static_cast<int>(scy + hh));
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x)
    {
      const float ex = (x - scx) / hw;
      const float ey = (y - scy) / hh;
      if (ex * ex + ey * ey <= 1.0F)
        dst[static_cast<std::size_t>(y) * w + x] = hull;
    }
  // Portholes.
  plotDot(dst, w, h, scx - hw * 0.45F, scy, std::max(1.0F, hh * 0.30F), ya, dark);
  plotDot(dst, w, h, scx, scy, std::max(1.0F, hh * 0.30F), ya, dark);
  plotDot(dst, w, h, scx + hw * 0.45F, scy, std::max(1.0F, hh * 0.30F), ya, dark);
}

// Drop-and-sink: the frame falls away as a yellow submarine drops in over a
// rising bluish ocean, then the sub sinks below the surface (trailing
// bubbles) and the ocean drains to black.
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

// James Bond gun-barrel: the frame is seen through a round barrel iris on a
// black field; then the muzzle flashes white, blood washes down, and the
// screen goes black.
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

// Crying eye: the frame narrows into an eye, a teardrop wells up at the lower
// lid and falls away, and the eye fades shut as everything dissipates.
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

// "Tears in rain" — a quiet homage to Roy Batty's farewell. The view weeps:
// its colours run downward in streaks while cold rain falls across it; a
// white dove lifts off and ascends as everything fades to black.
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

// The word-reveal anthology: closing lines drawn at random when no message
// is supplied. Mostly public-domain (Shakespeare etc.) plus quirks, celebrity
// epitaphs, international farewells, and theatrical / shell puns. All A-Z and
// the few punctuation glyphs the font carries. Exposed via exitWordline*().
const char* const kExitWordlines[] = {
    "THE END",
    "THE REST IS SILENCE",
    "GOOD NIGHT SWEET PRINCE",
    "OUR REVELS NOW ARE ENDED",
    "OUT OUT BRIEF CANDLE",
    "ALL THE WORLDS A STAGE",
    "JOURNEYS END IN LOVERS MEETING",
    "TOMORROW AND TOMORROW AND TOMORROW",
    "ALL GOOD THINGS MUST END",
    "UNTIL WE MEET AGAIN",
    "THIS TOO SHALL PASS",
    "THE PARTY IS OVER",
    "I COULD HAVE BEEN A CONTENDER",
    "TOMORROW IS ANOTHER DAY",
    "THE HORROR THE HORROR",
    "ROLL THE CREDITS",
    // Quirks — a wink on the way out.
    "WHAT? ALREADY?",
    "LEAVING SO SOON?",
    "WAS IT SOMETHING I SAID?",
    "DONT BE A STRANGER",
    "OH. OKAY THEN.",
    "BYE THEN!",
    "GAME OVER",
    "FIN",
    "THAT IS IT?",
    "SAME TIME TOMORROW?",
    "MIND THE DOOR",
    "AND SCENE.",
    "GOING DARK",
    "THIS WAS BORING ANYWAY",
    "TO BE OR NOT TO BE JUST BULLSHIT",
    "SHALL I WRAP UP?",
    "CURTAIN CALL",
    "ELVIS HAS LEFT THE BUILDING",
    "DISAPPOINTED!",
    "ILL SEE MYSELF OUT",
    "SHOWING MYSELF OUT",
    "NO TAKEBACKS",
    "TOO LATE NOW",
    "DECISION FINAL",
    "RAGE QUIT",
    "BLINK AND ITS GONE",
    // Deadpan celebrity epitaphs — quote reveals word by word, then the
    // "- NAME" credit lands on its own card.
    "SHOULDNT HAVE DONE THIS EITHER - SEAN BEAN",
    "I AM CLOSER THAN YOU THINK - KEVIN BACON",
    "I AM NOT DONE WITH YOU YET - CHUCK NORRIS",
    "NO ONE WILL BELIEVE YOU - BILL MURRAY",
    "I DID MY OWN EXIT - JACKIE CHAN",
    "AND SO HE LEFT - MORGAN FREEMAN",
    "NOW YOU ARE SPLITTING? - JEAN-CLAUDE VAN DAMME",
    "I WOULD HAVE RUN HERE TOO - TOM CRUISE",
    "I WAS GONE THE WHOLE TIME - BRUCE WILLIS",
    "ILL WALK THIS OFF - HUGH JACKMAN",
    "IT WAS ABOUT FAMILY - VIN DIESEL",
    // Internationally recognised words for "the end" / farewell.
    "FINITO",
    "KAPUT",
    "FINIS",
    "BASTA!",
    "ADIOS",
    "SAYONARA",
    "ARRIVEDERCI",
    "AUF WIEDERSEHEN",
    "ADIEU",
    "CIAO",
    "VALE",
    // Finnish.
    "LOPPU",
    "VALMIS",
    "MORJENS",
    "MOI MOI",
    // Theatrical exits (the diamond: a stage direction from The Winter's Tale).
    "EXIT PURSUED BY A BEAR",
    "EXEUNT",
    "EXEUNT OMNES",
    "TAKE A BOW",
    "THE STAGE GOES DARK",
    "DAISY DAISY GIVE ME YOUR ANSWER DO",
    "I CAME I SAW I LEFT",
    // Terminal / shell puns.
    "EXIT CODE ZERO",
    "RETURNING TO THE SHELL",
    "BACK TO THE PROMPT",
    "SIGTERM RECEIVED",
    "GRACEFUL SHUTDOWN",
    "SESSION TERMINATED",
    "GONE BUT NOT DEBUGGED",
    "REST IN PROCESSES",
};
constexpr int kExitWordlineCount =
    static_cast<int>(sizeof(kExitWordlines) / sizeof(kExitWordlines[0]));

// Word reveal: the view separates into glowing particles that gather to spell
// each word of `message` in turn (one at a time), then fade away. Text is
// caller-supplied (App passes --exit-message); falls back to "THE END".
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

// "koyaanisqatsi" — life out of balance: the view wobbles with growing
// amplitude, loses its footing, then topples into an accelerating spin and
// fades to black.
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

// "rosebud" — a last word on a deathbed: the view dims as a vignette closes
// in, snow drifts down (the globe), and ROSEBUD glows faintly at the centre
// before the screen goes dark.
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

// Shiver: the whole view trembles — jittering by a growing random offset each
// frame — while it fades to black.
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

// HAL 9000 shutdown: the view funnels inward into HAL's red eye, which holds
// its gaze, then slowly dims to dark — a last hot pinpoint winking out.
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

// Juggle into a solved cube: the view scatters into tumbling, juggling
// particles that arc through the air, then converge and resolve into a solved
// Rubik's cube before fading.
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

// Effect roster. Keep in sync with the dispatch switch below and with the
// names in exitEffectName().
constexpr int kEffectCount = 23;
}  // namespace

int exitEffectCount()
{
  return kEffectCount;
}

const char* exitEffectName(int effectIndex)
{
  static const char* const kNames[kEffectCount] = {
      "explode",  "implode + ring", "spiral",      "matrix drop",   "dissolve",
      "CRT off",  "fade",           "fire",        "fireworks",     "Pac-Man",
      "train",    "The End",        "eye wink",    "submarine",     "Bond barrel",
      "teardrop", "tears in rain",  "word reveal", "koyaanisqatsi", "rosebud",
      "shiver",   "HAL 9000",       "Rubik"};
  if (effectIndex < 0 || effectIndex >= kEffectCount)
    return "random";
  return kNames[effectIndex];
}

int exitWordlineCount()
{
  return kExitWordlineCount;
}

const char* exitWordline(int index)
{
  return (index >= 0 && index < kExitWordlineCount) ? kExitWordlines[index] : "";
}

int exitEffectIndexByName(const std::string& name)
{
  // Normalise to lowercase alphanumerics so spacing / case / punctuation
  // differences don't matter.
  auto norm = [](const std::string& s)
  {
    std::string o;
    for (char c : s)
      if (std::isalnum(static_cast<unsigned char>(c)) != 0)
        o.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return o;
  };
  const std::string target = norm(name);
  if (target.empty())
    return -1;
  for (int i = 0; i < kEffectCount; ++i)
    if (norm(exitEffectName(i)) == target)
      return i;
  return -1;
}

ExitEffectPlay playExitEffect(const Renderer& renderer,
                              std::vector<Rgb> frame,
                              int subW,
                              int subH,
                              int effectIndex,
                              unsigned seed,
                              std::string words)
{
  // A nonzero seed always survives the round-trip, so a stored {index, seed}
  // can be replayed verbatim by the repeat key. 0 means "pick a fresh one".
  if (seed == 0)
  {
    seed = std::random_device{}();
    if (seed == 0)
      seed = 1U;
  }

  if (subW < 2 || subH < 2 ||
      frame.size() < static_cast<std::size_t>(subW) * static_cast<std::size_t>(subH))
    return {effectIndex < 0 ? 0 : effectIndex % kEffectCount, seed};

  std::mt19937 rng(seed);
  // Always draw once for the (possibly overridden) effect choice, so the rng
  // stream the effect then consumes is identical whether the index was random
  // or forced — that's what makes a replayed seed reproduce the exact frame.
  const int pick = static_cast<int>(rng() % static_cast<unsigned>(kEffectCount));
  const int e = (effectIndex < 0) ? pick : (effectIndex % kEffectCount);

  // Disable autowrap so the bottom-right cell can be painted without
  // scrolling the screen.
  std::fputs("\x1b[?7l", stdout);

  switch (e)
  {
    case 0:
      effectExplode(renderer, frame, subW, subH);
      break;
    case 1:
      effectImplodeRing(renderer, frame, subW, subH);
      break;
    case 2:
      effectSpiral(renderer, frame, subW, subH);
      break;
    case 3:
      effectMatrix(renderer, frame, subW, subH, rng);
      break;
    case 4:
      effectDissolve(renderer, frame, subW, subH, rng);
      break;
    case 5:
      effectCrtOff(renderer, frame, subW, subH);
      break;
    case 6:
      effectFade(renderer, frame, subW, subH);
      break;
    case 7:
      effectFire(renderer, frame, subW, subH, rng);
      break;
    case 8:
      effectFireworks(renderer, frame, subW, subH, rng);
      break;
    case 9:
      effectPacman(renderer, frame, subW, subH);
      break;
    case 10:
      effectTrain(renderer, frame, subW, subH);
      break;
    case 11:
      effectTheEnd(renderer, frame, subW, subH, rng);
      break;
    case 12:
      effectEyewink(renderer, frame, subW, subH);
      break;
    case 13:
      effectSubmarine(renderer, frame, subW, subH, rng);
      break;
    case 14:
      effectBond(renderer, frame, subW, subH);
      break;
    case 15:
      effectTeardrop(renderer, frame, subW, subH);
      break;
    case 16:
      effectTearsInRain(renderer, frame, subW, subH, rng);
      break;
    case 17:
      effectWordReveal(renderer, frame, subW, subH, rng, words);
      break;
    case 18:
      effectKoyaanisqatsi(renderer, frame, subW, subH);
      break;
    case 19:
      effectRosebud(renderer, frame, subW, subH, rng);
      break;
    case 20:
      effectShiver(renderer, frame, subW, subH, rng);
      break;
    case 21:
      effectHal9000(renderer, frame, subW, subH);
      break;
    case 22:
      effectRubik(renderer, frame, subW, subH, rng);
      break;
    default:
      effectFade(renderer, frame, subW, subH);
      break;
  }

  // Guaranteed clean end state: restore autowrap, reset attributes, clear.
  std::fputs("\x1b[?7h\x1b[0m\x1b[2J\x1b[H", stdout);
  std::fflush(stdout);
  return {e, seed};
}
}  // namespace Qdless
