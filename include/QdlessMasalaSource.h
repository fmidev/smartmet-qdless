#pragma once

#include "QdlessCatalog.h"
#include "QdlessDataSource.h"

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Qdless
{
// A DataSource over one masala "cube": a single (producer, reftime, geometry)
// whose per-file slices form a (parameter x level x leadtime) hypercube. The
// axes come entirely from filenames (via MasalaCube); the actual grid for the
// currently-selected (param, level, time) is read on demand by opening that
// one file through DataSource::open (a single-message GridFilesSource) and
// delegating interpolatedValue / boundingBox / uvToLatLon to it. Opened files
// are kept in a small LRU cache so probing and animation don't reopen on
// every sample.
//
// Parameter ids are resolved to real newbase ids from a representative file
// per parameter (so -p and palette lookup behave exactly as for a single
// GRIB file); when a name doesn't resolve, a unique synthetic id is assigned
// so the parameter is still pickable.
class MasalaSource : public DataSource
{
 public:
  explicit MasalaSource(MasalaCube cube);
  ~MasalaSource() override;
  MasalaSource(const MasalaSource&) = delete;
  MasalaSource& operator=(const MasalaSource&) = delete;
  MasalaSource(MasalaSource&&) = delete;
  MasalaSource& operator=(MasalaSource&&) = delete;

  std::vector<int> paramIds() const override;
  std::string paramShortName(int paramId) const override;
  std::string paramLongName(int paramId) const override;
  std::string paramUnits(int paramId) const override;
  int currentParamId() const override;
  bool selectParamId(int paramId) override;

  std::size_t timeCount() const override;
  std::size_t currentTimeIndex() const override;
  void selectTimeIndex(std::size_t i) override;
  NFmiMetTime currentValidTime() const override;
  NFmiMetTime originTime() const override;

  std::vector<LevelGroup> levelGroupsForParam(int paramId) const override;
  void selectLevelGroup(int paramId, int groupIdx) override;
  int currentLevelGroupIndex(int paramId) const override;

  std::size_t levelCount() const override;
  std::size_t currentLevelIndex() const override;
  void selectLevelIndex(std::size_t i) override;
  float levelValueAt(std::size_t i) const override;
  std::string levelLabel(std::size_t i) const override;
  bool levelsAscendWithValue() const override;

  float interpolatedValue(double lat, double lon) const override;
  float sampleValueAtUV(double u, double v) const override;
  LatLonBox boundingBox() const override;
  void uvToLatLon(double u, double v, double& lat, double& lon) const override;
  void latLonToUV(double lat, double lon, double& u, double& v) const override;
  void latLonToUVBatch(const std::vector<float>& lats,
                       const std::vector<float>& lons,
                       std::vector<float>& us,
                       std::vector<float>& vs) const override;
  std::string gridSignature() const override;
  std::vector<std::pair<std::string, std::string>> extraMetadata() const override;
  SourceCategory category() const override { return SourceCategory::Gridded; }

 private:
  // Per-parameter resolved metadata.
  struct ParamMeta
  {
    int id = 0;             // newbase id (or synthetic)
    std::string shortName;  // file token, e.g. "T-K"
    std::string longName;
    std::string units;
  };

  const MasalaCube::Param& curParam() const { return itsCube.params()[itsParamIdx]; }
  const MasalaCube::LevelAxis* activeAxis() const;
  // Resolve the file path for the current (param, group, level, time).
  std::string currentPath() const;
  // Open (or fetch from cache) the delegate for the current slice; nullptr if
  // the file is missing or fails to open.
  DataSource* currentDelegate() const;
  DataSource* openCached(const std::string& path) const;

  MasalaCube itsCube;
  std::vector<ParamMeta> itsMeta;  // parallel to itsCube.params()

  std::size_t itsParamIdx = 0;
  std::size_t itsTimeIdx = 0;
  // Active level group per parameter index, and active level within (param,
  // group). Defaults to group 0, level 0.
  mutable std::map<std::size_t, int> itsActiveGroup;
  mutable std::map<std::pair<std::size_t, int>, std::size_t> itsActiveLevel;

  // LRU cache of opened per-file delegates. Each masala slice is its own
  // file, so a single time animation steps through one file per lead time;
  // sizing the cache to cover a typical forecast length (≈24–66 steps) keeps
  // a full animation loop entirely in cache — no re-open / re-decode per
  // frame once warm. Each cached GridFilesSource also holds its decoded
  // value grid (≈nx*ny floats), so this trades memory for animation speed.
  static constexpr std::size_t kCacheMax = 64;
  mutable std::map<std::string, std::unique_ptr<DataSource>> itsCache;
  mutable std::deque<std::string> itsCacheOrder;

  NFmiMetTime itsOrigin;      // parsed from reftime (only valid if itsHasOrigin)
  bool itsHasOrigin = false;  // reftime parsed successfully
};
}  // namespace Qdless
