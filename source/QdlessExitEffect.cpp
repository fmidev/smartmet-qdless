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

// Speed multiplier for the skeleton dance effects. Each dance states its own
// natural rhythm (its beat at speed 1.0); this scales them all together, so one
// knob sets how frantic the whole crew is — and it can be tuned from outside
// later.
const auto speed = 5.0F;

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

// Riverdance: the view fades to a dark stage and a row of bone-white skeletons
// (tinted from the view) rises and dances in unison — rigid torsos and arms,
// rapid synchronized high kicks.
void effectRiverdance(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  constexpr int kN = 5;
  const float floorY = (h - 1) * 0.94F;
  const float bodyH = h * 0.60F;
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  std::array<Rgb, kN> bones{};
  for (int i = 0; i < kN; ++i)
  {
    const Rgb v = sample(src, w, h, w * ((i + 0.5F) / kN), (h - 1) * 0.5F);
    const float vr = v.transparent ? 200.0F : v.r;
    const float vg = v.transparent ? 200.0F : v.g;
    const float vb = v.transparent ? 200.0F : v.b;
    bones[i] = Rgb{
        u8(228 * 0.7F + vr * 0.3F), u8(226 * 0.7F + vg * 0.3F), u8(208 * 0.7F + vb * 0.3F), false};
  }

  auto skeleton =
      [&](std::vector<Rgb>& dst, float cx, float baseY, float beat, const Rgb& bone, const Rgb& eye)
  {
    const float Hh = bodyH;
    const float bob = std::sin(beat) * Hh * 0.02F;
    const float hy = baseY - 0.46F * Hh - bob;
    const float neckY = baseY - 0.78F * Hh - bob;
    const float skullY = baseY - 0.88F * Hh - bob;
    const float shY = baseY - 0.73F * Hh - bob;
    const float rad = Hh * 0.085F;
    const float bR = std::max(1.0F, Hh * 0.016F);
    drawSeg(dst, w, h, cx, hy, cx, neckY, bR, ya, bone);  // spine
    for (int r = 0; r < 3; ++r)                           // ribs
    {
      const float ry = neckY + (hy - neckY) * (0.28F + 0.22F * r);
      const float rw = Hh * 0.11F * (1.0F - 0.13F * r);
      drawSeg(dst, w, h, cx - rw, ry, cx + rw, ry, bR * 0.7F, ya, bone);
    }
    const float shW = Hh * 0.13F;
    drawSeg(dst, w, h, cx - shW, shY, cx + shW, shY, bR, ya, bone);  // shoulders
    drawSeg(dst, w, h, cx - shW, shY, cx - shW * 0.85F, hy + Hh * 0.02F, bR * 0.9F, ya, bone);
    drawSeg(dst, w, h, cx + shW, shY, cx + shW * 0.85F, hy + Hh * 0.02F, bR * 0.9F, ya, bone);
    drawSeg(dst, w, h, cx - Hh * 0.07F, hy, cx + Hh * 0.07F, hy, bR, ya, bone);  // pelvis
    for (int s = -1; s <= 1; s += 2)                                             // legs
    {
      const float lift = std::max(0.0F, s > 0 ? std::sin(beat) : -std::sin(beat));
      const float hipx = cx + s * Hh * 0.06F;
      const float footx =
          (cx + s * Hh * 0.06F) + (cx + s * Hh * 0.24F - (cx + s * Hh * 0.06F)) * lift;
      const float footy = baseY + ((hy - Hh * 0.30F) - baseY) * lift;
      const float kneex =
          (cx + s * Hh * 0.05F) + (cx + s * Hh * 0.17F - (cx + s * Hh * 0.05F)) * lift;
      const float kneey = ((hy + baseY) * 0.5F) + ((hy - Hh * 0.02F) - (hy + baseY) * 0.5F) * lift;
      drawSeg(dst, w, h, hipx, hy, kneex, kneey, bR, ya, bone);
      drawSeg(dst, w, h, kneex, kneey, footx, footy, bR * 0.9F, ya, bone);
      drawSeg(dst, w, h, footx, footy, footx + s * Hh * 0.05F, footy, bR * 0.8F, ya, bone);
    }
    plotDot(dst, w, h, cx, skullY, rad, ya, bone);  // skull
    plotDot(
        dst, w, h, cx - rad * 0.42F, skullY - rad * 0.05F, std::max(1.0F, rad * 0.26F), ya, eye);
    plotDot(
        dst, w, h, cx + rad * 0.42F, skullY - rad * 0.05F, std::max(1.0F, rad * 0.26F), ya, eye);
    plotDot(dst, w, h, cx, skullY + rad * 0.4F, std::max(1.0F, rad * 0.16F), ya, eye);  // nasal
  };

  runFrames(
      renderer,
      w,
      h,
      5200,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float vf = std::clamp(1.0F - t * 4.5F, 0.0F, 1.0F);  // view -> dark stage
        for (std::size_t k = 0; k < dst.size(); ++k)
        {
          const Rgb& s0 = src[k];
          dst[k] = s0.transparent ? Rgb{0, 0, 0, false}
                                  : Rgb{u8(s0.r * vf), u8(s0.g * vf), u8(s0.b * vf), false};
        }
        const int fy = static_cast<int>(floorY);
        if (fy >= 0 && fy < h)
          for (int x = 0; x < w; ++x)
            dst[static_cast<std::size_t>(fy) * w + x] = Rgb{42, 42, 50, false};  // stage floor
        const float alpha = std::clamp((t - 0.10F) / 0.13F, 0.0F, 1.0F);
        if (alpha <= 0.0F)
          return;
        const float beat = t * 8.0F * speed;  // tempo (low natural beat: fast footwork already)
        for (int i = 0; i < kN; ++i)
        {
          const Rgb b{
              u8(bones[i].r * alpha), u8(bones[i].g * alpha), u8(bones[i].b * alpha), false};
          const Rgb e{u8(18 * alpha), u8(12 * alpha), u8(12 * alpha), false};
          skeleton(dst, w * ((i + 0.5F) / kN), floorY, beat, b, e);
        }
      });
}

// Pac-Man duel: a small regular yellow Pac-Man chomps across, erasing the data
// behind it; then the remaining data suddenly flies together (imploding) to
// form a Pac-Man much larger than it, which turns to face it and lunges to
// defend itself — sending the little one fleeing in terror off the screen.
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

// 3D tornado: the data wraps onto a spinning funnel (wide at the cloud,
// narrowing to the ground) that rotates rapidly, sways, and recedes into the
// distance.
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

// --- Skeleton crew --------------------------------------------------------
// A posable skeleton shared by the dance effects. A limb is two segments
// (shoulder->elbow->hand, or hip->knee->foot); ex/ey and hx/hy are the elbow
// and hand/foot offsets from the shoulder/hip in body-height units (+x right,
// +y down). `bob` raises the whole body; `headDX` leans the skull.
struct Limb
{
  float ex;
  float ey;
  float hx;
  float hy;
};

inline Limb lerpLimb(const Limb& a, const Limb& b, float t)
{
  return Limb{a.ex + (b.ex - a.ex) * t,
              a.ey + (b.ey - a.ey) * t,
              a.hx + (b.hx - a.hx) * t,
              a.hy + (b.hy - a.hy) * t};
}

void drawSkeleton(std::vector<Rgb>& dst,
                  int w,
                  int h,
                  float ya,
                  float cx,
                  float baseY,
                  float H,
                  float bob,
                  float headDX,
                  const Limb& aL,
                  const Limb& aR,
                  const Limb& lL,
                  const Limb& lR,
                  const Rgb& bone,
                  const Rgb& eye,
                  float latScale = 1.0F)  // <1 squashes horizontally (a pirouette/turn)
{
  const float hy = baseY - 0.46F * H - bob;
  const float neckY = baseY - 0.78F * H - bob;
  const float skullY = baseY - 0.88F * H - bob;
  const float shY = baseY - 0.73F * H - bob;
  const float rad = H * 0.085F;
  const float bR = std::max(1.0F, H * 0.016F);
  const float shW = H * 0.13F;
  auto X = [&](float off) { return cx + off * latScale; };  // horizontal, scaled about cx
  drawSeg(dst, w, h, X(0), hy, X(0), neckY, bR, ya, bone);  // spine
  for (int r = 0; r < 3; ++r)                               // ribs
  {
    const float ry = neckY + (hy - neckY) * (0.28F + 0.22F * r);
    const float rw = H * 0.11F * (1.0F - 0.13F * r);
    drawSeg(dst, w, h, X(-rw), ry, X(rw), ry, bR * 0.7F, ya, bone);
  }
  drawSeg(dst, w, h, X(-shW), shY, X(shW), shY, bR, ya, bone);            // shoulders
  drawSeg(dst, w, h, X(-H * 0.07F), hy, X(H * 0.07F), hy, bR, ya, bone);  // pelvis
  {
    const float o = -shW;  // left arm
    drawSeg(dst, w, h, X(o), shY, X(o + aL.ex * H), shY + aL.ey * H, bR * 0.9F, ya, bone);
    drawSeg(dst,
            w,
            h,
            X(o + aL.ex * H),
            shY + aL.ey * H,
            X(o + aL.hx * H),
            shY + aL.hy * H,
            bR * 0.85F,
            ya,
            bone);
  }
  {
    const float o = shW;  // right arm
    drawSeg(dst, w, h, X(o), shY, X(o + aR.ex * H), shY + aR.ey * H, bR * 0.9F, ya, bone);
    drawSeg(dst,
            w,
            h,
            X(o + aR.ex * H),
            shY + aR.ey * H,
            X(o + aR.hx * H),
            shY + aR.hy * H,
            bR * 0.85F,
            ya,
            bone);
  }
  {
    const float o = -H * 0.06F;  // left leg
    drawSeg(dst, w, h, X(o), hy, X(o + lL.ex * H), hy + lL.ey * H, bR, ya, bone);
    drawSeg(dst,
            w,
            h,
            X(o + lL.ex * H),
            hy + lL.ey * H,
            X(o + lL.hx * H),
            hy + lL.hy * H,
            bR * 0.9F,
            ya,
            bone);
  }
  {
    const float o = H * 0.06F;  // right leg
    drawSeg(dst, w, h, X(o), hy, X(o + lR.ex * H), hy + lR.ey * H, bR, ya, bone);
    drawSeg(dst,
            w,
            h,
            X(o + lR.ex * H),
            hy + lR.ey * H,
            X(o + lR.hx * H),
            hy + lR.hy * H,
            bR * 0.9F,
            ya,
            bone);
  }
  const float skX = X(headDX);
  plotDot(dst, w, h, skX, skullY, rad, ya, bone);  // skull
  plotDot(dst,
          w,
          h,
          skX - rad * 0.42F * latScale,
          skullY - rad * 0.05F,
          std::max(1.0F, rad * 0.26F),
          ya,
          eye);
  plotDot(dst,
          w,
          h,
          skX + rad * 0.42F * latScale,
          skullY - rad * 0.05F,
          std::max(1.0F, rad * 0.26F),
          ya,
          eye);
  plotDot(dst, w, h, skX, skullY + rad * 0.4F, std::max(1.0F, rad * 0.16F), ya, eye);
}

// Fade the view to a dark stage with a floor line; return the skeletons' fade-in
// alpha (0 until ~t=0.10, full by ~0.23).
float skeletonStage(
    std::vector<Rgb>& dst, const std::vector<Rgb>& src, int w, int h, float t, int floorY)
{
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float vf = std::clamp(1.0F - t * 4.5F, 0.0F, 1.0F);
  for (std::size_t k = 0; k < dst.size(); ++k)
  {
    const Rgb& s0 = src[k];
    dst[k] = s0.transparent ? Rgb{0, 0, 0, false}
                            : Rgb{u8(s0.r * vf), u8(s0.g * vf), u8(s0.b * vf), false};
  }
  if (floorY >= 0 && floorY < h)
    for (int x = 0; x < w; ++x)
      dst[static_cast<std::size_t>(floorY) * w + x] = Rgb{42, 42, 50, false};
  return std::clamp((t - 0.10F) / 0.13F, 0.0F, 1.0F);
}

// Bone-white tint for the skeleton at column x, blended from the view and
// pre-multiplied by `alpha`.
Rgb boneTint(const std::vector<Rgb>& src, int w, int h, float x, float alpha)
{
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const Rgb v = sample(src, w, h, x, (h - 1) * 0.5F);
  const float vr = v.transparent ? 200.0F : v.r;
  const float vg = v.transparent ? 200.0F : v.g;
  const float vb = v.transparent ? 200.0F : v.b;
  return Rgb{u8((228 * 0.7F + vr * 0.3F) * alpha),
             u8((226 * 0.7F + vg * 0.3F) * alpha),
             u8((208 * 0.7F + vb * 0.3F) * alpha),
             false};
}

// Thriller: a row of skeletons does a synchronized undead shuffle — heads
// lurch, clawed arms thrust up and sweep side to side, hips sway.
void effectThriller(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  constexpr int kN = 5;
  const float floorY = (h - 1) * 0.94F;
  const float H = h * 0.58F;
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer,
            w,
            h,
            5000,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float alpha = skeletonStage(dst, src, w, h, t, static_cast<int>(floorY));
              if (alpha <= 0.0F)
                return;
              const float beat = t * 9.0F * speed;
              const float sway = std::sin(beat);
              const float bob = std::sin(beat * 2.0F) * 0.025F * H;
              const float headDX = sway * 0.05F * H;
              const Limb aL{-0.18F + sway * 0.05F, -0.05F, -0.10F + sway * 0.20F, -0.32F};
              const Limb aR{0.18F + sway * 0.05F, -0.05F, 0.10F + sway * 0.20F, -0.32F};
              const Limb lL{-0.01F + sway * 0.03F, 0.23F, 0.02F + sway * 0.05F, 0.46F};
              const Limb lR{0.01F + sway * 0.03F, 0.23F, -0.02F + sway * 0.05F, 0.46F};
              for (int i = 0; i < kN; ++i)
              {
                const float cx = w * ((i + 0.5F) / kN);
                drawSkeleton(dst,
                             w,
                             h,
                             ya,
                             cx,
                             floorY,
                             H,
                             bob,
                             headDX,
                             aL,
                             aR,
                             lL,
                             lR,
                             boneTint(src, w, h, cx, alpha),
                             Rgb{u8(18 * alpha), u8(12 * alpha), u8(12 * alpha), false});
              }
            });
}

// YMCA: the row throws its arms into the Y-M-C-A letter shapes in turn.
void effectYMCA(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  constexpr int kN = 5;
  const float floorY = (h - 1) * 0.94F;
  const float H = h * 0.58F;
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  static const std::array<Limb, 4> kLA = {{{-0.14F, -0.19F, -0.28F, -0.46F},
                                           {-0.19F, -0.19F, -0.07F, 0.16F},
                                           {-0.17F, -0.20F, -0.31F, -0.06F},
                                           {-0.12F, -0.26F, -0.04F, -0.48F}}};
  static const std::array<Limb, 4> kRA = {{{0.14F, -0.19F, 0.28F, -0.46F},
                                           {0.19F, -0.19F, 0.07F, 0.16F},
                                           {-0.03F, -0.03F, -0.25F, 0.12F},
                                           {0.12F, -0.26F, 0.04F, -0.48F}}};
  const Limb lL{-0.01F, 0.23F, 0.02F, 0.46F};
  const Limb lR{0.01F, 0.23F, -0.02F, 0.46F};
  runFrames(renderer,
            w,
            h,
            5200,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float alpha = skeletonStage(dst, src, w, h, t, static_cast<int>(floorY));
              if (alpha <= 0.0F)
                return;
              const float seg = 0.16F / speed;  // letter-hold time
              const int idx = static_cast<int>(t / seg) % 4;
              const int prev = (idx + 3) % 4;
              const float blend = std::clamp((t / seg - std::floor(t / seg)) / 0.3F, 0.0F, 1.0F);
              const Limb aL = lerpLimb(kLA[prev], kLA[idx], blend);
              const Limb aR = lerpLimb(kRA[prev], kRA[idx], blend);
              const float bob = std::sin(t * 9.0F * speed) * 0.02F * H;
              for (int i = 0; i < kN; ++i)
              {
                const float cx = w * ((i + 0.5F) / kN);
                drawSkeleton(dst,
                             w,
                             h,
                             ya,
                             cx,
                             floorY,
                             H,
                             bob,
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

// The wave: a stadium 'wave' ripples down the line — each skeleton flings its
// arms up and bobs in sequence.
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

// Greek (Sirtaki): a linked line — arms stretched out to neighbours — doing
// alternating cross-kicks with a side sway.
void effectGreekDance(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  constexpr int kN = 5;
  const float floorY = (h - 1) * 0.94F;
  const float H = h * 0.58F;
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const Limb aL{-0.30F, -0.06F, -0.55F, -0.06F};  // arms out, linking the line
  const Limb aR{0.30F, -0.06F, 0.55F, -0.06F};
  runFrames(renderer,
            w,
            h,
            5200,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float alpha = skeletonStage(dst, src, w, h, t, static_cast<int>(floorY));
              if (alpha <= 0.0F)
                return;
              const float beat = t * 7.0F * speed;
              const float sway = std::sin(beat);
              const float kickL = std::max(0.0F, -std::sin(beat));
              const float kickR = std::max(0.0F, std::sin(beat));
              const Limb lL =
                  lerpLimb({-0.01F, 0.23F, 0.02F, 0.46F}, {0.10F, 0.10F, 0.22F, 0.16F}, kickL);
              const Limb lR =
                  lerpLimb({0.01F, 0.23F, -0.02F, 0.46F}, {-0.10F, 0.10F, -0.22F, 0.16F}, kickR);
              const float bob = std::fabs(sway) * 0.02F * H;
              const float headDX = sway * 0.03F * H;
              for (int i = 0; i < kN; ++i)
              {
                const float cx = w * ((i + 0.5F) / kN);
                drawSkeleton(dst,
                             w,
                             h,
                             ya,
                             cx,
                             floorY,
                             H,
                             bob,
                             headDX,
                             aL,
                             aR,
                             lL,
                             lR,
                             boneTint(src, w, h, cx, alpha),
                             Rgb{u8(18 * alpha), u8(12 * alpha), u8(12 * alpha), false});
              }
            });
}

// Russian (Cossack): deep squat, arms folded, fast alternating leg kicks.
void effectRussianDance(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  constexpr int kN = 5;
  const float floorY = (h - 1) * 0.94F;
  const float H = h * 0.58F;
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const Limb aL{0.12F, -0.08F, 0.16F, 0.05F};  // arms folded across the chest
  const Limb aR{-0.12F, -0.08F, -0.16F, 0.05F};
  runFrames(renderer,
            w,
            h,
            5000,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float alpha = skeletonStage(dst, src, w, h, t, static_cast<int>(floorY));
              if (alpha <= 0.0F)
                return;
              const float beat = t * 12.0F * speed;
              const float bob = -0.12F * H + std::sin(beat) * 0.04F * H;  // squat + bounce
              const float kickL = std::max(0.0F, -std::sin(beat));
              const float kickR = std::max(0.0F, std::sin(beat));
              // Cossack squat: the thighs splay out from the hips (the bend is at
              // the hip, not the knee) and the shins drop to the feet; on the beat
              // one leg shoots straight out to the side.
              const Limb lL =
                  lerpLimb({-0.20F, 0.16F, -0.09F, 0.33F}, {-0.16F, 0.06F, -0.55F, 0.06F}, kickL);
              const Limb lR =
                  lerpLimb({0.20F, 0.16F, 0.09F, 0.33F}, {0.16F, 0.06F, 0.55F, 0.06F}, kickR);
              for (int i = 0; i < kN; ++i)
              {
                const float cx = w * ((i + 0.5F) / kN);
                drawSkeleton(dst,
                             w,
                             h,
                             ya,
                             cx,
                             floorY,
                             H,
                             bob,
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

// Ballet: a pirouette. Arms pull into first position, the working leg tucks to
// passé, the dancer rises onto pointe and spins about its vertical axis — the
// turn is faked by squashing the figure horizontally (latScale = cos(angle)),
// so it goes edge-on and back to full width once per half-turn.
void effectBallet(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  constexpr int kN = 5;
  const float floorY = (h - 1) * 0.94F;
  const float H = h * 0.58F;
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const Limb lL{0.0F, 0.25F, 0.0F, 0.50F};       // supporting leg, straight on pointe
  const Limb lR{0.16F, 0.12F, 0.02F, 0.26F};     // working leg in passé (foot to knee)
  const Limb aL{-0.14F, -0.02F, -0.02F, 0.10F};  // arms rounded in first position
  const Limb aR{0.14F, -0.02F, 0.02F, 0.10F};
  runFrames(renderer,
            w,
            h,
            4500,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float alpha = skeletonStage(dst, src, w, h, t, static_cast<int>(floorY));
              if (alpha <= 0.0F)
                return;
              const float spin = t * 6.2832F * 1.2F * speed;  // ~6 turns at speed 5
              const float latScale = std::cos(spin);
              const float bob = 0.05F * H;  // relevé, held on pointe
              for (int i = 0; i < kN; ++i)
              {
                const float cx = w * ((i + 0.5F) / kN);
                drawSkeleton(dst,
                             w,
                             h,
                             ya,
                             cx,
                             floorY,
                             H,
                             bob,
                             0.0F,
                             aL,
                             aR,
                             lL,
                             lR,
                             boneTint(src, w, h, cx, alpha),
                             Rgb{u8(18 * alpha), u8(12 * alpha), u8(12 * alpha), false},
                             latScale);
              }
            });
}

// Fall to pieces: the skeletons stand, then their bones detach (staggered) and
// tumble under gravity into heaps on the floor.
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

// Macarena: the row runs the arm sequence — out front, cross to the opposite
// shoulder, hands behind the head, hands to the hips — with the left arm one
// count behind the right, a hip sway, and a little quarter-turn hop at the end
// of every eight-count.
void effectMacarena(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  constexpr int kN = 5;
  const float floorY = (h - 1) * 0.94F;
  const float H = h * 0.58F;
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  // One arm's landmark positions, in order (right arm; the left arm uses the
  // mirror image, one count behind). {elbow.x, elbow.y, hand.x, hand.y} as
  // offsets from the shoulder in H units; +x is this arm's own side.
  static const std::array<Limb, 8> kArm = {{
      {0.18F, -0.02F, 0.42F, -0.02F},   // 0: out to the side, level (palm down)
      {0.18F, -0.02F, 0.42F, -0.02F},   // 1: hold (palm turns up)
      {0.02F, 0.02F, -0.20F, 0.02F},    // 2: hand crosses to the opposite shoulder
      {0.02F, 0.02F, -0.20F, 0.02F},    // 3: hold
      {0.06F, -0.18F, -0.06F, -0.34F},  // 4: hand behind the head
      {0.06F, -0.18F, -0.06F, -0.34F},  // 5: hold
      {-0.02F, 0.16F, -0.12F, 0.40F},   // 6: hand to the opposite hip
      {-0.02F, 0.16F, -0.12F, 0.40F},   // 7: hold
  }};
  auto mirror = [](const Limb& a) { return Limb{-a.ex, a.ey, -a.hx, a.hy}; };
  const Limb lL{-0.01F, 0.23F, 0.02F, 0.46F};
  const Limb lR{0.01F, 0.23F, -0.02F, 0.46F};
  runFrames(renderer,
            w,
            h,
            6000,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float alpha = skeletonStage(dst, src, w, h, t, static_cast<int>(floorY));
              if (alpha <= 0.0F)
                return;
              const float seg = 0.16F / speed;  // per-count hold time
              const float step = t / seg;
              const int idx = static_cast<int>(step) % 8;
              const int prev = (idx + 7) % 8;
              const float blend = std::clamp((step - std::floor(step)) / 0.3F, 0.0F, 1.0F);
              // Right arm steps through kArm; the left arm trails by one count.
              const Limb aR = lerpLimb(kArm[prev], kArm[idx], blend);
              const Limb aL = mirror(lerpLimb(kArm[(prev + 7) % 8], kArm[(idx + 7) % 8], blend));
              // A quarter-turn hop at the end of each eight-count.
              const float cyc = t / (8.0F * seg);
              const float frac = cyc - std::floor(cyc);
              const float jump = std::clamp((frac - 0.85F) / 0.15F, 0.0F, 1.0F);
              const float quarters = std::floor(cyc) + jump;
              // Quarter-turn facing, but floored so a side-on count stays a
              // narrow silhouette instead of collapsing to an invisible line.
              const float c = std::cos(quarters * 1.5708F);
              const float latScale = (c < 0.0F ? -1.0F : 1.0F) * (0.4F + 0.6F * std::fabs(c));
              const float hop = std::sin(jump * 3.14159F) * 0.06F * H;
              const float bob = -hop + std::sin(t * 9.0F * speed) * 0.02F * H;
              const float sway = std::sin(t * 4.5F * speed) * 0.03F * (w / static_cast<float>(kN));
              for (int i = 0; i < kN; ++i)
              {
                const float cx = w * ((i + 0.5F) / kN) + sway;
                drawSkeleton(dst,
                             w,
                             h,
                             ya,
                             cx,
                             floorY,
                             H,
                             bob,
                             0.0F,
                             aL,
                             aR,
                             lL,
                             lR,
                             boneTint(src, w, h, cx, alpha),
                             Rgb{u8(18 * alpha), u8(12 * alpha), u8(12 * alpha), false},
                             latScale);
              }
            });
}

// Neo: one big skeleton takes the iconic "bring it" ready stance — side-on to
// the opponent (off to the right), front leg bent and rear leg pushed back, the
// near arm pointing at the opponent and the far arm raised — held steady,
// against falling green digital rain, eyes glowing Matrix-green.
void effectNeo(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float floorY = (h - 1) * 0.96F;
  const float H = h * 0.86F;  // one large figure, near full height
  const float cx = w * 0.5F;
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  // Deep, low stance: both knees splay strongly outward — past the feet — and
  // drop low so he is almost kneeling; feet stay planted at the floor (the deep
  // kneel itself comes from the large negative bob below). Knee is the widest
  // point of each leg; the shin angles back inward to the planted foot.
  const Limb lR{0.40F, 0.14F, 0.30F, 0.24F};    // front leg: knee out to the right
  const Limb lL{-0.42F, 0.16F, -0.36F, 0.24F};  // rear leg: knee out to the left, pushed back
  // Near (right) arm points at the opponent — long reach, angled slightly down
  // so it reads apart from the shoulder line; far (left) arm raised up and out,
  // clear of the head.
  const Limb aR{0.22F, 0.04F, 0.62F, 0.08F};
  const Limb aL{-0.16F, -0.16F, -0.30F, -0.42F};
  runFrames(
      renderer,
      w,
      h,
      5200,
      [&](float t, std::vector<Rgb>& dst)
      {
        // Dim the view almost to black for the void behind the rain.
        const float vf = std::clamp(1.0F - t * 4.5F, 0.0F, 1.0F) * 0.25F;
        for (std::size_t k = 0; k < dst.size(); ++k)
        {
          const Rgb& s0 = src[k];
          dst[k] = s0.transparent ? Rgb{0, 0, 0, false}
                                  : Rgb{u8(s0.r * vf), u8(s0.g * vf), u8(s0.b * vf), false};
        }
        // Green digital rain: one stream every other sub-column, each with its
        // own phase and speed, a bright head and a fading tail.
        const float tail = h * 0.28F;
        for (int xc = 0; xc < w; xc += 2)
        {
          const float ph = hash(xc * 3 + 1);
          const float spd = 1.4F + hash(xc * 7 + 5) * 2.2F;
          const float headY = std::fmod(ph + t * spd, 1.0F) * (h + tail) - tail;
          for (int d = 0; d < static_cast<int>(tail); ++d)
          {
            const int y = static_cast<int>(headY) - d;
            if (y < 0 || y >= h)
              continue;
            const float in = 1.0F - d / tail;
            const Rgb c = (d == 0) ? Rgb{180, 255, 190, false}
                                   : Rgb{u8(20 * in), u8(70 + 175 * in), u8(40 * in), false};
            dst[static_cast<std::size_t>(y) * w + xc] = c;
          }
        }
        // Faint dojo floor.
        const int fy = static_cast<int>(floorY);
        if (fy >= 0 && fy < h)
          for (int x = 0; x < w; ++x)
            dst[static_cast<std::size_t>(fy) * w + x] = Rgb{0, 60, 24, false};
        const float alpha = std::clamp((t - 0.10F) / 0.13F, 0.0F, 1.0F);
        if (alpha <= 0.0F)
          return;
        // Turn side-on to the opponent as the stance is taken, then hold.
        const float turn = std::clamp((t - 0.15F) / 0.20F, 0.0F, 1.0F);
        const float latScale = 1.0F - 0.42F * turn;  // 1.0 facing us -> 0.58 strongly side-on
        // A slight kneel plus quiet breathing — no other motion.
        const float bob = -0.22F * H + std::sin(t * 6.0F) * 0.008F * H;  // deep kneel + breathing
        const Rgb bone = boneTint(src, w, h, cx, alpha);
        const Rgb eye{u8(40 * alpha), u8(255 * alpha), u8(80 * alpha), false};  // Matrix-green
        drawSkeleton(dst, w, h, ya, cx, floorY, H, bob, 0.0F, aL, aR, lL, lR, bone, eye, latScale);
      });
}

// The Truman Show: the painted-sky world dims, a staircase climbs to a small
// lit EXIT door; Truman walks up, gives his little bow and steps through — then
// the doorway's darkness floods out and swallows the screen.
void effectTruman(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float doorCx = w * 0.66F;
  const float doorSill = h * 0.44F;  // top of the stairs == door base
  const float doorW = std::max(6.0F, w * 0.14F);
  const float doorH = std::max(8.0F, h * 0.22F);
  const float dtop = doorSill - doorH;
  const float stairX0 = w * 0.15F, stairY0 = h * 0.95F;  // foot of the stairs
  const float stairX1 = doorCx - doorW * 0.5F;
  constexpr int kSteps = 7;
  const int txtScale = std::max(1, static_cast<int>(std::lround(w / 130.0F)));
  runFrames(
      renderer,
      w,
      h,
      5800,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float dim = std::clamp(1.0F - t * 1.6F, 0.20F, 1.0F);
        for (int y = 0; y < h; ++y)
        {
          const bool sea = y > h * 0.62F;
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float b = s.transparent ? 90.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            const float l = b * dim;
            dst[static_cast<std::size_t>(y) * w + x] =
                sea ? Rgb{u8(8 + l * 0.10F), u8(26 + l * 0.16F), u8(52 + l * 0.20F), false}
                    : Rgb{u8(70 + l * 0.18F), u8(118 + l * 0.20F), u8(178 + l * 0.20F), false};
          }
        }
        auto fillRect = [&](float x0, float y0, float x1, float y1, const Rgb& c)
        {
          for (int y = std::max(0, static_cast<int>(y0));
               y <= std::min(h - 1, static_cast<int>(y1));
               ++y)
            for (int x = std::max(0, static_cast<int>(x0));
                 x <= std::min(w - 1, static_cast<int>(x1));
                 ++x)
              dst[static_cast<std::size_t>(y) * w + x] = c;
        };
        for (int i = 0; i < kSteps; ++i)  // staircase
        {
          const float f0 = static_cast<float>(i) / kSteps;
          const float f1 = static_cast<float>(i + 1) / kSteps;
          const float sx = stairX0 + (stairX1 - stairX0) * f0;
          const float sx2 = stairX0 + (stairX1 - stairX0) * f1;
          const float sy = stairY0 + (doorSill - stairY0) * f1;
          fillRect(sx, sy, sx2 + 1, stairY0, Rgb{150, 146, 140, false});
          fillRect(sx, sy, sx2 + 1, sy + std::max(1.0F, h * 0.012F), Rgb{212, 208, 198, false});
        }
        const float open = std::clamp((t - 0.58F) / 0.16F, 0.0F, 1.0F);
        const float dl = doorCx - doorW * 0.5F, dr = doorCx + doorW * 0.5F;
        fillRect(dl - 2, dtop - 2, dr + 2, doorSill, Rgb{236, 232, 220, false});  // lit frame
        fillRect(dl,
                 dtop,
                 dr,
                 doorSill,
                 Rgb{u8(118 * (1 - open)), u8(150 * (1 - open)), u8(196 * (1 - open)), false});
        auto stampText =
            [&](const std::string& str, float leftX, float topY, int scale, const Rgb& c)
        {
          constexpr int kFW = 5, kFH = 7, kGap = 1;
          for (int ci = 0; ci < static_cast<int>(str.size()); ++ci)
          {
            const auto g =
                glyph5x7(static_cast<char>(std::toupper(static_cast<unsigned char>(str[ci]))));
            const int cox = ci * (kFW + kGap) * scale;
            for (int fy = 0; fy < kFH; ++fy)
              for (int fx = 0; fx < kFW; ++fx)
              {
                if (g[fy][fx] != '1')
                  continue;
                for (int sy = 0; sy < scale; ++sy)
                  for (int sx = 0; sx < scale; ++sx)
                  {
                    const int x = static_cast<int>(leftX) + cox + fx * scale + sx;
                    const int y = static_cast<int>(topY) + fy * scale + sy;
                    if (x >= 0 && x < w && y >= 0 && y < h)
                      dst[static_cast<std::size_t>(y) * w + x] = c;
                  }
              }
          }
        };
        const float signW = 4 * 6 * txtScale - txtScale;
        stampText("EXIT",
                  doorCx - signW * 0.5F,
                  dtop - 7 * txtScale - 2,
                  txtScale,
                  Rgb{40, 255, 90, false});
        auto drawPerson = [&](float cx, float feetY, float ht, float lean, const Rgb& c)
        {
          const float headR = ht * 0.13F;
          const float hipY = feetY - ht * 0.46F;
          const float neckY = feetY - ht * 0.78F;
          const float lx = lean * ht;
          drawSeg(dst, w, h, cx, hipY, cx + lx, neckY, ht * 0.09F, ya, c);
          plotDot(dst, w, h, cx + lx, neckY - headR, headR, ya, c);
          drawSeg(dst, w, h, cx, hipY, cx - ht * 0.11F, feetY, ht * 0.07F, ya, c);
          drawSeg(dst, w, h, cx, hipY, cx + ht * 0.11F, feetY, ht * 0.07F, ya, c);
          const float shY = neckY + ht * 0.06F;
          drawSeg(dst, w, h, cx + lx * 0.8F, shY, cx - ht * 0.10F + lx, hipY, ht * 0.06F, ya, c);
          drawSeg(dst,
                  w,
                  h,
                  cx + lx * 0.8F,
                  shY,
                  cx + ht * 0.14F + lx,
                  hipY - ht * 0.02F,
                  ht * 0.06F,
                  ya,
                  c);
        };
        float fx, fyFeet, ht = h * 0.15F, lean = 0.0F;
        bool drawFig = true;
        if (t < 0.50F)
        {
          const float wf = std::clamp((t - 0.06F) / 0.44F, 0.0F, 1.0F);
          fx = (stairX0 + 10) + (doorCx - (stairX0 + 10)) * wf;
          fyFeet = (stairY0 - 4) + ((doorSill - 2) - (stairY0 - 4)) * wf;
          lean = 0.14F + std::sin(t * 40.0F) * 0.03F;  // trudging up
        }
        else if (t < 0.62F)
        {
          fx = doorCx;
          fyFeet = doorSill - 2;
          lean = 0.10F +
                 std::sin(std::clamp((t - 0.50F) / 0.12F, 0.0F, 1.0F) * 3.14159F) * 0.5F;  // bow
        }
        else
        {
          fx = doorCx;
          fyFeet = doorSill - 2;
          const float ef = std::clamp((t - 0.62F) / 0.18F, 0.0F, 1.0F);
          ht = h * 0.15F * (1.0F - 0.6F * ef);  // step into the dark
          if (ef >= 1.0F)
            drawFig = false;
        }
        if (drawFig)
          drawPerson(fx, fyFeet, ht, lean, Rgb{16, 20, 30, false});
        const float flood = std::clamp((t - 0.80F) / 0.20F, 0.0F, 1.0F);
        if (flood > 0.0F)
          plotDot(dst,
                  w,
                  h,
                  doorCx,
                  dtop + doorH * 0.5F,
                  flood * std::hypot(static_cast<float>(w), static_cast<float>(h)),
                  ya,
                  Rgb{0, 0, 0, false});
      });
}

// A Trip to the Moon (Méliès, 1902): the view fades to a starry night, a great
// pale Moon-face fades in, and a rocket flies in from the left and lands smack
// in its right eye — which scrunches around the embedded capsule.
void effectMoonRocket(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float moonR = 0.33F * mn;
  const float moonCx = w * 0.52F, moonCy = h * 0.46F;
  const float eyeRx = moonCx + moonR * 0.40F, eyeRy = moonCy - moonR * 0.16F;
  const float eyeLx = moonCx - moonR * 0.40F, eyeLy = moonCy - moonR * 0.16F;
  const float hitT = 0.64F;
  runFrames(
      renderer,
      w,
      h,
      5200,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float nf = std::clamp(1.0F - t * 3.0F, 0.0F, 1.0F);
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 0.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b) * nf;
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(6 + l * 0.05F), u8(8 + l * 0.05F), u8(20 + l * 0.07F), false};
          }
        for (int i = 0; i < 70; ++i)
        {
          const int sx = static_cast<int>(hash(i * 2) * w),
                    sy = static_cast<int>(hash(i * 2 + 1) * h);
          if (sx < 0 || sx >= w || sy < 0 || sy >= h)
            continue;
          const float tw = 0.45F + 0.55F * std::sin(t * 8.0F + i);
          dst[static_cast<std::size_t>(sy) * w + sx] =
              Rgb{u8(170 * tw), u8(170 * tw), u8(195 * tw), false};
        }
        const float mi = std::clamp(t * 5.0F, 0.0F, 1.0F);
        plotDot(dst,
                w,
                h,
                moonCx,
                moonCy,
                moonR,
                ya,
                Rgb{u8(238 * mi), u8(232 * mi), u8(196 * mi), false});
        const bool hit = t >= hitT;
        const float wince = std::clamp((t - hitT) / 0.10F, 0.0F, 1.0F);
        const Rgb feat{u8(120 * mi), u8(94 * mi), u8(64 * mi), false};
        auto drawRocket = [&](float x, float y, float scl)
        {
          const float L = moonR * 0.46F * scl, R = moonR * 0.085F * scl;
          const Rgb body{u8(205 * mi), u8(205 * mi), u8(212 * mi), false};
          const Rgb nose{u8(150 * mi), u8(150 * mi), u8(160 * mi), false};
          for (int i = 0; i <= 14; ++i)
          {
            const float f = i / 14.0F, along = (f - 0.5F) * L;
            const float q = (f - 0.42F) / 0.5F;
            float rad = R * std::sqrt(std::max(0.0F, 1.0F - q * q));
            if (f > 0.82F)
              rad *= (1.0F - (f - 0.82F) / 0.18F);
            plotDot(dst, w, h, x + along, y, std::max(1.0F, rad), ya, f > 0.80F ? nose : body);
          }
          drawSeg(dst,
                  w,
                  h,
                  x - L * 0.5F,
                  y,
                  x - L * 0.60F,
                  y - R * 1.7F,
                  std::max(1.0F, R * 0.35F),
                  ya,
                  body);
          drawSeg(dst,
                  w,
                  h,
                  x - L * 0.5F,
                  y,
                  x - L * 0.60F,
                  y + R * 1.7F,
                  std::max(1.0F, R * 0.35F),
                  ya,
                  body);
        };
        plotDot(dst, w, h, eyeLx, eyeLy, moonR * 0.10F, ya, feat);
        drawSeg(dst,
                w,
                h,
                eyeLx - moonR * 0.14F,
                eyeLy - moonR * 0.20F,
                eyeLx + moonR * 0.10F,
                eyeLy - moonR * 0.16F,
                std::max(1.0F, moonR * 0.035F),
                ya,
                feat);
        drawSeg(dst,
                w,
                h,
                moonCx,
                moonCy - moonR * 0.02F,
                moonCx - moonR * 0.05F,
                moonCy + moonR * 0.16F,
                std::max(1.0F, moonR * 0.045F),
                ya,
                feat);
        {
          const float my = moonCy + moonR * 0.42F, curve = hit ? -0.12F : 0.10F;
          for (int k = -5; k <= 5; ++k)
          {
            const float fxn = k / 5.0F;
            plotDot(dst,
                    w,
                    h,
                    moonCx + fxn * moonR * 0.26F,
                    my - curve * moonR * (1.0F - fxn * fxn),
                    std::max(1.0F, moonR * 0.03F),
                    ya,
                    feat);
          }
        }
        if (!hit)
        {
          plotDot(dst, w, h, eyeRx, eyeRy, moonR * 0.10F, ya, feat);
          drawSeg(dst,
                  w,
                  h,
                  eyeRx - moonR * 0.10F,
                  eyeRy - moonR * 0.16F,
                  eyeRx + moonR * 0.14F,
                  eyeRy - moonR * 0.20F,
                  std::max(1.0F, moonR * 0.035F),
                  ya,
                  feat);
          const float rp = std::clamp((t - 0.14F) / (hitT - 0.14F), 0.0F, 1.0F);
          const float rx = -0.12F * w + (eyeRx - (-0.12F * w)) * rp;
          const float ry =
              h * 0.10F + (eyeRy - h * 0.10F) * rp - std::sin(rp * 3.14159F) * moonR * 0.55F;
          drawRocket(rx, ry, 1.0F);
        }
        else
        {
          const Rgb gunk{u8(48 * mi), u8(34 * mi), u8(24 * mi), false};
          plotDot(dst, w, h, eyeRx, eyeRy, moonR * (0.12F + 0.10F * wince), ya, gunk);
          for (int k = 0; k < 8; ++k)
          {
            const float a = k / 8.0F * 6.2832F, dd = moonR * 0.22F * wince;
            plotDot(dst,
                    w,
                    h,
                    eyeRx + std::cos(a) * dd,
                    eyeRy + std::sin(a) * dd,
                    std::max(1.0F, moonR * 0.035F),
                    ya,
                    gunk);
          }
          drawSeg(dst,
                  w,
                  h,
                  eyeRx - moonR * 0.16F,
                  eyeRy - moonR * 0.10F,
                  eyeRx + moonR * 0.10F,
                  eyeRy - moonR * 0.24F,
                  std::max(1.0F, moonR * 0.035F),
                  ya,
                  feat);
          drawRocket(eyeRx - moonR * 0.16F, eyeRy - moonR * 0.04F, 0.9F);
          if (wince < 0.4F)
          {
            const float fl = 1.0F - wince / 0.4F;
            plotDot(dst,
                    w,
                    h,
                    eyeRx,
                    eyeRy,
                    moonR * (0.10F + 0.5F * fl),
                    ya,
                    Rgb{u8(255 * fl), u8(240 * fl), u8(180 * fl), false});
          }
        }
      });
}

// Cinema Paradiso / a film burning in the projector gate: the frame weaves and
// flickers, then the celluloid blisters from a point — a ragged brown-and-
// orange burn front eats outward to a blown-out white, which fades to black.
void effectFilmBurn(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float ox = w * 0.40F, oy = h * 0.60F;
  const float maxR = std::hypot(std::max(ox, w - ox), std::max(oy * ya, (h - oy) * ya)) + 4.0F;
  auto noise = [](float a)
  {
    return 1.0F + 0.13F * std::sin(a * 6.0F + 1.3F) + 0.07F * std::sin(a * 13.0F) +
           0.05F * std::sin(a * 23.0F + 0.7F);
  };
  runFrames(
      renderer,
      w,
      h,
      4400,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float weave = std::sin(t * 55.0F) * 1.3F + std::sin(t * 12.0F) * 0.6F;
        const float flick = 0.82F + 0.18F * std::sin(t * 80.0F) * std::sin(t * 37.0F);
        const float burn = std::clamp((t - 0.16F) / 0.66F, 0.0F, 1.0F);
        const float R = burn * burn * maxR;
        const float edge = 0.05F * maxR + 1.0F, heatBand = 0.14F * maxR;
        const float whiteOut = std::clamp((t - 0.82F) / 0.10F, 0.0F, 1.0F);
        const float toBlack = std::clamp((t - 0.92F) / 0.08F, 0.0F, 1.0F);
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb s = sample(src, w, h, static_cast<float>(x), static_cast<float>(y) + weave);
            float r = s.transparent ? 14.0F : s.r, g = s.transparent ? 16.0F : s.g,
                  b = s.transparent ? 22.0F : s.b;
            r *= flick;
            g *= flick;
            b *= flick;
            const float dx = x - ox, dy = (y - oy) * ya;
            const float d = std::hypot(dx, dy);
            const float rr = R * noise(std::atan2(dy, dx));
            Rgb out;
            if (d < rr - edge)
            {
              const float wv = 232.0F * flick;
              out = Rgb{u8(wv + 20), u8(wv + 8), u8(wv - 22), false};
            }
            else if (d < rr)
            {
              const float e = (rr - d) / edge;
              out = Rgb{255, u8(110 + 125 * e), u8(15 + 85 * e), false};
            }
            else
            {
              const float heat = std::clamp(1.0F - (d - rr) / heatBand, 0.0F, 1.0F);
              out = Rgb{u8(r * (1 - heat) + 72 * heat),
                        u8(g * (1 - heat) + 40 * heat),
                        u8(b * (1 - heat) + 18 * heat),
                        false};
              if (heat > 0.82F)
              {
                const float k = (heat - 0.82F) / 0.18F;
                out = Rgb{u8(out.r * (1 - k)), u8(out.g * (1 - k)), u8(out.b * (1 - k)), false};
              }
            }
            if (whiteOut > 0.0F)
              out = Rgb{u8(out.r + (255 - out.r) * whiteOut),
                        u8(out.g + (255 - out.g) * whiteOut),
                        u8(out.b + (255 - out.b) * whiteOut),
                        false};
            if (toBlack > 0.0F)
              out = Rgb{u8(out.r * (1 - toBlack)),
                        u8(out.g * (1 - toBlack)),
                        u8(out.b * (1 - toBlack)),
                        false};
            dst[static_cast<std::size_t>(y) * w + x] = out;
          }
      });
}

// E.T.: the view fades to a starlit night with a great pale Moon, and a
// bike-and-rider silhouette (E.T. in the front basket) glides across the sky,
// passing in front of the Moon.
void effectET(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float moonR = 0.30F * mn;
  const float moonCx = w * 0.60F, moonCy = h * 0.40F;
  runFrames(renderer,
            w,
            h,
            5200,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float nf = std::clamp(1.0F - t * 3.0F, 0.0F, 1.0F);
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l =
                      s.transparent ? 0.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b) * nf;
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(8 + l * 0.05F), u8(12 + l * 0.06F), u8(28 + l * 0.08F), false};
                }
              for (int i = 0; i < 60; ++i)
              {
                const int sx = static_cast<int>(hash(i * 2) * w),
                          sy = static_cast<int>(hash(i * 2 + 1) * h * 0.85F);
                if (sx < 0 || sx >= w || sy < 0 || sy >= h)
                  continue;
                const float tw = 0.55F + 0.45F * std::sin(t * 7.0F + i);
                dst[static_cast<std::size_t>(sy) * w + sx] =
                    Rgb{u8(185 * tw), u8(185 * tw), u8(205 * tw), false};
              }
              const float mi = std::clamp(t * 4.0F, 0.0F, 1.0F);
              plotDot(dst,
                      w,
                      h,
                      moonCx,
                      moonCy,
                      moonR * 1.16F,
                      ya,
                      Rgb{u8(38 * mi), u8(42 * mi), u8(58 * mi), false});
              plotDot(dst,
                      w,
                      h,
                      moonCx,
                      moonCy,
                      moonR,
                      ya,
                      Rgb{u8(236 * mi), u8(232 * mi), u8(206 * mi), false});
              plotDot(dst,
                      w,
                      h,
                      moonCx - moonR * 0.30F,
                      moonCy - moonR * 0.22F,
                      moonR * 0.16F,
                      ya,
                      Rgb{u8(208 * mi), u8(204 * mi), u8(180 * mi), false});
              plotDot(dst,
                      w,
                      h,
                      moonCx + moonR * 0.26F,
                      moonCy + moonR * 0.18F,
                      moonR * 0.12F,
                      ya,
                      Rgb{u8(210 * mi), u8(206 * mi), u8(182 * mi), false});
              auto drawBike = [&](float cx, float cy, float s, const Rgb& c)
              {
                const float rw = 0.30F * s, th = std::max(1.0F, s * 0.05F);
                auto ring = [&](float wx)
                {
                  for (int k = 0; k < 28; ++k)
                  {
                    const float a = k / 28.0F * 6.2832F;
                    plotDot(dst,
                            w,
                            h,
                            wx + std::cos(a) * rw,
                            cy + std::sin(a) * rw,
                            std::max(1.0F, s * 0.045F),
                            ya,
                            c);
                  }
                };
                const float rearX = cx - 0.72F * s, frontX = cx + 0.72F * s;
                ring(rearX);
                ring(frontX);
                const float seatX = cx - 0.28F * s, seatY = cy - 0.60F * s;
                const float barX = cx + 0.55F * s, barY = cy - 0.52F * s;
                drawSeg(dst, w, h, rearX, cy, cx, cy, th, ya, c);
                drawSeg(dst, w, h, cx, cy, seatX, seatY, th, ya, c);
                drawSeg(dst, w, h, rearX, cy, seatX, seatY, th, ya, c);
                drawSeg(dst, w, h, seatX, seatY, barX, barY, th, ya, c);
                drawSeg(dst, w, h, cx, cy, barX, barY, th, ya, c);
                drawSeg(dst, w, h, barX, barY, frontX, cy, th, ya, c);
                const float shX = cx + 0.16F * s, shY = cy - 1.00F * s;
                drawSeg(dst, w, h, seatX, seatY, shX, shY, s * 0.08F, ya, c);
                plotDot(dst, w, h, cx + 0.32F * s, cy - 1.14F * s, s * 0.13F, ya, c);
                drawSeg(dst, w, h, shX, shY, barX, barY, s * 0.05F, ya, c);
                drawSeg(dst, w, h, seatX, seatY, cx, cy, s * 0.05F, ya, c);
                plotDot(dst, w, h, frontX, cy - 0.40F * s, s * 0.13F, ya, c);
                plotDot(dst, w, h, frontX + 0.02F * s, cy - 0.58F * s, s * 0.09F, ya, c);
              };
              const float bp = std::clamp((t - 0.06F) / 0.86F, 0.0F, 1.0F);
              const float bx = (-0.18F + 1.36F * bp) * w;
              const float by = h * 0.62F - std::sin(bp * 3.14159F) * h * 0.24F - bp * h * 0.05F;
              drawBike(bx, by, 0.085F * mn, Rgb{6, 8, 16, false});
            });
}

// Thelma & Louise: the desert dims in, a convertible (two heads, arms up) races
// across the mesa, launches off the cliff edge and arcs out over the canyon —
// where the image freeze-frames and bleaches to white.
void effectThelma(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float cliffX = w * 0.46F, groundY = h * 0.72F;
  const float startX = w * 0.05F, endX = w * 0.74F, arcH = h * 0.18F;
  const float freezeT = 0.74F;
  runFrames(
      renderer,
      w,
      h,
      5000,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float dim = std::clamp(1.0F - t * 1.8F, 0.30F, 1.0F);
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l =
                (s.transparent ? 60.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b)) * dim;
            Rgb c;
            if (y < groundY)  // sky: deep blue up top -> orange at the horizon
            {
              const float sf = y / groundY;
              c = Rgb{u8(40 + 200 * sf + l * 0.10F),
                      u8(50 + 110 * sf + l * 0.10F),
                      u8(110 - 40 * sf + l * 0.10F),
                      false};
            }
            else if (x <= cliffX)  // mesa
              c = Rgb{u8(150 + l * 0.20F), u8(90 + l * 0.15F), u8(50 + l * 0.10F), false};
            else  // canyon void
              c = Rgb{u8(28 + l * 0.06F), u8(20 + l * 0.05F), u8(18 + l * 0.05F), false};
            dst[static_cast<std::size_t>(y) * w + x] = c;
          }
        const float p = std::clamp(t / freezeT, 0.0F, 1.0F);
        const float carX = startX + (endX - startX) * p * p;
        float carY = groundY, tilt = 0.0F;
        if (carX > cliffX)
        {
          const float a = (carX - cliffX) / (endX - cliffX);
          carY = groundY - arcH * (1.3F * a - 0.45F * a * a);
          tilt = 0.55F * a;
        }
        if (carX <= cliffX)  // dust trail on the mesa
          for (int k = 1; k <= 5; ++k)
            plotDot(dst,
                    w,
                    h,
                    carX - k * mn * 0.05F,
                    groundY + mn * 0.01F,
                    mn * 0.02F * k,
                    ya,
                    Rgb{u8(150 - k * 12), u8(120 - k * 10), u8(90 - k * 8), false});
        const float s = 0.085F * mn;
        const Rgb car{20, 18, 26, false};
        const float bw = 1.5F * s, bh = 0.45F * s;
        drawSeg(dst, w, h, carX - bw, carY, carX + bw, carY - tilt * bw, 0.42F * s, ya, car);
        plotDot(dst, w, h, carX - bw * 0.7F, carY + bh, 0.34F * s, ya, Rgb{14, 14, 18, false});
        plotDot(dst,
                w,
                h,
                carX + bw * 0.7F,
                carY - tilt * bw * 0.7F + bh,
                0.34F * s,
                ya,
                Rgb{14, 14, 18, false});
        drawSeg(dst,
                w,
                h,
                carX,
                carY - bh,
                carX + bw * 0.5F,
                carY - tilt * bw - bh * 1.8F,
                0.12F * s,
                ya,
                car);
        plotDot(dst, w, h, carX - bw * 0.3F, carY - bh * 1.7F, 0.22F * s, ya, car);
        plotDot(dst, w, h, carX + bw * 0.1F, carY - tilt * bw - bh * 1.9F, 0.22F * s, ya, car);
        if (t > freezeT)  // freeze-frame bleach to white
        {
          const float bl = std::clamp((t - freezeT) / 0.22F, 0.0F, 1.0F) * 0.9F;
          for (std::size_t k = 0; k < dst.size(); ++k)
            dst[k] = Rgb{u8(dst[k].r + (255 - dst[k].r) * bl),
                         u8(dst[k].g + (250 - dst[k].g) * bl),
                         u8(dst[k].b + (235 - dst[k].b) * bl),
                         false};
        }
      });
}

// Back to the Future: the scene smears with speed, a DeLorean recedes to a
// vanishing point, a blinding flash — and it is gone, leaving two burning tire
// trails that blaze and die.
void effectDeLorean(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float vanX = w * 0.5F, vanY = h * 0.46F, roadY = h * 0.78F;
  const float startCX = w * 0.32F, flashT = 0.55F;
  runFrames(
      renderer,
      w,
      h,
      4400,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float speed = std::clamp(t / flashT, 0.0F, 1.0F);
        const float smear = speed * mn * 0.14F;
        const float vdim = 1.0F - 0.55F * speed;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb a = sample(src, w, h, x - smear, static_cast<float>(y));
            const Rgb b = sample(src, w, h, static_cast<float>(x), static_cast<float>(y));
            const float ar = a.transparent ? 10 : a.r, ag = a.transparent ? 12 : a.g,
                        ab = a.transparent ? 18 : a.b;
            const float br = b.transparent ? 10 : b.r, bg = b.transparent ? 12 : b.g,
                        bb = b.transparent ? 18 : b.b;
            dst[static_cast<std::size_t>(y) * w + x] = Rgb{u8((ar * 0.5F + br * 0.5F) * vdim),
                                                           u8((ag * 0.5F + bg * 0.5F) * vdim),
                                                           u8((ab * 0.5F + bb * 0.5F) * vdim),
                                                           false};
          }
        if (t < flashT)  // DeLorean receding (we see its tail)
        {
          const float p = t / flashT;
          const float cs = 0.16F * mn * (1.0F - 0.8F * p);
          const float ccx = startCX + (vanX - startCX) * p, ccy = roadY + (vanY - roadY) * p;
          drawSeg(dst, w, h, ccx - cs, ccy, ccx + cs, ccy, cs * 0.5F, ya, Rgb{30, 30, 40, false});
          drawSeg(dst,
                  w,
                  h,
                  ccx - cs * 0.7F,
                  ccy - cs * 0.5F,
                  ccx + cs * 0.7F,
                  ccy - cs * 0.5F,
                  cs * 0.28F,
                  ya,
                  Rgb{40, 40, 52, false});
          plotDot(dst, w, h, ccx - cs * 0.6F, ccy, cs * 0.2F, ya, Rgb{255, 40, 30, false});
          plotDot(dst, w, h, ccx + cs * 0.6F, ccy, cs * 0.2F, ya, Rgb{255, 40, 30, false});
        }
        const float flash = std::exp(-std::pow((t - flashT) / 0.05F, 2.0F));
        if (t > flashT)  // two burning tire trails fanning toward the camera
        {
          const float ft = std::clamp((t - flashT) / 0.45F, 0.0F, 1.0F);
          for (int side = -1; side <= 1; side += 2)
            for (int k = 0; k <= 20; ++k)
            {
              const float f = k / 20.0F;
              const float tx = vanX + side * (mn * 0.02F + f * mn * 0.22F);
              const float tyy = vanY + (roadY - vanY) * f;
              const float flick = 0.6F + 0.4F * std::sin(t * 40.0F + k);
              const float life = (1.0F - ft) * flick * (0.4F + 0.6F * f);
              if (life < 0.05F)
                continue;
              plotDot(dst,
                      w,
                      h,
                      tx,
                      tyy,
                      mn * 0.025F * life + 0.6F,
                      ya,
                      Rgb{u8(255 * life), u8((150 + 80 * hash(k)) * life), u8(20 * life), false});
            }
        }
        if (flash > 0.01F)
          for (std::size_t k = 0; k < dst.size(); ++k)
            dst[k] = Rgb{u8(dst[k].r + (255 - dst[k].r) * flash),
                         u8(dst[k].g + (255 - dst[k].g) * flash),
                         u8(dst[k].b + (255 - dst[k].b) * flash),
                         false};
      });
}

// Up: the view dims into a daytime sky, a little house gathers in the centre,
// and a great bouquet of balloons sprouts from its roof and carries it up and
// off the top of the screen, swaying as it goes.
void effectUp(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float hw = 0.12F * mn, hh = 0.12F * mn;
  static const Rgb kBalloon[6] = {{235, 60, 60, false},
                                  {245, 180, 40, false},
                                  {70, 150, 235, false},
                                  {80, 200, 90, false},
                                  {200, 90, 210, false},
                                  {240, 240, 90, false}};
  runFrames(
      renderer,
      w,
      h,
      5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float vf = std::clamp(1.0F - t * 2.5F, 0.0F, 1.0F) * 0.30F;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 0.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b) * vf;
            const float sf = static_cast<float>(y) / h;
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(120 + 70 * sf + l), u8(170 + 50 * sf + l), u8(225 - 10 * sf + l), false};
          }
        const float form = std::clamp(t / 0.30F, 0.0F, 1.0F);
        const float rise = (t < 0.32F) ? 0.0F : std::pow((t - 0.32F) / 0.68F, 1.6F);
        const float liftY = -rise * (h * 1.4F);
        const float sway =
            std::sin(t * 3.0F) * h * 0.03F * std::clamp((t - 0.30F) / 0.2F, 0.0F, 1.0F);
        const float hx = w * 0.5F + sway, hy = h * 0.66F + liftY;
        const float nb = std::clamp((t - 0.18F) / 0.20F, 0.0F, 1.0F);
        const int balloons = static_cast<int>(nb * 34);
        const float bcx = hx, bcy = hy - hh * 1.1F - mn * 0.18F;
        for (int i = 0; i < 4; ++i)  // strings
          drawSeg(dst,
                  w,
                  h,
                  hx + (i - 1.5F) * hw * 0.3F,
                  hy - hh,
                  bcx + (hash(i) - 0.5F) * mn * 0.1F,
                  bcy,
                  std::max(1.0F, mn * 0.006F),
                  ya,
                  Rgb{210, 210, 210, false});
        for (int i = 0; i < balloons; ++i)
        {
          const float a = hash(i * 3) * 6.2832F, rr = std::sqrt(hash(i * 3 + 1)) * mn * 0.16F;
          const float bx = bcx + std::cos(a) * rr, by = bcy + std::sin(a) * rr * 0.9F;
          plotDot(dst, w, h, bx, by, mn * 0.03F, ya, kBalloon[i % 6]);
        }
        const float s = form;
        auto fillRect = [&](float x0, float y0, float x1, float y1, const Rgb& c)
        {
          for (int y = std::max(0, static_cast<int>(y0));
               y <= std::min(h - 1, static_cast<int>(y1));
               ++y)
            for (int x = std::max(0, static_cast<int>(x0));
                 x <= std::min(w - 1, static_cast<int>(x1));
                 ++x)
              dst[static_cast<std::size_t>(y) * w + x] = c;
        };
        const float bwd = hw * s, bht = hh * s;
        fillRect(hx - bwd, hy - bht, hx + bwd, hy, Rgb{225, 210, 175, false});
        for (int y = 0; y <= static_cast<int>(bht * 0.9F); ++y)  // roof triangle
        {
          const float frac = 1.0F - y / (bht * 0.9F);
          const float rwd = bwd * 1.15F * frac;
          const float ry = hy - bht - y;
          fillRect(hx - rwd, ry, hx + rwd, ry + 1, Rgb{170, 70, 55, false});
        }
        if (s > 0.5F)
        {
          fillRect(
              hx - bwd * 0.22F, hy - bht * 0.5F, hx + bwd * 0.22F, hy, Rgb{120, 80, 50, false});
          fillRect(hx - bwd * 0.7F,
                   hy - bht * 0.75F,
                   hx - bwd * 0.4F,
                   hy - bht * 0.45F,
                   Rgb{250, 240, 150, false});
          fillRect(hx + bwd * 0.4F,
                   hy - bht * 0.75F,
                   hx + bwd * 0.7F,
                   hy - bht * 0.45F,
                   Rgb{250, 240, 150, false});
        }
      });
}

// Academy countdown leader: an aged, flickering film-leader frame with a big
// ring, crosshairs and a sweeping clock hand, counting 5-4-3-2-1, then a cue
// dot and black.
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

// Looney Tunes iris-out: a circular iris closes in from the edges down to a dot
// at the centre, the rest going to black — "That's all, folks."
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

// Lawrence of Arabia: a match burns in the dark, is blown out — and on the cut
// a vast desert sunrise blazes up, the sun climbing over the horizon.
void effectLawrence(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float cutT = 0.46F, horizonY = h * 0.60F;
  runFrames(
      renderer,
      w,
      h,
      5000,
      [&](float t, std::vector<Rgb>& dst)
      {
        if (t < cutT)  // the match in the dark
        {
          const float flame = 1.0F - std::clamp((t - (cutT - 0.12F)) / 0.12F, 0.0F, 1.0F);
          const float fx = w * 0.5F + std::sin(t * 40.0F) * w * 0.004F, fy = h * 0.5F;
          for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
              const float gd = std::hypot(x - fx, (y - fy) * ya);
              const float glow = flame * std::exp(-gd / (0.18F * mn));
              dst[static_cast<std::size_t>(y) * w + x] =
                  Rgb{u8(10 + glow * 230), u8(8 + glow * 150), u8(6 + glow * 60), false};
            }
          drawSeg(dst,
                  w,
                  h,
                  fx,
                  fy + h * 0.02F,
                  w * 0.46F,
                  h * 0.80F,
                  mn * 0.012F,
                  ya,
                  Rgb{90, 60, 35, false});
          if (flame > 0.02F)
          {
            const float fl = flame * (0.9F + 0.1F * std::sin(t * 60.0F));
            plotDot(dst, w, h, fx, fy, mn * 0.05F * fl, ya, Rgb{240, 130, 30, false});
            plotDot(
                dst, w, h, fx, fy - h * 0.018F * fl, mn * 0.03F * fl, ya, Rgb{255, 210, 90, false});
            plotDot(dst,
                    w,
                    h,
                    fx,
                    fy - h * 0.03F * fl,
                    mn * 0.014F * fl,
                    ya,
                    Rgb{255, 250, 230, false});
          }
        }
        else  // the desert sunrise
        {
          const float sr = std::clamp((t - cutT) / 0.5F, 0.0F, 1.0F);
          const float sunX = w * 0.5F, sunY = horizonY + h * 0.12F - sr * h * 0.24F;
          for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
              const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
              const float l = s.transparent ? 70.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
              const float sd = std::hypot(x - sunX, (y - sunY) * ya);
              const float glow = std::exp(-sd / (0.45F * mn));
              Rgb c;
              if (y < horizonY)  // sky
              {
                const float sf = y / horizonY;  // 0 at top -> 1 at horizon
                c = Rgb{u8(55 + 200 * sf + 70 * glow),
                        u8(70 + 100 * sf + 80 * glow),
                        u8(140 - 70 * sf + 50 * glow),
                        false};
              }
              else  // desert dunes from the view luma
                c = Rgb{u8(120 + l * 0.25F + 60 * glow),
                        u8(70 + l * 0.18F + 40 * glow),
                        u8(35 + l * 0.10F),
                        false};
              dst[static_cast<std::size_t>(y) * w + x] = c;
            }
          plotDot(dst, w, h, sunX, sunY, 0.17F * mn, ya, Rgb{255, 232, 165, false});
          plotDot(dst, w, h, sunX, sunY, 0.12F * mn, ya, Rgb{255, 250, 225, false});
        }
      });
}

// Jurassic Park: the surface of the view shivers in concentric ripples and the
// frame jolts with each approaching footstep — THUD… THUD… — building until it
// arrives and the screen goes dark.
void effectJurassic(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float cx = w * 0.5F, cy = h * 0.42F;
  const float mn = std::min(static_cast<float>(w), h * ya);
  constexpr int kThuds = 6;
  runFrames(renderer,
            w,
            h,
            5200,
            [&](float t, std::vector<Rgb>& dst)
            {
              float shake = 0.0F;
              for (int i = 0; i < kThuds; ++i)
              {
                const float ti = (i + 1.0F) / (kThuds + 1.0F);
                if (t > ti)
                {
                  const float dt = t - ti, amp = 0.25F + 0.75F * i / (kThuds - 1.0F);
                  shake += amp * std::exp(-dt * 26.0F) * std::sin(dt * 90.0F);
                }
              }
              const float shakePx = shake * h * 0.05F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const float dx = x - cx, dy = (y - cy) * ya, d = std::hypot(dx, dy);
                  float disp = 0.0F;
                  for (int i = 0; i < kThuds; ++i)
                  {
                    const float ti = (i + 1.0F) / (kThuds + 1.0F);
                    if (t > ti)
                    {
                      const float age = t - ti, ringR = age * mn * 2.4F;
                      const float amp = 0.15F + 0.85F * i / (kThuds - 1.0F);
                      const float band = std::exp(-std::pow((d - ringR) / (0.12F * mn), 2.0F));
                      disp += amp * band * std::sin((d - ringR) * 0.7F) * mn * 0.05F;
                    }
                  }
                  const float ux = d > 0.1F ? dx / d : 0.0F, uy = d > 0.1F ? dy / d : 0.0F;
                  const Rgb s = sample(src, w, h, x + ux * disp, y + uy * disp / ya + shakePx);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      s.transparent ? Rgb{8, 10, 14, false} : s;
                }
              const float endDark = std::clamp((t - 0.93F) / 0.07F, 0.0F, 1.0F);
              if (endDark > 0.0F)
                for (std::size_t k = 0; k < dst.size(); ++k)
                  dst[k] = Rgb{u8(dst[k].r * (1 - endDark)),
                               u8(dst[k].g * (1 - endDark)),
                               u8(dst[k].b * (1 - endDark)),
                               false};
            });
}

// The Good, the Bad and the Ugly: a Mexican standoff in extreme close-up — the
// cut jumps between three gunmen's darting eyes, faster and tighter, until a
// muzzle flash whites out and cuts to black.
void effectStandoff(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  static const Rgb kSkin[3] = {
      {185, 142, 100, false}, {150, 120, 96, false}, {205, 172, 132, false}};
  runFrames(
      renderer,
      w,
      h,
      5000,
      [&](float t, std::vector<Rgb>& dst)
      {
        const int cut = static_cast<int>(t * t * 17.0F);
        const int who = cut % 3;
        const float zoom = 1.0F + t * 1.7F;
        const Rgb skin = kSkin[who];
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const float vg = 1.0F - 0.4F * std::hypot((x - w * 0.5F) / w, (y - h * 0.5F) / h);
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(skin.r * vg), u8(skin.g * vg), u8(skin.b * vg), false};
          }
        const float browY = h * (0.34F - 0.05F * t);
        const float eyeY = h * 0.54F;
        const float eyeDX = w * 0.18F / std::max(1.0F, zoom * 0.6F);
        const float eyeR = mn * 0.10F * zoom * 0.6F;
        const float dart = std::sin(t * 26.0F + who * 2.0F) * eyeR * 0.9F;
        // heavy brow shadow
        for (int y = 0; y < static_cast<int>(browY); ++y)
          for (int x = 0; x < w; ++x)
          {
            const float k = 1.0F - static_cast<float>(y) / browY * 0.6F;
            Rgb& p = dst[static_cast<std::size_t>(y) * w + x];
            p = Rgb{u8(p.r * (1 - 0.7F * k)),
                    u8(p.g * (1 - 0.7F * k)),
                    u8(p.b * (1 - 0.7F * k)),
                    false};
          }
        for (int s = -1; s <= 1; s += 2)
        {
          const float ex = w * 0.5F + s * eyeDX;
          drawSeg(dst,
                  w,
                  h,
                  ex - eyeR,
                  eyeY,
                  ex + eyeR,
                  eyeY,
                  eyeR * 0.55F,
                  ya,
                  Rgb{235, 230, 220, false});  // eye white
          plotDot(dst, w, h, ex + dart, eyeY, eyeR * 0.5F, ya, Rgb{70, 45, 30, false});   // iris
          plotDot(dst, w, h, ex + dart, eyeY, eyeR * 0.22F, ya, Rgb{10, 10, 10, false});  // pupil
        }
        if (t > 0.88F)  // muzzle flash, then black
        {
          const float f = (t - 0.88F) / 0.12F;
          if (f < 0.45F)
          {
            const float wf = f / 0.45F;
            for (std::size_t k = 0; k < dst.size(); ++k)
              dst[k] = Rgb{u8(dst[k].r + (255 - dst[k].r) * wf),
                           u8(dst[k].g + (255 - dst[k].g) * wf),
                           u8(dst[k].b + (255 - dst[k].b) * wf),
                           false};
          }
          else
            for (std::size_t k = 0; k < dst.size(); ++k)
              dst[k] = Rgb{0, 0, 0, false};
        }
      });
}

// Jaws: the view sinks to a dim sea, a dorsal fin carves slowly across to the
// quickening two-note pulse (the water reddening on each beat), then it
// submerges and the surface stills.
void effectJaws(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float surfaceY = h * 0.5F;
  runFrames(
      renderer,
      w,
      h,
      5200,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float beat = std::pow(t, 1.6F) * 30.0F;
        const float pulse = std::pow(0.5F + 0.5F * std::sin(beat), 3.0F);
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 0.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            const float shim = 0.5F + 0.5F * std::sin(x * 0.3F + t * 6.0F);
            const float depth = std::clamp((y - surfaceY) / (h * 0.5F), 0.0F, 1.0F);
            dst[static_cast<std::size_t>(y) * w + x] = Rgb{u8(10 + l * 0.05F + pulse * 40 * depth),
                                                           u8(30 + l * 0.06F + shim * 14),
                                                           u8(60 + l * 0.08F),
                                                           false};
          }
        const float fp = std::clamp(t / 0.85F, 0.0F, 1.0F);
        const float finX = -0.1F * w + 1.2F * w * fp;
        const float submerge = std::clamp((t - 0.80F) / 0.18F, 0.0F, 1.0F);
        const float finH = mn * 0.13F * (1.0F - submerge);
        if (finH > 1.0F)
        {
          for (int k = 1; k <= 6; ++k)  // V-wake foam behind (moving right)
          {
            const float f = k / 6.0F;
            const Rgb foam{u8(180 * (1 - f)), u8(200 * (1 - f)), u8(210 * (1 - f)), false};
            plotDot(dst,
                    w,
                    h,
                    finX - f * mn * 0.22F,
                    surfaceY - f * finH * 0.5F,
                    mn * 0.012F,
                    ya,
                    foam);
            plotDot(dst,
                    w,
                    h,
                    finX - f * mn * 0.22F,
                    surfaceY + f * finH * 0.5F,
                    mn * 0.012F,
                    ya,
                    foam);
          }
          const float ax = finX + mn * 0.06F, ay = surfaceY;  // fin: leaned-back triangle
          const float bx = finX - mn * 0.08F, by = surfaceY;
          const float ttx = finX - mn * 0.02F, tty = surfaceY - finH;
          const int x0 = static_cast<int>(finX - mn * 0.10F),
                    x1 = static_cast<int>(finX + mn * 0.08F);
          const int y0 = static_cast<int>(surfaceY - finH - 1), y1 = static_cast<int>(surfaceY + 1);
          for (int y = std::max(0, y0); y <= std::min(h - 1, y1); ++y)
            for (int x = std::max(0, x0); x <= std::min(w - 1, x1); ++x)
              if (inTri(static_cast<float>(x), static_cast<float>(y), ax, ay, bx, by, ttx, tty))
                dst[static_cast<std::size_t>(y) * w + x] = Rgb{18, 22, 30, false};
        }
        const float endDark = std::clamp((t - 0.90F) / 0.10F, 0.0F, 1.0F);
        if (endDark > 0.0F)
          for (std::size_t k = 0; k < dst.size(); ++k)
            dst[k] = Rgb{u8(dst[k].r * (1 - endDark)),
                         u8(dst[k].g * (1 - endDark)),
                         u8(dst[k].b * (1 - endDark)),
                         false};
      });
}

// 2001's Star Gate: a slit-scan corridor of vivid colour streaks rushing past
// toward a blazing white core — "My God, it's full of stars."
void effectStarGate(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hsv = [&](float hh, float s, float v) -> Rgb
  {
    hh -= std::floor(hh);
    const float H = hh * 6.0F;
    const int i = static_cast<int>(H);
    const float f = H - i, p = v * (1 - s), q = v * (1 - s * f), tt = v * (1 - s * (1 - f));
    float r, g, b;
    switch (i % 6)
    {
      case 0:
        r = v;
        g = tt;
        b = p;
        break;
      case 1:
        r = q;
        g = v;
        b = p;
        break;
      case 2:
        r = p;
        g = v;
        b = tt;
        break;
      case 3:
        r = p;
        g = q;
        b = v;
        break;
      case 4:
        r = tt;
        g = p;
        b = v;
        break;
      default:
        r = v;
        g = p;
        b = q;
        break;
    }
    return Rgb{u8(r * 255), u8(g * 255), u8(b * 255), false};
  };
  const float cx = w * 0.5F, cy = h * 0.5F;
  const float mn = std::min(static_cast<float>(w), h * ya);
  runFrames(renderer,
            w,
            h,
            5200,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float speed = 1.0F + t * 3.0F;
              const float onset = std::clamp(t * 3.0F, 0.0F, 1.0F);
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const float dx = x - cx, dy = (y - cy) * ya, r = std::hypot(dx, dy);
                  const float depth = (mn * 0.5F) / (r + 1.0F);
                  const float ang = std::atan2(dy, dx);
                  const float hue = depth * 0.5F + ang * 0.16F - t * speed * 0.5F;
                  const float flick = 0.7F + 0.3F * std::sin(depth * 22.0F - t * speed * 6.0F);
                  const float v = std::clamp(0.18F + depth * 0.9F, 0.0F, 1.0F) * flick * onset;
                  Rgb c = (r < mn * 0.05F) ? Rgb{255, 255, 255, false} : hsv(hue, 0.85F, v);
                  dst[static_cast<std::size_t>(y) * w + x] = c;
                }
            });
}

// Close Encounters: a night ridge, the five tones flash in coloured lights, and
// the mothership rises from behind the hills, glowing brighter until its light
// fills the screen.
void effectCloseEncounters(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float ridgeY = h * 0.74F;
  static const Rgb kTone[5] = {{235, 80, 80, false},
                               {245, 160, 50, false},
                               {240, 235, 90, false},
                               {90, 210, 110, false},
                               {90, 140, 240, false}};
  static const int kSeq[8] = {2, 3, 1, 0, 4, 2, 3, 1};  // a five-tone-ish phrase
  runFrames(
      renderer,
      w,
      h,
      5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float rise = std::clamp((t - 0.50F) / 0.45F, 0.0F, 1.0F);
        const float shipCy = ridgeY + mn * 0.30F - rise * mn * 0.42F;
        const float shipR = mn * 0.42F;
        const float halo = rise;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const float ridge =
                ridgeY + std::sin(x * 0.10F) * h * 0.02F + std::sin(x * 0.031F + 1.0F) * h * 0.03F;
            const float gd = std::hypot(x - w * 0.5F, (y - shipCy) * ya);
            const float g = halo * std::exp(-gd / (0.7F * mn)) * 0.6F;
            Rgb c;
            if (y > ridge)
              c = Rgb{u8(12 + g * 200), u8(10 + g * 200), u8(14 + g * 160), false};  // dark ridge
            else
            {
              const float sf = y / ridgeY;
              c = Rgb{u8(10 + 18 * sf + g * 220),
                      u8(12 + 22 * sf + g * 220),
                      u8(26 + 40 * sf + g * 180),
                      false};
            }
            dst[static_cast<std::size_t>(y) * w + x] = c;
          }
        for (int i = 0; i < 70; ++i)  // stars
        {
          const int sx = static_cast<int>(hash(i * 2) * w),
                    sy = static_cast<int>(hash(i * 2 + 1) * ridgeY);
          if (sx >= 0 && sx < w && sy >= 0 && sy < h)
            dst[static_cast<std::size_t>(sy) * w + sx] = Rgb{150, 150, 175, false};
        }
        const int step = static_cast<int>(t * 14.0F);  // tone sequence
        const float local = t * 14.0F - step;
        const int lit = kSeq[step % 8];
        const float env = std::sin(std::clamp(local, 0.0F, 1.0F) * 3.14159F);
        for (int i = 0; i < 5; ++i)  // five coloured tone-lights on the ridge
        {
          const float px = w * (0.22F + 0.14F * i), py = ridgeY - mn * 0.02F;
          const float on = (i == lit) ? env : 0.12F;
          plotDot(dst,
                  w,
                  h,
                  px,
                  py,
                  mn * 0.055F,
                  ya,
                  Rgb{u8(kTone[i].r * on), u8(kTone[i].g * on), u8(kTone[i].b * on), false});
        }
        if (rise > 0.0F)  // the mothership
        {
          plotDot(dst, w, h, w * 0.5F, shipCy, shipR, ya, Rgb{30, 32, 40, false});
          plotDot(dst,
                  w,
                  h,
                  w * 0.5F,
                  shipCy,
                  shipR * 0.34F,
                  ya,
                  Rgb{u8(220 * halo + 35), u8(225 * halo + 35), u8(255 * halo + 45), false});
          for (int i = 0; i < 26; ++i)  // studded lights around the rim
          {
            const float a = i / 26.0F * 3.14159F;  // lower hemisphere
            const float lx = w * 0.5F + std::cos(a) * shipR * 0.86F;
            const float lyy = shipCy + std::sin(a) * shipR * 0.86F / ya * ya;
            const float fl = 0.5F + 0.5F * std::sin(t * 12.0F + i);
            const Rgb& k = kTone[i % 5];
            plotDot(dst,
                    w,
                    h,
                    lx,
                    lyy,
                    mn * 0.012F,
                    ya,
                    Rgb{u8(k.r * fl), u8(k.g * fl), u8(k.b * fl), false});
          }
        }
        const float fin = std::clamp((t - 0.93F) / 0.07F, 0.0F, 1.0F);
        if (fin > 0.0F)
          for (std::size_t k = 0; k < dst.size(); ++k)
            dst[k] = Rgb{u8(dst[k].r + (255 - dst[k].r) * fin),
                         u8(dst[k].g + (255 - dst[k].g) * fin),
                         u8(dst[k].b + (255 - dst[k].b) * fin),
                         false};
      });
}

// Titanic: the view tilts as the stern rises, then the whole ship slides down
// and under into a dark, star-cold sea.
void effectTitanic(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  const float pivotX = w * 0.5F, pivotY = h * 0.92F, waterY = h * 0.66F;
  runFrames(
      renderer,
      w,
      h,
      5600,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float ang = std::pow(t, 1.3F) * 1.05F;  // tilt, stern rising
        const float sink = std::pow(std::clamp((t - 0.28F) / 0.72F, 0.0F, 1.0F), 1.4F) * h * 1.15F;
        const float ca = std::cos(-ang), sa = std::sin(-ang);
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const float rx = x - pivotX, ry = (y - sink) - pivotY;
            const float sxv = pivotX + (rx * ca - ry * sa);
            const float syv = pivotY + (rx * sa + ry * ca);
            const Rgb s = sample(src, w, h, sxv, syv);
            Rgb out;
            if (!s.transparent && sxv >= 0 && sxv < w && syv >= 0 && syv < h)
            {
              if (y > waterY)  // ship gone under: deep, dark, blue
              {
                const float dd = std::clamp((y - waterY) / (h - waterY), 0.0F, 1.0F);
                out = Rgb{u8(s.r * 0.10F * (1 - dd)),
                          u8(s.g * 0.14F * (1 - dd) + 8),
                          u8(s.b * 0.20F * (1 - dd) + 18),
                          false};
              }
              else
                out = Rgb{u8(s.r * 0.55F), u8(s.g * 0.55F), u8(s.b * 0.6F), false};  // night ship
            }
            else if (y > waterY)  // sea
            {
              const float dd = (y - waterY) / (h - waterY);
              out =
                  Rgb{u8(6 + 6 * (1 - dd)), u8(12 + 10 * (1 - dd)), u8(28 + 18 * (1 - dd)), false};
              if (hash(x * 13 + y * 7) > 0.992F)
                out = Rgb{60, 70, 90, false};  // debris / froth fleck
            }
            else  // night sky
            {
              out = Rgb{6, 8, 18, false};
              if (hash(x * 7 + y * 131) > 0.985F)
                out = Rgb{150, 150, 180, false};  // star
            }
            dst[static_cast<std::size_t>(y) * w + x] = out;
          }
      });
}

// Inception: the view gathers into a spinning top (its body wrapped from the
// data), set spinning on a dark table; it wobbles more and more — and we cut to
// black before we ever learn whether it falls.
void effectInception(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float cx = w * 0.5F, tableY = h * 0.66F;
  runFrames(
      renderer,
      w,
      h,
      5200,
      [&](float t, std::vector<Rgb>& dst)
      {
        for (int y = 0; y < h; ++y)  // dim the data into a shadowy room + tabletop
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float dim = (y > tableY) ? 0.22F : 0.34F;
            dst[static_cast<std::size_t>(y) * w + x] =
                s.transparent ? Rgb{10, 10, 14, false}
                              : Rgb{u8(s.r * dim), u8(s.g * dim), u8(s.b * dim + 4), false};
          }
        const float form = std::clamp(t / 0.22F, 0.0F, 1.0F);
        const float bodyR = mn * 0.12F * form;
        const float spin = t * t * 80.0F;
        const float wob = std::pow(std::clamp((t - 0.40F) / 0.55F, 0.0F, 1.0F), 1.5F);
        const float prec = t * 13.0F;
        const float pivotX = cx + std::sin(prec) * wob * mn * 0.07F;
        const float tiltX = std::sin(prec) * wob * bodyR * 0.9F;
        const float bodyCx = pivotX + tiltX, bodyCy = tableY - bodyR * 1.25F;
        if (bodyR > 1.0F)
        {
          const int x0 = static_cast<int>(std::min(bodyCx - bodyR, pivotX) - 1);
          const int x1 = static_cast<int>(std::max(bodyCx + bodyR, pivotX) + 1);
          for (int y = static_cast<int>(bodyCy); y <= static_cast<int>(tableY); ++y)
            for (int x = std::max(0, x0); x <= std::min(w - 1, x1); ++x)
              if (inTri(static_cast<float>(x),
                        static_cast<float>(y),
                        bodyCx - bodyR * 0.5F,
                        bodyCy,
                        bodyCx + bodyR * 0.5F,
                        bodyCy,
                        pivotX,
                        tableY))
              {
                const Rgb s = sample(src, w, h, x, y);
                dst[static_cast<std::size_t>(y) * w + x] =
                    s.transparent ? Rgb{40, 40, 48, false}
                                  : Rgb{u8(s.r * 0.5F), u8(s.g * 0.5F), u8(s.b * 0.5F), false};
              }
          drawSphere(dst, w, h, src, bodyCx, bodyCy, bodyR, bodyR, 0.85F, spin, 0.92F, 0.92F, 1.0F);
          drawSeg(dst,
                  w,
                  h,
                  bodyCx,
                  bodyCy - bodyR,
                  bodyCx + tiltX * 0.2F,
                  bodyCy - bodyR * 1.4F,
                  std::max(1.0F, bodyR * 0.08F),
                  ya,
                  Rgb{180, 180, 190, false});
        }
        if (t > 0.93F)  // the smash cut to black
          for (std::size_t k = 0; k < dst.size(); ++k)
            dst[k] = Rgb{0, 0, 0, false};
      });
}

// Vertigo: the view is dragged into a hypnotic Saul-Bass log-spiral while a
// dolly-zoom pushes and pulls it, red spiral arms sweeping through — then it
// winds down to nothing.
void effectVertigo(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float cx = w * 0.5F, cy = h * 0.5F;
  runFrames(
      renderer,
      w,
      h,
      5200,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float rot = t * 3.5F;
        const float twist = (1.5F + t * 7.0F) * 0.1F;
        const float zoom = 1.0F + 0.5F * std::sin(t * 3.0F) + t * 0.9F;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const float dx = x - cx, dy = (y - cy) * ya, r = std::hypot(dx, dy);
            const float th = std::atan2(dy, dx);
            const float th2 = th + rot + std::log(r + 1.0F) * twist;
            const float r2 = r / zoom;
            const Rgb s = sample(src, w, h, cx + std::cos(th2) * r2, cy + std::sin(th2) * r2 / ya);
            const float arm = std::sin(th * 6.0F + std::log(r + 1.0F) * 4.0F - t * 6.0F);
            const float red = std::clamp(arm, 0.0F, 1.0F) * 0.5F * std::clamp(1.3F - t, 0.0F, 1.0F);
            const float br = s.transparent ? 10 : s.r, bg = s.transparent ? 10 : s.g,
                        bb = s.transparent ? 14 : s.b;
            dst[static_cast<std::size_t>(y) * w + x] = Rgb{u8(br + (255 - br) * red * 0.7F),
                                                           u8(bg * (1 - red * 0.5F)),
                                                           u8(bb * (1 - red * 0.5F)),
                                                           false};
          }
        const float fin = std::clamp((t - 0.90F) / 0.10F, 0.0F, 1.0F);
        if (fin > 0.0F)
          for (std::size_t k = 0; k < dst.size(); ++k)
            dst[k] = Rgb{u8(dst[k].r * (1 - fin)),
                         u8(dst[k].g * (1 - fin)),
                         u8(dst[k].b * (1 - fin)),
                         false};
      });
}

// Psycho: the data spirals down the shower drain — swirling ever faster toward
// the centre as a black plughole opens and swallows it.
void effectPsycho(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float cx = w * 0.5F, cy = h * 0.5F, mn = std::min(static_cast<float>(w), h * ya);
  runFrames(renderer,
            w,
            h,
            4800,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float pull = 1.0F + t * 0.5F;
              const float swirl = 0.8F + t * 3.2F;
              const float holeR = std::pow(t, 1.9F) * mn * 0.58F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const float dx = x - cx, dy = (y - cy) * ya, r = std::hypot(dx, dy);
                  if (r < holeR)
                  {
                    dst[static_cast<std::size_t>(y) * w + x] = Rgb{0, 0, 0, false};
                    continue;
                  }
                  const float th = std::atan2(dy, dx) - swirl / (r / mn + 0.18F);
                  const float r2 = r * pull;
                  const Rgb s =
                      sample(src, w, h, cx + std::cos(th) * r2, cy + std::sin(th) * r2 / ya);
                  const float fade = std::clamp((r - holeR) / (mn * 0.25F), 0.0F, 1.0F);
                  const float br = s.transparent ? 6 : s.r, bg = s.transparent ? 8 : s.g,
                              bb = s.transparent ? 12 : s.b;
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(br * fade), u8(bg * fade), u8(bb * fade), false};
                }
            });
}

// Forrest Gump: a feather — tinted by the weather behind it — drifts and tumbles
// on the breeze across a pale sky, settling at the bottom of the frame.
void effectFeather(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  runFrames(renderer,
            w,
            h,
            5600,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float vf = std::clamp(1.0F - t * 1.5F, 0.12F, 1.0F);
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 200.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  const float sf = static_cast<float>(y) / h;
                  dst[static_cast<std::size_t>(y) * w + x] = Rgb{u8(196 - 26 * sf + l * 0.10F * vf),
                                                                 u8(208 - 16 * sf + l * 0.10F * vf),
                                                                 u8(226 + l * 0.08F * vf),
                                                                 false};
                }
              const float fall = std::pow(t, 1.1F);
              const float fx = w * (0.5F + 0.24F * std::sin(t * 2.2F));
              const float fy = h * (0.06F + 0.86F * fall);
              const float rot = 0.5F + std::sin(t * 2.6F) * 0.6F;
              const float L = mn * 0.42F, maxW = mn * 0.085F;
              const float ca = std::cos(rot), sa = std::sin(rot);
              for (int i = 0; i <= static_cast<int>(L); ++i)
              {
                const float sps = i / L - 0.5F;  // -0.5 (tip) .. 0.5 (base)
                const float halfw = maxW * std::sin((sps + 0.5F) * 3.14159F);
                for (int j = -static_cast<int>(halfw); j <= static_cast<int>(halfw); ++j)
                {
                  const float lx = static_cast<float>(j), ly = sps * L;
                  const bool rachis = std::fabs(lx) < std::max(1.0F, maxW * 0.12F);
                  const bool gap = (!rachis) &&
                                   (static_cast<int>(ly * 0.9F + std::fabs(lx) * 1.3F) % 3 == 0) &&
                                   std::fabs(lx) > halfw * 0.35F;
                  if (gap)
                    continue;  // feathery barb gaps at the vane edge
                  const float px = fx + lx * ca - ly * sa;
                  const float py = fy + (lx * sa + ly * ca) / ya;
                  if (px < 0 || px >= w || py < 0 || py >= h)
                    continue;
                  const Rgb d = sample(src, w, h, px, py);
                  const float dr = d.transparent ? 210 : d.r, dg = d.transparent ? 210 : d.g,
                              db = d.transparent ? 220 : d.b;
                  dst[static_cast<std::size_t>(py) * w + static_cast<std::size_t>(px)] =
                      rachis ? Rgb{205, 200, 188, false}
                             : Rgb{u8(250 * 0.86F + dr * 0.14F),
                                   u8(250 * 0.86F + dg * 0.14F),
                                   u8(252 * 0.86F + db * 0.14F),
                                   false};
                }
              }
            });
}

// The Red Balloon: the view gathers into a single red balloon (its skin wrapped
// from the data), which lifts off and dwindles up into a pale sky, its string
// trailing.
void effectRedBalloon(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  runFrames(
      renderer,
      w,
      h,
      5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float vf = std::clamp(1.0F - t * 1.6F, 0.15F, 1.0F);
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 0.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b) * vf;
            const float sf = static_cast<float>(y) / h;
            dst[static_cast<std::size_t>(y) * w + x] = Rgb{u8(182 - 20 * sf + l * 0.12F),
                                                           u8(196 - 14 * sf + l * 0.12F),
                                                           u8(216 + l * 0.10F),
                                                           false};
          }
        const float form = std::clamp(t / 0.22F, 0.0F, 1.0F);
        const float rise = std::clamp((t - 0.20F) / 0.80F, 0.0F, 1.0F);
        const float r = mn * 0.16F * form * (1.0F - 0.6F * rise);
        const float bx = w * 0.5F + std::sin(t * 2.0F) * mn * 0.06F * rise;
        const float by = h * 0.60F - rise * h * 0.95F;
        if (r > 1.0F)
        {
          for (int k = 1; k <= 8; ++k)  // dangling wavy string
          {
            const float sy = by + r * 1.15F + k * r * 0.22F;
            const float sx = bx + std::sin(k * 0.7F + t * 3.0F) * r * 0.18F;
            plotDot(dst, w, h, sx, sy, std::max(1.0F, r * 0.05F), ya, Rgb{60, 50, 50, false});
          }
          drawSphere(dst, w, h, src, bx, by, r, r * 1.12F, 0.85F, t * 0.5F, 1.0F, 0.32F, 0.30F);
          plotDot(dst, w, h, bx, by + r * 1.06F, r * 0.13F, ya, Rgb{150, 30, 30, false});
        }
      });
}

// Mary Poppins: a prim figure rises into the clouds beneath an open umbrella —
// whose canopy is woven from the data — swaying gently as it climbs.
void effectMaryPoppins(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  const float mn = std::min(static_cast<float>(w), h * ya);
  runFrames(
      renderer,
      w,
      h,
      5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        for (int y = 0; y < h; ++y)  // sky + drifting clouds (from data)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 0.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            const float sf = static_cast<float>(y) / h;
            const float cloud = std::clamp((l / 255.0F - 0.45F) * 2.0F, 0.0F, 1.0F) * 0.5F;
            dst[static_cast<std::size_t>(y) * w + x] = Rgb{u8(135 + 70 * sf + cloud * 110),
                                                           u8(170 + 55 * sf + cloud * 90),
                                                           u8(220 - 10 * sf + cloud * 40),
                                                           false};
          }
        const float rise = std::clamp((t - 0.10F) / 0.90F, 0.0F, 1.0F);
        const float sway = std::sin(t * 2.2F) * mn * 0.04F;
        const float cxp = w * 0.5F + sway;
        const float cyp = h * 0.72F - rise * h * 0.92F;  // canopy rim height
        const float r = mn * 0.20F;
        const int x0 = static_cast<int>(cxp - r), x1 = static_cast<int>(cxp + r);
        const int y0 = static_cast<int>(cyp - r * 0.85F), y1 = static_cast<int>(cyp);
        for (int y = std::max(0, y0); y <= std::min(h - 1, y1); ++y)
          for (int x = std::max(0, x0); x <= std::min(w - 1, x1); ++x)
          {
            const float nx = (x - cxp) / r, ny = (y - cyp) / (r * 0.85F);
            if (ny > 0 || nx * nx + ny * ny > 1.0F)
              continue;                                         // upper half-dome only
            const float scallop = 0.06F * std::sin(nx * 9.0F);  // scalloped rim
            if (ny > -scallop - 0.02F && y > cyp - 2)
              continue;
            const Rgb s = sample(src, w, h, x, y);
            const float dr = s.transparent ? 120 : s.r, dg = s.transparent ? 120 : s.g,
                        db = s.transparent ? 130 : s.b;
            const bool rib = std::fabs(std::fmod(std::atan2(-ny, nx) * 6.0F, 1.0F)) < 0.12F;
            dst[static_cast<std::size_t>(y) * w + x] =
                rib ? Rgb{40, 30, 40, false}
                    : Rgb{u8(dr * 0.55F + 30), u8(dg * 0.55F + 20), u8(db * 0.55F + 30), false};
          }
        drawSeg(dst,
                w,
                h,
                cxp,
                cyp - r * 0.85F,
                cxp,
                cyp - r * 1.0F,
                std::max(1.0F, mn * 0.01F),
                ya,
                Rgb{60, 50, 40, false});  // ferrule
        drawSeg(dst,
                w,
                h,
                cxp,
                cyp,
                cxp,
                cyp + r * 0.9F,
                std::max(1.0F, mn * 0.012F),
                ya,
                Rgb{90, 60, 40, false});  // handle shaft
        drawSeg(dst,
                w,
                h,
                cxp,
                cyp + r * 0.9F,
                cxp + r * 0.18F,
                cyp + r * 0.95F,
                std::max(1.0F, mn * 0.012F),
                ya,
                Rgb{90, 60, 40, false});                                 // crook
        const float fcx = cxp, ftop = cyp + r * 0.55F, fh = mn * 0.16F;  // the figure
        const Rgb coat{30, 32, 48, false};
        plotDot(dst, w, h, fcx, ftop, fh * 0.16F, ya, coat);  // head/hat
        drawSeg(dst, w, h, fcx, ftop + fh * 0.16F, fcx, ftop + fh * 0.7F, fh * 0.12F, ya, coat);
        drawSeg(
            dst, w, h, fcx, ftop + fh * 0.7F, fcx - fh * 0.06F, ftop + fh, fh * 0.07F, ya, coat);
        drawSeg(
            dst, w, h, fcx, ftop + fh * 0.7F, fcx + fh * 0.06F, ftop + fh, fh * 0.07F, ya, coat);
        drawSeg(dst,
                w,
                h,
                fcx,
                ftop + fh * 0.32F,
                cxp,
                cyp + r * 0.9F,
                fh * 0.05F,
                ya,
                coat);  // arm to handle
        plotDot(dst,
                w,
                h,
                fcx + fh * 0.22F,
                ftop + fh * 0.5F,
                fh * 0.12F,
                ya,
                Rgb{120, 80, 40, false});  // carpet bag
        (void)hash;
      });
}

// Star Trek: "punch it." The view's bright points become stars that streak
// outward faster and faster, then a flash snaps the ship to warp.
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

// 2001's monolith: the data settles to a still lunar twilight, a black slab
// rises from the surface, and the sun climbs behind it with the crescent moon
// aligned above — a blaze of light at the conjunction.
void effectMonolith(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float horizonY = h * 0.74F, monoCx = w * 0.5F;
  runFrames(renderer,
            w,
            h,
            5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float rise = std::clamp((t - 0.10F) / 0.42F, 0.0F, 1.0F);
              const float align = std::clamp((t - 0.52F) / 0.40F, 0.0F, 1.0F);
              const float sunY = horizonY - h * 0.10F - align * h * 0.34F;
              const float flare = std::pow(std::clamp((t - 0.88F) / 0.12F, 0.0F, 1.0F), 0.6F);
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 40.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  const float gd = std::hypot(x - monoCx, (y - sunY) * ya);
                  const float glow = std::exp(-gd / (0.5F * mn)) * (0.3F + 0.7F * align);
                  Rgb c;
                  if (y > horizonY)  // dim lunar ground from the data
                    c = Rgb{u8(28 + l * 0.18F + glow * 60),
                            u8(26 + l * 0.16F + glow * 60),
                            u8(30 + l * 0.14F + glow * 50),
                            false};
                  else  // dark sky
                    c = Rgb{u8(6 + glow * 230), u8(7 + glow * 210), u8(16 + glow * 150), false};
                  dst[static_cast<std::size_t>(y) * w + x] = c;
                }
              plotDot(dst,
                      w,
                      h,
                      monoCx,
                      sunY,
                      mn * 0.13F,
                      ya,
                      Rgb{u8(255), u8(238), u8(180 + 60 * flare), false});  // sun
              const float crY = sunY - mn * 0.36F;                          // crescent above
              plotDot(dst, w, h, monoCx, crY, mn * 0.075F, ya, Rgb{220, 222, 210, false});
              plotDot(dst,
                      w,
                      h,
                      monoCx + mn * 0.028F,
                      crY - mn * 0.008F,
                      mn * 0.07F,
                      ya,
                      Rgb{8, 9, 18, false});
              const float mh = rise * h * 0.56F, mw = mn * 0.055F;  // the black slab
              for (int y = static_cast<int>(horizonY - mh); y <= static_cast<int>(horizonY); ++y)
                for (int x = static_cast<int>(monoCx - mw); x <= static_cast<int>(monoCx + mw); ++x)
                  if (x >= 0 && x < w && y >= 0 && y < h)
                    dst[static_cast<std::size_t>(y) * w + x] = Rgb{2, 2, 4, false};
              if (flare > 0.0F)
                for (std::size_t k = 0; k < dst.size(); ++k)
                  dst[k] = Rgb{u8(dst[k].r + (255 - dst[k].r) * flare * 0.7F),
                               u8(dst[k].g + (255 - dst[k].g) * flare * 0.7F),
                               u8(dst[k].b + (255 - dst[k].b) * flare * 0.7F),
                               false};
            });
}

// Tron: the data becomes a neon perspective grid rushing toward the horizon,
// with light-cycle ribbons streaking down it, then a derez flash.
void effectTron(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float horizon = h * 0.40F, cxw = w * 0.5F, A = h - horizon;
  runFrames(
      renderer,
      w,
      h,
      4800,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float scroll = t * 6.0F;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            Rgb out;
            if (y <= horizon)  // dark sky with a glow band at the horizon
            {
              const float gb = std::exp(-(horizon - y) / (h * 0.18F)) * 0.6F;
              out = Rgb{u8(8 + gb * 30), u8(14 + gb * 120), u8(26 + gb * 150), false};
            }
            else
            {
              const float v = A / (y - horizon);
              const float wu = (x - cxw) * v / (w * 0.5F);
              const float wv = v + scroll;
              const Rgb s = sample(src, w, h, (wu * 0.25F + 0.5F) * w, std::fmod(wv * 12.0F, h));
              const float du = std::fabs(wu / 0.5F - std::round(wu / 0.5F));
              const float dv = std::fabs(wv - std::round(wv));
              const float line = std::max(std::exp(-du * 18.0F), std::exp(-dv * 8.0F));
              const float base = s.transparent ? 20.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
              out = Rgb{u8(8 + base * 0.06F + line * 60),
                        u8(20 + base * 0.10F + line * 200),
                        u8(36 + base * 0.14F + line * 230),
                        false};
            }
            dst[static_cast<std::size_t>(y) * w + x] = out;
          }
        for (int c = 0; c < 2; ++c)  // light-cycle ribbons racing toward the viewer
        {
          const float lu = (c == 0 ? -0.32F : 0.30F);
          const Rgb col = (c == 0) ? Rgb{90, 230, 255, false} : Rgb{255, 170, 40, false};
          const float headV = 0.6F + (1.0F - t) * 5.0F;
          for (int k = 0; k <= 24; ++k)
          {
            const float v = headV + k * 0.5F;
            const float sy = horizon + A / v;
            const float sx = cxw + lu * w * 0.5F / v;
            if (sy > horizon && sy < h)
              plotDot(dst, w, h, sx, sy, std::max(1.0F, (h - sy) * 0.04F), ya, col);
          }
        }
        const float derez = std::pow(std::clamp((t - 0.9F) / 0.1F, 0.0F, 1.0F), 0.6F);
        if (derez > 0.0F)
          for (std::size_t k = 0; k < dst.size(); ++k)
            dst[k] = Rgb{u8(dst[k].r + (120 - dst[k].r) * derez),
                         u8(dst[k].g + (240 - dst[k].g) * derez),
                         u8(dst[k].b + (255 - dst[k].b) * derez),
                         false};
      });
}

// Raiders of the Lost Ark: the view rolls up into a colossal boulder (its face
// wrapped from the data) that comes rumbling forward, growing until it crushes
// the screen in a cloud of dust.
void effectBoulder(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  const float mn = std::min(static_cast<float>(w), h * ya);
  runFrames(renderer,
            w,
            h,
            4800,
            [&](float t, std::vector<Rgb>& dst)
            {
              for (int y = 0; y < h; ++y)  // dim corridor from the data
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  dst[static_cast<std::size_t>(y) * w + x] =
                      s.transparent ? Rgb{14, 12, 10, false}
                                    : Rgb{u8(s.r * 0.28F), u8(s.g * 0.26F), u8(s.b * 0.22F), false};
                }
              const float grow = std::pow(t, 1.7F);
              const float r = mn * (0.10F + grow * 0.95F);
              const float bx = w * (0.5F + 0.18F * std::sin(t * 3.0F) * (1.0F - grow));
              const float by = h * (0.34F + grow * 0.30F);
              const float spin = t * 9.0F;
              for (int k = 0; k < 14; ++k)  // dust kicked up around the base
              {
                const float a = hash(k) * 6.2832F, dd = r * (0.8F + hash(k * 3) * 0.5F);
                plotDot(dst,
                        w,
                        h,
                        bx + std::cos(a) * dd,
                        by + r * 0.6F + std::sin(a) * r * 0.3F,
                        r * 0.18F,
                        ya,
                        Rgb{u8(90 + hash(k * 7) * 40), u8(82), u8(70), false});
              }
              drawSphere(dst, w, h, src, bx, by, r, r, 0.9F, spin, 0.62F, 0.55F, 0.48F);
              const float crush = std::clamp((t - 0.88F) / 0.12F, 0.0F, 1.0F);
              if (crush > 0.0F)
                for (std::size_t k = 0; k < dst.size(); ++k)
                  dst[k] = Rgb{u8(dst[k].r * (1 - crush) + 60 * crush),
                               u8(dst[k].g * (1 - crush) + 54 * crush),
                               u8(dst[k].b * (1 - crush) + 46 * crush),
                               false};
            });
}

// Apocalypse Now: the data burns into a blazing sunset haze over a dark
// treeline, and helicopter silhouettes beat across it.
void effectApocalypse(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float treeY = h * 0.78F, sunX = w * 0.5F, sunY = h * 0.62F;
  runFrames(
      renderer,
      w,
      h,
      5200,
      [&](float t, std::vector<Rgb>& dst)
      {
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 70.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            const float sd = std::hypot(x - sunX, (y - sunY) * ya);
            const float glow = std::exp(-sd / (0.55F * mn));
            Rgb c;
            if (y > treeY + std::sin(x * 0.20F) * h * 0.02F)  // dark jungle treeline
              c = Rgb{u8(14 + l * 0.04F), u8(10 + l * 0.03F), u8(8), false};
            else  // sunset haze from the data
            {
              const float sf = static_cast<float>(y) / treeY;
              c = Rgb{u8(120 + 120 * sf + glow * 110 + l * 0.12F),
                      u8(50 + 70 * sf + glow * 90 + l * 0.10F),
                      u8(30 + 20 * sf + glow * 40),
                      false};
            }
            dst[static_cast<std::size_t>(y) * w + x] = c;
          }
        plotDot(dst, w, h, sunX, sunY, 0.16F * mn, ya, Rgb{255, 180, 90, false});  // hazy sun
        const Rgb heli{12, 8, 6, false};
        for (int i = 0; i < 4; ++i)  // choppers crossing at different depths
        {
          const float dep = 0.4F + 0.6F * (i / 3.0F);
          const float hx = std::fmod(t * (0.5F + dep) + i * 0.27F, 1.3F) * w - w * 0.15F;
          const float hy = h * (0.26F + 0.10F * i) + std::sin(t * 3.0F + i) * h * 0.01F;
          const float s = mn * 0.05F * dep;
          drawSeg(dst, w, h, hx - s * 1.6F, hy, hx + s * 0.8F, hy, s * 0.4F, ya, heli);  // body
          drawSeg(dst, w, h, hx + s * 0.8F, hy, hx + s * 2.0F, hy - s * 0.3F, s * 0.12F, ya, heli);
          const float rot = (0.6F + 0.4F * std::sin(t * 50.0F + i)) * s * 2.2F;  // blurred rotor
          drawSeg(dst, w, h, hx - rot, hy - s * 0.7F, hx + rot, hy - s * 0.7F, s * 0.1F, ya, heli);
          drawSeg(dst,
                  w,
                  h,
                  hx - s * 1.2F,
                  hy + s * 0.5F,
                  hx + s * 0.4F,
                  hy + s * 0.5F,
                  s * 0.1F,
                  ya,
                  heli);  // skids
        }
      });
}

// King Kong: the data settles into a night skyline, the great ape stands atop
// the tower swatting at circling biplanes — then loses his grip and falls.
void effectKong(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float towerX = w * 0.5F, towerTop = h * 0.34F;
  runFrames(
      renderer,
      w,
      h,
      5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        for (int y = 0; y < h; ++y)  // night sky + skyline silhouette from the data
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 0.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            const float sky = static_cast<float>(y) / h;
            const float skylineY =
                h * 0.62F + std::sin(x * 0.21F) * h * 0.04F + std::sin(x * 0.07F) * h * 0.05F;
            if (y > skylineY)  // buildings: dark with lit windows from data
            {
              const bool win =
                  (static_cast<int>(x) % 3 == 0) && (static_cast<int>(y) % 2 == 0) && l > 120;
              dst[static_cast<std::size_t>(y) * w + x] =
                  win ? Rgb{200, 190, 120, false} : Rgb{u8(10 + l * 0.04F), u8(10), u8(16), false};
            }
            else
              dst[static_cast<std::size_t>(y) * w + x] = Rgb{
                  u8(12 + 16 * (1 - sky)), u8(14 + 16 * (1 - sky)), u8(28 + 22 * (1 - sky)), false};
          }
        // pale moon behind the perch, so the ape reads as a silhouette
        plotDot(dst, w, h, towerX, towerTop - mn * 0.02F, mn * 0.26F, ya, Rgb{55, 58, 74, false});
        plotDot(
            dst, w, h, towerX, towerTop - mn * 0.02F, mn * 0.17F, ya, Rgb{206, 206, 198, false});
        {  // the tower (Empire State) rising to the perch
          const float tw = mn * 0.05F;
          for (int y = static_cast<int>(towerTop); y < h; ++y)
          {
            const float taper = (y < towerTop + mn * 0.14F) ? 0.45F : 1.0F;
            for (int x = static_cast<int>(towerX - tw * taper);
                 x <= static_cast<int>(towerX + tw * taper);
                 ++x)
              if (x >= 0 && x < w)
                dst[static_cast<std::size_t>(y) * w + x] =
                    ((static_cast<int>(y) % 2 == 0) && (static_cast<int>(x) % 2 == 0))
                        ? Rgb{120, 110, 80, false}
                        : Rgb{34, 32, 42, false};
          }
        }
        const float fall = std::clamp((t - 0.82F) / 0.18F, 0.0F, 1.0F);
        const float kongY = towerTop + fall * h * 0.8F;
        const float kongX = towerX + fall * fall * w * 0.12F;
        const float s = mn * 0.10F;
        const float roll = fall * 6.0F;
        if (fall < 1.0F)
        {
          const float ca = std::cos(roll), sa = std::sin(roll);
          auto rp = [&](float lx, float ly, float& ox, float& oy)
          {
            ox = kongX + lx * ca - ly * sa;
            oy = kongY + (lx * sa + ly * ca) / ya;
          };
          float bx, by, hx, hy, ax, ay;
          rp(0, 0, bx, by);
          rp(0, -s * 1.3F, hx, hy);
          const float swat = std::sin(t * 12.0F) * 0.6F;  // swatting arm
          rp(s * 1.1F, -s * (1.0F + swat), ax, ay);
          plotDot(dst, w, h, bx, by, s * 1.05F, ya, Rgb{50, 42, 36, false});  // body
          plotDot(dst, w, h, hx, hy, s * 0.55F, ya, Rgb{46, 38, 32, false});  // head
          drawSeg(dst, w, h, bx, by - s * 0.4F, ax, ay, s * 0.3F, ya, Rgb{50, 42, 36, false});
          drawSeg(dst,
                  w,
                  h,
                  bx,
                  by - s * 0.4F,
                  bx - s * 1.0F,
                  by - s * 0.2F,
                  s * 0.3F,
                  ya,
                  Rgb{50, 42, 36, false});  // gripping arm
        }
        for (int i = 0; i < 3; ++i)  // circling biplanes
        {
          const float a = t * 4.0F + i * 2.094F;
          const float px = towerX + std::cos(a) * mn * 0.34F;
          const float py = towerTop + std::sin(a) * mn * 0.20F - mn * 0.04F;
          const Rgb pl{20, 20, 26, false};
          drawSeg(dst, w, h, px - mn * 0.03F, py, px + mn * 0.03F, py, mn * 0.012F, ya, pl);
          drawSeg(dst, w, h, px, py - mn * 0.018F, px, py + mn * 0.018F, mn * 0.01F, ya, pl);
          if (static_cast<int>(t * 20.0F + i) % 4 == 0)  // muzzle flicker
            plotDot(dst, w, h, px - mn * 0.03F, py, mn * 0.012F, ya, Rgb{255, 220, 120, false});
        }
      });
}

// The Wizard of Oz: the world drains from Technicolor to sepia, three clicks of
// the ruby slippers spark, and then a tornado swirl whisks it all up and away.
void effectOz(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float cx = w * 0.5F, cy = h * 0.5F, mn = std::min(static_cast<float>(w), h * ya);
  runFrames(renderer,
            w,
            h,
            5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float desat = std::clamp(t / 0.35F, 0.0F, 1.0F);
              const float swirl = std::clamp((t - 0.45F) / 0.55F, 0.0F, 1.0F);
              const float spin = swirl * swirl * 12.0F;
              const float pull = 1.0F + swirl * 2.6F;
              const float vy = cy - swirl * cy * 1.1F;  // funnel climbs to the top
              const float fade = 1.0F - swirl * 0.7F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const float dx = x - cx, dy = (y - vy) * ya, r = std::hypot(dx, dy);
                  const float th = std::atan2(dy, dx) - spin / (r / mn + 0.2F);
                  const float r2 = r * pull;
                  const Rgb s =
                      sample(src, w, h, cx + std::cos(th) * r2, vy + std::sin(th) * r2 / ya);
                  const float l = s.transparent ? 0.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  const float br = s.transparent ? 0 : s.r, bg = s.transparent ? 0 : s.g,
                              bb = s.transparent ? 0 : s.b;
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8((br * (1 - desat) + l * 1.07F * desat) * fade),
                          u8((bg * (1 - desat) + l * 0.88F * desat) * fade),
                          u8((bb * (1 - desat) + l * 0.62F * desat) * fade),
                          false};
                }
              for (int c = 0; c < 3; ++c)  // ruby-slipper heel clicks
              {
                const float fl = std::exp(-std::pow((t - (0.36F + c * 0.045F)) / 0.018F, 2.0F));
                if (fl > 0.02F)
                  plotDot(dst,
                          w,
                          h,
                          cx,
                          h * 0.82F,
                          mn * 0.06F * fl,
                          ya,
                          Rgb{u8(255 * fl), u8(120 * fl), u8(150 * fl), false});
              }
            });
}

// Nosferatu: an eerie wall (the data, cold and dim) over which a great clawed
// shadow-hand creeps upward, the darkness rising with it until all is night.
void effectNosferatu(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  runFrames(renderer,
            w,
            h,
            5200,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float climb = std::pow(t, 1.15F);
              const float shadowTop = h * (1.08F - climb * 1.2F);
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 0.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  const float edge = shadowTop + std::sin(x * 0.18F) * h * 0.015F;
                  dst[static_cast<std::size_t>(y) * w + x] =
                      (y > edge)
                          ? Rgb{3, 3, 6, false}
                          : Rgb{u8(l * 0.40F + 22), u8(l * 0.42F + 26), u8(l * 0.52F + 36), false};
                }
              const float hx = w * 0.5F, handBase = shadowTop + mn * 0.10F;  // clawed hand
              const Rgb shadow{2, 2, 5, false};
              plotDot(dst, w, h, hx, handBase, mn * 0.13F, ya, shadow);  // palm
              for (int f = 0; f < 5; ++f)
              {
                const float ang = (f - 2) * 0.34F + std::sin(t * 3.0F + f) * 0.05F;
                const float fl = mn * (0.42F + (f == 2 ? 0.06F : 0.0F));  // middle finger longest
                const float tx = hx + std::sin(ang) * fl, tyy = handBase - std::cos(ang) * fl;
                drawSeg(dst,
                        w,
                        h,
                        hx + std::sin(ang) * mn * 0.05F,
                        handBase - mn * 0.04F,
                        tx,
                        tyy,
                        std::max(1.0F, mn * 0.024F),
                        ya,
                        shadow);
                plotDot(dst, w, h, tx, tyy, std::max(1.0F, mn * 0.02F), ya, shadow);  // claw tip
              }
            });
}

// The Birds: the scene dims to dread as silhouettes gather — a few at first,
// then more and more, until the birds blacken the whole frame.
void effectBirds(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  const float mn = std::min(static_cast<float>(w), h * ya);
  runFrames(
      renderer,
      w,
      h,
      5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float d = 0.72F - 0.32F * t;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            dst[static_cast<std::size_t>(y) * w + x] =
                s.transparent ? Rgb{6, 6, 10, false}
                              : Rgb{u8(s.r * d), u8(s.g * d), u8(s.b * d), false};
          }
        const int N = static_cast<int>(t * t * 420);
        const Rgb bird{6, 6, 8, false};
        for (int i = 0; i < N; ++i)
        {
          const float drift =
              (hash(i * 5) < 0.45F) ? std::sin(t * 2.0F + i) * mn * 0.06F : 0.0F;  // some in flight
          const float bx = hash(i * 2) * w + drift, by = hash(i * 2 + 1) * h;
          const float sz = mn * 0.018F * (0.7F + hash(i * 7) * 0.7F);
          drawSeg(dst, w, h, bx - sz, by, bx, by - sz * 0.5F, std::max(1.0F, sz * 0.4F), ya, bird);
          drawSeg(dst, w, h, bx, by - sz * 0.5F, bx + sz, by, std::max(1.0F, sz * 0.4F), ya, bird);
        }
      });
}

// Pulp Fiction: a briefcase in the dark room is opened, and the unknowable
// golden glow within spills out, warming everything and swelling to white.
void effectPulp(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float caseX = w * 0.5F, caseY = h * 0.60F;
  const float cw = mn * 0.16F, ch = mn * 0.11F;
  runFrames(
      renderer,
      w,
      h,
      5000,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float open = std::clamp((t - 0.15F) / 0.55F, 0.0F, 1.0F);
        const float glow = std::pow(open, 1.3F) * (0.85F + 0.15F * std::sin(t * 9.0F));
        const float whiteout = std::clamp((t - 0.85F) / 0.15F, 0.0F, 1.0F);
        const float gy = caseY - ch * 0.6F;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float br = s.transparent ? 14 : s.r * 0.4F, bg = s.transparent ? 12 : s.g * 0.4F,
                        bb = s.transparent ? 18 : s.b * 0.45F;
            const float gd = std::hypot(x - caseX, (y - gy) * ya);
            const float g = glow * std::exp(-gd / (0.5F * mn)) * 1.5F;
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(br + g * 255), u8(bg + g * 205), u8(bb + g * 70), false};
          }
        for (int y = static_cast<int>(caseY - ch); y <= static_cast<int>(caseY + ch); ++y)
          for (int x = static_cast<int>(caseX - cw); x <= static_cast<int>(caseX + cw); ++x)
            if (x >= 0 && x < w && y >= 0 && y < h)
              dst[static_cast<std::size_t>(y) * w + x] = Rgb{26, 22, 20, false};  // case body
        const float gap = open * ch * 1.1F;  // the glowing opening as the lid lifts
        for (int y = static_cast<int>(caseY - ch - gap); y <= static_cast<int>(caseY - ch); ++y)
          for (int x = static_cast<int>(caseX - cw * 0.9F);
               x <= static_cast<int>(caseX + cw * 0.9F);
               ++x)
            if (x >= 0 && x < w && y >= 0 && y < h)
              dst[static_cast<std::size_t>(y) * w + x] = Rgb{255, 220, 110, false};
        if (whiteout > 0.0F)
          for (std::size_t k = 0; k < dst.size(); ++k)
            dst[k] = Rgb{u8(dst[k].r + (255 - dst[k].r) * whiteout),
                         u8(dst[k].g + (250 - dst[k].g) * whiteout),
                         u8(dst[k].b + (210 - dst[k].b) * whiteout),
                         false};
      });
}

// Dr. Strangelove: the data packs into a falling bomb with a hat-waving rider
// astride it, dwindling toward the target — then a blinding flash and a mushroom
// cloud (billowed from the data) rises.
void effectStrangelove(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float flashT = 0.56F;
  runFrames(
      renderer,
      w,
      h,
      5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        if (t < flashT)  // the ride down
        {
          for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
              const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
              const float l = s.transparent ? 0.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
              const float sf = static_cast<float>(y) / h;
              const float cl = std::clamp((l / 255.0F - 0.5F) * 2.0F, 0.0F, 1.0F) * 0.5F;
              dst[static_cast<std::size_t>(y) * w + x] = Rgb{u8(70 + 60 * sf + cl * 130),
                                                             u8(110 + 60 * sf + cl * 110),
                                                             u8(175 - 20 * sf + cl * 40),
                                                             false};
            }
          const float p = t / flashT;
          const float bx = w * 0.5F, by = h * (0.26F + 0.55F * p);
          const float s = mn * 0.14F * (1.0F - 0.65F * p);
          for (int i = -6; i <= 6; ++i)  // bomb body (capsule), data-tinted steel
          {
            const float f = i / 6.0F, along = f * s * 1.4F;
            const float rad = s * 0.34F * std::sqrt(std::max(0.0F, 1.0F - f * f));
            const Rgb d = sample(src, w, h, bx, by + along);
            plotDot(dst,
                    w,
                    h,
                    bx,
                    by + along,
                    std::max(1.0F, rad),
                    ya,
                    Rgb{u8(120 + d.r * 0.2F), u8(122 + d.g * 0.2F), u8(128 + d.b * 0.2F), false});
          }
          drawSeg(dst,
                  w,
                  h,
                  bx - s * 0.4F,
                  by - s * 1.3F,
                  bx + s * 0.4F,
                  by - s * 1.3F,
                  s * 0.1F,
                  ya,
                  Rgb{90, 90, 96, false});                    // tail fins
          const float wave = std::sin(t * 18.0F) * s * 0.5F;  // rider waving his hat
          plotDot(dst, w, h, bx, by - s * 0.4F, s * 0.18F, ya, Rgb{30, 24, 20, false});  // rider
          plotDot(
              dst, w, h, bx + wave, by - s * 0.9F, s * 0.14F, ya, Rgb{60, 45, 30, false});  // hat
        }
        else  // flash and mushroom cloud
        {
          const float mp = std::clamp((t - flashT) / 0.44F, 0.0F, 1.0F);
          const float groundY = h * 0.86F;
          for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
              dst[static_cast<std::size_t>(y) * w + x] =
                  (y > groundY) ? Rgb{40, 30, 22, false} : Rgb{30, 24, 30, false};
          const float capY = groundY - mp * h * 0.55F, capR = mn * (0.10F + mp * 0.34F);
          const float stemW = capR * 0.3F;
          for (int y = static_cast<int>(capY); y < static_cast<int>(groundY); ++y)  // stem
            for (int x = static_cast<int>(w * 0.5F - stemW);
                 x <= static_cast<int>(w * 0.5F + stemW);
                 ++x)
              if (x >= 0 && x < w && y >= 0 && y < h)
              {
                const Rgb d = sample(src, w, h, x, y);
                dst[static_cast<std::size_t>(y) * w + x] =
                    Rgb{u8(150 + d.r * 0.2F), u8(90 + d.g * 0.2F), u8(50), false};
              }
          for (int i = 0; i < 26; ++i)  // billowing cap from the data
          {
            const float a = hash(i) * 6.2832F, rad = std::sqrt(hash(i * 3)) * capR;
            const float px = w * 0.5F + std::cos(a) * rad, py = capY + std::sin(a) * rad * 0.55F;
            const Rgb d = sample(src, w, h, px, py);
            const float core = 1.0F - rad / capR;
            plotDot(dst,
                    w,
                    h,
                    px,
                    py,
                    capR * 0.3F,
                    ya,
                    Rgb{u8(120 + d.r * 0.25F + core * 130),
                        u8(78 + d.g * 0.2F + core * 70),
                        u8(46 + d.b * 0.1F),
                        false});
          }
          const float flash = std::exp(-std::pow((t - flashT) / 0.05F, 2.0F));
          if (flash > 0.01F)
            for (std::size_t k = 0; k < dst.size(); ++k)
              dst[k] = Rgb{u8(dst[k].r + (255 - dst[k].r) * flash),
                           u8(dst[k].g + (255 - dst[k].g) * flash),
                           u8(dst[k].b + (255 - dst[k].b) * flash),
                           false};
        }
      });
}

// Akira: Kaneda's bike power-slides across the night, laying down a long glowing
// red light-trail smeared from the data.
void effectAkira(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float roadY = h * 0.58F;
  runFrames(
      renderer,
      w,
      h,
      4600,
      [&](float t, std::vector<Rgb>& dst)
      {
        for (int y = 0; y < h; ++y)  // dark night road from the data
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float road = (y > roadY) ? 0.22F : 0.12F;
            dst[static_cast<std::size_t>(y) * w + x] =
                s.transparent ? Rgb{8, 8, 14, false}
                              : Rgb{u8(s.r * road), u8(s.g * road), u8(s.b * road + 6), false};
          }
        const float p = 1.0F - std::pow(1.0F - t, 2.2F);  // decelerating slide
        const float startX = -0.12F * w, bx = startX + 1.2F * w * p;
        const int steps = static_cast<int>((bx - startX) / 1.2F) + 1;
        for (int k = 0; k <= steps; ++k)  // glowing red light-trail (data-smeared)
        {
          const float f = static_cast<float>(k) / steps;  // 0 tail .. 1 head
          const float tx = startX + (bx - startX) * f;
          const float tyy = roadY + std::sin(f * 6.0F + t) * mn * 0.015F;
          const Rgb d = sample(src, w, h, tx, tyy);
          const float dr = d.transparent ? 40 : d.r;
          const float glow = 0.3F + 0.7F * f;  // brighter toward the head
          plotDot(dst,
                  w,
                  h,
                  tx,
                  tyy,
                  mn * (0.02F + 0.03F * f),
                  ya,
                  Rgb{u8((150 + dr * 0.4F) * glow + 60),
                      u8(30 * glow + d.g * 0.1F),
                      u8(40 * glow),
                      false});
        }
        const float s = mn * 0.08F;  // the bike, leaning into the slide
        const Rgb bike{210, 40, 40, false};
        drawSeg(dst,
                w,
                h,
                bx - s * 1.4F,
                roadY - s * 0.2F,
                bx + s * 0.9F,
                roadY - s * 0.5F,
                s * 0.4F,
                ya,
                bike);
        plotDot(dst, w, h, bx - s * 1.1F, roadY + s * 0.2F, s * 0.5F, ya, Rgb{20, 20, 26, false});
        plotDot(dst, w, h, bx + s * 0.7F, roadY, s * 0.5F, ya, Rgb{20, 20, 26, false});
        plotDot(dst,
                w,
                h,
                bx - s * 0.2F,
                roadY - s * 0.9F,
                s * 0.32F,
                ya,
                Rgb{200, 70, 60, false});  // hunched rider
        for (int sp = 0; sp < 5; ++sp)     // skid sparks
          plotDot(dst,
                  w,
                  h,
                  bx - s * 1.2F - sp * mn * 0.02F,
                  roadY + s * 0.3F,
                  std::max(1.0F, mn * 0.008F),
                  ya,
                  Rgb{255, u8(180 - sp * 20), 60, false});
      });
}

// A Clockwork Orange: a single enormous eye stares out, the data swimming in its
// iris, the spiked lashes splayed below — then it blinks shut to black.
void effectClockwork(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float cx = w * 0.5F, cy = h * 0.5F, mn = std::min(static_cast<float>(w), h * ya);
  const float irisR = mn * 0.27F, eyeR = mn * 0.46F;
  runFrames(
      renderer,
      w,
      h,
      4800,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float blink = std::clamp((t - 0.82F) / 0.18F, 0.0F, 1.0F);
        const float dil = 0.42F + 0.30F * std::sin(t * 3.0F);  // pupil dilation
        const float lidTop = cy - eyeR + blink * eyeR, lidBot = cy + eyeR - blink * eyeR;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const float dx = x - cx, dy = (y - cy) * ya, r = std::hypot(dx, dy);
            Rgb c;
            if (r < irisR)  // the iris carries the data
            {
              const float ang = std::atan2(dy, dx);
              const Rgb d =
                  sample(src, w, h, (dx / irisR * 0.5F + 0.5F) * w, (dy / irisR * 0.5F + 0.5F) * h);
              const float stri = 0.78F + 0.22F * std::sin(ang * 38.0F);
              const float rim = (r > irisR * 0.86F) ? 0.5F : 1.0F;  // dark limbal ring
              if (r < irisR * dil)
                c = Rgb{6, 5, 8, false};  // pupil
              else
                c = Rgb{u8(d.r * stri * rim), u8(d.g * stri * rim), u8(d.b * stri * rim), false};
            }
            else if (r < eyeR)  // sclera, with a little curvature shading and veins
            {
              const float sh = 0.82F + 0.18F * (1.0F - r / eyeR);
              const float vein = (std::fabs(dx) > eyeR * 0.7F) ? 0.85F : 1.0F;
              c = Rgb{u8(228 * sh), u8(222 * sh * vein), u8(214 * sh * vein), false};
            }
            else
              c = Rgb{150, 110, 95, false};  // surrounding skin
            if (y < lidTop || y > lidBot)
              c = Rgb{170, 128, 110, false};  // eyelid skin closing
            dst[static_cast<std::size_t>(y) * w + x] = c;
          }
        if (blink < 0.5F)  // the spiked lashes on the lower lid (Alex's eye)
          for (int i = -3; i <= 3; ++i)
          {
            const float lx = cx + i * eyeR * 0.26F;
            const float ly = cy + eyeR * 0.62F;
            drawSeg(dst,
                    w,
                    h,
                    lx,
                    ly,
                    lx + i * mn * 0.02F,
                    ly + mn * 0.12F,
                    std::max(1.0F, mn * 0.012F),
                    ya,
                    Rgb{12, 10, 12, false});
          }
        if (blink >= 1.0F)
          for (std::size_t k = 0; k < dst.size(); ++k)
            dst[k] = Rgb{0, 0, 0, false};
      });
}

// Spinning newspaper: the data flies in as a tumbling newspaper page (its body
// the sampled data, sepia-newsprint tinted), spins to a stop facing the camera
// with the headline EXTRA EXTRA above it, then fades.
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

// The End card: the data desaturates to sepia, vignettes down, and a silent-era
// double-bordered title card centred with "THE END" fades in over it.
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

// The Shining elevator: the data forms a corridor in 1-point perspective (the
// carpet is the data warped into the floor). At the vanishing point an elevator
// opens — and a red flood pours forward, the level rising toward the camera
// until it engulfs the screen.
void effectShining(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float horizon = h * 0.46F, cxw = w * 0.5F, A = h - horizon;
  const float wallA = horizon;
  runFrames(
      renderer,
      w,
      h,
      5200,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float openT = std::clamp((t - 0.20F) / 0.18F, 0.0F, 1.0F);
        const float flow = std::clamp((t - 0.38F) / 0.50F, 0.0F, 1.0F);
        const float front = horizon + (h - horizon) * flow;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            Rgb out;
            if (y > horizon)
            {
              const float v = A / (y - horizon);
              const float wu = (x - cxw) * v / (w * 0.5F);
              const Rgb d = sample(src, w, h, (wu * 0.25F + 0.5F) * w, std::fmod(v * 22.0F, h));
              const float l = d.transparent ? 50 : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
              out = Rgb{u8(80 + l * 0.18F), u8(40 + l * 0.10F), u8(50 + l * 0.10F), false};
            }
            else
            {
              const float du = std::fabs(x - cxw) / (w * 0.5F);
              const float vv = wallA / (horizon - y + 1.0F);
              const float wally = (1.0F - du);
              const Rgb d = sample(src, w, h, std::fmod(vv * 26.0F, w), (du * 0.5F + 0.25F) * h);
              const float l = d.transparent ? 60 : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
              out = Rgb{u8(130 + l * 0.18F * wally),
                        u8(110 + l * 0.16F * wally),
                        u8(90 + l * 0.12F * wally),
                        false};
            }
            dst[static_cast<std::size_t>(y) * w + x] = out;
          }
        const float dw = std::max(2.0F, w * 0.06F), dh = std::max(3.0F, h * 0.10F);
        const float ey = horizon - dh * 0.5F;
        for (int yo = 0; yo <= static_cast<int>(dh); ++yo)
          for (int xo = -static_cast<int>(dw); xo <= static_cast<int>(dw); ++xo)
          {
            const int x = static_cast<int>(cxw) + xo, y = static_cast<int>(ey) + yo;
            if (x < 0 || x >= w || y < 0 || y >= h)
              continue;
            const float opx = std::fabs(static_cast<float>(xo)) / dw;
            const Rgb door{20, 12, 12, false}, mouth{180, 8, 8, false};
            dst[static_cast<std::size_t>(y) * w + x] = (opx < openT) ? mouth : door;
          }
        if (flow > 0.0F)
        {
          for (int y = static_cast<int>(front); y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
              const float turb = 0.85F + 0.15F * std::sin((x - y) * 0.3F + t * 8.0F);
              const float deep = std::clamp((y - front) / (h - front + 1.0F), 0.0F, 1.0F);
              const Rgb& base = dst[static_cast<std::size_t>(y) * w + x];
              dst[static_cast<std::size_t>(y) * w + x] =
                  Rgb{u8((180 + 60 * deep) * turb + base.r * 0.10F),
                      u8(14 * turb + base.g * 0.05F),
                      u8(14 * turb + base.b * 0.05F),
                      false};
            }
          for (int x = 0; x < w; ++x)
          {
            const int yf = static_cast<int>(front + std::sin(x * 0.4F + t * 6.0F) * 1.2F);
            if (yf >= 0 && yf < h)
              dst[static_cast<std::size_t>(yf) * w + x] = Rgb{255, 130, 130, false};
          }
        }
      });
}

// Dial M for Murder: a rotary phone dial appears, the finger pulls the wheel
// round to "M" against the stop, then releases and the wheel spins back home —
// then the line rings and we cut to black.
void effectDialM(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float cx = w * 0.5F, cy = h * 0.52F;
  const float R = mn * 0.30F;
  const float holeR = mn * 0.045F;
  const float ringR = R + holeR + 2.0F;
  const int mPos = 5;
  const float aPer = 6.2832F / 10.0F;
  runFrames(
      renderer,
      w,
      h,
      4800,
      [&](float t, std::vector<Rgb>& dst)
      {
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            dst[static_cast<std::size_t>(y) * w + x] =
                s.transparent
                    ? Rgb{12, 10, 14, false}
                    : Rgb{u8(s.r * 0.22F + 4), u8(s.g * 0.20F + 4), u8(s.b * 0.18F + 6), false};
          }
        float ang = 0.0F;
        if (t < 0.18F)
          ang = 0.0F;
        else if (t < 0.48F)
          ang = ((t - 0.18F) / 0.30F) * (aPer * mPos);
        else if (t < 0.78F)
        {
          const float k = (t - 0.48F) / 0.30F;
          ang = (1.0F - std::pow(k, 0.85F)) * (aPer * mPos);
        }
        else
          ang = 0.0F;
        const float ringFlash = (t > 0.80F) ? std::exp(-std::pow((t - 0.84F) / 0.04F, 2.0F)) +
                                                  std::exp(-std::pow((t - 0.92F) / 0.04F, 2.0F))
                                            : 0.0F;
        plotDot(dst, w, h, cx, cy, ringR, ya, Rgb{42, 36, 32, false});
        plotDot(dst, w, h, cx, cy, R + holeR * 0.2F, ya, Rgb{215, 208, 195, false});
        plotDot(dst, w, h, cx, cy, R * 0.32F, ya, Rgb{40, 32, 28, false});
        for (int k = 0; k < 10; ++k)
        {
          const float a = -1.5708F + k * aPer + ang;
          const float hx = cx + std::sin(a) * R, hy = cy - std::cos(a) * R;
          plotDot(dst, w, h, hx, hy, holeR, ya, Rgb{20, 16, 16, false});
          if (k == mPos)
          {
            const auto g = glyph5x7('M');
            const int sc = std::max(1, static_cast<int>(holeR / 4.0F));
            for (int fy = 0; fy < 7; ++fy)
              for (int fx = 0; fx < 5; ++fx)
                if (g[fy][fx] == '1')
                  for (int sy = 0; sy < sc; ++sy)
                    for (int sx = 0; sx < sc; ++sx)
                    {
                      const int xp = static_cast<int>(hx - 2.5F * sc) + fx * sc + sx;
                      const int yp = static_cast<int>(hy - 3.5F * sc) + fy * sc + sy;
                      if (xp >= 0 && xp < w && yp >= 0 && yp < h)
                        dst[static_cast<std::size_t>(yp) * w + xp] = Rgb{230, 220, 200, false};
                    }
          }
        }
        const float pa = -1.5708F + 7.5F * aPer;
        plotDot(dst,
                w,
                h,
                cx + std::sin(pa) * (R + holeR * 0.5F),
                cy - std::cos(pa) * (R + holeR * 0.5F),
                holeR * 0.5F,
                ya,
                Rgb{60, 50, 44, false});
        if (t > 0.18F && t < 0.50F)
        {
          const float a = -1.5708F + mPos * aPer + ang;
          plotDot(dst,
                  w,
                  h,
                  cx + std::sin(a) * R,
                  cy - std::cos(a) * R,
                  holeR * 0.75F,
                  ya,
                  Rgb{220, 180, 150, false});
        }
        if (ringFlash > 0.02F)
          for (std::size_t k = 0; k < dst.size(); ++k)
            dst[k] = Rgb{u8(dst[k].r + (255 - dst[k].r) * ringFlash * 0.7F),
                         u8(dst[k].g + (240 - dst[k].g) * ringFlash * 0.7F),
                         u8(dst[k].b + (200 - dst[k].b) * ringFlash * 0.7F),
                         false};
        if (t > 0.96F)
          for (std::size_t k = 0; k < dst.size(); ++k)
            dst[k] = Rgb{0, 0, 0, false};
      });
}

// Saul Bass spiral: an extreme close-up of an eye (data in its iris), with the
// graphic black-and-white Bass title spiral expanding from the pupil, rotating —
// finally swallowing the screen.
void effectBassSpiral(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float ya = yAspectFor(renderer);
  const float cx = w * 0.5F, cy = h * 0.5F, mn = std::min(static_cast<float>(w), h * ya);
  const float eyeR = mn * 0.48F, irisR = mn * 0.27F;
  runFrames(
      renderer, w, h, 4800,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float spinR = std::pow(t, 1.1F) * mn * 0.72F;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const float dx = x - cx, dy = (y - cy) * ya, r = std::hypot(dx, dy);
            Rgb base;
            if (r < irisR)
            {
              const Rgb d = sample(src, w, h, (dx / irisR * 0.5F + 0.5F) * w,
                                   (dy / irisR * 0.5F + 0.5F) * h);
              if (r < irisR * 0.22F)
                base = Rgb{4, 4, 6, false};
              else
                base = Rgb{u8(d.r * 0.85F), u8(d.g * 0.85F), u8(d.b * 0.85F), false};
            }
            else if (r < eyeR)
              base = Rgb{225, 220, 212, false};
            else
              base = Rgb{30, 22, 20, false};
            if (r < spinR)
            {
              const float th = std::atan2(dy, dx);
              const float arm = std::sin(th * 4.0F - std::log(r + 1.0F) * 2.6F + t * 7.0F);
              if (arm > 0.0F)
                base = Rgb{8, 8, 10, false};
              else if (arm < -0.7F)
                base = Rgb{240, 235, 226, false};
            }
            dst[static_cast<std::size_t>(y) * w + x] = base;
          }
      });
}

// The Big Lebowski: POV inside the bowling ball as it rolls down the lane (a
// data-grain wooden floor) toward the pins — then a STRIKE: the pins fly outward
// in a white flash.
void effectLebowski(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float horizon = h * 0.40F, cxw = w * 0.5F, A = h - horizon;
  struct Pin
  {
    float wu;
    float wv;
  };
  static const Pin pins[] = {{0.0F, 0.0F},   {-0.18F, 0.22F}, {0.18F, 0.22F}, {-0.34F, 0.44F},
                             {0.0F, 0.44F},  {0.34F, 0.44F},  {-0.50F, 0.66F}, {-0.17F, 0.66F},
                             {0.17F, 0.66F}, {0.50F, 0.66F}};
  runFrames(
      renderer, w, h, 4800,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float approach = t * 3.2F;
        const float strike = std::clamp((t - 0.85F) / 0.10F, 0.0F, 1.0F);
        const float scroll = t * 4.0F;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            Rgb out;
            if (y <= horizon)
            {
              const float gb = std::exp(-(horizon - y) / (h * 0.2F)) * 0.3F;
              out = Rgb{u8(12 + gb * 80), u8(10 + gb * 60), u8(20 + gb * 40), false};
            }
            else
            {
              const float v = A / (y - horizon);
              const float wu = (x - cxw) * v / (w * 0.5F);
              const float wv = v + scroll;
              const bool gutter = std::fabs(wu) > 0.55F;
              const Rgb d = sample(src, w, h, (wu * 0.30F + 0.5F) * w, std::fmod(wv * 18.0F, h));
              const float l = d.transparent ? 70 : (0.3F * d.r + 0.59F * d.g + 0.11F * d.b);
              if (gutter)
                out = Rgb{30, 22, 18, false};
              else
              {
                const float plank = std::sin(wv * 8.0F) * 0.08F;
                out = Rgb{u8(180 + l * 0.20F + plank * 30), u8(120 + l * 0.16F + plank * 20),
                          u8(60 + l * 0.10F), false};
              }
            }
            dst[static_cast<std::size_t>(y) * w + x] = out;
          }
        for (int i = 0; i < 10; ++i)
        {
          float pwv = 3.5F + pins[i].wv - approach;
          float pwu = pins[i].wu;
          if (strike > 0.0F)
          {
            const float spd = 0.4F + hash(i * 3) * 0.6F;
            pwu +=
                (pins[i].wu < 0 ? -1.0F : (pins[i].wu > 0 ? 1.0F : (hash(i) - 0.5F))) * strike * spd;
            pwv += strike * 0.6F;
          }
          if (pwv < 0.05F)
            continue;
          const float sy = horizon + A / pwv;
          const float sx = cxw + pwu * w * 0.5F / pwv;
          const float sz = mn * 0.18F / pwv;
          if (sy < 0 || sy > h + sz || sx < -sz || sx > w + sz)
            continue;
          plotDot(dst, w, h, sx, sy, sz * 0.5F, ya, Rgb{245, 240, 230, false});
          plotDot(dst, w, h, sx, sy - sz * 0.6F, sz * 0.3F, ya, Rgb{245, 240, 230, false});
          plotDot(dst, w, h, sx, sy - sz * 0.35F, sz * 0.18F, ya, Rgb{200, 30, 30, false});
        }
        const float flash = (t > 0.83F) ? std::exp(-std::pow((t - 0.86F) / 0.04F, 2.0F)) : 0.0F;
        if (flash > 0.01F)
          for (std::size_t k = 0; k < dst.size(); ++k)
            dst[k] = Rgb{u8(dst[k].r + (255 - dst[k].r) * flash),
                         u8(dst[k].g + (255 - dst[k].g) * flash),
                         u8(dst[k].b + (255 - dst[k].b) * flash), false};
      });
}

// Singin' in the Rain: a figure swings off a lamppost with an umbrella in the
// pouring rain, on a wet night street made of the dimmed data.
void effectSinginRain(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float postX = w * 0.55F;
  runFrames(
      renderer, w, h, 5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float lantX = postX, lantY = h * 0.22F;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 30.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            const float gd = std::hypot(x - lantX, (y - lantY) * ya);
            const float glow = std::exp(-gd / (mn * 0.30F));
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(16 + l * 0.10F + glow * 130), u8(24 + l * 0.12F + glow * 100),
                    u8(38 + l * 0.18F + glow * 60), false};
          }
        drawSeg(dst, w, h, postX, h * 0.95F, postX, h * 0.22F, std::max(1.0F, mn * 0.014F), ya,
                Rgb{18, 16, 18, false});
        plotDot(dst, w, h, lantX, lantY, mn * 0.06F, ya, Rgb{255, 230, 150, false});
        const float barX = postX - mn * 0.16F, barY = h * 0.26F;
        drawSeg(dst, w, h, postX, barY, barX, barY, std::max(1.0F, mn * 0.012F), ya,
                Rgb{18, 16, 18, false});
        const float swing = std::sin(t * 2.2F) * 0.45F;
        const float L = mn * 0.20F;
        const float bodyX = barX + std::sin(swing) * L, bodyY = barY + std::cos(swing) * L;
        const Rgb suit{30, 26, 32, false};
        drawSeg(dst, w, h, barX, barY, bodyX, bodyY, std::max(1.0F, mn * 0.020F), ya, suit);
        plotDot(dst, w, h, bodyX + std::sin(swing) * mn * 0.04F,
                bodyY + std::cos(swing) * mn * 0.04F, mn * 0.04F, ya, Rgb{210, 180, 150, false});
        const float kick = std::sin(t * 4.0F) * 0.5F;
        const float legX = bodyX + std::sin(swing) * mn * 0.18F;
        const float legY = bodyY + std::cos(swing) * mn * 0.18F;
        drawSeg(dst, w, h, bodyX, bodyY, legX - kick * mn * 0.08F, legY, std::max(1.0F, mn * 0.014F),
                ya, suit);
        drawSeg(dst, w, h, bodyX, bodyY, legX + kick * mn * 0.08F, legY, std::max(1.0F, mn * 0.014F),
                ya, suit);
        const float uHand = bodyX - mn * 0.10F, uHandY = bodyY - mn * 0.05F;
        drawSeg(dst, w, h, bodyX, bodyY - mn * 0.04F, uHand, uHandY, std::max(1.0F, mn * 0.012F), ya,
                suit);
        const float uTop = uHandY - mn * 0.20F;
        drawSeg(dst, w, h, uHand, uHandY, uHand, uTop, std::max(1.0F, mn * 0.010F), ya, suit);
        for (int k = -5; k <= 5; ++k)
        {
          const float a = k / 5.0F;
          const float ux = uHand + a * mn * 0.18F;
          const float uy = uTop - (1.0F - a * a) * mn * 0.08F;
          plotDot(dst, w, h, ux, uy, mn * 0.024F, ya, Rgb{40, 32, 36, false});
        }
        for (int i = 0; i < 120; ++i)
        {
          const float rx = std::fmod(hash(i * 2) * w + t * w * 0.05F + i * 0.7F, (float)w);
          const float ry = std::fmod(hash(i * 2 + 1) * h + t * h * 1.6F + i * 0.4F, (float)h);
          drawSeg(dst, w, h, rx, ry, rx - mn * 0.018F, ry + mn * 0.04F, std::max(1.0F, mn * 0.005F),
                  ya, Rgb{170, 200, 230, false});
        }
      });
}

// Rocky: dawn breaks over the city (data tinted morning), a silhouette sprints
// up the museum steps and at the top throws both arms up in victory.
void effectRocky(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float botX = w * 0.5F, botY = h * 0.96F;
  const float topY = h * 0.42F;
  constexpr int kSteps = 10;
  runFrames(
      renderer, w, h, 5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float sun = std::clamp((t - 0.10F) / 0.8F, 0.0F, 1.0F);
        const float sunY = h * 0.50F - sun * h * 0.24F, sunX = w * 0.5F;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 40.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            const float sf = static_cast<float>(y) / h;
            const float sd = std::hypot(x - sunX, (y - sunY) * ya);
            const float glow = std::exp(-sd / (mn * 0.55F)) * sun;
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(60 + 90 * sf + l * 0.16F + glow * 120),
                    u8(70 + 60 * sf + l * 0.14F + glow * 90),
                    u8(120 - 60 * sf + l * 0.10F + glow * 30), false};
          }
        plotDot(dst, w, h, sunX, sunY, mn * 0.10F * (0.6F + 0.4F * sun), ya,
                Rgb{u8(255), u8(220), u8(160), false});
        for (int i = 0; i < kSteps; ++i)
        {
          const float f0 = static_cast<float>(i) / kSteps;
          const float f1 = static_cast<float>(i + 1) / kSteps;
          const float sw0 = (1.0F - f0) * (w * 0.42F);
          const float sw1 = (1.0F - f1) * (w * 0.42F);
          const float sy = botY + (topY - botY) * f1;
          const Rgb stone{u8(95 - i * 3), u8(85 - i * 2), u8(75 - i * 2), false};
          const Rgb edge{u8(150 - i * 4), u8(140 - i * 4), u8(125 - i * 3), false};
          const int y0 = static_cast<int>(sy);
          const int y1 = static_cast<int>(botY + (topY - botY) * f0);
          for (int y = std::min(y0, y1); y <= std::max(y0, y1); ++y)
          {
            const float ff =
                static_cast<float>(y - y1) / std::max(1.0F, static_cast<float>(y0 - y1));
            const float halfw = sw1 + (sw0 - sw1) * (1.0F - ff);
            for (int x = static_cast<int>(botX - halfw); x <= static_cast<int>(botX + halfw); ++x)
              if (x >= 0 && x < w && y >= 0 && y < h)
                dst[static_cast<std::size_t>(y) * w + x] = stone;
          }
          for (int x = static_cast<int>(botX - sw1); x <= static_cast<int>(botX + sw1); ++x)
            if (x >= 0 && x < w && static_cast<int>(sy) >= 0 && static_cast<int>(sy) < h)
              dst[static_cast<std::size_t>(static_cast<int>(sy)) * w + x] = edge;
        }
        const float climb = std::clamp(t / 0.72F, 0.0F, 1.0F);
        const float victory = std::clamp((t - 0.72F) / 0.28F, 0.0F, 1.0F);
        const float fx = botX, fy = botY + (topY - botY) * climb;
        const float ht = mn * 0.14F;
        const Rgb body{30, 26, 30, false};
        plotDot(dst, w, h, fx, fy - ht * 1.0F, ht * 0.13F, ya, body);
        drawSeg(dst, w, h, fx, fy - ht * 0.92F, fx, fy - ht * 0.40F, ht * 0.12F, ya, body);
        if (victory < 0.05F)
        {
          const float stride = std::sin(t * 16.0F) * ht * 0.20F;
          drawSeg(dst, w, h, fx, fy - ht * 0.40F, fx - stride, fy, ht * 0.10F, ya, body);
          drawSeg(dst, w, h, fx, fy - ht * 0.40F, fx + stride, fy, ht * 0.10F, ya, body);
          const float armSwing = std::sin(t * 16.0F + 1.5708F) * ht * 0.18F;
          drawSeg(dst, w, h, fx, fy - ht * 0.78F, fx + armSwing, fy - ht * 0.52F, ht * 0.08F, ya,
                  body);
          drawSeg(dst, w, h, fx, fy - ht * 0.78F, fx - armSwing, fy - ht * 0.52F, ht * 0.08F, ya,
                  body);
        }
        else
        {
          const float vh = victory * ht * 0.6F;
          drawSeg(dst, w, h, fx, fy - ht * 0.40F, fx - ht * 0.10F, fy, ht * 0.10F, ya, body);
          drawSeg(dst, w, h, fx, fy - ht * 0.40F, fx + ht * 0.10F, fy, ht * 0.10F, ya, body);
          drawSeg(dst, w, h, fx, fy - ht * 0.78F, fx - ht * 0.22F, fy - ht * 1.10F - vh, ht * 0.08F,
                  ya, body);
          drawSeg(dst, w, h, fx, fy - ht * 0.78F, fx + ht * 0.22F, fy - ht * 1.10F - vh, ht * 0.08F,
                  ya, body);
        }
      });
}

// Effect roster. Keep in sync with the dispatch switch below and with the
// names in exitEffectName().
constexpr int kEffectCount = 105;
}  // namespace

int exitEffectCount()
{
  return kEffectCount;
}

const char* exitEffectName(int effectIndex)
{
  static const char* const kNames[kEffectCount] = {"explode",
                                                   "implode + ring",
                                                   "spiral",
                                                   "matrix drop",
                                                   "dissolve",
                                                   "CRT off",
                                                   "fade",
                                                   "fire",
                                                   "fireworks",
                                                   "Pac-Man",
                                                   "train",
                                                   "The End",
                                                   "eye wink",
                                                   "submarine",
                                                   "Bond barrel",
                                                   "teardrop",
                                                   "tears in rain",
                                                   "word reveal",
                                                   "koyaanisqatsi",
                                                   "rosebud",
                                                   "shiver",
                                                   "HAL 9000",
                                                   "Rubik",
                                                   "foot stomp",
                                                   "Monty Python",
                                                   "tunnel",
                                                   "gunshot",
                                                   "snowfall",
                                                   "gotcha",
                                                   "Star Wars",
                                                   "Python wars",
                                                   "Flatland",
                                                   "test card",
                                                   "doves",
                                                   "aurora",
                                                   "thunderstorm",
                                                   "game of life",
                                                   "Thanos snap",
                                                   "Utopia",
                                                   "bouncing ball",
                                                   "solar system",
                                                   "Saturn",
                                                   "black hole",
                                                   "Interstellar",
                                                   "stingray",
                                                   "jellyfish",
                                                   "butterfly",
                                                   "fish",
                                                   "crab",
                                                   "spider",
                                                   "riverdance",
                                                   "Pac-Man duel",
                                                   "tornado",
                                                   "Thriller",
                                                   "YMCA",
                                                   "skeleton wave",
                                                   "Greek dance",
                                                   "Russian dance",
                                                   "ballet",
                                                   "fall to pieces",
                                                   "Macarena",
                                                   "Neo",
                                                   "Truman",
                                                   "moon rocket",
                                                   "film burn",
                                                   "E.T.",
                                                   "Thelma & Louise",
                                                   "DeLorean",
                                                   "Up",
                                                   "countdown",
                                                   "iris out",
                                                   "Lawrence",
                                                   "Jurassic",
                                                   "standoff",
                                                   "Jaws",
                                                   "Star Gate",
                                                   "Close Encounters",
                                                   "Titanic",
                                                   "Inception",
                                                   "Vertigo",
                                                   "Psycho",
                                                   "feather",
                                                   "red balloon",
                                                   "Mary Poppins",
                                                   "warp",
                                                   "monolith",
                                                   "Tron",
                                                   "boulder",
                                                   "Apocalypse",
                                                   "King Kong",
                                                   "Wizard of Oz",
                                                   "Nosferatu",
                                                   "the Birds",
                                                   "Pulp Fiction",
                                                   "Strangelove",
                                                   "Akira",
                                                   "Clockwork",
                                                   "newspaper",
                                                   "end card",
                                                   "Shining",
                                                   "Dial M",
                                                   "Bass spiral",
                                                   "Lebowski",
                                                   "Singin'",
                                                   "Rocky"};
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
    case 50:
      effectRiverdance(renderer, frame, subW, subH);
      break;
    case 51:
      effectPacmanDuel(renderer, frame, subW, subH);
      break;
    case 52:
      effectTornado(renderer, frame, subW, subH);
      break;
    case 53:
      effectThriller(renderer, frame, subW, subH);
      break;
    case 54:
      effectYMCA(renderer, frame, subW, subH);
      break;
    case 55:
      effectSkeletonWave(renderer, frame, subW, subH);
      break;
    case 56:
      effectGreekDance(renderer, frame, subW, subH);
      break;
    case 57:
      effectRussianDance(renderer, frame, subW, subH);
      break;
    case 58:
      effectBallet(renderer, frame, subW, subH);
      break;
    case 59:
      effectFallToPieces(renderer, frame, subW, subH);
      break;
    case 60:
      effectMacarena(renderer, frame, subW, subH);
      break;
    case 61:
      effectNeo(renderer, frame, subW, subH);
      break;
    case 62:
      effectTruman(renderer, frame, subW, subH);
      break;
    case 63:
      effectMoonRocket(renderer, frame, subW, subH);
      break;
    case 64:
      effectFilmBurn(renderer, frame, subW, subH);
      break;
    case 65:
      effectET(renderer, frame, subW, subH);
      break;
    case 66:
      effectThelma(renderer, frame, subW, subH);
      break;
    case 67:
      effectDeLorean(renderer, frame, subW, subH);
      break;
    case 68:
      effectUp(renderer, frame, subW, subH);
      break;
    case 69:
      effectCountdown(renderer, frame, subW, subH);
      break;
    case 70:
      effectIrisOut(renderer, frame, subW, subH);
      break;
    case 71:
      effectLawrence(renderer, frame, subW, subH);
      break;
    case 72:
      effectJurassic(renderer, frame, subW, subH);
      break;
    case 73:
      effectStandoff(renderer, frame, subW, subH);
      break;
    case 74:
      effectJaws(renderer, frame, subW, subH);
      break;
    case 75:
      effectStarGate(renderer, frame, subW, subH);
      break;
    case 76:
      effectCloseEncounters(renderer, frame, subW, subH);
      break;
    case 77:
      effectTitanic(renderer, frame, subW, subH);
      break;
    case 78:
      effectInception(renderer, frame, subW, subH);
      break;
    case 79:
      effectVertigo(renderer, frame, subW, subH);
      break;
    case 80:
      effectPsycho(renderer, frame, subW, subH);
      break;
    case 81:
      effectFeather(renderer, frame, subW, subH);
      break;
    case 82:
      effectRedBalloon(renderer, frame, subW, subH);
      break;
    case 83:
      effectMaryPoppins(renderer, frame, subW, subH);
      break;
    case 84:
      effectWarp(renderer, frame, subW, subH);
      break;
    case 85:
      effectMonolith(renderer, frame, subW, subH);
      break;
    case 86:
      effectTron(renderer, frame, subW, subH);
      break;
    case 87:
      effectBoulder(renderer, frame, subW, subH);
      break;
    case 88:
      effectApocalypse(renderer, frame, subW, subH);
      break;
    case 89:
      effectKong(renderer, frame, subW, subH);
      break;
    case 90:
      effectOz(renderer, frame, subW, subH);
      break;
    case 91:
      effectNosferatu(renderer, frame, subW, subH);
      break;
    case 92:
      effectBirds(renderer, frame, subW, subH);
      break;
    case 93:
      effectPulp(renderer, frame, subW, subH);
      break;
    case 94:
      effectStrangelove(renderer, frame, subW, subH);
      break;
    case 95:
      effectAkira(renderer, frame, subW, subH);
      break;
    case 96:
      effectClockwork(renderer, frame, subW, subH);
      break;
    case 97:
      effectNewspaper(renderer, frame, subW, subH);
      break;
    case 98:
      effectEndCard(renderer, frame, subW, subH);
      break;
    case 99:
      effectShining(renderer, frame, subW, subH);
      break;
    case 100:
      effectDialM(renderer, frame, subW, subH);
      break;
    case 101:
      effectBassSpiral(renderer, frame, subW, subH);
      break;
    case 102:
      effectLebowski(renderer, frame, subW, subH);
      break;
    case 103:
      effectSinginRain(renderer, frame, subW, subH);
      break;
    case 104:
      effectRocky(renderer, frame, subW, subH);
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
