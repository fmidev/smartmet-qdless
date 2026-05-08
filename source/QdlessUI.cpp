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

void UI::drawStatusBar()
{
  werase(itsStatusWin);
  // Layout: [Q]uit  [P]aram  [L]evel  Time ←→  Zoom +/-  Pan hjkl  [0]Reset  [?]Help
  int x = 1;
  auto put = [&](const std::string& s, int hotPos) {
    writeLabel(itsStatusWin, 0, x, s, hotPos);
    x += static_cast<int>(s.size()) + 2;
  };
  put("[Q]uit", 1);
  put("[P]aram", 1);
  put("[L]evel", 1);
  put("[G]Legend", 1);
  put("[N]Grid", 1);
  put("[W]ind", 1);
  put("[I]Cities", 1);
  put("[C]oast", 1);
  put("[B]orders", 1);
  put("[E]xport", 1);
  put("[/]Search", 1);
  put("[X]Section", 1);
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
    const int interiorW = width - 2;

    constexpr int kFooterRows = 1;
    const int maxRows = std::max(1, LINES - 8 - kFooterRows);
    const int innerH = std::min(static_cast<int>(matches.size()), maxRows);
    const int height = innerH + 4 + kFooterRows;  // top + query + sep + body + footer + bottom
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

    // Body rows.
    for (int i = 0; i < innerH; ++i)
    {
      putAt(os, top + 3 + i, left);
      os << kEscReset << kEscBgBlack << kEscFgCyan << "\xe2\x94\x82";
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
      os << kEscReset << kEscBgBlack << kEscFgCyan << "\xe2\x94\x82" << kEscReset;
    }
    // Pad empty body rows when fewer matches than slots.
    for (int i = innerH; i < std::max(1, std::min(static_cast<int>(matches.size() == 0 ? 1 : 0),
                                                  maxRows));
         ++i)
    {
      putAt(os, top + 3 + i, left);
      os << kEscReset << kEscBgBlack << kEscFgCyan << "\xe2\x94\x82" << kEscBgBlack
         << kEscFgWhite;
      pad(os, interiorW);
      os << kEscBgBlack << kEscFgCyan << "\xe2\x94\x82" << kEscReset;
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
      {"",                          ""},
      {"g",                         "Legend"},
      {"c",                         "Toggle coastlines"},
      {"b",                         "Toggle borders"},
      {"n",                         "Toggle lat/lon graticule"},
      {"w",                         "Toggle wind arrows"},
      {"i",                         "Toggle city overlay"},
      {"PgUp PgDn",                 "Cities: sparser / denser"},
      {"/",                         "Place search"},
      {"x",                         "Cross-section"},
      {"e",                         "Export PNG"},
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
                        const std::vector<float>& series, int currentIndex,
                        const Renderer& renderer, const Palette& palette,
                        std::function<void(int)> onTimeChange, int avoidCellRow,
                        int avoidCellCol)
{
  (void)palette;  // reserved for future colour-by-band line rendering
  if (series.empty()) return currentIndex;

  // Compute value range over finite samples.
  float dataLo = std::numeric_limits<float>::infinity();
  float dataHi = -std::numeric_limits<float>::infinity();
  int finiteCount = 0;
  for (float v : series)
    if (finiteValue(v))
    {
      dataLo = std::min(dataLo, v);
      dataHi = std::max(dataHi, v);
      ++finiteCount;
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
  // Compute "nice" axis bounds and tick step (1, 2 or 5 × 10^k) so the Y
  // axis labels are clean integers / decimals rather than raw extrema.
  const int desiredLabels = 5;
  const double tickStep = niceStep(static_cast<double>(dataHi - dataLo), desiredLabels);
  const double niceMin = std::floor(dataLo / tickStep) * tickStep;
  const double niceMax = std::ceil(dataHi / tickStep) * tickStep;
  const float vmin = static_cast<float>(niceMin);
  const float vmax = static_cast<float>(niceMax);
  // Decimals based on the step's magnitude.
  const int stepExp = static_cast<int>(std::floor(std::log10(tickStep)));
  const int decimals = std::max(0, -stepExp);

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
  auto fmtAxis = [decimals](double v) {
    std::string tmp = fmt::format("{:.{}f}", v, decimals);
    // Avoid "-0" output.
    if (tmp == std::string("-") + std::string(decimals + 1, '0').replace(1, 1, "."))
      tmp = fmt::format("{:.{}f}", 0.0, decimals);
    return tmp;
  };
  int labelW = std::max(utf8Width(fmtAxis(niceMin)), utf8Width(fmtAxis(niceMax)));
  labelW = std::max(labelW, 3);
  const int chartLeftPad = labelW + 3;  // " <label> ┤"

  // Build the tick set and map each tick to its chart row.
  std::vector<bool> hasLabel(chartH, false);
  std::vector<double> labelValue(chartH, 0);
  if (niceMax > niceMin)
  {
    for (double t = niceMin; t <= niceMax + tickStep * 0.5; t += tickStep)
    {
      const double frac = (niceMax - t) / (niceMax - niceMin);  // 0 at top
      const int row =
          std::clamp(static_cast<int>(std::lround(frac * (chartH - 1))), 0, chartH - 1);
      hasLabel[row] = true;
      labelValue[row] = t;
    }
  }

  const int width = std::max({chartW + chartLeftPad + 4,
                              utf8Width(paramName) + 6,
                              utf8Width(latlonBuf) + 6,
                              40});
  const int height = chartH + 6;  // border + 3 header rows + chart + footer + border
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

  // Build the braille bitmask grid.
  const int subW = chartW * 2;
  const int subH = chartH * 4;
  std::vector<unsigned> grid(static_cast<std::size_t>(chartW) * chartH, 0U);

  auto setDot = [&](int sx, int sy) {
    if (sx < 0 || sx >= subW || sy < 0 || sy >= subH) return;
    const int cx = sx / 2;
    const int cy = sy / 4;
    grid[static_cast<std::size_t>(cy) * chartW + cx] |= 1U << brailleBit(sx % 2, sy % 4);
  };

  auto valueToSubY = [&](float v) -> int {
    const float yn = (v - vmin) / (vmax - vmin);
    return std::clamp(static_cast<int>(std::lround((1.0F - yn) * (subH - 1))), 0, subH - 1);
  };

  // Render the line by walking pairs of consecutive samples and connecting
  // them with a Bresenham line in sub-cell coordinates.
  const int n = static_cast<int>(series.size());
  auto sampleSubX = [&](int idx) {
    return (n > 1) ? (idx * (subW - 1) / (n - 1)) : (subW / 2);
  };

  int prevSx = -1;
  int prevSy = -1;
  bool prevValid = false;
  for (int i = 0; i < n; ++i)
  {
    if (!finiteValue(series[i]))
    {
      prevValid = false;
      continue;
    }
    const int sx = sampleSubX(i);
    const int sy = valueToSubY(series[i]);
    if (prevValid)
    {
      // Bresenham between (prevSx, prevSy) and (sx, sy).
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
        setDot(x0, y0);
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
      setDot(sx, sy);
    }
    prevSx = sx;
    prevSy = sy;
    prevValid = true;
  }

  // Helper to render one full popup frame with a given marker index.
  auto renderFrame = [&](int markerIdx) {
    // Vertical marker at the current time step.
    int markerSubX = -1;
    if (markerIdx >= 0 && markerIdx < n) markerSubX = sampleSubX(markerIdx);

    // "now" value depends on marker.
    const float curVal =
        (markerIdx >= 0 && markerIdx < static_cast<int>(series.size()))
            ? series[markerIdx]
            : std::numeric_limits<float>::quiet_NaN();
    const std::string rangeBuf =
        finiteValue(curVal)
            ? fmt::format("min {:.{}f}  max {:.{}f}  step {}/{}  now {:.{}f}", dataLo, decimals,
                          dataHi, decimals, markerIdx + 1, static_cast<int>(series.size()),
                          curVal, decimals)
            : fmt::format("min {:.{}f}  max {:.{}f}  step {}/{}  now -", dataLo, decimals, dataHi,
                          decimals, markerIdx + 1, static_cast<int>(series.size()));

    std::ostringstream os;

    // Top border.
    putAt(os, top, left);
    os << kEscReset << kEscBgBlack << kEscFgCyan << "\xe2\x94\x8c";
    for (int i = 0; i < width - 2; ++i) os << "\xe2\x94\x80";
    os << "\xe2\x94\x90" << kEscReset;

    auto writeRow = [&](int rowOffset, std::string_view label, bool bold = false) {
      putAt(os, top + rowOffset, left);
      os << kEscReset << kEscBgBlack << kEscFgCyan << "\xe2\x94\x82" << kEscBgBlack
         << kEscFgWhite;
      if (bold) os << kEscBold;
      os << ' ' << label;
      pad(os, interiorW - 1 - utf8Width(label));
      os << kEscReset << kEscBgBlack << kEscFgCyan << "\xe2\x94\x82" << kEscReset;
    };

    writeRow(1, paramName, true);
    writeRow(2, latlonBuf, false);
    writeRow(3, rangeBuf, false);

    // Chart rows.
    for (int cy = 0; cy < chartH; ++cy)
    {
      putAt(os, top + 4 + cy, left);
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

      // Chart cells.
      for (int cx = 0; cx < chartW; ++cx)
      {
        const bool inMarker = (markerSubX >= 0 && (markerSubX / 2) == cx);
        const unsigned mask = grid[static_cast<std::size_t>(cy) * chartW + cx];
        if (inMarker && mask == 0)
        {
          os << kEscFgRed << "\xe2\x94\x82" << kEscFgWhite;
        }
        else if (mask == 0)
        {
          os << ' ';
        }
        else
        {
          if (inMarker) os << kEscFgRed;
          os << brailleGlyph(mask);
          if (inMarker) os << kEscFgWhite;
        }
      }
      // Right-side padding inside the box.
      pad(os, interiorW - chartLeftPad - chartW);
      os << kEscReset << kEscBgBlack << kEscFgCyan << "\xe2\x94\x82" << kEscReset;
    }

    writeRow(4 + chartH, "\xe2\x86\x90\xe2\x86\x92 step time   any other key closes", false);

    putAt(os, top + height - 1, left);
    os << kEscReset << kEscBgBlack << kEscFgCyan << "\xe2\x94\x94";
    for (int i = 0; i < width - 2; ++i) os << "\xe2\x94\x80";
    os << "\xe2\x94\x98" << kEscReset;

    const std::string s = os.str();
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fflush(stdout);
  };

  int idx = std::clamp(currentIndex, 0, static_cast<int>(series.size()) - 1);

  while (true)
  {
    renderFrame(idx);
    int ch = wgetch(itsStatusWin);
    auto invokeChange = [&]() {
      if (onTimeChange) onTimeChange(idx);
    };
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
    // Any other key dismisses.
    break;
  }

  touch();
  return idx;
}
}  // namespace Qdless
