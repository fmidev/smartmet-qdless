#include "QdlessLandSea.h"

#include <netcdf>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>

namespace Qdless
{
namespace
{
template <typename T>
std::vector<T> readVar(const netCDF::NcFile& nc, const char* name, std::size_t n)
{
  std::vector<T> out(n);
  nc.getVar(name).getVar(out.data());
  return out;
}

int readScalar(const netCDF::NcFile& nc, const char* name)
{
  int v = 0;
  nc.getVar(name).getVar(&v);
  return v;
}

// One GSHHS segment: a polyline plus its level (1=land, 2=lake, 3=island in
// lake, 4=pond). Odd levels are land.
struct Seg
{
  std::vector<float> lon;
  std::vector<float> lat;
  int level = 0;
};
}  // namespace

bool LandSea::build(const std::string& filename, int rows)
{
  itsRows = 0;
  itsCols = 0;
  itsMask.clear();
  if (rows < 2 || !std::filesystem::exists(filename))
    return false;

  netCDF::NcFile nc;
  try
  {
    nc.open(filename, netCDF::NcFile::read);
  }
  catch (const std::exception&)
  {
    return false;
  }

  std::vector<Seg> segs;
  try
  {
    const int binSize = readScalar(nc, "Bin_size_in_minutes");
    const int nLon = readScalar(nc, "N_bins_in_360_longitude_range");
    const int nLat = readScalar(nc, "N_bins_in_180_degree_latitude_range");
    const int nBins = nLon * nLat;
    const int nSegs = readScalar(nc, "N_segments_in_file");
    const int nPts = readScalar(nc, "N_points_in_file");
    if (nBins <= 0 || nSegs <= 0 || nPts <= 0)
      return false;

    auto firstSegInBin = readVar<int>(nc, "Id_of_first_segment_in_a_bin", nBins);
    auto nSegsInBin = readVar<short>(nc, "N_segments_in_a_bin", nBins);
    auto firstPtInSeg = readVar<int>(nc, "Id_of_first_point_in_a_segment", nSegs);
    auto relLon = readVar<short>(nc, "Relative_longitude_from_SW_corner_of_bin", nPts);
    auto relLat = readVar<short>(nc, "Relative_latitude_from_SW_corner_of_bin", nPts);
    // Packed per segment as (npts<<9)|(level<<6)|(entry<<3)|exit — verified
    // empirically against known bins; only `level` is needed here.
    auto segInfo = readVar<int>(nc, "Embedded_npts_levels_exit_entry_for_a_segment", nSegs);

    const float binDeg = static_cast<float>(binSize) / 60.0F;
    const float scale = binDeg / 65535.0F;

    segs.reserve(static_cast<std::size_t>(nSegs));
    for (int b = 0; b < nBins; ++b)
    {
      if (nSegsInBin[b] <= 0)
        continue;
      const int latBin = b / nLon;
      const int lonBin = b % nLon;
      const float latSW = 90.0F - static_cast<float>(latBin + 1) * binDeg;
      // Shift the bin origin into [-180,180) rather than wrapping each point:
      // 180° is always a bin boundary, so this keeps every bin's longitudes
      // continuous. Per-point wrapping instead leaves the lon==180 vertex at
      // +180 while its neighbour flips to -180, which fabricates a full-width
      // horizontal edge that crosses every meridian (a scanline-only hazard;
      // 3D coastline drawing is immune because ±180 map to the same point).
      float lonSW = static_cast<float>(lonBin) * binDeg;
      if (lonSW >= 180.0F)
        lonSW -= 360.0F;

      const int firstSeg = firstSegInBin[b];
      const int lastSeg = firstSeg + nSegsInBin[b];
      for (int s = firstSeg; s < lastSeg; ++s)
      {
        const int level = (segInfo[s] >> 6) & 7;
        if (level < 1)
          continue;
        const int firstPt = firstPtInSeg[s];
        const int lastPt = (s + 1 < nSegs) ? firstPtInSeg[s + 1] : nPts;
        if (lastPt - firstPt < 2)
          continue;

        Seg seg;
        seg.level = level;
        seg.lon.reserve(static_cast<std::size_t>(lastPt - firstPt));
        seg.lat.reserve(static_cast<std::size_t>(lastPt - firstPt));
        for (int p = firstPt; p < lastPt; ++p)
        {
          const auto u = static_cast<float>(static_cast<std::uint16_t>(relLon[p]));
          const auto v = static_cast<float>(static_cast<std::uint16_t>(relLat[p]));
          const float lon = lonSW + u * scale;
          const float lat = latSW + v * scale;
          seg.lon.push_back(lon);
          seg.lat.push_back(lat);
        }
        segs.push_back(std::move(seg));
      }
    }
  }
  catch (const std::exception&)
  {
    return false;  // not a GSHHS shoreline file (borders/rivers lack the arrays)
  }

  if (segs.empty())
    return false;

  const int cols = rows * 2;
  std::vector<unsigned char> mask(static_cast<std::size_t>(rows) * cols, 0);

  // Fill along meridians (constant longitude, marching north from the south
  // pole). The pole states are known — the south pole is land (Antarctica is
  // level 1), the north pole is open sea — so there is no baseline to guess,
  // and a fixed-longitude ray never spans the ±180 seam. This is what makes
  // Antarctica and dateline-straddling landmasses come out right.
  std::vector<float> cross[5];  // crossing latitudes for the current meridian

  for (int c = 0; c < cols; ++c)
  {
    const double lonC = -180.0 + (c + 0.5) * 360.0 / cols;
    for (int lv = 1; lv <= 4; ++lv)
      cross[lv].clear();

    for (const auto& seg : segs)
    {
      const int lv = seg.level;
      if (lv < 1 || lv > 4)
        continue;
      const std::size_t n = seg.lon.size();
      for (std::size_t i = 1; i < n; ++i)
      {
        const double x0 = seg.lon[i - 1];
        const double x1 = seg.lon[i];
        // Half-open span test counts a shared vertex once. Intra-bin edges
        // never straddle the antimeridian, so a plain compare is safe.
        if ((x0 <= lonC) == (x1 <= lonC))
          continue;
        const double y0 = seg.lat[i - 1];
        const double y1 = seg.lat[i];
        const double t = (lonC - x0) / (x1 - x0);
        cross[lv].push_back(static_cast<float>(y0 + t * (y1 - y0)));
      }
    }

    for (int lv = 1; lv <= 4; ++lv)
      std::sort(cross[lv].begin(), cross[lv].end());

    // Antarctica is stored with GMT's ANT flag as open arcs that close only at
    // the pole, so it never forms clean even-odd crossings. Everything south of
    // the southernmost coastline crossing on this meridian (when that crossing
    // lies in polar latitudes) is therefore land — Antarctic interior.
    constexpr double kAntLat = -55.0;
    double antCoast = -90.0;
    if (!cross[1].empty() && cross[1].front() < kAntLat)
      antCoast = cross[1].front();  // sorted ascending → first = southernmost

    for (int r = 0; r < rows; ++r)
    {
      const double latR = -90.0 + (r + 0.5) * 180.0 / rows;
      if (latR <= antCoast)
      {
        mask[static_cast<std::size_t>(r) * cols + c] = 1U;  // Antarctic interior
        continue;
      }
      int level = 0;
      for (int lv = 1; lv <= 4; ++lv)
      {
        const auto& cs = cross[lv];
        // March from the north pole (open sea): every level starts outside, so
        // a cell is inside level lv iff an odd number of lv-crossings lie north
        // of it. Seam-safe because the ray is a fixed meridian.
        const std::size_t below = static_cast<std::size_t>(
            std::upper_bound(cs.begin(), cs.end(), static_cast<float>(latR)) - cs.begin());
        const std::size_t above = cs.size() - below;
        if ((above & 1U) != 0U)
          level = lv;  // deepest enclosing level wins (levels nest)
      }
      mask[static_cast<std::size_t>(r) * cols + c] = (level & 1) ? 1U : 0U;  // odd = land
    }
  }

  itsRows = rows;
  itsCols = cols;
  itsMask = std::move(mask);
  return true;
}

bool LandSea::isLand(double lat, double lon) const
{
  if (!valid())
    return false;
  // Wrap into range so callers needn't normalise.
  while (lon < -180.0)
    lon += 360.0;
  while (lon >= 180.0)
    lon -= 360.0;
  lat = std::clamp(lat, -90.0, 90.0);
  int r = static_cast<int>((lat + 90.0) / 180.0 * itsRows);
  int c = static_cast<int>((lon + 180.0) / 360.0 * itsCols);
  r = std::clamp(r, 0, itsRows - 1);
  c = std::clamp(c, 0, itsCols - 1);
  return itsMask[static_cast<std::size_t>(r) * itsCols + c] != 0U;
}
}  // namespace Qdless
