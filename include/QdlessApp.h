#pragma once

#include "QdlessCities.h"
#include "QdlessCoastline.h"
#include "QdlessDataSource.h"
#include "QdlessExtrema.h"
#include "QdlessPalette.h"
#include "QdlessGraphics.h"
#include "QdlessRenderer.h"

#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Qdless
{
// Forward declaration so draw3D / draw3DQueryData can accept a Layout
// without QdlessApp.h needing to drag in QdlessUI.h (which pulls in
// ncurses and panels). Full definition is in QdlessUI.h, included by
// QdlessApp.cpp.
struct Layout;

struct Options
{
  // Single-file convenience. Either `filename` or `filenames` (multi) is
  // populated by the CLI parser; main fills `filename` from `filenames[0]`
  // when there's exactly one input so single-file callers don't need to
  // change. The App constructor uses `filenames` if non-empty.
  std::string filename;
  std::vector<std::string> filenames;
  // PostGIS browser mode. When `pgConn` is non-empty the App opens a
  // PostgreSQL connection through OGR's "PG:" driver and presents a
  // layer picker; `pgSchema` (optional) restricts the picker to one
  // schema; `pgTable` (optional, "schema.table" form) skips the
  // picker and opens that table directly.
  std::string pgConn;
  std::string pgSchema;
  std::string pgTable;
  // PNG-tree browser mode. When `browseRoot` is non-empty the App walks
  // the directory tree at startup, lists every leaf directory that
  // directly contains *.png files, and presents a picker. Set by
  // `--dir <root>` when the argument does not contain image files
  // directly (so the existing flat --dir behaviour is preserved when
  // the user points at a single animation directory).
  std::string browseRoot;
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
  std::string paletteOverride;  // empty -> resolve via config / built-in
  int timeIndex = 0;            // 0-based; -1 means last
  int levelIndex = 0;           // 0-based; -1 means last
  bool noCoastline = false;
  bool noBorders = false;
  double minLakeAreaKm2 = 3000.0;  // keep Ladoga, Vänern, Saimaa-by-area
  double minLakeRoundness = 0.15;  // drop fractal shores (Saimaa ~0.05)
  double minIslandAreaKm2 = 10.0;  // drop sub-10 km² islets (Åland noise)
  bool dumpAndExit = false;        // print one frame to stdout and exit (no curses)
  bool start3D = false;            // start in 3D point-cloud mode (e.g. --dump --3d)
  bool startGlobe = false;         // start in globe view (e.g. --dump --globe)
  bool dumpExtrema = false;        // --extrema: print persistent 3D extrema and exit
  bool noExitEffect = false;       // skip the random quit animation
  // Text spelled out by the word-reveal exit effect. Empty (the default) means
  // the effect draws a random closing line from its built-in anthology;
  // --exit-message overrides it with your own text.
  std::string exitMessage;
  // Restricts which exit effects play on quit: a comma-separated list of
  // effect names (see --list-exit-effects). One name pins that effect; several
  // pick at random among them. Empty (default) = any of them. Names match
  // case/spacing/punctuation-insensitively; unknown names are ignored.
  std::string exitEffects;
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
  // For parameters that exist on multiple level types in the file
  // (e.g. Temperature on both pressure and hybrid in the same GRIB), the
  // active group is remembered per panel so cycling panels or switching
  // back to a parameter restores the user's choice.
  int levelGroupIdx = 0;
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

// Coastline / border render style. Cycled with `c` / `b`.
enum class LineStyle
{
  Braille,  // thin braille overlay on top of the rendered data (default)
  Thick,    // rasterised into the data pixel buffer (half-cell-wide quads)
  None,     // not drawn
};

// Sub-rectangle of NFmiArea coordinates we are currently displaying.
struct Viewport
{
  float uMin = 0;
  float uMax = 1;
  float vMin = 0;
  float vMax = 1;

  void reset();
  void zoom(float factor);                                  // <1 zooms in, >1 zooms out
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
  // Capture the current view as a full-screen raster and play a quit animation
  // over it (see QdlessExitEffect). effectIndex < 0 picks one at random and
  // honours --no-exit-effect (the real quit path); a concrete index forces
  // that effect (preview / repeat / menu keys). seed == 0 picks a fresh seed;
  // nonzero reproduces a prior run. wordsOverride, when non-empty, is the text
  // fed to the word-reveal effect; empty falls back to itsOpts.exitMessage.
  // Records what was played in itsLastExit{Index,Seed} for the repeat key.
  void playExitEffect(int effectIndex = -1,
                      unsigned seed = 0,
                      const std::string& wordsOverride = {});
  int itsExitEffectPreview = 0;  // next effect the cycle key (F9) shows
  int itsExitThemePreview = 0;   // F8 two-step picker: theme cursor (remembered across opens)
  int itsExitWordPreview = 0;    // current word-reveal line for F11 / F12
  int itsLastExitIndex = -1;     // last effect played, for the repeat key (F10)
  unsigned itsLastExitSeed = 0;  // its seed; 0 = nothing played yet
  // Most recent raster composed by a 3D renderer (point cloud, at map origin),
  // cached so playExitEffect can animate a 3D view too. The 2D path re-samples
  // via sampleSlice instead and leaves this untouched.
  std::vector<Rgb> itsLast3DRaster;
  int itsLast3DRasterW = 0;
  int itsLast3DRasterH = 0;
  void cache3DRaster(const std::vector<Rgb>& pixels, int subW, int subH);

  void buildIndices();
  void loadPalette();
  // Resolve a palette by name through the same candidate-directory
  // search loadPalette() uses. Returns std::nullopt if no candidate
  // file existed or could be parsed. Used by draw3DSurfaceStack to
  // pick a different palette per layer (precip / fog / cloud) without
  // disturbing the active panel's palette.
  std::optional<Palette> loadPaletteByName(const std::string& name) const;
  // subPixelsW / subPixelsH are the sub-pixel dimensions the coastline
  // will be rendered into (cellW*2, cellH*4 for Braille — the finest
  // path). Pass 0 to derive them from the current terminal size — used
  // by the startup path before the UI has been laid out.
  void loadCoastlines(int subPixelsW = 0, int subPixelsH = 0);

  std::vector<Rgb> sampleSlice(int subWidth, int subHeight, float& dataMin, float& dataMax) const;
  void overlayPolylines(std::vector<Rgb>& pixels,
                        int subWidth,
                        int subHeight,
                        const std::vector<Polyline>& polylines,
                        Rgb color) const;
  // Colour to draw the borders overlay in. Mid grey for GSHHS political
  // borders; black for shapefile outlines so they're visible against
  // the flat fill (which is itself a mid grey).
  Rgb borderColor() const;

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
  // Persistent cross-section overlay. When active, the popup is redrawn
  // each frame from the stored geographic endpoints so time-stepping
  // (left/right) and animation (Space) keep working with it open.
  bool itsCrossActive = false;
  double itsCrossLat1 = 0;
  double itsCrossLon1 = 0;
  double itsCrossLat2 = 0;
  double itsCrossLon2 = 0;
  // Chart-area rectangle of the current cross-section popup (cell coords).
  // Cached at drawCrossSection so mouse-motion events can test whether
  // the cursor is over the chart and translate column → distance along
  // the great-circle. Width is 0 when there is no popup on screen.
  int itsCrossChartRow = 0;
  int itsCrossChartCol = 0;
  int itsCrossChartW = 0;
  int itsCrossChartH = 0;
  // Lat/lon currently highlighted by the mouse over the cross-section
  // chart. Drawn as a dot on the map. Cleared when the mouse leaves
  // the chart or the popup closes.
  std::optional<std::pair<double, double>> itsCrossHoverLatLon;
  // When the source supports native height-sampling (PVOL): draw the
  // cross-section with Y-axis in km (true RHI). Toggled with [Y]. Falls
  // back to one row per elevation angle when false. Has no effect for
  // sources without hasNativeHeight().
  bool itsCrossHeightAxis = true;

  // 3D mode (radar volume): rotate-and-zoom view onto the polar data as
  // a point cloud above a coastline-and-graticule ground plane. Toggled
  // with [3]. Camera orbits the radar at (yaw, pitch); zoom scales the
  // viewport. Active for PVOL sources and for QueryData sources with a
  // height parameter (GeomHeight / GeopHeight) alongside multiple
  // levels; falls back to 2D for everything else.
  bool itsMode3D = false;
  double itsCamYaw = 0.0;         // radians; 0 = looking north from south
  double itsCamPitch = 0.6;       // radians; 0 = horizontal, π/2 = top-down
  double itsCamZoom = 1.0;        // 1.0 = data extent fits the viewport

  // Globe view (key [g]): orthographic 3D sphere with the data ray-cast onto
  // the near hemisphere, coastlines / borders / graticule projected on top.
  // Reuses itsCamYaw / itsCamPitch / itsCamZoom as the orbit camera, but here
  // itsCamYaw is the centre longitude and itsCamPitch the centre latitude (so
  // the poles can be brought into a natural view). Mutually exclusive with the
  // point-cloud / curtain 3D modes. Available for any gridded geographic
  // source — global data benefits most, but it also gives a distortion-free
  // polar view of arctic data.
  bool itsModeGlobe = false;
  float itsThreshold3D = -10.0F;  // drop bins below this threshold. Units
                                  // depend on the source: dBZ for PVOL
                                  // (default −10), percent for QueryData
                                  // cloud-cover-like fields, etc. , / .
                                  // raise the bar interactively. Reset to
                                  // a source-appropriate default each time
                                  // 3D mode is toggled on.
  double itsVexagger3D = 8.0;     // vertical exaggeration. Radar volumes
                                  // are ~30:1 wide:tall in real geometry;
                                  // at true scale the storm flattens to a
                                  // disc. Default 8× makes the storm body
                                  // visibly tall without distorting echo
                                  // *positions* (only their heights stretch).
                                  // NWP hybrid-level volumes are ~100:1
                                  // wide:tall; default lifts to ~50× when
                                  // entering 3D on a QueryData source.
  // Label of the threshold unit currently in effect. Drives the HUD
  // ("thresh=10dBZ" vs "thresh=10%") so the same key handlers serve
  // both backends without leaking source-specific text into draw3D*.
  std::string itsThreshold3DUnit = "dBZ";

  // Persistent-extrema overlay for the volumetric 3D view (toggle [x]). When
  // on, draw3DQueryData replaces the full point cloud with just the most
  // persistent anomaly "air masses" (merge-tree blobs), each shown as a solid
  // mass plus a vertical stem + marker at its extremum. The cache is keyed by
  // param+time so spinning the camera / animating does not re-run the
  // O(N log N) merge tree every frame.
  bool itsShowExtrema = false;
  VolumeGrid itsExtremaGrid;                // original (un-detrended) volume — coords + colour
  std::vector<Feature> itsExtremaFeatures;  // retained non-global maxima then minima
  std::string itsExtremaKey;                // cache signature: "paramId|timeIndex"

  // 3D + cross-section curtain (key [4]). Toggled on when the source
  // declares hasNativeHeight() — multi-level QueryData with GeomHeight,
  // or PVOL polar volumes. Renders the surface as a coloured floor,
  // overlays a vertical curtain between two endpoints showing the
  // cross-section sampled via interpolatedValueAtHeight, and lets the
  // user fly the curtain around with the arrow keys without leaving 3D.
  // Time animation (Space) keeps working; the curtain re-samples each
  // frame so the slice evolves with the data underneath.
  bool itsMode3DCurtain = false;
  double itsCurtainLat1 = 0.0;
  double itsCurtainLon1 = 0.0;
  double itsCurtainLat2 = 0.0;
  double itsCurtainLon2 = 0.0;
  bool itsCurtainEndpointsInitialised = false;
  // Curtain ceiling height in km — clipped to the source's heightRangeKm.
  // PgUp / PgDn change it.
  double itsCurtainHeightKm = 12.0;
  // Which endpoint the arrow keys move. Cycled with Tab.
  // View is a fourth sub-mode where arrows orbit / pitch the camera and
  // +/- zooms instead of moving endpoints — the user asked for a clean
  // "edit vs view" split so the cursor keys never accidentally drag the
  // curtain while you're framing the camera.
  enum class CurtainEnd : std::uint8_t
  {
    A,
    B,
    Both,
    View,
  };
  CurtainEnd itsCurtainActiveEnd = CurtainEnd::Both;
  // Bbox-fraction step the arrow keys apply per press in mode 4.
  static constexpr double kCurtainStepFrac = 0.03;

  // 3D curtain animations. Each flag is an independent toggle; any subset
  // can be on at once. Phases are accumulated separately so toggling on /
  // off doesn't snap the visible angle — the curtain resumes from where
  // it stopped. AnimSpeed is a single multiplier the user steps with +/-
  // while in an Edit sub-mode (View mode's +/- still zooms the camera).
  bool itsCurtainTwoPlane = false;     // X-cross: render a second plane
                                        // perpendicular through the centre
  bool itsCurtainAutoSwing = false;    // swing azimuth back-and-forth
  bool itsCurtainAutoRotate = false;   // continuously rotate azimuth
  bool itsCurtainAutoOrbit = false;    // orbit the centre in a circle
  bool itsCurtainAutoTilt = false;     // tilt the plane(s) about the long
                                        // axis — combined with rotate this
                                        // makes the X-cross tumble in 3D
  double itsCurtainAnimSpeed = 1.0;
  static constexpr double kCurtainSpeedStep = 1.25;
  static constexpr double kCurtainSpeedMin = 0.05;
  static constexpr double kCurtainSpeedMax = 20.0;
  double itsCurtainSwingPhase = 0.0;
  double itsCurtainRotatePhase = 0.0;
  double itsCurtainOrbitPhase = 0.0;
  double itsCurtainTiltPhase = 0.0;
  std::chrono::steady_clock::time_point itsCurtainAnimLastTick{};
  bool itsCurtainAnimTickValid = false;

  // Animation state.
  bool itsAnimating = false;
  int itsAnimationDelayMs = 250;

  // Overlay toggles.
  bool itsShowWindArrows = false;
  bool itsShowCities = false;
  // Graticule + coastline + political-border render styles. Cycled with
  // `n` / `c` / `b` (Braille → Thick → None → Braille). Coastline / border
  // are also initialised from Options::noCoastline / noBorders (None when
  // set, Braille otherwise).
  LineStyle itsGraticuleStyle = LineStyle::Braille;
  LineStyle itsCoastlineStyle = LineStyle::Braille;
  LineStyle itsBorderStyle = LineStyle::Braille;
  // Cell rendering style; cycled with `t`. Sextant (2×3 sub-pixels) is the
  // default; SmallTriangle (2×2 + corner bevels) is the fallback for fonts
  // that don't ship the Symbols-for-Legacy-Computing block. macOS Terminal.app
  // is auto-detected and starts in Square — see defaultCornerStyle().
  CornerStyle itsCornerStyle = defaultCornerStyle();
  // Pixel-grade output for the 2D map. Toggled with `s`/`S`, which cycles
  // through whichever protocols the host terminal supports — see
  // probeTerminalCapabilities(). When non-Block, the map is sampled at
  // terminal-pixel resolution (cellW*cellPxW × cellH*cellPxH) and emitted
  // as a graphics blob instead of block glyphs; popups / 3D / exit effects
  // keep block-glyph output regardless.
  enum class GraphicsMode : std::uint8_t
  {
    Block,  // block-glyph cells (the default)
    Kitty,  // Kitty graphics protocol (RGBA, no quantisation)
    Sixel,  // sixel (≤254-colour quantisation per frame)
  };
  GraphicsMode itsGraphicsMode = GraphicsMode::Block;
  TerminalCapabilities itsCaps{};
  // Top-N cap for the cities overlay; PageUp / PageDown step through fixed
  // levels (5, 10, 25, 50, 100, 250, 500). Default = a comfortable mid value.
  int itsCityOverlayN = 25;

  // Shapefile palette mode, cycled by [R]. 0 = flat (mid grey),
  // 1 = rainbow per burn id (default — distinguishes adjacent
  // polygons immediately). Only used when the source is a
  // ShapeSource; for any other backend the [R] key is a no-op and
  // this member is ignored.
  int itsShapePaletteMode = 1;
  // Polylines extracted from the shapefile (polygon exterior + hole
  // rings, plus any LineString geometries). Lives in its own slot so
  // [B] keeps cycling GSHHS political borders unchanged; [O] cycles
  // this overlay.
  std::vector<Polyline> itsShapeOutlines;
  LineStyle itsShapeOutlineStyle = LineStyle::Braille;

  // PostGIS browser mode. itsPgDataset stays open for the lifetime
  // of the App so [T] (table picker) can re-pick layers from the
  // same connection without paying the libpq round-trip again. The
  // pointer is non-null whenever the App was launched with --pg.
  // Stored as void* in the header to avoid pulling gdal_priv.h into
  // every translation unit; reinterpret on use in QdlessApp.cpp.
  void* itsPgDataset = nullptr;
  // Open the PG dataset, run the layer picker, and replace itsSource
  // with a ShapeSource over the picked layer. Called once at startup
  // and re-invoked on [T]. Returns true on success.
  bool openPgPicker(UI& ui);
  // Build a ShapeSource over a named layer of itsPgDataset. Used by
  // openPgPicker after selection and by --table direct-open.
  void openPgLayer(const std::string& schemaTable);

  // PNG-tree browser. Each entry is one leaf directory found under
  // itsOpts.browseRoot (a directory that directly contains at least
  // one *.png). Populated lazily on the first openBrowsePicker call.
  struct BrowseLeaf
  {
    std::string fullPath;
    std::string relPath;  // path relative to itsOpts.browseRoot
  };
  std::vector<BrowseLeaf> itsBrowseLeaves;
  bool itsBrowseLeavesScanned = false;
  // Rescan the tree and refill itsBrowseLeaves. Called on every picker
  // open so newly-arrived directories are picked up.
  void scanBrowseTree();
  // Open the (search + column-nav) leaf picker. Returns true if a leaf
  // was picked (and the corresponding MultiFileSource is now active).
  bool openBrowsePicker(UI& ui);
  // Replace itsSource with a MultiFileSource over the given leaf
  // directory's sorted *.png files, then run initFromSource.
  void openBrowseLeaf(const std::string& dir);
  // All post-itsSource init: parameter resolution, panel layout,
  // palette / coastline load, etc. Pulled out of the ctor so the
  // PostGIS deferred-pick path can run it after the user picks a
  // layer at startup.
  void initFromSource();

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
  // Braille variant: emits positioned braille glyphs to `os` on top of the
  // already-rendered cells. Mirrors appendPolylineBraille; uses pixels[] only
  // to sample the per-cell background colour for each emitted glyph.
  void appendGraticuleBraille(std::ostringstream& os,
                              const std::vector<Rgb>& pixels,
                              int subWidth,
                              int originRow,
                              int originCol) const;
  // Walk meridians + parallels at the resolution `bW × bH`, returning the
  // list of valid segments (round-trip filtered, antimeridian-guarded). Each
  // entry is {x0, y0, x1, y1} in sub-cell coords. Shared between the Thick
  // and Braille graticule renderers.
  std::vector<std::array<int, 4>> traceGraticuleSegments(int bW, int bH) const;
  void overlayMarker(std::vector<Rgb>& pixels, int subWidth, int subHeight) const;
  void overlayCities(std::vector<Rgb>& pixels, int subWidth, int subHeight) const;
  // Build the geographic extras the "Utopia" exit effect needs: `lines` is a
  // coastlines+borders raster on black; `swedenMask` is a 0/1 mask of the
  // pixels inside an approximate Sweden polygon. Both left empty when the
  // source isn't geographic / no coastlines are loaded / the viewport is
  // degenerate, in which case Utopia falls back to a plain fade.
  void buildUtopiaGeo(int subWidth,
                      int subHeight,
                      std::vector<Rgb>& lines,
                      std::vector<char>& swedenMask) const;
  // Draw the active cross-section great-circle line, its endpoints, and
  // the mouse-tracked hover dot into the data pixel buffer. No-op when
  // itsCrossActive is false.
  void overlayCrossSection(std::vector<Rgb>& pixels, int subWidth, int subHeight) const;
  // Append a braille-glyph overlay of a polyline set, written AFTER the
  // renderer's quadrant blocks. Each polyline is rasterised into a 2x4
  // sub-cell mask (4x finer Y than the data raster); cells with any dots
  // get a braille glyph in `color` over `pixels[topLeftSubcell]` as bg.
  // Yields a much thinner-looking line than overlaying into the data
  // pixel buffer, at the cost of replacing the data quadrant-block in
  // those cells with a single bg sample.
  void appendPolylineBraille(std::ostringstream& os,
                             const std::vector<Polyline>& polylines,
                             Rgb color,
                             const std::vector<Rgb>& pixels,
                             int subWidth,
                             int originRow,
                             int originCol) const;
  std::string buildWindArrows(int cellW, int cellH, int originRow, int originCol);
  std::string buildCityLabels(int cellW, int cellH, int originRow, int originCol);
  // 3D volume renderer. Active only when itsMode3D and the source is a
  // PVOL. Writes a complete frame (radar points + ground-plane coastlines
  // + status overlay) to stdout, replacing the usual 2D map. Takes a
  // Layout (not a UI) so `--dump --3d` can call it from runOnce without
  // initscr.
  void draw3D(const Layout& layout);
  // QueryData volume renderer (cloud cover / cloud water on hybrid or
  // pressure levels). Reuses the same orbit-and-zoom camera as draw3D
  // but feeds the point cloud from QueryDataSource::sampleVolume()
  // instead of the polar bin loop. Coastlines are projected through a
  // flat-Earth frame anchored at the data bbox centre — same maths,
  // different anchor.
  void draw3DQueryData(const Layout& layout);
  // Surface-stack 3D view for QueryData files that have no real vertical
  // axis but carry the canonical "ground / fog / low / med / high"
  // surface parameters (Precipitation1h, FogIntensity, LowCloudCover,
  // MediumCloudCover, optional HighCloudCover). Each layer is rendered
  // at a fixed canonical height with its own palette and threshold,
  // producing a cartoon — but visually informative — vertical
  // cross-section of the weather. Same camera + z-buffer pipeline as
  // draw3DQueryData.
  void draw3DSurfaceStack(const Layout& layout);
  // 3D + cross-section curtain renderer for [4] mode. The surface is
  // painted by inverse-projecting each output sub-pixel onto z=0 and
  // sampling the bottom-level slice; the curtain between
  // (lat1,lon1)..(lat2,lon2) is ray-cast against and filled via
  // interpolatedValueAtHeight. Both share one z-buffer, so the curtain
  // self-occludes against the ground and against the camera.
  void draw3DCrossSection(const Layout& layout);
  // Globe renderer (key [g]). Inverse-orthographic: every output sub-pixel is
  // ray-cast against a unit sphere; the near intersection's (lat,lon) is
  // sampled via interpolatedValue and coloured through the active palette,
  // with a shaded bare-sphere fill where the data has no value. Coastlines,
  // borders and a lat/lon graticule are projected on top with back-face
  // culling against the same z-buffer. Takes a Layout (not a UI) so
  // `--dump --globe` can call it from runOnce without initscr.
  void drawGlobe(const Layout& layout);
  // Initialise itsCurtainLat1/Lon1/Lat2/Lon2 from the current viewport
  // if they're still at their defaults. Called on the first [4] press
  // and any time the viewport changes substantially.
  void ensureCurtainEndpoints();
  // Advance the curtain animation phases by the elapsed wall-clock time
  // multiplied by itsCurtainAnimSpeed. No-op the first time it's called
  // each session (no anchor yet) and resets the anchor whenever the user
  // toggles a flag off — so re-enabling resumes from the same phase.
  void tickCurtainAnimation();
  // True if the active source can be rendered in 3D. Tested by [3]
  // and by drawMap when deciding whether to honour itsMode3D.
  bool sourceSupports3D() const;
  // True if the active source can be drawn on the globe — any gridded
  // geographic source (images / vector layers have no scalar field).
  bool sourceSupportsGlobe() const;

  // --extrema verification path: run the persistence / merge-tree extrema
  // finder on the active volumetric QueryData parameter (per-level-median
  // anomaly) and print the most persistent maxima / minima to stdout.
  // Returns a process exit code. No rendering — headless analysis only.
  int dumpExtremaReport() const;

  // Rebuild itsExtremaGrid + itsExtremaFeatures for the active volumetric
  // parameter when the param/time signature changes; no-op on a cache hit.
  // Detrends a per-level-median copy, runs the merge-tree finder, and keeps
  // the most persistent non-global maxima and minima. Clears the cache when
  // the source is not a volumetric QueryData.
  void ensureExtremaCache();
  // Reset itsThreshold3D / itsThreshold3DUnit / itsVexagger3D to
  // source-appropriate defaults (dBZ for PVOL, % for QueryData). Called
  // each time 3D is toggled on so a switch between sources doesn't
  // strand the user at the wrong threshold scale.
  void apply3DDefaultsForSource();
  // True if the active source carries both WindUMS and WindVMS — used to
  // gate the [W]ind status-bar toggle and the wind-arrow overlay.
  bool hasWindComponents() const;

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

  // Activate the persistent cross-section overlay between two click cells.
  // Validates levels and stores the geographic endpoints; drawCrossSection
  // is then called from the main loop on each redraw.
  void beginCrossSection(int x1, int y1, int x2, int y2, UI& ui);
  // Render the cross-section popup at the current time / parameter using
  // the stored endpoints. No-op when itsCrossActive is false.
  void drawCrossSection(UI& ui);

  // Compose the (label, value) rows for the metadata popup ('M').
  // Combines common fields (file path, size, time/level/param counts,
  // lat/lon extent, parameter listing) with backend-specific extras from
  // DataSource::extraMetadata().
  std::vector<std::pair<std::string, std::string>> buildMetadataRows() const;

  // Min / mean / max of the active panel's parameter sampled across the
  // currently-visible cells of the viewport, one entry per time step.
  // Drawn as a translucent overlay on the time-series probe popup ('s'
  // toggles the overlay).
  struct ViewportStats
  {
    float min = 0;
    float mean = 0;
    float max = 0;
    bool valid = false;  // false when the slice contains no finite samples
  };

  // Fetch (computing on cache miss) the viewport stats series for the
  // active panel's (param, level) at the current viewport. Cached so
  // re-opening the probe popup at a different point — or the eventual
  // in-popup time animation — reuses the result without rescanning. The
  // cache invalidates on viewport / param / level change.
  std::vector<ViewportStats> ensureViewportStats() const;

  // Cache for ensureViewportStats(). Invalidated on viewport/param/
  // level mismatch. Mutable so const probes can refresh it on demand.
  mutable bool itsStatsCacheValid = false;
  mutable int itsStatsCacheParam = -1;
  mutable std::size_t itsStatsCacheLevel = 0;
  mutable Viewport itsStatsCacheViewport{};
  mutable std::vector<ViewportStats> itsStatsCacheSeries;
};
}  // namespace Qdless
