#pragma once

#include "QdlessCities.h"
#include "QdlessCoastline.h"
#include "QdlessDataSource.h"
#include "QdlessPalette.h"
#include "QdlessRenderer.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Qdless
{
struct Options
{
  std::string filename;
  std::string paletteDir = "/usr/share/smartmet/qdless/palettes";
  std::string configFile = "/usr/share/smartmet/qdless/qdless.conf";
  std::string coastlineDir = "/usr/share/gshhg-gmt-nc4";
  // Parameters to launch with. Empty -> use first parameter in the file.
  // 1 entry -> Single layout. 2 -> Side. 3 or 4 -> Quad. >4 is rejected.
  // For 3 parameters in Quad layout, the 4th panel clones the first.
  std::vector<std::string> parameterOverrides;
  // Optional layout override ("single", "side", "quad"). When empty, the
  // layout is picked from parameterOverrides.size(). When set, must hold
  // at least as many panels as parameterOverrides.
  std::string layoutOverride;
  std::string paletteOverride;    // empty -> resolve via config / built-in
  int timeIndex = 0;              // 0-based; -1 means last
  int levelIndex = 0;             // 0-based; -1 means last
  bool noCoastline = false;
  bool noBorders = false;
  double minLakeAreaKm2 = 3000.0;     // keep Ladoga, Vänern, Saimaa-by-area
  double minLakeRoundness = 0.15;     // drop fractal shores (Saimaa ~0.05)
  double minIslandAreaKm2 = 10.0;     // drop sub-10 km² islets (Åland noise)
  bool dumpAndExit = false;       // print one frame to stdout and exit (no curses)
};

// Per-panel state. Today there is always exactly one panel; the field set
// is structured so the upcoming side-by-side and 2x2 layouts can extend the
// vector without further plumbing changes. `paramIndex` is an index into
// `App::itsParamIds`; `palette` is resolved per-panel via the same logic as
// the single-panel case (config lookup → built-in ramp). `valueScale` /
// `valueOffset` carry the auto Kelvin → Celsius (etc.) shift produced by
// `guessFromUnits` for that panel's parameter.
struct Panel
{
  int paramIndex = 0;
  std::size_t levelIndex = 0;
  Palette palette;
  float valueScale = 1.0F;
  float valueOffset = 0.0F;
};

// Sub-rectangle of cell coordinates inside the map area. Width/height in
// terminal cells; row/col are absolute screen positions.
struct PanelRect
{
  int row = 0;
  int col = 0;
  int height = 0;
  int width = 0;
};

// Display tiling. Panels share viewport / time / overlays; only parameter,
// level, and palette differ. Cycled with F2.
enum class PanelLayout
{
  Single,  // 1 panel — full map area
  Side,    // 2 panels, side-by-side (meteorologist classic)
  Quad,    // 2x2 grid
};

// Sub-rectangle of NFmiArea coordinates we are currently displaying.
struct Viewport
{
  float uMin = 0;
  float uMax = 1;
  float vMin = 0;
  float vMax = 1;

  void reset();
  void zoom(float factor);  // <1 zooms in, >1 zooms out
  void zoomAt(float factor, float anchorU, float anchorV);  // anchor stays fixed
  void pan(float duFrac, float dvFrac);
  void clamp();
};

class UI;  // forward

class App
{
 public:
  explicit App(Options opts);
  ~App();
  App(const App&) = delete;
  App& operator=(const App&) = delete;
  App(App&&) = delete;
  App& operator=(App&&) = delete;

  // One-shot dump (--dump): writes one frame to stdout and returns.
  int runOnce();
  // Interactive ncurses event loop (default).
  int runInteractive();

 private:
  void buildIndices();
  void loadPalette();
  void loadCoastlines();

  std::vector<Rgb> sampleSlice(int subWidth, int subHeight, float& dataMin,
                               float& dataMax) const;
  void overlayPolylines(std::vector<Rgb>& pixels, int subWidth, int subHeight,
                        const std::vector<Polyline>& polylines, Rgb color) const;

  // Interactive helpers:
  void selectParam(int newIndex);
  void selectLevel(int newIndex);
  void cyclePanelLayout();
  // Resize / re-fill itsPanels for `layout`. New panels are clones of the
  // active panel with paramIndex rotated by their position in the vector.
  void setPanelLayout(PanelLayout layout);
  // Make `idx` the active panel (no-op if out of range). Restores the
  // source's (paramId, level) selection to the new active panel so probe /
  // legend / cross-section see consistent state.
  void setActivePanel(int idx);
  void cycleActivePanel(int step);  // +1 next, -1 previous

  // Compute panel sub-rectangles for the current itsPanelLayout inside the
  // given map area (row, col, height, width).
  std::vector<PanelRect> currentPanelRects(int row, int col, int height, int width) const;
  // Map a screen cell to a panel index (or nullopt if the cell is on a
  // gutter or outside the map area).
  std::optional<int> panelAtCell(const UI& ui, int cellX, int cellY) const;
  // Returns true if a redraw is needed; sets quit=true on quit.
  bool handleKey(int key, UI& ui, bool& quit);
  void drawMap(UI& ui);
  std::string currentTimeLabel() const;
  std::string originTimeLabel() const;
  // Build the timeline header label and push it to the ncurses windows.
  // Used by the main draw loop and by the time-series probe so the bottom
  // panel updates live as the user steps through times inside the popup.
  void renderTimeline(UI& ui);
  std::vector<std::string> paramLabels() const;
  std::vector<std::string> levelLabels() const;

  Options itsOpts;
  std::unique_ptr<DataSource> itsSource;
  Renderer itsRenderer;
  std::vector<Polyline> itsCoastlines;
  std::vector<Polyline> itsBorders;
  std::string itsCoastlinePath;  // currently loaded shoreline file
  std::string itsBorderPath;     // currently loaded border file

  // Available parameters (newbase numeric IDs), in file order.
  std::vector<int> itsParamIds;

  // Display panels. Always non-empty; size depends on itsPanelLayout
  // (1, 2, or 4). The active panel receives parameter / level / palette /
  // probe commands.
  std::vector<Panel> itsPanels;
  int itsActivePanel = 0;
  PanelLayout itsPanelLayout = PanelLayout::Single;
  Panel& activePanel() { return itsPanels[itsActivePanel]; }
  const Panel& activePanel() const { return itsPanels[itsActivePanel]; }

  Viewport itsViewport;

  // Mouse drag state (cell coordinates at button-down).
  bool itsDragging = false;
  int itsDragStartX = 0;
  int itsDragStartY = 0;

  // Cross-section pending picks: 0 = inactive, 2 = waiting first click,
  // 1 = waiting second click. Stored coords are the first endpoint.
  int itsCrossPicks = 0;
  int itsCrossX1 = 0;
  int itsCrossY1 = 0;

  // Animation state.
  bool itsAnimating = false;
  int itsAnimationDelayMs = 250;

  // Overlay toggles.
  bool itsShowGraticule = false;
  bool itsShowWindArrows = false;
  bool itsShowCities = false;
  // Top-N cap for the cities overlay; PageUp / PageDown step through fixed
  // levels (5, 10, 25, 50, 100, 250, 500). Default = a comfortable mid value.
  int itsCityOverlayN = 25;

  // Transient status message shown on the timeline header for one redraw
  // (e.g. "Saved foo.png"). Cleared by the next non-message-producing key.
  std::string itsLastMessage;

  // Helper: apply the active panel's value transform (auto Kelvin → Celsius
  // etc.) before palette lookup, but keep missing/sentinel as-is.
  float transform(float v) const;

  // City lookup index, lazily loaded on first '/' (place search).
  mutable CityIndex itsCityIndex;
  mutable bool itsCityIndexAttempted = false;

  // Helpers for mouse handling.
  bool cellToLatLon(const UI& ui, int cellX, int cellY, double& lat, double& lon) const;
  bool cellToViewport(const UI& ui, int cellX, int cellY, float& u, float& v) const;
  void zoomAt(float factor, float anchorU, float anchorV);
  void openProbe(int cellX, int cellY, UI& ui);
  void openProbeAt(double lat, double lon, UI& ui);

  // Overlays.
  void overlayGraticule(std::vector<Rgb>& pixels, int subWidth, int subHeight) const;
  void overlayMarker(std::vector<Rgb>& pixels, int subWidth, int subHeight) const;
  void overlayCities(std::vector<Rgb>& pixels, int subWidth, int subHeight) const;
  std::string buildWindArrows(int cellW, int cellH, int originRow, int originCol);
  std::string buildCityLabels(int cellW, int cellH, int originRow, int originCol);

  // Optional pin marker (lat, lon) drawn on the map; set by place search and
  // click-to-probe. Persists until the next set.
  std::optional<std::pair<double, double>> itsMarker;

  // Export current slice as a PNG. Returns the filename written, or empty
  // string on failure (with `err` set).
  std::string exportPng(std::string& err) const;

  // Lazily load the cities database. Returns true if it's usable.
  bool ensureCityIndex() const;
  // Open the place-search popup; on selection recentre the viewport on the
  // city's lat/lon (zoomed to a regional view).
  void openPlaceSearch(UI& ui);

  // Render a cross-section popup of the current parameter from (x1,y1) to
  // (x2,y2) in cell coords, sampled across all levels.
  void renderCrossSection(int x1, int y1, int x2, int y2, UI& ui);
};
}  // namespace Qdless
