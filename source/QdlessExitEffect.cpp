#include "QdlessExitEffect.h"

#include "QdlessImageSource.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <boost/dll/runtime_symbol_info.hpp>

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

// Surprise-stomp interrupt. One exit in ten can be cut short by the Monty
// Python foot: playExitEffect arms this before running the chosen effect, and
// runFrames watches the wall clock — once past the trigger it snapshots the
// frame on screen and bails out, leaving playExitEffect to stomp that snapshot.
bool g_stompArmed = false;
bool g_stompFired = false;
double g_stompTriggerMs = 0.0;
std::chrono::steady_clock::time_point g_stompArmStart;
std::vector<Rgb> g_stompCapture;

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
  if (g_stompFired)
    return;  // a surprise stomp already preempted this exit
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
    if (g_stompArmed)
    {
      const double elapsed = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - g_stompArmStart)
                                 .count();
      if (elapsed >= g_stompTriggerMs)
      {
        g_stompCapture = dst;  // snapshot the view the foot will crush
        g_stompFired = true;
        return;
      }
    }
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
    case ',':
      return {"00000", "00000", "00000", "00000", "00100", "00100", "01000"};
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

// Foot stomp: a giant flat bare foot — sole-first, five toes — drops from above
// and squishes the data view flat, compressing it into the shrinking strip
// beneath the descending sole until nothing is left, then stays planted.
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

// Resolve the path to the Monty Python foot image. Mirrors the cities1000.tsv
// lookup in App: installed copy first, then the dev tree beside the binary,
// then a user override. Returns empty if none exists.
std::string findFootImage()
{
  std::vector<std::filesystem::path> candidates{
      std::filesystem::path("/usr/share/smartmet/qdless/foot.png")};
  try
  {
    const std::filesystem::path exeDir = boost::dll::program_location().parent_path().string();
    candidates.push_back(exeDir / "data" / "foot.png");
  }
  catch (const std::exception&)
  {
  }
  if (const char* home = std::getenv("HOME"))
    candidates.push_back(std::filesystem::path(home) / ".config" / "qdless" / "foot.png");
  for (const auto& p : candidates)
  {
    std::error_code ec;
    if (std::filesystem::exists(p, ec))
      return p.string();
  }
  return {};
}

// Decode data/foot.png via the project's image reader. Returns nullptr (and
// leaves fw/fh untouched) if the file is missing or fails to decode, so the
// caller can fall back to the hand-drawn foot.
std::unique_ptr<ImageSource> loadFootImage(std::size_t& fw, std::size_t& fh)
{
  const std::string path = findFootImage();
  if (path.empty())
    return nullptr;
  try
  {
    auto img = std::make_unique<ImageSource>(path);
    const auto [iw, ih] = img->pixelSize();
    if (iw == 0 || ih == 0)
      return nullptr;
    fw = iw;
    fh = ih;
    return img;
  }
  catch (const std::exception&)
  {
    return nullptr;
  }
}

// One frame of the Monty Python stomp: the foot's sole sits at contactY =
// p^2*h; the whole `view` is squashed into the strip below it, and the foot
// painting (sampled by footPixel over [0,1]^2, with v=1 the sole) fills the
// screen above. p in [0,1]; p=1 is the full slam — screen all foot, view gone.
template <typename FootFn>
void stompFrame(
    std::vector<Rgb>& dst, const std::vector<Rgb>& view, int w, int h, float p, FootFn&& footPixel)
{
  const float contactY = p * p * static_cast<float>(h);  // sole row
  // Below the sole: the entire view squashed into [contactY, h).
  for (int y = static_cast<int>(std::ceil(contactY)); y < h; ++y)
  {
    const float frac = (y - contactY) / std::max(1.0F, static_cast<float>(h) - contactY);
    const int row = std::min(h - 1, static_cast<int>(frac * (h - 1)));
    for (int x = 0; x < w; ++x)
      dst[static_cast<std::size_t>(y) * w + x] = view[static_cast<std::size_t>(row) * w + x];
  }
  // Above the sole: the painting, sole (v=1) pinned to the contact line,
  // scaled so it exactly fills the screen at full slam.
  const int contactRow = static_cast<int>(std::ceil(contactY));
  for (int y = 0; y < contactRow && y < h; ++y)
  {
    const float fv =
        (static_cast<float>(y) - (contactY - static_cast<float>(h))) / static_cast<float>(h);
    for (int x = 0; x < w; ++x)
      dst[static_cast<std::size_t>(y) * w + x] = footPixel((x + 0.5F) / w, fv);
  }
}

void effectMontyPythonProcedural(const Renderer& renderer,
                                 const std::vector<Rgb>& src,
                                 int w,
                                 int h);

// "...and now for something completely different." Terry Gilliam's foot
// (data/foot.png) drops straight in from the top of the screen — sole leading —
// and STOMPS, crushing the current view into a vanishing strip beneath it,
// then stays planted. Each frame is composited into an off-screen buffer and
// presented whole (DEC 2026 synchronised output) through the high-fidelity
// block-glyph Renderer; nothing is drawn onto the live screen incrementally.
// If the image isn't installed we fall back to the hand-drawn foot below.
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

// Fallback for effectMontyPython when data/foot.png can't be loaded: a
// hand-drawn Monty Python foot — Terry Gilliam's side-on cut-out, a bare leg
// trailing off the top — slides straight down and STOMPS, squishing the data
// view flat beneath its flat sole, then stays planted.
void effectMontyPythonProcedural(const Renderer& renderer,
                                 const std::vector<Rgb>& src,
                                 int w,
                                 int h)
{
  const float ya = yAspectFor(renderer);
  const float px = (w - 1) * 0.52F;              // the ankle / leg column
  const float sx = std::min(0.58F * w, h * ya);  // foot spans ~0.94*w toe to heel
  const float sy = sx / ya;

  // Side-profile foot (Bronzino's cherub foot, à la Gilliam) in local coords (nx
  // across: toe at -1, heel at +0.66; ny up = negative, sole at 0). kTop is the
  // rounded instep dome; kBot the flat sole, which scallops at the front into
  // splayed toe pads. toeFreq/toePhase lay out the four toes.
  constexpr float toeFreq = 43.0F;
  constexpr float toePhase = 5.30F;
  static const std::pair<float, float> kTop[] = {
      {-1.00F, -0.14F},  // toe tips
      {-0.78F, -0.22F},
      {-0.52F, -0.34F},
      {-0.26F, -0.47F},
      {-0.08F, -0.52F},  // instep peak (forward of ankle)
      {0.20F, -0.52F},   // high under the leg
      {0.44F, -0.46F},   // achilles / back of ankle
      {0.56F, -0.28F},
      {0.64F, -0.08F}};  // heel
  static const std::pair<float, float> kBot[] = {
      {-1.00F, -0.01F}, {-0.86F, 0.00F}, {0.46F, 0.00F}, {0.58F, 0.00F}, {0.64F, -0.06F}};
  auto profile = [](const std::pair<float, float>* p, int n, float x)
  {
    if (x <= p[0].first)
      return p[0].second;
    if (x >= p[n - 1].first)
      return p[n - 1].second;
    for (int i = 1; i < n; ++i)
      if (x <= p[i].first)
        return p[i - 1].second + (x - p[i - 1].first) / (p[i].first - p[i - 1].first) *
                                     (p[i].second - p[i - 1].second);
    return p[n - 1].second;
  };
  auto inFootBody = [&](float nx, float ny)
  {
    if (nx < -1.00F || nx > 0.64F)
      return false;
    float bot = profile(kBot, 5, nx);
    if (nx < -0.42F)  // front: the sole scallops into separate toe pads
      bot = -0.03F + 0.03F * std::cos(toeFreq * nx + toePhase);
    return ny >= profile(kTop, 9, nx) && ny <= bot;
  };
  // The leg rises over the back of the foot: a thick ankle flaring out at the
  // bottom and tapering up into the shin.
  auto inLeg = [](float nx, float ny)
  {
    if (ny > -0.30F)
      return false;
    const float t = std::min(1.0F, (-0.30F - ny) / 0.80F);
    return nx >= -0.04F + 0.06F * t && nx <= 0.48F - 0.05F * t;
  };
  auto inSkin = [&](float nx, float ny) { return inFootBody(nx, ny) || inLeg(nx, ny); };

  // Bronzino's foot is pink: rose skin, dark-rose creases / outline, pale nail.
  const Rgb skin{223, 163, 169, false};
  const Rgb crease{178, 112, 124, false};  // dark rose, between the toes
  const Rgb nail{236, 200, 200, false};
  const Rgb rim{158, 96, 110, false};
  // Paint the foot with its sole at screen y = cy (leg rising off the top);
  // `alpha` fades it out.
  auto drawFoot = [&](std::vector<Rgb>& dst, float cx, float cy, float alpha)
  {
    const int x0 = std::max(0, static_cast<int>(std::floor(cx - 1.05F * sx)));
    const int x1 = std::min(w - 1, static_cast<int>(std::ceil(cx + 0.64F * sx)));
    const int y1 = std::min(h - 1, static_cast<int>(std::ceil(cy + 0.05F * sy)));
    constexpr float e = 0.04F;  // neighbour offset for the rim test
    for (int y = 0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x)
      {
        const float nx = (x - cx) / sx;
        const float ny = (y - cy) / sy;
        if (!inSkin(nx, ny))
          continue;
        Rgb c = skin;
        float lit = 0.80F + 0.20F * std::min(1.0F, -ny / 0.50F);  // top-lit roundness
        if (nx < -0.40F && ny > -0.18F && std::cos(toeFreq * nx + toePhase) < -0.30F)
        {
          c = crease;  // the splay between two toes
          lit = 1.0F;
        }
        else if (nx <= -0.95F && ny >= -0.13F && ny <= -0.06F)
        {
          c = nail;  // big-toe nail at the front
          lit = 1.0F;
        }
        if (!inSkin(nx - e, ny) || !inSkin(nx + e, ny) || !inSkin(nx, ny - e) ||
            !inSkin(nx, ny + e))
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
            700,
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
              drawFoot(dst, px, contactY, 1.0F);  // the sole presses at contactY
            });
}

// "Light at the end of the tunnel": the view warps into a round, receding 3D
// tunnel with a black vanishing point; the far end then lights up white and we
// accelerate down the bore until the light floods the screen.
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

// Stamp a bullet hole at (cx,cy): a near-black puncture with a scorched ring,
// a torn dark-red rim and a scatter of blood specks. `r` is the bore radius in
// x-units; ya keeps it round.
void drawBulletHole(std::vector<Rgb>& dst, int w, int h, float cx, float cy, float r, float ya)
{
  const Rgb blood{86, 12, 12, false};
  const Rgb scorch{34, 16, 14, false};
  const Rgb hole{10, 8, 9, false};
  for (int k = 0; k < 6; ++k)  // blood spatter around the entry
  {
    const float a = static_cast<float>(k) * 1.0472F + cx * 0.13F;  // 60deg apart, jittered
    const float d = r * 2.4F;
    plotDot(dst,
            w,
            h,
            cx + std::cos(a) * d,
            cy + std::sin(a) * d / ya,
            std::max(1.0F, r * 0.2F),
            ya,
            blood);
  }
  plotDot(dst, w, h, cx, cy, r * 1.7F, ya, blood);
  plotDot(dst, w, h, cx, cy, r * 1.25F, ya, scorch);
  plotDot(dst, w, h, cx, cy, r, ya, hole);
}

// True if (dx, dyPx) lies inside a six-armed snowflake of radius R (x-units;
// ya corrects the vertical): a hex core, six tapering primary arms, six shorter
// secondary arms offset 30deg, and little branch tufts on the primary arms.
bool inSnowflake(float dx, float dyPx, float R, float ya)
{
  const float dy = dyPx * ya;
  const float r = std::hypot(dx, dy);
  if (r > R)
    return false;
  if (r < R * 0.14F)
    return true;  // hex core
  const float rr = r / R;
  const float ang = std::atan2(dy, dx);
  constexpr float s60 = 1.0471976F;
  constexpr float s30 = 0.5235988F;
  const float a1 = ang - s60 * std::round(ang / s60);  // angle to nearest primary axis
  if (std::fabs(a1) < 0.16F * (1.0F - 0.7F * rr))
    return true;  // primary arm, tapering outward
  if (rr < 0.55F)
  {
    const float a2 = (ang - s30) - s60 * std::round((ang - s30) / s60);
    if (std::fabs(a2) < 0.13F * (1.0F - rr))
      return true;  // shorter secondary arm
  }
  if ((std::fabs(rr - 0.50F) < 0.06F || std::fabs(rr - 0.74F) < 0.06F) &&
      std::fabs(std::fabs(a1) - 0.40F) < 0.16F)
    return true;  // branch tufts
  return false;
}

// Stamp centred multi-line text (5x7 font) in `color`, scaled to fit
// widthFrac x heightFrac of the screen. Each line is centred horizontally.
void drawLines(std::vector<Rgb>& dst,
               int w,
               int h,
               const std::vector<std::string>& lines,
               const Rgb& color,
               float widthFrac,
               float heightFrac)
{
  constexpr int kFW = 5;
  constexpr int kFH = 7;
  constexpr int kGap = 1;
  constexpr int kLineGap = 3;
  int maxCols = 1;
  for (const auto& s : lines)
    maxCols = std::max(maxCols, static_cast<int>(s.size()));
  const int totalFx = maxCols * kFW + (maxCols - 1) * kGap;
  const int nLines = static_cast<int>(lines.size());
  const int totalFy = nLines * kFH + (nLines - 1) * kLineGap;
  const int scale =
      std::max(1, static_cast<int>(std::min(widthFrac * w / totalFx, heightFrac * h / totalFy)));
  const float oy = (h - static_cast<float>(totalFy) * scale) * 0.5F;
  for (int li = 0; li < nLines; ++li)
  {
    const std::string& s = lines[li];
    const int cols = static_cast<int>(s.size());
    const int lineFx = std::max(1, cols * kFW + (cols - 1) * kGap);
    const float ox = (w - static_cast<float>(lineFx) * scale) * 0.5F;
    const float ly = oy + static_cast<float>(li * (kFH + kLineGap) * scale);
    for (int ci = 0; ci < cols; ++ci)
    {
      const auto g = glyph5x7(static_cast<char>(std::toupper(static_cast<unsigned char>(s[ci]))));
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
              const int y = static_cast<int>(std::lround(ly + fy * scale + sy));
              if (x >= 0 && x < w && y >= 0 && y < h)
                dst[static_cast<std::size_t>(y) * w + x] = color;
            }
        }
    }
  }
}

// Build a lit/unlit bitmap for one or more centred text lines at native 5x7
// font resolution. Sets bw/bh to the bitmap size.
std::vector<char> buildTextBitmap(const std::vector<std::string>& lines, int& bw, int& bh)
{
  constexpr int kFW = 5;
  constexpr int kFH = 7;
  constexpr int kGap = 1;
  constexpr int kLineGap = 2;
  int maxCols = 1;
  for (const auto& s : lines)
    maxCols = std::max(maxCols, static_cast<int>(s.size()));
  const int nLines = static_cast<int>(lines.size());
  bw = std::max(1, maxCols * kFW + (maxCols - 1) * kGap);
  bh = std::max(1, nLines * kFH + (nLines - 1) * kLineGap);
  std::vector<char> grid(static_cast<std::size_t>(bw) * bh, 0);
  for (int li = 0; li < nLines; ++li)
  {
    const std::string& s = lines[li];
    const int cols = static_cast<int>(s.size());
    const int lineFx = cols * kFW + (cols - 1) * kGap;
    const int ox0 = (bw - lineFx) / 2;  // centre each line in the bitmap
    const int oy = li * (kFH + kLineGap);
    for (int ci = 0; ci < cols; ++ci)
    {
      const auto g = glyph5x7(static_cast<char>(std::toupper(static_cast<unsigned char>(s[ci]))));
      const int ox = ox0 + ci * (kFW + kGap);
      for (int fy = 0; fy < kFH; ++fy)
        for (int fx = 0; fx < kFW; ++fx)
          if (g[fy][fx] == '1')
            grid[static_cast<std::size_t>(oy + fy) * bw + ox + fx] = 1;
    }
  }
  return grid;
}

// Overlay a Star Wars-style crawl onto `dst`: the text bitmap (bw*bh) laid on a
// ground plane that recedes to a vanishing point at row h*0.30 (Mode-7
// perspective). scrollFront sets how far the text's near edge has receded
// (larger = farther); gFade dims the whole crawl. nearWidthFrac is the text
// width (as a fraction of the screen) at the near edge — values > 1 make the
// text overflow the sides when close and shrink to fit as it recedes. Sampling
// is coverage-based (a screen cell lights if ANY text texel it covers is lit),
// so thin strokes — e.g. the bottom bar that tells 'E' from 'F' — never drop
// out under minification.
void drawCrawl(std::vector<Rgb>& dst,
               int w,
               int h,
               const std::vector<char>& text,
               int bw,
               int bh,
               float scrollFront,
               const Rgb& color,
               float gFade,
               float nearWidthFrac)
{
  const float cx = (w - 1) * 0.5F;
  const float vy = h * 0.30F;                 // horizon / vanishing row
  const float camH = h - vy;                  // so depth == 1 at the bottom row
  const float textDepth = 0.9F;               // depth band the text occupies
  const float kx = bw / (nearWidthFrac * w);  // horizontal world scale
  for (int y = static_cast<int>(std::ceil(vy)) + 1; y < h; ++y)
  {
    // This screen row spans depths [depthNear, depthFar]; find the text rows
    // it covers and OR over them.
    const float depthFar = camH / (static_cast<float>(y) - vy);
    const float depthNear = camH / (static_cast<float>(y + 1) - vy);
    float tvfA = bh * (scrollFront + textDepth - depthFar) / textDepth;
    float tvfB = bh * (scrollFront + textDepth - depthNear) / textDepth;
    if (tvfA > tvfB)
      std::swap(tvfA, tvfB);
    if (tvfB < 0.0F || tvfA >= static_cast<float>(bh))
      continue;
    const int rLo = std::max(0, static_cast<int>(std::floor(tvfA)));
    const int rHi = std::min(bh - 1, static_cast<int>(std::floor(tvfB)));
    const float depthMid = camH / (static_cast<float>(y) - vy + 0.5F);
    const float bright = std::clamp(1.15F - depthMid * 0.12F, 0.30F, 1.0F) * gFade;
    if (bright <= 0.0F)
      continue;
    const Rgb c{static_cast<std::uint8_t>(color.r * bright),
                static_cast<std::uint8_t>(color.g * bright),
                static_cast<std::uint8_t>(color.b * bright),
                false};
    const float colStep = depthMid * kx;  // text columns spanned per screen cell
    for (int x = 0; x < w; ++x)
    {
      const float tu = (x - cx) * depthMid * kx + bw * 0.5F;
      const int cLo = std::max(0, static_cast<int>(std::floor(tu)));
      const int cHi = std::min(bw - 1, static_cast<int>(std::floor(tu + colStep)));
      bool lit = false;
      for (int r = rLo; r <= rHi && !lit; ++r)
        for (int c2 = cLo; c2 <= cHi; ++c2)
          if (text[static_cast<std::size_t>(r) * bw + c2] != 0)
          {
            lit = true;
            break;
          }
      if (lit)
        dst[static_cast<std::size_t>(y) * w + x] = c;
    }
  }
}

// Death by gunshot: three bullet holes punch into the view in quick succession
// (each with an impact spark), a brief pause, a single eyelid blink, a beat,
// then the holed view topples to the left and drops away as vision fades.
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

// The view turns into a flurry of giant six-armed snowflakes (each tinted with
// the colour it covers) that drift down, sway, and shrink away to nothing.
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

// Anticlimax: nothing happens for a long beat, then the punchline cuts in.
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

// End-credits crawl: "THE END" recedes up a ground plane toward a vanishing
// point (Mode-7 perspective) while the coloured view fades to black behind it.
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

// Star Wars prologue meets Monty Python: the blue two-line crawl recedes into
// the distance, and then the foot drops in and STOMPS it flat.
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

// Flatland (after Abbott): the view rotates back in 3D about its horizontal
// centre axis until it is edge-on — a single flat line. Then small dots crawl
// to and fro along that line while the word "FLATLAND" fades in above it.
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

// The classic TV test card: a burst of static "tunes in" to 75% SMPTE colour
// bars (with a reverse-bar strip and a PLUGE row), wobbling on a vertical hold
// that settles, under faint CRT scanlines.
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

// Draw one dove: a flapping chevron (two wing-lines sweeping up and out from a
// body apex at px,py), rotated by `ang` so it leans into its flight direction.
// `rise` is the current wing height (modulated by the caller for the flap);
// span is the half-wingspan. Shape is built in physical coords (ya-corrected).
void drawDove(std::vector<Rgb>& dst,
              int w,
              int h,
              float px,
              float py,
              float span,
              float rise,
              float ang,
              float ya,
              const Rgb& col)
{
  const float ca = std::cos(ang);
  const float sa = std::sin(ang);
  for (int sgn = -1; sgn <= 1; sgn += 2)
    for (float u = 0.0F; u <= 1.0F; u += 0.1F)
    {
      const float lx = static_cast<float>(sgn) * span * u;  // local: out along the wing
      // Curved (gull) wing: arcs up to a peak then eases the tip over, instead
      // of a flat straight line.
      const float ly = -rise * std::sin(u * 2.2F);
      const float rx = lx * ca - ly * sa;
      const float ryp = lx * sa + ly * ca;
      plotDot(dst, w, h, px + rx, py + ryp / ya, 1.1F, ya, col);
    }
}

// A large flock of doves: from the (fading) view, white doves lift off in a
// staggered wave and climb away, fanning out in different upward directions
// while their wings beat, until the screen empties.
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

// Aurora borealis: the view fades to a deep-blue night sky with twinkling
// stars while shimmering green/violet curtains of light wave and ripple
// overhead, swelling and then fading away.
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

// Thunderstorm: the view darkens under storm light and diagonal rain while
// forked lightning bolts crack across with a white flash, then it fades out.
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

// Conway's Game of Life: the view's bright pixels seed a (toroidal) Life grid
// that evolves generation by generation — each living cell painted in the
// colour the image holds at that spot — then fades to an empty grid.
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

// Thanos snap: the view disintegrates into ash. A diagonal sweep (with random
// per-pixel jitter) turns each pixel to a drifting, greying mote that blows up
// and to the right and fades, until nothing is left.
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

// "Utopia": the weather data fades to reveal just the coastlines and borders,
// and then Sweden quietly eats itself out — its outline dissolving from within
// into a growing, ragged void — leaving empty space between Norway and Finland.
// Falls back to a plain fade when the geography isn't available (no coastlines,
// an image source, or Sweden simply isn't in view). `linesFrame` is a w*h
// raster of the coastlines/borders on black; `swedenMask` marks the pixels
// inside Sweden.
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

// Bouncing soft ball: the view collapses into a sphere-shaded ball textured
// with itself, then bounces around the terminal under gravity — losing energy
// on each bounce, squashing and stretching on impact, drifting and rolling —
// reasonably close to real ball physics.
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

// Draw a sphere textured with the view: a globe-mapped, lit ball (longitude
// `lon0` spins it). rx/ry are the screen radii; `shade` ramps the 3D shading in
// (0 flat, 1 full sphere); (tr,tg,tb) tint the surface (1,1,1 = untinted).
void drawSphere(std::vector<Rgb>& dst,
                int w,
                int h,
                const std::vector<Rgb>& src,
                float cx,
                float cy,
                float rx,
                float ry,
                float shade,
                float lon0,
                float tr,
                float tg,
                float tb)
{
  if (rx < 0.5F || ry < 0.5F)
    return;
  const int x0 = std::max(0, static_cast<int>(std::floor(cx - rx)));
  const int x1 = std::min(w - 1, static_cast<int>(std::ceil(cx + rx)));
  const int y0 = std::max(0, static_cast<int>(std::floor(cy - ry)));
  const int y1 = std::min(h - 1, static_cast<int>(std::ceil(cy + ry)));
  constexpr float kLx = -0.42F;
  constexpr float kLy = -0.50F;
  constexpr float kLz = 0.76F;
  constexpr float kTwoPi = 6.2831853F;
  constexpr float kPi = 3.14159265F;
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x)
    {
      const float nx = (x - cx) / rx;
      const float ny = (y - cy) / ry;
      const float r2 = nx * nx + ny * ny;
      if (r2 > 1.0F)
        continue;
      const float z = std::sqrt(1.0F - r2);
      float u = 0.5F + (std::atan2(nx, z) + lon0) / kTwoPi;
      u -= std::floor(u);
      const float v = 0.5F + std::asin(std::clamp(ny, -1.0F, 1.0F)) / kPi;
      const Rgb tex = sample(src, w, h, u * (w - 1), v * (h - 1));
      const float cr = tex.transparent ? 60.0F : tex.r;
      const float cg = tex.transparent ? 62.0F : tex.g;
      const float cb = tex.transparent ? 72.0F : tex.b;
      const float ndotl = std::max(0.0F, nx * kLx + ny * kLy + z * kLz);
      const float lit = (1.0F - shade) + shade * (0.30F + 0.70F * ndotl);
      const float spec = shade * std::pow(ndotl, 22.0F) * 200.0F;
      dst[static_cast<std::size_t>(y) * w + x] =
          Rgb{u8(cr * lit * tr + spec), u8(cg * lit * tg + spec), u8(cb * lit * tb + spec), false};
    }
}

// Solar system: the view collapses into a (warm, glowing) sun and then buds off
// several different-sized planets that fly out and orbit it on a tilted
// ecliptic — inner ones faster (Kepler-ish), passing in front of and behind the
// sun, over faint orbit rings.
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

// Saturn: the view collapses into a spinning tan globe wrapped by tilted,
// slowly-rotating rings (with a Cassini gap) that pass in front of and behind
// the planet.
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

// Black hole: everything spirals into a central singularity. The view shrinks
// inward and swirls (faster nearer the centre, like infalling matter), dimming
// with gravitational redshift, behind a growing event horizon rimmed by a
// blue-white accretion glow, until all is swallowed.
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

// Interstellar's Gargantua: a static, gravitationally-lensed black hole formed
// from the view. The background (the view) bends around the shadow; a thin
// photon ring outlines it; and a tilted, rotating accretion disk — Doppler-
// brightened on its approaching side — wraps the shadow, its near half crossing
// in front while a warm halo arcs over the top and under the bottom.
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

// Outline of a stingray seen from above (bx lateral, by longitudinal; by=+1
// nose, wings reach bx=±1, trailing edge sweeps back to the tail base near
// by=-0.6). The whip tail is added separately.
const std::array<std::pair<float, float>, 13> kRay = {{{0.00F, 1.00F},
                                                       {0.35F, 0.72F},
                                                       {0.72F, 0.42F},
                                                       {1.00F, 0.04F},
                                                       {0.74F, -0.34F},
                                                       {0.34F, -0.54F},
                                                       {0.12F, -0.60F},
                                                       {-0.12F, -0.60F},
                                                       {-0.34F, -0.54F},
                                                       {-0.74F, -0.34F},
                                                       {-1.00F, 0.04F},
                                                       {-0.72F, 0.42F},
                                                       {-0.35F, 0.72F}}};

// Stingray: the view collapses into a ray textured with itself, which then
// swims away along a curving path — its heading turning, its body shrinking and
// foreshortening into the distance, wings undulating and tail swaying.
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

// Point-in-triangle test (used for the butterfly's pointed wing spikes).
bool inTri(float px, float py, float ax, float ay, float bx, float by, float cx, float cy)
{
  auto cross = [](float px, float py, float ax, float ay, float bx, float by)
  { return (px - bx) * (ay - by) - (ax - bx) * (py - by); };
  const float d1 = cross(px, py, ax, ay, bx, by);
  const float d2 = cross(px, py, bx, by, cx, cy);
  const float d3 = cross(px, py, cx, cy, ax, ay);
  const bool neg = d1 < 0 || d2 < 0 || d3 < 0;
  const bool pos = d1 > 0 || d2 > 0 || d3 > 0;
  return !(neg && pos);
}

// Draw a thick segment (a limb / tentacle / tail) as a row of aspect-correct
// dots from (ax,ay) to (bxe,bye).
void drawSeg(std::vector<Rgb>& dst,
             int w,
             int h,
             float ax,
             float ay,
             float bxe,
             float bye,
             float rad,
             float ya,
             const Rgb& c)
{
  const float dx = bxe - ax;
  const float dy = bye - ay;
  const int n = std::max(1, static_cast<int>(std::hypot(dx, dy * ya) / std::max(0.6F, rad * 0.7F)));
  for (int i = 0; i <= n; ++i)
  {
    const float f = static_cast<float>(i) / n;
    plotDot(dst, w, h, ax + dx * f, ay + dy * f, rad, ya, c);
  }
}

// Jellyfish: the view collapses into a translucent bell that rhythmically
// pulses (contract = taller+narrower, with a little jet upward) as it drifts up
// and out of frame, trailing long wavy tentacles.
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

// Butterfly: the view collapses into a butterfly whose wings open and close
// (flapping = foreshortening their span) as it flutters along a looping path,
// banking with each turn, and off the screen.
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

// Fish: the view collapses into a side-profile fish that swims off along a
// curving path (turning as it goes), its body undulating and tail sweeping,
// shrinking into the distance.
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

// Crab: the view collapses into a crab — a wide textured shell on eight jointed
// legs (a walking gait), eyestalks, and two raised, waving claws — that
// scuttles sideways across and off the screen.
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

// Spider: the view collapses into a round textured body that descends from the
// top of the frame on a silk thread, its eight bent legs wiggling.
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

// Effect roster. Keep in sync with the dispatch switch below and with the
// names in exitEffectName().
constexpr int kEffectCount = 50;
}  // namespace

int exitEffectCount()
{
  return kEffectCount;
}

const char* exitEffectName(int effectIndex)
{
  static const char* const kNames[kEffectCount] = {
      "explode",      "implode + ring", "spiral",      "matrix drop",   "dissolve",
      "CRT off",      "fade",           "fire",        "fireworks",     "Pac-Man",
      "train",        "The End",        "eye wink",    "submarine",     "Bond barrel",
      "teardrop",     "tears in rain",  "word reveal", "koyaanisqatsi", "rosebud",
      "shiver",       "HAL 9000",       "Rubik",       "foot stomp",    "Monty Python",
      "tunnel",       "gunshot",        "snowfall",    "gotcha",        "Star Wars",
      "Python wars",  "Flatland",       "test card",   "doves",         "aurora",
      "thunderstorm", "game of life",   "Thanos snap", "Utopia",        "bouncing ball",
      "solar system", "Saturn",         "black hole",  "Interstellar",  "stingray",
      "jellyfish",    "butterfly",      "fish",        "crab",          "spider"};
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
                              std::string words,
                              const std::vector<Rgb>* linesFrame,
                              const std::vector<char>* swedenMask)
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

  // Reset any leftover interrupt state, then roll for a surprise Monty Python
  // stomp. Both draws happen unconditionally so the rng stream the effect then
  // consumes stays identical for a replayed seed. One exit in five is cut short
  // — except the stomp-based effects, which already end on a foot.
  g_stompArmed = false;
  g_stompFired = false;
  const bool stompRoll = (rng() % 5U) == 0U;
  const double stompDelayMs = 350.0 + static_cast<double>(rng() % 700U);
  std::unique_ptr<ImageSource> stompFoot;
  if (stompRoll && e != 23 && e != 24 && e != 30)
  {
    std::size_t sfw = 0;
    std::size_t sfh = 0;
    stompFoot = loadFootImage(sfw, sfh);
    if (stompFoot)
    {
      g_stompArmed = true;
      g_stompTriggerMs = stompDelayMs;
      g_stompArmStart = std::chrono::steady_clock::now();
    }
  }

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
    case 23:
      effectFoot(renderer, frame, subW, subH);
      break;
    case 24:
      effectMontyPython(renderer, frame, subW, subH);
      break;
    case 25:
      effectTunnel(renderer, frame, subW, subH);
      break;
    case 26:
      effectGunshot(renderer, frame, subW, subH, rng);
      break;
    case 27:
      effectSnowflakes(renderer, frame, subW, subH, rng);
      break;
    case 28:
      effectGotcha(renderer, frame, subW, subH);
      break;
    case 29:
      effectStarWars(renderer, frame, subW, subH);
      break;
    case 30:
      effectPythonWars(renderer, frame, subW, subH);
      break;
    case 31:
      effectFlatland(renderer, frame, subW, subH, rng);
      break;
    case 32:
      effectTestCard(renderer, frame, subW, subH, rng);
      break;
    case 33:
      effectDoves(renderer, frame, subW, subH, rng);
      break;
    case 34:
      effectAurora(renderer, frame, subW, subH, rng);
      break;
    case 35:
      effectThunderstorm(renderer, frame, subW, subH, rng);
      break;
    case 36:
      effectGameOfLife(renderer, frame, subW, subH);
      break;
    case 37:
      effectThanos(renderer, frame, subW, subH, rng);
      break;
    case 38:
      effectUtopia(renderer, frame, subW, subH, rng, linesFrame, swedenMask);
      break;
    case 39:
      effectBall(renderer, frame, subW, subH);
      break;
    case 40:
      effectSolarSystem(renderer, frame, subW, subH, rng);
      break;
    case 41:
      effectSaturn(renderer, frame, subW, subH);
      break;
    case 42:
      effectBlackHole(renderer, frame, subW, subH);
      break;
    case 43:
      effectInterstellar(renderer, frame, subW, subH);
      break;
    case 44:
      effectStingray(renderer, frame, subW, subH);
      break;
    case 45:
      effectJellyfish(renderer, frame, subW, subH);
      break;
    case 46:
      effectButterfly(renderer, frame, subW, subH);
      break;
    case 47:
      effectFish(renderer, frame, subW, subH);
      break;
    case 48:
      effectCrab(renderer, frame, subW, subH);
      break;
    case 49:
      effectSpider(renderer, frame, subW, subH);
      break;
    default:
      effectFade(renderer, frame, subW, subH);
      break;
  }

  // If a surprise stomp fired mid-effect, crush the snapshot under the foot —
  // even faster than the usual stomp (slam in ~75 ms), then hold the planted
  // foot a beat so the gag registers before the program exits.
  g_stompArmed = false;
  if (stompFoot && g_stompFired)
  {
    g_stompFired = false;  // let the stomp's own runFrames run to completion
    auto footPixel = [&](float u, float v) -> Rgb
    { return stompFoot->pixelAtUV(std::clamp(u, 0.0F, 0.999F), std::clamp(v, 0.0F, 0.999F)); };
    std::vector<Rgb> view = std::move(g_stompCapture);
    runFrames(renderer,
              subW,
              subH,
              500,
              [&](float t, std::vector<Rgb>& dst)
              { stompFrame(dst, view, subW, subH, std::min(1.0F, t / 0.15F), footPixel); });
  }

  // Guaranteed clean end state: restore autowrap, reset attributes, clear.
  std::fputs("\x1b[?7h\x1b[0m\x1b[2J\x1b[H", stdout);
  std::fflush(stdout);
  return {e, seed};
}
}  // namespace Qdless
