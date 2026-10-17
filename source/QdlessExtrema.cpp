#include "QdlessExtrema.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <utility>
#include <vector>

namespace Qdless
{
namespace
{
// 6-connectivity neighbour offsets in (i,j,k); applied with bounds checks.
struct Neighbour
{
  int di;
  int dj;
  int dk;
};
constexpr Neighbour kNeighbours[6] = {
    {+1, 0, 0}, {-1, 0, 0}, {0, +1, 0}, {0, -1, 0}, {0, 0, +1}, {0, 0, -1}};

// Decompose a flat cell index into (i,j,k).
inline void unflatten(std::size_t flat, std::size_t nx, std::size_t ny, std::size_t& i,
                      std::size_t& j, std::size_t& k)
{
  const std::size_t slice = nx * ny;
  k = flat / slice;
  const std::size_t rem = flat - k * slice;
  j = rem / nx;
  i = rem - j * nx;
}

// Gather the in-bounds 6-neighbours of a cell into `out`; returns the count.
inline int neighboursOf(std::size_t flat, std::size_t nx, std::size_t ny, std::size_t nz,
                        std::size_t out[6])
{
  std::size_t i = 0;
  std::size_t j = 0;
  std::size_t k = 0;
  unflatten(flat, nx, ny, i, j, k);
  int n = 0;
  for (const auto& nb : kNeighbours)
  {
    const long ni = static_cast<long>(i) + nb.di;
    const long nj = static_cast<long>(j) + nb.dj;
    const long nk = static_cast<long>(k) + nb.dk;
    if (ni < 0 || nj < 0 || nk < 0 || ni >= static_cast<long>(nx) || nj >= static_cast<long>(ny) ||
        nk >= static_cast<long>(nz))
      continue;
    out[n++] = (static_cast<std::size_t>(nk) * ny + static_cast<std::size_t>(nj)) * nx +
               static_cast<std::size_t>(ni);
  }
  return n;
}

// Union-find with path halving; each root carries the index of its component's
// peak cell. Sizes are int32 to keep the (potentially multi-million-cell)
// arrays compact.
class DisjointSet
{
 public:
  explicit DisjointSet(std::size_t n) : itsParent(n), itsPeak(n)
  {
    std::iota(itsParent.begin(), itsParent.end(), 0U);
    std::iota(itsPeak.begin(), itsPeak.end(), 0U);
  }
  std::uint32_t find(std::uint32_t x)
  {
    while (itsParent[x] != x)
    {
      itsParent[x] = itsParent[itsParent[x]];  // path halving
      x = itsParent[x];
    }
    return x;
  }
  void attach(std::uint32_t child, std::uint32_t root) { itsParent[child] = root; }
  void setPeak(std::uint32_t root, std::uint32_t peakCell) { itsPeak[root] = peakCell; }
  std::uint32_t peak(std::uint32_t root) const { return itsPeak[root]; }

 private:
  std::vector<std::uint32_t> itsParent;
  std::vector<std::uint32_t> itsPeak;  // valid at roots only
};

// Working value of a cell: the value, sign-flipped when looking for minima
struct WorkValue
{
  const VolumeGrid& grid;
  float sign;
  float operator()(std::size_t c) const { return sign * grid.values[c]; }
};

// A component that died when it merged into a higher one
struct Death
{
  std::uint32_t peakCell;
  float peakWork;
  float saddleWork;
};

// Distinct roots of the already-processed neighbours of cell c
int processedRoots(std::uint32_t c,
                   const VolumeGrid& grid,
                   DisjointSet& dsu,
                   const std::vector<char>& processed,
                   std::size_t (&neigh)[6],
                   std::uint32_t (&roots)[6])
{
  int nroots = 0;
  const int nn = neighboursOf(c, grid.nx, grid.ny, grid.nz, neigh);
  for (int t = 0; t < nn; ++t)
  {
    const std::size_t m = neigh[t];
    if (!processed[m])
      continue;
    const std::uint32_t r = dsu.find(static_cast<std::uint32_t>(m));
    if (std::find(roots, roots + nroots, r) == roots + nroots)
      roots[nroots++] = r;
  }
  return nroots;
}

// c joins the first component (its value can only be <= those peaks); the
// other components merge into it at saddle c, the lower peak of each merge dies
void mergeAtSaddle(std::uint32_t c,
                   const std::uint32_t (&roots)[6],
                   int nroots,
                   DisjointSet& dsu,
                   const WorkValue& work,
                   std::vector<Death>& deaths)
{
  std::uint32_t r0 = roots[0];
  dsu.attach(c, r0);
  for (int q = 1; q < nroots; ++q)
  {
    std::uint32_t rk = dsu.find(roots[q]);
    r0 = dsu.find(r0);
    if (rk == r0)
      continue;
    // Merge: the lower peak dies at this saddle (value of c).
    const std::uint32_t pk0 = dsu.peak(r0);
    const std::uint32_t pkk = dsu.peak(rk);
    std::uint32_t survivor = r0;
    std::uint32_t dier = rk;
    std::uint32_t dierPeak = pkk;
    if (work(pkk) > work(pk0) || (work(pkk) == work(pk0) && pkk < pk0))
    {
      survivor = rk;
      dier = r0;
      dierPeak = pk0;
    }
    deaths.push_back({dierPeak, work(dierPeak), work(c)});
    dsu.attach(dier, survivor);
    dsu.setPeak(survivor, (survivor == r0) ? pk0 : pkk);
    if (work(dsu.peak(survivor)) < work(dierPeak))
      dsu.setPeak(survivor, dierPeak);
    r0 = survivor;
  }
}

// The feature of a dead (or the surviving) component
Feature makeFeature(
    const Death& d, ExtremumKind kind, float sign, float globalMinWork, const VolumeGrid& grid)
{
  Feature f;
  f.kind = kind;
  f.peakCell = d.peakCell;
  f.value = sign * d.peakWork;     // back to original sign
  f.saddle = sign * d.saddleWork;  // back to original sign
  f.persistence = d.peakWork - d.saddleWork;
  f.isGlobal = (d.saddleWork == globalMinWork);
  const std::size_t pc = d.peakCell;
  const std::size_t slice = grid.sliceCount();
  const std::size_t s = pc % slice;
  f.lat = (grid.lats.size() == slice) ? grid.lats[s] : 0.0;
  f.lon = (grid.lons.size() == slice) ? grid.lons[s] : 0.0;
  f.heightMeters = (grid.heights.size() == grid.cellCount()) ? grid.heights[pc] : 0.0;
  return f;
}

// Blob = the superlevel component containing the peak just before it
// merges: flood from the peak over cells whose working value strictly
// exceeds the merge saddle (so it stops exactly at the saddle).
// visited is all zero on entry and on return.
void floodBlob(Feature& f,
               const VolumeGrid& grid,
               const WorkValue& work,
               float saddleWork,
               std::size_t blobCellCap,
               std::vector<char>& visited,
               std::vector<std::size_t>& stack)
{
  std::size_t neigh[6];
  stack.clear();
  stack.push_back(f.peakCell);
  visited[f.peakCell] = 1;
  f.blob.clear();
  while (!stack.empty())
  {
    const std::size_t c = stack.back();
    stack.pop_back();
    f.blob.push_back(c);
    if (f.blob.size() >= blobCellCap)
    {
      f.blobTruncated = true;
      break;
    }
    const int nn = neighboursOf(c, grid.nx, grid.ny, grid.nz, neigh);
    for (int t = 0; t < nn; ++t)
    {
      const std::size_t m = neigh[t];
      if (visited[m] || !std::isfinite(grid.values[m]))
        continue;
      if (work(m) > saddleWork)
      {
        visited[m] = 1;
        stack.push_back(m);
      }
    }
  }
  for (const std::size_t c : f.blob)  // reset for the next feature
    visited[c] = 0;
  while (!stack.empty())  // any cells left visited but unpopped (cap hit)
  {
    visited[stack.back()] = 0;
    stack.pop_back();
  }
}
}  // namespace

void detrendPerLevelMedian(VolumeGrid& grid)
{
  const std::size_t slice = grid.sliceCount();
  if (slice == 0)
    return;

  // cos-lat weights are level-invariant; compute once.
  std::vector<double> weight(slice, 1.0);
  if (grid.lats.size() == slice)
    for (std::size_t s = 0; s < slice; ++s)
    {
      const double w = std::cos(grid.lats[s] * M_PI / 180.0);
      weight[s] = (std::isfinite(w) && w > 0.0) ? w : 0.0;
    }

  std::vector<std::pair<float, double>> samples;  // (value, weight)
  samples.reserve(slice);
  for (std::size_t k = 0; k < grid.nz; ++k)
  {
    samples.clear();
    double total = 0.0;
    for (std::size_t s = 0; s < slice; ++s)
    {
      const float v = grid.values[k * slice + s];
      if (!std::isfinite(v) || weight[s] <= 0.0)
        continue;
      samples.emplace_back(v, weight[s]);
      total += weight[s];
    }
    if (samples.empty() || total <= 0.0)
      continue;
    std::sort(samples.begin(), samples.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    // Weighted 50th percentile: walk cumulative weight to the half point.
    const double half = total * 0.5;
    double acc = 0.0;
    float median = samples.back().first;
    for (const auto& [val, w] : samples)
    {
      acc += w;
      if (acc >= half)
      {
        median = val;
        break;
      }
    }
    for (std::size_t s = 0; s < slice; ++s)
    {
      float& v = grid.values[k * slice + s];
      if (std::isfinite(v))
        v -= median;
    }
  }
}

std::vector<Feature> findExtrema(const VolumeGrid& grid,
                                 ExtremumKind kind,
                                 float minPersistence,
                                 std::size_t maxFeatures,
                                 std::size_t blobCellCap)
{
  const std::size_t n = grid.cellCount();
  std::vector<Feature> out;
  if (n == 0 || grid.values.size() != n)
    return out;

  // Work on a sign-flipped copy for minima so the same "sweep from the top,
  // peaks are born, the lower one dies at a merge" logic finds both.
  const float sign = (kind == ExtremumKind::Max) ? 1.0F : -1.0F;
  const WorkValue work{grid, sign};

  // Finite cells, sorted by descending working value (ties by index, a
  // deterministic stand-in for simulation-of-simplicity on plateaus).
  std::vector<std::uint32_t> order;
  order.reserve(n);
  float globalMinWork = std::numeric_limits<float>::infinity();
  for (std::size_t c = 0; c < n; ++c)
    if (std::isfinite(grid.values[c]))
    {
      order.push_back(static_cast<std::uint32_t>(c));
      globalMinWork = std::min(globalMinWork, work(c));
    }
  if (order.empty())
    return out;
  std::sort(order.begin(), order.end(),
            [&](std::uint32_t a, std::uint32_t b)
            {
              const float wa = work(a);
              const float wb = work(b);
              if (wa != wb)
                return wa > wb;
              return a < b;
            });

  DisjointSet dsu(n);
  std::vector<char> processed(n, 0);

  std::vector<Death> deaths;

  std::size_t neigh[6];
  for (const std::uint32_t c : order)
  {
    // Distinct roots among already-processed neighbours.
    std::uint32_t roots[6];
    const int nroots = processedRoots(c, grid, dsu, processed, neigh, roots);
    if (nroots == 0)
    {
      // Birth of a new local extremum: c is its own component, peak = c.
      dsu.setPeak(c, c);
    }
    else
      mergeAtSaddle(c, roots, nroots, dsu, work, deaths);
    processed[c] = 1;
  }

  // The single surviving component is the global extremum (never merges).
  std::vector<Death> all = std::move(deaths);
  {
    const std::uint32_t root = dsu.find(order.front());
    all.push_back({dsu.peak(root), work(dsu.peak(root)), globalMinWork});
  }

  // Build features, filtering by persistence.
  for (const auto& d : all)
  {
    const float persistence = d.peakWork - d.saddleWork;
    if (persistence < minPersistence)
      continue;
    out.push_back(makeFeature(d, kind, sign, globalMinWork, grid));
  }

  std::sort(out.begin(), out.end(),
            [](const Feature& a, const Feature& b) { return a.persistence > b.persistence; });
  if (maxFeatures != 0 && out.size() > maxFeatures)
    out.resize(maxFeatures);

  // Flood the blob of each feature.
  std::vector<char> visited(n, 0);
  std::vector<std::size_t> stack;
  for (auto& f : out)
    floodBlob(f, grid, work, sign * f.saddle, blobCellCap, visited, stack);

  return out;
}
}  // namespace Qdless
