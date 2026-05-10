#include "QdlessUI.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <locale.h>
#include <sstream>
#include <string_view>

namespace Qdless
{
namespace
{
constexpr short kPairBase = 1;
constexpr short kPairHot = 2;
constexpr short kPairSel = 3;
constexpr short kPairBox = 4;
constexpr short kPairPopup = 5;

// Auto-assigned hotkey for menu item index i: 1..9 then a..z (skipping q
// because q is "quit" elsewhere). Returns 0 if i is beyond the supported
// range.
char menuHotkey(int i)
{
  if (i < 9) return static_cast<char>('1' + i);
  static const char letters[] = "abcdefghijklmnoprstuvwxyz";  // no 'q'
  int li = i - 9;
  if (li < static_cast<int>(sizeof(letters) - 1)) return letters[li];
  return 0;
}

// Raw-ANSI escape strings for popup rendering. Using these instead of
// ncurses windows for popups: ncurses' diff-vs-curscr optimization breaks
// when the area underneath is raw-ANSI map output that ncurses doesn't
// track, leaving see-through gaps and stale content on repeated opens.
constexpr std::string_view kEscReset = "\x1b[0m";
constexpr std::string_view kEscBgBlack = "\x1b[48;5;0m";
constexpr std::string_view kEscBgWhite = "\x1b[48;5;15m";
constexpr std::string_view kEscFgWhite = "\x1b[38;5;15m";
constexpr std::string_view kEscFgBlack = "\x1b[38;5;0m";
constexpr std::string_view kEscFgRed = "\x1b[38;5;9m";
constexpr std::string_view kEscFgCyan = "\x1b[38;5;14m";
constexpr std::string_view kEscBold = "\x1b[1m";

// Count display columns in a UTF-8 string (number of non-continuation bytes).
// Approximate: assumes every codepoint occupies one cell, which is fine for
// Latin + arrows + box-drawing. Wide CJK / emoji would need wcwidth.
int utf8Width(std::string_view s)
{
  int w = 0;
  for (unsigned char c : s)
    if ((c & 0xC0) != 0x80) ++w;
  return w;
}

// Stream a fixed-width pad of spaces.
void pad(std::ostringstream& os, int n)
{
  if (n > 0) os << std::string(n, ' ');
}

// Position the cursor at popup-relative (row, col), 0-based.
void putAt(std::ostringstream& os, int absRow, int absCol)
{
  os << "\x1b[" << (absRow + 1) << ';' << (absCol + 1) << 'H';
}
}  // namespace

UI::UI()
{
  // Required for wide-character (UTF-8) glyphs.
  setlocale(LC_ALL, "");

  initscr();
  cbreak();
  noecho();
  keypad(stdscr, TRUE);
  curs_set(0);
  mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, nullptr);
  // Click resolution window for single vs double click detection (ms).
  mouseinterval(200);
  // Ncurses ships a SIGWINCH handler that posts KEY_RESIZE through getch().

  if (has_colors() != 0)
  {
    start_color();
    use_default_colors();
    init_pair(kPairBase, COLOR_WHITE, -1);
    init_pair(kPairHot, COLOR_RED, -1);
    init_pair(kPairSel, COLOR_BLACK, COLOR_WHITE);
    init_pair(kPairBox, COLOR_CYAN, COLOR_BLACK);
    init_pair(kPairPopup, COLOR_WHITE, COLOR_BLACK);
  }
  itsBaseAttr = COLOR_PAIR(kPairBase);
  itsHotAttr = COLOR_PAIR(kPairHot) | A_BOLD;
  itsSelAttr = COLOR_PAIR(kPairSel) | A_BOLD;
  itsBoxAttr = COLOR_PAIR(kPairBox);
  itsPopupAttr = COLOR_PAIR(kPairPopup);

  recomputeLayout();
}

UI::~UI()
{
  if (itsStatusWin != nullptr) delwin(itsStatusWin);
  if (itsTimeWin != nullptr) delwin(itsTimeWin);
  endwin();
}

void UI::recomputeLayout()
{
  int rows = LINES;
  int cols = COLS;
  // Reserve 1 row for status, 2 for timeline.
  itsLayout.status = {rows - 1, 0, 1, cols};
  itsLayout.time = {rows - 3, 0, 2, cols};
  itsLayout.map = {0, 0, std::max(0, rows - 3), cols};

  if (itsStatusWin != nullptr) delwin(itsStatusWin);
  if (itsTimeWin != nullptr) delwin(itsTimeWin);
  itsTimeWin = newwin(itsLayout.time.height, itsLayout.time.width, itsLayout.time.row,
                      itsLayout.time.col);
  itsStatusWin = newwin(itsLayout.status.height, itsLayout.status.width, itsLayout.status.row,
                        itsLayout.status.col);
  // We block on input via wgetch(itsStatusWin); without keypad enabled,
  // arrow keys arrive as raw ESC sequences and the leading ESC byte (27)
  // would be interpreted as "quit".
  keypad(itsStatusWin, TRUE);
}

int UI::waitInput(int timeoutMs)
{
  // Read input from a window we own rather than stdscr, so ncurses doesn't
  // implicitly refresh stdscr (which could re-blank the raw-ANSI map area).
  wtimeout(itsStatusWin, timeoutMs);
  int ch = wgetch(itsStatusWin);
  wtimeout(itsStatusWin, -1);  // restore blocking for nested wgetch callers
  if (ch == KEY_RESIZE) recomputeLayout();
  return ch;
}

void UI::touch()
{
  // Force ncurses to repaint our managed windows on next refresh.
  redrawwin(itsTimeWin);
  redrawwin(itsStatusWin);
  wnoutrefresh(itsTimeWin);
  wnoutrefresh(itsStatusWin);
}

void UI::writeLabel(WINDOW* w, int y, int x, const std::string& label, int hotPos)
{
  int n = static_cast<int>(label.size());
  if (hotPos < 0 || hotPos >= n) hotPos = 0;
  if (hotPos > 0) mvwaddnstr(w, y, x, label.c_str(), hotPos);
  wattron(w, itsHotAttr);
  mvwaddnstr(w, y, x + hotPos, label.c_str() + hotPos, 1);
  wattroff(w, itsHotAttr);
  if (hotPos + 1 < n) mvwaddnstr(w, y, x + hotPos + 1, label.c_str() + hotPos + 1, n - hotPos - 1);
}

void UI::drawTimeline(const std::string& label, int idx, int total)
{
  werase(itsTimeWin);
  mvwaddstr(itsTimeWin, 0, 1, label.c_str());

  // Slider: ├─────●─────┤ across full width.
  int barWidth = itsLayout.time.width - 4;
  if (barWidth < 4) barWidth = 4;
  if (total < 1) total = 1;
  int pos = (total > 1) ? idx * (barWidth - 1) / (total - 1) : 0;

  wattron(itsTimeWin, itsBoxAttr);
  mvwaddstr(itsTimeWin, 1, 1, "├");  // ├
  for (int i = 0; i < barWidth - 2; ++i) mvwaddstr(itsTimeWin, 1, 2 + i, "─");  // ─
  mvwaddstr(itsTimeWin, 1, barWidth - 1, "┤");  // ┤
  wattroff(itsTimeWin, itsBoxAttr);
  wattron(itsTimeWin, itsHotAttr);
  mvwaddstr(itsTimeWin, 1, 1 + pos, "●");  // ●
  wattroff(itsTimeWin, itsHotAttr);

  wnoutrefresh(itsTimeWin);
}

void UI::drawStatusBar(bool imageMode, bool shapeMode)
{
  werase(itsStatusWin);
  // Layout: [Q]uit  [P]aram  [L]evel  Time ←→  Zoom +/-  Pan hjkl  [0]Reset  [?]Help
  int x = 1;
  auto put = [&](const std::string& s, int hotPos) {
    writeLabel(itsStatusWin, 0, x, s, hotPos);
    x += static_cast<int>(s.size()) + 2;
  };
  put("[Q]uit", 1);
  if (!imageMode)
  {
    if (!shapeMode)
    {
      // Param / Level pickers don't apply to a single synthetic
      // shape parameter; cross-section over feature ids has no
      // useful interpretation either.
      put("[P]aram", 1);
      put("[L]evel", 1);
    }
    put("[G]Legend", 1);
    put("[N]Grid", 1);
    put("[W]ind", 1);
    put("[I]Cities", 1);
    put("[C]oast", 1);
    put("[B]orders", 1);
    if (shapeMode)
    {
      put("[O]utlines", 1);
      put("[R]ainbow", 1);
    }
  }
  put("[E]xport", 1);
  if (!imageMode)
  {
    put("[/]Search", 1);
    if (!shapeMode) put("[X]Section", 1);
  }
  put("[?]Help", 1);
  put("[\xe2\x90\xa3]Play", 1);  // [␣]Play
  wnoutrefresh(itsStatusWin);
}

int UI::popupMenu(const std::string& title, const std::vector<std::string>& items,
                  int currentIndex)
{
  if (items.empty()) return -1;

  // Sizing.
  int maxLabel = 0;
  for (const auto& s : items) maxLabel = std::max(maxLabel, utf8Width(s));
  int width = std::max(maxLabel + 8, utf8Width(title) + 6);
  width = std::min(width, COLS - 4);
  int interiorW = width - 2;

  constexpr int kFooterRows = 3;
  int maxRows = std::max(1, LINES - 6 - kFooterRows);
  int innerH = std::min(static_cast<int>(items.size()), maxRows);
  int height = innerH + kFooterRows + 2;
  int top = std::max(0, (LINES - height) / 2);
  int left = std::max(0, (COLS - width) / 2);

  int sel = std::clamp(currentIndex, 0, static_cast<int>(items.size()) - 1);
  int scroll = std::max(0, sel - innerH / 2);
  int result = -1;

  static const std::array<std::string_view, 3> kFooter = {
      "\xe2\x86\x91\xe2\x86\x93 select", "Enter pick", "Esc cancel"};

  while (true)
  {
    if (sel < scroll) scroll = sel;
    if (sel >= scroll + innerH) scroll = sel - innerH + 1;

    std::ostringstream os;

    // Top border with title.
    putAt(os, top, left);
    os << kEscReset << kEscBgBlack << kEscFgCyan << "\xe2\x94\x8c\xe2\x94\x80[" << kEscFgWhite
       << title << kEscFgCyan << "]";
    int titleConsumed = 4 + utf8Width(title);
    for (int i = 0; i < width - titleConsumed - 1; ++i) os << "\xe2\x94\x80";
    os << "\xe2\x94\x90" << kEscReset;

    // Menu rows.
    for (int i = 0; i < innerH; ++i)
    {
      putAt(os, top + 1 + i, left);
      int idx = scroll + i;
      os << kEscReset << kEscBgBlack << kEscFgCyan << "\xe2\x94\x82";
      if (idx >= static_cast<int>(items.size()))
      {
        os << kEscBgBlack << kEscFgWhite;
        pad(os, interiorW);
      }
      else
      {
        const bool isSel = (idx == sel);
        const char hk = menuHotkey(idx);
        if (isSel)
          os << kEscBgWhite << kEscFgBlack << kEscBold;
        else
          os << kEscBgBlack << kEscFgWhite;
        os << ' ';
        int consumed = 1;
        if (hk != 0)
        {
          if (isSel)
          {
            os << '[' << hk << "] ";
          }
          else
          {
            os << '[' << kEscFgRed << kEscBold << hk << kEscReset << kEscBgBlack << kEscFgWhite
               << "] ";
          }
          consumed += 4;
        }
        os << items[idx];
        consumed += utf8Width(items[idx]);
        pad(os, interiorW - consumed);
      }
      os << kEscReset << kEscBgBlack << kEscFgCyan << "\xe2\x94\x82" << kEscReset;
    }

    // Footer: 3 rows.
    for (int fr = 0; fr < kFooterRows; ++fr)
    {
      putAt(os, top + innerH + 1 + fr, left);
      os << kEscReset << kEscBgBlack << kEscFgCyan << "\xe2\x94\x82" << kEscBgBlack << kEscFgWhite
         << ' ' << kFooter[fr];
      pad(os, interiorW - 1 - utf8Width(kFooter[fr]));
      os << kEscBgBlack << kEscFgCyan << "\xe2\x94\x82" << kEscReset;
    }

    // Bottom border.
    putAt(os, top + height - 1, left);
    os << kEscReset << kEscBgBlack << kEscFgCyan << "\xe2\x94\x94";
    for (int i = 0; i < width - 2; ++i) os << "\xe2\x94\x80";
    os << "\xe2\x94\x98" << kEscReset;

    const std::string s = os.str();
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fflush(stdout);

    int ch = wgetch(itsStatusWin);
    if (ch == 27 || ch == 'q')
    {
      result = -1;
      break;
    }
    if (ch == '\n' || ch == KEY_ENTER)
    {
      result = sel;
      break;
    }
    if (ch == KEY_UP || ch == 'k') { sel = std::max(0, sel - 1); continue; }
    if (ch == KEY_DOWN || ch == 'j')
    {
      sel = std::min(static_cast<int>(items.size()) - 1, sel + 1);
      continue;
    }
    if (ch == KEY_HOME) { sel = 0; continue; }
    if (ch == KEY_END) { sel = static_cast<int>(items.size()) - 1; continue; }
    if (ch == KEY_NPAGE)
    {
      sel = std::min(static_cast<int>(items.size()) - 1, sel + innerH);
      continue;
    }
    if (ch == KEY_PPAGE) { sel = std::max(0, sel - innerH); continue; }
    if (ch == KEY_RESIZE)
    {
      recomputeLayout();
      result = -1;
      break;
    }

    bool found = false;
    for (int i = 0; i < static_cast<int>(items.size()); ++i)
      if (menuHotkey(i) == ch)
      {
        result = i;
        found = true;
        break;
      }
    if (found) break;
  }

  // Map underneath needs to be repainted; App's main loop handles this.
  touch();
  return result;
}

int UI::popupSearch(const std::string& title,
                    std::function<std::vector<std::string>(const std::string&)> matcher)
{
  std::string query;
  int sel = 0;
  // Per-session sticky dimensions. Each keystroke recomputes the natural
  // popup size from the current matches, but we never shrink within one
  // invocation — otherwise a smaller popup leaves the previous frame's
  // borders behind because we render directly to stdout (no window
  // manager underneath us). Growth is fine; shrinkage produces artifacts.
  int stickyWidth = 0;
  int stickyHeight = 0;

  while (true)
  {
    const std::vector<std::string> matches = matcher(query);
    if (matches.empty())
      sel = 0;
    else
      sel = std::clamp(sel, 0, static_cast<int>(matches.size()) - 1);

    // Sizing.
    int maxLabel = 0;
    for (const auto& s : matches) maxLabel = std::max(maxLabel, utf8Width(s));
    int width = std::max(maxLabel + 8, std::max(40, utf8Width(title) + 8));
    width = std::min(width, COLS - 4);
    width = std::max(width, stickyWidth);
    stickyWidth = width;
    const int interiorW = width - 2;

    constexpr int kFooterRows = 1;
    const int maxRows = std::max(1, LINES - 8 - kFooterRows);
    const int innerH = std::min(static_cast<int>(matches.size()), maxRows);
    int height = innerH + 4 + kFooterRows;  // top + query + sep + body + footer + bottom
    height = std::max(height, stickyHeight);
    stickyHeight = height;
    const int top = std::max(0, (LINES - height) / 2);
    const int left = std::max(0, (COLS - width) / 2);

    std::ostringstream os;

    // Top border with title.
    putAt(os, top, left);
    os << kEscReset << kEscBgBlack << kEscFgCyan << "\xe2\x94\x8c\xe2\x94\x80[" << kEscFgWhite
       << title << kEscFgCyan << "]";
    int titleConsumed = 4 + utf8Width(title);
    for (int i = 0; i < width - titleConsumed - 1; ++i) os << "\xe2\x94\x80";
    os << "\xe2\x94\x90" << kEscReset;

    // Query row (with cursor caret).
    putAt(os, top + 1, left);
    os << kEscReset << kEscBgBlack << kEscFgCyan << "\xe2\x94\x82" << kEscBgBlack
       << kEscFgWhite << " > " << kEscBold << query << kEscReset << kEscBgBlack << kEscFgWhite
       << "_";
    pad(os, interiorW - 4 - utf8Width(query));
    os << kEscBgBlack << kEscFgCyan << "\xe2\x94\x82" << kEscReset;

    // Separator row.
    putAt(os, top + 2, left);
    os << kEscReset << kEscBgBlack << kEscFgCyan << "\xe2\x94\x9c";
    for (int i = 0; i < width - 2; ++i) os << "\xe2\x94\x80";
    os << "\xe2\x94\xa4" << kEscReset;

    // Body region: every row between separator and footer must be drawn
    // every frame, otherwise rows from a wider previous match list stay
    // on screen when the result set shrinks (especially when the user
    // deletes the entire query and there are no matches).
    const int bodyRows = height - 4 - kFooterRows;  // 1 top + query + sep + footer + bottom
    for (int i = 0; i < bodyRows; ++i)
    {
      putAt(os, top + 3 + i, left);
      os << kEscReset << kEscBgBlack << kEscFgCyan << "\xe2\x94\x82";
      if (i < static_cast<int>(matches.size()))
      {
        const bool isSel = (i == sel);
        if (isSel)
          os << kEscBgWhite << kEscFgBlack << kEscBold;
        else
          os << kEscBgBlack << kEscFgWhite;
        // Hotkey [N] for first 9.
        const char hk = (i < 9) ? static_cast<char>('1' + i) : ' ';
        os << ' ';
        int consumed = 1;
        if (hk != ' ')
        {
          if (isSel)
          {
            os << '[' << hk << "] ";
          }
          else
          {
            os << '[' << kEscFgRed << kEscBold << hk << kEscReset << kEscBgBlack << kEscFgWhite
               << "] ";
          }
          consumed += 4;
        }
        else
        {
          os << "    ";
          consumed += 4;
        }
        os << matches[i];
        consumed += utf8Width(matches[i]);
        pad(os, interiorW - consumed);
      }
      else
      {
        os << kEscBgBlack << kEscFgWhite;
        pad(os, interiorW);
      }
      os << kEscReset << kEscBgBlack << kEscFgCyan << "\xe2\x94\x82" << kEscReset;
    }

    // Footer.
    putAt(os, top + height - 2, left);
    std::string_view footer = "\xe2\x86\x91\xe2\x86\x93 select  Enter pick  Esc cancel";
    os << kEscReset << kEscBgBlack << kEscFgCyan << "\xe2\x94\x82" << kEscBgBlack
       << kEscFgWhite << ' ' << footer;
    pad(os, interiorW - 1 - utf8Width(footer));
    os << kEscBgBlack << kEscFgCyan << "\xe2\x94\x82" << kEscReset;

    // Bottom border.
    putAt(os, top + height - 1, left);
    os << kEscReset << kEscBgBlack << kEscFgCyan << "\xe2\x94\x94";
    for (int i = 0; i < width - 2; ++i) os << "\xe2\x94\x80";
    os << "\xe2\x94\x98" << kEscReset;

    const std::string s = os.str();
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fflush(stdout);

    int ch = wgetch(itsStatusWin);
    if (ch == 27)
    {
      touch();
      return -1;
    }
    if (ch == '\n' || ch == KEY_ENTER)
    {
      touch();
      return matches.empty() ? -1 : sel;
    }
    if (ch == KEY_BACKSPACE || ch == 127 || ch == 8)
    {
      // Drop last UTF-8 codepoint (one or more continuation bytes + one lead).
      while (!query.empty() && (static_cast<unsigned char>(query.back()) & 0xC0) == 0x80)
        query.pop_back();
      if (!query.empty()) query.pop_back();
      sel = 0;
      continue;
    }
    if (ch == KEY_UP)
    {
      sel = std::max(0, sel - 1);
      continue;
    }
    if (ch == KEY_DOWN)
    {
      if (!matches.empty()) sel = std::min(static_cast<int>(matches.size()) - 1, sel + 1);
      continue;
    }
    if (ch >= '1' && ch <= '9')
    {
      const int idx = ch - '1';
      if (idx < static_cast<int>(matches.size()))
      {
        touch();
        return idx;
      }
      // Fall through to add as part of query.
    }
    if (ch >= 32 && ch < 127)
    {
      query += static_cast<char>(ch);
      sel = 0;
      continue;
    }
    if (ch == KEY_RESIZE)
    {
      recomputeLayout();
      continue;
    }
    // Other: ignore.
  }
}

void UI::popupHelp()
{
  // Two-column listing: keys on the left (highlighted), action on the right.
  // Empty key/action pair = visual separator (blank row).
  static const std::vector<std::pair<std::string, std::string>> kEntries = {
      {"q  Esc",                    "Quit"},
      {"p",                         "Parameter menu"},
      {"L (Shift)",                 "Level menu"},
      {"\xe2\x86\x90 \xe2\x86\x92", "Previous / next time"},
      {"Home  End",                 "First / last time"},
      {"",                          ""},
      {"Space",                     "Play / pause animation"},
      {"\xe2\x86\x91 \xe2\x86\x93", "Animation speed up / down"},
      {"",                          ""},
      {"+  -",                      "Zoom in / out (centre)"},
      {"dbl-click L / R",           "Zoom in / out at cursor"},
      {"0",                         "Reset view"},
      {"h j k l",                   "Pan left / down / up / right"},
      {"Shift+arrow",               "Pan"},
      {"drag (mouse)",              "Pan"},
      {"",                          ""},
      {"click (mouse)",             "Time-series probe at point"},
      {"\xe2\x86\x90 \xe2\x86\x92 in probe",
                                    "Step time, map updates"},
      {"Space in probe",            "Play / pause animation (\xe2\x86\x91\xe2\x86\x93 speed)"},
      {"s in probe",                "Toggle viewport min/mean/max overlay"},
      {"",                          ""},
      {"g",                         "Legend"},
      {"r",                         "Cycle palette (shapefiles: flat \xe2\x86\x92 rainbow)"},
      {"c",                         "Coastlines: braille \xe2\x86\x92 thick \xe2\x86\x92 off"},
      {"b",                         "Borders: braille \xe2\x86\x92 thick \xe2\x86\x92 off"},
      {"t",                         "Cell style: sextants \xe2\x86\x92 triangles \xe2\x86\x92 squares (font fallback)"},
      {"n",                         "Toggle lat/lon graticule"},
      {"w",                         "Toggle wind arrows"},
      {"i",                         "Toggle city overlay"},
      {"PgUp PgDn",                 "Cities: sparser / denser"},
      {"/",                         "Place search"},
      {"x",                         "Cross-section"},
      {"e",                         "Export PNG (active panel)"},
      {"",                          ""},
      {"M",                         "File metadata"},
      {"F2",                        "Cycle layout: single \xe2\x86\x92 side \xe2\x86\x92 2x2"},
      {"Tab  Shift+Tab",            "Next / previous active panel"},
      {"1 2 3 4",                   "Activate panel by number"},
      {"click (mouse)",             "Activate the panel under the cursor"},
      {"",                          ""},
      {"?",                         "This help"},
  };

  int maxL = 0;
  int maxR = 0;
  for (const auto& [l, r] : kEntries)
  {
    maxL = std::max(maxL, utf8Width(l));
    maxR = std::max(maxR, utf8Width(r));
  }
  const int sep = 3;  // gap between key column and action column
  const std::string title = "Help";
  const int contentW = maxL + sep + maxR;
  const int width = std::min(COLS - 4, std::max({contentW + 4, utf8Width(title) + 8, 40}));
  const int interiorW = width - 2;

  // Layout: top border + N body rows + 1 footer + bottom border = N + 3.
  const int height = static_cast<int>(kEntries.size()) + 3;
  const int top = std::max(0, (LINES - height) / 2);
  const int left = std::max(0, (COLS - width) / 2);

  std::ostringstream os;

  // Top border with embedded title.
  putAt(os, top, left);
  os << kEscReset << kEscBgBlack << kEscFgCyan << "\xe2\x94\x8c\xe2\x94\x80[" << kEscFgWhite
     << title << kEscFgCyan << "]";
  int titleConsumed = 4 + utf8Width(title);
  for (int i = 0; i < width - titleConsumed - 1; ++i) os << "\xe2\x94\x80";
  os << "\xe2\x94\x90" << kEscReset;

  // Body rows.
  for (std::size_t i = 0; i < kEntries.size(); ++i)
  {
    putAt(os, top + 1 + static_cast<int>(i), left);
    os << kEscReset << kEscBgBlack << kEscFgCyan << "\xe2\x94\x82" << kEscBgBlack;

    const auto& [keys, action] = kEntries[i];
    if (keys.empty() && action.empty())
    {
      // Separator row.
      os << kEscFgWhite;
      pad(os, interiorW);
    }
    else
    {
      os << ' ';
      // Keys in red.
      os << kEscFgRed << kEscBold << keys << kEscReset << kEscBgBlack << kEscFgWhite;
      pad(os, maxL - utf8Width(keys));
      // Separator + action.
      pad(os, sep);
      os << action;
      pad(os, interiorW - 1 - maxL - sep - utf8Width(action));
    }
    os << kEscReset << kEscBgBlack << kEscFgCyan << "\xe2\x94\x82" << kEscReset;
  }

  // Footer.
  putAt(os, top + height - 2, left);
  std::string_view footer = "any key to close";
  os << kEscReset << kEscBgBlack << kEscFgCyan << "\xe2\x94\x82" << kEscBgBlack << kEscFgWhite
     << ' ' << footer;
  pad(os, interiorW - 1 - utf8Width(footer));
  os << kEscBgBlack << kEscFgCyan << "\xe2\x94\x82" << kEscReset;

  // Bottom border.
  putAt(os, top + height - 1, left);
  os << kEscReset << kEscBgBlack << kEscFgCyan << "\xe2\x94\x94";
  for (int i = 0; i < width - 2; ++i) os << "\xe2\x94\x80";
  os << "\xe2\x94\x98" << kEscReset;

  const std::string s = os.str();
  std::fwrite(s.data(), 1, s.size(), stdout);
  std::fflush(stdout);

  wgetch(itsStatusWin);
  touch();
}

void UI::popupMetadata(const std::string& title,
                       const std::vector<std::pair<std::string, std::string>>& rows)
{
  if (rows.empty()) return;

  // Width budget: clamp at the screen width. Long values (filename,
  // projection string, parameter listing) wrap across continuation rows
  // rather than truncating, so the popup gets taller for big lists
  // instead of losing data.
  const int screenW = std::max(40, COLS - 4);
  int maxL = 0;
  for (const auto& [l, v] : rows) maxL = std::max(maxL, utf8Width(l));
  const int sep = 2;            // gap between label and value
  constexpr int kMargin = 4;    // borders + interior padding (1 left + 1 right + 2 borders)
  int maxV = 0;
  for (const auto& [l, v] : rows) maxV = std::max(maxV, utf8Width(v));
  // Cap width so the popup doesn't span the full screen for one row that
  // happens to be very long. 100 cols is a comfortable reading width;
  // values longer than that wrap.
  const int desired = std::max({maxL + sep + maxV + kMargin, utf8Width(title) + 8, 50});
  const int width = std::min({screenW, 100, desired});
  const int interiorW = width - 2;
  const int valueW = std::max(8, interiorW - 1 - maxL - sep - 1);

  // Wrap a long value into chunks each fitting in `valueW` columns.
  // Prefers breaking at ", " (for comma-separated lists like the
  // parameter listing); otherwise at any whitespace; otherwise hard-
  // breaks. The empty input is treated as a single empty chunk.
  auto wrapValue = [&](const std::string& s) -> std::vector<std::string> {
    std::vector<std::string> out;
    if (utf8Width(s) <= valueW)
    {
      out.push_back(s);
      return out;
    }
    std::string remaining = s;
    while (!remaining.empty())
    {
      if (utf8Width(remaining) <= valueW)
      {
        out.push_back(remaining);
        break;
      }
      // Walk the byte index to find the latest break-friendly position
      // whose prefix fits in valueW columns. utf8Width counts column
      // width including multi-byte characters; for the param listing
      // and ASCII values these are 1:1 with bytes.
      int bestComma = -1;
      int bestSpace = -1;
      int bestHard = -1;
      // Iterate forward from byte 0; track display width.
      int width_so_far = 0;
      for (std::size_t i = 0; i < remaining.size(); ++i)
      {
        // Treat ASCII range as width-1; multi-byte continuations (bytes
        // with the 10xxxxxx pattern) add zero width. Good enough for our
        // values (filenames, projection strings, param names).
        const unsigned char c = static_cast<unsigned char>(remaining[i]);
        if ((c & 0xC0) != 0x80) width_so_far += 1;
        if (width_so_far > valueW) break;
        // After this byte, look at the boundary BETWEEN i and i+1.
        if (i + 1 < remaining.size())
        {
          if (remaining[i] == ',' && remaining[i + 1] == ' ')
            bestComma = static_cast<int>(i + 2);  // include the space
          else if (remaining[i] == ' ')
            bestSpace = static_cast<int>(i + 1);
        }
        bestHard = static_cast<int>(i + 1);
      }
      const int cut = bestComma > 0 ? bestComma : bestSpace > 0 ? bestSpace : bestHard;
      if (cut <= 0)
      {
        out.push_back(remaining);
        break;
      }
      std::string chunk = remaining.substr(0, cut);
      // Trim trailing space on continuation chunks for tidiness.
      while (!chunk.empty() && chunk.back() == ' ') chunk.pop_back();
      out.push_back(chunk);
      remaining = remaining.substr(cut);
    }
    if (out.empty()) out.push_back(std::string{});
    return out;
  };

  // Expand each input row into one or more physical lines: the first
  // physical line carries the label; continuation lines have a blank
  // label so the value stays aligned. A blank input row stays as one
  // blank physical row.
  struct PhysRow
  {
    std::string label;
    std::string value;
    bool blank = false;
  };
  std::vector<PhysRow> phys;
  phys.reserve(rows.size());
  for (const auto& [label, value] : rows)
  {
    if (label.empty() && value.empty())
    {
      phys.push_back(PhysRow{"", "", true});
      continue;
    }
    auto chunks = wrapValue(value);
    for (std::size_t k = 0; k < chunks.size(); ++k)
      phys.push_back(PhysRow{k == 0 ? label : std::string{}, chunks[k], false});
  }

  // Layout: top border + N physical body rows + 1 footer + bottom border.
  const int height = static_cast<int>(phys.size()) + 3;
  const int top = std::max(0, (LINES - height) / 2);
  const int left = std::max(0, (COLS - width) / 2);

  std::ostringstream os;

  putAt(os, top, left);
  os << kEscReset << kEscBgBlack << kEscFgCyan << "\xe2\x94\x8c\xe2\x94\x80[" << kEscFgWhite
     << title << kEscFgCyan << "]";
  int titleConsumed = 4 + utf8Width(title);
  for (int i = 0; i < width - titleConsumed - 1; ++i) os << "\xe2\x94\x80";
  os << "\xe2\x94\x90" << kEscReset;

  for (std::size_t i = 0; i < phys.size(); ++i)
  {
    putAt(os, top + 1 + static_cast<int>(i), left);
    os << kEscReset << kEscBgBlack << kEscFgCyan << "\xe2\x94\x82" << kEscBgBlack;

    const auto& r = phys[i];
    if (r.blank)
    {
      os << kEscFgWhite;
      pad(os, interiorW);
    }
    else
    {
      os << ' ';
      os << kEscFgRed << kEscBold << r.label << kEscReset << kEscBgBlack << kEscFgWhite;
      pad(os, maxL - utf8Width(r.label));
      pad(os, sep);
      os << r.value;
      pad(os, interiorW - 1 - maxL - sep - utf8Width(r.value));
    }
    os << kEscReset << kEscBgBlack << kEscFgCyan << "\xe2\x94\x82" << kEscReset;
  }

  putAt(os, top + height - 2, left);
  std::string_view footer = "any key to close";
  os << kEscReset << kEscBgBlack << kEscFgCyan << "\xe2\x94\x82" << kEscBgBlack << kEscFgWhite
     << ' ' << footer;
  pad(os, interiorW - 1 - utf8Width(footer));
  os << kEscBgBlack << kEscFgCyan << "\xe2\x94\x82" << kEscReset;

  putAt(os, top + height - 1, left);
  os << kEscReset << kEscBgBlack << kEscFgCyan << "\xe2\x94\x94";
  for (int i = 0; i < width - 2; ++i) os << "\xe2\x94\x80";
  os << "\xe2\x94\x98" << kEscReset;

  const std::string s = os.str();
  std::fwrite(s.data(), 1, s.size(), stdout);
  std::fflush(stdout);

  wgetch(itsStatusWin);
  touch();
}

void UI::popupLegend(const std::string& paramName, const std::string& paletteName,
                     const Palette& palette, const Renderer& renderer)
{
  const auto& bands = palette.bands();
  if (bands.empty()) return;

  auto formatBound = [](std::optional<float> v) -> std::string {
    if (!v.has_value()) return "*";
    return fmt::format("{:g}", *v);
  };

  std::vector<std::string> labels;
  labels.reserve(bands.size());
  int maxLabel = 0;
  for (const auto& b : bands)
  {
    std::string s = formatBound(b.lo) + " .. " + formatBound(b.hi);
    maxLabel = std::max(maxLabel, utf8Width(s));
    labels.push_back(std::move(s));
  }

  constexpr int kSwatchW = 4;
  int contentW = kSwatchW + 2 + maxLabel;
  int titleW = std::max(utf8Width(paramName), utf8Width(paletteName));
  int width = std::max(contentW, titleW) + 4;
  width = std::min(width, COLS - 4);
  int interiorW = width - 2;

  constexpr int kHeaderRows = 2;
  constexpr int kFooterRows = 1;
  int maxBodyRows = std::max(1, LINES - 4 - kHeaderRows - kFooterRows);
  int n = std::min(static_cast<int>(bands.size()), maxBodyRows);
  int height = kHeaderRows + n + kFooterRows + 2;
  int top = std::max(0, (LINES - height) / 2);
  int left = std::max(0, (COLS - width) / 2);

  std::ostringstream os;

  // Top border.
  putAt(os, top, left);
  os << kEscReset << kEscBgBlack << kEscFgCyan << "\xe2\x94\x8c";
  for (int i = 0; i < width - 2; ++i) os << "\xe2\x94\x80";
  os << "\xe2\x94\x90" << kEscReset;

  // Title rows.
  putAt(os, top + 1, left);
  os << kEscReset << kEscBgBlack << kEscFgCyan << "\xe2\x94\x82" << kEscBgBlack << kEscFgWhite
     << kEscBold << ' ' << paramName << kEscReset << kEscBgBlack << kEscFgWhite;
  pad(os, interiorW - 1 - utf8Width(paramName));
  os << kEscBgBlack << kEscFgCyan << "\xe2\x94\x82" << kEscReset;

  putAt(os, top + 2, left);
  os << kEscReset << kEscBgBlack << kEscFgCyan << "\xe2\x94\x82" << kEscBgBlack << kEscFgWhite
     << ' ' << paletteName;
  pad(os, interiorW - 1 - utf8Width(paletteName));
  os << kEscBgBlack << kEscFgCyan << "\xe2\x94\x82" << kEscReset;

  // Bands, largest first.
  for (int i = 0; i < n; ++i)
  {
    int bandIdx = static_cast<int>(bands.size()) - 1 - i;
    putAt(os, top + kHeaderRows + 1 + i, left);
    os << kEscReset << kEscBgBlack << kEscFgCyan << "\xe2\x94\x82" << kEscBgBlack << kEscFgWhite
       << ' ' << renderer.bgEscape(bands[bandIdx].rgb);
    for (int sp = 0; sp < kSwatchW; ++sp) os << ' ';
    os << kEscReset << kEscBgBlack << kEscFgWhite << ' ' << labels[bandIdx];
    int consumed = 1 + kSwatchW + 1 + utf8Width(labels[bandIdx]);
    pad(os, interiorW - consumed);
    os << kEscBgBlack << kEscFgCyan << "\xe2\x94\x82" << kEscReset;
  }

  // Footer.
  putAt(os, top + height - 2, left);
  std::string_view footer =
      (n < static_cast<int>(bands.size())) ? "(more bands hidden)" : "any key to close";
  os << kEscReset << kEscBgBlack << kEscFgCyan << "\xe2\x94\x82" << kEscBgBlack << kEscFgWhite
     << ' ' << footer;
  pad(os, interiorW - 1 - utf8Width(footer));
  os << kEscBgBlack << kEscFgCyan << "\xe2\x94\x82" << kEscReset;

  // Bottom border.
  putAt(os, top + height - 1, left);
  os << kEscReset << kEscBgBlack << kEscFgCyan << "\xe2\x94\x94";
  for (int i = 0; i < width - 2; ++i) os << "\xe2\x94\x80";
  os << "\xe2\x94\x98" << kEscReset;

  const std::string s = os.str();
  std::fwrite(s.data(), 1, s.size(), stdout);
  std::fflush(stdout);

  // Wait for any key to dismiss.
  wgetch(itsStatusWin);

  touch();
}

namespace
{
// Pick a "nice" tick step for an axis spanning `range`, aiming for at most
// `maxTicks` labels. Returns one of {1, 2, 5} × 10^k for some integer k.
double niceStep(double range, int maxTicks)
{
  if (range <= 0 || maxTicks < 1) return 1.0;
  const double rough = range / maxTicks;
  const double exponent = std::floor(std::log10(rough));
  const double power = std::pow(10.0, exponent);
  const double fraction = rough / power;
  double nf = 1.0;
  if (fraction < 1.5)
    nf = 1.0;
  else if (fraction < 3.5)
    nf = 2.0;
  else if (fraction < 7.5)
    nf = 5.0;
  else
    nf = 10.0;
  return nf * power;
}

// Encode a braille codepoint U+2800+mask as a 3-byte UTF-8 string.
// Dot bit layout (per Unicode braille block):
//   col 0: bits 0,1,2,6   (rows 0..3)
//   col 1: bits 3,4,5,7   (rows 0..3)
std::string brailleGlyph(unsigned mask)
{
  std::string s(3, '\0');
  s[0] = static_cast<char>(0xE2);
  s[1] = static_cast<char>(0xA0 | ((mask >> 6) & 0x03));
  s[2] = static_cast<char>(0x80 | (mask & 0x3F));
  return s;
}

unsigned brailleBit(int subCol, int subRow)
{
  // subCol in 0..1, subRow in 0..3.
  if (subRow == 3) return (subCol == 0) ? 6U : 7U;
  return static_cast<unsigned>(subCol * 3 + subRow);
}

bool finiteValue(float v)
{
  return std::isfinite(v) && std::abs(v) < 1e10F;
}
}  // namespace

int UI::popupTimeseries(const std::string& paramName, double lat, double lon,
                        const std::vector<float>& series,
                        const std::vector<std::string>& timeLabels, int currentIndex,
                        const Renderer& renderer, const Palette& palette,
                        std::function<void(int)> onTimeChange,
                        std::function<StatsSeries()> computeStats,
                        const std::string& units, int* animationDelayMs,
                        int avoidCellRow, int avoidCellCol,
                        int* outClickRow, int* outClickCol)
{
  (void)palette;  // reserved for future colour-by-band line rendering
  if (outClickRow) *outClickRow = -1;
  if (outClickCol) *outClickCol = -1;
  if (series.empty()) return currentIndex;

  // Stats overlay state. Lazily fetched on first 's' press; the App
  // caches the underlying scan, so re-toggling within a probe (or
  // re-opening the probe at a new coordinate within the same viewport)
  // is instant.
  StatsSeries stats;
  bool statsVisible = false;
  bool statsAttempted = false;

  // Recomputed on every show/hide of the stats overlay so the chart
  // expands or contracts to fit the visible curves.
  float dataLo = 0;
  float dataHi = 1;
  double tickStep = 1.0;
  float vmin = 0;
  float vmax = 1;
  int decimals = 0;

  auto rebuildAxis = [&]() {
    dataLo = std::numeric_limits<float>::infinity();
    dataHi = -std::numeric_limits<float>::infinity();
    int finiteCount = 0;
    auto includeSeries = [&](const std::vector<float>& s) {
      for (float v : s)
        if (finiteValue(v))
        {
          dataLo = std::min(dataLo, v);
          dataHi = std::max(dataHi, v);
          ++finiteCount;
        }
    };
    includeSeries(series);
    if (statsVisible)
    {
      includeSeries(stats.min);
      includeSeries(stats.mean);
      includeSeries(stats.max);
    }
    if (finiteCount == 0)
    {
      dataLo = 0;
      dataHi = 1;
    }
    if (dataHi - dataLo < 1e-6F)
    {
      dataLo -= 0.5F;
      dataHi += 0.5F;
    }
    // Compute "nice" axis bounds and tick step (1, 2 or 5 × 10^k) so
    // the Y axis labels are clean integers / decimals.
    constexpr int desiredLabels = 5;
    tickStep = niceStep(static_cast<double>(dataHi - dataLo), desiredLabels);
    const double niceMin = std::floor(dataLo / tickStep) * tickStep;
    const double niceMax = std::ceil(dataHi / tickStep) * tickStep;
    vmin = static_cast<float>(niceMin);
    vmax = static_cast<float>(niceMax);
    const int stepExp = static_cast<int>(std::floor(std::log10(tickStep)));
    decimals = std::max(0, -stepExp);
  };
  rebuildAxis();

  // Header lines that don't change as the marker moves.
  const std::string latlonBuf = fmt::format("{:.4f}°N  {:.4f}°E", lat, lon);

  // Layout. Average of the previous two attempts: 75% of terminal (too
  // big) and 25%-with-30/5-floor (too small) lands at a comfortable
  // ~45×7 on 80×24, ~60×10 on 120×40, and ~100×16 on 200×60 — readable
  // at typical zoom levels and still scaling up enough that a tiny-font
  // window doesn't render a postage-stamp popup.
  const int avgW = (COLS * 75 / 100 + std::max(30, COLS / 4)) / 2;
  const int avgH = (LINES * 42 / 100 + std::max(5, LINES / 8)) / 2;
  const int desiredChartW = std::min(std::max(20, COLS - 16), avgW);
  const int desiredChartH = std::min(std::max(4, (LINES - 10) / 2), avgH);
  const int chartW = std::max(20, desiredChartW);
  const int chartH = std::max(4, desiredChartH);

  // Format axis labels with the precision dictated by the chosen step.
  // `decimals` is captured by reference so toggling stats picks up the
  // updated precision.
  auto fmtAxis = [&decimals](double v) {
    std::string tmp = fmt::format("{:.{}f}", v, decimals);
    // Avoid "-0" output.
    if (tmp == std::string("-") + std::string(decimals + 1, '0').replace(1, 1, "."))
      tmp = fmt::format("{:.{}f}", 0.0, decimals);
    return tmp;
  };
  // Pick label-column width once based on the initial axis range. Don't
  // resize the popup on stats toggle — a wider stats range typically still
  // fits in 5–6 chars; if it doesn't, the rightmost digits clip rather
  // than the popup jumping geometry.
  int labelW = std::max(utf8Width(fmtAxis(static_cast<double>(vmin))),
                        utf8Width(fmtAxis(static_cast<double>(vmax))));
  labelW = std::max(labelW, 3);
  const int chartLeftPad = labelW + 3;  // " <label> ┤"

  // Recomputed on every axis change.
  std::vector<bool> hasLabel(chartH, false);
  std::vector<double> labelValue(chartH, 0);
  auto rebuildLabels = [&]() {
    std::fill(hasLabel.begin(), hasLabel.end(), false);
    std::fill(labelValue.begin(), labelValue.end(), 0.0);
    if (vmax > vmin)
    {
      const double dvmin = vmin;
      const double dvmax = vmax;
      for (double t = dvmin; t <= dvmax + tickStep * 0.5; t += tickStep)
      {
        const double frac = (dvmax - t) / (dvmax - dvmin);  // 0 at top
        const int row =
            std::clamp(static_cast<int>(std::lround(frac * (chartH - 1))), 0, chartH - 1);
        hasLabel[row] = true;
        labelValue[row] = t;
      }
    }
  };
  rebuildLabels();

  const bool showTimeRow = !timeLabels.empty();
  // Width must also fit the time label if present (e.g. "2026-05-08 12:00 UTC").
  int timeMaxW = 0;
  if (showTimeRow)
    for (const auto& t : timeLabels) timeMaxW = std::max(timeMaxW, utf8Width(t));
  const std::string footer = animationDelayMs != nullptr
      ? std::string("\xe2\x86\x90\xe2\x86\x92 / click step time   "
                    "Space play   \xe2\x86\x91\xe2\x86\x93 speed   "
                    "s: viewport stats   any key: close")
      : std::string("\xe2\x86\x90\xe2\x86\x92 / click / drag step time   "
                    "s: viewport stats   click map: re-probe   any key: close");
  const int width = std::max({chartW + chartLeftPad + 4,
                              utf8Width(paramName) + 6,
                              utf8Width(latlonBuf) + 6,
                              timeMaxW + 6,
                              utf8Width(footer) + 4,
                              40});
  // border + (param, latlon, [time], range) header rows + chart + footer + border
  const int headerRows = showTimeRow ? 4 : 3;
  const int height = chartH + headerRows + 3;
  // Place the popup in the quadrant opposite the marker if known, so the
  // crosshair on the map stays visible. Falls back to centred placement.
  int top = std::max(0, (LINES - height) / 2);
  int left = std::max(0, (COLS - width) / 2);
  if (avoidCellRow >= 0 && avoidCellCol >= 0)
  {
    top = (avoidCellRow < LINES / 2) ? std::max(0, LINES - height - 2) : 1;
    left = (avoidCellCol < COLS / 2) ? std::max(0, COLS - width - 2) : 1;
  }
  const int interiorW = width - 2;

  // Braille sub-cell coordinates: 2 columns × 4 rows per chart cell.
  const int subW = chartW * 2;
  const int subH = chartH * 4;
  const int n = static_cast<int>(series.size());
  auto sampleSubX = [&](int idx) {
    return (n > 1) ? (idx * (subW - 1) / (n - 1)) : (subW / 2);
  };

  // Up to four overlaid series (point + stats min/mean/max), each with
  // its own braille mask grid and colour. Indices below are ordered by
  // priority: when multiple series have a dot in the same cell, we draw
  // the highest-numbered one (point series wins over stats so the user's
  // probe is always foreground).
  enum SeriesIdx
  {
    kStatsMin = 0,
    kStatsMax = 1,
    kStatsMean = 2,
    kPoint = 3,
    kSeriesCount = 4,
  };
  std::array<std::vector<unsigned>, kSeriesCount> grids;
  for (auto& g : grids) g.assign(static_cast<std::size_t>(chartW) * chartH, 0U);

  auto valueToSubY = [&](float v) -> int {
    const float yn = (v - vmin) / (vmax - vmin);
    return std::clamp(static_cast<int>(std::lround((1.0F - yn) * (subH - 1))), 0, subH - 1);
  };

  auto setDot = [&](std::vector<unsigned>& g, int sx, int sy) {
    if (sx < 0 || sx >= subW || sy < 0 || sy >= subH) return;
    const int cx = sx / 2;
    const int cy = sy / 4;
    g[static_cast<std::size_t>(cy) * chartW + cx] |= 1U << brailleBit(sx % 2, sy % 4);
  };

  // Rasterise one series via Bresenham line segments. Missing values
  // break the line so jumps don't connect across gaps.
  auto rasterise = [&](std::vector<unsigned>& g, const std::vector<float>& s) {
    int prevSx = -1;
    int prevSy = -1;
    bool prevValid = false;
    const int sn = static_cast<int>(s.size());
    for (int i = 0; i < sn; ++i)
    {
      if (!finiteValue(s[i]))
      {
        prevValid = false;
        continue;
      }
      const int sx = (sn > 1) ? (i * (subW - 1) / (sn - 1)) : (subW / 2);
      const int sy = valueToSubY(s[i]);
      if (prevValid)
      {
        int x0 = prevSx;
        int y0 = prevSy;
        const int x1 = sx;
        const int y1 = sy;
        const int dx = std::abs(x1 - x0);
        const int dy = -std::abs(y1 - y0);
        const int sxStep = x0 < x1 ? 1 : -1;
        const int syStep = y0 < y1 ? 1 : -1;
        int err = dx + dy;
        while (true)
        {
          setDot(g, x0, y0);
          if (x0 == x1 && y0 == y1) break;
          const int e2 = 2 * err;
          if (e2 >= dy)
          {
            err += dy;
            x0 += sxStep;
          }
          if (e2 <= dx)
          {
            err += dx;
            y0 += syStep;
          }
        }
      }
      else
      {
        setDot(g, sx, sy);
      }
      prevSx = sx;
      prevSy = sy;
      prevValid = true;
    }
  };

  auto rebuildGrids = [&]() {
    for (auto& g : grids) std::fill(g.begin(), g.end(), 0U);
    rasterise(grids[kPoint], series);
    if (statsVisible)
    {
      if (!stats.min.empty()) rasterise(grids[kStatsMin], stats.min);
      if (!stats.max.empty()) rasterise(grids[kStatsMax], stats.max);
      if (!stats.mean.empty()) rasterise(grids[kStatsMean], stats.mean);
    }
  };
  rebuildGrids();

  // First chart row offset (logical, relative to popup top): top border +
  // paramName + latlonBuf + optional timeBuf + rangeBuf.
  const int chartTopOffset = 3 + (showTimeRow ? 1 : 0) + 1;

  // Helper to render one full popup frame with a given marker index.
  auto renderFrame = [&](int markerIdx) {
    // Vertical marker at the current time step.
    int markerSubX = -1;
    if (markerIdx >= 0 && markerIdx < n) markerSubX = sampleSubX(markerIdx);

    // The point-series value at the current marker. Labelled "value"
    // rather than "now" because the marker can be anywhere in the
    // forecast; "now" implies real-time which it isn't.
    const float curVal =
        (markerIdx >= 0 && markerIdx < static_cast<int>(series.size()))
            ? series[markerIdx]
            : std::numeric_limits<float>::quiet_NaN();

    // Format a numeric reading with optional units appended.
    auto fmtNum = [&](float v) {
      std::string s = fmt::format("{:.{}f}", v, decimals);
      if (!units.empty())
      {
        s += ' ';
        s += units;
      }
      return s;
    };
    // Colour the numeric values to match the chart series: point series
    // (and the "value" reading) green; min/max grey (envelope); mean
    // teal. Build both an ANSI-coloured string and a parallel visible-
    // width counter so writeRow can pad correctly without trying to
    // strip the escapes.
    constexpr std::string_view kFgValueGreen = "\x1b[38;5;46m";
    constexpr std::string_view kFgMeanTeal = "\x1b[38;5;38m";
    constexpr std::string_view kFgMinMaxGrey = "\x1b[38;5;240m";

    std::string rangeBuf;
    int rangeBufVisible = 0;
    auto pushPlain = [&](std::string_view t) {
      rangeBuf.append(t);
      rangeBufVisible += utf8Width(std::string(t));
    };
    auto pushColored = [&](std::string_view colour, std::string_view t) {
      rangeBuf.append(colour);
      rangeBuf.append(t);
      rangeBuf.append(kEscFgWhite);
      rangeBufVisible += utf8Width(std::string(t));
    };

    // When stats are visible, the info line summarises viewport-wide
    // min / mean / max at the marker's time step instead of duplicating
    // the point series's overall extrema (which the chart shows
    // anyway).
    if (statsVisible && markerIdx >= 0 && markerIdx < static_cast<int>(stats.mean.size()) &&
        finiteValue(stats.mean[markerIdx]))
    {
      pushPlain("viewport min ");
      pushColored(kFgMinMaxGrey, fmtNum(stats.min[markerIdx]));
      pushPlain("  mean ");
      pushColored(kFgMeanTeal, fmtNum(stats.mean[markerIdx]));
      pushPlain("  max ");
      pushColored(kFgMinMaxGrey, fmtNum(stats.max[markerIdx]));
    }
    else if (statsVisible && markerIdx >= 0)
    {
      pushPlain("viewport: no finite samples");
    }
    else
    {
      pushPlain("min ");
      pushPlain(fmtNum(dataLo));
      pushPlain("  max ");
      pushPlain(fmtNum(dataHi));
    }
    pushPlain("  step ");
    pushPlain(fmt::format("{}/{}", markerIdx + 1, static_cast<int>(series.size())));
    pushPlain("  value ");
    if (finiteValue(curVal))
      pushColored(kFgValueGreen, fmtNum(curVal));
    else
      pushPlain("-");

    std::ostringstream os;

    // Top border.
    putAt(os, top, left);
    os << kEscReset << kEscBgBlack << kEscFgCyan << "\xe2\x94\x8c";
    for (int i = 0; i < width - 2; ++i) os << "\xe2\x94\x80";
    os << "\xe2\x94\x90" << kEscReset;

    // `visibleWidth` overrides utf8Width(label) for strings that already
    // contain ANSI colour escapes (rangeBuf). When negative, we measure
    // the label as plain text.
    auto writeRow = [&](int rowOffset, std::string_view label, bool bold = false,
                        int visibleWidth = -1) {
      putAt(os, top + rowOffset, left);
      os << kEscReset << kEscBgBlack << kEscFgCyan << "\xe2\x94\x82" << kEscBgBlack
         << kEscFgWhite;
      if (bold) os << kEscBold;
      os << ' ' << label;
      const int w = visibleWidth >= 0 ? visibleWidth : utf8Width(std::string(label));
      pad(os, interiorW - 1 - w);
      os << kEscReset << kEscBgBlack << kEscFgCyan << "\xe2\x94\x82" << kEscReset;
    };

    writeRow(1, paramName, true);
    writeRow(2, latlonBuf, false);
    int rowCursor = 3;
    if (showTimeRow)
    {
      const std::string& timeStr =
          (markerIdx >= 0 && markerIdx < static_cast<int>(timeLabels.size()))
              ? timeLabels[markerIdx]
              : timeLabels.front();
      writeRow(rowCursor++, timeStr, false);
    }
    writeRow(rowCursor++, rangeBuf, false, rangeBufVisible);
    // chart starts at top + chartTopOffset (computed outside this lambda).

    // Chart rows.
    for (int cy = 0; cy < chartH; ++cy)
    {
      putAt(os, top + chartTopOffset + cy, left);
      os << kEscReset << kEscBgBlack << kEscFgCyan << "\xe2\x94\x82" << kEscBgBlack
         << kEscFgWhite;

      // Y-axis label + tick (or just spacer + axis line).
      if (hasLabel[cy])
      {
        const std::string lab = fmtAxis(labelValue[cy]);
        os << ' ';
        pad(os, labelW - utf8Width(lab));
        os << lab << " \xe2\x94\xa4";  // " label ┤"
      }
      else
      {
        os << ' ';
        pad(os, labelW);
        os << " \xe2\x94\x82";  // axis │
      }

      // Per-series colour. Min/max are dim grey (envelope), mean is
      // teal, point series is bright green so the user's chosen-
      // coordinate trace stands out as the primary signal. Marker
      // cells override fg to red to keep the time cursor unambiguous.
      static constexpr std::array<std::string_view, kSeriesCount> kSeriesFg = {
          "\x1b[38;5;240m",  // min — dark grey
          "\x1b[38;5;240m",  // max — dark grey
          "\x1b[38;5;38m",   // mean — teal
          "\x1b[38;5;46m",   // point series — bright green
      };
      for (int cx = 0; cx < chartW; ++cx)
      {
        const bool inMarker = (markerSubX >= 0 && (markerSubX / 2) == cx);
        const std::size_t cellIdx = static_cast<std::size_t>(cy) * chartW + cx;
        // Pick the highest-priority series with a dot in this cell.
        int chosen = -1;
        for (int s = kSeriesCount - 1; s >= 0; --s)
        {
          if (grids[s][cellIdx] != 0U)
          {
            chosen = s;
            break;
          }
        }
        if (inMarker && chosen < 0)
        {
          os << kEscFgRed << "\xe2\x94\x82" << kEscFgWhite;
        }
        else if (chosen < 0)
        {
          os << ' ';
        }
        else
        {
          os << (inMarker ? kEscFgRed : kSeriesFg[chosen]);
          os << brailleGlyph(grids[chosen][cellIdx]);
          os << kEscFgWhite;
        }
      }
      // Right-side padding inside the box.
      pad(os, interiorW - chartLeftPad - chartW);
      os << kEscReset << kEscBgBlack << kEscFgCyan << "\xe2\x94\x82" << kEscReset;
    }

    writeRow(chartTopOffset + chartH, footer, false);

    putAt(os, top + height - 1, left);
    os << kEscReset << kEscBgBlack << kEscFgCyan << "\xe2\x94\x94";
    for (int i = 0; i < width - 2; ++i) os << "\xe2\x94\x80";
    os << "\xe2\x94\x98" << kEscReset;

    const std::string s = os.str();
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fflush(stdout);
  };

  int idx = std::clamp(currentIndex, 0, static_cast<int>(series.size()) - 1);
  bool dragging = false;
  bool animating = false;

  // Brief flashes a "Computing viewport stats…" message in the chart's
  // status row before invoking the (potentially slow) stats fetch, so a
  // multi-second scan doesn't look like the popup hung. The next render
  // overwrites the message with the real info row.
  auto showComputing = [&]() {
    std::ostringstream os;
    putAt(os, top + (showTimeRow ? 4 : 3), left);
    os << kEscReset << kEscBgBlack << kEscFgCyan << "\xe2\x94\x82" << kEscBgBlack << kEscFgWhite;
    static const std::string_view msg = " Computing viewport stats\xe2\x80\xa6";
    os << msg;
    pad(os, interiorW - utf8Width(std::string(msg)));
    os << kEscReset << kEscBgBlack << kEscFgCyan << "\xe2\x94\x82" << kEscReset;
    const std::string s = os.str();
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fflush(stdout);
  };

  // Translate a screen cell column to a series index, or -1 if outside the
  // chart's horizontal span. The chart starts at left + 1 + chartLeftPad
  // (after the box border and Y-axis label area) and is `chartW` cells wide.
  auto cellToIdx = [&](int cellX) -> int {
    const int rel = cellX - (left + 1 + chartLeftPad);
    if (rel < 0 || rel >= chartW) return -1;
    if (n <= 1) return 0;
    return std::clamp(rel * (n - 1) / std::max(1, chartW - 1), 0, n - 1);
  };

  while (true)
  {
    renderFrame(idx);
    // When animating, wait at most animationDelayMs ms for input; if no
    // key arrives, advance the marker one step (wrapping at the end).
    int ch = ERR;
    if (animating && animationDelayMs != nullptr)
    {
      wtimeout(itsStatusWin, *animationDelayMs);
      ch = wgetch(itsStatusWin);
      wtimeout(itsStatusWin, -1);
    }
    else
    {
      ch = wgetch(itsStatusWin);
    }
    auto invokeChange = [&]() {
      if (onTimeChange) onTimeChange(idx);
    };
    if (ch == ERR)
    {
      // Animation tick: advance and wrap.
      const int n = static_cast<int>(series.size());
      idx = (idx + 1) % n;
      invokeChange();
      continue;
    }
    if (ch == KEY_LEFT)
    {
      if (idx > 0)
      {
        idx -= 1;
        invokeChange();
      }
      continue;
    }
    if (ch == KEY_RIGHT)
    {
      if (idx + 1 < static_cast<int>(series.size()))
      {
        idx += 1;
        invokeChange();
      }
      continue;
    }
    if (ch == KEY_HOME)
    {
      if (idx != 0)
      {
        idx = 0;
        invokeChange();
      }
      continue;
    }
    if (ch == KEY_END)
    {
      if (idx != static_cast<int>(series.size()) - 1)
      {
        idx = static_cast<int>(series.size()) - 1;
        invokeChange();
      }
      continue;
    }
    if (ch == ' ' && animationDelayMs != nullptr)
    {
      animating = !animating;
      continue;
    }
    if (ch == KEY_UP && animationDelayMs != nullptr)
    {
      // Same scaling as App's outside-popup KEY_UP / KEY_DOWN.
      *animationDelayMs = std::max(50, static_cast<int>(*animationDelayMs * 0.7));
      continue;
    }
    if (ch == KEY_DOWN && animationDelayMs != nullptr)
    {
      *animationDelayMs = std::min(2000, static_cast<int>(*animationDelayMs / 0.7));
      continue;
    }
    if (ch == 's' || ch == 'S')
    {
      // Toggle the viewport-stats overlay. First toggle pulls the data
      // from the App-side cache (computeStats); subsequent toggles just
      // hide/show — the popup keeps its own copy alive.
      if (!statsAttempted && computeStats)
      {
        statsAttempted = true;
        showComputing();
        stats = computeStats();
      }
      const bool hasAnyStats = !stats.empty();
      if (hasAnyStats)
      {
        statsVisible = !statsVisible;
        rebuildAxis();
        rebuildLabels();
        rebuildGrids();
      }
      continue;
    }
    if (ch == KEY_MOUSE)
    {
      MEVENT ev;
      if (getmouse(&ev) != OK) continue;
      const int hitIdx = cellToIdx(ev.x);
      const bool inChart = hitIdx >= 0 && ev.y >= top + chartTopOffset &&
                           ev.y < top + chartTopOffset + chartH;
      // Some terminals deliver BUTTON1_PRESSED + BUTTON1_RELEASED for a click;
      // others deliver only BUTTON1_CLICKED (atomic). Treat both as a click.
      // Drag scrubbing only works under the press/release model — CLICKED is
      // a single event with no motion phase.
      const bool pressed = (ev.bstate & BUTTON1_PRESSED) != 0U;
      const bool clicked = (ev.bstate & BUTTON1_CLICKED) != 0U;
      if (pressed || clicked)
      {
        if (inChart)
        {
          if (pressed) dragging = true;
          if (idx != hitIdx)
          {
            idx = hitIdx;
            invokeChange();
          }
          continue;
        }
        // Click outside the chart but on the map area: report the click cell
        // so the caller can re-probe at the new location. Clicks landing
        // outside the map area are ignored — only the keyboard closes.
        const auto& l = itsLayout.map;
        const bool onMap = l.width > 0 && l.height > 0 && ev.x >= l.col &&
                           ev.x < l.col + l.width && ev.y >= l.row &&
                           ev.y < l.row + l.height;
        if (!onMap) continue;
        if (outClickRow) *outClickRow = ev.y;
        if (outClickCol) *outClickCol = ev.x;
        break;
      }
      if ((ev.bstate & BUTTON1_RELEASED) != 0U)
      {
        dragging = false;
        continue;
      }
      if (dragging && hitIdx >= 0 && hitIdx != idx)
      {
        // Mouse moved while held: scrub the timeline live.
        idx = hitIdx;
        invokeChange();
        continue;
      }
      // Other mouse events (other buttons, motion without drag) are
      // ignored so the popup doesn't close on stray hover reports.
      continue;
    }
    // Any other key dismisses.
    break;
  }

  touch();
  return idx;
}
}  // namespace Qdless
