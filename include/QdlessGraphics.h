#pragma once

#include "QdlessPalette.h"

#include <ostream>
#include <vector>

namespace Qdless
{
// What the host terminal supports + how big a cell is in physical pixels.
// Populated once at startup via probeTerminalCapabilities(); cached on the
// App so the graphics toggle and the renderers can read it without
// re-querying. Both Kitty and Sixel are queried in the same probe so the
// brief raw-mode read happens once.
struct TerminalCapabilities
{
  bool kitty = false;  // Kitty graphics protocol (queried via \e_G...a=q)
  bool sixel = false;  // sixel (DA1 advertised feature 4)
  int cellPxW = 8;     // cell pixel width  (\e[16t reply); fallback 8
  int cellPxH = 16;    // cell pixel height (\e[16t reply); fallback 16
};

// Send DA1 (\e[c), \e[16t (cell pixel size), and the Kitty graphics-query
// APC to the terminal on stdout, read replies from stdin with a short
// timeout, and parse out the feature flags + cell pixel dimensions. Briefly
// puts stdin in raw mode for the read and restores it afterwards. Returns
// the defaults (no protocol support, 8x16 cell) when stdin isn't a tty or
// no reply arrives in time.
TerminalCapabilities probeTerminalCapabilities();

// Encode `pixels` (row-major, subWidth x subHeight) as a sixel blob and
// write it to `os` positioned at originRow/originCol (1-based terminal
// cell coordinates). Quantises to <=254 colours per frame via median-cut.
// Transparent input pixels are left at the terminal's current background
// (the introducer uses background-hold).
void renderSixel(std::ostream& os,
                 const std::vector<Rgb>& pixels,
                 int subWidth,
                 int subHeight,
                 int originRow,
                 int originCol);

// Encode `pixels` as a Kitty graphics protocol RGBA transmission and write
// it to `os` positioned at originRow/originCol. No quantisation — every
// pixel keeps its full 8-bit per channel value, and transparent pixels
// carry alpha=0 so the terminal blends them against whatever's behind.
// Payload is base64-encoded and chunked into 4096-byte transmissions
// (m=1 / m=0 framing).
void renderKitty(std::ostream& os,
                 const std::vector<Rgb>& pixels,
                 int subWidth,
                 int subHeight,
                 int originRow,
                 int originCol);
}  // namespace Qdless
