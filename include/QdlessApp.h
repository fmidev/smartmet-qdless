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
  std::string parameterOverride;  // empty -> use first parameter
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
  Palette palette;
  float valueScale = 1.0F;
  float valueOffset = 0.0F;
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

  // Display panels. Always non-empty; today size() == 1 (single full-screen
  // panel). Side-by-side and 2x2 layouts will grow this vector. The active
  // panel receives parameter / level / palette / probe commands.
  std::vector<Panel> itsPanels;
  int itsActivePanel = 0;
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
