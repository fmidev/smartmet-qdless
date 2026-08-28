#include "QdlessCatalog.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace Qdless
{
namespace
{
// Lowercase leveltype keywords seen across the archive. The parser also
// accepts an unknown lowercase token if it sits in a structurally valid
// position (see parseMasalaName), so novel types still parse.
const std::set<std::string>& knownLevelTypes()
{
  static const std::set<std::string> k = {"hybrid",
                                          "height",
                                          "pressure",
                                          "depth",
                                          "ground",
                                          "general",
                                          "meansea",
                                          "top",
                                          "entatm",
                                          "maxthetae",
                                          "mixing",
                                          "himan",
                                          "mudl",
                                          "meps",
                                          "surface",
                                          "altitude",
                                          "msl",
                                          "seabed"};
  return k;
}

bool isAllLowerAlpha(const std::string& s)
{
  if (s.empty())
    return false;
  for (unsigned char c : s)
    if (!std::islower(c))
      return false;
  return true;
}

bool isAllDigits(const std::string& s)
{
  if (s.empty())
    return false;
  for (unsigned char c : s)
    if (!std::isdigit(c))
      return false;
  return true;
}

// A level token is a number ("100", "850"), a signed/decimal number, or a
// range ("0-6", "72-73"). Returns true and sets `val` to the leading number.
bool parseLevelToken(const std::string& s, double& val)
{
  if (s.empty())
    return false;
  char* end = nullptr;
  val = std::strtod(s.c_str(), &end);
  if (end == s.c_str())
    return false;  // no leading number
  // Accept trailing range marker "-N" or empty.
  return true;
}

std::vector<std::string> splitUnderscore(const std::string& s)
{
  std::vector<std::string> out;
  std::size_t start = 0;
  while (true)
  {
    const std::size_t pos = s.find('_', start);
    if (pos == std::string::npos)
    {
      out.push_back(s.substr(start));
      break;
    }
    out.push_back(s.substr(start, pos - start));
    start = pos + 1;
  }
  return out;
}
}  // namespace

MasalaName parseMasalaName(const std::string& basename)
{
  MasalaName r;
  // Split extension.
  const std::size_t dot = basename.rfind('.');
  if (dot == std::string::npos)
    return r;
  const std::string ext = basename.substr(dot + 1);
  // Only the gridded formats follow the grammar.
  if (ext != "grib" && ext != "grib2" && ext != "grb" && ext != "grb2" && ext != "gr2" &&
      ext != "nc")
    return r;
  const std::string stem = basename.substr(0, dot);
  const auto f = splitUnderscore(stem);
  if (f.size() < 6)
    return r;

  // Anchor on the leveltype: the first lowercase-alpha field at index >= 1
  // that is either a known leveltype OR sits in a structurally valid spot
  // (followed by level, a lowercase projection token, and two integer grid
  // dimensions). Anchoring on the keyword — rather than counting fields from
  // either end — tolerates params containing '_' and producers that append
  // extra trailing fields after the grid dimensions.
  for (std::size_t k = 1; k + 4 < f.size() + 1 && k + 4 <= f.size(); ++k)
  {
    if (!isAllLowerAlpha(f[k]))
      continue;
    // Need level, proj, nx, ny after it.
    if (k + 4 >= f.size())
      break;
    const std::string& levelStr = f[k + 1];
    const std::string& proj = f[k + 2];
    const std::string& nxs = f[k + 3];
    const std::string& nys = f[k + 4];
    double lv = 0;
    const bool structural = parseLevelToken(levelStr, lv) && isAllLowerAlpha(proj) &&
                            isAllDigits(nxs) && isAllDigits(nys);
    const bool known = knownLevelTypes().count(f[k]) != 0;
    if (!structural && !known)
      continue;
    if (!structural)
      continue;  // known type but malformed tail — skip
    // Param is everything before the leveltype, rejoined with '_'.
    std::string param;
    for (std::size_t i = 0; i < k; ++i)
    {
      if (i)
        param += '_';
      param += f[i];
    }
    if (param.empty())
      continue;
    r.ok = true;
    r.param = param;
    r.levelType = f[k];
    r.levelStr = levelStr;
    r.level = lv;
    r.proj = proj;
    r.nx = std::atoi(nxs.c_str());
    r.ny = std::atoi(nys.c_str());
    r.ext = ext;
    return r;
  }
  return r;
}

int masalaLevelTypeId(const std::string& token)
{
  // FmiLevelType numeric constants (see DataSource::levelTypeName).
  if (token == "ground" || token == "surface")
    return 1;
  if (token == "pressure")
    return 100;
  if (token == "meansea" || token == "msl")
    return 102;
  if (token == "altitude")
    return 103;
  if (token == "height")
    return 105;
  if (token == "hybrid")
    return 109;
  if (token == "depth")
    return 160;
  return 0;  // general / unknown -> generic "Levels"
}

namespace
{
bool ascendsForType(const std::string& token)
{
  // Altitude-like axes increase with value (ground at the bottom);
  // pressure and hybrid decrease with altitude.
  return token == "height" || token == "altitude" || token == "depth";
}

std::string indexKey(const std::string& param,
                     const std::string& typeToken,
                     const std::string& levelStr,
                     int lead)
{
  return param + '\x1f' + typeToken + '\x1f' + levelStr + '\x1f' + std::to_string(lead);
}
}  // namespace

MasalaCube MasalaCube::build(const std::string& dir)
{
  MasalaCube cube;
  cube.itsDir = dir;

  // Infer producer / reftime / geometry from the path tail.
  {
    fs::path p(dir);
    const std::string geo = p.filename().string();
    const std::string ref = p.parent_path().filename().string();
    const std::string prod = p.parent_path().parent_path().filename().string();
    if (MasalaCatalog::isReftimeName(geo))
    {
      // dir IS a reftime (no geometry layer): files live directly under it.
      cube.itsReftime = geo;
      cube.itsProducer = ref;
    }
    else if (MasalaCatalog::isReftimeName(ref))
    {
      cube.itsGeometry = geo;
      cube.itsReftime = ref;
      cube.itsProducer = prod;
    }
  }

  // Collect (leadHours, leafDir) pairs. Numeric subdirs are leadtimes; if
  // there are none but files live directly in `dir`, that's a single leaf at
  // leadtime 0.
  std::vector<std::pair<int, std::string>> leaves;
  const auto subs = MasalaCatalog::listSubdirs(dir);
  for (const auto& s : subs)
    if (MasalaCatalog::isAllDigitsName(s))
      leaves.emplace_back(std::atoi(s.c_str()), dir + "/" + s);
  if (leaves.empty() && MasalaCatalog::hasCubeFiles(dir))
    leaves.emplace_back(0, dir);
  if (leaves.empty())
    return cube;

  std::sort(leaves.begin(), leaves.end());

  // Accumulators: param -> (typeToken -> set<levelStr,val>), and the set of
  // lead hours actually present.
  struct LevAcc
  {
    int typeId = 0;
    bool ascends = false;
    std::map<std::string, double> levels;  // levelStr -> val
  };
  std::map<std::string, std::map<std::string, LevAcc>> acc;  // param -> type -> LevAcc
  std::vector<std::string> paramOrder;                       // first-seen order
  std::set<int> leadSet;

  for (const auto& [lead, leafDir] : leaves)
  {
    const auto files = MasalaCatalog::listFiles(leafDir);
    bool anyHere = false;
    for (const auto& fn : files)
    {
      const MasalaName n = parseMasalaName(fn);
      if (!n.ok)
        continue;
      anyHere = true;
      auto& byType = acc[n.param];
      if (byType.empty() &&
          std::find(paramOrder.begin(), paramOrder.end(), n.param) == paramOrder.end())
        paramOrder.push_back(n.param);
      auto& la = byType[n.levelType];
      la.typeId = masalaLevelTypeId(n.levelType);
      la.ascends = ascendsForType(n.levelType);
      la.levels[n.levelStr] = n.level;
      cube.itsFiles[indexKey(n.param, n.levelType, n.levelStr, lead)] = leafDir + "/" + fn;
    }
    if (anyHere)
      leadSet.insert(lead);
  }

  cube.itsLeadHours.assign(leadSet.begin(), leadSet.end());

  for (const auto& pname : paramOrder)
  {
    Param param;
    param.name = pname;
    for (auto& [typeToken, la] : acc[pname])
    {
      LevelAxis axis;
      axis.typeToken = typeToken;
      axis.typeId = la.typeId;
      axis.ascends = la.ascends;
      for (const auto& [ls, v] : la.levels)
      {
        axis.levelStrs.push_back(ls);
        axis.levelVals.push_back(v);
      }
      // Sort levels by numeric value (natural order).
      std::vector<std::size_t> idx(axis.levelStrs.size());
      for (std::size_t i = 0; i < idx.size(); ++i)
        idx[i] = i;
      std::sort(idx.begin(),
                idx.end(),
                [&](std::size_t a, std::size_t b)
                { return axis.levelVals[a] < axis.levelVals[b]; });
      std::vector<std::string> ss;
      std::vector<double> vv;
      ss.reserve(idx.size());
      vv.reserve(idx.size());
      for (auto i : idx)
      {
        ss.push_back(axis.levelStrs[i]);
        vv.push_back(axis.levelVals[i]);
      }
      axis.levelStrs = std::move(ss);
      axis.levelVals = std::move(vv);
      param.levels.push_back(std::move(axis));
    }
    cube.itsParams.push_back(std::move(param));
  }
  return cube;
}

std::string MasalaCube::pathFor(const std::string& param,
                                const std::string& typeToken,
                                const std::string& levelStr,
                                int lead) const
{
  const auto it = itsFiles.find(indexKey(param, typeToken, levelStr, lead));
  return it == itsFiles.end() ? std::string{} : it->second;
}

namespace MasalaCatalog
{
bool isAllDigitsName(const std::string& name)
{
  if (name.empty())
    return false;
  for (unsigned char c : name)
    if (!std::isdigit(c))
      return false;
  return true;
}

std::vector<std::string> listSubdirs(const std::string& dir)
{
  std::vector<std::string> out;
  std::error_code ec;
  for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator();
       it.increment(ec))
  {
    if (it->is_directory(ec))
      out.push_back(it->path().filename().string());
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<std::string> listFiles(const std::string& dir)
{
  std::vector<std::string> out;
  std::error_code ec;
  for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator();
       it.increment(ec))
  {
    if (it->is_regular_file(ec))
      out.push_back(it->path().filename().string());
  }
  std::sort(out.begin(), out.end());
  return out;
}

bool isReftimeName(const std::string& name)
{
  if (name.size() != 12)
    return false;
  for (unsigned char c : name)
    if (!std::isdigit(c))
      return false;
  return true;
}

bool hasCubeFiles(const std::string& dir)
{
  std::error_code ec;
  for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator();
       it.increment(ec))
  {
    if (it->is_regular_file(ec) && parseMasalaName(it->path().filename().string()).ok)
      return true;
  }
  return false;
}

bool isCubeDir(const std::string& dir)
{
  if (hasCubeFiles(dir))
    return true;
  // Numeric leadtime subdir holding parseable files?
  for (const auto& s : listSubdirs(dir))
    if (isAllDigitsName(s) && hasCubeFiles(dir + "/" + s))
      return true;
  // A directory of georeferenced rasters (radar nowcasts etc.) is also a
  // (raster) cube — recognise it so the picker opens it instead of drilling
  // down to an empty file-level listing.
  return isRasterCubeDir(dir);
}

namespace
{
bool isRasterName(const std::string& name)
{
  const std::size_t dot = name.rfind('.');
  if (dot == std::string::npos)
    return false;
  std::string ext = name.substr(dot + 1);
  for (auto& c : ext)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return ext == "tif" || ext == "tiff";
}
}  // namespace

std::vector<std::string> listRasterFiles(const std::string& dir)
{
  std::vector<std::string> out;
  for (const auto& f : listFiles(dir))
    if (isRasterName(f))
      out.push_back(f);
  return out;  // listFiles already sorts
}

bool hasRasterFiles(const std::string& dir)
{
  std::error_code ec;
  for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator();
       it.increment(ec))
  {
    if (it->is_regular_file(ec) && isRasterName(it->path().filename().string()))
      return true;
  }
  return false;
}

bool isRasterCubeDir(const std::string& dir)
{
  if (hasRasterFiles(dir))
    return true;
  // Radar nowcasts live one level down, under a numeric date dir
  // (<geometry>/<YYYYMMDD>/*.tif). Restricting to all-digit subdirs keeps
  // this to a single readdir of date dirs, never a tree walk.
  for (const auto& s : listSubdirs(dir))
    if (isAllDigitsName(s) && hasRasterFiles(dir + "/" + s))
      return true;
  return false;
}

namespace
{
// Well-known model paths -> model name. Keyed by the canonical absolute path
// (an s3fs mount root, or a producer-id / dataset dir under /masala/datasets).
// Matched exactly or as a trailing suffix so a relocated catalog root still
// resolves (see modelNameForPath). Sourced from the FMI radon producer table.
const std::vector<std::pair<std::string, std::string>>& modelTable()
{
  static const std::vector<std::pair<std::string, std::string>> k = {
      {"/gem_data", "GEM"},
      {"/hrnwc-data/preop", "HRNWC_PREOP"},
      {"/hrnwc-data/prod", "HRNWC"},
      {"/meps-ml-correction/preop", "MEPS_ML_PREOP"},
      {"/masala/datasets/aila", "AILA"},
      {"/masala/datasets/bris", "BRIS"},
      {"/masala/datasets/meps-biascorrection", "MEPSMTADEV"},
      {"/masala/datasets/mnwc-biascorrection", "MNWCMTADEV"},
      {"/masala/datasets/simo", "SIMO"},
      {"/masala/datasets/vire", "VIRE"},
      {"/masala/datasets/virenwc", "VIRENWC"},
      {"/masala/datasets/virenwc_history", "VIRENWC_HISTORY"},
      {"/masala/datasets/4", "MEPS"},
      {"/masala/datasets/5", "MNWC"},
      {"/masala/datasets/7", "METCOOP_HYBRID1"},
      {"/masala/datasets/8", "METCOOP_HYBRID2"},
      {"/masala/datasets/10", "MEPS_PREOP"},
      {"/masala/datasets/16", "MEPS1500D_HYBRID"},
      {"/masala/datasets/52", "ENFUSER"},
      {"/masala/datasets/53", "KWBG"},
      {"/masala/datasets/100", "WILDFIRES_HISTORYA"},
      {"/masala/datasets/101", "MESAN"},
      {"/masala/datasets/103", "WILDFIRES"},
      {"/masala/datasets/105", "MTLICE"},
      {"/masala/datasets/107", "LAPSFIN"},
      {"/masala/datasets/109", "LAPSSCAN"},
      {"/masala/datasets/110", "PPNFIN"},
      {"/masala/datasets/112", "WAM_EC"},
      {"/masala/datasets/113", "WAM_BALMFC"},
      {"/masala/datasets/115", "DIW"},
      {"/masala/datasets/119", "FMI_PEPS"},
      {"/masala/datasets/120", "ECMOSKRIGING"},
      {"/masala/datasets/122", "HIMAN"},
      {"/masala/datasets/126", "FROST"},
      {"/masala/datasets/130", "GFS0250"},
      {"/masala/datasets/131", "ECG"},
      {"/masala/datasets/133", "ECGSEA"},
      {"/masala/datasets/134", "ECGEPS"},
      {"/masala/datasets/147", "NEMO"},
      {"/masala/datasets/148", "COPERNICUSNEMO"},
      {"/masala/datasets/153", "WAM_HKI"},
      {"/masala/datasets/154", "WAM_BALMFC_ARCH"},
      {"/masala/datasets/170", "ICON_GLO"},
      {"/masala/datasets/183", "LAPSLAMBERT2500"},
      {"/masala/datasets/189", "METAN"},
      {"/masala/datasets/190", "FMIICING"},
      {"/masala/datasets/220", "ICONMTA"},
      {"/masala/datasets/240", "ECGMTA"},
      {"/masala/datasets/242", "ECM_PROB"},
      {"/masala/datasets/243", "ECGEPSMTA"},
      {"/masala/datasets/244", "ECGEPSCALIB"},
      {"/masala/datasets/250", "GFSMTA"},
      {"/masala/datasets/260", "MEPSMTA"},
      {"/masala/datasets/261", "MEPS_PREOPMTA"},
      {"/masala/datasets/270", "MNWCMTA"},
      {"/masala/datasets/272", "MEPS1500D_SURF"},
      {"/masala/datasets/301", "SILAM_AQ"},
      {"/masala/datasets/601", "MEPS_STATISTICAL"},
  };
  return k;
}

// Normalise a path for model matching: strip trailing slashes and treat '_'
// and '-' as the same character (so the table's "/gem_data" matches an fstab
// "/gem-data" mount, and producer dirs spelled either way still resolve).
std::string normForModel(const std::string& path)
{
  std::string p = path;
  while (p.size() > 1 && p.back() == '/')
    p.pop_back();
  for (char& c : p)
    if (c == '_')
      c = '-';
  return p;
}
}  // namespace

std::optional<std::string> modelNameForPath(const std::string& absPath)
{
  const std::string p = normForModel(absPath);
  const std::string* best = nullptr;
  std::size_t bestLen = 0;
  for (const auto& [key, name] : modelTable())
  {
    const std::string k = normForModel(key);
    // Exact match, or `k` is a suffix of `p`. Every key begins with '/', so a
    // suffix match implicitly lands on a path-component boundary (the matched
    // region starts at a '/'), e.g. ".../masala/masala/datasets/131" matches
    // "/masala/datasets/131".
    const bool exact = (p == k);
    const bool suffix = p.size() > k.size() && p.compare(p.size() - k.size(), k.size(), k) == 0;
    if ((exact || suffix) && k.size() >= bestLen)
    {
      best = &name;
      bestLen = k.size();
    }
  }
  if (best != nullptr)
    return *best;
  return std::nullopt;
}

std::string fstabPath()
{
  if (const char* env = std::getenv("QDLESS_FSTAB"); env != nullptr && env[0] != '\0')
    return env;
  return "/etc/fstab";
}

namespace
{
// Local / pseudo filesystems that never hold a browseable weather archive —
// used to abort fstab lines with zero I/O.
bool isLocalFsType(const std::string& fstype)
{
  static const std::set<std::string> k = {
      "ext2",       "ext3",   "ext4",     "xfs",        "btrfs",   "vfat",
      "swap",       "tmpfs",  "proc",     "sysfs",      "devpts",  "devtmpfs",
      "cgroup",     "cgroup2", "mqueue",  "hugetlbfs",  "debugfs", "configfs",
      "securityfs", "overlay", "squashfs", "autofs",    "none"};
  return k.count(fstype) != 0;
}

// True if `mp` equals `prefix` or sits directly beneath it.
bool underPath(const std::string& mp, const std::string& prefix)
{
  if (mp == prefix)
    return true;
  return mp.size() > prefix.size() && mp.compare(0, prefix.size(), prefix) == 0 &&
         mp[prefix.size()] == '/';
}

// True if `name` is one of the known model names (used by the content probe to
// recognise a directory of model-named producer dirs).
bool isKnownModelName(const std::string& name)
{
  for (const auto& [key, model] : modelTable())
    if (model == name)
      return true;
  return false;
}

// Single shallow readdir: does `dir` look like a weather archive? True if any
// immediate child is a producer id, reference time, a known model name, or
// "datasets", or the dir itself directly holds cube / raster files.
bool looksLikeWeatherDir(const std::string& dir)
{
  for (const auto& s : MasalaCatalog::listSubdirs(dir))
    if (MasalaCatalog::isAllDigitsName(s) || MasalaCatalog::isReftimeName(s) || s == "datasets" ||
        isKnownModelName(s))
      return true;
  return MasalaCatalog::hasCubeFiles(dir) || MasalaCatalog::hasRasterFiles(dir);
}
}  // namespace

std::vector<WeatherRoot> discoverWeatherRoots()
{
  std::vector<WeatherRoot> out;
  std::ifstream in(fstabPath());
  std::string line;
  while (std::getline(in, line))
  {
    // Skip blank / comment lines (s3fs devices contain '#', so only a leading
    // '#' is a comment).
    const std::size_t first = line.find_first_not_of(" \t");
    if (first == std::string::npos || line[first] == '#')
      continue;
    std::istringstream ls(line);
    std::string device, mp, fstype, options;
    if (!(ls >> device >> mp >> fstype >> options))
      continue;

    // Cheap aborts (no I/O).
    if (isLocalFsType(fstype))
      continue;
    if (mp == "/" || underPath(mp, "/home") || underPath(mp, "/boot") || underPath(mp, "/proc") ||
        underPath(mp, "/sys") || underPath(mp, "/dev") || underPath(mp, "/var") ||
        underPath(mp, "/usr") || underPath(mp, "/run") || underPath(mp, "/tmp") ||
        underPath(mp, "/opt"))
      continue;

    const bool isS3fs = device.rfind("s3fs", 0) == 0;
    const bool networked = isS3fs || fstype == "nfs" || fstype == "nfs4" || fstype == "fuse";
    if (!networked)
      continue;

    // Cheap accepts (no I/O), then a content probe (one readdir) for the rest.
    bool weather = isS3fs || underPath(mp, "/masala") || modelNameForPath(mp).has_value();
    if (!weather)
      weather = looksLikeWeatherDir(mp);
    if (!weather)
      continue;

    WeatherRoot r;
    r.path = mp;
    if (auto m = modelNameForPath(mp))
      r.model = *m;
    out.push_back(std::move(r));
  }

  // Well-known local GRIB directories that aren't separate fstab mounts (so the
  // scan above won't surface them). Included automatically when they exist; the
  // dedup below drops them if they also appeared as a mount.
  for (const char* p : {"/srv/data/grib"})
  {
    std::error_code ec;
    if (!std::filesystem::is_directory(p, ec))
      continue;
    WeatherRoot r;
    r.path = p;
    if (auto m = modelNameForPath(p))
      r.model = *m;
    out.push_back(std::move(r));
  }

  std::sort(out.begin(), out.end(), [](const WeatherRoot& a, const WeatherRoot& b)
            { return a.path < b.path; });
  out.erase(std::unique(out.begin(), out.end(), [](const WeatherRoot& a, const WeatherRoot& b)
                        { return a.path == b.path; }),
            out.end());
  return out;
}
}  // namespace MasalaCatalog
}  // namespace Qdless
