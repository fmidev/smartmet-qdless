#pragma once

// Filesystem catalog for the "masala" forecast archive.
//
// The archive is a SmartMet/radon file store laid out as
//
//   <root>/<producer>/<reftime>/<geometry>/<leadtime>/<param-file>
//
// where each leaf file holds exactly one (parameter, level, timestep) slice
// and ALL of its metadata is encoded in the directory path + filename — so
// the whole navigable hypercube can be reconstructed from names alone,
// without opening a single data file. Navigation is lazy: one readdir per
// node the user expands, never a recursive tree walk (a full `ls -lR` of the
// real mount takes ~90 s; we never do that).
//
// Filename grammar (radon convention):
//
//   <PARAM>_<LEVELTYPE>_<LEVEL>_<PROJ>_<NX>_<NY>_<SCAN>_<FFF>[...].<ext>
//   T-K    _hybrid    _100   _rll  _661 _576 _0      _000        .grib2
//   LAI_HV-M2M2_ground_0_rll_661_576_0_000.grib                        (param has '_')
//   ATMICEG-GH-3CM_height_3500_lcc_949_1069_0_000_3_7.grib2            (extra trailing fields)
//
// PARAM may itself contain underscores and always contains dashes/digits;
// LEVELTYPE is a lowercase keyword (hybrid/pressure/height/depth/ground/...).
// We therefore anchor the parse on the leveltype keyword rather than on fixed
// field positions, which makes it robust to both param-with-underscore and
// the extra trailing fields some producers append.

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Qdless
{
// Parsed components of one leaf filename. `ok` is false if the name does not
// match the grammar (e.g. radar GeoTIFFs, *-times.npy, dotfiles).
struct MasalaName
{
  bool ok = false;
  std::string param;      // "T-K", "LAI_HV-M2M2", "ATMICEG-GH-3CM"
  std::string levelType;  // "hybrid", "pressure", "height", "depth", "ground", ...
  std::string levelStr;   // raw level token: "100", "0-6", "850"
  double level = 0;       // numeric sort key (leading number of levelStr)
  std::string proj;       // "ll", "rll", "lcc", "tm"
  int nx = 0;
  int ny = 0;
  std::string ext;  // "grib2", "grib", "nc"
};

// Parse a leaf basename into its components. Returns ok=false on no match.
MasalaName parseMasalaName(const std::string& basename);

// FmiLevelType numeric id for a leveltype token (109=hybrid, 100=pressure,
// 105=height, 160=depth, 1=ground, 102=meansea, ...). 0 for unknown tokens.
int masalaLevelTypeId(const std::string& token);

// One navigable cube: a single (producer, reftime, geometry) with the full
// (param x leveltype x level x leadtime) index built from filenames. Holds
// only paths — no data is read until a slice is rendered.
class MasalaCube
{
 public:
  struct LevelAxis
  {
    std::string typeToken;               // "hybrid"
    int typeId = 0;                      // FmiLevelType
    std::vector<std::string> levelStrs;  // sorted, parallel to levelVals
    std::vector<double> levelVals;       // numeric, sorted natural order
    bool ascends = false;                // does altitude increase with value?
  };
  struct Param
  {
    std::string name;               // "T-K"
    std::vector<LevelAxis> levels;  // one axis per leveltype present
  };

  // Build a cube from a geometry directory (with numeric leadtime subdirs) or
  // a single leaf directory (files directly inside, treated as leadtime 0).
  // Producer / reftime / geometry are inferred from the path tail when they
  // match the expected patterns. Returns an empty cube (empty()) if no
  // parseable files are found.
  static MasalaCube build(const std::string& dir);

  bool empty() const { return itsParams.empty(); }
  const std::string& dir() const { return itsDir; }
  const std::string& producer() const { return itsProducer; }
  const std::string& reftime() const { return itsReftime; }
  const std::string& geometry() const { return itsGeometry; }
  const std::vector<Param>& params() const { return itsParams; }
  const std::vector<int>& leadHours() const { return itsLeadHours; }

  // Absolute path of the file holding (param, leveltype, level, lead), or ""
  // if absent.
  std::string pathFor(const std::string& param,
                      const std::string& typeToken,
                      const std::string& levelStr,
                      int lead) const;

 private:
  std::string itsDir;
  std::string itsProducer;
  std::string itsReftime;
  std::string itsGeometry;
  std::vector<Param> itsParams;
  std::vector<int> itsLeadHours;
  std::map<std::string, std::string> itsFiles;  // index key -> path
};

// Lazy navigation helpers over the catalog root. Each performs one readdir of
// a single directory (no recursion, no stat beyond d_type) and returns sorted
// basenames.
namespace MasalaCatalog
{
// Immediate subdirectory basenames of `dir`, sorted. Empty on error.
std::vector<std::string> listSubdirs(const std::string& dir);
// Regular-file basenames directly in `dir`, sorted. Empty on error.
std::vector<std::string> listFiles(const std::string& dir);
// True if `name` is a 12-digit reference time YYYYMMDDHHMM.
bool isReftimeName(const std::string& name);
// True if `name` is a non-empty all-digits string (a leadtime subdir).
bool isAllDigitsName(const std::string& name);
// True if `dir` directly contains at least one masala-parseable data file.
bool hasCubeFiles(const std::string& dir);
// True if `dir` is (or contains) a renderable cube: it has numeric leadtime
// subdirs holding parseable files, or parseable files directly inside.
bool isCubeDir(const std::string& dir);

// Georeferenced raster basenames (`.tif` / `.tiff`) directly in `dir`,
// sorted. These don't follow the radon grammar — they are nowcast / product
// rasters (e.g. FMI-PPN radar) handled as a time-animated MultiFileSource
// rather than a MasalaCube. Empty on error.
std::vector<std::string> listRasterFiles(const std::string& dir);
// True if `dir` directly contains at least one renderable raster file.
bool hasRasterFiles(const std::string& dir);
// True if `dir` is (or one subdir level down holds) a raster animation cube.
bool isRasterCubeDir(const std::string& dir);

// Human-readable model name for a catalog directory, looked up from a built-in
// table keyed by the well-known mount/producer paths (e.g. "/masala/datasets/4"
// -> "MEPS", "/gem_data" -> "GEM"). Matches `absPath` exactly or as a trailing
// path suffix on a '/' boundary, so it also resolves under a relocated catalog
// root (test fixtures, bind mounts); '_' and '-' are treated as equivalent so
// the table's "/gem_data" matches an fstab "/gem-data" mount. Returns nullopt
// when the path is not a known model. The longest matching key wins.
std::optional<std::string> modelNameForPath(const std::string& absPath);

// Path of the fstab to inspect: $QDLESS_FSTAB if set (for tests), else
// "/etc/fstab".
std::string fstabPath();

// A mount discovered in fstab that looks like a browseable weather-data root.
struct WeatherRoot
{
  std::string path;   // mount point, e.g. "/masala/datasets", "/gem-data"
  std::string model;  // known model name when the path maps to one, else ""
};

// Inspect fstab and return the network / s3fs mounts that hold weather data,
// sorted by path. Cheap classification first (fstype / device / known-model
// paths / under /masala), then a single shallow readdir ("content probe") for
// the remaining network mounts; obvious system mounts (/, /home, /boot, local
// filesystems, …) are aborted without any I/O. Empty if fstab is absent or no
// weather mount is found. Used by the [D] / bare-`--catalog` root picker.
std::vector<WeatherRoot> discoverWeatherRoots();
}  // namespace MasalaCatalog
}  // namespace Qdless
