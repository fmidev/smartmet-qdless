#include "QdlessGraphics.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <poll.h>
#include <string>
#include <termios.h>
#include <unistd.h>
#include <vector>

namespace Qdless
{
namespace
{
// Read available bytes from stdin until either the deadline passes or `stop`
// returns true on the accumulated buffer (used to short-circuit when both
// expected replies have arrived). The buffer is appended to in place.
template <typename StopFn>
void readUntil(std::string& out, int budgetMs, StopFn&& stop)
{
  const auto start = std::chrono::steady_clock::now();
  while (true)
  {
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
    if (elapsed >= budgetMs)
      return;
    pollfd pfd{STDIN_FILENO, POLLIN, 0};
    int rc = ::poll(&pfd, 1, static_cast<int>(budgetMs - elapsed));
    if (rc <= 0)
      return;
    if ((pfd.revents & POLLIN) == 0)
      return;
    std::array<char, 256> buf{};
    const ssize_t n = ::read(STDIN_FILENO, buf.data(), buf.size());
    if (n <= 0)
      return;
    out.append(buf.data(), static_cast<std::size_t>(n));
    if (stop(out))
      return;
  }
}

// Parse a DA1 reply (`\e[?<params>c`). Returns true if found and any param
// equals 4 (= sixel). Tolerant of multiple control sequences in the same
// buffer (we may see DA1 + DECRPM + \e[16t reply interleaved).
bool parseSixelFromDA1(const std::string& buf)
{
  std::size_t i = 0;
  while (i + 2 < buf.size())
  {
    if (buf[i] == '\x1b' && buf[i + 1] == '[' && buf[i + 2] == '?')
    {
      const std::size_t start = i + 3;
      std::size_t end = start;
      while (end < buf.size() && buf[end] != 'c' && buf[end] != '\x1b')
        ++end;
      if (end >= buf.size() || buf[end] != 'c')
        return false;
      // Split params on ';' and look for "4".
      std::size_t j = start;
      while (j < end)
      {
        std::size_t k = j;
        while (k < end && buf[k] != ';')
          ++k;
        if (k - j == 1 && buf[j] == '4')
          return true;
        j = k + 1;
      }
      return false;
    }
    ++i;
  }
  return false;
}

// Any `\e_G<keys>;...\e\` APC sequence in the buffer means the terminal
// recognised the Kitty graphics-query introducer. The status string
// (`OK`, `ENOENT:...`, etc.) is irrelevant for capability detection — what
// matters is that the terminal echoed a properly-framed response back.
bool parseKittyReply(const std::string& buf)
{
  std::size_t i = 0;
  while (i + 1 < buf.size())
  {
    if (buf[i] == '\x1b' && buf[i + 1] == '_' && i + 2 < buf.size() && buf[i + 2] == 'G')
    {
      // Find the ST (\e\) terminator that closes the APC.
      for (std::size_t j = i + 3; j + 1 < buf.size(); ++j)
        if (buf[j] == '\x1b' && buf[j + 1] == '\\')
          return true;
      return false;  // unterminated -> ignore
    }
    ++i;
  }
  return false;
}

// Parse a `\e[6;<h>;<w>t` reply (cell pixel size). Writes h/w on success;
// returns false (and leaves the outputs untouched) if no such reply is in
// the buffer or it's malformed.
bool parseCellSize(const std::string& buf, int& cellH, int& cellW)
{
  std::size_t i = 0;
  while (i + 3 < buf.size())
  {
    if (buf[i] == '\x1b' && buf[i + 1] == '[' && buf[i + 2] == '6' && buf[i + 3] == ';')
    {
      const std::size_t start = i + 4;
      std::size_t end = start;
      while (end < buf.size() && buf[end] != 't' && buf[end] != '\x1b')
        ++end;
      if (end >= buf.size() || buf[end] != 't')
        return false;
      // <h>;<w>
      std::size_t semi = start;
      while (semi < end && buf[semi] != ';')
        ++semi;
      if (semi >= end)
        return false;
      char* unused = nullptr;
      const long h = std::strtol(buf.c_str() + start, &unused, 10);
      const long w = std::strtol(buf.c_str() + semi + 1, &unused, 10);
      if (h <= 0 || w <= 0 || h > 4096 || w > 4096)
        return false;
      cellH = static_cast<int>(h);
      cellW = static_cast<int>(w);
      return true;
    }
    ++i;
  }
  return false;
}
}  // namespace

TerminalCapabilities probeTerminalCapabilities()
{
  TerminalCapabilities caps;  // defaults: no sixel, 8x16
  if (::isatty(STDIN_FILENO) == 0 || ::isatty(STDOUT_FILENO) == 0)
    return caps;

  termios saved{};
  if (::tcgetattr(STDIN_FILENO, &saved) != 0)
    return caps;
  termios raw = saved;
  raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 0;
  if (::tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0)
    return caps;

  // Drain anything stale before asking — otherwise a leftover paste or a
  // mouse-report might be misparsed as a DA reply.
  {
    pollfd pfd{STDIN_FILENO, POLLIN, 0};
    while (::poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN) != 0)
    {
      std::array<char, 256> drop{};
      if (::read(STDIN_FILENO, drop.data(), drop.size()) <= 0)
        break;
    }
  }

  // Send three queries; the terminal answers them in roughly the same order.
  //   ESC [ c       = DA1             -> reply ends in 'c'
  //   ESC [ 16 t    = cell pixel size -> reply ends in 't'
  //   ESC _ G ... ST = Kitty graphics query -> reply is another APC (ESC _ G ... ST)
  // The Kitty query is a 1x1 transmit-and-query of four base64'd bytes; non-
  // Kitty terminals swallow the APC (per ECMA-48) and don't reply, which our
  // 250 ms budget tolerates.
  static const char query[] = "\x1b[c\x1b[16t\x1b_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\x1b\\";
  ::write(STDOUT_FILENO, query, sizeof(query) - 1);

  std::string buf;
  buf.reserve(256);
  readUntil(buf,
            250,
            [&](const std::string& s)
            {
              // Stop early once we have all three terminators. Some terminals
              // skip \e[16t and/or the Kitty reply; the budget covers that.
              const bool haveDA = s.find('c') != std::string::npos;
              const bool haveCell = s.find('t') != std::string::npos;
              // APC ST is "\e\\" (2 bytes ESC + '\').
              bool haveKitty = false;
              for (std::size_t i = 0; i + 1 < s.size(); ++i)
                if (s[i] == '\x1b' && s[i + 1] == '\\')
                {
                  haveKitty = true;
                  break;
                }
              return haveDA && haveCell && haveKitty;
            });

  ::tcsetattr(STDIN_FILENO, TCSANOW, &saved);

  caps.sixel = parseSixelFromDA1(buf);
  caps.kitty = parseKittyReply(buf);
  int h = 0;
  int w = 0;
  if (parseCellSize(buf, h, w))
  {
    caps.cellPxW = w;
    caps.cellPxH = h;
  }
  return caps;
}

// ---------------------------------------------------------------------------
// Sixel encoder.
// ---------------------------------------------------------------------------
namespace
{
struct Box
{
  // Half-open index range into `idx` (the working order of pixel indices).
  std::size_t lo = 0;
  std::size_t hi = 0;
  // RGB extent of the pixels currently in this box.
  int rMin = 0, rMax = 0, gMin = 0, gMax = 0, bMin = 0, bMax = 0;
  // Mean RGB (palette entry).
  int rMean = 0, gMean = 0, bMean = 0;
};

void recomputeBox(Box& box,
                  const std::vector<std::uint32_t>& idx,
                  const std::vector<std::uint32_t>& opaquePixels)
{
  box.rMin = box.gMin = box.bMin = 255;
  box.rMax = box.gMax = box.bMax = 0;
  long sR = 0, sG = 0, sB = 0;
  const auto n = box.hi - box.lo;
  if (n == 0)
    return;
  for (std::size_t i = box.lo; i < box.hi; ++i)
  {
    const std::uint32_t packed = opaquePixels[idx[i]];
    const int r = static_cast<int>((packed >> 16) & 0xFF);
    const int g = static_cast<int>((packed >> 8) & 0xFF);
    const int b = static_cast<int>(packed & 0xFF);
    box.rMin = std::min(box.rMin, r);
    box.rMax = std::max(box.rMax, r);
    box.gMin = std::min(box.gMin, g);
    box.gMax = std::max(box.gMax, g);
    box.bMin = std::min(box.bMin, b);
    box.bMax = std::max(box.bMax, b);
    sR += r;
    sG += g;
    sB += b;
  }
  box.rMean = static_cast<int>(sR / static_cast<long>(n));
  box.gMean = static_cast<int>(sG / static_cast<long>(n));
  box.bMean = static_cast<int>(sB / static_cast<long>(n));
}

// Median-cut quantisation. Returns the palette (≤target entries) for the
// supplied opaque pixels packed as 0x00RRGGBB.
std::vector<Box> medianCut(const std::vector<std::uint32_t>& opaquePixels, std::size_t target)
{
  std::vector<std::uint32_t> idx(opaquePixels.size());
  for (std::uint32_t i = 0; i < opaquePixels.size(); ++i)
    idx[i] = i;

  std::vector<Box> boxes;
  boxes.reserve(target);
  boxes.push_back(Box{0, opaquePixels.size()});
  recomputeBox(boxes.back(), idx, opaquePixels);

  while (boxes.size() < target)
  {
    // Pick the box with the biggest range on its widest axis.
    int bestBox = -1;
    int bestSpan = -1;
    int bestAxis = 0;
    for (std::size_t i = 0; i < boxes.size(); ++i)
    {
      const Box& b = boxes[i];
      if (b.hi - b.lo < 2)
        continue;  // can't split a singleton
      const int dR = b.rMax - b.rMin;
      const int dG = b.gMax - b.gMin;
      const int dB = b.bMax - b.bMin;
      int span = dR;
      int axis = 0;
      if (dG > span)
      {
        span = dG;
        axis = 1;
      }
      if (dB > span)
      {
        span = dB;
        axis = 2;
      }
      if (span > bestSpan)
      {
        bestSpan = span;
        bestBox = static_cast<int>(i);
        bestAxis = axis;
      }
    }
    if (bestBox < 0 || bestSpan <= 0)
      break;  // every box is a single point — no benefit in splitting further

    Box& box = boxes[bestBox];
    auto key = [&](std::uint32_t pi)
    {
      const std::uint32_t p = opaquePixels[pi];
      switch (bestAxis)
      {
        case 0:
          return static_cast<int>((p >> 16) & 0xFF);
        case 1:
          return static_cast<int>((p >> 8) & 0xFF);
        default:
          return static_cast<int>(p & 0xFF);
      }
    };
    std::sort(idx.begin() + box.lo,
              idx.begin() + box.hi,
              [&](std::uint32_t a, std::uint32_t b) { return key(a) < key(b); });
    const std::size_t mid = box.lo + (box.hi - box.lo) / 2;
    Box left{box.lo, mid};
    Box right{mid, box.hi};
    boxes[bestBox] = left;
    boxes.push_back(right);
    recomputeBox(boxes[bestBox], idx, opaquePixels);
    recomputeBox(boxes.back(), idx, opaquePixels);
  }
  return boxes;
}

inline int dist2(int r1, int g1, int b1, int r2, int g2, int b2)
{
  const int dr = r1 - r2, dg = g1 - g2, db = b1 - b2;
  return dr * dr + dg * dg + db * db;
}

// Append `n` copies of `byte` to `out`, using sixel's `!<count><byte>` run-
// length compression when it pays off. byte is one of the printable range
// '?'(0x3F) .. '~'(0x7E).
void appendRun(std::string& out, char byte, int n)
{
  if (n <= 0)
    return;
  if (n >= 4)
  {
    char buf[16];
    const int len = std::snprintf(buf, sizeof(buf), "!%d", n);
    out.append(buf, static_cast<std::size_t>(len));
    out.push_back(byte);
  }
  else
  {
    for (int i = 0; i < n; ++i)
      out.push_back(byte);
  }
}
}  // namespace

void renderSixel(std::ostream& os,
                 const std::vector<Rgb>& pixels,
                 int subWidth,
                 int subHeight,
                 int originRow,
                 int originCol)
{
  if (subWidth <= 0 || subHeight <= 0 ||
      pixels.size() < static_cast<std::size_t>(subWidth) * subHeight)
    return;

  // Collect opaque pixels for quantisation. Transparent pixels reuse
  // register 0 (which a "background=hold" introducer leaves at the
  // terminal's current background — but we paint them with an empty
  // mask, so they actually stay untouched).
  std::vector<std::uint32_t> opaque;
  opaque.reserve(static_cast<std::size_t>(subWidth) * subHeight);
  for (const auto& p : pixels)
    if (!p.transparent)
      opaque.push_back((static_cast<std::uint32_t>(p.r) << 16) |
                       (static_cast<std::uint32_t>(p.g) << 8) | p.b);

  // No opaque pixels — emit nothing (the cells stay at their current bg).
  if (opaque.empty())
    return;

  // Cap palette at 254 entries so we leave room for register 0 (transparent
  // sentinel — never emitted) and one unused slot.
  constexpr std::size_t kTarget = 254;
  std::vector<Box> boxes = medianCut(opaque, kTarget);

  const std::size_t nPal = boxes.size();
  std::vector<std::array<int, 3>> palette(nPal);
  for (std::size_t i = 0; i < nPal; ++i)
    palette[i] = {boxes[i].rMean, boxes[i].gMean, boxes[i].bMean};

  // Map every pixel to its palette index (or 0xFF for transparent).
  // Brute-force nearest neighbour: O(N x K). At ~250 kpx and 254 colours
  // it's ~60M ops — fine for a manual-toggle refresh.
  constexpr std::uint8_t kTransparent = 0xFF;
  std::vector<std::uint8_t> map(static_cast<std::size_t>(subWidth) * subHeight, kTransparent);
  for (std::size_t i = 0; i < map.size(); ++i)
  {
    const Rgb& p = pixels[i];
    if (p.transparent)
      continue;
    int best = 0;
    int bestD = std::numeric_limits<int>::max();
    for (std::size_t k = 0; k < nPal; ++k)
    {
      const int d = dist2(p.r, p.g, p.b, palette[k][0], palette[k][1], palette[k][2]);
      if (d < bestD)
      {
        bestD = d;
        best = static_cast<int>(k);
      }
    }
    map[i] = static_cast<std::uint8_t>(best);
  }

  // Build the sixel payload.
  std::string out;
  out.reserve(static_cast<std::size_t>(subWidth) * subHeight / 2);

  // Position the cursor at the desired top-left cell.
  char pos[32];
  const int len = std::snprintf(pos, sizeof(pos), "\x1b[%d;%dH", originRow + 1, originCol + 1);
  out.append(pos, static_cast<std::size_t>(len));

  // DCS introducer: aspect 0 (1:1), background hold (1), grid 0.
  out.append("\x1bP0;1;0q");

  // Define raster attributes: 1:1 pixel aspect, full image size. Helps
  // terminals reserve the right area up front.
  char raster[64];
  const int rlen =
      std::snprintf(raster, sizeof(raster), "\"1;1;%d;%d", subWidth, subHeight);
  out.append(raster, static_cast<std::size_t>(rlen));

  // Emit palette definitions. Sixel colour scale is 0..100, NOT 0..255.
  for (std::size_t k = 0; k < nPal; ++k)
  {
    char def[48];
    const int dlen = std::snprintf(def,
                                   sizeof(def),
                                   "#%zu;2;%d;%d;%d",
                                   k,
                                   (palette[k][0] * 100 + 127) / 255,
                                   (palette[k][1] * 100 + 127) / 255,
                                   (palette[k][2] * 100 + 127) / 255);
    out.append(def, static_cast<std::size_t>(dlen));
  }

  // Walk in 6-row bands. Within each band, emit each used colour as one
  // pass over the columns, with run-length compression. `$` returns to the
  // start of the band between colours; `-` advances to the next band.
  std::vector<std::uint8_t> bandColors;
  bandColors.reserve(nPal);
  std::vector<char> sixelByte(static_cast<std::size_t>(subWidth));
  for (int y0 = 0; y0 < subHeight; y0 += 6)
  {
    const int rows = std::min(6, subHeight - y0);
    // Which palette entries actually appear in this band? Avoids paying for
    // 254 colour passes when the band only uses a handful.
    bandColors.clear();
    std::array<std::uint8_t, 256> seen{};
    seen.fill(0);
    for (int dy = 0; dy < rows; ++dy)
    {
      const std::uint8_t* row = map.data() + static_cast<std::size_t>(y0 + dy) * subWidth;
      for (int x = 0; x < subWidth; ++x)
      {
        const std::uint8_t c = row[x];
        if (c == kTransparent || seen[c])
          continue;
        seen[c] = 1;
        bandColors.push_back(c);
      }
    }

    for (std::size_t ci = 0; ci < bandColors.size(); ++ci)
    {
      const std::uint8_t c = bandColors[ci];
      // Build the per-column 6-bit mask.
      for (int x = 0; x < subWidth; ++x)
      {
        std::uint8_t mask = 0;
        for (int dy = 0; dy < rows; ++dy)
        {
          if (map[static_cast<std::size_t>(y0 + dy) * subWidth + x] == c)
            mask = static_cast<std::uint8_t>(mask | (1U << dy));
        }
        sixelByte[static_cast<std::size_t>(x)] = static_cast<char>('?' + mask);
      }
      // `#<idx>` selects current colour register.
      char colorSel[16];
      const int cl = std::snprintf(colorSel, sizeof(colorSel), "#%u", c);
      out.append(colorSel, static_cast<std::size_t>(cl));
      // Run-length compressed column sweep.
      int runStart = 0;
      while (runStart < subWidth)
      {
        int runEnd = runStart + 1;
        while (runEnd < subWidth && sixelByte[runEnd] == sixelByte[runStart])
          ++runEnd;
        appendRun(out, sixelByte[runStart], runEnd - runStart);
        runStart = runEnd;
      }
      // Return to start of band for the next colour (skip after last).
      if (ci + 1 < bandColors.size())
        out.push_back('$');
    }
    // Advance to next band (skip after last band).
    if (y0 + 6 < subHeight)
      out.push_back('-');
  }

  // String terminator.
  out.append("\x1b\\");

  os << out;
}

// ---------------------------------------------------------------------------
// Kitty graphics protocol encoder.
// ---------------------------------------------------------------------------
namespace
{
// Standard base64 alphabet, no line breaks. Kitty parsers accept the URL-
// safe alphabet too but the classic one is universally understood.
void base64Append(std::string& out, const unsigned char* data, std::size_t n)
{
  static constexpr char kTbl[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  out.reserve(out.size() + ((n + 2) / 3) * 4);
  std::size_t i = 0;
  while (i + 3 <= n)
  {
    const unsigned v = (static_cast<unsigned>(data[i]) << 16) |
                       (static_cast<unsigned>(data[i + 1]) << 8) |
                       static_cast<unsigned>(data[i + 2]);
    out.push_back(kTbl[(v >> 18) & 0x3F]);
    out.push_back(kTbl[(v >> 12) & 0x3F]);
    out.push_back(kTbl[(v >> 6) & 0x3F]);
    out.push_back(kTbl[v & 0x3F]);
    i += 3;
  }
  if (i < n)
  {
    unsigned v = static_cast<unsigned>(data[i]) << 16;
    if (i + 1 < n)
      v |= static_cast<unsigned>(data[i + 1]) << 8;
    out.push_back(kTbl[(v >> 18) & 0x3F]);
    out.push_back(kTbl[(v >> 12) & 0x3F]);
    out.push_back((i + 1 < n) ? kTbl[(v >> 6) & 0x3F] : '=');
    out.push_back('=');
  }
}
}  // namespace

void renderKitty(std::ostream& os,
                 const std::vector<Rgb>& pixels,
                 int subWidth,
                 int subHeight,
                 int originRow,
                 int originCol)
{
  if (subWidth <= 0 || subHeight <= 0 ||
      pixels.size() < static_cast<std::size_t>(subWidth) * subHeight)
    return;

  // Pack RGBA. Transparent input pixels carry alpha=0 so the terminal
  // blends them against whatever's behind (cell background / underlying
  // image). f=32 (RGBA) is one byte fatter per pixel than f=24 but worth
  // it for the transparent-data sentinel.
  const std::size_t nPixels = static_cast<std::size_t>(subWidth) * subHeight;
  std::vector<unsigned char> raw(nPixels * 4);
  for (std::size_t i = 0; i < nPixels; ++i)
  {
    const Rgb& p = pixels[i];
    raw[i * 4 + 0] = p.r;
    raw[i * 4 + 1] = p.g;
    raw[i * 4 + 2] = p.b;
    raw[i * 4 + 3] = static_cast<unsigned char>(p.transparent ? 0 : 255);
  }

  // Base64 the whole image once; chunking happens over the base64 stream.
  std::string b64;
  base64Append(b64, raw.data(), raw.size());

  std::string out;
  out.reserve(b64.size() + (b64.size() / 4096 + 1) * 32);

  // Position cursor at top-left of the target cell rectangle.
  char pos[32];
  const int plen = std::snprintf(pos, sizeof(pos), "\x1b[%d;%dH", originRow + 1, originCol + 1);
  out.append(pos, static_cast<std::size_t>(plen));

  // Chunk the base64 payload. Kitty requires each chunk <= 4096 bytes; the
  // first chunk carries the metadata (format, dimensions, action), middle
  // chunks just say m=1, the last chunk says m=0.
  constexpr std::size_t kMaxChunk = 4096;
  std::size_t off = 0;
  bool first = true;
  while (off < b64.size())
  {
    const std::size_t take = std::min(kMaxChunk, b64.size() - off);
    const bool isLast = (off + take == b64.size());
    if (first)
    {
      char hdr[80];
      const int hlen = std::snprintf(hdr,
                                     sizeof(hdr),
                                     "\x1b_Gf=32,s=%d,v=%d,a=T,q=2,m=%d;",
                                     subWidth,
                                     subHeight,
                                     isLast ? 0 : 1);
      out.append(hdr, static_cast<std::size_t>(hlen));
      first = false;
    }
    else
    {
      out.append(isLast ? "\x1b_Gm=0;" : "\x1b_Gm=1;");
    }
    out.append(b64, off, take);
    out.append("\x1b\\", 2);
    off += take;
  }

  os << out;
}
}  // namespace Qdless
