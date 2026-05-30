#pragma once

// Auto-extracted helpers for the exit-effect engine.
// Each effect cpp #includes this header. Helpers live INLINE in the
// header (templates / inline functions / static-storage-life utilities)
// so per-theme TUs compile independently and the compiler still inlines
// the hot inner-loop ones at -O2.

#include "QdlessGraphics.h"
#include "QdlessImageSource.h"
#include "QdlessPalette.h"
#include "QdlessRenderer.h"

#include <boost/dll/runtime_symbol_info.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <sstream>
#include <thread>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include <poll.h>
#include <unistd.h>

namespace Qdless
{
namespace ee_detail
{

constexpr Rgb kBlank{0, 0, 0, true};

constexpr int kFps = 30;

constexpr int kFrameMs = 1000 / kFps;  // ~33 ms

// Speed multiplier for the skeleton dance effects. Each dance states its own
// natural rhythm (its beat at speed 1.0); this scales them all together, so one
// knob sets how frantic the whole crew is — and it can be tuned from outside
// later.
const auto speed = 5.0F;

inline Rgb sample(const std::vector<Rgb>& src, int w, int h, float fx, float fy)
{
  const int x = static_cast<int>(std::lround(fx));
  const int y = static_cast<int>(std::lround(fy));
  if (x < 0 || x >= w || y < 0 || y >= h)
    return kBlank;
  return src[static_cast<std::size_t>(y) * w + x];
}

inline void present(const Renderer& renderer, const std::vector<Rgb>& buf, int w, int h)
{
  std::ostringstream os;
  renderer.render(os, buf, w, h, 0, 0);
  const std::string body = os.str();
  std::fputs("\x1b[?2026h", stdout);
  std::fwrite(body.data(), 1, body.size(), stdout);
  std::fputs("\x1b[?2026l", stdout);
  std::fflush(stdout);
}

inline bool exitKeyPressed()
{
  struct pollfd pfd{STDIN_FILENO, POLLIN, 0};
  if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN) != 0)
  {
    char buf[32];
    return read(STDIN_FILENO, buf, sizeof(buf)) > 0;
  }
  return false;
}

inline bool g_stompArmed = false;

inline bool g_stompFired = false;

inline double g_stompTriggerMs = 0.0;

inline std::chrono::steady_clock::time_point g_stompArmStart;

inline std::vector<Rgb> g_stompCapture;

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

inline float yAspectFor(const Renderer& renderer)
{
  return 4.0F / static_cast<float>(subRowsForStyle(renderer.cornerStyle()));
}

inline std::array<const char*, 7> glyph5x7(char c)
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

inline std::vector<std::pair<int, int>> wordTargets(
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

inline Rgb hsv2rgb(float hue, float sat, float val)
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

inline void drawOcean(std::vector<Rgb>& dst, int w, int h, float waterY, float fade)
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

inline void drawSubmarine(std::vector<Rgb>& dst, int w, int h, float ya, float scx, float scy)
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

inline const char* const kExitWordlines[] = {
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

inline std::string findDataImage(const char* basename)
{
  std::vector<std::filesystem::path> candidates{
      std::filesystem::path("/usr/share/smartmet/qdless") / basename};
  try
  {
    const std::filesystem::path exeDir = boost::dll::program_location().parent_path().string();
    candidates.push_back(exeDir / "data" / basename);
  }
  catch (const std::exception&)
  {
  }
  if (const char* home = std::getenv("HOME"))
    candidates.push_back(std::filesystem::path(home) / ".config" / "qdless" / basename);
  for (const auto& p : candidates)
  {
    std::error_code ec;
    if (std::filesystem::exists(p, ec))
      return p.string();
  }
  return {};
}

inline std::unique_ptr<ImageSource> loadDataImage(const char* basename, std::size_t& fw, std::size_t& fh)
{
  const std::string path = findDataImage(basename);
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

inline std::unique_ptr<ImageSource> loadFootImage(std::size_t& fw, std::size_t& fh)
{
  return loadDataImage("foot.png", fw, fh);
}

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

inline void effectMontyPythonProcedural(const Renderer& renderer,
                                 const std::vector<Rgb>& src,
                                 int w,
                                 int h);

inline void effectMontyPythonProcedural(const Renderer& renderer,
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

inline void drawBulletHole(std::vector<Rgb>& dst, int w, int h, float cx, float cy, float r, float ya)
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

inline bool inSnowflake(float dx, float dyPx, float R, float ya)
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

inline void drawLines(std::vector<Rgb>& dst,
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

inline std::vector<char> buildTextBitmap(const std::vector<std::string>& lines, int& bw, int& bh)
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

inline void drawCrawl(std::vector<Rgb>& dst,
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

inline void drawDove(std::vector<Rgb>& dst,
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

inline void drawSphere(std::vector<Rgb>& dst,
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

inline const std::array<std::pair<float, float>, 13> kRay = {{{0.00F, 1.00F},
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

inline bool inTri(float px, float py, float ax, float ay, float bx, float by, float cx, float cy)
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

inline void drawSeg(std::vector<Rgb>& dst,
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

inline void drawSkeleton(std::vector<Rgb>& dst,
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

inline float skeletonStage(
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

inline Rgb boneTint(const std::vector<Rgb>& src, int w, int h, float x, float alpha)
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

inline bool globePxToLatLon(float cx, float cy, float R, float ya,
                            float cLat, float sLat, float centerLonDeg,
                            float px, float py, float& lat, float& lon)
{
  const float u = (px - cx) / R;
  const float v = (py - cy) * ya / R;
  const float r2 = u * u + v * v;
  if (r2 > 1.0F) return false;
  const float nz = std::sqrt(1.0F - r2);
  const float yE = v * cLat + nz * sLat;
  const float zE = -v * sLat + nz * cLat;
  lat = std::asin(std::clamp(yE, -1.0F, 1.0F)) * 180.0F / 3.14159F;
  lon = centerLonDeg + std::atan2(u, zE) * 180.0F / 3.14159F;
  return true;
}

inline bool globeLatLonToPx(float cx, float cy, float R, float ya,
                            float cLat, float sLat, float centerLonDeg,
                            float lat, float lon, float& px, float& py)
{
  const float latR = lat * 3.14159F / 180.0F;
  const float lonR = (lon - centerLonDeg) * 3.14159F / 180.0F;
  const float x3 = std::cos(latR) * std::sin(lonR);
  const float y3 = std::sin(latR);
  const float z3 = std::cos(latR) * std::cos(lonR);
  const float yV = y3 * cLat - z3 * sLat;
  const float zV = y3 * sLat + z3 * cLat;
  if (zV < 0.0F) return false;
  px = cx + x3 * R;
  py = cy - yV * R / ya;
  return true;
}

inline void drawFootSprite(std::vector<Rgb>& dst, int w, int h, float ya,
                           ImageSource* foot, std::size_t fw, std::size_t fh,
                           bool needChromaKey, float cx, float cy, float scaleH, float ang)
{
  if (foot == nullptr) return;
  const float aspect = (fh > 0) ? static_cast<float>(fw) / static_cast<float>(fh) : 1.5F;
  const float scaleW = scaleH * aspect;
  const float cs = std::cos(-ang), sn = std::sin(-ang);
  const float halfDiag = 0.5F * std::sqrt(scaleW * scaleW + scaleH * scaleH);
  const int x0 = static_cast<int>(std::max(0.0F, cx - halfDiag));
  const int x1 = static_cast<int>(std::min(static_cast<float>(w - 1), cx + halfDiag));
  const int y0 = static_cast<int>(std::max(0.0F, cy - halfDiag / ya));
  const int y1 = static_cast<int>(std::min(static_cast<float>(h - 1), cy + halfDiag / ya));
  auto isFootColour = [](const Rgb& c)
  { return !c.transparent && c.r > c.g + 12 && c.r > c.b + 24 && c.r > 110; };
  for (int yy = y0; yy <= y1; ++yy)
    for (int xx = x0; xx <= x1; ++xx)
    {
      const float dx = static_cast<float>(xx) - cx;
      const float dy = (static_cast<float>(yy) - cy) * ya;
      const float lx = dx * cs - dy * sn;
      const float lyL = dx * sn + dy * cs;
      const float u = lx / scaleW + 0.5F;
      const float v = lyL / scaleH + 0.5F;
      if (u < 0.0F || u > 1.0F || v < 0.0F || v > 1.0F) continue;
      const Rgb pix = foot->pixelAtUV(std::clamp(u, 0.0F, 0.999F), std::clamp(v, 0.0F, 0.999F));
      if (pix.transparent) continue;
      if (needChromaKey && !isFootColour(pix)) continue;
      dst[static_cast<std::size_t>(yy) * w + xx] = pix;
    }
}

struct FootSprite
{
  std::unique_ptr<ImageSource> img;
  std::size_t fw = 0, fh = 0;
  bool needChromaKey = false;
};

inline FootSprite loadPythonFootSprite()
{
  FootSprite s;
  s.img = loadDataImage("transfoot.png", s.fw, s.fh);
  if (!s.img) {
    s.img = loadFootImage(s.fw, s.fh);
    s.needChromaKey = true;
  }
  return s;
}

// Muybridge motion studies. Each motion is a sequence of pre-rendered
// rotoscope silhouettes (black on transparent alpha) prepared by
// scripts/gif2muybridge.py from the original 1880s GIFs on Wikimedia
// Commons. The list of motion names below has to match the directories
// under data/muybridge/. The frame count per motion is whatever the
// source GIF supplied — typically 9-32 frames.
struct MuybridgeMotion
{
  std::string name;
  std::vector<std::unique_ptr<ImageSource>> frames;
  std::vector<std::pair<std::size_t, std::size_t>> sizes;  // (w, h) per frame
};

constexpr const char* kMuybridgeMotions[] = {
    // Humans first — the rotoscope idea was originally human-focused.
    "man_walk",
    "stairs",
    "woman_walk",
    "disc_throw",
    "leapfrog",
    "dance",
    "somersault",
    "hammering",
    "wrestling",
    "waltz",
    // Animals — Sallie Gardner is here because she made the whole idea
    // famous in 1878 and the user explicitly asked for her.
    "race_horse",
    "horse_gallop",
    "horse_jump",
    "horse_walk",
    "elephant",
    "buffalo",
};
constexpr int kMuybridgeMotionCount =
    static_cast<int>(sizeof(kMuybridgeMotions) / sizeof(kMuybridgeMotions[0]));

// English labels shown on the gallery effect's caption row.
constexpr const char* kMuybridgeLabels[kMuybridgeMotionCount] = {
    "Man walking",
    "Climbing stairs",
    "Woman walking",
    "Throwing disc",
    "Leapfrog",
    "Dancing",
    "Somersault",
    "Hammering",
    "Wrestling",
    "Waltz",
    "Annie G racing",
    "Horse galloping",
    "Horse jumping",
    "Horse walking",
    "Elephant walking",
    "Buffalo galloping",
};

inline MuybridgeMotion loadMuybridgeMotion(const char* name)
{
  MuybridgeMotion m;
  m.name = name;
  // Discover frames by attempting frame_NN.png in sequence until one
  // fails. The conversion script numbers them sequentially from 00.
  for (int i = 0; i < 64; ++i)
  {
    char rel[128];
    std::snprintf(rel, sizeof(rel), "muybridge/%s/frame_%02d.png", name, i);
    std::size_t fw = 0, fh = 0;
    auto img = loadDataImage(rel, fw, fh);
    if (!img)
      break;
    m.frames.push_back(std::move(img));
    m.sizes.emplace_back(fw, fh);
  }
  return m;
}

// One-shot loader: returns a vector with every motion attempted. Motions
// that fail to find their files come back with empty .frames so the
// caller can skip them.
inline std::vector<MuybridgeMotion> loadAllMuybridgeMotions()
{
  std::vector<MuybridgeMotion> out;
  out.reserve(kMuybridgeMotionCount);
  for (int i = 0; i < kMuybridgeMotionCount; ++i)
    out.push_back(loadMuybridgeMotion(kMuybridgeMotions[i]));
  return out;
}

// Composite a Muybridge frame at (cx,cy) at a target height in screen
// pixels, tinted to `fg`. Alpha controls opacity so the antialiased edge
// from the LANCZOS downsample blends correctly with the underlying
// raster. The sprite's natural aspect is preserved.
inline void drawMuybridgeFrame(std::vector<Rgb>& dst, int w, int h, float ya,
                               const MuybridgeMotion& m, int frameIdx,
                               float cx, float cy, float spriteH, Rgb fg)
{
  if (m.frames.empty())
    return;
  const int idx = ((frameIdx % static_cast<int>(m.frames.size())) +
                   static_cast<int>(m.frames.size())) %
                  static_cast<int>(m.frames.size());
  const auto& img = m.frames[idx];
  const auto [iw, ih] = m.sizes[idx];
  if (img == nullptr || iw == 0 || ih == 0)
    return;
  const float aspect = static_cast<float>(iw) / static_cast<float>(ih);
  const float spriteW = spriteH * aspect;
  const int x0 = static_cast<int>(std::max(0.0F, cx - spriteW * 0.5F));
  const int x1 = static_cast<int>(std::min(static_cast<float>(w - 1), cx + spriteW * 0.5F));
  const int y0 = static_cast<int>(std::max(0.0F, cy - spriteH * 0.5F / ya));
  const int y1 = static_cast<int>(std::min(static_cast<float>(h - 1), cy + spriteH * 0.5F / ya));
  if (x1 <= x0 || y1 <= y0)
    return;
  for (int yy = y0; yy <= y1; ++yy)
  {
    const float v = (static_cast<float>(yy) - (cy - spriteH * 0.5F / ya)) /
                    (spriteH / ya);
    if (v < 0.0F || v > 1.0F)
      continue;
    for (int xx = x0; xx <= x1; ++xx)
    {
      const float u = (static_cast<float>(xx) - (cx - spriteW * 0.5F)) / spriteW;
      if (u < 0.0F || u > 1.0F)
        continue;
      const Rgb pix = img->pixelAtUV(std::clamp(u, 0.0F, 0.999F),
                                     std::clamp(v, 0.0F, 0.999F));
      // ImageSource collapses alpha to binary (anything < 128 is
      // transparent). Our PNGs are black-on-transparent; the Python
      // morphological cleanup already crispened edges, so a hard cutoff
      // is fine and matches what the loader is going to give us anyway.
      if (pix.transparent)
        continue;
      const std::size_t di = static_cast<std::size_t>(yy) * w + xx;
      dst[di] = fg;
      dst[di].transparent = false;
    }
  }
}

constexpr int kEffectCount = 327;

enum class Theme : std::uint8_t
{
  Cinema = 0,
  Cartoon,
  Music,
  Art,
  History,
  Myth,
  Physics,
  Chemistry,
  Biology,
  Weather,
  TerminalFx,
};

constexpr int kThemeCount = 11;

constexpr const char* kThemeNames[kThemeCount] = {
    "Cinema",
    "Cartoon, TV & games",
    "Music & dance",
    "Art",
    "History",
    "Myth & legend",
    "Maths & physics",
    "Chemistry",
    "Evolution & biology",
    "Weather & nature",
    "Terminal effects",
};

constexpr Theme kThemes[kEffectCount] = {
    /*   0 Accretion Disk         */ Theme::Physics,
    /*   1 Acid Trip              */ Theme::Chemistry,
    /*   2 ACME Anvil             */ Theme::Cartoon,
    /*   3 Akira                  */ Theme::Cinema,
    /*   4 AMOC                   */ Theme::Weather,
    /*   5 Andromeda              */ Theme::Physics,
    /*   6 Anubis                 */ Theme::Myth,
    /*   7 Apocalypse             */ Theme::Cinema,
    /*   8 Apollo 11              */ Theme::History,
    /*   9 Atlas                  */ Theme::Myth,
    /*  10 Aurora                 */ Theme::Weather,
    /*  11 Auroral Oval           */ Theme::Weather,
    /*  12 Avogadro               */ Theme::Chemistry,
    /*  13 Ballet                 */ Theme::Music,
    /*  14 Banksy Balloon         */ Theme::Art,
    /*  15 Bass Spiral            */ Theme::Music,
    /*  16 Beagle                 */ Theme::Biology,
    /*  17 Beethoven Fifth        */ Theme::Music,
    /*  18 Benzene                */ Theme::Chemistry,
    /*  19 Berlin Wall            */ Theme::History,
    /*  20 Big Bang               */ Theme::Physics,
    /*  21 Big Keyboard           */ Theme::Cinema,
    /*  22 Black Hole             */ Theme::Physics,
    /*  23 Bohr Atom              */ Theme::Physics,
    /*  24 Bok Globule            */ Theme::Physics,
    /*  25 Bond Barrel            */ Theme::Cinema,
    /*  26 Bone Chandelier        */ Theme::Art,
    /*  27 Bone Cut               */ Theme::Cinema,
    /*  28 Boulder                */ Theme::Cinema,
    /*  29 Bouncing Ball          */ Theme::TerminalFx,
    /*  30 Brachistochrone        */ Theme::Physics,
    /*  31 Brownian               */ Theme::Chemistry,
    /*  32 Buckyball              */ Theme::Chemistry,
    /*  33 Butterfly              */ Theme::Physics,
    /*  34 Cambrian               */ Theme::Biology,
    /*  35 Cassini Finale         */ Theme::Physics,
    /*  36 Catalyst               */ Theme::Chemistry,
    /*  37 Ceci n'est pas         */ Theme::Art,
    /*  38 Cell Divides           */ Theme::Biology,
    /*  39 Cepheid                */ Theme::Physics,
    /*  40 Cetacean               */ Theme::Biology,
    /*  41 Cezanne Still          */ Theme::Art,
    /*  42 Chinese Dragon         */ Theme::Myth,
    /*  43 Chladni                */ Theme::Physics,
    /*  44 Chromatography         */ Theme::Chemistry,
    /*  45 Clockwork              */ Theme::Cinema,
    /*  46 Close Encounters       */ Theme::Cinema,
    /*  47 CMB Glow               */ Theme::Physics,
    /*  48 Co-evolution           */ Theme::Biology,
    /*  49 Columbus               */ Theme::History,
    /*  50 Comet Tail             */ Theme::Physics,
    /*  51 Conductor Baton        */ Theme::Music,
    /*  52 Coriolis               */ Theme::Weather,
    /*  53 Cosmic Web             */ Theme::Physics,
    /*  54 Countdown              */ Theme::TerminalFx,
    /*  55 Crab                   */ Theme::TerminalFx,
    /*  56 Crab Nebula            */ Theme::Physics,
    /*  57 Crown                  */ Theme::History,
    /*  58 CRT Off                */ Theme::TerminalFx,
    /*  59 Crystal Ball           */ Theme::Cartoon,
    /*  60 Damocles Foot          */ Theme::Myth,
    /*  61 Darwin                 */ Theme::Biology,
    /*  62 Day Terminator         */ Theme::Physics,
    /*  63 Deep Field             */ Theme::Physics,
    /*  64 DeLorean               */ Theme::Cinema,
    /*  65 Derecho                */ Theme::Weather,
    /*  66 Dial M                 */ Theme::Cinema,
    /*  67 Dictator Globe         */ Theme::Cinema,
    /*  68 Dissolve               */ Theme::TerminalFx,
    /*  69 DNA                    */ Theme::Biology,
    /*  70 Dodecahedron           */ Theme::Physics,
    /*  71 Double Slit            */ Theme::Physics,
    /*  72 Doves                  */ Theme::Cinema,
    /*  73 Dyson Sphere           */ Theme::Physics,
    /*  74 E.T.                   */ Theme::Cinema,
    /*  75 Eden                   */ Theme::Myth,
    /*  76 Eiffel Tower           */ Theme::History,
    /*  77 El Nino                */ Theme::Weather,
    /*  78 Electrolysis           */ Theme::Chemistry,
    /*  79 End Card               */ Theme::TerminalFx,
    /*  80 Endosymbiosis          */ Theme::Biology,
    /*  81 Euler Identity         */ Theme::Physics,
    /*  82 Europa Ice             */ Theme::Physics,
    /*  83 Excalibur              */ Theme::Myth,
    /*  84 Explode                */ Theme::TerminalFx,
    /*  85 Eye Wink               */ Theme::TerminalFx,
    /*  86 Fade                   */ Theme::TerminalFx,
    /*  87 Fall to Pieces         */ Theme::TerminalFx,
    /*  88 Feather                */ Theme::Cinema,
    /*  89 Film Burn              */ Theme::Cinema,
    /*  90 Finches                */ Theme::Biology,
    /*  91 Fire                   */ Theme::TerminalFx,
    /*  92 Fireworks              */ Theme::TerminalFx,
    /*  93 Fish                   */ Theme::TerminalFx,
    /*  94 Flame Test             */ Theme::Chemistry,
    /*  95 Flatland               */ Theme::Physics,
    /*  96 Fogbow                 */ Theme::Weather,
    /*  97 Foot Stomp             */ Theme::Cartoon,
    /*  98 Foucault Pendulum      */ Theme::Physics,
    /*  99 Fourier                */ Theme::Physics,
    /* 100 Galapagos              */ Theme::Biology,
    /* 101 Galaxy Collision       */ Theme::Physics,
    /* 102 Galileo Telescope      */ Theme::History,
    /* 103 Galileo Tower          */ Theme::Physics,
    /* 104 Game of Life           */ Theme::Physics,
    /* 105 Garuda                 */ Theme::Myth,
    /* 106 Gas Giant              */ Theme::Physics,
    /* 107 Glow Stick             */ Theme::Chemistry,
    /* 108 Golden Spiral          */ Theme::Physics,
    /* 109 Gotcha                 */ Theme::TerminalFx,
    /* 110 Gravity Lens           */ Theme::Physics,
    /* 111 Greek Dance            */ Theme::Music,
    /* 112 Guillotine             */ Theme::History,
    /* 113 Gunshot                */ Theme::TerminalFx,
    /* 114 Gutenberg              */ Theme::History,
    /* 115 Hadley Cell            */ Theme::Weather,
    /* 116 HAL 9000               */ Theme::Cinema,
    /* 117 HAL Stare              */ Theme::Cinema,
    /* 118 Halley                 */ Theme::Physics,
    /* 119 Hawking Radiation      */ Theme::Physics,
    /* 120 Helix Nebula           */ Theme::Physics,
    /* 121 Hendrix Guitar         */ Theme::Music,
    /* 122 Hitchcock              */ Theme::Cinema,
    /* 123 Hokusai Wave           */ Theme::Art,
    /* 124 HR Diagram             */ Theme::Physics,
    /* 125 Hubble Expansion       */ Theme::Physics,
    /* 126 Hubble Telescope       */ Theme::Physics,
    /* 127 Hurricane Eye          */ Theme::Weather,
    /* 128 Hurricane Tracks       */ Theme::Weather,
    /* 129 Implode + Ring         */ Theme::TerminalFx,
    /* 130 Inception              */ Theme::Cinema,
    /* 131 Indy Idol              */ Theme::Cinema,
    /* 132 Interstellar           */ Theme::Cinema,
    /* 133 Iris Out               */ Theme::TerminalFx,
    /* 134 ISS Track              */ Theme::Physics,
    /* 135 ITCZ                   */ Theme::Weather,
    /* 136 Jaws                   */ Theme::Cinema,
    /* 137 Jellyfish              */ Theme::TerminalFx,
    /* 138 Jet Stream             */ Theme::Weather,
    /* 139 Jurassic               */ Theme::Cinema,
    /* 140 JWST                   */ Theme::Physics,
    /* 141 King Kong              */ Theme::Cinema,
    /* 142 Koyaanisqatsi          */ Theme::Cinema,
    /* 143 Krakatoa               */ Theme::Weather,
    /* 144 Lagrange Points        */ Theme::Physics,
    /* 145 Lava Lamp              */ Theme::Chemistry,
    /* 146 Lawrence               */ Theme::Cinema,
    /* 147 Lebowski               */ Theme::Cinema,
    /* 148 Liberty Torch          */ Theme::History,
    /* 149 Liesegang              */ Theme::Chemistry,
    /* 150 LIGO Chirp             */ Theme::Physics,
    /* 151 Lorenz Attractor       */ Theme::Physics,
    /* 152 Lucy                   */ Theme::Biology,
    /* 153 Lunar Eclipse          */ Theme::Physics,
    /* 154 Macarena               */ Theme::Music,
    /* 155 Magna Carta            */ Theme::History,
    /* 156 Magnetar               */ Theme::Physics,
    /* 157 Magnetosphere          */ Theme::Physics,
    /* 158 Magritte Bowler        */ Theme::Art,
    /* 159 Mammatus               */ Theme::Weather,
    /* 160 Mandelbrot             */ Theme::Physics,
    /* 161 March of Progress      */ Theme::Biology,
    /* 162 Mars Rover             */ Theme::Physics,
    /* 163 Mary Poppins           */ Theme::Cinema,
    /* 164 Matrix Drop            */ Theme::Cinema,
    /* 165 Memento                */ Theme::Cinema,
    /* 166 Mendel                 */ Theme::Biology,
    /* 167 Mendeleev              */ Theme::Chemistry,
    /* 168 Mentos                 */ Theme::Chemistry,
    /* 169 Mitochondrial Eve      */ Theme::Biology,
    /* 170 MJO                    */ Theme::Weather,
    /* 171 Mjolnir                */ Theme::Myth,
    /* 172 Mobius                 */ Theme::Physics,
    /* 173 Mona Lisa              */ Theme::Art,
    /* 174 Monolith               */ Theme::Cinema,
    /* 175 Monsoon                */ Theme::Weather,
    /* 176 Monty Python           */ Theme::Cartoon,
    /* 177 Moon Flag              */ Theme::History,
    /* 178 Moon Phases            */ Theme::Physics,
    /* 179 Moon Rocket            */ Theme::Cinema,
    /* 180 Munch Scream           */ Theme::Art,
    /* 181 Mutation               */ Theme::Biology,
    /* 182 Muybridge              */ Theme::Cinema,
    /* 183 NaCl Lattice           */ Theme::Chemistry,
    /* 184 Napoleon               */ Theme::History,
    /* 185 Neo                    */ Theme::Cinema,
    /* 186 Neutron Star           */ Theme::Physics,
    /* 187 Newspaper              */ Theme::TerminalFx,
    /* 188 Newton                 */ Theme::History,
    /* 189 Newton Cradle          */ Theme::Physics,
    /* 190 Nosferatu              */ Theme::Cinema,
    /* 191 Olympic Torch          */ Theme::History,
    /* 192 Olympus Mons           */ Theme::Physics,
    /* 193 Opera Curtain          */ Theme::Music,
    /* 194 Oscar Statue           */ Theme::Cinema,
    /* 195 Out of Africa          */ Theme::Biology,
    /* 196 Ozone Hole             */ Theme::Weather,
    /* 197 Pac-Man                */ Theme::Cartoon,
    /* 198 Pac-Man Duel           */ Theme::Cartoon,
    /* 199 Pandora                */ Theme::Myth,
    /* 200 Pandora Foot           */ Theme::Myth,
    /* 201 Pangaea                */ Theme::Physics,
    /* 202 Parker Probe           */ Theme::Physics,
    /* 203 Parthenon              */ Theme::History,
    /* 204 Peacock                */ Theme::Biology,
    /* 205 Pegasus                */ Theme::Myth,
    /* 206 Pendulum Waves         */ Theme::Physics,
    /* 207 Peppered Moth          */ Theme::Biology,
    /* 208 Periodic Table         */ Theme::Chemistry,
    /* 209 pH Strip               */ Theme::Chemistry,
    /* 210 Phase Transition       */ Theme::Chemistry,
    /* 211 Phoenix                */ Theme::Myth,
    /* 212 Pi                     */ Theme::Physics,
    /* 213 Piano Keys             */ Theme::Music,
    /* 214 Pillars of Creation    */ Theme::Physics,
    /* 215 Pink Panther           */ Theme::Cartoon,
    /* 216 Pleasantville          */ Theme::Cinema,
    /* 217 Polar Vortex           */ Theme::Weather,
    /* 218 Pompeii                */ Theme::History,
    /* 219 Pride Rock             */ Theme::Cinema,
    /* 220 Psycho                 */ Theme::Cinema,
    /* 221 Pulp Briefcase         */ Theme::Cinema,
    /* 222 Pulp Fiction           */ Theme::Cinema,
    /* 223 Pulsar                 */ Theme::Physics,
    /* 224 Punctuated             */ Theme::Biology,
    /* 225 Pyramids               */ Theme::History,
    /* 226 Pythagoras             */ Theme::Physics,
    /* 227 Python Cut             */ Theme::Cartoon,
    /* 228 Python Wars            */ Theme::Cartoon,
    /* 229 Quetzalcoatl           */ Theme::Myth,
    /* 230 Ragnarok               */ Theme::Myth,
    /* 231 Red Balloon            */ Theme::Cinema,
    /* 232 Ring of Fire           */ Theme::Physics,
    /* 233 Riverdance             */ Theme::Music,
    /* 234 Rocky                  */ Theme::Cinema,
    /* 235 Rosebud                */ Theme::Cinema,
    /* 236 Rubik                  */ Theme::Physics,
    /* 237 Russian Dance          */ Theme::Music,
    /* 238 Sagittarius A          */ Theme::Physics,
    /* 239 Saharan Dust           */ Theme::Weather,
    /* 240 Saturn                 */ Theme::Physics,
    /* 241 Saturn Rings           */ Theme::Physics,
    /* 242 Schrodinger            */ Theme::Physics,
    /* 243 Sea Ice                */ Theme::Weather,
    /* 244 Selection              */ Theme::Biology,
    /* 245 Shawshank              */ Theme::Cinema,
    /* 246 Sheet Music            */ Theme::Music,
    /* 247 Shining                */ Theme::Cinema,
    /* 248 Shiver                 */ Theme::TerminalFx,
    /* 249 Sierpinski             */ Theme::Physics,
    /* 250 Silly Walk             */ Theme::Cartoon,
    /* 251 Singin'                */ Theme::Music,
    /* 252 Sistine                */ Theme::Art,
    /* 253 Skeleton Wave          */ Theme::TerminalFx,
    /* 254 Snow Tree              */ Theme::Weather,
    /* 255 Snowfall               */ Theme::Weather,
    /* 256 Soap Bubble            */ Theme::Chemistry,
    /* 257 Solar Eclipse          */ Theme::Physics,
    /* 258 Solar Flare            */ Theme::Physics,
    /* 259 Solar System           */ Theme::Physics,
    /* 260 Sound of Music         */ Theme::Music,
    /* 261 Spaghettify            */ Theme::Physics,
    /* 262 Spider                 */ Theme::TerminalFx,
    /* 263 Spiral                 */ Theme::TerminalFx,
    /* 264 Spirited Train         */ Theme::Cinema,
    /* 265 Sputnik                */ Theme::History,
    /* 266 Standing Wave          */ Theme::Physics,
    /* 267 Standoff               */ Theme::Cinema,
    /* 268 Star Gate              */ Theme::Cinema,
    /* 269 Star Wars              */ Theme::Cinema,
    /* 270 Stephenson             */ Theme::History,
    /* 271 Stingray               */ Theme::Weather,
    /* 272 Stonehenge             */ Theme::History,
    /* 273 Stradivarius           */ Theme::Music,
    /* 274 Strangelove            */ Theme::Cinema,
    /* 275 Submarine              */ Theme::Music,
    /* 276 Sun Dogs               */ Theme::Physics,
    /* 277 Sunspot Cycle          */ Theme::Physics,
    /* 278 Supercell              */ Theme::Weather,
    /* 279 Supernova              */ Theme::Physics,
    /* 280 Tatooine               */ Theme::Cinema,
    /* 281 Teardrop               */ Theme::TerminalFx,
    /* 282 Tears in Rain          */ Theme::Cinema,
    /* 283 Test Card              */ Theme::TerminalFx,
    /* 284 Thanos Snap            */ Theme::Cinema,
    /* 285 That's All Folks       */ Theme::Cartoon,
    /* 286 The Birds              */ Theme::Cinema,
    /* 287 The End                */ Theme::TerminalFx,
    /* 288 Thelma & Louise        */ Theme::Cinema,
    /* 289 Theremin               */ Theme::Music,
    /* 290 Thriller               */ Theme::Music,
    /* 291 Thunderstorm           */ Theme::Weather,
    /* 292 Titanic                */ Theme::Cinema,
    /* 293 Top Hat                */ Theme::Cartoon,
    /* 294 Tornado                */ Theme::Weather,
    /* 295 Tornado Duel           */ Theme::Weather,
    /* 296 Totoro                 */ Theme::Cinema,
    /* 297 Tracks                 */ Theme::Cinema,
    /* 298 Train                  */ Theme::Cinema,
    /* 299 Tree of Life           */ Theme::Biology,
    /* 300 Trojan Foot            */ Theme::Myth,
    /* 301 Trojan Horse           */ Theme::History,
    /* 302 Tron                   */ Theme::Cinema,
    /* 303 Trophy                 */ Theme::Cartoon,
    /* 304 Truman                 */ Theme::Cinema,
    /* 305 Tsunami                */ Theme::Weather,
    /* 306 Tunnel                 */ Theme::TerminalFx,
    /* 307 UFO                    */ Theme::Cinema,
    /* 308 Up                     */ Theme::Cinema,
    /* 309 Utopia                 */ Theme::TerminalFx,
    /* 310 Valkyrie Ride          */ Theme::Music,
    /* 311 Vertigo                */ Theme::Cinema,
    /* 312 Viking Longboat        */ Theme::History,
    /* 313 Vinyl Spin             */ Theme::Music,
    /* 314 Voyager                */ Theme::Physics,
    /* 315 Walker Cell            */ Theme::Weather,
    /* 316 Warhol Banana          */ Theme::Art,
    /* 317 Warp                   */ Theme::TerminalFx,
    /* 318 Wildfire Smoke         */ Theme::Weather,
    /* 319 William Tell           */ Theme::Myth,
    /* 320 Wizard of Oz           */ Theme::Cinema,
    /* 321 Word Reveal            */ Theme::TerminalFx,
    /* 322 Wormhole               */ Theme::Physics,
    /* 323 Wright Flyer           */ Theme::History,
    /* 324 Yggdrasil              */ Theme::Myth,
    /* 325 YMCA                   */ Theme::Music,
    /* 326 Yorick                 */ Theme::Cinema,
};

static_assert(sizeof(kThemes) / sizeof(kThemes[0]) == kEffectCount,
              "kThemes must be parallel to kNames");

}  // namespace ee_detail
}  // namespace Qdless

#include "QdlessExitEffectEffects.h"
