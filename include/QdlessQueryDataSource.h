#pragma once

#include "QdlessDataSource.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

class NFmiQueryData;
class NFmiFastQueryInfo;

namespace Qdless
{
// DataSource backed by newbase NFmiFastQueryInfo. Used for SmartMet's
// native QueryData (.sqd) files. The fastest path; preserves any
// QueryData-specific behaviour.
class QueryDataSource : public DataSource
{
 public:
  explicit QueryDataSource(const std::string& filename);
  ~QueryDataSource() override;
  QueryDataSource(const QueryDataSource&) = delete;
  QueryDataSource& operator=(const QueryDataSource&) = delete;
  QueryDataSource(QueryDataSource&&) = delete;
  QueryDataSource& operator=(QueryDataSource&&) = delete;

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

  std::size_t levelCount() const override;
  std::size_t currentLevelIndex() const override;
  void selectLevelIndex(std::size_t i) override;
  float levelValueAt(std::size_t i) const override;
  std::string levelLabel(std::size_t i) const override;
  bool levelsAscendWithValue() const override;
  std::vector<LevelGroup> levelGroupsForParam(int paramId) const override;

  // Multi-level files that carry GeomHeight (or GeopHeight) can be
  // sampled at arbitrary heights via vertical interpolation between
  // levels; this enables the 3D curtain ('v') and the height-axis 2D
  // cross-section to work on NWP hybrid / pressure data the same way
  // they already do for PVOL polar volumes.
  bool hasNativeHeight() const override;
  std::pair<double, double> heightRangeKm() const override;
  float interpolatedValueAtHeight(double lat, double lon, double heightKm) const override;
  ColumnProfile sampleColumnProfile(double lat, double lon) const override;

  float interpolatedValue(double lat, double lon) const override;
  LatLonBox boundingBox() const override;
  void uvToLatLon(double u, double v, double& lat, double& lon) const override;
  void latLonToUV(double lat, double lon, double& u, double& v) const override;

  std::vector<std::pair<std::string, std::string>> extraMetadata() const override;
  std::string gridSignature() const override;

  // Volume sampling for the 3D point-cloud view. Returns true and emits
  // (lat, lon, heightMeters, value) tuples for every (level, grid-cell)
  // of the currently-active param at the currently-active time when this
  // file carries a height field — kFmiGeomHeight, or kFmiGeopHeight which
  // is divided by g=9.80665 to get geometric height. Returns false when
  // there is no height field or only one level, leaving callbacks
  // un-invoked. State (param / level / location index) is restored on
  // return so the call is transparent to the rest of the App.
  struct VolumeSample
  {
    double lat;
    double lon;
    double heightMeters;
    float value;
  };
  bool isVolumetric() const;
  bool sampleVolume(const std::function<void(const VolumeSample&)>& cb) const;

  // Dense structured-grid variant for analyses that need the native (i,j,k)
  // lattice topology (e.g. the persistence / merge-tree extrema finder).
  // Fills nx/ny/nz and the flat arrays: values & heights are nz*ny*nx in
  // level-outer, row-major (locationIndex = j*nx+i) order, matching
  // VolumeGrid::idx; lats/lons are ny*nx. Missing or non-finite cells are
  // written as NaN. Returns false (arrays untouched) when the file is not a
  // multi-level grid with a height field. Param/level/loc indices restored.
  // targetMaxCells > 0 caps the grid: when the full volume exceeds it the
  // read is horizontally strided (levels kept) so nx*ny*nz stays under the
  // budget, bounding both this read and any downstream O(N) analysis. The
  // reported nx/ny are then the strided dimensions. 0 = full resolution.
  bool sampleVolumeGrid(std::size_t& nx,
                        std::size_t& ny,
                        std::size_t& nz,
                        std::vector<float>& values,
                        std::vector<float>& heights,
                        std::vector<float>& lats,
                        std::vector<float>& lons,
                        std::size_t targetMaxCells = 0) const;

  // True if the file carries the "surface stack" of layers used for the
  // synthetic 3D cloud + precipitation view: Precipitation1h,
  // FogIntensity, LowCloudCover, MediumCloudCover. HighCloudCover is
  // optional. Used when the source has no real vertical axis (isVolumetric
  // == false) so [3] can still render a layered point cloud at canonical
  // heights — see App::draw3DSurfaceStack.
  bool isSurfaceStack() const;

  // Iterate every grid cell of the currently-active time + level for a
  // *specified* parameter, yielding (lat, lon, value). Param/level/loc
  // indices are restored on return. Returns false (without invoking cb)
  // when paramId is not present in the file. Used by the surface-stack
  // renderer to walk each layer's 2D slab independently.
  bool sampleSlab(int paramId,
                  const std::function<void(double lat, double lon, float value)>& cb) const;

  // Returns a fresh QueryDataSource that shares the underlying
  // NFmiQueryData but owns its own NFmiFastQueryInfo iterator. Per the
  // newbase contract, two independent iterators over one NFmiQueryData
  // are safe to use concurrently for read-only access — which is the
  // mechanism the phenomenon-detector worker thread uses to inspect
  // the data without locking the main thread out of rendering.
  std::unique_ptr<DataSource> cloneForRead() const override;

 private:
  // Clone constructor — shares itsData with the parent and gives the
  // clone its own iterator. Public only via cloneForRead().
  QueryDataSource(std::shared_ptr<NFmiQueryData> data,
                  std::vector<int> paramIds);

  std::shared_ptr<NFmiQueryData> itsData;
  std::unique_ptr<NFmiFastQueryInfo> itsInfo;
  std::vector<int> itsParamIds;
};
}  // namespace Qdless
