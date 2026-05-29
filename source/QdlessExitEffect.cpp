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

// Screen-round, data-textured disk tinted with `tint`. Cell aspect is folded
// into the y-radius so the result reads as a circle on the terminal — matching
// plotDot's geometry — but with the data wrapped on a lit sphere instead of a
// flat fill. Lets the sun/moon/balloon disks across the effect catalogue carry
// the underlying view as their surface.
inline void drawDataDisk(std::vector<Rgb>& dst,
                         int w,
                         int h,
                         const std::vector<Rgb>& src,
                         float cx,
                         float cy,
                         float r,
                         float ya,
                         float shade,
                         float spin,
                         const Rgb& tint)
{
  drawSphere(dst,
             w,
             h,
             src,
             cx,
             cy,
             r,
             r / ya,
             shade,
             spin,
             tint.r / 255.0F,
             tint.g / 255.0F,
             tint.b / 255.0F);
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
        auto fillRectData = [&](float x0, float y0, float x1, float y1, const Rgb& base, float kBase)
        {
          for (int yy = std::max(0, static_cast<int>(y0));
               yy <= std::min(h - 1, static_cast<int>(y1));
               ++yy)
            for (int xx = std::max(0, static_cast<int>(x0));
                 xx <= std::min(w - 1, static_cast<int>(x1));
                 ++xx)
            {
              const Rgb d = sample(src, w, h, xx, yy);
              const float dr = d.transparent ? 120.0F : d.r;
              const float dg = d.transparent ? 120.0F : d.g;
              const float db = d.transparent ? 120.0F : d.b;
              dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{u8(base.r * kBase + dr * (1 - kBase)),
                                                               u8(base.g * kBase + dg * (1 - kBase)),
                                                               u8(base.b * kBase + db * (1 - kBase)),
                                                               false};
            }
        };
        for (int i = 0; i < kSteps; ++i)  // staircase — risers + treads wear the data
        {
          const float f0 = static_cast<float>(i) / kSteps;
          const float f1 = static_cast<float>(i + 1) / kSteps;
          const float sx = stairX0 + (stairX1 - stairX0) * f0;
          const float sx2 = stairX0 + (stairX1 - stairX0) * f1;
          const float sy = stairY0 + (doorSill - stairY0) * f1;
          fillRectData(sx, sy, sx2 + 1, stairY0, Rgb{150, 146, 140, false}, 0.62F);
          fillRectData(
              sx, sy, sx2 + 1, sy + std::max(1.0F, h * 0.012F), Rgb{212, 208, 198, false}, 0.72F);
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
        drawDataDisk(dst,
                     w,
                     h,
                     src,
                     moonCx,
                     moonCy,
                     moonR,
                     ya,
                     0.70F,
                     t * 0.25F,
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
              drawDataDisk(dst,
                           w,
                           h,
                           src,
                           moonCx,
                           moonCy,
                           moonR,
                           ya,
                           0.70F,
                           t * 0.25F,
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
          drawDataDisk(dst,
                       w,
                       h,
                       src,
                       bx,
                       by,
                       mn * 0.03F,
                       ya,
                       0.85F,
                       hash(i * 5) * 6.2832F + t,
                       kBalloon[i % 6]);
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
        // Walls and roof carry the data: each pixel samples the underlying
        // view and modulates the tan/red tint, so as the house rises into the
        // air it visibly wears the weather instead of a flat fill.
        {
          const int wx0 = std::max(0, static_cast<int>(hx - bwd));
          const int wx1 = std::min(w - 1, static_cast<int>(hx + bwd));
          const int wy0 = std::max(0, static_cast<int>(hy - bht));
          const int wy1 = std::min(h - 1, static_cast<int>(hy));
          for (int y = wy0; y <= wy1; ++y)
            for (int x = wx0; x <= wx1; ++x)
            {
              const Rgb d = sample(src, w, h, x, y);
              const float dr = d.transparent ? 120.0F : d.r;
              const float dg = d.transparent ? 120.0F : d.g;
              const float db = d.transparent ? 120.0F : d.b;
              dst[static_cast<std::size_t>(y) * w + x] = Rgb{u8(225 * 0.62F + dr * 0.30F),
                                                             u8(210 * 0.62F + dg * 0.30F),
                                                             u8(175 * 0.62F + db * 0.28F),
                                                             false};
            }
        }
        for (int y = 0; y <= static_cast<int>(bht * 0.9F); ++y)  // roof triangle
        {
          const float frac = 1.0F - y / (bht * 0.9F);
          const float rwd = bwd * 1.15F * frac;
          const float ry = hy - bht - y;
          const int ryi = static_cast<int>(ry);
          if (ryi < 0 || ryi >= h)
            continue;
          const int rx0 = std::max(0, static_cast<int>(hx - rwd));
          const int rx1 = std::min(w - 1, static_cast<int>(hx + rwd));
          for (int x = rx0; x <= rx1; ++x)
          {
            const Rgb d = sample(src, w, h, x, ry);
            const float dr = d.transparent ? 100.0F : d.r;
            const float dg = d.transparent ? 100.0F : d.g;
            const float db = d.transparent ? 100.0F : d.b;
            dst[static_cast<std::size_t>(ryi) * w + x] = Rgb{u8(170 * 0.65F + dr * 0.32F),
                                                             u8(70 * 0.65F + dg * 0.25F),
                                                             u8(55 * 0.65F + db * 0.22F),
                                                             false};
          }
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
          drawDataDisk(
              dst, w, h, src, sunX, sunY, 0.17F * mn, ya, 0.55F, t * 0.3F, Rgb{255, 232, 165, false});
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
        // The whole-screen close-up: skin tone modulated by the data so the
        // weather rides the cheek instead of a flat fill — soft enough that
        // the face still reads.
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const float vg = 1.0F - 0.4F * std::hypot((x - w * 0.5F) / w, (y - h * 0.5F) / h);
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 80.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            const float k = (l - 128.0F) / 255.0F;  // -0.5..+0.5
            dst[static_cast<std::size_t>(y) * w + x] = Rgb{u8(skin.r * vg + k * 60.0F),
                                                           u8(skin.g * vg + k * 50.0F),
                                                           u8(skin.b * vg + k * 30.0F),
                                                           false};
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
          drawDataDisk(dst,
                       w,
                       h,
                       src,
                       w * 0.5F,
                       shipCy,
                       shipR,
                       ya,
                       0.85F,
                       t * 0.20F,
                       Rgb{50, 54, 70, false});  // hull, data-wrapped
          drawDataDisk(dst,
                       w,
                       h,
                       src,
                       w * 0.5F,
                       shipCy,
                       shipR * 0.34F,
                       ya,
                       0.30F,
                       t * 0.60F,
                       Rgb{u8(220 * halo + 35),
                           u8(225 * halo + 35),
                           u8(255 * halo + 45),
                           false});  // bright core
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
                             : Rgb{u8(248 * 0.68F + dr * 0.32F),
                                   u8(248 * 0.68F + dg * 0.32F),
                                   u8(250 * 0.68F + db * 0.30F),
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
              drawDataDisk(dst,
                           w,
                           h,
                           src,
                           monoCx,
                           sunY,
                           mn * 0.13F,
                           ya,
                           0.50F,
                           t * 0.3F,
                           Rgb{u8(255), u8(238), u8(180 + 60 * flare), false});  // sun
              const float crY = sunY - mn * 0.36F;                               // crescent above
              drawDataDisk(dst,
                           w,
                           h,
                           src,
                           monoCx,
                           crY,
                           mn * 0.075F,
                           ya,
                           0.75F,
                           t * 0.4F,
                           Rgb{220, 222, 210, false});
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
        drawDataDisk(dst,
                     w,
                     h,
                     src,
                     sunX,
                     sunY,
                     0.16F * mn,
                     ya,
                     0.55F,
                     t * 0.25F,
                     Rgb{255, 180, 90, false});  // hazy sun
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
        drawDataDisk(dst,
                     w,
                     h,
                     src,
                     towerX,
                     towerTop - mn * 0.02F,
                     mn * 0.17F,
                     ya,
                     0.70F,
                     t * 0.2F,
                     Rgb{206, 206, 198, false});
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
                const float bx = hx + std::sin(ang) * mn * 0.05F;
                const float by = handBase - mn * 0.04F;
                const float tx = hx + std::sin(ang) * fl;
                const float tyy = handBase - std::cos(ang) * fl;
                // Tapered finger: thick at the knuckle, narrowing to a needle
                // point. Stamping shrinking dots along the line gives a smooth
                // bevel that drawSeg's uniform stroke can't.
                constexpr int kSteps = 24;
                for (int k = 0; k <= kSteps; ++k)
                {
                  const float u = static_cast<float>(k) / kSteps;
                  const float px = bx + (tx - bx) * u;
                  const float py = by + (tyy - by) * u;
                  const float rad = std::max(0.6F, mn * 0.028F * (1.0F - u * 0.95F));
                  plotDot(dst, w, h, px, py, rad, ya, shadow);
                }
                // Sharp continuation past the tip — a hairline barb that gives
                // the claw a distinct point rather than ending in a stub.
                const float barb = mn * 0.045F;
                drawSeg(dst,
                        w,
                        h,
                        tx,
                        tyy,
                        tx + std::sin(ang) * barb,
                        tyy - std::cos(ang) * barb,
                        std::max(1.0F, mn * 0.006F),
                        ya,
                        shadow);
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
          // Perspective flood: the blood pours from the door at the
          // vanishing point and floods the corridor floor. Its footprint
          // is the 1-point-perspective wedge — apex at the door rim
          // (cxw, horizon+dh/2), widening linearly to the bottom screen
          // corners at the camera. As the front advances from the door
          // toward the camera (yFront from horizon → h), the wedge fills
          // out into a trapezoid.
          const float doorRimY = horizon + dh * 0.5F;
          const float floorH = std::max(1.0F, static_cast<float>(h) - doorRimY);
          const float doorHalfW = dw * 0.5F;
          const float yFront = doorRimY + (static_cast<float>(h) - doorRimY) * flow;
          const int yLo = static_cast<int>(doorRimY) + 1;
          const int yHi = std::min(h - 1, static_cast<int>(yFront));
          for (int y = yLo; y <= yHi; ++y)
          {
            const float t01 = std::clamp((y - doorRimY) / floorH, 0.0F, 1.0F);
            const float halfWedge = doorHalfW + t01 * (w * 0.5F - doorHalfW);
            const int xLo = std::max(0, static_cast<int>(std::round(cxw - halfWedge)));
            const int xHi = std::min(w - 1, static_cast<int>(std::round(cxw + halfWedge)));
            for (int x = xLo; x <= xHi; ++x)
            {
              const float turb = 0.85F + 0.15F * std::sin((x - y) * 0.3F + t * 8.0F);
              // Deeper red toward the leading edge, lighter pooled blood
              // toward the door — gives the flood a sense of direction.
              const float depthOfPool = std::clamp(t01, 0.0F, 1.0F);
              const Rgb& base = dst[static_cast<std::size_t>(y) * w + x];
              dst[static_cast<std::size_t>(y) * w + x] =
                  Rgb{u8((180 + 60 * depthOfPool) * turb + base.r * 0.10F),
                      u8(14 * turb + base.g * 0.05F),
                      u8(14 * turb + base.b * 0.05F),
                      false};
            }
          }
          // Bright leading edge along the front, also clipped to the
          // wedge so it never crosses into the wall region.
          if (yHi > yLo)
          {
            const float t01 = std::clamp((yFront - doorRimY) / floorH, 0.0F, 1.0F);
            const float halfWedgeF = doorHalfW + t01 * (w * 0.5F - doorHalfW);
            const int xLoF = std::max(0, static_cast<int>(std::round(cxw - halfWedgeF)));
            const int xHiF = std::min(w - 1, static_cast<int>(std::round(cxw + halfWedgeF)));
            for (int x = xLoF; x <= xHiF; ++x)
            {
              const int yj =
                  static_cast<int>(yFront + std::sin(x * 0.4F + t * 6.0F) * 1.2F);
              if (yj >= 0 && yj < h)
                dst[static_cast<std::size_t>(yj) * w + x] = Rgb{255, 130, 130, false};
            }
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
        drawDataDisk(dst,
                     w,
                     h,
                     src,
                     cx,
                     cy,
                     R + holeR * 0.2F,
                     ya,
                     0.55F,
                     0.0F,
                     Rgb{215, 208, 195, false});  // bakelite face wraps the data
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
  runFrames(renderer,
            w,
            h,
            4800,
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
                    const Rgb d = sample(
                        src, w, h, (dx / irisR * 0.5F + 0.5F) * w, (dy / irisR * 0.5F + 0.5F) * h);
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
  static const Pin pins[] = {{0.0F, 0.0F},
                             {-0.18F, 0.22F},
                             {0.18F, 0.22F},
                             {-0.34F, 0.44F},
                             {0.0F, 0.44F},
                             {0.34F, 0.44F},
                             {-0.50F, 0.66F},
                             {-0.17F, 0.66F},
                             {0.17F, 0.66F},
                             {0.50F, 0.66F}};
  runFrames(
      renderer,
      w,
      h,
      4800,
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
                out = Rgb{u8(180 + l * 0.20F + plank * 30),
                          u8(120 + l * 0.16F + plank * 20),
                          u8(60 + l * 0.10F),
                          false};
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
            pwu += (pins[i].wu < 0 ? -1.0F : (pins[i].wu > 0 ? 1.0F : (hash(i) - 0.5F))) * strike *
                   spd;
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
                         u8(dst[k].b + (255 - dst[k].b) * flash),
                         false};
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
  runFrames(renderer,
            w,
            h,
            5400,
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
                  dst[static_cast<std::size_t>(y) * w + x] = Rgb{u8(16 + l * 0.10F + glow * 130),
                                                                 u8(24 + l * 0.12F + glow * 100),
                                                                 u8(38 + l * 0.18F + glow * 60),
                                                                 false};
                }
              drawSeg(dst,
                      w,
                      h,
                      postX,
                      h * 0.95F,
                      postX,
                      h * 0.22F,
                      std::max(1.0F, mn * 0.014F),
                      ya,
                      Rgb{18, 16, 18, false});
              plotDot(dst, w, h, lantX, lantY, mn * 0.06F, ya, Rgb{255, 230, 150, false});
              const float barX = postX - mn * 0.16F, barY = h * 0.26F;
              drawSeg(dst,
                      w,
                      h,
                      postX,
                      barY,
                      barX,
                      barY,
                      std::max(1.0F, mn * 0.012F),
                      ya,
                      Rgb{18, 16, 18, false});
              const float swing = std::sin(t * 2.2F) * 0.45F;
              const float L = mn * 0.20F;
              const float bodyX = barX + std::sin(swing) * L, bodyY = barY + std::cos(swing) * L;
              const Rgb suit{30, 26, 32, false};
              drawSeg(dst, w, h, barX, barY, bodyX, bodyY, std::max(1.0F, mn * 0.020F), ya, suit);
              plotDot(dst,
                      w,
                      h,
                      bodyX + std::sin(swing) * mn * 0.04F,
                      bodyY + std::cos(swing) * mn * 0.04F,
                      mn * 0.04F,
                      ya,
                      Rgb{210, 180, 150, false});
              const float kick = std::sin(t * 4.0F) * 0.5F;
              const float legX = bodyX + std::sin(swing) * mn * 0.18F;
              const float legY = bodyY + std::cos(swing) * mn * 0.18F;
              drawSeg(dst,
                      w,
                      h,
                      bodyX,
                      bodyY,
                      legX - kick * mn * 0.08F,
                      legY,
                      std::max(1.0F, mn * 0.014F),
                      ya,
                      suit);
              drawSeg(dst,
                      w,
                      h,
                      bodyX,
                      bodyY,
                      legX + kick * mn * 0.08F,
                      legY,
                      std::max(1.0F, mn * 0.014F),
                      ya,
                      suit);
              const float uHand = bodyX - mn * 0.10F, uHandY = bodyY - mn * 0.05F;
              drawSeg(dst,
                      w,
                      h,
                      bodyX,
                      bodyY - mn * 0.04F,
                      uHand,
                      uHandY,
                      std::max(1.0F, mn * 0.012F),
                      ya,
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
                drawSeg(dst,
                        w,
                        h,
                        rx,
                        ry,
                        rx - mn * 0.018F,
                        ry + mn * 0.04F,
                        std::max(1.0F, mn * 0.005F),
                        ya,
                        Rgb{170, 200, 230, false});
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
      renderer,
      w,
      h,
      5400,
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
                    u8(120 - 60 * sf + l * 0.10F + glow * 30),
                    false};
          }
        drawDataDisk(dst,
                     w,
                     h,
                     src,
                     sunX,
                     sunY,
                     mn * 0.10F * (0.6F + 0.4F * sun),
                     ya,
                     0.55F,
                     t * 0.3F,
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
          drawSeg(
              dst, w, h, fx, fy - ht * 0.78F, fx + armSwing, fy - ht * 0.52F, ht * 0.08F, ya, body);
          drawSeg(
              dst, w, h, fx, fy - ht * 0.78F, fx - armSwing, fy - ht * 0.52F, ht * 0.08F, ya, body);
        }
        else
        {
          const float vh = victory * ht * 0.6F;
          drawSeg(dst, w, h, fx, fy - ht * 0.40F, fx - ht * 0.10F, fy, ht * 0.10F, ya, body);
          drawSeg(dst, w, h, fx, fy - ht * 0.40F, fx + ht * 0.10F, fy, ht * 0.10F, ya, body);
          drawSeg(dst,
                  w,
                  h,
                  fx,
                  fy - ht * 0.78F,
                  fx - ht * 0.22F,
                  fy - ht * 1.10F - vh,
                  ht * 0.08F,
                  ya,
                  body);
          drawSeg(dst,
                  w,
                  h,
                  fx,
                  fy - ht * 0.78F,
                  fx + ht * 0.22F,
                  fy - ht * 1.10F - vh,
                  ht * 0.08F,
                  ya,
                  body);
        }
      });
}

// 2001 bone-to-spaceship match cut: the ape hurls a thigh-bone spinning into a
// dim savanna sky (the data tinted ochre) — and on the cut the bone has become
// a slender spaceship adrift in starlit space, on exactly the same axis.
void effectBoneCut(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float cutT = 0.56F;
  auto drawCapsule =
      [&](std::vector<Rgb>& dst, float cx, float cy, float L, float R, float ang, const Rgb& col)
  {
    const float dx = std::cos(ang) * L * 0.5F, dy = std::sin(ang) * L * 0.5F;
    drawSeg(dst, w, h, cx - dx, cy - dy, cx + dx, cy + dy, R, ya, col);
    plotDot(dst, w, h, cx - dx, cy - dy, R * 1.25F, ya, col);
    plotDot(dst, w, h, cx + dx, cy + dy, R * 1.25F, ya, col);
  };
  runFrames(
      renderer,
      w,
      h,
      5200,
      [&](float t, std::vector<Rgb>& dst)
      {
        if (t < cutT)  // the throw across the savanna sky
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
                      : Rgb{u8(140 + 60 * sf + l * 0.15F),
                            u8(95 + 60 * sf + l * 0.12F),
                            u8(70 + 40 * sf + l * 0.10F),
                            false};
            }
          const float p = t / cutT;
          const float bx = w * (0.30F + 0.45F * p);
          const float by = h * 0.78F - std::sin(p * 3.14159F) * h * 0.55F;
          const float ang = p * 18.0F + 0.6F;  // spinning fast
          const float L = mn * 0.22F, R = mn * 0.022F;
          drawCapsule(dst, bx, by, L, R, ang, Rgb{240, 235, 222, false});
          plotDot(dst,
                  w,
                  h,
                  bx + std::cos(ang) * L * 0.5F,
                  by + std::sin(ang) * L * 0.5F,
                  R * 1.6F,
                  ya,
                  Rgb{240, 235, 222, false});  // knob ends slightly bigger
          plotDot(dst,
                  w,
                  h,
                  bx - std::cos(ang) * L * 0.5F,
                  by - std::sin(ang) * L * 0.5F,
                  R * 1.6F,
                  ya,
                  Rgb{240, 235, 222, false});
        }
        else  // the cut: same axis, now a spaceship in space
        {
          for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
              dst[static_cast<std::size_t>(y) * w + x] = Rgb{2, 3, 7, false};
          for (int i = 0; i < 90; ++i)  // stars seeded from the data's bright pixels
          {
            const int sx = static_cast<int>(hash(i * 2) * w),
                      sy = static_cast<int>(hash(i * 2 + 1) * h);
            if (sx < 0 || sx >= w || sy < 0 || sy >= h)
              continue;
            const Rgb& s = src[static_cast<std::size_t>(sy) * w + sx];
            const float l = s.transparent ? 100.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            if (l < 90.0F)
              continue;
            const float tw = 0.5F + 0.5F * std::sin(t * 6.0F + i);
            dst[static_cast<std::size_t>(sy) * w + sx] =
                Rgb{u8(190 + tw * 50), u8(190 + tw * 50), u8(210 + tw * 40), false};
          }
          const float p = (t - cutT) / (1.0F - cutT);
          const float bx = w * (0.74F + 0.08F * p), by = h * 0.20F + 6.0F * std::sin(p * 4.0F);
          const float ang = 0.6F + p * 0.4F;  // slow spin
          const float L = mn * 0.30F, R = mn * 0.030F;
          drawCapsule(dst, bx, by, L, R, ang, Rgb{200, 200, 215, false});
          // two fins near the tail
          const float fdx = std::cos(ang), fdy = std::sin(ang);
          const float tx = bx - fdx * L * 0.40F, ty = by - fdy * L * 0.40F;
          drawSeg(dst,
                  w,
                  h,
                  tx,
                  ty,
                  tx - fdy * mn * 0.04F,
                  ty + fdx * mn * 0.04F,
                  R * 0.6F,
                  ya,
                  Rgb{160, 160, 180, false});
          drawSeg(dst,
                  w,
                  h,
                  tx,
                  ty,
                  tx + fdy * mn * 0.04F,
                  ty - fdx * mn * 0.04F,
                  R * 0.6F,
                  ya,
                  Rgb{160, 160, 180, false});
          plotDot(dst,
                  w,
                  h,
                  bx + fdx * L * 0.5F,
                  by + fdy * L * 0.5F,
                  R * 1.6F,
                  ya,
                  Rgb{255, 245, 200, false});  // bright nose lamp
        }
      });
}

// The Lion King — Pride Rock: dawn breaks across a savanna (the data tinted
// gold), the silhouetted rock rising at centre while a figure lifts a cub aloft
// as the sun climbs behind.
void effectPrideRock(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float horizonY = h * 0.66F, rockTop = h * 0.32F;
  runFrames(renderer,
            w,
            h,
            5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float sun = std::clamp((t - 0.12F) / 0.70F, 0.0F, 1.0F);
              const float sunX = w * 0.50F, sunY = horizonY - sun * h * 0.34F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 80.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  const float sf = static_cast<float>(y) / horizonY;
                  const float gd = std::hypot(x - sunX, (y - sunY) * ya);
                  const float glow = std::exp(-gd / (mn * 0.55F)) * sun;
                  Rgb c;
                  if (y > horizonY)  // dark savanna foreground
                    c = Rgb{u8(30 + l * 0.10F + glow * 60),
                            u8(22 + l * 0.08F + glow * 50),
                            u8(18 + l * 0.06F),
                            false};
                  else
                    c = Rgb{u8(140 + 110 * (1 - sf) + glow * 110 + l * 0.10F),
                            u8(70 + 90 * (1 - sf) + glow * 80 + l * 0.10F),
                            u8(40 + 30 * sf + glow * 30),
                            false};
                  dst[static_cast<std::size_t>(y) * w + x] = c;
                }
              drawDataDisk(dst,
                           w,
                           h,
                           src,
                           sunX,
                           sunY,
                           mn * 0.13F,
                           ya,
                           0.55F,
                           t * 0.3F,
                           Rgb{255, 220, 140, false});  // sun
              // Pride Rock silhouette: a tilted boulder rising from the horizon
              const float rx0 = w * 0.32F, rx1 = w * 0.68F;  // base
              const float rtx = w * 0.56F;                   // tip
              for (int y = static_cast<int>(rockTop); y <= static_cast<int>(horizonY) + 2; ++y)
              {
                const float f = (y - rockTop) / (horizonY - rockTop);
                const float lx = rx0 + (rtx - mn * 0.04F) * (1.0F - f) * 0.0F;
                (void)lx;
                // left silhouette edge ramps from tip to base
                const float xl = rtx - (rtx - rx0) * f;
                const float xr = rtx + (rx1 - rtx) * f;
                for (int x = static_cast<int>(xl); x <= static_cast<int>(xr); ++x)
                  if (x >= 0 && x < w)
                    dst[static_cast<std::size_t>(y) * w + x] = Rgb{18, 14, 10, false};
              }
              // figure on the tip with the cub raised
              const float lift = std::clamp((t - 0.35F) / 0.45F, 0.0F, 1.0F);
              const float fcx = rtx, fy = rockTop;
              const Rgb fig{6, 6, 8, false};
              plotDot(dst, w, h, fcx, fy - mn * 0.020F, mn * 0.018F, ya, fig);  // head
              drawSeg(
                  dst, w, h, fcx, fy - mn * 0.010F, fcx, fy + mn * 0.030F, mn * 0.012F, ya, fig);
              // arms reaching up holding the cub
              const float ay = fy - mn * 0.020F - lift * mn * 0.10F;
              drawSeg(dst,
                      w,
                      h,
                      fcx - mn * 0.015F,
                      fy - mn * 0.005F,
                      fcx - mn * 0.020F,
                      ay,
                      std::max(1.0F, mn * 0.010F),
                      ya,
                      fig);
              drawSeg(dst,
                      w,
                      h,
                      fcx + mn * 0.015F,
                      fy - mn * 0.005F,
                      fcx + mn * 0.020F,
                      ay,
                      std::max(1.0F, mn * 0.010F),
                      ya,
                      fig);
              if (lift > 0.05F)
                plotDot(dst,
                        w,
                        h,
                        fcx,
                        ay - mn * 0.012F,
                        mn * 0.022F * lift + 1.0F,
                        ya,
                        Rgb{u8(45), u8(34), u8(22), false});  // cub
            });
}

// Pleasantville: the data desaturates to monochrome, a single seed of red appears
// at centre, and colour bleeds outward from it — reclaiming the whole frame —
// before the final fade.
void effectPleasantville(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float seedX = w * 0.50F, seedY = h * 0.58F;
  runFrames(renderer,
            w,
            h,
            5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float desat = std::clamp(t / 0.28F, 0.0F, 1.0F);
              const float spread = std::clamp((t - 0.42F) / 0.46F, 0.0F, 1.0F);
              const float cR = spread * std::hypot(static_cast<float>(w), h * ya);
              const float fade = std::clamp((t - 0.92F) / 0.08F, 0.0F, 1.0F);
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 0.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  const float br = s.transparent ? 0 : s.r, bg = s.transparent ? 0 : s.g,
                              bb = s.transparent ? 0 : s.b;
                  const float d = std::hypot(x - seedX, (y - seedY) * ya);
                  // colour mix: how much colour does this pixel keep?
                  const float colorAmt = std::max(
                      1.0F - desat, std::clamp((cR - d) / (mn * 0.10F + 0.5F), 0.0F, 1.0F));
                  float r = l * (1 - colorAmt) + br * colorAmt;
                  float g = l * (1 - colorAmt) + bg * colorAmt;
                  float b = l * (1 - colorAmt) + bb * colorAmt;
                  if (desat >= 1.0F && spread > 0.0F && d < mn * 0.025F)  // the red seed bloom
                  {
                    const float k = 1.0F - d / (mn * 0.025F);
                    r = r + (255 - r) * k;
                    g = g * (1 - k);
                    b = b * (1 - k);
                  }
                  r *= (1 - fade);
                  g *= (1 - fade);
                  b *= (1 - fade);
                  dst[static_cast<std::size_t>(y) * w + x] = Rgb{u8(r), u8(g), u8(b), false};
                }
            });
}

// Tatooine binary sunset: the data fades into a desert dusk; two suns sink
// side-by-side over a sand horizon while a figure silhouette stands watching.
void effectTatooine(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float horizonY = h * 0.62F;
  runFrames(
      renderer,
      w,
      h,
      5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float setp = std::clamp(t / 0.85F, 0.0F, 1.0F);
        const float sunY = horizonY - mn * 0.10F + setp * mn * 0.18F;  // both descend
        const float sepX = mn * 0.20F;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 70.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            const float gd1 = std::hypot(x - (w * 0.5F - sepX), (y - sunY) * ya);
            const float gd2 = std::hypot(x - (w * 0.5F + sepX), (y - sunY + mn * 0.02F) * ya);
            const float glow =
                std::exp(-gd1 / (mn * 0.40F)) + std::exp(-gd2 / (mn * 0.32F)) * 0.85F;
            Rgb c;
            if (y > horizonY + std::sin(x * 0.08F) * h * 0.012F)  // sand
              c = Rgb{u8(120 + l * 0.16F + glow * 60),
                      u8(75 + l * 0.12F + glow * 40),
                      u8(40 + l * 0.06F),
                      false};
            else  // dusk sky
            {
              const float sf = static_cast<float>(y) / horizonY;
              c = Rgb{u8(80 + 175 * sf + glow * 70 + l * 0.10F),
                      u8(50 + 90 * sf + glow * 70 + l * 0.08F),
                      u8(95 - 35 * sf + glow * 25 + l * 0.06F),
                      false};
            }
            dst[static_cast<std::size_t>(y) * w + x] = c;
          }
        drawDataDisk(dst,
                     w,
                     h,
                     src,
                     w * 0.5F - sepX,
                     sunY,
                     mn * 0.11F,
                     ya,
                     0.55F,
                     t * 0.3F,
                     Rgb{255, 215, 130, false});
        drawDataDisk(dst,
                     w,
                     h,
                     src,
                     w * 0.5F + sepX,
                     sunY + mn * 0.02F,
                     mn * 0.085F,
                     ya,
                     0.55F,
                     t * 0.45F,
                     Rgb{255, 175, 90, false});  // smaller, slightly lower sister sun
        // a small figure silhouette in the foreground
        const float fx = w * 0.30F, fy = horizonY + mn * 0.08F, ht = mn * 0.10F;
        const Rgb fig{6, 6, 10, false};
        plotDot(dst, w, h, fx, fy - ht * 1.0F, ht * 0.12F, ya, fig);
        drawSeg(dst, w, h, fx, fy - ht * 0.92F, fx, fy - ht * 0.40F, ht * 0.12F, ya, fig);
        drawSeg(dst, w, h, fx, fy - ht * 0.40F, fx - ht * 0.10F, fy, ht * 0.10F, ya, fig);
        drawSeg(dst, w, h, fx, fy - ht * 0.40F, fx + ht * 0.10F, fy, ht * 0.10F, ya, fig);
        drawSeg(
            dst, w, h, fx, fy - ht * 0.78F, fx - ht * 0.16F, fy - ht * 0.50F, ht * 0.07F, ya, fig);
        drawSeg(
            dst, w, h, fx, fy - ht * 0.78F, fx + ht * 0.16F, fy - ht * 0.50F, ht * 0.07F, ya, fig);
      });
}

// Memento: a Polaroid in shaking hands — the data is the photograph, fading
// *backward* (reverse-developing) to a blank white card as memory slips away.
void effectMemento(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float pw = mn * 0.42F, ph = mn * 0.52F;
  const float imW = pw * 0.86F, imH = ph * 0.66F;
  runFrames(
      renderer,
      w,
      h,
      5000,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float jx = std::sin(t * 32.0F) * 1.4F, jy = std::cos(t * 23.0F) * 1.0F;
        const float cxp = w * 0.5F + jx, cyp = h * 0.5F + jy;
        const float reveal = 1.0F - std::clamp(t / 0.85F, 0.0F, 1.0F);  // image strength
        const float endFade = std::clamp((t - 0.90F) / 0.10F, 0.0F, 1.0F);
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
            dst[static_cast<std::size_t>(y) * w + x] = Rgb{6, 6, 10, false};
        const int x0 = static_cast<int>(cxp - pw * 0.5F), x1 = static_cast<int>(cxp + pw * 0.5F);
        const int y0 = static_cast<int>(cyp - ph * 0.5F), y1 = static_cast<int>(cyp + ph * 0.5F);
        for (int y = std::max(0, y0); y <= std::min(h - 1, y1); ++y)
          for (int x = std::max(0, x0); x <= std::min(w - 1, x1); ++x)
            dst[static_cast<std::size_t>(y) * w + x] = Rgb{240, 238, 228, false};  // Polaroid card
        const float iY0 = cyp - ph * 0.45F, iY1 = cyp - ph * 0.45F + imH;
        const float iX0 = cxp - imW * 0.5F, iX1 = cxp + imW * 0.5F;
        for (int y = std::max(0, static_cast<int>(iY0));
             y <= std::min(h - 1, static_cast<int>(iY1));
             ++y)
          for (int x = std::max(0, static_cast<int>(iX0));
               x <= std::min(w - 1, static_cast<int>(iX1));
               ++x)
          {
            const float u = (x - iX0) / imW, v = (y - iY0) / imH;
            const Rgb d = sample(src, w, h, u * w, v * h);
            const float dr = d.transparent ? 200.0F : d.r, dg = d.transparent ? 200.0F : d.g,
                        db = d.transparent ? 200.0F : d.b;
            // image fades to the Polaroid's blank-white as it un-develops
            dst[static_cast<std::size_t>(y) * w + x] = Rgb{u8(dr * reveal + 240 * (1 - reveal)),
                                                           u8(dg * reveal + 238 * (1 - reveal)),
                                                           u8(db * reveal + 228 * (1 - reveal)),
                                                           false};
          }
        if (endFade > 0.0F)
          for (std::size_t k = 0; k < dst.size(); ++k)
            dst[k] = Rgb{u8(dst[k].r * (1 - endFade)),
                         u8(dst[k].g * (1 - endFade)),
                         u8(dst[k].b * (1 - endFade)),
                         false};
      });
}

// Stand By Me: four small silhouettes walk train tracks into a sunset, the data
// tinted warm dusk behind them; rails converge to a vanishing point and the
// figures shrink as they recede.
void effectStandByMe(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float horizonY = h * 0.54F;
  runFrames(
      renderer,
      w,
      h,
      5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 60.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            const float sd = std::hypot(x - w * 0.5F, (y - horizonY + h * 0.02F) * ya);
            const float glow = std::exp(-sd / (mn * 0.50F));
            const float sf = static_cast<float>(y) / horizonY;
            Rgb c;
            if (y > horizonY)  // dim ground / cinder bed
              c = Rgb{u8(28 + l * 0.10F + glow * 80),
                      u8(20 + l * 0.08F + glow * 50),
                      u8(18 + l * 0.06F),
                      false};
            else  // sunset sky
              c = Rgb{u8(160 + 90 * (1 - sf) + glow * 80 + l * 0.10F),
                      u8(90 + 80 * (1 - sf) + glow * 70 + l * 0.10F),
                      u8(60 + 20 * sf + glow * 20),
                      false};
            dst[static_cast<std::size_t>(y) * w + x] = c;
          }
        drawDataDisk(dst,
                     w,
                     h,
                     src,
                     w * 0.5F,
                     horizonY - mn * 0.02F,
                     mn * 0.10F,
                     ya,
                     0.50F,
                     t * 0.3F,
                     Rgb{255, 200, 130, false});  // setting sun
        // rails + sleepers, converging from bottom to horizon
        const float vx = w * 0.5F, vy = horizonY;
        const float gauge = mn * 0.18F;  // half-rail spacing at the bottom
        const Rgb rail{30, 22, 18, false}, slpr{50, 36, 28, false};
        for (int k = 0; k < 18; ++k)  // sleepers
        {
          const float q = static_cast<float>(k) / 18.0F;
          const float qd = 1.0F - std::pow(1.0F - q, 2.0F);  // bunch up toward vanishing
          const float py = h + (vy - h) * qd;
          const float pw = gauge * (1.0F - qd) * 1.4F;
          drawSeg(dst,
                  w,
                  h,
                  vx - pw,
                  py,
                  vx + pw,
                  py,
                  std::max(1.0F, mn * 0.012F * (1.0F - qd) + 0.4F),
                  ya,
                  slpr);
        }
        drawSeg(dst, w, h, vx - gauge, h - 1.0F, vx, vy, std::max(1.0F, mn * 0.012F), ya, rail);
        drawSeg(dst, w, h, vx + gauge, h - 1.0F, vx, vy, std::max(1.0F, mn * 0.012F), ya, rail);
        // figures: 4 boys walking, advancing into the distance
        const float adv = t * 0.30F;
        const Rgb body{8, 8, 10, false};
        for (int i = 0; i < 4; ++i)
        {
          const float q = std::clamp(0.18F + i * 0.10F + adv, 0.0F, 0.95F);
          const float qd = 1.0F - std::pow(1.0F - q, 2.0F);
          const float fy = h + (vy - h) * qd;
          const float fx = vx + (i % 2 == 0 ? -1 : 1) * gauge * (1.0F - qd) * 0.45F;
          const float ht = mn * 0.12F * (1.0F - qd);
          if (ht < 1.5F)
            continue;
          plotDot(dst, w, h, fx, fy - ht * 1.0F, ht * 0.14F, ya, body);
          drawSeg(dst, w, h, fx, fy - ht * 0.92F, fx, fy - ht * 0.40F, ht * 0.12F, ya, body);
          const float stride = std::sin(t * 8.0F + i) * ht * 0.14F;
          drawSeg(dst, w, h, fx, fy - ht * 0.40F, fx - stride, fy, ht * 0.10F, ya, body);
          drawSeg(dst, w, h, fx, fy - ht * 0.40F, fx + stride, fy, ht * 0.10F, ya, body);
        }
      });
}

// Shawshank: Zihuatanejo. The data dissolves into a calm ocean horizon, a low
// sun glinting on the water, and a tiny boat anchored offshore.
void effectShawshank(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float horizonY = h * 0.50F, beachY = h * 0.84F;
  runFrames(
      renderer,
      w,
      h,
      5600,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float bob = std::sin(t * 1.7F) * 1.5F;  // boat bobbing
        const float sunX = w * 0.5F + std::sin(t * 0.4F) * mn * 0.02F, sunY = horizonY - mn * 0.02F;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 80.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            Rgb c;
            if (y > beachY)  // sand
              c = Rgb{u8(225 + l * 0.06F), u8(205 + l * 0.06F), u8(160 + l * 0.05F), false};
            else if (y > horizonY)  // sea: rippled, with a vertical sun glint column
            {
              const float ripple = 0.85F + 0.15F * std::sin(x * 0.40F + y * 0.30F + t * 6.0F);
              const float glint = std::exp(-std::fabs(x - sunX) / (mn * 0.04F)) *
                                  std::clamp(1.0F - (y - horizonY) / (h - horizonY), 0.0F, 1.0F);
              c = Rgb{u8(70 * ripple + glint * 200 + l * 0.06F),
                      u8(130 * ripple + glint * 200 + l * 0.06F),
                      u8(165 * ripple + glint * 120 + l * 0.06F),
                      false};
            }
            else  // warm sky
            {
              const float sf = static_cast<float>(y) / horizonY;
              c = Rgb{u8(180 + 60 * sf + l * 0.08F),
                      u8(195 + 40 * sf + l * 0.08F),
                      u8(225 - 20 * sf + l * 0.06F),
                      false};
            }
            dst[static_cast<std::size_t>(y) * w + x] = c;
          }
        drawDataDisk(dst,
                     w,
                     h,
                     src,
                     sunX,
                     sunY,
                     mn * 0.08F,
                     ya,
                     0.45F,
                     t * 0.3F,
                     Rgb{255, 245, 200, false});
        // a tiny boat moored offshore
        const float bx = w * 0.62F, by = horizonY + mn * 0.05F + bob;
        const Rgb hull{40, 30, 22, false};
        drawSeg(dst, w, h, bx - mn * 0.04F, by, bx + mn * 0.04F, by, mn * 0.012F, ya, hull);
        drawSeg(dst,
                w,
                h,
                bx - mn * 0.03F,
                by,
                bx - mn * 0.045F,
                by + mn * 0.012F,
                mn * 0.008F,
                ya,
                hull);
        drawSeg(dst,
                w,
                h,
                bx + mn * 0.03F,
                by,
                bx + mn * 0.045F,
                by + mn * 0.012F,
                mn * 0.008F,
                ya,
                hull);
        drawSeg(dst, w, h, bx, by, bx, by - mn * 0.06F, std::max(1.0F, mn * 0.006F), ya, hull);
        drawSeg(dst,
                w,
                h,
                bx,
                by - mn * 0.06F,
                bx + mn * 0.03F,
                by - mn * 0.02F,
                std::max(1.0F, mn * 0.005F),
                ya,
                Rgb{235, 230, 220, false});  // sail
      });
}

// The Sound of Music: a figure on the hilltop spins arms outstretched while the
// data — the alpine meadow and sky — wheels around them.
void effectSoundOfMusic(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float cx = w * 0.5F, cy = h * 0.55F;
  runFrames(
      renderer,
      w,
      h,
      5600,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float rot = t * 4.0F;
        const float ca = std::cos(rot), sa = std::sin(rot);
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const float dx = x - cx, dy = (y - cy) * ya;
            const float rx = dx * ca - dy * sa, ry = dx * sa + dy * ca;
            const Rgb s = sample(src, w, h, cx + rx, cy + ry / ya);
            const float l = s.transparent ? 60.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            const float sf = std::clamp(0.5F + (cy + ry / ya) / h * 0.5F, 0.0F, 1.0F);
            Rgb c;
            if (sf > 0.65F)  // green meadow underfoot
              c = Rgb{u8(60 + 60 * sf + l * 0.15F),
                      u8(130 + 50 * sf + l * 0.18F),
                      u8(50 + 20 * sf + l * 0.08F),
                      false};
            else  // alpine sky
              c = Rgb{u8(110 + 90 * sf + l * 0.10F),
                      u8(150 + 60 * sf + l * 0.10F),
                      u8(220 + l * 0.08F),
                      false};
            dst[static_cast<std::size_t>(y) * w + x] = c;
          }
        // the figure: a small spinning silhouette at centre, arms outstretched
        const float ht = mn * 0.14F;
        const Rgb maria{18, 14, 18, false}, dress{40, 70, 130, false};
        const float armSpin = std::sin(t * 6.0F) * 0.06F;  // little wobble
        const float fy = cy + ht * 0.4F;
        plotDot(dst, w, h, cx, fy - ht * 1.0F, ht * 0.13F, ya, maria);  // head
        drawSeg(
            dst, w, h, cx, fy - ht * 0.90F, cx, fy - ht * 0.35F, ht * 0.13F, ya, dress);  // body
        for (int leg = -1; leg <= 1; leg += 2)  // skirt flare into legs
          drawSeg(dst, w, h, cx, fy - ht * 0.35F, cx + leg * ht * 0.16F, fy, ht * 0.13F, ya, dress);
        drawSeg(dst,
                w,
                h,
                cx,
                fy - ht * 0.78F,
                cx - ht * 0.38F,
                fy - ht * 0.72F + armSpin * ht,
                ht * 0.08F,
                ya,
                maria);  // arms out
        drawSeg(dst,
                w,
                h,
                cx,
                fy - ht * 0.78F,
                cx + ht * 0.38F,
                fy - ht * 0.72F - armSpin * ht,
                ht * 0.08F,
                ya,
                maria);
      });
}

// The Great Dictator globe: a small figure tip-toes and juggles a translucent
// Earth-balloon (wrapped from the data), the globe rising and falling, settling
// into the figure's hands.
void effectGlobeDance(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float floorY = h * 0.86F;
  runFrames(
      renderer,
      w,
      h,
      5600,
      [&](float t, std::vector<Rgb>& dst)
      {
        for (int y = 0; y < h; ++y)  // chancellery: data dimmed warmly
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 40.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            dst[static_cast<std::size_t>(y) * w + x] =
                (y > floorY)
                    ? Rgb{40, 30, 22, false}
                    : Rgb{u8(28 + l * 0.18F), u8(22 + l * 0.16F), u8(38 + l * 0.18F), false};
          }
        // figure on tip-toe
        const float fcx = w * 0.5F, ftop = h * 0.36F;
        const float bob = std::sin(t * 3.5F) * mn * 0.012F;
        const Rgb suit{30, 28, 32, false};
        plotDot(dst, w, h, fcx, ftop + bob, mn * 0.022F, ya, suit);
        drawSeg(dst,
                w,
                h,
                fcx,
                ftop + mn * 0.022F + bob,
                fcx,
                floorY - mn * 0.05F,
                std::max(1.0F, mn * 0.014F),
                ya,
                suit);
        // pointing tip-toes
        drawSeg(dst,
                w,
                h,
                fcx,
                floorY - mn * 0.05F,
                fcx - mn * 0.025F,
                floorY,
                std::max(1.0F, mn * 0.010F),
                ya,
                suit);
        drawSeg(dst,
                w,
                h,
                fcx,
                floorY - mn * 0.05F,
                fcx + mn * 0.025F,
                floorY,
                std::max(1.0F, mn * 0.010F),
                ya,
                suit);
        // little Hitler-mustache dot
        plotDot(dst, w, h, fcx, ftop + bob + mn * 0.012F, mn * 0.006F, ya, Rgb{14, 12, 14, false});
        // hands held up; globe juggled above
        const float ghY = ftop - mn * 0.04F + std::sin(t * 2.5F) * mn * 0.10F;  // rises/falls
        const float ghX = fcx + std::cos(t * 1.7F) * mn * 0.04F;
        const float arm = (ghY < ftop) ? 1.0F : 0.5F;  // arms reach up when ball is high
        drawSeg(dst,
                w,
                h,
                fcx - mn * 0.016F,
                ftop + bob + mn * 0.014F,
                ghX - mn * 0.022F,
                ghY + mn * 0.020F * arm,
                std::max(1.0F, mn * 0.010F),
                ya,
                suit);
        drawSeg(dst,
                w,
                h,
                fcx + mn * 0.016F,
                ftop + bob + mn * 0.014F,
                ghX + mn * 0.022F,
                ghY + mn * 0.020F * arm,
                std::max(1.0F, mn * 0.010F),
                ya,
                suit);
        // the Earth-balloon (data wrapped on a slowly spinning sphere)
        const float gR = mn * 0.10F;
        drawSphere(dst, w, h, src, ghX, ghY, gR, gR, 0.45F, t * 1.2F, 0.45F, 0.65F, 0.55F);
        plotDot(dst,
                w,
                h,
                ghX - gR * 0.42F,
                ghY - gR * 0.42F,
                gR * 0.15F,
                ya,
                Rgb{255, 255, 250, false});  // specular highlight
      });
}

// Spirited Away train: a silhouette train glides along rails over the half-
// submerged sea, lit windows reflecting in the rippling data-water.
void effectSpiritedTrain(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float waterY = h * 0.54F, trackY = h * 0.62F;
  runFrames(renderer,
            w,
            h,
            5600,
            [&](float t, std::vector<Rgb>& dst)
            {
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 70.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  const float sf = static_cast<float>(y) / h;
                  Rgb c;
                  if (y > waterY)  // shallow sea (data reflected)
                  {
                    const float ripple = 0.86F + 0.14F * std::sin(x * 0.30F - t * 4.0F);
                    const float refl = sample(src, w, h, x, 2.0F * waterY - y).transparent
                                           ? 60.0F
                                           : (0.3F * sample(src, w, h, x, 2.0F * waterY - y).r +
                                              0.59F * sample(src, w, h, x, 2.0F * waterY - y).g +
                                              0.11F * sample(src, w, h, x, 2.0F * waterY - y).b);
                    c = Rgb{u8(50 + refl * 0.12F + ripple * 20),
                            u8(85 + refl * 0.16F + ripple * 30),
                            u8(120 + refl * 0.18F + ripple * 40),
                            false};
                  }
                  else  // sky
                    c = Rgb{u8(180 + 40 * (1 - sf) + l * 0.10F),
                            u8(165 + 50 * (1 - sf) + l * 0.10F),
                            u8(190 - 20 * sf + l * 0.08F),
                            false};
                  dst[static_cast<std::size_t>(y) * w + x] = c;
                }
              // rails just above the waterline
              drawSeg(dst,
                      w,
                      h,
                      0,
                      trackY,
                      w - 1,
                      trackY,
                      std::max(1.0F, mn * 0.006F),
                      ya,
                      Rgb{40, 30, 24, false});
              drawSeg(dst,
                      w,
                      h,
                      0,
                      trackY + mn * 0.012F,
                      w - 1,
                      trackY + mn * 0.012F,
                      std::max(1.0F, mn * 0.006F),
                      ya,
                      Rgb{40, 30, 24, false});
              // train: dark silhouette with lit windows, gliding across
              const float bx0 = -mn * 0.5F + t * (w + mn * 1.0F);
              const float bH = mn * 0.10F;
              const Rgb body{18, 16, 22, false};
              // engine
              const float ex = bx0;
              drawSeg(dst,
                      w,
                      h,
                      ex - mn * 0.10F,
                      trackY - bH * 0.4F,
                      ex + mn * 0.10F,
                      trackY - bH * 0.4F,
                      bH * 0.7F,
                      ya,
                      body);
              plotDot(
                  dst, w, h, ex - mn * 0.12F, trackY - bH * 0.55F, bH * 0.18F, ya, body);  // funnel
              plotDot(dst,
                      w,
                      h,
                      ex - mn * 0.12F,
                      trackY - bH * 0.78F,
                      bH * 0.12F,
                      ya,
                      Rgb{230, 220, 200, false});  // steam puff
              // carriages with lit windows
              for (int c = 1; c <= 3; ++c)
              {
                const float cx0 = bx0 - mn * (0.20F + c * 0.24F);
                drawSeg(dst,
                        w,
                        h,
                        cx0 - mn * 0.10F,
                        trackY - bH * 0.35F,
                        cx0 + mn * 0.10F,
                        trackY - bH * 0.35F,
                        bH * 0.65F,
                        ya,
                        body);
                for (int wnd = 0; wnd < 3; ++wnd)
                {
                  const float wx = cx0 - mn * 0.07F + wnd * mn * 0.07F;
                  plotDot(dst,
                          w,
                          h,
                          wx,
                          trackY - bH * 0.4F,
                          mn * 0.012F,
                          ya,
                          Rgb{255, 220, 130, false});  // window
                }
              }
            });
}

// Totoro at the bus stop: a great friendly silhouette with an umbrella, a smaller
// figure beside, raindrops bouncing on the big belly, all over a rainy data dusk.
void effectTotoro(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float groundY = h * 0.88F;
  runFrames(
      renderer,
      w,
      h,
      5600,
      [&](float t, std::vector<Rgb>& dst)
      {
        for (int y = 0; y < h; ++y)  // forest dusk from data
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 50.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            const float sf = static_cast<float>(y) / h;
            dst[static_cast<std::size_t>(y) * w + x] = (y > groundY)
                                                           ? Rgb{20, 24, 18, false}
                                                           : Rgb{u8(40 + 30 * sf + l * 0.14F),
                                                                 u8(50 + 30 * sf + l * 0.16F),
                                                                 u8(58 + 20 * sf + l * 0.14F),
                                                                 false};
          }
        const float tx = w * 0.58F, ty = groundY - mn * 0.20F;  // Totoro centre-right
        const Rgb fur{24, 26, 30, false};
        // belly: the data wrapped on a softly-lit sphere, kept dark by the
        // low-luminance tint so Totoro still reads as a night silhouette.
        drawDataDisk(dst, w, h, src, tx, ty, mn * 0.20F, ya, 0.55F, 0.10F, Rgb{60, 64, 72, false});
        plotDot(dst,
                w,
                h,
                tx - mn * 0.06F,
                ty - mn * 0.18F,
                mn * 0.03F,
                ya,
                Rgb{240, 240, 240, false});  // big eye L
        plotDot(dst,
                w,
                h,
                tx + mn * 0.06F,
                ty - mn * 0.18F,
                mn * 0.03F,
                ya,
                Rgb{240, 240, 240, false});  // big eye R
        plotDot(dst,
                w,
                h,
                tx - mn * 0.06F,
                ty - mn * 0.18F,
                mn * 0.012F,
                ya,
                Rgb{8, 8, 10, false});  // pupil L
        plotDot(dst, w, h, tx + mn * 0.06F, ty - mn * 0.18F, mn * 0.012F, ya, Rgb{8, 8, 10, false});
        plotDot(dst, w, h, tx, ty - mn * 0.14F, mn * 0.014F, ya, Rgb{40, 28, 22, false});  // nose
        // ears
        drawSeg(dst,
                w,
                h,
                tx - mn * 0.10F,
                ty - mn * 0.18F,
                tx - mn * 0.14F,
                ty - mn * 0.30F,
                mn * 0.02F,
                ya,
                fur);
        drawSeg(dst,
                w,
                h,
                tx + mn * 0.10F,
                ty - mn * 0.18F,
                tx + mn * 0.14F,
                ty - mn * 0.30F,
                mn * 0.02F,
                ya,
                fur);
        // umbrella held over Totoro
        const float uHx = tx + mn * 0.18F, uHy = ty - mn * 0.10F;
        drawSeg(dst,
                w,
                h,
                uHx,
                uHy,
                uHx,
                uHy - mn * 0.22F,
                std::max(1.0F, mn * 0.010F),
                ya,
                Rgb{60, 44, 30, false});
        for (int k = -5; k <= 5; ++k)  // umbrella canopy dome
        {
          const float a = k / 5.0F;
          plotDot(dst,
                  w,
                  h,
                  uHx + a * mn * 0.18F,
                  uHy - mn * 0.22F - (1.0F - a * a) * mn * 0.06F,
                  mn * 0.022F,
                  ya,
                  Rgb{60, 44, 30, false});
        }
        // little Mei beside him
        const float mcx = w * 0.32F, mcy = groundY - mn * 0.08F;
        drawSeg(
            dst, w, h, mcx, mcy - mn * 0.08F, mcx, mcy, mn * 0.018F, ya, Rgb{200, 60, 50, false});
        plotDot(dst, w, h, mcx, mcy - mn * 0.10F, mn * 0.014F, ya, Rgb{200, 160, 120, false});
        // rain
        for (int i = 0; i < 150; ++i)
        {
          const float rx = std::fmod(hash(i * 2) * w + t * w * 0.05F + i * 0.8F, (float)w);
          const float ry = std::fmod(hash(i * 2 + 1) * h + t * h * 2.0F + i * 0.4F, (float)h);
          drawSeg(dst,
                  w,
                  h,
                  rx,
                  ry,
                  rx - mn * 0.012F,
                  ry + mn * 0.03F,
                  std::max(1.0F, mn * 0.004F),
                  ya,
                  Rgb{170, 200, 220, false});
        }
        // raindrops bouncing off Totoro's belly
        for (int b = 0; b < 8; ++b)
        {
          const float a = hash(b * 13 + static_cast<int>(t * 6.0F));
          const float bx = tx + (a - 0.5F) * mn * 0.36F;
          const float by =
              ty - mn * 0.04F + std::fmod(t * 4.0F + b * 0.13F, 1.0F) * mn * 0.10F - mn * 0.05F;
          plotDot(dst, w, h, bx, by, std::max(1.0F, mn * 0.008F), ya, Rgb{220, 230, 240, false});
        }
      });
}

// Big: a giant illuminated piano floor — feet hop key to key playing notes; each
// step lights a key and a music note drifts up. The data tints the room.
void effectBigKeyboard(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  const float mn = std::min(static_cast<float>(w), h * ya);
  const float keysTop = h * 0.40F, keysBot = h * 0.95F;
  constexpr int kKeys = 12;
  runFrames(
      renderer,
      w,
      h,
      5600,
      [&](float t, std::vector<Rgb>& dst)
      {
        for (int y = 0; y < h; ++y)  // toy-store room dimly tinted by the data
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 30.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            dst[static_cast<std::size_t>(y) * w + x] =
                (y < keysTop)
                    ? Rgb{u8(34 + l * 0.18F), u8(28 + l * 0.16F), u8(40 + l * 0.20F), false}
                    : Rgb{6, 6, 10, false};
          }
        // the white-key floor (12 keys across) — each key's ivory carries the
        // data, so the dance floor is the weather you've been looking at
        const float keyW = static_cast<float>(w) / kKeys;
        const float lit = std::fmod(t * 5.0F, kKeys);
        const int litI = static_cast<int>(lit) % kKeys;
        for (int k = 0; k < kKeys; ++k)
        {
          const float x0 = k * keyW + 1.0F, x1 = (k + 1) * keyW - 1.0F;
          const bool litKey = (k == litI);
          const float k_lit = litKey ? (1.0F - (lit - litI)) : 0.0F;
          for (int y = static_cast<int>(keysTop); y <= static_cast<int>(keysBot); ++y)
            for (int x = static_cast<int>(x0); x <= static_cast<int>(x1); ++x)
            {
              if (x < 0 || x >= w || y < 0 || y >= h)
                continue;
              const Rgb d = sample(src, w, h, x, y);
              const float dr = d.transparent ? 130.0F : d.r;
              const float dg = d.transparent ? 130.0F : d.g;
              const float db = d.transparent ? 130.0F : d.b;
              const float baseR = litKey ? (235.0F + 20.0F * k_lit) : 235.0F;
              const float baseG = litKey ? (220.0F + 30.0F * k_lit) : 230.0F;
              const float baseB = litKey ? (110.0F + 90.0F * k_lit) : 220.0F;
              const float ivR = baseR * 0.68F + dr * 0.28F;
              const float ivG = baseG * 0.68F + dg * 0.28F;
              const float ivB = baseB * 0.68F + db * 0.25F;
              dst[static_cast<std::size_t>(y) * w + x] = Rgb{u8(ivR), u8(ivG), u8(ivB), false};
            }
        }
        // black keys (groups of 2 and 3 between whites)
        for (int k = 0; k < kKeys - 1; ++k)
        {
          const int p = k % 7;
          if (p == 2 || p == 6)
            continue;  // skip the gaps E-F and B-C
          const float bx = (k + 1) * keyW;
          const float bw = keyW * 0.55F;
          for (int y = static_cast<int>(keysTop);
               y <= static_cast<int>(keysTop + (keysBot - keysTop) * 0.62F);
               ++y)
            for (int x = static_cast<int>(bx - bw * 0.5F); x <= static_cast<int>(bx + bw * 0.5F);
                 ++x)
              if (x >= 0 && x < w && y >= 0 && y < h)
                dst[static_cast<std::size_t>(y) * w + x] = Rgb{14, 14, 18, false};
        }
        // the feet hopping
        const float beat = t * 5.0F;
        const float bx = (litI + 0.5F) * keyW;
        const float by = keysTop + (keysBot - keysTop) * 0.28F -
                         std::sin(std::fmod(beat, 1.0F) * 3.14159F) * mn * 0.10F;
        plotDot(dst, w, h, bx - mn * 0.025F, by, mn * 0.018F, ya, Rgb{60, 40, 30, false});
        plotDot(dst, w, h, bx + mn * 0.025F, by, mn * 0.018F, ya, Rgb{60, 40, 30, false});
        // music notes drifting up
        for (int n = 0; n < 6; ++n)
        {
          const float nt = std::fmod(t * 1.2F + n * 0.17F, 1.0F);
          const float nx = hash(n * 3) * w;
          const float ny = (keysTop)*nt + 0.0F * (1 - nt) - mn * 0.05F + nt * (-mn * 0.20F);
          const float alpha = 1.0F - nt;
          if (alpha < 0.05F)
            continue;
          plotDot(dst,
                  w,
                  h,
                  nx,
                  ny,
                  mn * 0.012F,
                  ya,
                  Rgb{u8(220 * alpha), u8(220 * alpha), u8(160 * alpha), false});  // notehead
          drawSeg(dst,
                  w,
                  h,
                  nx + mn * 0.012F,
                  ny,
                  nx + mn * 0.012F,
                  ny - mn * 0.05F * alpha,
                  std::max(1.0F, mn * 0.005F),
                  ya,
                  Rgb{u8(220 * alpha), u8(220 * alpha), u8(160 * alpha), false});  // stem
        }
      });
}

// ---------------------------------------------------------------------------
// Pink Panther: a long pink-cat silhouette saunters in from the right with the
// classic high tail and ear pair; a fat magenta ink-line trails behind so the
// path he just walked stays visible as he exits stage left.
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

// ACME Anvil: classic Looney Tunes anvil drops from above and slams the screen
// flat — squashes on impact, ACME stencilled on its side, dust ring kicks up.
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

// Banksy "Girl with Balloon": a heart-shaped red balloon drifts up across the
// view; the data dims to museum-wall beige. Near the top the balloon shreds
// itself bottom-up — strips peel off, drift down — Banksy's "Love is in the
// Bin" gag.
void effectBanksyBalloon(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 80.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(220 + l * 0.10F), u8(212 + l * 0.10F), u8(196 + l * 0.08F), false};
                }
              const float rise = std::clamp(t / 0.85F, 0.0F, 1.0F);
              const float bx = w * (0.45F + 0.06F * std::sin(t * 1.7F));
              const float by = h * (1.05F - rise * 1.05F);
              const float R = mn * 0.10F;
              const Rgb red{210, 30, 30, false};
              // String trailing down.
              drawSeg(dst, w, h, bx, by + R * 1.05F, bx + std::sin(t * 1.5F) * mn * 0.04F,
                      h * 0.95F, std::max(1.0F, mn * 0.004F), ya, Rgb{60, 60, 60, false});
              // Heart shape: two circles + downward triangle. Shred from bottom
              // upward over the last quarter of the animation.
              const float shred = std::clamp((t - 0.72F) / 0.28F, 0.0F, 1.0F);
              const int yTop = static_cast<int>(by - R * 1.10F);
              const int yBot = static_cast<int>(by + R * 1.20F);
              for (int yy = yTop; yy <= yBot; ++yy)
              {
                if (yy < 0 || yy >= h) continue;
                const float ny = (yy - by) / R;
                for (int xx = static_cast<int>(bx - R * 1.15F);
                     xx <= static_cast<int>(bx + R * 1.15F); ++xx)
                {
                  if (xx < 0 || xx >= w) continue;
                  const float nx = (xx - bx) / R;
                  // Heart curve: (x² + y² - 1)³ - x² y³ ≤ 0
                  const float a = nx * nx + ny * ny - 1.0F;
                  const float heart = a * a * a - nx * nx * ny * ny * ny;
                  if (heart <= 0.0F)
                  {
                    // Below shred-front: replace with strip drifting downward.
                    const float shredFront = (1.0F - shred) * 2.2F - 1.1F;
                    if (ny > shredFront) continue;
                    dst[static_cast<std::size_t>(yy) * w + xx] = red;
                  }
                }
              }
              // Drifting paper strips below the shred front.
              if (shred > 0.0F)
              {
                for (int i = 0; i < 14; ++i)
                {
                  const float u = static_cast<float>(i) / 14.0F;
                  const float fx =
                      bx + (u - 0.5F) * R * 2.0F + std::sin(t * 4.0F + i) * mn * 0.02F;
                  const float fy = by + R * 1.0F + shred * shred * mn * 0.5F * (0.5F + u);
                  drawSeg(dst, w, h, fx, fy, fx + mn * 0.005F, fy + mn * 0.04F,
                          std::max(1.0F, mn * 0.005F), ya, red);
                }
              }
            });
}

// Bone Chandelier (Sedlec Ossuary, Kutná Hora): the opposite of Fall to Pieces.
// The room dims to chapel-candle warmth; bones (femurs, ribs, finger bones, a
// crown of skulls) rise from the bottom and snap into a hanging chandelier
// silhouette, swinging gently when complete.
void effectBoneChandelier(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  // Target shape, world-relative to the chandelier centre.
  struct Piece
  {
    float x, y;    // target (in units of R)
    float r;       // dot radius scale
    bool isSkull;  // skulls get eye pits
  };
  std::vector<Piece> pieces;
  const float R = mn * 0.18F;
  // Vertical chain from ceiling.
  for (int k = 0; k < 6; ++k)
    pieces.push_back({0.0F, -1.55F + k * 0.10F, 0.06F, false});
  // Central skull crown — 6 skulls in a horizontal ring.
  for (int k = 0; k < 6; ++k)
  {
    const float a = k / 6.0F * 6.2832F;
    pieces.push_back({std::cos(a) * 0.55F, std::sin(a) * 0.20F, 0.16F, true});
  }
  // Outer ring of finger bones.
  for (int k = 0; k < 16; ++k)
  {
    const float a = k / 16.0F * 6.2832F;
    pieces.push_back({std::cos(a) * 1.0F, std::sin(a) * 0.40F + 0.35F, 0.10F, false});
  }
  // Cross femurs at the bottom.
  pieces.push_back({-0.5F, 0.85F, 0.12F, false});
  pieces.push_back({0.5F, 0.85F, 0.12F, false});
  pieces.push_back({0.0F, 0.95F, 0.14F, true});  // bottom skull
  runFrames(
      renderer, w, h, 5600,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float dim = std::clamp(1.0F - t * 0.8F, 0.18F, 1.0F);
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 30.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(28 + l * 0.12F * dim), u8(22 + l * 0.10F * dim),
                    u8(18 + l * 0.08F * dim), false};
          }
        const float cxw = w * 0.5F;
        const float cy = h * 0.42F + std::sin(t * 1.4F) * mn * 0.012F;
        const Rgb bone{222, 210, 188, false};
        const Rgb eye{14, 10, 12, false};
        for (std::size_t i = 0; i < pieces.size(); ++i)
        {
          const Piece& p = pieces[i];
          const float pt = std::clamp(t / 0.85F - hash(static_cast<int>(i) * 7) * 0.20F, 0.0F, 1.0F);
          const float startX = cxw + (hash(static_cast<int>(i)) - 0.5F) * w * 0.6F;
          const float startY = h * 1.05F;
          const float targetX = cxw + p.x * R;
          const float targetY = cy + p.y * R;
          const float ease = 1.0F - (1.0F - pt) * (1.0F - pt);
          const float px = startX + (targetX - startX) * ease;
          const float py = startY + (targetY - startY) * ease;
          if (pt <= 0.0F) continue;
          plotDot(dst, w, h, px, py, mn * 0.022F * p.r * 3.0F, ya, bone);
          if (p.isSkull && pt > 0.7F)
          {
            plotDot(dst, w, h, px - mn * 0.012F, py - mn * 0.004F, std::max(1.0F, mn * 0.008F),
                    ya, eye);
            plotDot(dst, w, h, px + mn * 0.012F, py - mn * 0.004F, std::max(1.0F, mn * 0.008F),
                    ya, eye);
          }
        }
        // Tea-light flicker around the central ring once the structure assembles.
        if (t > 0.7F)
        {
          const float flick = (1.0F + std::sin(t * 13.0F)) * 0.5F;
          for (int k = 0; k < 6; ++k)
          {
            const float a = k / 6.0F * 6.2832F;
            plotDot(dst, w, h, cxw + std::cos(a) * 0.55F * R, cy + std::sin(a) * 0.20F * R - mn * 0.03F,
                    mn * 0.015F * (0.7F + 0.5F * flick), ya, Rgb{255, 210, 130, false});
          }
        }
      });
}

// Magritte "Ceci n'est pas une pipe": data fades to museum cream. A simple
// pipe silhouette draws itself in via a hand-pulled stroke; below, the famous
// text types out — only it now says "Ceci n'est pas un terminal."
void effectCeciNestPas(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 80.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(218 + l * 0.10F), u8(204 + l * 0.10F), u8(168 + l * 0.08F), false};
                }
              const Rgb ink{34, 24, 18, false};
              const float pipeCx = w * 0.5F, pipeCy = h * 0.40F;
              const float bowlR = mn * 0.07F;
              const float stemLen = mn * 0.35F;
              const float draw = std::clamp(t / 0.55F, 0.0F, 1.0F);
              // Stem.
              const float stemEnd = pipeCx + stemLen * draw;
              drawSeg(dst, w, h, pipeCx, pipeCy, stemEnd, pipeCy, std::max(1.5F, mn * 0.010F), ya,
                      ink);
              // Bowl (rises out of the stem near its left end).
              if (draw > 0.55F)
              {
                const float bd = std::clamp((draw - 0.55F) / 0.45F, 0.0F, 1.0F);
                for (float a = -0.2F; a < 2.0F * 3.14159F * bd; a += 0.06F)
                {
                  const float bxd = pipeCx + std::sin(a) * bowlR;
                  const float byd = pipeCy - bowlR * 0.6F - bowlR * (1.0F - std::cos(a));
                  plotDot(dst, w, h, bxd, byd, std::max(1.0F, mn * 0.008F), ya, ink);
                }
              }
              // Text below the pipe — types out letter by letter.
              const std::string line = "CECI N'EST PAS UN TERMINAL";
              const float typeT = std::clamp((t - 0.45F) / 0.50F, 0.0F, 1.0F);
              const int nReveal = static_cast<int>(typeT * line.size());
              const int sc = std::max(2, static_cast<int>(mn / 70.0F));
              const float lineW = static_cast<float>(line.size()) * 6 * sc;
              const float startX = pipeCx - lineW * 0.5F;
              const float startY = pipeCy + mn * 0.20F;
              for (int ci = 0; ci < nReveal && ci < static_cast<int>(line.size()); ++ci)
              {
                const char ch = line[ci];
                const auto g = glyph5x7(ch);
                for (int fy = 0; fy < 7; ++fy)
                  for (int fx = 0; fx < 5; ++fx)
                    if (g[fy][fx] == '1')
                      plotDot(dst, w, h, startX + (ci * 6 + fx) * sc, startY + fy * sc,
                              std::max(1.0F, static_cast<float>(sc) * 0.5F), ya, ink);
              }
            });
}

// HAL Stare: a small HAL 9000 lens pops in at a screen corner and glares for
// the duration of the effect, pupil dilating with the soundtrack heartbeat —
// a distinct, intrusive cousin of the existing full-screen HAL 9000 effect.
void effectHalStare(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  const int corner = static_cast<int>(hash(static_cast<int>(w + h)) * 4.0F) % 4;
  const float cx =
      (corner == 0 || corner == 2) ? mn * 0.18F : w - mn * 0.18F;
  const float cy = (corner < 2) ? mn * 0.18F : h - mn * 0.18F;
  const float R = mn * 0.14F;
  runFrames(renderer, w, h, 4800,
            [&](float t, std::vector<Rgb>& dst)
            {
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float gd = std::hypot(x - cx, (y - cy) * ya);
                  const float vd = 1.0F - std::clamp(gd / (R * 2.5F), 0.0F, 1.0F);
                  const float dim = 0.5F + 0.5F * (1.0F - vd);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      s.transparent ? Rgb{4, 4, 8, false}
                                    : Rgb{u8(s.r * dim), u8(s.g * dim), u8(s.b * dim), false};
                }
              const float pulse = 0.5F + 0.5F * std::sin(t * 4.0F * 3.14159F);
              for (int yy = static_cast<int>(cy - R); yy <= static_cast<int>(cy + R); ++yy)
                for (int xx = static_cast<int>(cx - R); xx <= static_cast<int>(cx + R); ++xx)
                {
                  if (xx < 0 || xx >= w || yy < 0 || yy >= h) continue;
                  const float dx = xx - cx, dy = (yy - cy) * ya;
                  const float d = std::hypot(dx, dy);
                  if (d > R) continue;
                  const float q = d / R;
                  Rgb c;
                  if (q < 0.18F)
                    c = Rgb{255, 220, 150, false};
                  else
                  {
                    const float rr = 1.0F - q;
                    c = Rgb{u8(50 + 200 * rr), u8(20 * rr * rr), 0, false};
                  }
                  dst[static_cast<std::size_t>(yy) * w + xx] = c;
                }
              const float pupilR = R * (0.10F + 0.06F * pulse);
              plotDot(dst, w, h, cx, cy, pupilR, ya, Rgb{20, 0, 0, false});
            });
}

// Hitchcock silhouette: the iconic chubby-profile slides in from stage left,
// stops at the centre — the rotund nose, double chin and rounded crown — and
// a single line of title-card text appears beneath.
void effectHitchcock(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(
      renderer, w, h, 5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float dim = std::clamp(1.0F - t * 1.2F, 0.20F, 1.0F);
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 50.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(210 + l * 0.10F * dim), u8(196 + l * 0.10F * dim),
                    u8(180 + l * 0.08F * dim), false};
          }
        const float slide = std::clamp(t / 0.50F, 0.0F, 1.0F);
        const float cx = -mn * 0.4F + slide * (w * 0.5F + mn * 0.4F);
        const float baseY = h * 0.58F;
        const Rgb ink{14, 12, 10, false};
        const float S = mn * 0.40F;
        // Hitchcock profile via stacked dots: crown, brow, nose, chin, body.
        struct Node { float dx, dy, r; };
        static const std::vector<Node> nodes = {
            {-0.20F, -0.90F, 0.30F},  // crown
            {0.05F, -0.85F, 0.27F},   // back of head bulge
            {-0.30F, -0.65F, 0.20F},  // brow region forward
            {-0.42F, -0.50F, 0.10F},  // nose tip
            {-0.32F, -0.40F, 0.14F},  // upper lip / mustache area
            {-0.28F, -0.28F, 0.16F},  // jowls
            {-0.20F, -0.15F, 0.20F},  // double chin
            {-0.05F, 0.05F, 0.32F},   // neck/collar
            {0.00F, 0.40F, 0.55F},    // shoulders/body
        };
        for (const auto& n : nodes)
          plotDot(dst, w, h, cx + n.dx * S, baseY + n.dy * S, n.r * S, ya, ink);
        if (slide >= 1.0F)
        {
          const std::string line = "GOOD EVENING";
          const int sc = std::max(2, static_cast<int>(mn / 80.0F));
          const float lineW = static_cast<float>(line.size()) * 6 * sc;
          const float fadeT = std::clamp((t - 0.55F) / 0.20F, 0.0F, 1.0F);
          for (int ci = 0; ci < static_cast<int>(line.size()); ++ci)
          {
            const char ch = line[ci];
            const auto g = glyph5x7(ch);
            for (int fy = 0; fy < 7; ++fy)
              for (int fx = 0; fx < 5; ++fx)
                if (g[fy][fx] == '1')
                {
                  const float px = cx + S * 1.0F + ((ci - 6) * 6 + fx) * sc;
                  const float py = baseY + S * 0.7F + fy * sc;
                  const Rgb tc{u8(20 * fadeT), u8(20 * fadeT), u8(20 * fadeT), false};
                  plotDot(dst, w, h, px, py, std::max(1.0F, static_cast<float>(sc) * 0.5F), ya,
                          tc);
                }
            (void)lineW;
          }
        }
      });
}

// Hokusai Great Wave: the wave curls up from the right; the wave body is the
// data tinted Prussian blue, with white-cap foam fingers, breaking over the
// frame as the frame dims to ukiyo-e palette.
void effectHokusaiWave(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(
      renderer, w, h, 5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float dim = std::clamp(1.0F - t * 1.2F, 0.30F, 1.0F);
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 70.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(230 + l * 0.06F * dim), u8(210 + l * 0.08F * dim),
                    u8(170 + l * 0.06F * dim), false};
          }
        const float adv = std::clamp(t / 0.85F, 0.0F, 1.0F);
        // Wave body: a curling arch from the right edge, growing as adv climbs.
        const float baseY = h * 0.62F;
        const float originX = w * (1.10F - adv * 0.95F);
        const Rgb prussian{18, 38, 100, false};
        const Rgb deep{8, 20, 60, false};
        const Rgb foam{240, 240, 245, false};
        const float reach = mn * 1.4F * adv;
        for (float u = 0.0F; u <= 1.0F; u += 0.015F)
        {
          const float ang = u * 3.10F;  // 0..pi -> arc
          const float r = reach * (0.30F + 0.7F * u);
          const float cxw = originX - r * std::sin(ang) * 0.7F;
          const float cyw = baseY - r * (1.0F - std::cos(ang)) * 0.45F;
          plotDot(dst, w, h, cxw, cyw, mn * 0.022F, ya, prussian);
          if (u > 0.65F)
          {
            // Curl tip — claws of foam fingers.
            const float fingerAng = ang + 0.6F * std::sin(t * 5.0F + u * 7.0F);
            const float fx = cxw + std::cos(fingerAng) * mn * 0.05F;
            const float fy = cyw + std::sin(fingerAng) * mn * 0.05F;
            plotDot(dst, w, h, fx, fy, mn * 0.012F, ya, foam);
          }
        }
        // Body shading — darker underbelly.
        for (float u = 0.0F; u <= 1.0F; u += 0.03F)
        {
          const float ang = u * 3.10F;
          const float r = reach * (0.30F + 0.7F * u) - mn * 0.05F;
          const float cxw = originX - r * std::sin(ang) * 0.7F;
          const float cyw = baseY - r * (1.0F - std::cos(ang)) * 0.45F + mn * 0.04F;
          plotDot(dst, w, h, cxw, cyw, mn * 0.018F, ya, deep);
        }
        // Sea surface in front.
        for (int x = 0; x < w; x += 4)
        {
          const float sy = baseY + std::sin(x * 0.18F + t * 6.0F) * mn * 0.012F + mn * 0.06F;
          plotDot(dst, w, h, static_cast<float>(x), sy, mn * 0.018F, ya, prussian);
        }
      });
}

// Lorenz Attractor: classical chaos. A particle traces the butterfly orbit;
// the trail glows hotter near the head and fades to dim behind. Data dims
// underneath to inky black so the orbit reads as a luminous wire.
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

// Magritte "Le Fils de l'Homme": a bowler-hatted man stands centre, a green
// apple floats up from beneath the brim and covers his face — same
// surrealist sleight the painting plays on the viewer.
void effectMagritteBowler(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float dim = 0.35F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 60.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  const float sf = static_cast<float>(y) / h;
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(150 + 80 * sf + l * 0.10F * dim),
                          u8(170 + 50 * sf + l * 0.10F * dim),
                          u8(200 + 20 * sf + l * 0.08F * dim), false};
                }
              const float cx = w * 0.5F, baseY = h * 0.85F;
              const float S = mn * 0.50F;
              const Rgb coat{20, 26, 40, false};
              const Rgb face{210, 175, 140, false};
              const Rgb hat{14, 14, 18, false};
              const Rgb apple{40, 160, 60, false};
              const Rgb appleLeaf{80, 50, 40, false};
              // Body: trapezoidal coat.
              for (int yy = static_cast<int>(baseY - S * 0.5F); yy <= static_cast<int>(baseY); ++yy)
              {
                const float yf = (yy - (baseY - S * 0.5F)) / (S * 0.5F);
                const int half = static_cast<int>(S * (0.20F + 0.20F * yf));
                for (int xo = -half; xo <= half; ++xo)
                  if (cx + xo >= 0 && cx + xo < w && yy >= 0 && yy < h)
                    dst[static_cast<std::size_t>(yy) * w + static_cast<int>(cx + xo)] = coat;
              }
              // Head.
              plotDot(dst, w, h, cx, baseY - S * 0.6F, S * 0.16F, ya, face);
              // Bowler hat: dome + flat brim.
              for (int yy = static_cast<int>(baseY - S * 0.82F); yy <= static_cast<int>(baseY - S * 0.70F);
                   ++yy)
              {
                const float yf = (yy - (baseY - S * 0.82F)) / (S * 0.12F);
                const int half = static_cast<int>(S * (0.14F + 0.04F * yf));
                for (int xo = -half; xo <= half; ++xo)
                  if (cx + xo >= 0 && cx + xo < w && yy >= 0 && yy < h)
                    dst[static_cast<std::size_t>(yy) * w + static_cast<int>(cx + xo)] = hat;
              }
              drawSeg(dst, w, h, cx - S * 0.20F, baseY - S * 0.70F, cx + S * 0.20F, baseY - S * 0.70F,
                      std::max(1.0F, mn * 0.008F), ya, hat);
              // Apple floats up to cover face.
              const float rise = std::clamp(t / 0.65F, 0.0F, 1.0F);
              const float ay = baseY - S * 0.10F - rise * S * 0.50F;
              plotDot(dst, w, h, cx, ay, S * 0.13F, ya, apple);
              drawSeg(dst, w, h, cx, ay - S * 0.13F, cx + S * 0.03F, ay - S * 0.22F,
                      std::max(1.0F, mn * 0.006F), ya, appleLeaf);
            });
}

// Mandelbrot: data fades to indigo void, the set draws itself from a sweeping
// scanline so the iconic cardioid + period-bulbs appear glyph-by-glyph; final
// flash zooms one notch into the seahorse valley.
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

// Möbius strip: a band twisted half a turn, with the data wrapped on it so the
// surface tells you it really is one-sided as it rotates.
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

// Munch Scream: sky goes orange + blood-red sunset, the iconic skull-faced
// figure with hands-on-cheeks pops in from a corner, wavy emanation lines
// undulate across the frame.
void effectMunchScream(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(
      renderer, w, h, 5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        for (int y = 0; y < h; ++y)
        {
          const float sf = static_cast<float>(y) / h;
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 70.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            const float wave =
                std::sin(x * 0.06F + y * 0.04F + t * 2.0F) * 0.06F + 1.0F;
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8((220 - 40 * sf + l * 0.10F) * wave),
                    u8((110 - 80 * sf + l * 0.10F) * wave),
                    u8((50 + 30 * sf + l * 0.05F) * wave), false};
          }
        }
        const float arrive = std::clamp(t / 0.30F, 0.0F, 1.0F);
        const float cx = w * (0.32F + 0.04F * std::sin(t * 1.5F));
        const float cy = h * (0.55F + 0.03F * std::cos(t * 1.5F));
        const float S = mn * 0.30F * arrive;
        if (S < 4.0F) return;
        const Rgb fig{30, 20, 35, false};
        const Rgb pale{218, 195, 150, false};
        // Body — wavy trapezoid descending.
        for (int yy = static_cast<int>(cy + S * 0.05F); yy <= static_cast<int>(cy + S * 1.30F); ++yy)
        {
          const float yf = (yy - cy) / (S * 1.30F);
          const float wob = std::sin(yy * 0.15F + t * 3.0F) * S * 0.06F;
          const int half = static_cast<int>(S * (0.18F + 0.16F * yf));
          for (int xo = -half; xo <= half; ++xo)
            if (cx + xo + wob >= 0 && cx + xo + wob < w && yy >= 0 && yy < h)
              dst[static_cast<std::size_t>(yy) * w + static_cast<int>(cx + xo + wob)] = fig;
        }
        // Head — skull oval.
        plotDot(dst, w, h, cx, cy - S * 0.15F, S * 0.25F, ya, pale);
        // Eyes — hollow O's.
        plotDot(dst, w, h, cx - S * 0.08F, cy - S * 0.20F, S * 0.05F, ya, fig);
        plotDot(dst, w, h, cx + S * 0.08F, cy - S * 0.20F, S * 0.05F, ya, fig);
        // Mouth — vertical O.
        plotDot(dst, w, h, cx, cy - S * 0.05F, S * 0.06F, ya, fig);
        // Hands cupped to cheeks.
        plotDot(dst, w, h, cx - S * 0.25F, cy - S * 0.12F, S * 0.10F, ya, pale);
        plotDot(dst, w, h, cx + S * 0.25F, cy - S * 0.12F, S * 0.10F, ya, pale);
      });
}

// Newton's Cradle: five steel balls in a row; one end ball lifts, drops,
// transfers momentum through the line, kicks the far ball out. Repeats with
// damping. Data dims to a museum-cabinet dark background.
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

// Pendulum Waves: 15 pendulums of progressively shorter length swing in sync
// at start, drift into mesmerising patterns, briefly resync at the end —
// a Harvard-lecture classic.
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

// Pythagoras: a right triangle in the centre; squares on each side grow out;
// labels a², b², c²; then the two smaller squares dissolve forward and
// reassemble inside the largest as a tessellation proof.
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

// Schrödinger's Cat: a box centred; an alive/dead overlay flickers as a
// superposition; at observation time the wavefunction collapses to one or the
// other at random — followed by a question-mark eyebrow.
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

// Silly Walk: a bowler-hatted stick figure crosses the screen doing the
// Ministry of Silly Walks high-leg over-rotation, briefcase swinging.
void effectSillyWalk(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
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
        // Ground line.
        for (int x = 0; x < w; ++x)
        {
          dst[static_cast<std::size_t>(groundY) * w + x] = Rgb{50, 40, 30, false};
        }
        const float cx = -mn * 0.3F + t * (w + mn * 0.6F);
        const float walkPhase = t * 14.0F;
        const float S = mn * 0.18F;
        const float bodyY = groundY - S * 1.3F;
        const Rgb ink{20, 20, 30, false};
        // Bowler hat
        drawSeg(dst, w, h, cx - S * 0.15F, bodyY - S * 1.1F, cx + S * 0.15F, bodyY - S * 1.1F,
                std::max(1.0F, mn * 0.006F), ya, ink);
        for (int yo = -static_cast<int>(S * 0.18F); yo <= -static_cast<int>(S * 0.04F); ++yo)
        {
          const int yy = static_cast<int>(bodyY - S * 1.10F) + yo;
          const int half = static_cast<int>(S * 0.13F);
          for (int xo = -half; xo <= half; ++xo)
            if (yy >= 0 && yy < h && cx + xo >= 0 && cx + xo < w)
              dst[static_cast<std::size_t>(yy) * w + static_cast<int>(cx + xo)] = ink;
        }
        // Head + body
        plotDot(dst, w, h, cx, bodyY - S * 0.9F, S * 0.13F, ya, Rgb{220, 195, 160, false});
        drawSeg(dst, w, h, cx, bodyY - S * 0.78F, cx, bodyY + S * 0.05F, std::max(1.0F, mn * 0.014F), ya, ink);
        // Arms with briefcase
        const float armPh = walkPhase;
        const float aL = std::sin(armPh) * 0.5F;
        const float aR = std::sin(armPh + 3.14F) * 0.5F;
        drawSeg(dst, w, h, cx, bodyY - S * 0.55F, cx - S * 0.25F + S * 0.20F * aL,
                bodyY - S * 0.20F + S * 0.30F * std::fabs(aL),
                std::max(1.0F, mn * 0.010F), ya, ink);
        drawSeg(dst, w, h, cx, bodyY - S * 0.55F, cx + S * 0.25F + S * 0.20F * aR,
                bodyY - S * 0.20F + S * 0.30F * std::fabs(aR),
                std::max(1.0F, mn * 0.010F), ya, ink);
        // Briefcase swings on right arm
        plotDot(dst, w, h, cx + S * 0.45F + S * 0.20F * aR,
                bodyY - S * 0.20F + S * 0.30F * std::fabs(aR) + S * 0.15F, S * 0.10F, ya,
                Rgb{80, 60, 40, false});
        // Legs: one leg high, one straight, alternating with comedic rotation.
        const float L1 = std::sin(walkPhase);
        const float L2 = std::sin(walkPhase + 3.14F);
        auto leg = [&](float phase)
        {
          const float lift = std::max(0.0F, phase);
          const float kneeX = cx + (std::sin(phase) * S * 0.4F);
          const float kneeY = bodyY + S * 0.4F - lift * S * 0.7F;
          const float footX = kneeX + std::sin(phase * 2.0F) * S * 0.3F;
          const float footY = groundY - lift * S * 0.4F;
          drawSeg(dst, w, h, cx, bodyY + S * 0.05F, kneeX, kneeY, std::max(1.0F, mn * 0.010F), ya, ink);
          drawSeg(dst, w, h, kneeX, kneeY, footX, footY, std::max(1.0F, mn * 0.010F), ya, ink);
        };
        leg(L1);
        leg(L2);
      });
}

// That's All Folks: Looney Tunes iris-close around wavy "THAT'S ALL FOLKS!"
// text — distinct from the existing "Iris Out" because of the typeface and
// the irregular wavy frame that traces the iris.
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

// UFO ("I want to believe"): a small saucer silhouette zips across the screen
// at high altitude, light-beam pulse, "I WANT TO BELIEVE" subtitle.
void effectUFO(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(
      renderer, w, h, 5200,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float dim = 0.20F;
        for (int y = 0; y < h; ++y)
        {
          const float sf = static_cast<float>(y) / h;
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 30.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(6 + 18 * (1 - sf) + l * 0.05F * dim),
                    u8(8 + 20 * (1 - sf) + l * 0.06F * dim),
                    u8(28 + 36 * (1 - sf) + l * 0.10F * dim), false};
          }
        }
        // Stars
        for (int i = 0; i < 80; ++i)
        {
          const int sx = static_cast<int>(hash(i * 3) * w);
          const int sy = static_cast<int>(hash(i * 3 + 1) * h * 0.7F);
          const float tw = 0.5F + 0.5F * std::sin(t * 10.0F + i);
          dst[static_cast<std::size_t>(sy) * w + sx] =
              Rgb{u8(170 * tw), u8(170 * tw), u8(200 * tw), false};
        }
        const float p = std::clamp((t - 0.1F) / 0.5F, 0.0F, 1.0F);
        const float ux = -mn * 0.3F + p * (w + mn * 0.6F);
        const float uy = h * 0.32F + std::sin(p * 3.14F * 2.0F) * mn * 0.04F;
        const float us = mn * 0.10F;
        // Disc body
        for (int yo = -static_cast<int>(us * 0.18F); yo <= static_cast<int>(us * 0.18F); ++yo)
        {
          const float yf = std::fabs(yo) / (us * 0.18F);
          const int half = static_cast<int>(us * (1.0F - 0.3F * yf));
          for (int xo = -half; xo <= half; ++xo)
          {
            const int xx = static_cast<int>(ux + xo), yy = static_cast<int>(uy + yo);
            if (xx >= 0 && xx < w && yy >= 0 && yy < h)
              dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{120, 120, 140, false};
          }
        }
        // Dome
        for (float a = 0; a < 3.14F; a += 0.05F)
        {
          const float dx = std::cos(a) * us * 0.4F;
          const float dy = -std::sin(a) * us * 0.25F;
          plotDot(dst, w, h, ux + dx, uy + dy, std::max(1.0F, mn * 0.005F), ya,
                  Rgb{200, 200, 220, false});
        }
        // Lights ring
        for (int k = 0; k < 5; ++k)
        {
          const float lx = ux + (k - 2) * us * 0.30F;
          const Rgb lc =
              (static_cast<int>(t * 20) + k) % 2 == 0 ? Rgb{255, 200, 80, false} : Rgb{80, 200, 255, false};
          plotDot(dst, w, h, lx, uy + us * 0.10F, std::max(1.0F, mn * 0.008F), ya, lc);
        }
        // Beam
        if (p > 0.3F && p < 0.7F)
        {
          const float bf = (p - 0.3F) / 0.4F * 3.14F;
          const float intensity = std::sin(bf);
          for (float v = 0; v <= 1.0F; v += 0.02F)
          {
            const float bw = us * 0.3F * v;
            for (float u = -bw; u <= bw; u += mn * 0.01F)
            {
              const float xx = ux + u;
              const float yy = uy + us * 0.18F + v * mn * 0.4F;
              if (xx >= 0 && xx < w && yy >= 0 && yy < h)
              {
                Rgb& c = dst[static_cast<std::size_t>(yy) * w + static_cast<int>(xx)];
                const float k = intensity * (1.0F - v);
                c = Rgb{u8(c.r + (255 - c.r) * k * 0.6F),
                        u8(c.g + (240 - c.g) * k * 0.6F),
                        u8(c.b + (180 - c.b) * k * 0.6F), false};
              }
            }
          }
        }
        // Subtitle
        if (t > 0.7F)
        {
          const std::string line = "I WANT TO BELIEVE";
          const int sc = std::max(2, static_cast<int>(mn / 80.0F));
          const float lineW = static_cast<float>(line.size()) * 6 * sc;
          for (int ci = 0; ci < static_cast<int>(line.size()); ++ci)
          {
            const char ch = line[ci];
            if (ch == ' ') continue;
            const auto g = glyph5x7(ch);
            for (int fy = 0; fy < 7; ++fy)
              for (int fx = 0; fx < 5; ++fx)
                if (g[fy][fx] == '1')
                  plotDot(dst, w, h, w * 0.5F - lineW * 0.5F + (ci * 6 + fx) * sc,
                          h * 0.90F + fy * sc,
                          std::max(1.0F, static_cast<float>(sc) * 0.5F), ya,
                          Rgb{240, 240, 240, false});
          }
        }
      });
}

// Warhol Banana: a fat yellow banana slides across the data leaving a pale
// yellow afterglow — Velvet Underground album-cover wink.
void effectWarholBanana(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5000,
            [&](float t, std::vector<Rgb>& dst)
            {
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                  dst[static_cast<std::size_t>(y) * w + x] =
                      src[static_cast<std::size_t>(y) * w + x];
              const float p = std::clamp(t, 0.0F, 1.0F);
              const float bx = -mn * 0.4F + p * (w + mn * 0.8F);
              const float by = h * 0.5F;
              const float S = mn * 0.22F;
              const Rgb yellow{255, 220, 60, false};
              const Rgb tip{120, 90, 30, false};
              // Body — curved banana arc.
              for (float u = -1.0F; u <= 1.0F; u += 0.02F)
              {
                const float px = bx + u * S;
                const float py = by + std::sin(u * 1.8F) * S * 0.5F;
                for (float v = -0.18F; v <= 0.18F; v += 0.04F)
                {
                  const float ppx = px;
                  const float ppy = py + v * S * (1.0F - std::fabs(u) * 0.5F);
                  plotDot(dst, w, h, ppx, ppy, std::max(1.0F, mn * 0.012F), ya, yellow);
                }
              }
              // Stems on each end.
              plotDot(dst, w, h, bx - S * 1.0F, by + std::sin(-1.8F) * S * 0.5F + S * 0.1F,
                      mn * 0.012F, ya, tip);
              plotDot(dst, w, h, bx + S * 1.0F, by + std::sin(1.8F) * S * 0.5F - S * 0.05F,
                      mn * 0.012F, ya, tip);
              // Pale afterglow to the left of the banana — where it's been.
              const int trailEnd = static_cast<int>(bx - S * 0.8F);
              for (int xx = 0; xx < trailEnd && xx < w; ++xx)
              {
                const float fade = std::max(0.0F, 1.0F - (trailEnd - xx) / (mn * 0.7F));
                if (fade <= 0.0F) continue;
                const int yyTop = std::max(0, static_cast<int>(by - S * 0.7F));
                const int yyBot = std::min(h - 1, static_cast<int>(by + S * 0.7F));
                for (int yy = yyTop; yy <= yyBot; ++yy)
                {
                  Rgb& c = dst[static_cast<std::size_t>(yy) * w + xx];
                  c = Rgb{u8(c.r + (255 - c.r) * fade * 0.25F),
                          u8(c.g + (230 - c.g) * fade * 0.25F),
                          u8(c.b + (120 - c.b) * fade * 0.20F), false};
                }
              }
            });
}

// ---------------------------------------------------------------------------
// MATHS & PHYSICS THEME ------------------------------------------------------
// ---------------------------------------------------------------------------

// Dodecahedron: data collapses into a wireframe regular dodecahedron rotating
// in 3D, edges traced in bright cyan over an inky vacuum.
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

// Euler's identity: e^(iπ) + 1 = 0 — the most beautiful equation in
// mathematics, typed out in elegant serif-y dots, each symbol fading in
// with a small flourish.
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

// Bohr atom: a nucleus + three concentric orbits with one electron each;
// electrons spin at proportional speeds while the data dims to vacuum.
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

// Foucault Pendulum: a pendulum bob swings; over time its plane of swing
// appears to rotate (as the Earth rotates beneath it); compass rose ground
// markings show the precession.
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

// Galileo's Leaning Tower: two balls of different mass are dropped from the
// tower and hit the ground simultaneously — the equivalence principle in
// twelve seconds.
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

// Fourier Synthesis: a square wave built from sine harmonics — n=1, then
// 1+3, then 1+3+5, etc. — converges to the iconic flat-topped Gibbs wiggle.
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

// π digits: digits of π scroll past on a tape; eventually the symbol π forms
// from the digits assembling into its shape.
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

// Golden Spiral: Fibonacci rectangles + a quarter-circle spiral, growing
// outward from a tiny seed at the centre.
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

// Double-slit interference: particles fired at two slits; a fringe pattern
// builds up on a screen on the far side.
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

// Sierpinski Triangle: the chaos game — iteratively plotted midpoints toward
// one of three triangle vertices builds the Sierpinski gasket.
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

// Brachistochrone: two balls race from A to B on different ramps — a straight
// line and the cycloid. The cycloid wins.
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

// Chladni Plate: a vibrating plate covered with sand, the nodal lines drawing
// out the famous Chladni patterns. Mode changes mid-effect.
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

// Standing Wave: a guitar string vibrating in successive modes (n=1, 2, 3, 4),
// each held briefly, transitions cross-faded between modes.
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

// ---------------------------------------------------------------------------
// CHEMISTRY THEME -----------------------------------------------------------
// ---------------------------------------------------------------------------

// DNA double helix: two strands wind up the screen, rungs are sampled from
// the underlying data so the base-pair colours echo the weather as they
// snap into place.
void effectDNA(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float dim = 0.20F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 30.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(18 + l * 0.10F * dim), u8(22 + l * 0.10F * dim),
                          u8(40 + l * 0.14F * dim), false};
                }
              const float cx = w * 0.5F;
              const float R = mn * 0.13F;
              const Rgb strandA{230, 120, 130, false};
              const Rgb strandB{120, 210, 220, false};
              const int nTurns = 5;
              for (int k = 0; k < h; ++k)
              {
                const float u = k / static_cast<float>(h);
                const float ang = u * nTurns * 6.2832F + t * 2.5F;
                const float xA = cx + std::cos(ang) * R;
                const float xB = cx + std::cos(ang + 3.14159F) * R;
                plotDot(dst, w, h, xA, k, std::max(1.0F, mn * 0.010F), ya, strandA);
                plotDot(dst, w, h, xB, k, std::max(1.0F, mn * 0.010F), ya, strandB);
                if (k % 5 == 0 && std::sin(ang) > 0)
                {
                  for (int rr = 1; rr < 12; ++rr)
                  {
                    const float ux = xA + (xB - xA) * rr / 12.0F;
                    const Rgb d = sample(src, w, h, ux, static_cast<float>(k));
                    const float dr = d.transparent ? 180.0F : d.r;
                    const float dg = d.transparent ? 180.0F : d.g;
                    const float db = d.transparent ? 200.0F : d.b;
                    plotDot(dst, w, h, ux, k, std::max(1.0F, mn * 0.005F), ya,
                            Rgb{u8(dr * 0.7F + 60), u8(dg * 0.7F + 60), u8(db * 0.7F + 60),
                                false});
                  }
                }
              }
            });
}

// Benzene ring: six carbons + six hydrogens hang off a rotating hex with
// alternating double bonds. Carbon and hydrogen atoms are data-textured
// spheres so the molecule wears the data.
void effectBenzene(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5200,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float dim = 0.15F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 36.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(16 + l * 0.08F * dim), u8(20 + l * 0.08F * dim),
                          u8(28 + l * 0.10F * dim), false};
                }
              const float cx = w * 0.5F, cy = h * 0.5F;
              const float R = mn * 0.26F;
              const float aRot = t * 1.2F;
              const Rgb cTint{200, 200, 210, false};
              const Rgb hTint{220, 110, 110, false};
              for (int i = 0; i < 6; ++i)
              {
                const float a1 = aRot + i * 6.2832F / 6.0F;
                const float a2 = aRot + (i + 1) * 6.2832F / 6.0F;
                const float c1x = cx + std::cos(a1) * R;
                const float c1y = cy + std::sin(a1) * R / ya;
                const float c2x = cx + std::cos(a2) * R;
                const float c2y = cy + std::sin(a2) * R / ya;
                drawSeg(dst, w, h, c1x, c1y, c2x, c2y, std::max(1.0F, mn * 0.005F), ya,
                        Rgb{210, 210, 210, false});
                if (i % 2 == 0)
                {
                  const float dx = c2x - c1x, dy = c2y - c1y;
                  const float nl = std::hypot(dx, dy);
                  const float nx = -dy / nl, ny = dx / nl;
                  const float oo = mn * 0.020F;
                  drawSeg(dst, w, h, c1x + nx * oo, c1y + ny * oo, c2x + nx * oo,
                          c2y + ny * oo, std::max(1.0F, mn * 0.004F), ya,
                          Rgb{200, 200, 200, false});
                }
                drawDataDisk(dst, w, h, src, c1x, c1y, mn * 0.035F, ya, 0.75F, t * 0.6F, cTint);
                const float hx = cx + std::cos(a1) * (R + mn * 0.10F);
                const float hy = cy + std::sin(a1) * (R + mn * 0.10F) / ya;
                drawDataDisk(dst, w, h, src, hx, hy, mn * 0.020F, ya, 0.70F, t * 0.6F, hTint);
              }
              for (float a = 0; a < 6.2832F; a += 0.04F)
                plotDot(dst, w, h, cx + std::cos(a) * R * 0.62F,
                        cy + std::sin(a) * R * 0.62F / ya, std::max(1.0F, mn * 0.003F), ya,
                        Rgb{160, 180, 230, false});
            });
}

// Buckminsterfullerene (C60): icosahedral soccer-ball cage rotating in 3D;
// each of the 60 vertex carbons is a data-textured sphere.
void effectBuckyball(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  // 60 vertices of a truncated icosahedron — generated via the golden ratio
  // construction. Three cyclic permutations of (0, ±1, ±3φ), (±1, ±(2+φ),
  // ±2φ), (±φ, ±2, ±(2φ+1)) give all 60.
  constexpr float phi = 1.618033988F;
  std::vector<std::array<float, 3>> verts;
  auto pm = [](float v, int s) { return s == 0 ? v : -v; };
  for (int sa = 0; sa < 2; ++sa)
    for (int sb = 0; sb < 2; ++sb)
    {
      verts.push_back({0, pm(1, sa), pm(3 * phi, sb)});
      verts.push_back({pm(3 * phi, sb), 0, pm(1, sa)});
      verts.push_back({pm(1, sa), pm(3 * phi, sb), 0});
    }
  for (int sa = 0; sa < 2; ++sa)
    for (int sb = 0; sb < 2; ++sb)
      for (int sc = 0; sc < 2; ++sc)
      {
        verts.push_back({pm(1, sa), pm(2 + phi, sb), pm(2 * phi, sc)});
        verts.push_back({pm(2 * phi, sc), pm(1, sa), pm(2 + phi, sb)});
        verts.push_back({pm(2 + phi, sb), pm(2 * phi, sc), pm(1, sa)});
        verts.push_back({pm(phi, sa), pm(2, sb), pm(2 * phi + 1, sc)});
        verts.push_back({pm(2 * phi + 1, sc), pm(phi, sa), pm(2, sb)});
        verts.push_back({pm(2, sb), pm(2 * phi + 1, sc), pm(phi, sa)});
      }
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float dim = 0.10F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 20.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(10 + l * 0.06F * dim), u8(14 + l * 0.06F * dim),
                          u8(24 + l * 0.10F * dim), false};
                }
              const float cyw = std::cos(t * 1.0F), syw = std::sin(t * 1.0F);
              const float cp = std::cos(t * 0.7F), sp = std::sin(t * 0.7F);
              const float scale = mn / 12.0F;
              const float cx = w * 0.5F, cy = h * 0.5F;
              struct Pt { float sx, sy, z; };
              std::vector<Pt> pts(verts.size());
              for (std::size_t i = 0; i < verts.size(); ++i)
              {
                const float x = verts[i][0], yv = verts[i][1], z = verts[i][2];
                const float x1 = x * cyw - z * syw, z1 = x * syw + z * cyw;
                const float y2 = yv * cp - z1 * sp, z2 = yv * sp + z1 * cp;
                pts[i] = {cx + x1 * scale, cy + y2 * scale / ya, z2};
              }
              for (const auto& p : pts)
              {
                const float depth = std::clamp((p.z + 6.0F) / 12.0F, 0.2F, 1.0F);
                drawDataDisk(dst, w, h, src, p.sx, p.sy, mn * 0.022F, ya, 0.7F * depth,
                             t * 0.5F, Rgb{u8(120 * depth + 40), u8(160 * depth + 40),
                                            u8(180 * depth + 40), false});
              }
            });
}

// Periodic Table: a sweeping render of all 118 cells fills the screen, each
// cell tinted by the data sampled at its position so the table reads as a
// data-tinted Mendeleev grid.
void effectPeriodicTable(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  // Position table (row, col) for each Z=1..118. 0 means "no cell."
  // Simplified: just lay out the standard short form for visual punch.
  constexpr int kCols = 18, kRows = 7;
  static const char* const kSyms[kRows][kCols] = {
      {"H",  "",   "",   "",   "",   "",   "",   "",   "",   "",   "",   "",   "",   "",   "",   "",   "",   "He"},
      {"Li", "Be", "",   "",   "",   "",   "",   "",   "",   "",   "",   "",   "B",  "C",  "N",  "O",  "F",  "Ne"},
      {"Na", "Mg", "",   "",   "",   "",   "",   "",   "",   "",   "",   "",   "Al", "Si", "P",  "S",  "Cl", "Ar"},
      {"K",  "Ca", "Sc", "Ti", "V",  "Cr", "Mn", "Fe", "Co", "Ni", "Cu", "Zn", "Ga", "Ge", "As", "Se", "Br", "Kr"},
      {"Rb", "Sr", "Y",  "Zr", "Nb", "Mo", "Tc", "Ru", "Rh", "Pd", "Ag", "Cd", "In", "Sn", "Sb", "Te", "I",  "Xe"},
      {"Cs", "Ba", "*",  "Hf", "Ta", "W",  "Re", "Os", "Ir", "Pt", "Au", "Hg", "Tl", "Pb", "Bi", "Po", "At", "Rn"},
      {"Fr", "Ra", "*",  "Rf", "Db", "Sg", "Bh", "Hs", "Mt", "Ds", "Rg", "Cn", "Nh", "Fl", "Mc", "Lv", "Ts", "Og"}};
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float dim = 0.10F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  dst[static_cast<std::size_t>(y) * w + x] =
                      s.transparent ? Rgb{16, 16, 22, false}
                                    : Rgb{u8(s.r * dim), u8(s.g * dim + 4), u8(s.b * dim + 8), false};
                }
              const float cellW = std::min(w * 0.9F / kCols, h * 0.78F / kRows);
              const float cellH = cellW;
              const float startX = (w - cellW * kCols) * 0.5F;
              const float startY = (h - cellH * kRows) * 0.5F;
              const int total = kRows * kCols;
              const int revealed = std::min(total, static_cast<int>(t * total * 1.2F));
              for (int r = 0; r < kRows; ++r)
                for (int c = 0; c < kCols; ++c)
                {
                  const int idx = r * kCols + c;
                  if (idx >= revealed) continue;
                  const char* sym = kSyms[r][c];
                  if (sym[0] == '\0') continue;
                  const float xa = startX + c * cellW, xb = xa + cellW * 0.94F;
                  const float yya = startY + r * cellH, yyb = yya + cellH * 0.94F;
                  // Tint: sample the data at the cell's anchor and blend with
                  // a per-group hue.
                  const Rgb d = sample(src, w, h, (xa + xb) * 0.5F, (yya + yyb) * 0.5F);
                  const float dr = d.transparent ? 80.0F : d.r;
                  const float dg = d.transparent ? 80.0F : d.g;
                  const float db = d.transparent ? 120.0F : d.b;
                  const Rgb fill{u8(80 + dr * 0.45F), u8(80 + dg * 0.45F), u8(110 + db * 0.40F),
                                 false};
                  for (int yy = static_cast<int>(yya); yy <= static_cast<int>(yyb); ++yy)
                    for (int xx = static_cast<int>(xa); xx <= static_cast<int>(xb); ++xx)
                      if (xx >= 0 && xx < w && yy >= 0 && yy < h)
                        dst[static_cast<std::size_t>(yy) * w + xx] = fill;
                  // Symbol text (1-2 chars).
                  const int sc = std::max(1, static_cast<int>(cellW / 14.0F));
                  for (int ci = 0; ci < 2 && sym[ci]; ++ci)
                  {
                    const auto g = glyph5x7(sym[ci]);
                    for (int fy = 0; fy < 7; ++fy)
                      for (int fx = 0; fx < 5; ++fx)
                        if (g[fy][fx] == '1')
                          plotDot(dst, w, h, xa + cellW * 0.15F + (ci * 6 + fx) * sc,
                                  yya + cellH * 0.25F + fy * sc,
                                  std::max(1.0F, static_cast<float>(sc) * 0.5F), ya,
                                  Rgb{240, 240, 240, false});
                  }
                }
            });
}

// Flame Tests: a Bunsen burner cycles through Li (crimson), Na (yellow),
// K (lilac), Cu (green), Sr (red) flames. The flame body is sampled from
// the data and tinted by the element's characteristic colour.
void effectFlameTest(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  static const std::array<Rgb, 5> kFlames = {Rgb{230, 60, 70, false},
                                             Rgb{255, 220, 60, false},
                                             Rgb{200, 100, 220, false},
                                             Rgb{60, 230, 90, false},
                                             Rgb{220, 30, 60, false}};
  static const std::array<const char*, 5> kLabels = {"LI", "NA", "K", "CU", "SR"};
  runFrames(
      renderer, w, h, 5600,
      [&](float t, std::vector<Rgb>& dst)
      {
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
            dst[static_cast<std::size_t>(y) * w + x] = Rgb{12, 14, 22, false};
        const int stage = std::min(4, static_cast<int>(t * 5.0F));
        const float local = t * 5.0F - stage;
        const Rgb& flameCol = kFlames[stage];
        const float burnerCx = w * 0.5F;
        const float burnerTop = h * 0.78F;
        // Burner shaft
        for (int yy = static_cast<int>(burnerTop); yy <= static_cast<int>(h * 0.95F); ++yy)
        {
          const int half = static_cast<int>(mn * 0.020F);
          for (int xo = -half; xo <= half; ++xo)
          {
            const int xx = static_cast<int>(burnerCx + xo);
            if (xx >= 0 && xx < w && yy >= 0 && yy < h)
              dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{60, 60, 70, false};
          }
        }
        // Flame: a teardrop shape with the data sampled inside and tinted.
        const float flameH = mn * 0.50F;
        const float flameW = mn * 0.18F;
        for (int yy = static_cast<int>(burnerTop - flameH); yy < static_cast<int>(burnerTop); ++yy)
        {
          const float yf = (burnerTop - yy) / flameH;  // 0 at base, 1 at tip
          const float half = flameW * std::sin(yf * 3.14159F) * (0.9F + 0.1F * std::sin(t * 30.0F + yf * 6.0F));
          for (int xo = -static_cast<int>(half); xo <= static_cast<int>(half); ++xo)
          {
            const int xx = static_cast<int>(burnerCx + xo);
            if (xx < 0 || xx >= w || yy < 0 || yy >= h) continue;
            const Rgb d = sample(src, w, h, xx, yy);
            const float dr = d.transparent ? 100.0F : d.r;
            const float dg = d.transparent ? 100.0F : d.g;
            const float db = d.transparent ? 100.0F : d.b;
            const float mix = 0.60F;
            dst[static_cast<std::size_t>(yy) * w + xx] =
                Rgb{u8(flameCol.r * mix + dr * (1 - mix) * 0.7F),
                    u8(flameCol.g * mix + dg * (1 - mix) * 0.7F),
                    u8(flameCol.b * mix + db * (1 - mix) * 0.7F), false};
          }
        }
        // Label
        const std::string lab(kLabels[stage]);
        const int sc = std::max(3, static_cast<int>(mn / 25.0F));
        const float lineW = static_cast<float>(lab.size()) * 6 * sc;
        for (int ci = 0; ci < static_cast<int>(lab.size()); ++ci)
        {
          const auto g = glyph5x7(lab[ci]);
          for (int fy = 0; fy < 7; ++fy)
            for (int fx = 0; fx < 5; ++fx)
              if (g[fy][fx] == '1')
                plotDot(dst, w, h, burnerCx - lineW * 0.5F + (ci * 6 + fx) * sc,
                        h * 0.12F + fy * sc, std::max(1.0F, static_cast<float>(sc) * 0.5F), ya,
                        flameCol);
        }
        (void)local;
      });
}

// Lava Lamp: buoyant wax blobs rise, cool, sink; each blob is a data-tinted
// disk so the wax wears the data while jostling.
void effectLavaLamp(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 5600,
            [&](float t, std::vector<Rgb>& dst)
            {
              // Glass envelope with warm orange backlight.
              const float cxw = w * 0.5F;
              const float lampW = mn * 0.32F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const float dx = std::fabs(x - cxw);
                  const float sf = static_cast<float>(y) / h;
                  if (dx < lampW)
                  {
                    const float glow = 1.0F - dx / lampW;
                    dst[static_cast<std::size_t>(y) * w + x] =
                        Rgb{u8(80 + 150 * glow * (1 - sf * 0.5F)),
                            u8(40 + 60 * glow * (1 - sf * 0.5F)),
                            u8(20 + 20 * glow * (1 - sf * 0.5F)), false};
                  }
                  else
                  {
                    const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                    const float l =
                        s.transparent ? 30.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                    dst[static_cast<std::size_t>(y) * w + x] =
                        Rgb{u8(20 + l * 0.10F), u8(20 + l * 0.10F), u8(28 + l * 0.10F), false};
                  }
                }
              const Rgb wax{240, 110, 40, false};
              for (int i = 0; i < 7; ++i)
              {
                const float phase = hash(i) * 6.2832F;
                const float speed = 0.4F + hash(i * 3) * 0.4F;
                const float bx = cxw + (hash(i * 7) - 0.5F) * lampW * 0.7F;
                const float by = h * (0.85F - 0.65F * (0.5F + 0.5F * std::sin(t * speed + phase)));
                const float bR = mn * (0.025F + 0.020F * hash(i * 11));
                drawDataDisk(dst, w, h, src, bx, by, bR, ya, 0.85F, t * 0.5F + i, wax);
              }
              // Lamp cap + base.
              for (int yy = 0; yy <= static_cast<int>(mn * 0.03F); ++yy)
              {
                const int xa = static_cast<int>(cxw - lampW * 1.05F);
                const int xb = static_cast<int>(cxw + lampW * 1.05F);
                for (int xx = xa; xx <= xb; ++xx)
                  if (xx >= 0 && xx < w && yy < h)
                    dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{60, 50, 40, false};
              }
              for (int yy = h - static_cast<int>(mn * 0.04F); yy < h; ++yy)
              {
                const int xa = static_cast<int>(cxw - lampW * 1.05F);
                const int xb = static_cast<int>(cxw + lampW * 1.05F);
                for (int xx = xa; xx <= xb; ++xx)
                  if (xx >= 0 && xx < w && yy >= 0 && yy < h)
                    dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{60, 50, 40, false};
              }
            });
}

// NaCl Lattice: an alternating Na+ / Cl- ion grid grows outward from the
// centre, both ion species rendered as data-textured spheres so the salt
// crystal carries the data.
void effectNaClLattice(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float dim = 0.10F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  dst[static_cast<std::size_t>(y) * w + x] =
                      s.transparent
                          ? Rgb{12, 12, 18, false}
                          : Rgb{u8(s.r * dim), u8(s.g * dim), u8(s.b * dim + 6), false};
                }
              const float cyw = std::cos(t * 0.8F), syw = std::sin(t * 0.8F);
              const float cp = std::cos(0.4F), sp = std::sin(0.4F);
              const float spacing = mn * 0.06F;
              const float cx = w * 0.5F, cy = h * 0.5F;
              const int extent = 3;
              const float grow = std::clamp(t * 1.5F, 0.0F, 1.0F);
              for (int ix = -extent; ix <= extent; ++ix)
                for (int iy = -extent; iy <= extent; ++iy)
                  for (int iz = -extent; iz <= extent; ++iz)
                  {
                    const float r0 = std::sqrt(static_cast<float>(ix * ix + iy * iy + iz * iz));
                    if (r0 > grow * (extent + 1)) continue;
                    const float wx = ix * spacing;
                    const float wy = iy * spacing;
                    const float wz = iz * spacing;
                    const float rx = wx * cyw - wz * syw;
                    const float rz = wx * syw + wz * cyw;
                    const float ry = wy * cp - rz * sp;
                    const float rz2 = wy * sp + rz * cp;
                    const float sx = cx + rx;
                    const float sy = cy + ry / ya;
                    const float depth = std::clamp((rz2 + extent * spacing) / (2 * extent * spacing),
                                                   0.2F, 1.0F);
                    const bool na = ((ix + iy + iz) & 1) == 0;
                    const Rgb tint = na ? Rgb{u8(200 * depth), u8(80 * depth), u8(220 * depth), false}
                                         : Rgb{u8(70 * depth), u8(220 * depth), u8(120 * depth), false};
                    drawDataDisk(dst, w, h, src, sx, sy, mn * 0.025F * depth, ya, 0.85F, t * 0.5F,
                                 tint);
                  }
            });
}

// pH Strip: a universal-indicator strip dips into solution; the colour
// gradient runs red (acid) → yellow → green → blue → purple (base) as the
// dye floods upward. The strip itself samples the data so the gradient
// blends with what was on screen.
void effectPhStrip(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  static const std::array<Rgb, 7> kRamp = {Rgb{220, 30, 30, false},   Rgb{240, 130, 30, false},
                                           Rgb{240, 220, 30, false}, Rgb{60, 200, 60, false},
                                           Rgb{40, 130, 220, false}, Rgb{100, 30, 200, false},
                                           Rgb{60, 0, 110, false}};
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 60.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(40 + l * 0.20F), u8(40 + l * 0.20F), u8(48 + l * 0.20F), false};
                }
              const float cxw = w * 0.5F;
              const float stripW = mn * 0.18F;
              const float stripTop = h * 0.10F;
              const float stripBot = h * 0.90F;
              const float dyeFront = stripBot - (stripBot - stripTop) * std::clamp(t * 1.2F, 0.0F, 1.0F);
              for (int yy = static_cast<int>(stripTop); yy <= static_cast<int>(stripBot); ++yy)
              {
                // Strip colour: dyed below dyeFront, pale above.
                const float yf = (yy - stripTop) / (stripBot - stripTop);
                Rgb tint;
                if (static_cast<float>(yy) > dyeFront)
                {
                  const float seg = yf * 6.0F;
                  const int i = std::clamp(static_cast<int>(seg), 0, 5);
                  const float f = seg - i;
                  const Rgb& a = kRamp[i];
                  const Rgb& b = kRamp[i + 1];
                  tint = Rgb{u8(a.r + (b.r - a.r) * f), u8(a.g + (b.g - a.g) * f),
                             u8(a.b + (b.b - a.b) * f), false};
                }
                else
                {
                  tint = Rgb{240, 232, 200, false};
                }
                for (int xo = -static_cast<int>(stripW * 0.5F);
                     xo <= static_cast<int>(stripW * 0.5F); ++xo)
                {
                  const int xx = static_cast<int>(cxw + xo);
                  if (xx < 0 || xx >= w || yy < 0 || yy >= h) continue;
                  const Rgb d = sample(src, w, h, xx, yy);
                  const float dr = d.transparent ? 200.0F : d.r;
                  const float dg = d.transparent ? 200.0F : d.g;
                  const float db = d.transparent ? 200.0F : d.b;
                  dst[static_cast<std::size_t>(yy) * w + xx] =
                      Rgb{u8(tint.r * 0.62F + dr * 0.30F), u8(tint.g * 0.62F + dg * 0.30F),
                          u8(tint.b * 0.62F + db * 0.30F), false};
                }
              }
            });
}

// Chromatography: solvent climbs filter paper, three dye spots separate by
// retention factor. Paper itself is sampled-data-tinted so the lab bench
// shows through.
void effectChromatography(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float paperLeft = w * 0.30F, paperRight = w * 0.70F;
              const float paperTop = h * 0.05F, paperBot = h * 0.92F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const bool onPaper = x >= paperLeft && x <= paperRight && y >= paperTop &&
                                       y <= paperBot;
                  const Rgb d = sample(src, w, h, x, y);
                  const float dr = d.transparent ? 220.0F : d.r;
                  const float dg = d.transparent ? 220.0F : d.g;
                  const float db = d.transparent ? 200.0F : d.b;
                  if (onPaper)
                    dst[static_cast<std::size_t>(y) * w + x] =
                        Rgb{u8(220 + dr * 0.10F), u8(212 + dg * 0.10F),
                            u8(190 + db * 0.10F), false};
                  else
                    dst[static_cast<std::size_t>(y) * w + x] =
                        Rgb{u8(30 + dr * 0.10F), u8(30 + dg * 0.10F),
                            u8(38 + db * 0.10F), false};
                }
              // Solvent front rises from the bottom.
              const float startY = paperBot - mn * 0.04F;
              const float frontY = startY - (startY - paperTop) * std::clamp(t * 1.1F, 0.0F, 1.0F);
              for (int x = static_cast<int>(paperLeft); x <= static_cast<int>(paperRight); ++x)
              {
                const float yf = frontY + std::sin(x * 0.4F + t * 5.0F) * mn * 0.005F;
                for (int yy = static_cast<int>(yf); yy <= static_cast<int>(startY); ++yy)
                  if (yy >= 0 && yy < h)
                  {
                    Rgb& c = dst[static_cast<std::size_t>(yy) * w + x];
                    c = Rgb{u8(c.r * 0.8F), u8(c.g * 0.85F), u8(c.b * 0.95F + 30), false};
                  }
              }
              // Three dye spots, each with its own Rf.
              static const std::array<Rgb, 3> kDyes = {Rgb{200, 30, 30, false},
                                                       Rgb{220, 200, 30, false},
                                                       Rgb{30, 60, 220, false}};
              static const std::array<float, 3> kRf = {0.55F, 0.40F, 0.25F};
              for (int d = 0; d < 3; ++d)
              {
                const float cx = paperLeft + (paperRight - paperLeft) * (0.30F + 0.20F * d);
                const float climbedY =
                    startY - (startY - paperTop) * std::clamp(t * 1.1F, 0.0F, 1.0F) * kRf[d];
                plotDot(dst, w, h, cx, climbedY, mn * 0.022F, ya, kDyes[d]);
              }
            });
}

// Brownian motion: a single visible particle traces a jagged random walk
// over the data, leaving a fading dotted history. The particle itself is
// data-textured so it carries the field it's swimming in.
void effectBrownian(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  // Pre-walk the path so it's deterministic.
  constexpr int N = 240;
  std::vector<std::pair<float, float>> path(N);
  float px = w * 0.5F, py = h * 0.5F;
  for (int i = 0; i < N; ++i)
  {
    const float a = hash(i * 3) * 6.2832F;
    const float r = mn * 0.025F * hash(i * 7);
    px = std::clamp(px + std::cos(a) * r, w * 0.05F, w * 0.95F);
    py = std::clamp(py + std::sin(a) * r, h * 0.05F, h * 0.95F);
    path[i] = {px, py};
  }
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float dim = 0.30F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 60.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(30 + l * 0.15F * dim), u8(30 + l * 0.15F * dim),
                          u8(40 + l * 0.20F * dim), false};
                }
              const int head = std::min(N - 1, static_cast<int>(t * N));
              for (int i = 1; i <= head; ++i)
              {
                const float ageF = 1.0F - static_cast<float>(head - i) / N;
                drawSeg(dst, w, h, path[i - 1].first, path[i - 1].second, path[i].first,
                        path[i].second, std::max(1.0F, mn * 0.004F * ageF), ya,
                        Rgb{u8(180 * ageF), u8(200 * ageF), u8(240 * ageF), false});
              }
              if (head < N)
              {
                drawDataDisk(dst, w, h, src, path[head].first, path[head].second, mn * 0.020F, ya,
                             0.80F, t * 2.0F, Rgb{240, 240, 255, false});
              }
              // Many tiny invisible "molecules" hinted as flecks.
              for (int i = 0; i < 60; ++i)
              {
                const float fx = std::fmod(hash(i) * w + t * 80, static_cast<float>(w));
                const float fy = std::fmod(hash(i * 3) * h + t * 50, static_cast<float>(h));
                plotDot(dst, w, h, fx, fy, std::max(1.0F, mn * 0.002F), ya,
                        Rgb{120, 120, 160, false});
              }
            });
}

// Acid Trip: the data does a violent palette warp — psychedelic chromatic
// shifts, channel swapping, sinusoidal hue rotation — the chemical "acid"
// pun in pure colour.
void effectAcidTrip(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  (void)renderer;
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5200,
            [&](float t, std::vector<Rgb>& dst)
            {
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float br = s.transparent ? 100.0F : s.r;
                  const float bg = s.transparent ? 100.0F : s.g;
                  const float bb = s.transparent ? 120.0F : s.b;
                  const float wob = std::sin(x * 0.06F + y * 0.08F + t * 8.0F);
                  const float swap = 0.5F + 0.5F * std::sin(t * 4.0F);
                  const float r = br * (1 - swap) + bg * swap + 60 * wob;
                  const float g = bg * (1 - swap) + bb * swap + 60 * std::sin(t * 7.0F + x * 0.05F);
                  const float b = bb * (1 - swap) + br * swap + 60 * std::sin(t * 5.0F + y * 0.05F);
                  dst[static_cast<std::size_t>(y) * w + x] = Rgb{u8(r), u8(g), u8(b), false};
                }
            });
}

// Catalyst surface: molecules zip around, collide on a catalytic surface
// at the bottom, and emerge as paired products. The molecule discs are
// data-textured so the substrate carries the data.
void effectCatalyst(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 5200,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float dim = 0.20F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 40.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(28 + l * 0.10F * dim), u8(30 + l * 0.10F * dim),
                          u8(36 + l * 0.10F * dim), false};
                }
              const float surfaceY = h * 0.80F;
              for (int x = 0; x < w; ++x)
                for (int yo = 0; yo <= static_cast<int>(mn * 0.04F); ++yo)
                {
                  const int yy = static_cast<int>(surfaceY) + yo;
                  if (yy < h)
                    dst[static_cast<std::size_t>(yy) * w + x] = Rgb{120, 100, 60, false};
                }
              // Molecules bounce against the surface.
              for (int i = 0; i < 14; ++i)
              {
                const float phase = hash(i) * 6.2832F;
                const float bounce = std::fabs(std::sin(t * 3.0F + phase));
                const float bx = w * (0.1F + 0.8F * hash(i * 3));
                const float by = surfaceY - mn * 0.06F - bounce * mn * 0.25F;
                const Rgb tint =
                    (i & 1) ? Rgb{210, 120, 80, false} : Rgb{80, 180, 210, false};
                drawDataDisk(dst, w, h, src, bx, by, mn * 0.022F, ya, 0.75F, t + i, tint);
              }
            });
}

// Glow Stick: a long horizontal capsule is snapped (kinked once) and then
// chemiluminescent green fills it from the snap outward. The tube interior
// is data-sampled so the glow blends with the data.
void effectGlowStick(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5200,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float dim = 0.15F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 40.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(14 + l * 0.06F * dim), u8(18 + l * 0.08F * dim),
                          u8(20 + l * 0.06F * dim), false};
                }
              const float midY = h * 0.5F;
              const float tubeLen = w * 0.7F;
              const float tubeR = mn * 0.04F;
              const float snapX = w * 0.5F;
              const float snapT = 0.18F;
              const float snapAng = std::clamp((t - snapT) / 0.05F, 0.0F, 1.0F) * 0.18F;
              const float spread = std::clamp((t - snapT) / 0.50F, 0.0F, 1.0F);
              const float L = tubeLen * 0.5F;
              for (float u = -1.0F; u <= 1.0F; u += 1.0F / (tubeLen * 0.5F))
              {
                const float bend = (u < 0) ? -snapAng : snapAng;
                const float xx = w * 0.5F + u * L;
                const float yy = midY + std::sin(bend) * std::fabs(u) * mn * 0.04F;
                const float yLo = yy - tubeR, yHi = yy + tubeR;
                const bool inGlow = std::fabs(xx - snapX) / L < spread;
                for (int iy = static_cast<int>(yLo); iy <= static_cast<int>(yHi); ++iy)
                {
                  if (iy < 0 || iy >= h) continue;
                  const int ix = static_cast<int>(xx);
                  if (ix < 0 || ix >= w) continue;
                  if (inGlow)
                  {
                    const Rgb d = sample(src, w, h, xx, iy);
                    const float dg = d.transparent ? 200.0F : d.g;
                    dst[static_cast<std::size_t>(iy) * w + ix] =
                        Rgb{u8(120 + dg * 0.20F), u8(255), u8(140 + dg * 0.20F), false};
                  }
                  else
                  {
                    dst[static_cast<std::size_t>(iy) * w + ix] = Rgb{200, 200, 210, false};
                  }
                }
              }
            });
}

// Mentos & Coke geyser: silhouette of a bottle, Mentos drop in, foam fountain
// erupts upward. Mentos are data-tinted balls, the bottle fill is sampled
// data so the prank carries the weather.
void effectMentos(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 50.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(16 + l * 0.10F), u8(20 + l * 0.10F), u8(24 + l * 0.10F), false};
                }
              const float cxw = w * 0.5F;
              const float bottleTop = h * 0.40F;
              const float bottleBot = h * 0.92F;
              // Bottle body (data-filled).
              for (int yy = static_cast<int>(bottleTop); yy <= static_cast<int>(bottleBot); ++yy)
              {
                const float yf = (yy - bottleTop) / (bottleBot - bottleTop);
                const int half = static_cast<int>(mn * (0.05F + 0.04F * yf));
                for (int xo = -half; xo <= half; ++xo)
                {
                  const int xx = static_cast<int>(cxw + xo);
                  if (xx < 0 || xx >= w || yy >= h) continue;
                  const Rgb d = sample(src, w, h, xx, yy);
                  const float dr = d.transparent ? 90.0F : d.r;
                  const float dg = d.transparent ? 40.0F : d.g;
                  const float db = d.transparent ? 30.0F : d.b;
                  dst[static_cast<std::size_t>(yy) * w + xx] =
                      Rgb{u8(90 + dr * 0.30F), u8(40 + dg * 0.20F), u8(30 + db * 0.15F), false};
                }
              }
              // Bottle cap.
              for (int yy = static_cast<int>(bottleTop - mn * 0.04F); yy < static_cast<int>(bottleTop);
                   ++yy)
              {
                const int half = static_cast<int>(mn * 0.04F);
                for (int xo = -half; xo <= half; ++xo)
                {
                  const int xx = static_cast<int>(cxw + xo);
                  if (xx >= 0 && xx < w && yy >= 0)
                    dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{160, 30, 30, false};
                }
              }
              // Drop a few Mentos in early.
              if (t < 0.25F)
              {
                for (int i = 0; i < 3; ++i)
                {
                  const float my = bottleTop - mn * 0.10F + (t / 0.25F) * mn * 0.15F + i * mn * 0.04F;
                  drawDataDisk(dst, w, h, src, cxw + (i - 1) * mn * 0.02F, my, mn * 0.020F, ya,
                               0.75F, t * 2.0F, Rgb{230, 230, 220, false});
                }
              }
              // Erupting foam fountain.
              if (t > 0.25F)
              {
                const float ft = (t - 0.25F) / 0.75F;
                const Rgb foam{220, 220, 230, false};
                for (int i = 0; i < 40; ++i)
                {
                  const float ph = hash(i) * 6.2832F;
                  const float vx = std::cos(ph) * mn * 0.04F;
                  const float vy = -mn * (0.20F + hash(i * 3) * 0.30F);
                  const float fx = cxw + vx * ft * 5.0F;
                  const float fy = bottleTop + vy * ft + 9.8F * mn * 0.5F * ft * ft;
                  if (fy < bottleTop + mn * 0.05F)
                    drawDataDisk(dst, w, h, src, fx, fy, mn * 0.015F, ya, 0.85F, t + i, foam);
                }
              }
            });
}

// Avogadro Mole: the chemistry-mole pun — a small burrowing mole pokes out
// of a hole carrying a sign "6.022 × 10^23". The mole body is data-textured.
void effectAvogadro(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5200,
            [&](float t, std::vector<Rgb>& dst)
            {
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 60.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  const float sf = static_cast<float>(y) / h;
                  if (y > h * 0.78F)
                    dst[static_cast<std::size_t>(y) * w + x] =
                        Rgb{u8(80 + l * 0.10F), u8(60 + l * 0.08F), u8(40 + l * 0.06F), false};
                  else
                    dst[static_cast<std::size_t>(y) * w + x] =
                        Rgb{u8(140 + 50 * (1 - sf) + l * 0.10F),
                            u8(180 + 50 * (1 - sf) + l * 0.10F),
                            u8(220 - 30 * sf + l * 0.10F), false};
                }
              const float groundY = h * 0.78F;
              const float cxw = w * 0.5F;
              // Hole — dark ellipse.
              plotDot(dst, w, h, cxw, groundY, mn * 0.08F, ya, Rgb{20, 14, 10, false});
              // Mole pokes up.
              const float rise = std::clamp(t / 0.5F, 0.0F, 1.0F);
              const float my = groundY - rise * mn * 0.10F;
              drawDataDisk(dst, w, h, src, cxw, my, mn * 0.07F, ya, 0.7F, 0.0F,
                           Rgb{80, 60, 50, false});
              // Tiny eyes + pink nose.
              plotDot(dst, w, h, cxw - mn * 0.020F, my - mn * 0.020F, std::max(1.0F, mn * 0.005F), ya,
                      Rgb{20, 10, 10, false});
              plotDot(dst, w, h, cxw + mn * 0.020F, my - mn * 0.020F, std::max(1.0F, mn * 0.005F), ya,
                      Rgb{20, 10, 10, false});
              plotDot(dst, w, h, cxw, my, std::max(1.0F, mn * 0.008F), ya,
                      Rgb{220, 130, 150, false});
              // Sign hovering above.
              if (rise >= 1.0F)
              {
                const std::string line = "6.022 X 10^23";
                const int sc = std::max(2, static_cast<int>(mn / 60.0F));
                const float lineW = static_cast<float>(line.size()) * 6 * sc;
                const float signX = cxw - lineW * 0.5F;
                const float signY = my - mn * 0.20F;
                for (int xo = -static_cast<int>(lineW * 0.55F);
                     xo <= static_cast<int>(lineW * 0.55F); ++xo)
                  for (int yo = -static_cast<int>(sc * 5); yo <= static_cast<int>(sc * 5); ++yo)
                  {
                    const int xx = static_cast<int>(cxw + xo);
                    const int yy = static_cast<int>(signY + yo);
                    if (xx >= 0 && xx < w && yy >= 0 && yy < h)
                      dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{240, 235, 200, false};
                  }
                for (int ci = 0; ci < static_cast<int>(line.size()); ++ci)
                {
                  const auto g = glyph5x7(line[ci]);
                  for (int fy = 0; fy < 7; ++fy)
                    for (int fx = 0; fx < 5; ++fx)
                      if (g[fy][fx] == '1')
                        plotDot(dst, w, h, signX + (ci * 6 + fx) * sc,
                                signY - sc * 3 + fy * sc,
                                std::max(1.0F, static_cast<float>(sc) * 0.5F), ya,
                                Rgb{40, 30, 20, false});
                }
              }
            });
}

// Mendeleev's dream: element symbols rain into their grid positions over an
// outline of the periodic table.
void effectMendeleev(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  constexpr int kCols = 18, kRows = 7;
  static const char* const kSyms[kRows][kCols] = {
      {"H",  "",   "",   "",   "",   "",   "",   "",   "",   "",   "",   "",   "",   "",   "",   "",   "",   "He"},
      {"Li", "Be", "",   "",   "",   "",   "",   "",   "",   "",   "",   "",   "B",  "C",  "N",  "O",  "F",  "Ne"},
      {"Na", "Mg", "",   "",   "",   "",   "",   "",   "",   "",   "",   "",   "Al", "Si", "P",  "S",  "Cl", "Ar"},
      {"K",  "Ca", "Sc", "Ti", "V",  "Cr", "Mn", "Fe", "Co", "Ni", "Cu", "Zn", "Ga", "Ge", "As", "Se", "Br", "Kr"},
      {"Rb", "Sr", "Y",  "Zr", "Nb", "Mo", "Tc", "Ru", "Rh", "Pd", "Ag", "Cd", "In", "Sn", "Sb", "Te", "I",  "Xe"},
      {"Cs", "Ba", "*",  "Hf", "Ta", "W",  "Re", "Os", "Ir", "Pt", "Au", "Hg", "Tl", "Pb", "Bi", "Po", "At", "Rn"},
      {"Fr", "Ra", "*",  "Rf", "Db", "Sg", "Bh", "Hs", "Mt", "Ds", "Rg", "Cn", "Nh", "Fl", "Mc", "Lv", "Ts", "Og"}};
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 30.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(20 + l * 0.10F), u8(22 + l * 0.10F), u8(36 + l * 0.14F), false};
                }
              const float cellW = std::min(w * 0.9F / kCols, h * 0.78F / kRows);
              const float cellH = cellW;
              const float startX = (w - cellW * kCols) * 0.5F;
              const float startY = (h - cellH * kRows) * 0.5F;
              for (int r = 0; r < kRows; ++r)
                for (int c = 0; c < kCols; ++c)
                {
                  const char* sym = kSyms[r][c];
                  if (sym[0] == '\0') continue;
                  const float tx = startX + c * cellW + cellW * 0.5F;
                  const float ty = startY + r * cellH + cellH * 0.5F;
                  const int seed = r * kCols + c;
                  const float startSx = hash(seed) * w;
                  const float startSy = -mn * 0.1F;
                  const float dropT = std::clamp(t - hash(seed * 3) * 0.4F, 0.0F, 1.0F);
                  const float ease = 1.0F - (1.0F - dropT) * (1.0F - dropT);
                  const float px = startSx + (tx - startSx) * ease;
                  const float py = startSy + (ty - startSy) * ease;
                  if (dropT <= 0.0F) continue;
                  const int sc = std::max(1, static_cast<int>(cellW / 12.0F));
                  for (int ci = 0; ci < 2 && sym[ci]; ++ci)
                  {
                    const auto g = glyph5x7(sym[ci]);
                    for (int fy = 0; fy < 7; ++fy)
                      for (int fx = 0; fx < 5; ++fx)
                        if (g[fy][fx] == '1')
                          plotDot(dst, w, h, px - sc * 3 + (ci * 6 + fx) * sc,
                                  py - sc * 3 + fy * sc,
                                  std::max(1.0F, static_cast<float>(sc) * 0.5F), ya,
                                  Rgb{u8(180 + 75 * dropT), u8(180 + 60 * dropT),
                                      u8(120 + 40 * dropT), false});
                  }
                }
            });
}

// Electrolysis: two electrodes in solution, bubbles rise from each at the
// stoichiometric 2:1 ratio (H2 : O2). Bubbles are data-textured spheres.
void effectElectrolysis(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              // Solution: data dimmed and shifted toward sky-blue.
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 70.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(20 + l * 0.18F), u8(40 + l * 0.20F), u8(80 + l * 0.30F), false};
                }
              const float cxl = w * 0.35F, cxr = w * 0.65F;
              const float botY = h * 0.85F;
              // Electrodes (vertical bars).
              for (int yy = static_cast<int>(h * 0.20F); yy <= static_cast<int>(botY); ++yy)
              {
                for (int xo = -2; xo <= 2; ++xo)
                {
                  const int xL = static_cast<int>(cxl + xo), xR = static_cast<int>(cxr + xo);
                  if (yy >= 0 && yy < h)
                  {
                    if (xL >= 0 && xL < w) dst[static_cast<std::size_t>(yy) * w + xL] = Rgb{220, 220, 220, false};
                    if (xR >= 0 && xR < w) dst[static_cast<std::size_t>(yy) * w + xR] = Rgb{220, 220, 220, false};
                  }
                }
              }
              // Bubbles: more from left (cathode → H2), fewer from right (anode → O2).
              for (int i = 0; i < 40; ++i)
              {
                const float lifetime = hash(i) * 2.0F;
                const float bt = std::fmod(t + lifetime, 1.0F);
                const float bx = cxl + (hash(i * 3) - 0.5F) * mn * 0.04F;
                const float by = botY - bt * (botY - h * 0.15F);
                drawDataDisk(dst, w, h, src, bx, by, mn * 0.014F, ya, 0.50F, t + i,
                             Rgb{220, 220, 240, false});
              }
              for (int i = 0; i < 20; ++i)
              {
                const float lifetime = hash(i * 17) * 2.0F;
                const float bt = std::fmod(t + lifetime, 1.0F);
                const float bx = cxr + (hash(i * 5) - 0.5F) * mn * 0.04F;
                const float by = botY - bt * (botY - h * 0.15F);
                drawDataDisk(dst, w, h, src, bx, by, mn * 0.014F, ya, 0.50F, t + i,
                             Rgb{220, 220, 240, false});
              }
              // + / - labels.
              const int sc = std::max(2, static_cast<int>(mn / 40.0F));
              const auto gp = glyph5x7('+');
              const auto gm = glyph5x7('-');
              for (int fy = 0; fy < 7; ++fy)
                for (int fx = 0; fx < 5; ++fx)
                {
                  if (gm[fy][fx] == '1')
                    plotDot(dst, w, h, cxl + (fx - 2.5F) * sc, h * 0.10F + fy * sc,
                            std::max(1.0F, static_cast<float>(sc) * 0.5F), ya,
                            Rgb{240, 240, 240, false});
                  if (gp[fy][fx] == '1')
                    plotDot(dst, w, h, cxr + (fx - 2.5F) * sc, h * 0.10F + fy * sc,
                            std::max(1.0F, static_cast<float>(sc) * 0.5F), ya,
                            Rgb{240, 240, 240, false});
                }
            });
}

// Soap bubble: a single large iridescent bubble fills the screen; the data
// shows through the soap film with rainbow thin-film interference fringes.
// The bubble pops at the end.
void effectSoapBubble(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(
      renderer, w, h, 5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 40.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(20 + l * 0.10F), u8(22 + l * 0.10F), u8(30 + l * 0.10F), false};
          }
        const float pop = std::clamp((t - 0.85F) / 0.15F, 0.0F, 1.0F);
        const float cx = w * 0.5F + std::sin(t * 1.5F) * mn * 0.04F;
        const float cy = h * 0.5F + std::cos(t * 1.3F) * mn * 0.03F;
        const float R = mn * 0.35F * (1.0F + pop * 0.3F);
        // Fill the bubble interior with sampled data, modulated by iridescent
        // fringes.
        for (int yy = static_cast<int>(cy - R / ya); yy <= static_cast<int>(cy + R / ya); ++yy)
        {
          if (yy < 0 || yy >= h) continue;
          for (int xx = static_cast<int>(cx - R); xx <= static_cast<int>(cx + R); ++xx)
          {
            if (xx < 0 || xx >= w) continue;
            const float dx = xx - cx, dy = (yy - cy) * ya;
            const float r = std::hypot(dx, dy);
            if (r > R) continue;
            const Rgb d = sample(src, w, h, xx, yy);
            const float dr = d.transparent ? 200.0F : d.r;
            const float dg = d.transparent ? 200.0F : d.g;
            const float db = d.transparent ? 220.0F : d.b;
            const float fringe = std::sin(r / mn * 30.0F + t * 6.0F);
            const float hue = fringe * 0.5F + 0.5F;
            const float fr = (std::sin(hue * 6.2832F) + 1) * 60;
            const float fg = (std::sin(hue * 6.2832F + 2.094F) + 1) * 60;
            const float fb = (std::sin(hue * 6.2832F + 4.189F) + 1) * 60;
            const float edge = 1.0F - std::clamp((R - r) / (R * 0.20F), 0.0F, 1.0F);
            const float alpha = 1.0F - pop;
            dst[static_cast<std::size_t>(yy) * w + xx] =
                Rgb{u8((dr * 0.55F + fr + edge * 80) * alpha + dr * (1 - alpha)),
                    u8((dg * 0.55F + fg + edge * 80) * alpha + dg * (1 - alpha)),
                    u8((db * 0.55F + fb + edge * 80) * alpha + db * (1 - alpha)), false};
          }
        }
        // Specular highlight.
        if (pop < 0.5F)
          plotDot(dst, w, h, cx - R * 0.35F, cy - R * 0.40F / ya, R * 0.08F, ya,
                  Rgb{255, 255, 255, false});
      });
}

// Liesegang rings: concentric coloured precipitation bands grow outward from
// a centre, each ring carrying a slice of the data so the periodic
// precipitation reads as a data-painted Petri dish.
void effectLiesegang(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5200,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float cx = w * 0.5F, cy = h * 0.5F;
              const float maxR = std::min(w, h) * 0.42F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const float dx = x - cx, dy = (y - cy) * ya;
                  const float r = std::hypot(dx, dy);
                  const Rgb d = sample(src, w, h, x, y);
                  const float dr = d.transparent ? 100.0F : d.r;
                  const float dg = d.transparent ? 100.0F : d.g;
                  const float db = d.transparent ? 120.0F : d.b;
                  if (r > maxR)
                    dst[static_cast<std::size_t>(y) * w + x] =
                        Rgb{u8(20 + dr * 0.08F), u8(24 + dg * 0.08F), u8(30 + db * 0.10F), false};
                  else
                  {
                    const float band = std::sin(r / mn * 22.0F - t * 5.0F);
                    const float front = std::clamp(t * maxR * 1.3F - r, 0.0F, 1.0F);
                    const float intensity = front * (0.5F + 0.5F * band);
                    dst[static_cast<std::size_t>(y) * w + x] =
                        Rgb{u8(220 + dr * 0.20F - intensity * 120),
                            u8(180 + dg * 0.20F - intensity * 60),
                            u8(80 + db * 0.20F + intensity * 100), false};
                  }
                }
              // Dish rim.
              for (float a = 0; a < 6.2832F; a += 0.02F)
                plotDot(dst, w, h, cx + std::cos(a) * maxR, cy + std::sin(a) * maxR / ya,
                        std::max(1.0F, mn * 0.005F), ya, Rgb{200, 200, 200, false});
            });
}

// Phase Transition: scattered "gas" molecules (small data balls) condense
// into close-packed "liquid" then snap into a regular "solid" lattice.
void effectPhaseTransition(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  constexpr int N = 56;
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float dim = 0.20F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 50.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(20 + l * 0.10F * dim), u8(20 + l * 0.10F * dim),
                          u8(30 + l * 0.10F * dim), false};
                }
              const float gasT = 0.35F, liqT = 0.65F;
              for (int i = 0; i < N; ++i)
              {
                const int gx = i % 8, gy = i / 8;
                const float latticeX = w * 0.18F + gx * w * 0.09F;
                const float latticeY = h * 0.25F + gy * mn * 0.10F;
                float px = latticeX + (hash(i) - 0.5F) * w * 0.6F * std::sin(t * 4.0F + i);
                float py = latticeY + (hash(i * 3) - 0.5F) * h * 0.6F * std::cos(t * 4.0F + i);
                if (t > gasT)
                {
                  const float mix = std::clamp((t - gasT) / (liqT - gasT), 0.0F, 1.0F);
                  px = px * (1 - mix) + latticeX * mix;
                  py = py * (1 - mix) + latticeY * mix;
                }
                if (t > liqT)
                {
                  px = latticeX;
                  py = latticeY;
                }
                drawDataDisk(dst, w, h, src, px, py, mn * 0.020F, ya, 0.75F, t + i,
                             Rgb{180, 220, 240, false});
              }
              // Phase label.
              const char* lab = t < gasT ? "GAS" : (t < liqT ? "LIQUID" : "SOLID");
              const int sc = std::max(3, static_cast<int>(mn / 25.0F));
              const std::string s(lab);
              const float lineW = static_cast<float>(s.size()) * 6 * sc;
              for (int ci = 0; ci < static_cast<int>(s.size()); ++ci)
              {
                const auto g = glyph5x7(s[ci]);
                for (int fy = 0; fy < 7; ++fy)
                  for (int fx = 0; fx < 5; ++fx)
                    if (g[fy][fx] == '1')
                      plotDot(dst, w, h, w * 0.5F - lineW * 0.5F + (ci * 6 + fx) * sc,
                              h * 0.85F + fy * sc, std::max(1.0F, static_cast<float>(sc) * 0.5F),
                              ya, Rgb{240, 240, 240, false});
              }
            });
}

// ---------------------------------------------------------------------------
// EVOLUTION THEME -----------------------------------------------------------
// ---------------------------------------------------------------------------

// March of Progress: silhouettes walking left-to-right, ape → australopithecine
// → erectus → sapiens, each progressively more upright.
void effectMarchOfProgress(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              // Sepia background.
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 100.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(190 + l * 0.10F), u8(160 + l * 0.10F), u8(110 + l * 0.08F), false};
                }
              const float groundY = h * 0.88F;
              for (int x = 0; x < w; ++x)
                dst[static_cast<std::size_t>(groundY) * w + x] = Rgb{60, 50, 40, false};
              const int N = 5;
              for (int i = 0; i < N; ++i)
              {
                const float frac = static_cast<float>(i) / (N - 1);
                const float cx = w * 0.12F + frac * w * 0.76F;
                const float ht = mn * (0.30F + 0.10F * frac);
                // Hunch decreases as i increases.
                const float hunch = (1.0F - frac) * 0.6F;
                const float walk = std::sin(t * 12.0F + i) * 0.3F;
                const Rgb fig{30, 22, 18, false};
                // Head.
                plotDot(dst, w, h, cx + hunch * ht * 0.3F, groundY - ht + hunch * ht * 0.2F,
                        ht * 0.10F, ya, fig);
                // Torso.
                drawSeg(dst, w, h, cx + hunch * ht * 0.2F, groundY - ht * 0.85F + hunch * ht * 0.15F,
                        cx, groundY - ht * 0.40F, ht * 0.10F, ya, fig);
                // Legs.
                drawSeg(dst, w, h, cx, groundY - ht * 0.40F, cx - ht * 0.15F + walk * ht * 0.1F,
                        groundY, ht * 0.08F, ya, fig);
                drawSeg(dst, w, h, cx, groundY - ht * 0.40F, cx + ht * 0.15F - walk * ht * 0.1F,
                        groundY, ht * 0.08F, ya, fig);
                // Arms.
                drawSeg(dst, w, h, cx, groundY - ht * 0.70F, cx - ht * 0.20F + walk * ht * 0.15F,
                        groundY - ht * 0.40F, ht * 0.06F, ya, fig);
                drawSeg(dst, w, h, cx, groundY - ht * 0.70F, cx + ht * 0.20F - walk * ht * 0.15F,
                        groundY - ht * 0.40F, ht * 0.06F, ya, fig);
              }
              // Time arrow.
              drawSeg(dst, w, h, w * 0.08F, h * 0.95F, w * 0.92F, h * 0.95F,
                      std::max(1.0F, mn * 0.004F), ya, Rgb{40, 30, 20, false});
              drawSeg(dst, w, h, w * 0.92F, h * 0.95F, w * 0.88F, h * 0.93F,
                      std::max(1.0F, mn * 0.004F), ya, Rgb{40, 30, 20, false});
            });
}

// Tree of Life: a radial phylogenetic tree branches outward from the centre,
// labels on the leaf tips, growing over the duration.
void effectTreeOfLife(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 30.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(20 + l * 0.10F), u8(22 + l * 0.10F), u8(20 + l * 0.06F), false};
                }
              const float cx = w * 0.5F, cy = h * 0.5F;
              const float maxR = std::min(w, h) * 0.40F;
              // Recursive-ish branching: start at centre, fan out.
              const Rgb branch{220, 200, 140, false};
              const int rings = 5;
              for (int r = 0; r < rings; ++r)
              {
                const float r0 = maxR * r / rings;
                const float r1 = maxR * (r + 1) / rings;
                const int n = (1 << (r + 1));
                const float reveal = std::clamp(t * rings - r, 0.0F, 1.0F);
                if (reveal <= 0.0F) continue;
                for (int i = 0; i < n; ++i)
                {
                  const float a = i / static_cast<float>(n) * 6.2832F;
                  const float x0 = cx + std::cos(a) * r0;
                  const float y0 = cy + std::sin(a) * r0 / ya;
                  const float x1 = cx + std::cos(a) * (r0 + (r1 - r0) * reveal);
                  const float y1 = cy + std::sin(a) * (r0 + (r1 - r0) * reveal) / ya;
                  drawSeg(dst, w, h, x0, y0, x1, y1, std::max(1.0F, mn * 0.004F), ya, branch);
                }
              }
              // Leaf-tip dots.
              const int leafN = 32;
              for (int i = 0; i < leafN; ++i)
              {
                const float a = i / static_cast<float>(leafN) * 6.2832F;
                const float lx = cx + std::cos(a) * maxR;
                const float ly = cy + std::sin(a) * maxR / ya;
                drawDataDisk(dst, w, h, src, lx, ly, mn * 0.012F, ya, 0.7F, 0.0F,
                             Rgb{180, 220, 140, false});
              }
              // Centre LUCA dot.
              plotDot(dst, w, h, cx, cy, mn * 0.020F, ya, Rgb{255, 240, 180, false});
            });
}

// Darwin's finches: four beak silhouettes morph from a common ancestor,
// arranged radially with arrows fanning out from the centre.
void effectFinches(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  struct BeakShape { float length, depth; const char* name; };
  static const std::array<BeakShape, 4> kBeaks = {
      BeakShape{0.30F, 0.06F, "WARBLER"},
      BeakShape{0.40F, 0.10F, "INSECT"},
      BeakShape{0.50F, 0.20F, "GROUND"},
      BeakShape{0.60F, 0.30F, "LARGE"}};
  runFrames(
      renderer, w, h, 5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float dim = 0.20F;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 60.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(200 + l * 0.10F * dim), u8(220 + l * 0.10F * dim),
                    u8(240 + l * 0.10F * dim), false};
          }
        // Common ancestor at centre.
        const float cx = w * 0.5F, cy = h * 0.5F;
        drawDataDisk(dst, w, h, src, cx, cy, mn * 0.05F, ya, 0.7F, 0.0F,
                     Rgb{120, 80, 60, false});
        // Four offspring at corners.
        const std::array<std::pair<float, float>, 4> positions = {
            std::make_pair(0.25F, 0.20F), std::make_pair(0.75F, 0.20F),
            std::make_pair(0.25F, 0.80F), std::make_pair(0.75F, 0.80F)};
        for (int i = 0; i < 4; ++i)
        {
          const float tx = w * positions[i].first;
          const float tyy = h * positions[i].second;
          const float reveal = std::clamp(t * 1.5F - i * 0.15F, 0.0F, 1.0F);
          if (reveal <= 0.0F) continue;
          drawSeg(dst, w, h, cx, cy, cx + (tx - cx) * reveal, cy + (tyy - cy) * reveal,
                  std::max(1.0F, mn * 0.004F), ya, Rgb{40, 30, 20, false});
          if (reveal > 0.8F)
          {
            // Beak silhouette = data-textured head + triangular beak.
            const BeakShape& b = kBeaks[i];
            const float headR = mn * 0.05F;
            drawDataDisk(dst, w, h, src, tx, tyy, headR, ya, 0.7F, 0.0F,
                         Rgb{180, 130, 90, false});
            // Beak as triangle pointing right.
            const float bx0 = tx + headR;
            const float bx1 = bx0 + mn * b.length;
            const float bd = mn * b.depth;
            for (float u = 0; u < 1.0F; u += 0.02F)
            {
              const float bx = bx0 + (bx1 - bx0) * u;
              const float byHalf = bd * (1.0F - u);
              plotDot(dst, w, h, bx, tyy, std::max(1.0F, byHalf), ya, Rgb{160, 100, 60, false});
            }
            // Name label.
            const int sc = std::max(1, static_cast<int>(mn / 60.0F));
            const std::string name(b.name);
            for (int ci = 0; ci < static_cast<int>(name.size()); ++ci)
            {
              const auto g = glyph5x7(name[ci]);
              for (int fy = 0; fy < 7; ++fy)
                for (int fx = 0; fx < 5; ++fx)
                  if (g[fy][fx] == '1')
                    plotDot(dst, w, h, tx + (ci * 6 + fx) * sc - name.size() * 3 * sc,
                            tyy + headR + sc * 2 + fy * sc,
                            std::max(1.0F, static_cast<float>(sc) * 0.5F), ya,
                            Rgb{40, 30, 20, false});
            }
          }
        }
      });
}

// Peppered Moth: two moths against a tree trunk; trunk darkens (industrial
// soot), the light moth fades away while the dark moth proliferates.
void effectPepperedMoth(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
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
        // Bark texture darkens with t.
        const float darkness = std::clamp(t * 0.9F, 0.0F, 1.0F);
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 100.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            const float scratch = std::sin(x * 0.20F + y * 0.04F) * 30 + std::sin(y * 0.15F) * 20;
            const float base = 200 - darkness * 180 + scratch * 0.3F;
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(base * 0.55F + l * 0.10F), u8(base * 0.50F + l * 0.08F),
                    u8(base * 0.42F + l * 0.06F), false};
          }
        // Pale moths fade out as t increases; dark moths fade in.
        const int nL = static_cast<int>((1.0F - darkness) * 8);
        const int nD = static_cast<int>(darkness * 8);
        for (int i = 0; i < nL; ++i)
        {
          const float mx = hash(i) * w;
          const float my = hash(i * 3) * h;
          const float mS = mn * 0.025F;
          plotDot(dst, w, h, mx, my, mS, ya, Rgb{210, 200, 180, false});
          plotDot(dst, w, h, mx - mS * 0.6F, my, mS * 0.5F, ya, Rgb{210, 200, 180, false});
          plotDot(dst, w, h, mx + mS * 0.6F, my, mS * 0.5F, ya, Rgb{210, 200, 180, false});
        }
        for (int i = 0; i < nD; ++i)
        {
          const float mx = hash((i + 99) * 7) * w;
          const float my = hash((i + 99) * 11) * h;
          const float mS = mn * 0.025F;
          plotDot(dst, w, h, mx, my, mS, ya, Rgb{40, 30, 20, false});
          plotDot(dst, w, h, mx - mS * 0.6F, my, mS * 0.5F, ya, Rgb{40, 30, 20, false});
          plotDot(dst, w, h, mx + mS * 0.6F, my, mS * 0.5F, ya, Rgb{40, 30, 20, false});
        }
      });
}

// Cambrian explosion: weird creatures (five-eyed, big-eyed) bloom from a
// flat sea floor in the duration of a few seconds. Body discs are
// data-textured so the explosion carries the field.
void effectCambrian(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 30.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  const float sf = static_cast<float>(y) / h;
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(8 + 20 * sf + l * 0.08F), u8(30 + 30 * sf + l * 0.10F),
                          u8(60 + 40 * sf + l * 0.12F), false};
                }
              const float floorY = h * 0.92F;
              for (int x = 0; x < w; ++x)
                for (int yo = 0; yo <= 2; ++yo)
                {
                  const int yy = static_cast<int>(floorY) + yo;
                  if (yy < h) dst[static_cast<std::size_t>(yy) * w + x] = Rgb{100, 80, 60, false};
                }
              const int N = 12;
              for (int i = 0; i < N; ++i)
              {
                const float reveal = std::clamp(t * 1.5F - hash(i) * 0.4F, 0.0F, 1.0F);
                if (reveal <= 0.0F) continue;
                const float bx = w * (0.08F + 0.84F * hash(i * 7));
                const float by = floorY - mn * (0.05F + 0.20F * hash(i * 11)) * reveal;
                const float bS = mn * (0.030F + 0.025F * hash(i * 13)) * reveal;
                drawDataDisk(dst, w, h, src, bx, by, bS, ya, 0.75F, t + i,
                             Rgb{u8(180 + hash(i * 19) * 75), u8(120 + hash(i * 23) * 100),
                                 u8(100 + hash(i * 29) * 100), false});
                // Eyes — varying numbers (1, 2, 5).
                const int nEyes = 1 + static_cast<int>(hash(i * 31) * 5);
                for (int e = 0; e < nEyes; ++e)
                {
                  const float ea = e / static_cast<float>(nEyes) * 6.2832F;
                  plotDot(dst, w, h, bx + std::cos(ea) * bS * 0.6F,
                          by + std::sin(ea) * bS * 0.6F, std::max(1.0F, bS * 0.18F), ya,
                          Rgb{30, 20, 30, false});
                }
                // Tentacles.
                for (int k = 0; k < 4; ++k)
                {
                  const float ka = k / 4.0F * 6.2832F + t;
                  drawSeg(dst, w, h, bx, by, bx + std::cos(ka) * bS * 1.2F,
                          by + std::sin(ka) * bS * 1.2F, std::max(1.0F, mn * 0.004F), ya,
                          Rgb{180, 130, 100, false});
                }
              }
            });
}

// Out of Africa: a 2D globe (data-textured!) with arrows fanning out from
// East Africa across to Europe, Asia, Oceania, and the Americas.
void effectOutOfAfrica(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                  dst[static_cast<std::size_t>(y) * w + x] = Rgb{6, 10, 22, false};
              const float cx = w * 0.5F, cy = h * 0.5F;
              const float R = std::min(w, h) * 0.40F;
              // Earth globe — data-textured.
              drawDataDisk(dst, w, h, src, cx, cy, R, ya, 0.85F, t * 0.4F,
                           Rgb{100, 180, 120, false});
              // East Africa origin (relative to centre of globe).
              const float ox = cx + R * 0.10F;
              const float oy = cy + R * 0.05F;
              plotDot(dst, w, h, ox, oy, mn * 0.022F, ya, Rgb{255, 80, 60, false});
              // Migration arrows fanning out.
              const std::array<std::pair<float, float>, 6> targets = {
                  std::make_pair(-0.35F, -0.20F),  // Europe
                  std::make_pair(0.20F, -0.20F),   // Asia
                  std::make_pair(0.40F, 0.00F),    // SE Asia
                  std::make_pair(0.55F, 0.25F),    // Australia
                  std::make_pair(-0.65F, 0.00F),   // Americas (across Atlantic shortcut)
                  std::make_pair(-0.50F, 0.20F)};  // South America
              for (int i = 0; i < 6; ++i)
              {
                const float reveal = std::clamp(t * 1.5F - i * 0.18F, 0.0F, 1.0F);
                if (reveal <= 0.0F) continue;
                const float tx = cx + targets[i].first * R;
                const float tyy = cy + targets[i].second * R / ya;
                const float px = ox + (tx - ox) * reveal;
                const float py = oy + (tyy - oy) * reveal;
                drawSeg(dst, w, h, ox, oy, px, py, std::max(1.0F, mn * 0.005F), ya,
                        Rgb{255, 200, 120, false});
                // Arrowhead.
                if (reveal > 0.8F)
                  plotDot(dst, w, h, px, py, mn * 0.012F, ya, Rgb{255, 200, 120, false});
              }
            });
}

// Cell → Organism: a single cell divides repeatedly (1 → 2 → 4 → 8 → 16),
// each cell a data-textured ball, reorganising into a multicellular blob.
void effectCellDivides(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float dim = 0.18F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 40.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(30 + l * 0.10F * dim), u8(30 + l * 0.10F * dim),
                          u8(38 + l * 0.10F * dim), false};
                }
              const int stage = std::min(5, static_cast<int>(t * 6.0F));
              const int nCells = 1 << stage;  // 1, 2, 4, 8, 16, 32
              const float cellR = mn * (0.30F / std::sqrt(static_cast<float>(nCells)));
              const float cx = w * 0.5F, cy = h * 0.5F;
              const Rgb tint{180, 200, 220, false};
              for (int i = 0; i < nCells; ++i)
              {
                const float a = i / static_cast<float>(nCells) * 6.2832F;
                const float ring = std::sqrt(static_cast<float>(i)) * cellR * 0.7F;
                const float bx = cx + std::cos(a + t) * ring;
                const float by = cy + std::sin(a + t) * ring / ya;
                drawDataDisk(dst, w, h, src, bx, by, cellR, ya, 0.80F, t + i, tint);
                // Nucleus dot.
                plotDot(dst, w, h, bx, by, cellR * 0.25F, ya, Rgb{80, 60, 100, false});
              }
              // Generation label.
              const int sc = std::max(2, static_cast<int>(mn / 50.0F));
              const std::string lab = "N=" + std::to_string(nCells);
              for (int ci = 0; ci < static_cast<int>(lab.size()); ++ci)
              {
                const auto g = glyph5x7(lab[ci]);
                for (int fy = 0; fy < 7; ++fy)
                  for (int fx = 0; fx < 5; ++fx)
                    if (g[fy][fx] == '1')
                      plotDot(dst, w, h, w * 0.08F + (ci * 6 + fx) * sc, h * 0.08F + fy * sc,
                              std::max(1.0F, static_cast<float>(sc) * 0.5F), ya,
                              Rgb{220, 240, 255, false});
              }
            });
}

// Punctuated equilibrium: step-line chart, long flat stretches interrupted by
// sharp vertical jumps. Eldredge & Gould in one image.
void effectPunctuated(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(
      renderer, w, h, 5200,
      [&](float t, std::vector<Rgb>& dst)
      {
        const float dim = 0.25F;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 40.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(24 + l * 0.10F * dim), u8(24 + l * 0.10F * dim),
                    u8(32 + l * 0.10F * dim), false};
          }
        // Axes.
        const float ax0 = w * 0.10F, ax1 = w * 0.92F;
        const float ay0 = h * 0.85F, ay1 = h * 0.15F;
        drawSeg(dst, w, h, ax0, ay0, ax1, ay0, std::max(1.0F, mn * 0.004F), ya,
                Rgb{220, 220, 220, false});
        drawSeg(dst, w, h, ax0, ay0, ax0, ay1, std::max(1.0F, mn * 0.004F), ya,
                Rgb{220, 220, 220, false});
        // Step-line: 4 plateaus separated by jumps.
        struct Step { float fracX; float level; };
        static const std::array<Step, 5> steps = {Step{0.00F, 0.15F}, Step{0.20F, 0.35F},
                                                  Step{0.45F, 0.55F}, Step{0.70F, 0.75F},
                                                  Step{0.90F, 0.90F}};
        const float reveal = std::clamp(t * 1.2F, 0.0F, 1.0F);
        float prevX = ax0, prevY = ay0 - (ay0 - ay1) * steps[0].level;
        for (std::size_t i = 0; i < steps.size(); ++i)
        {
          const float sx = ax0 + (ax1 - ax0) * steps[i].fracX;
          const float sy = ay0 - (ay0 - ay1) * steps[i].level;
          if (sx / w > reveal) break;
          // Flat plateau.
          drawSeg(dst, w, h, prevX, prevY, sx, prevY, std::max(1.0F, mn * 0.006F), ya,
                  Rgb{120, 200, 240, false});
          // Vertical jump.
          drawSeg(dst, w, h, sx, prevY, sx, sy, std::max(1.0F, mn * 0.006F), ya,
                  Rgb{220, 120, 80, false});
          prevX = sx;
          prevY = sy;
        }
        // Continuation to current reveal.
        const float revX = ax0 + (ax1 - ax0) * reveal;
        if (revX > prevX)
          drawSeg(dst, w, h, prevX, prevY, revX, prevY, std::max(1.0F, mn * 0.006F), ya,
                  Rgb{120, 200, 240, false});
        // Labels.
        const int sc = std::max(1, static_cast<int>(mn / 70.0F));
        const std::string lab = "PUNCTUATED EQUILIBRIUM";
        for (int ci = 0; ci < static_cast<int>(lab.size()); ++ci)
        {
          const char ch = lab[ci];
          if (ch == ' ') continue;
          const auto g = glyph5x7(ch);
          for (int fy = 0; fy < 7; ++fy)
            for (int fx = 0; fx < 5; ++fx)
              if (g[fy][fx] == '1')
                plotDot(dst, w, h, ax0 + (ci * 6 + fx) * sc, h * 0.04F + fy * sc,
                        std::max(1.0F, static_cast<float>(sc) * 0.5F), ya,
                        Rgb{200, 200, 220, false});
        }
      });
}

// Galápagos tortoise: slow silhouette of a giant tortoise crosses the screen,
// dome shell and saddle neck. Shell uses drawDataDisk so the back carries
// the data — the joke is the slowness.
void effectGalapagos(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5800,
            [&](float t, std::vector<Rgb>& dst)
            {
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 70.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  const float sf = static_cast<float>(y) / h;
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(140 + 80 * (1 - sf) + l * 0.10F),
                          u8(180 + 60 * (1 - sf) + l * 0.10F),
                          u8(140 + 30 * sf + l * 0.08F), false};
                }
              const float groundY = h * 0.85F;
              for (int x = 0; x < w; ++x)
                dst[static_cast<std::size_t>(groundY) * w + x] = Rgb{80, 60, 40, false};
              const float cx = -mn * 0.4F + t * (w + mn * 0.6F);
              const float S = mn * 0.25F;
              const Rgb skin{60, 80, 50, false};
              // Shell — data-textured dome.
              drawDataDisk(dst, w, h, src, cx, groundY - S * 0.4F, S * 0.7F, ya, 0.80F, 0.0F,
                           Rgb{80, 110, 60, false});
              // Underbody.
              for (int yo = -static_cast<int>(S * 0.08F); yo <= static_cast<int>(S * 0.08F); ++yo)
                for (int xo = -static_cast<int>(S * 0.65F); xo <= static_cast<int>(S * 0.65F); ++xo)
                {
                  const int xx = static_cast<int>(cx + xo);
                  const int yy = static_cast<int>(groundY - S * 0.05F) + yo;
                  if (xx >= 0 && xx < w && yy >= 0 && yy < h)
                    dst[static_cast<std::size_t>(yy) * w + xx] = skin;
                }
              // Neck + head.
              drawSeg(dst, w, h, cx - S * 0.55F, groundY - S * 0.45F, cx - S * 0.80F,
                      groundY - S * 0.65F, S * 0.10F, ya, skin);
              plotDot(dst, w, h, cx - S * 0.85F, groundY - S * 0.65F, S * 0.08F, ya, skin);
              // Legs.
              for (int leg = 0; leg < 4; ++leg)
              {
                const float lx = cx - S * 0.50F + leg * S * 0.30F;
                drawSeg(dst, w, h, lx, groundY - S * 0.10F, lx, groundY, S * 0.07F, ya, skin);
              }
            });
}

// Darwin's portrait: bearded silhouette fades in; "On the Origin of Species"
// types out below. Data settles into a marbled book-cover beige.
void effectDarwin(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float dim = 0.30F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 100.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(170 + l * 0.10F * dim), u8(140 + l * 0.10F * dim),
                          u8(95 + l * 0.10F * dim), false};
                }
              const float fade = std::clamp(t * 1.6F, 0.0F, 1.0F);
              const float cx = w * 0.5F, cy = h * 0.42F;
              const float S = mn * 0.30F;
              const Rgb ink{30, 20, 14, false};
              // Head (top dome).
              plotDot(dst, w, h, cx, cy - S * 0.15F, S * 0.25F * fade, ya, ink);
              // Beard (large oval below).
              for (int yo = static_cast<int>(S * 0.05F); yo <= static_cast<int>(S * 0.55F); ++yo)
              {
                const float yf = (yo - S * 0.05F) / (S * 0.50F);
                const int half = static_cast<int>(S * (0.30F + 0.10F * std::sin(yf * 3.14159F)) * fade);
                for (int xo = -half; xo <= half; ++xo)
                {
                  const int xx = static_cast<int>(cx + xo);
                  const int yy = static_cast<int>(cy + yo);
                  if (xx >= 0 && xx < w && yy >= 0 && yy < h)
                    dst[static_cast<std::size_t>(yy) * w + xx] = ink;
                }
              }
              // Eyebrows.
              if (fade > 0.6F)
              {
                drawSeg(dst, w, h, cx - S * 0.12F, cy - S * 0.20F, cx - S * 0.05F,
                        cy - S * 0.22F, std::max(1.0F, mn * 0.005F), ya, Rgb{200, 200, 200, false});
                drawSeg(dst, w, h, cx + S * 0.05F, cy - S * 0.22F, cx + S * 0.12F,
                        cy - S * 0.20F, std::max(1.0F, mn * 0.005F), ya, Rgb{200, 200, 200, false});
              }
              // Type out "ON THE ORIGIN OF SPECIES".
              const std::string text = "ON THE ORIGIN OF SPECIES";
              const float typeT = std::clamp((t - 0.45F) / 0.50F, 0.0F, 1.0F);
              const int nReveal = static_cast<int>(typeT * text.size());
              const int sc = std::max(2, static_cast<int>(mn / 50.0F));
              const float lineW = static_cast<float>(text.size()) * 6 * sc;
              for (int ci = 0; ci < nReveal && ci < static_cast<int>(text.size()); ++ci)
              {
                const char ch = text[ci];
                if (ch == ' ') continue;
                const auto g = glyph5x7(ch);
                for (int fy = 0; fy < 7; ++fy)
                  for (int fx = 0; fx < 5; ++fx)
                    if (g[fy][fx] == '1')
                      plotDot(dst, w, h, cx - lineW * 0.5F + (ci * 6 + fx) * sc,
                              h * 0.85F + fy * sc,
                              std::max(1.0F, static_cast<float>(sc) * 0.5F), ya, ink);
              }
            });
}

// Peacock display: feather tail unfurls; many iridescent eye-spots, each a
// data-textured disk so the tail wears the data.
void effectPeacock(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 30.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(20 + l * 0.10F), u8(28 + l * 0.10F), u8(40 + l * 0.10F), false};
                }
              const float cx = w * 0.5F, cy = h * 0.82F;
              const float spread = std::clamp(t * 1.2F, 0.0F, 1.0F);
              const int N = 80;
              for (int i = 0; i < N; ++i)
              {
                const float frac = i / static_cast<float>(N);
                const float a = (frac - 0.5F) * 3.14159F * 0.9F;
                const float r = mn * 0.55F * spread;
                const float fx = cx + std::sin(a) * r;
                const float fy = cy - std::cos(a) * r / ya;
                // Feather quill.
                drawSeg(dst, w, h, cx, cy, fx, fy, std::max(1.0F, mn * 0.002F), ya,
                        Rgb{60, 90, 30, false});
                // Eye spot.
                drawDataDisk(dst, w, h, src, fx, fy, mn * 0.022F, ya, 0.85F, frac * 6.0F,
                             Rgb{60, 100, 180, false});
                plotDot(dst, w, h, fx, fy, mn * 0.010F, ya, Rgb{30, 50, 100, false});
                plotDot(dst, w, h, fx, fy, mn * 0.005F, ya, Rgb{220, 180, 60, false});
              }
              // Body.
              plotDot(dst, w, h, cx, cy, mn * 0.05F, ya, Rgb{20, 60, 100, false});
              // Head + crest.
              plotDot(dst, w, h, cx, cy - mn * 0.07F, mn * 0.020F, ya, Rgb{30, 100, 160, false});
              for (int k = -2; k <= 2; ++k)
              {
                drawSeg(dst, w, h, cx + k * mn * 0.005F, cy - mn * 0.08F,
                        cx + k * mn * 0.010F, cy - mn * 0.12F, std::max(1.0F, mn * 0.003F), ya,
                        Rgb{30, 100, 160, false});
              }
            });
}

// Mendel's peas: a Punnett square fills in over 4 quadrants; below, four pea
// pods grow showing wrinkled/smooth phenotypes per genotype.
void effectMendel(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 80.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(220 + l * 0.08F), u8(212 + l * 0.08F), u8(186 + l * 0.06F), false};
                }
              const float cx = w * 0.5F, cy = h * 0.5F;
              const float S = std::min(w, h) * 0.30F;
              // 2x2 grid.
              const Rgb ink{40, 30, 20, false};
              for (int g = 0; g < 3; ++g)
              {
                drawSeg(dst, w, h, cx - S, cy - S + g * S, cx + S, cy - S + g * S,
                        std::max(1.0F, mn * 0.004F), ya, ink);
                drawSeg(dst, w, h, cx - S + g * S, cy - S, cx - S + g * S, cy + S,
                        std::max(1.0F, mn * 0.004F), ya, ink);
              }
              static const std::array<const char*, 4> kCells = {"YY", "Yy", "Yy", "yy"};
              const int sc = std::max(2, static_cast<int>(mn / 40.0F));
              for (int gr = 0; gr < 2; ++gr)
                for (int gc = 0; gc < 2; ++gc)
                {
                  const int idx = gr * 2 + gc;
                  const float reveal = std::clamp(t * 1.5F - idx * 0.15F, 0.0F, 1.0F);
                  if (reveal <= 0.0F) continue;
                  const float ccx = cx - S * 0.5F + gc * S;
                  const float ccy = cy - S * 0.5F + gr * S;
                  // Letters.
                  const std::string ll(kCells[idx]);
                  for (int ci = 0; ci < 2; ++ci)
                  {
                    const auto g = glyph5x7(ll[ci]);
                    for (int fy = 0; fy < 7; ++fy)
                      for (int fx = 0; fx < 5; ++fx)
                        if (g[fy][fx] == '1')
                          plotDot(dst, w, h, ccx - sc * 5 + (ci * 6 + fx) * sc,
                                  ccy - sc * 3 + fy * sc,
                                  std::max(1.0F, static_cast<float>(sc) * 0.5F), ya, ink);
                  }
                  // Pea pod.
                  const Rgb yellow{220, 200, 80, false};
                  const Rgb green{120, 180, 80, false};
                  const Rgb wrinkly{160, 130, 60, false};
                  const bool dominant = idx != 3;
                  drawDataDisk(dst, w, h, src, ccx, ccy + sc * 8, S * 0.20F, ya, 0.75F, 0.0F,
                               dominant ? yellow : wrinkly);
                  (void)green;
                }
              // Header.
              const std::string hdr = "F2 GENERATION 3:1";
              const int sh = std::max(2, static_cast<int>(mn / 40.0F));
              for (int ci = 0; ci < static_cast<int>(hdr.size()); ++ci)
              {
                const char ch = hdr[ci];
                if (ch == ' ') continue;
                const auto g = glyph5x7(ch);
                for (int fy = 0; fy < 7; ++fy)
                  for (int fx = 0; fx < 5; ++fx)
                    if (g[fy][fx] == '1')
                      plotDot(dst, w, h, w * 0.5F - hdr.size() * 3 * sh + (ci * 6 + fx) * sh,
                              h * 0.10F + fy * sh,
                              std::max(1.0F, static_cast<float>(sh) * 0.5F), ya, ink);
              }
            });
}

// Mutation cascade: a long ACGT sequence scrolls across; somewhere mid-screen
// a letter abruptly changes, the substitution ripples downstream.
void effectMutation(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  runFrames(renderer, w, h, 5200,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float dim = 0.20F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 30.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(20 + l * 0.10F * dim), u8(24 + l * 0.10F * dim),
                          u8(20 + l * 0.06F * dim), false};
                }
              const int sc = std::max(2, static_cast<int>(mn / 30.0F));
              const int cellW = 6 * sc;
              const float scrollPx = t * cellW * 12;
              const int charW = w / cellW + 2;
              const char nts[] = {'A', 'C', 'G', 'T'};
              const Rgb cols[4] = {Rgb{220, 90, 80, false}, Rgb{220, 200, 60, false},
                                   Rgb{80, 200, 100, false}, Rgb{80, 160, 230, false}};
              for (int row = 0; row < 6; ++row)
              {
                const float rowY = h * 0.18F + row * mn * 0.12F;
                for (int i = 0; i < charW; ++i)
                {
                  const float px = w - (i * cellW + std::fmod(scrollPx, cellW)) - cellW;
                  const int seed = row * 137 + i + static_cast<int>(scrollPx / cellW);
                  int nti = static_cast<int>(hash(seed) * 4) % 4;
                  // Mutation: at row 3, halfway through, force a colour shift.
                  bool mutated = false;
                  if (row >= 3 && i > 5 && t > 0.4F)
                  {
                    nti = (nti + 2) % 4;
                    mutated = true;
                  }
                  const auto g = glyph5x7(nts[nti]);
                  for (int fy = 0; fy < 7; ++fy)
                    for (int fx = 0; fx < 5; ++fx)
                      if (g[fy][fx] == '1')
                        plotDot(dst, w, h, px + fx * sc, rowY + fy * sc,
                                std::max(1.0F, static_cast<float>(sc) * 0.5F), ya,
                                mutated ? Rgb{255, 80, 80, false} : cols[nti]);
                }
              }
            });
}

// HMS Beagle: a tall ship silhouette sails across a starlit ocean; "1831-1836"
// scrolls underneath. Sea surface is sampled from the data.
void effectBeagle(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
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
        const float horY = h * 0.62F;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 60.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            if (y > horY)
            {
              const float ripple = std::sin(x * 0.3F + y * 0.15F + t * 6.0F);
              dst[static_cast<std::size_t>(y) * w + x] =
                  Rgb{u8(10 + l * 0.10F + ripple * 10), u8(30 + l * 0.12F + ripple * 14),
                      u8(60 + l * 0.18F + ripple * 18), false};
            }
            else
            {
              const float sf = static_cast<float>(y) / horY;
              dst[static_cast<std::size_t>(y) * w + x] =
                  Rgb{u8(8 + 20 * (1 - sf) + l * 0.06F), u8(12 + 20 * (1 - sf) + l * 0.06F),
                      u8(20 + 30 * (1 - sf) + l * 0.08F), false};
            }
          }
        for (int i = 0; i < 60; ++i)
        {
          const int sx = static_cast<int>(hash(i) * w);
          const int sy = static_cast<int>(hash(i * 3) * horY);
          const float tw = 0.5F + 0.5F * std::sin(t * 10.0F + i);
          dst[static_cast<std::size_t>(sy) * w + sx] = Rgb{u8(180 * tw), u8(180 * tw), u8(220 * tw), false};
        }
        // Ship moves left-to-right.
        const float sx = -mn * 0.3F + t * (w + mn * 0.6F);
        const float sy = horY - mn * 0.05F;
        const float SS = mn * 0.18F;
        const Rgb hull{20, 16, 14, false};
        // Hull (trapezoid).
        for (int yo = 0; yo <= static_cast<int>(SS * 0.18F); ++yo)
        {
          const float yf = yo / (SS * 0.18F);
          const int half = static_cast<int>(SS * (0.32F - 0.10F * yf));
          for (int xo = -half; xo <= half; ++xo)
          {
            const int xx = static_cast<int>(sx + xo);
            const int yy = static_cast<int>(sy + yo);
            if (xx >= 0 && xx < w && yy >= 0 && yy < h)
              dst[static_cast<std::size_t>(yy) * w + xx] = hull;
          }
        }
        // Three masts.
        for (int m = -1; m <= 1; ++m)
        {
          const float mx = sx + m * SS * 0.20F;
          drawSeg(dst, w, h, mx, sy, mx, sy - SS * 0.55F, std::max(1.0F, mn * 0.004F), ya, hull);
          // Sails.
          for (int s2 = 0; s2 < 3; ++s2)
          {
            const float sy2 = sy - SS * (0.12F + s2 * 0.16F);
            drawSeg(dst, w, h, mx - SS * 0.13F, sy2, mx + SS * 0.13F, sy2, SS * 0.05F, ya,
                    Rgb{230, 220, 200, false});
          }
        }
        // Years label.
        const std::string lab = "1831 - 1836";
        const int sc = std::max(2, static_cast<int>(mn / 50.0F));
        for (int ci = 0; ci < static_cast<int>(lab.size()); ++ci)
        {
          const char ch = lab[ci];
          if (ch == ' ') continue;
          const auto g = glyph5x7(ch);
          for (int fy = 0; fy < 7; ++fy)
            for (int fx = 0; fx < 5; ++fx)
              if (g[fy][fx] == '1')
                plotDot(dst, w, h, w * 0.5F - lab.size() * 3 * sc + (ci * 6 + fx) * sc,
                        h * 0.92F + fy * sc, std::max(1.0F, static_cast<float>(sc) * 0.5F), ya,
                        Rgb{220, 200, 160, false});
        }
      });
}

// Endosymbiosis: a large host cell engulfs a smaller bacterium-like cell;
// the smaller one settles inside and becomes a mitochondrion with cristae.
// Both cells are data-textured spheres.
void effectEndosymbiosis(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float dim = 0.25F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 50.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(40 + l * 0.10F * dim), u8(60 + l * 0.10F * dim),
                          u8(50 + l * 0.10F * dim), false};
                }
              const float hostCx = w * 0.45F, hostCy = h * 0.5F;
              const float hostR = mn * 0.22F;
              // Host cell (data-textured).
              drawDataDisk(dst, w, h, src, hostCx, hostCy, hostR, ya, 0.70F, t * 0.3F,
                           Rgb{180, 200, 230, false});
              // Nucleus.
              plotDot(dst, w, h, hostCx + hostR * 0.20F, hostCy - hostR * 0.10F, hostR * 0.25F, ya,
                      Rgb{100, 80, 140, false});
              // Smaller bacterium approaches and enters.
              const float approach = std::clamp(t * 1.5F, 0.0F, 1.0F);
              const float bx = w * 0.85F - approach * (w * 0.85F - hostCx + hostR * 0.50F);
              const float by = hostCy + std::sin(t * 2.0F) * mn * 0.04F * (1 - approach);
              const float bR = mn * 0.08F;
              drawDataDisk(dst, w, h, src, bx, by, bR, ya, 0.75F, t * 1.0F,
                           Rgb{220, 130, 80, false});
              // Cristae folds inside the bacterium when settled.
              if (approach >= 0.7F)
              {
                const float inT = (approach - 0.7F) / 0.3F;
                for (int k = -2; k <= 2; ++k)
                {
                  const float ky = by + k * bR * 0.30F;
                  drawSeg(dst, w, h, bx - bR * 0.5F, ky, bx + bR * 0.5F, ky,
                          std::max(1.0F, mn * 0.003F * inT), ya, Rgb{120, 50, 30, false});
                }
              }
            });
}

// Lucy: a small bipedal hominid skeleton silhouette stands at the centre;
// "3.2 MA" subtitle at the end. Australopithecus afarensis as one icon.
void effectLucy(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 80.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(170 + l * 0.10F), u8(140 + l * 0.10F), u8(90 + l * 0.08F), false};
                }
              const float cx = w * 0.5F, baseY = h * 0.92F;
              const float H = mn * 0.50F;
              const Rgb bone{220, 215, 195, false};
              // Skull.
              plotDot(dst, w, h, cx, baseY - H * 0.92F, H * 0.10F, ya, bone);
              // Jaw.
              plotDot(dst, w, h, cx + H * 0.02F, baseY - H * 0.84F, H * 0.06F, ya, bone);
              // Spine.
              drawSeg(dst, w, h, cx, baseY - H * 0.80F, cx, baseY - H * 0.45F,
                      std::max(1.0F, mn * 0.004F), ya, bone);
              // Ribs.
              for (int r = 0; r < 4; ++r)
              {
                const float yy = baseY - H * (0.75F - r * 0.05F);
                drawSeg(dst, w, h, cx - H * (0.06F + r * 0.005F), yy,
                        cx + H * (0.06F + r * 0.005F), yy, std::max(1.0F, mn * 0.003F), ya, bone);
              }
              // Pelvis.
              for (int xo = -static_cast<int>(H * 0.10F); xo <= static_cast<int>(H * 0.10F); ++xo)
                for (int yo = 0; yo <= static_cast<int>(H * 0.05F); ++yo)
                {
                  const int xx = static_cast<int>(cx + xo);
                  const int yy = static_cast<int>(baseY - H * 0.45F) + yo;
                  if (xx >= 0 && xx < w && yy >= 0 && yy < h)
                    dst[static_cast<std::size_t>(yy) * w + xx] = bone;
                }
              // Legs (bent slightly, bipedal).
              drawSeg(dst, w, h, cx - H * 0.05F, baseY - H * 0.40F, cx - H * 0.08F, baseY,
                      std::max(1.0F, mn * 0.004F), ya, bone);
              drawSeg(dst, w, h, cx + H * 0.05F, baseY - H * 0.40F, cx + H * 0.08F, baseY,
                      std::max(1.0F, mn * 0.004F), ya, bone);
              // Arms.
              drawSeg(dst, w, h, cx, baseY - H * 0.72F, cx - H * 0.10F, baseY - H * 0.48F,
                      std::max(1.0F, mn * 0.003F), ya, bone);
              drawSeg(dst, w, h, cx, baseY - H * 0.72F, cx + H * 0.10F, baseY - H * 0.48F,
                      std::max(1.0F, mn * 0.003F), ya, bone);
              // Speech bubble at the end.
              if (t > 0.5F)
              {
                const std::string line = "3.2 MA";
                const int sc = std::max(3, static_cast<int>(mn / 25.0F));
                const float lineW = static_cast<float>(line.size()) * 6 * sc;
                const float bx = cx + H * 0.20F, by = baseY - H * 0.90F;
                for (int xo = -static_cast<int>(lineW * 0.55F);
                     xo <= static_cast<int>(lineW * 0.55F); ++xo)
                  for (int yo = -static_cast<int>(sc * 5); yo <= static_cast<int>(sc * 5); ++yo)
                  {
                    const int xx = static_cast<int>(bx + xo);
                    const int yy = static_cast<int>(by + yo);
                    if (xx >= 0 && xx < w && yy >= 0 && yy < h)
                      dst[static_cast<std::size_t>(yy) * w + xx] = Rgb{240, 230, 200, false};
                  }
                for (int ci = 0; ci < static_cast<int>(line.size()); ++ci)
                {
                  const char ch = line[ci];
                  if (ch == ' ') continue;
                  const auto g = glyph5x7(ch);
                  for (int fy = 0; fy < 7; ++fy)
                    for (int fx = 0; fx < 5; ++fx)
                      if (g[fy][fx] == '1')
                        plotDot(dst, w, h, bx - lineW * 0.5F + (ci * 6 + fx) * sc,
                                by - sc * 3 + fy * sc,
                                std::max(1.0F, static_cast<float>(sc) * 0.5F), ya,
                                Rgb{40, 30, 20, false});
                }
              }
            });
}

// Mitochondrial Eve: a single central figure with descendant connections
// fanning out — a graphical of common matrilineal ancestry.
void effectMitochondrialEve(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(
      renderer, w, h, 5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 30.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(20 + l * 0.10F), u8(20 + l * 0.10F), u8(28 + l * 0.10F), false};
          }
        const float cx = w * 0.5F, cy = h * 0.5F;
        const int N = 24;
        for (int i = 0; i < N; ++i)
        {
          const float a = i / static_cast<float>(N) * 6.2832F;
          const float R = std::min(w, h) * 0.4F;
          const float reveal = std::clamp(t * 1.5F - i * 0.02F, 0.0F, 1.0F);
          if (reveal <= 0.0F) continue;
          const float dx = cx + std::cos(a) * R * reveal;
          const float dy = cy + std::sin(a) * R * reveal / ya;
          drawSeg(dst, w, h, cx, cy, dx, dy, std::max(1.0F, mn * 0.003F), ya,
                  Rgb{240, 180, 200, false});
          drawDataDisk(dst, w, h, src, dx, dy, mn * 0.018F, ya, 0.75F, 0.0F,
                       Rgb{200, 140, 160, false});
        }
        // Eve at centre — data-textured.
        drawDataDisk(dst, w, h, src, cx, cy, mn * 0.05F, ya, 0.85F, t * 0.2F,
                     Rgb{255, 100, 140, false});
        plotDot(dst, w, h, cx, cy, mn * 0.020F, ya, Rgb{255, 240, 240, false});
      });
}

// Co-evolution: a giraffe + an acacia tree growing taller together.
void effectCoEvolution(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 70.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  const float sf = static_cast<float>(y) / h;
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(200 - 30 * sf + l * 0.10F), u8(160 - 20 * sf + l * 0.10F),
                          u8(110 - 30 * sf + l * 0.08F), false};
                }
              const float groundY = h * 0.92F;
              for (int x = 0; x < w; ++x)
                for (int yo = 0; yo <= 2; ++yo)
                  if (groundY + yo < h)
                    dst[static_cast<std::size_t>(groundY + yo) * w + x] = Rgb{120, 90, 50, false};
              const float scale = t;
              // Tree on the right (acacia umbrella).
              const float treeX = w * 0.7F;
              const float treeTop = groundY - mn * 0.65F * scale;
              drawSeg(dst, w, h, treeX, groundY, treeX, treeTop, std::max(1.0F, mn * 0.012F), ya,
                      Rgb{90, 60, 30, false});
              for (int b = 0; b < 8; ++b)
              {
                const float a = (b - 3.5F) * 0.3F;
                const float bx = treeX + std::sin(a) * mn * 0.18F;
                const float by = treeTop - std::cos(a) * mn * 0.08F;
                plotDot(dst, w, h, bx, by, mn * 0.055F, ya, Rgb{70, 130, 70, false});
              }
              // Giraffe on the left.
              const float gx = w * 0.30F;
              const float bodyY = groundY - mn * 0.18F;
              const Rgb hide{220, 180, 110, false};
              const Rgb spots{120, 80, 40, false};
              // Body
              for (int yo = -static_cast<int>(mn * 0.06F); yo <= static_cast<int>(mn * 0.06F); ++yo)
                for (int xo = -static_cast<int>(mn * 0.12F); xo <= static_cast<int>(mn * 0.12F); ++xo)
                {
                  const int xx = static_cast<int>(gx + xo);
                  const int yy = static_cast<int>(bodyY + yo);
                  if (xx >= 0 && xx < w && yy >= 0 && yy < h)
                    dst[static_cast<std::size_t>(yy) * w + xx] = hide;
                }
              // Spots
              for (int i = 0; i < 12; ++i)
              {
                const float sa = i / 12.0F * 6.2832F;
                plotDot(dst, w, h, gx + std::cos(sa) * mn * 0.08F,
                        bodyY + std::sin(sa) * mn * 0.04F, std::max(1.0F, mn * 0.012F), ya, spots);
              }
              // Long neck — grows with t.
              const float neckTop = groundY - mn * 0.40F - mn * 0.30F * scale;
              drawSeg(dst, w, h, gx + mn * 0.08F, bodyY - mn * 0.04F, gx + mn * 0.20F, neckTop,
                      std::max(1.0F, mn * 0.04F), ya, hide);
              // Head + tongue.
              plotDot(dst, w, h, gx + mn * 0.22F, neckTop, mn * 0.030F, ya, hide);
              drawSeg(dst, w, h, gx + mn * 0.24F, neckTop, gx + mn * 0.34F, neckTop,
                      std::max(1.0F, mn * 0.006F), ya, Rgb{220, 90, 130, false});
              // Legs.
              for (int leg = 0; leg < 4; ++leg)
              {
                const float lx = gx - mn * 0.10F + leg * mn * 0.07F;
                drawSeg(dst, w, h, lx, bodyY + mn * 0.04F, lx, groundY, std::max(1.0F, mn * 0.014F),
                        ya, hide);
              }
            });
}

// Selection pressure: a population of variable-sized dots; a predator culls
// the larger ones; surviving smaller dots multiply. Dots are data-textured.
void effectSelection(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  auto hash = [](int n)
  { return std::fmod(std::sin(n * 12.9898F) * 43758.5453F, 1.0F) * 0.5F + 0.5F; };
  constexpr int N = 30;
  runFrames(renderer, w, h, 5400,
            [&](float t, std::vector<Rgb>& dst)
            {
              const float dim = 0.25F;
              for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                  const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
                  const float l = s.transparent ? 50.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
                  dst[static_cast<std::size_t>(y) * w + x] =
                      Rgb{u8(40 + l * 0.10F * dim), u8(50 + l * 0.10F * dim),
                          u8(36 + l * 0.10F * dim), false};
                }
              for (int i = 0; i < N; ++i)
              {
                const float x0 = hash(i) * w;
                const float y0 = hash(i * 3) * h;
                const float sz = mn * (0.012F + 0.030F * hash(i * 7));
                const bool large = sz > mn * 0.025F;
                const bool culled = large && t > 0.4F;
                if (culled && t > 0.4F + hash(i * 9) * 0.3F) continue;
                drawDataDisk(dst, w, h, src, x0, y0, sz, ya, 0.75F, t + i,
                             Rgb{160, 200, 120, false});
              }
              // Predator hint: a darting shadow.
              if (t > 0.3F && t < 0.6F)
              {
                const float pt = (t - 0.3F) / 0.3F;
                const float px = -mn * 0.2F + pt * (w + mn * 0.4F);
                const float py = h * 0.5F + std::sin(pt * 6.0F) * mn * 0.15F;
                plotDot(dst, w, h, px, py, mn * 0.06F, ya, Rgb{30, 20, 30, false});
              }
            });
}

// Cetacean transition: three sequential whale-like silhouettes — pakicetus
// (legged land mammal) → ambulocetus (crocodile-like swimmer) → modern whale.
void effectCetacean(const Renderer& renderer, const std::vector<Rgb>& src, int w, int h)
{
  const float ya = yAspectFor(renderer);
  const float mn = std::min(static_cast<float>(w), h * ya);
  auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)); };
  runFrames(
      renderer, w, h, 5400,
      [&](float t, std::vector<Rgb>& dst)
      {
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x)
          {
            const Rgb& s = src[static_cast<std::size_t>(y) * w + x];
            const float l = s.transparent ? 60.0F : (0.3F * s.r + 0.59F * s.g + 0.11F * s.b);
            const float sf = static_cast<float>(y) / h;
            dst[static_cast<std::size_t>(y) * w + x] =
                Rgb{u8(120 + 80 * (1 - sf) + l * 0.08F), u8(170 + 50 * (1 - sf) + l * 0.10F),
                    u8(220 - 40 * sf + l * 0.12F), false};
          }
        const float groundY = h * 0.85F;
        for (int x = 0; x < w; ++x)
          for (int yo = 0; yo <= 1; ++yo)
            dst[static_cast<std::size_t>(groundY + yo) * w + x] = Rgb{60, 100, 130, false};
        // Three sequential silhouettes; each reveals over its portion of t.
        const Rgb body{30, 30, 40, false};
        for (int stage = 0; stage < 3; ++stage)
        {
          const float reveal = std::clamp(t * 3.0F - stage, 0.0F, 1.0F);
          if (reveal <= 0.0F) continue;
          const float cx = w * (0.20F + stage * 0.30F);
          const float cy = groundY - mn * 0.10F;
          const float bodyL = mn * 0.16F;
          // Body = elongated ellipse.
          for (int yo = -static_cast<int>(mn * 0.03F); yo <= static_cast<int>(mn * 0.03F); ++yo)
            for (int xo = -static_cast<int>(bodyL); xo <= static_cast<int>(bodyL); ++xo)
            {
              const float nx = xo / bodyL, ny = yo / (mn * 0.03F);
              if (nx * nx + ny * ny > 1.0F) continue;
              const int xx = static_cast<int>(cx + xo);
              const int yy = static_cast<int>(cy + yo);
              if (xx >= 0 && xx < w && yy >= 0 && yy < h)
                dst[static_cast<std::size_t>(yy) * w + xx] = body;
            }
          // Legs (stage 0 + 1) or flippers (stage 2).
          if (stage == 0)
          {
            for (int leg = 0; leg < 4; ++leg)
            {
              const float lx = cx + (leg - 1.5F) * bodyL * 0.4F;
              drawSeg(dst, w, h, lx, cy, lx, groundY, std::max(1.0F, mn * 0.010F), ya, body);
            }
          }
          else if (stage == 1)
          {
            for (int leg = 0; leg < 4; ++leg)
            {
              const float lx = cx + (leg - 1.5F) * bodyL * 0.4F;
              drawSeg(dst, w, h, lx, cy + mn * 0.02F, lx + mn * 0.025F, cy + mn * 0.06F,
                      std::max(1.0F, mn * 0.008F), ya, body);
            }
          }
          else
          {
            // Flippers + tail fluke
            drawSeg(dst, w, h, cx - bodyL * 0.4F, cy + mn * 0.02F, cx - bodyL * 0.6F,
                    cy + mn * 0.07F, std::max(1.0F, mn * 0.010F), ya, body);
            drawSeg(dst, w, h, cx + bodyL, cy, cx + bodyL * 1.4F, cy - mn * 0.05F,
                    std::max(1.0F, mn * 0.014F), ya, body);
            drawSeg(dst, w, h, cx + bodyL, cy, cx + bodyL * 1.4F, cy + mn * 0.05F,
                    std::max(1.0F, mn * 0.014F), ya, body);
          }
          // Eye.
          plotDot(dst, w, h, cx - bodyL * 0.7F, cy - mn * 0.005F, std::max(1.0F, mn * 0.005F), ya,
                  Rgb{220, 220, 220, false});
        }
      });
}

// Effect roster. Keep in sync with the dispatch switch below and with the
// names in exitEffectName().
constexpr int kEffectCount = 191;
}  // namespace

int exitEffectCount()
{
  return kEffectCount;
}

const char* exitEffectName(int effectIndex)
{
  // Alphabetical roster (movie/book title case: short prepositions/articles
  // lowercased mid-string; acronyms preserved). The switch in playExitEffect
  // is dispatched by this index, so the two lists must stay in lockstep.
  static const char* const kNames[kEffectCount] = {
      "ACME Anvil",         "Acid Trip",          "Akira",              "Apocalypse",
      "Aurora",             "Avogadro",           "Ballet",             "Banksy Balloon",
      "Bass Spiral",        "Beagle",             "Benzene",            "Big Keyboard",
      "Black Hole",         "Bohr Atom",          "Bond Barrel",        "Bone Chandelier",
      "Bone Cut",           "Bouncing Ball",      "Boulder",            "Brachistochrone",
      "Brownian",           "Buckyball",          "Butterfly",          "Cambrian",
      "Catalyst",           "Ceci n'est pas",     "Cell Divides",       "Cetacean",
      "Chladni",            "Chromatography",     "Clockwork",          "Close Encounters",
      "Co-evolution",       "Countdown",          "Crab",               "CRT Off",
      "Darwin",             "DeLorean",           "Dial M",             "Dictator Globe",
      "Dissolve",           "DNA",                "Dodecahedron",       "Double Slit",
      "Doves",              "E.T.",               "Electrolysis",       "End Card",
      "Endosymbiosis",      "Euler Identity",     "Explode",            "Eye Wink",
      "Fade",               "Fall to Pieces",     "Feather",            "Film Burn",
      "Finches",            "Fire",               "Fireworks",          "Fish",
      "Flame Test",         "Flatland",           "Foot Stomp",         "Foucault Pendulum",
      "Fourier",            "Galapagos",          "Galileo Tower",      "Game of Life",
      "Glow Stick",         "Golden Spiral",      "Gotcha",             "Greek Dance",
      "Gunshot",            "HAL 9000",           "HAL Stare",          "Hitchcock",
      "Hokusai Wave",       "Implode + Ring",     "Inception",          "Interstellar",
      "Iris Out",           "Jaws",               "Jellyfish",          "Jurassic",
      "King Kong",          "Koyaanisqatsi",      "Lava Lamp",          "Lawrence",
      "Lebowski",           "Liesegang",          "Lorenz Attractor",   "Lucy",
      "Macarena",           "Magritte Bowler",    "Mandelbrot",         "March of Progress",
      "Mary Poppins",       "Matrix Drop",        "Memento",            "Mendel",
      "Mendeleev",          "Mentos",             "Mitochondrial Eve",  "Mobius",
      "Monolith",           "Monty Python",       "Moon Rocket",        "Munch Scream",
      "Mutation",           "NaCl Lattice",       "Neo",                "Newspaper",
      "Newton Cradle",      "Nosferatu",          "Out of Africa",      "Pac-Man",
      "Pac-Man Duel",       "Peacock",            "Pendulum Waves",     "Peppered Moth",
      "Periodic Table",     "Phase Transition",   "pH Strip",           "Pi",
      "Pink Panther",       "Pleasantville",      "Pride Rock",         "Psycho",
      "Pulp Fiction",       "Punctuated",         "Pythagoras",         "Python Wars",
      "Red Balloon",        "Riverdance",         "Rocky",              "Rosebud",
      "Rubik",              "Russian Dance",      "Saturn",             "Schrodinger",
      "Selection",          "Shawshank",          "Shining",            "Shiver",
      "Sierpinski",         "Silly Walk",         "Singin'",            "Skeleton Wave",
      "Snowfall",           "Soap Bubble",        "Solar System",       "Sound of Music",
      "Spider",             "Spiral",             "Spirited Train",     "Standing Wave",
      "Standoff",           "Star Gate",          "Star Wars",          "Stingray",
      "Strangelove",        "Submarine",          "Tatooine",           "Teardrop",
      "Tears in Rain",      "Test Card",          "Thanos Snap",        "That's All Folks",
      "The Birds",          "The End",            "Thelma & Louise",    "Thriller",
      "Thunderstorm",       "Titanic",            "Tornado",            "Totoro",
      "Tracks",             "Train",              "Tree of Life",       "Tron",
      "Truman",             "Tunnel",             "UFO",                "Up",
      "Utopia",             "Vertigo",            "Warhol Banana",      "Warp",
      "Wizard of Oz",       "Word Reveal",        "YMCA"};
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
  // Foot Stomp (62), Monty Python (105), Python Wars (131) — these already
  // end on a foot, so don't double-stomp them.
  if (stompRoll && e != 62 && e != 105 && e != 131)
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

  // Cases follow the alphabetical kNames[] order above.
  switch (e)
  {
    case 0: effectAcmeAnvil(renderer, frame, subW, subH); break;
    case 1: effectAcidTrip(renderer, frame, subW, subH); break;
    case 2: effectAkira(renderer, frame, subW, subH); break;
    case 3: effectApocalypse(renderer, frame, subW, subH); break;
    case 4: effectAurora(renderer, frame, subW, subH, rng); break;
    case 5: effectAvogadro(renderer, frame, subW, subH); break;
    case 6: effectBallet(renderer, frame, subW, subH); break;
    case 7: effectBanksyBalloon(renderer, frame, subW, subH); break;
    case 8: effectBassSpiral(renderer, frame, subW, subH); break;
    case 9: effectBeagle(renderer, frame, subW, subH); break;
    case 10: effectBenzene(renderer, frame, subW, subH); break;
    case 11: effectBigKeyboard(renderer, frame, subW, subH); break;
    case 12: effectBlackHole(renderer, frame, subW, subH); break;
    case 13: effectBohrAtom(renderer, frame, subW, subH); break;
    case 14: effectBond(renderer, frame, subW, subH); break;
    case 15: effectBoneChandelier(renderer, frame, subW, subH); break;
    case 16: effectBoneCut(renderer, frame, subW, subH); break;
    case 17: effectBall(renderer, frame, subW, subH); break;
    case 18: effectBoulder(renderer, frame, subW, subH); break;
    case 19: effectBrachistochrone(renderer, frame, subW, subH); break;
    case 20: effectBrownian(renderer, frame, subW, subH); break;
    case 21: effectBuckyball(renderer, frame, subW, subH); break;
    case 22: effectButterfly(renderer, frame, subW, subH); break;
    case 23: effectCambrian(renderer, frame, subW, subH); break;
    case 24: effectCatalyst(renderer, frame, subW, subH); break;
    case 25: effectCeciNestPas(renderer, frame, subW, subH); break;
    case 26: effectCellDivides(renderer, frame, subW, subH); break;
    case 27: effectCetacean(renderer, frame, subW, subH); break;
    case 28: effectChladni(renderer, frame, subW, subH); break;
    case 29: effectChromatography(renderer, frame, subW, subH); break;
    case 30: effectClockwork(renderer, frame, subW, subH); break;
    case 31: effectCloseEncounters(renderer, frame, subW, subH); break;
    case 32: effectCoEvolution(renderer, frame, subW, subH); break;
    case 33: effectCountdown(renderer, frame, subW, subH); break;
    case 34: effectCrab(renderer, frame, subW, subH); break;
    case 35: effectCrtOff(renderer, frame, subW, subH); break;
    case 36: effectDarwin(renderer, frame, subW, subH); break;
    case 37: effectDeLorean(renderer, frame, subW, subH); break;
    case 38: effectDialM(renderer, frame, subW, subH); break;
    case 39: effectGlobeDance(renderer, frame, subW, subH); break;
    case 40: effectDissolve(renderer, frame, subW, subH, rng); break;
    case 41: effectDNA(renderer, frame, subW, subH); break;
    case 42: effectDodecahedron(renderer, frame, subW, subH); break;
    case 43: effectDoubleSlit(renderer, frame, subW, subH); break;
    case 44: effectDoves(renderer, frame, subW, subH, rng); break;
    case 45: effectET(renderer, frame, subW, subH); break;
    case 46: effectElectrolysis(renderer, frame, subW, subH); break;
    case 47: effectEndCard(renderer, frame, subW, subH); break;
    case 48: effectEndosymbiosis(renderer, frame, subW, subH); break;
    case 49: effectEulerIdentity(renderer, frame, subW, subH); break;
    case 50: effectExplode(renderer, frame, subW, subH); break;
    case 51: effectEyewink(renderer, frame, subW, subH); break;
    case 52: effectFade(renderer, frame, subW, subH); break;
    case 53: effectFallToPieces(renderer, frame, subW, subH); break;
    case 54: effectFeather(renderer, frame, subW, subH); break;
    case 55: effectFilmBurn(renderer, frame, subW, subH); break;
    case 56: effectFinches(renderer, frame, subW, subH); break;
    case 57: effectFire(renderer, frame, subW, subH, rng); break;
    case 58: effectFireworks(renderer, frame, subW, subH, rng); break;
    case 59: effectFish(renderer, frame, subW, subH); break;
    case 60: effectFlameTest(renderer, frame, subW, subH); break;
    case 61: effectFlatland(renderer, frame, subW, subH, rng); break;
    case 62: effectFoot(renderer, frame, subW, subH); break;
    case 63: effectFoucaultPendulum(renderer, frame, subW, subH); break;
    case 64: effectFourier(renderer, frame, subW, subH); break;
    case 65: effectGalapagos(renderer, frame, subW, subH); break;
    case 66: effectGalileoTower(renderer, frame, subW, subH); break;
    case 67: effectGameOfLife(renderer, frame, subW, subH); break;
    case 68: effectGlowStick(renderer, frame, subW, subH); break;
    case 69: effectGoldenSpiral(renderer, frame, subW, subH); break;
    case 70: effectGotcha(renderer, frame, subW, subH); break;
    case 71: effectGreekDance(renderer, frame, subW, subH); break;
    case 72: effectGunshot(renderer, frame, subW, subH, rng); break;
    case 73: effectHal9000(renderer, frame, subW, subH); break;
    case 74: effectHalStare(renderer, frame, subW, subH); break;
    case 75: effectHitchcock(renderer, frame, subW, subH); break;
    case 76: effectHokusaiWave(renderer, frame, subW, subH); break;
    case 77: effectImplodeRing(renderer, frame, subW, subH); break;
    case 78: effectInception(renderer, frame, subW, subH); break;
    case 79: effectInterstellar(renderer, frame, subW, subH); break;
    case 80: effectIrisOut(renderer, frame, subW, subH); break;
    case 81: effectJaws(renderer, frame, subW, subH); break;
    case 82: effectJellyfish(renderer, frame, subW, subH); break;
    case 83: effectJurassic(renderer, frame, subW, subH); break;
    case 84: effectKong(renderer, frame, subW, subH); break;
    case 85: effectKoyaanisqatsi(renderer, frame, subW, subH); break;
    case 86: effectLavaLamp(renderer, frame, subW, subH); break;
    case 87: effectLawrence(renderer, frame, subW, subH); break;
    case 88: effectLebowski(renderer, frame, subW, subH); break;
    case 89: effectLiesegang(renderer, frame, subW, subH); break;
    case 90: effectLorenzAttractor(renderer, frame, subW, subH); break;
    case 91: effectLucy(renderer, frame, subW, subH); break;
    case 92: effectMacarena(renderer, frame, subW, subH); break;
    case 93: effectMagritteBowler(renderer, frame, subW, subH); break;
    case 94: effectMandelbrot(renderer, frame, subW, subH); break;
    case 95: effectMarchOfProgress(renderer, frame, subW, subH); break;
    case 96: effectMaryPoppins(renderer, frame, subW, subH); break;
    case 97: effectMatrix(renderer, frame, subW, subH, rng); break;
    case 98: effectMemento(renderer, frame, subW, subH); break;
    case 99: effectMendel(renderer, frame, subW, subH); break;
    case 100: effectMendeleev(renderer, frame, subW, subH); break;
    case 101: effectMentos(renderer, frame, subW, subH); break;
    case 102: effectMitochondrialEve(renderer, frame, subW, subH); break;
    case 103: effectMobius(renderer, frame, subW, subH); break;
    case 104: effectMonolith(renderer, frame, subW, subH); break;
    case 105: effectMontyPython(renderer, frame, subW, subH); break;
    case 106: effectMoonRocket(renderer, frame, subW, subH); break;
    case 107: effectMunchScream(renderer, frame, subW, subH); break;
    case 108: effectMutation(renderer, frame, subW, subH); break;
    case 109: effectNaClLattice(renderer, frame, subW, subH); break;
    case 110: effectNeo(renderer, frame, subW, subH); break;
    case 111: effectNewspaper(renderer, frame, subW, subH); break;
    case 112: effectNewtonCradle(renderer, frame, subW, subH); break;
    case 113: effectNosferatu(renderer, frame, subW, subH); break;
    case 114: effectOutOfAfrica(renderer, frame, subW, subH); break;
    case 115: effectPacman(renderer, frame, subW, subH); break;
    case 116: effectPacmanDuel(renderer, frame, subW, subH); break;
    case 117: effectPeacock(renderer, frame, subW, subH); break;
    case 118: effectPendulumWaves(renderer, frame, subW, subH); break;
    case 119: effectPepperedMoth(renderer, frame, subW, subH); break;
    case 120: effectPeriodicTable(renderer, frame, subW, subH); break;
    case 121: effectPhaseTransition(renderer, frame, subW, subH); break;
    case 122: effectPhStrip(renderer, frame, subW, subH); break;
    case 123: effectPi(renderer, frame, subW, subH); break;
    case 124: effectPinkPanther(renderer, frame, subW, subH); break;
    case 125: effectPleasantville(renderer, frame, subW, subH); break;
    case 126: effectPrideRock(renderer, frame, subW, subH); break;
    case 127: effectPsycho(renderer, frame, subW, subH); break;
    case 128: effectPulp(renderer, frame, subW, subH); break;
    case 129: effectPunctuated(renderer, frame, subW, subH); break;
    case 130: effectPythagoras(renderer, frame, subW, subH); break;
    case 131: effectPythonWars(renderer, frame, subW, subH); break;
    case 132: effectRedBalloon(renderer, frame, subW, subH); break;
    case 133: effectRiverdance(renderer, frame, subW, subH); break;
    case 134: effectRocky(renderer, frame, subW, subH); break;
    case 135: effectRosebud(renderer, frame, subW, subH, rng); break;
    case 136: effectRubik(renderer, frame, subW, subH, rng); break;
    case 137: effectRussianDance(renderer, frame, subW, subH); break;
    case 138: effectSaturn(renderer, frame, subW, subH); break;
    case 139: effectSchrodinger(renderer, frame, subW, subH); break;
    case 140: effectSelection(renderer, frame, subW, subH); break;
    case 141: effectShawshank(renderer, frame, subW, subH); break;
    case 142: effectShining(renderer, frame, subW, subH); break;
    case 143: effectShiver(renderer, frame, subW, subH, rng); break;
    case 144: effectSierpinski(renderer, frame, subW, subH); break;
    case 145: effectSillyWalk(renderer, frame, subW, subH); break;
    case 146: effectSinginRain(renderer, frame, subW, subH); break;
    case 147: effectSkeletonWave(renderer, frame, subW, subH); break;
    case 148: effectSnowflakes(renderer, frame, subW, subH, rng); break;
    case 149: effectSoapBubble(renderer, frame, subW, subH); break;
    case 150: effectSolarSystem(renderer, frame, subW, subH, rng); break;
    case 151: effectSoundOfMusic(renderer, frame, subW, subH); break;
    case 152: effectSpider(renderer, frame, subW, subH); break;
    case 153: effectSpiral(renderer, frame, subW, subH); break;
    case 154: effectSpiritedTrain(renderer, frame, subW, subH); break;
    case 155: effectStandingWave(renderer, frame, subW, subH); break;
    case 156: effectStandoff(renderer, frame, subW, subH); break;
    case 157: effectStarGate(renderer, frame, subW, subH); break;
    case 158: effectStarWars(renderer, frame, subW, subH); break;
    case 159: effectStingray(renderer, frame, subW, subH); break;
    case 160: effectStrangelove(renderer, frame, subW, subH); break;
    case 161: effectSubmarine(renderer, frame, subW, subH, rng); break;
    case 162: effectTatooine(renderer, frame, subW, subH); break;
    case 163: effectTeardrop(renderer, frame, subW, subH); break;
    case 164: effectTearsInRain(renderer, frame, subW, subH, rng); break;
    case 165: effectTestCard(renderer, frame, subW, subH, rng); break;
    case 166: effectThanos(renderer, frame, subW, subH, rng); break;
    case 167: effectThatsAllFolks(renderer, frame, subW, subH); break;
    case 168: effectBirds(renderer, frame, subW, subH); break;
    case 169: effectTheEnd(renderer, frame, subW, subH, rng); break;
    case 170: effectThelma(renderer, frame, subW, subH); break;
    case 171: effectThriller(renderer, frame, subW, subH); break;
    case 172: effectThunderstorm(renderer, frame, subW, subH, rng); break;
    case 173: effectTitanic(renderer, frame, subW, subH); break;
    case 174: effectTornado(renderer, frame, subW, subH); break;
    case 175: effectTotoro(renderer, frame, subW, subH); break;
    case 176: effectStandByMe(renderer, frame, subW, subH); break;
    case 177: effectTrain(renderer, frame, subW, subH); break;
    case 178: effectTreeOfLife(renderer, frame, subW, subH); break;
    case 179: effectTron(renderer, frame, subW, subH); break;
    case 180: effectTruman(renderer, frame, subW, subH); break;
    case 181: effectTunnel(renderer, frame, subW, subH); break;
    case 182: effectUFO(renderer, frame, subW, subH); break;
    case 183: effectUp(renderer, frame, subW, subH); break;
    case 184: effectUtopia(renderer, frame, subW, subH, rng, linesFrame, swedenMask); break;
    case 185: effectVertigo(renderer, frame, subW, subH); break;
    case 186: effectWarholBanana(renderer, frame, subW, subH); break;
    case 187: effectWarp(renderer, frame, subW, subH); break;
    case 188: effectOz(renderer, frame, subW, subH); break;
    case 189: effectWordReveal(renderer, frame, subW, subH, rng, words); break;
    case 190: effectYMCA(renderer, frame, subW, subH); break;
    default: effectFade(renderer, frame, subW, subH); break;
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
