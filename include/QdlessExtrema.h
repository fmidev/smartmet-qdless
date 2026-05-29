#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Qdless
{
// Dense structured 3D volume on the model's native (i,j,k) lattice, as
// produced by QueryDataSource::sampleVolumeGrid. `values` and `heights` are
// nz*ny*nx (k outermost, then j, then i — matching the source's level-outer,
// row-major location order); `lats` / `lons` are ny*nx. Missing cells carry
// NaN so the extrema sweep can skip them with std::isfinite. The module is
// deliberately free of newbase / App dependencies so it can be unit-tested
// and reasoned about in isolation.
struct VolumeGrid
{
  std::size_t nx = 0;
  std::size_t ny = 0;
  std::size_t nz = 0;
  std::vector<float> values;   // detrended in place by detrendPerLevelMedian
  std::vector<float> heights;  // metres
  std::vector<float> lats;     // ny*nx, degrees
  std::vector<float> lons;     // ny*nx, degrees

  std::size_t idx(std::size_t i, std::size_t j, std::size_t k) const
  {
    return (k * ny + j) * nx + i;
  }
  std::size_t cellCount() const { return nx * ny * nz; }
  std::size_t sliceCount() const { return nx * ny; }
};

enum class ExtremumKind : std::uint8_t
{
  Max,
  Min,
};

// One persistent extremum and the air-mass blob around it.
struct Feature
{
  ExtremumKind kind = ExtremumKind::Max;
  double lat = 0;
  double lon = 0;
  double heightMeters = 0;
  float value = 0;        // detrended (anomaly) value at the peak cell
  float saddle = 0;       // value at which this feature merges with another
  float persistence = 0;  // |value - saddle|; larger = more prominent
  std::size_t peakCell = 0;
  bool isGlobal = false;       // the never-merging component (blob = whole field)
  bool blobTruncated = false;  // flood hit the cell cap before completing
  std::vector<std::size_t> blob;  // member cells, bounded by the merge saddle
};

// Subtract each level's area-weighted (cos-lat) median from every finite cell
// on that level, turning the field into a per-level anomaly so the vertical
// background gradient does not dominate the extrema sweep. The median is the
// weighted 50th percentile (so dense high-latitude rows do not skew it).
// No-op for levels with no finite cells. Mutates grid.values.
void detrendPerLevelMedian(VolumeGrid& grid);

// Merge-tree (union-find) extraction of persistent maxima or minima over the
// 6-connected lattice. A maximum is "born" at a local peak and "dies" when a
// descending sweep first connects it to a higher peak through a saddle; its
// persistence is peak-minus-saddle, exactly the contrast that must build up
// before the feature unites with the surrounding field. Returns features with
// persistence >= minPersistence, sorted by descending persistence, each
// carrying its blob (flood from the peak bounded by the merge saddle).
// maxFeatures caps the count (0 = all); blobCellCap bounds each flood.
std::vector<Feature> findExtrema(const VolumeGrid& grid,
                                 ExtremumKind kind,
                                 float minPersistence,
                                 std::size_t maxFeatures = 0,
                                 std::size_t blobCellCap = 200000);
}  // namespace Qdless
