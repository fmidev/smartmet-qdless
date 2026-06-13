#include "QdlessCatalog.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <set>
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
  return false;
}
}  // namespace MasalaCatalog
}  // namespace Qdless
