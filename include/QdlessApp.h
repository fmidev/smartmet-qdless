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
  Palette itsPalette;
  Renderer itsRenderer;
  std::vector<Polyline> itsCoastlines;
  std::vector<Polyline> itsBorders;
  std::string itsCoastlinePath;  // currently loaded shoreline file
  std::string itsBorderPath;     // currently loaded border file

  // Available parameters (newbase numeric IDs), in file order.
  std::vector<int> itsParamIds;
  int itsParamIndex = 0;

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

  // Time-series popup text zoom: 1 = native font, 2 = DECDHL doubled
  // (terminals that honour ESC#3 / ESC#4 render the popup at 2× width
  // and 2× height so text stays legible at very small font sizes).
  int itsTextZoom = 1;

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

  // Optional value transform applied to each sampled value before palette
  // lookup. Used to auto-shift Kelvin → Celsius so K-unit data picks up the
  // temperature palette correctly. value' = value * scale + offset.
  float itsValueScale = 1.0F;
  float itsValueOffset = 0.0F;
  // Helper: apply transform, but keep missing/sentinel as-is.
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
