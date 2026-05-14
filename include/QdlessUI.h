#pragma once

#include "QdlessPalette.h"
#include "QdlessRenderer.h"

#include <ncurses.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Qdless
{
struct Rect
{
  int row = 0;
  int col = 0;
  int height = 0;
  int width = 0;
};

struct Layout
{
  Rect map;     // raw-ANSI map area
  Rect time;    // 2 rows: label + bar
  Rect status;  // 1 row at bottom
};

class UI
{
 public:
  UI();
  ~UI();
  UI(const UI&) = delete;
  UI& operator=(const UI&) = delete;
  UI(UI&&) = delete;
  UI& operator=(UI&&) = delete;

  Layout layout() const { return itsLayout; }
  void recomputeLayout();

  // Wait for a key. timeoutMs < 0 = block forever; 0 = poll; > 0 = block
  // up to that many ms then return ERR. Returns ncurses key code or ERR.
  int waitInput(int timeoutMs = -1);

  void drawTimeline(const std::string& label, int idx, int total);
  // Status-bar layout shifts based on the source kind:
  //   imageMode   — naked image (PNG/WebP/...): drop param/level/
  //                 legend/projection-dependent overlays/probe.
  //   shapeMode   — shapefile / PostGIS layer: keep the full bar and
  //                 add [O]utlines + [R]ainbow + [A]ttrs.
  //   pgMode      — PostGIS connection: also offer [D]Tables to
  //                 re-open the layer picker.
  //   browseMode  — --dir tree mode: offer [D]Browse to re-open the
  //                 PNG-tree picker. Coexists with imageMode (the
  //                 picked leaf loads as a multi-file image source).
  // imageMode wins when both image and shape flags are set.
  void drawStatusBar(bool imageMode = false, bool shapeMode = false,
                     bool pgMode = false, bool browseMode = false);


  // Re-blank ncurses windows (after popup close). Caller redraws map.
  void touch();

  // Centred popup menu. Returns 0-based index, or -1 if Esc cancelled.
  // Items beyond hotkey-able count are reachable via arrow keys + Enter.
  // When `allowTab` is true, pressing Tab returns kPopupSearchTab so the
  // caller can switch to a sibling view (matches popupSearch).
  int popupMenu(const std::string& title, const std::vector<std::string>& items,
                int currentIndex, bool allowTab = false);

  // Centred legend popup showing palette colour swatches and value ranges.
  // The two title strings appear on two stacked rows so the popup can stay
  // narrow. Dismissed by any key.
  void popupLegend(const std::string& paramName, const std::string& paletteName,
                   const Palette& palette, const Renderer& renderer);

  // Centred help popup listing key bindings and mouse gestures.
  // Dismissed by any key.
  // Help popup. The context flags hide entries that don't apply to
  // the current source so the help reads as a contextually relevant
  // cheat sheet rather than an exhaustive list. Defaults match the
  // gridded data path (the original full help).
  struct HelpContext
  {
    bool isImage = false;        // raw image: no projection / no time
    bool isShape = false;        // shapefile / PostGIS: no time / no probe
    bool isPg = false;           // PostGIS connection: show [D]Tables
    bool hasTimeAxis = true;     // false when timeCount() <= 1
    bool hasMultipleParams = true;
    bool hasMultipleLevels = true;
  };
  void popupHelp(HelpContext ctx);
  void popupHelp();  // gridded-data default — calls popupHelp({}) internally

  // Centred metadata popup listing (label, value) rows. Used to display
  // file-level info: filename, format, grid type, dimensions, parameter
  // / level / time counts, lat/lon extent, etc. An empty (label, value)
  // pair renders as a blank separator row. Dismissed by any key.
  // Returns the (x, y) cell coordinates if the user clicked somewhere
  // OUTSIDE the popup — caller can use that to re-probe the new
  // location (chains attribute-popup hops on a vector source). Returns
  // nullopt for any other dismissal.
  struct PopupClick
  {
    int x;
    int y;
  };
  std::optional<PopupClick> popupMetadata(
      const std::string& title,
      const std::vector<std::pair<std::string, std::string>>& rows);

  // Live-filter search popup. The matcher is invoked on every keystroke
  // with the current query string and must return formatted display rows.
  // Returns the 0-based index into the matcher's last result, or -1 on
  // cancel (Esc). When `allowTab` is true, pressing Tab closes the popup
  // and returns kPopupSearchTab so the caller can switch to a sibling
  // view (e.g. a column-navigator picker that shares the same dataset).
  static constexpr int kPopupSearchTab = -2;
  int popupSearch(const std::string& title,
                  std::function<std::vector<std::string>(const std::string&)> matcher,
                  const std::string& header = {}, bool allowTab = false);

  // Min/mean/max series across the visible viewport, one float per time
  // step, used as a translucent overlay on popupTimeseries when 's' is
  // pressed inside the popup. Populated by App::ensureViewportStats and
  // copied into the popup; the popup itself doesn't know how the stats
  // were computed.
  struct StatsSeries
  {
    std::vector<float> min;
    std::vector<float> mean;
    std::vector<float> max;
    bool empty() const { return min.empty() && mean.empty() && max.empty(); }
  };

  // Timeseries probe popup: shows a braille-sparkline of `series` at the
  // given lat/lon, with a vertical marker at currentIndex and numeric Y-axis
  // labels along the left edge.
  //
  // Left/Right (and Home/End) arrows step the marker; on each step
  // `onTimeChange(newIdx)` is invoked so the caller can update the time on
  // the underlying map while the popup stays visible. Space toggles
  // animation: when active, the marker auto-advances every
  // `animationDelayMs` and wraps; Up / Down adjust that delay (and the
  // updated delay is written back through the pointer so the App's
  // outside-popup animation picks up the same speed). 's' toggles a stats
  // overlay (min/mean/max curves across the viewport) — on first toggle
  // `computeStats` is invoked to fetch the data; subsequent toggles just
  // hide / show the cached series. Any other keyboard key dismisses the
  // popup. A mouse click outside the chart but on the map area is reported
  // back via `outClickRow` / `outClickCol` (if non-null) so the caller can
  // re-probe at that cell; clicks elsewhere are ignored. Returns the final
  // time index.
  // `timeLabels[i]` is the human-readable time for series step i; if
  // empty, no time row is drawn.
  // `units` is appended to numeric values in the info row (e.g. "K",
  // "%"). Empty string means no unit shown.
  // `animationDelayMs` (nullable): when non-null and Space is pressed,
  // animation runs at *animationDelayMs ms/frame; Up/Down keys adjust
  // it. Pass nullptr to disable in-popup animation.
  // `avoidCellRow` / `avoidCellCol` (-1 = ignore): if both ≥ 0, the popup
  // shifts into the opposite quadrant so a map marker at that cell stays
  // visible behind the popup.
  int popupTimeseries(const std::string& paramName, double lat, double lon,
                      const std::vector<float>& series,
                      const std::vector<std::string>& timeLabels, int currentIndex,
                      const Renderer& renderer, const Palette& palette,
                      std::function<void(int)> onTimeChange = {},
                      std::function<StatsSeries()> computeStats = {},
                      const std::string& units = {},
                      int* animationDelayMs = nullptr,
                      int avoidCellRow = -1, int avoidCellCol = -1,
                      int* outClickRow = nullptr, int* outClickCol = nullptr);

 private:
  void writeLabel(WINDOW* w, int y, int x, const std::string& label, int hotPos);

  WINDOW* itsTimeWin = nullptr;
  WINDOW* itsStatusWin = nullptr;
  Layout itsLayout;

  // Combined ncurses attribute masks (color pair + A_BOLD/etc).
  chtype itsBaseAttr = 0;
  chtype itsHotAttr = 0;
  chtype itsSelAttr = 0;
  chtype itsBoxAttr = 0;
  chtype itsPopupAttr = 0;
};
}  // namespace Qdless
