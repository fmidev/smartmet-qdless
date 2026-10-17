#include "QdlessCoastline.h"

#include <gis/OGR.h>
#include <netcdf>

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
}  // namespace

namespace
{
// The bin index of a GSHHG binned NetCDF file
struct BinnedFile
{
  int nLon = 0;
  int nBins = 0;
  int nSegs = 0;
  int nPts = 0;
  float binDeg = 0;
  float scale = 0;
  std::vector<int> firstSegInBin;
  std::vector<short> nSegsInBin;
  std::vector<int> firstPtInSeg;
  std::vector<short> relLon;
  std::vector<short> relLat;

  int lastPt(int s) const { return (s + 1 < nSegs) ? firstPtInSeg[s + 1] : nPts; }
  float lon(int p, float lonSW) const
  {
    return lonSW + static_cast<float>(static_cast<std::uint16_t>(relLon[p])) * scale;
  }
  float lat(int p, float latSW) const
  {
    return latSW + static_cast<float>(static_cast<std::uint16_t>(relLat[p])) * scale;
  }
};

// Read the bin index; false if the file has no bins, segments or points
bool readBinnedFile(const netCDF::NcFile& nc, BinnedFile& f)
{
  const int binSize = readScalar(nc, "Bin_size_in_minutes");
  f.nLon = readScalar(nc, "N_bins_in_360_longitude_range");
  const int nLat = readScalar(nc, "N_bins_in_180_degree_latitude_range");
  f.nBins = f.nLon * nLat;
  f.nSegs = readScalar(nc, "N_segments_in_file");
  f.nPts = readScalar(nc, "N_points_in_file");
  if (f.nBins <= 0 || f.nSegs <= 0 || f.nPts <= 0) return false;

  f.firstSegInBin = readVar<int>(nc, "Id_of_first_segment_in_a_bin", f.nBins);
  f.nSegsInBin = readVar<short>(nc, "N_segments_in_a_bin", f.nBins);
  f.firstPtInSeg = readVar<int>(nc, "Id_of_first_point_in_a_segment", f.nSegs);
  f.relLon = readVar<short>(nc, "Relative_longitude_from_SW_corner_of_bin", f.nPts);
  f.relLat = readVar<short>(nc, "Relative_latitude_from_SW_corner_of_bin", f.nPts);

  f.binDeg = static_cast<float>(binSize) / 60.0F;  // 20.0 for crude
  f.scale = f.binDeg / 65535.0F;
  return true;
}

// Call fn(segment, lonSW, latSW) for every segment of every non-empty bin
template <typename Fn>
void forEachSegment(const BinnedFile& f, Fn&& fn)
{
  for (int b = 0; b < f.nBins; ++b)
  {
    if (f.nSegsInBin[b] <= 0) continue;
    const int latBin = b / f.nLon;
    const int lonBin = b % f.nLon;
    // GSHHG indexes lat bins top-down: bin 0 spans [70, 90], bin 1 [50, 70], ...
    const float latSW = 90.0F - static_cast<float>(latBin + 1) * f.binDeg;
    const float lonSW = static_cast<float>(lonBin) * f.binDeg;  // 0..360
    const int firstSeg = f.firstSegInBin[b];
    const int lastSeg = firstSeg + f.nSegsInBin[b];
    for (int s = firstSeg; s < lastSeg; ++s)
      fn(s, lonSW, latSW);
  }
}

// Length of a segment in km
double segmentLengthKm(const BinnedFile& f, int firstPt, int lastPt, float lonSW, float latSW)
{
  double len = 0.0;
  float prevLon = f.lon(firstPt, lonSW);
  float prevLat = f.lat(firstPt, latSW);
  for (int p = firstPt + 1; p < lastPt; ++p)
  {
    const float lon = f.lon(p, lonSW);
    const float lat = f.lat(p, latSW);
    const double avgLatRad = (lat + prevLat) * 0.5 * M_PI / 180.0;
    const double dLon = (lon - prevLon) * std::cos(avgLatRad);
    const double dLat = lat - prevLat;
    // 1° at the equator ≈ 111.32 km.
    len += std::sqrt(dLon * dLon + dLat * dLat) * 111.32;
    prevLon = lon;
    prevLat = lat;
  }
  return len;
}

// Per-polygon perimeter (km) summed across all segments, for roundness.
// This walks every segment in the file once; cheap relative to the I/O.
std::vector<double> polygonPerimeters(const BinnedFile& f, const std::vector<int>& polyId, int nPoly)
{
  std::vector<double> polyPerim(nPoly, 0.0);
  forEachSegment(f,
                 [&](int s, float lonSW, float latSW)
                 {
                   const int firstPt = f.firstPtInSeg[s];
                   const int lastPt = f.lastPt(s);
                   if (lastPt - firstPt < 2) return;
                   const double len = segmentLengthKm(f, firstPt, lastPt, lonSW, latSW);
                   const int pid = polyId[s];
                   if (pid >= 0 && pid < nPoly) polyPerim[pid] += len;
                 });
  return polyPerim;
}

// GSHHS polygon metadata and the size / roundness filter thresholds
struct PolygonFilter
{
  std::vector<int> polyId;
  std::vector<int> polyParent;
  std::vector<double> polyArea;
  std::vector<double> polyPerim;  // perimeter in km, summed across all segments
  int nPoly = 0;
  bool hasPolygonMeta = false;
  bool wantRoundness = false;
  double minLakeAreaKm2 = 0;
  double minLakeRoundness = 0;
  double minIslandAreaKm2 = 0;

  // Polygon filtering — different criteria for continents/islands
  // (top-level) vs. lakes/etc. (nested).
  bool keep(int s) const
  {
    if (!hasPolygonMeta) return true;
    const int pid = polyId[s];
    if (pid < 0 || pid >= nPoly) return true;
    const double area = polyArea[pid];
    // Top-level: continent or island. Drop if too small.
    if (polyParent[pid] == -1) return !(area < minIslandAreaKm2);
    // Nested: lake / island-in-lake / pond.
    if (area < minLakeAreaKm2) return false;
    return !(wantRoundness && Fmi::OGR::compactness(area, polyPerim[pid]) < minLakeRoundness);
  }
};

// Polygon-level metadata is only present in GSHHS shoreline files.
// Borders / rivers don't have polygon arrays.
void readPolygonMeta(const netCDF::NcFile& nc, int nSegs, PolygonFilter& pf)
{
  try
  {
    pf.nPoly = readScalar(nc, "N_polygons_in_file");
    if (pf.nPoly > 0)
    {
      pf.polyId = readVar<int>(nc, "Id_of_GSHHS_ID", nSegs);
      pf.polyParent = readVar<int>(nc, "Id_of_parent_polygons", pf.nPoly);
      pf.polyArea = readVar<double>(nc, "The_km_squared_area_of_polygons", pf.nPoly);
      pf.hasPolygonMeta = true;
    }
  }
  catch (const std::exception&)
  {
    pf.hasPolygonMeta = false;  // not a GSHHS file
  }
}

// The points of a segment as a polyline with longitudes in -180..180
Polyline segmentPolyline(const BinnedFile& f, int firstPt, int lastPt, float lonSW, float latSW)
{
  Polyline pl;
  pl.lons.reserve(static_cast<std::size_t>(lastPt - firstPt));
  pl.lats.reserve(static_cast<std::size_t>(lastPt - firstPt));
  for (int p = firstPt; p < lastPt; ++p)
  {
    // Raw shorts encode unsigned 0..65535 offsets; reinterpret as unsigned.
    const auto u = static_cast<float>(static_cast<std::uint16_t>(f.relLon[p]));
    const auto v = static_cast<float>(static_cast<std::uint16_t>(f.relLat[p]));
    float lon = lonSW + u * f.scale;
    float lat = latSW + v * f.scale;
    if (lon > 180.0F) lon -= 360.0F;
    pl.lons.push_back(lon);
    pl.lats.push_back(lat);
  }
  return pl;
}
}  // namespace

std::vector<Polyline> Coastline::read(const std::string& filename, double minLakeAreaKm2,
                                      double minLakeRoundness, double minIslandAreaKm2)
{
  if (!std::filesystem::exists(filename)) return {};

  netCDF::NcFile nc;
  try
  {
    nc.open(filename, netCDF::NcFile::read);
  }
  catch (const std::exception&)
  {
    return {};
  }

  BinnedFile f;
  if (!readBinnedFile(nc, f)) return {};

  PolygonFilter pf;
  pf.wantRoundness = (minLakeRoundness > 0.0);
  pf.minLakeAreaKm2 = minLakeAreaKm2;
  pf.minLakeRoundness = minLakeRoundness;
  pf.minIslandAreaKm2 = minIslandAreaKm2;
  if (minLakeAreaKm2 > 0.0 || pf.wantRoundness || minIslandAreaKm2 > 0.0)
    readPolygonMeta(nc, f.nSegs, pf);

  // Pass 1: accumulate per-polygon perimeter so we can compute roundness.
  if (pf.hasPolygonMeta && pf.wantRoundness)
    pf.polyPerim = polygonPerimeters(f, pf.polyId, pf.nPoly);

  std::vector<Polyline> out;
  out.reserve(static_cast<std::size_t>(f.nSegs));
  forEachSegment(f,
                 [&](int s, float lonSW, float latSW)
                 {
                   if (!pf.keep(s)) return;
                   const int firstPt = f.firstPtInSeg[s];
                   const int lastPt = f.lastPt(s);
                   if (lastPt <= firstPt) return;
                   out.push_back(segmentPolyline(f, firstPt, lastPt, lonSW, latSW));
                 });
  return out;
}

std::string Coastline::pickFile(const std::string& dir,
                                const std::string& kind,
                                float degreesPerPix)
{
  // Approx GSHHS vertex spacings (deg of arc at the equator):
  //   c (crude)        ≈ 0.25°  (~25 km)
  //   l (low)          ≈ 0.05°  (~5 km)
  //   i (intermediate) ≈ 0.01°  (~1 km)
  //   h (high)         ≈ 0.002° (~200 m)
  // Pick the coarsest file whose vertex spacing is still ≤ pixel size,
  // so each vertex lands in its own pixel — anything coarser draws as
  // straight chords the user can see; anything denser is wasted detail.
  const char* res = "h";
  if (degreesPerPix >= 0.25F)
    res = "c";
  else if (degreesPerPix >= 0.05F)
    res = "l";
  else if (degreesPerPix >= 0.01F)
    res = "i";

  std::filesystem::path candidate = std::filesystem::path(dir) /
                                    ("binned_" + kind + "_" + res + ".nc");
  if (std::filesystem::exists(candidate)) return candidate.string();
  // Fall back: try denser then coarser resolutions.
  for (const auto* r : {"h", "i", "l", "c"})
  {
    candidate = std::filesystem::path(dir) / ("binned_" + kind + "_" + r + ".nc");
    if (std::filesystem::exists(candidate)) return candidate.string();
  }
  return {};
}
}  // namespace Qdless
