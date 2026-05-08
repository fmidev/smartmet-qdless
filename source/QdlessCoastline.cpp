#include "QdlessCoastline.h"

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

  const int binSize = readScalar(nc, "Bin_size_in_minutes");
  const int nLon = readScalar(nc, "N_bins_in_360_longitude_range");
  const int nLat = readScalar(nc, "N_bins_in_180_degree_latitude_range");
  const int nBins = nLon * nLat;
  const int nSegs = readScalar(nc, "N_segments_in_file");
  const int nPts = readScalar(nc, "N_points_in_file");
  if (nBins <= 0 || nSegs <= 0 || nPts <= 0) return {};

  auto firstSegInBin = readVar<int>(nc, "Id_of_first_segment_in_a_bin", nBins);
  auto nSegsInBin = readVar<short>(nc, "N_segments_in_a_bin", nBins);
  auto firstPtInSeg = readVar<int>(nc, "Id_of_first_point_in_a_segment", nSegs);
  auto relLon = readVar<short>(nc, "Relative_longitude_from_SW_corner_of_bin", nPts);
  auto relLat = readVar<short>(nc, "Relative_latitude_from_SW_corner_of_bin", nPts);

  // Polygon-level metadata is only present in GSHHS shoreline files.
  // Borders / rivers don't have polygon arrays.
  std::vector<int> polyId;
  std::vector<int> polyParent;
  std::vector<double> polyArea;
  std::vector<double> polyPerim;  // perimeter in km, summed across all segments
  int nPoly = 0;
  bool hasPolygonMeta = false;
  bool wantRoundness = (minLakeRoundness > 0.0);
  if (minLakeAreaKm2 > 0.0 || wantRoundness || minIslandAreaKm2 > 0.0)
  {
    try
    {
      nPoly = readScalar(nc, "N_polygons_in_file");
      if (nPoly > 0)
      {
        polyId = readVar<int>(nc, "Id_of_GSHHS_ID", nSegs);
        polyParent = readVar<int>(nc, "Id_of_parent_polygons", nPoly);
        polyArea = readVar<double>(nc, "The_km_squared_area_of_polygons", nPoly);
        hasPolygonMeta = true;
      }
    }
    catch (const std::exception&)
    {
      hasPolygonMeta = false;  // not a GSHHS file
    }
  }

  const float binDeg = static_cast<float>(binSize) / 60.0F;  // 20.0 for crude
  const float scale = binDeg / 65535.0F;

  // Pass 1: accumulate per-polygon perimeter so we can compute roundness.
  // This walks every segment in the file once; cheap relative to the I/O.
  if (hasPolygonMeta && wantRoundness)
  {
    polyPerim.assign(nPoly, 0.0);
    for (int b = 0; b < nBins; ++b)
    {
      if (nSegsInBin[b] <= 0) continue;
      const int latBin = b / nLon;
      const int lonBin = b % nLon;
      const float latSW = 90.0F - static_cast<float>(latBin + 1) * binDeg;
      const float lonSW = static_cast<float>(lonBin) * binDeg;
      const int firstSeg = firstSegInBin[b];
      const int lastSeg = firstSeg + nSegsInBin[b];
      for (int s = firstSeg; s < lastSeg; ++s)
      {
        const int firstPt = firstPtInSeg[s];
        const int lastPt = (s + 1 < nSegs) ? firstPtInSeg[s + 1] : nPts;
        if (lastPt - firstPt < 2) continue;

        double len = 0.0;
        auto absLon = [&](int p) {
          return lonSW + static_cast<float>(static_cast<std::uint16_t>(relLon[p])) * scale;
        };
        auto absLat = [&](int p) {
          return latSW + static_cast<float>(static_cast<std::uint16_t>(relLat[p])) * scale;
        };
        float prevLon = absLon(firstPt);
        float prevLat = absLat(firstPt);
        for (int p = firstPt + 1; p < lastPt; ++p)
        {
          const float lon = absLon(p);
          const float lat = absLat(p);
          const double avgLatRad = (lat + prevLat) * 0.5 * M_PI / 180.0;
          const double dLon = (lon - prevLon) * std::cos(avgLatRad);
          const double dLat = lat - prevLat;
          // 1° at the equator ≈ 111.32 km.
          len += std::sqrt(dLon * dLon + dLat * dLat) * 111.32;
          prevLon = lon;
          prevLat = lat;
        }
        const int pid = polyId[s];
        if (pid >= 0 && pid < nPoly) polyPerim[pid] += len;
      }
    }
  }

  std::vector<Polyline> out;
  out.reserve(static_cast<std::size_t>(nSegs));

  for (int b = 0; b < nBins; ++b)
  {
    if (nSegsInBin[b] <= 0) continue;
    const int latBin = b / nLon;
    const int lonBin = b % nLon;
    // GSHHG indexes lat bins top-down: bin 0 spans [70, 90], bin 1 [50, 70], ...
    const float latSW = 90.0F - static_cast<float>(latBin + 1) * binDeg;
    const float lonSW = static_cast<float>(lonBin) * binDeg;  // 0..360

    const int firstSeg = firstSegInBin[b];
    const int lastSeg = firstSeg + nSegsInBin[b];
    for (int s = firstSeg; s < lastSeg; ++s)
    {
      // Polygon filtering — different criteria for continents/islands
      // (top-level) vs. lakes/etc. (nested).
      if (hasPolygonMeta)
      {
        const int pid = polyId[s];
        if (pid >= 0 && pid < nPoly)
        {
          const double area = polyArea[pid];
          if (polyParent[pid] == -1)
          {
            // Top-level: continent or island. Drop if too small.
            if (area < minIslandAreaKm2) continue;
          }
          else
          {
            // Nested: lake / island-in-lake / pond.
            if (area < minLakeAreaKm2) continue;
            if (wantRoundness)
            {
              const double L = polyPerim[pid];
              const double roundness = (L > 0) ? (4.0 * M_PI * area / (L * L)) : 0.0;
              if (roundness < minLakeRoundness) continue;
            }
          }
        }
      }

      const int firstPt = firstPtInSeg[s];
      const int lastPt = (s + 1 < nSegs) ? firstPtInSeg[s + 1] : nPts;
      if (lastPt <= firstPt) continue;

      Polyline pl;
      pl.lons.reserve(static_cast<std::size_t>(lastPt - firstPt));
      pl.lats.reserve(static_cast<std::size_t>(lastPt - firstPt));
      for (int p = firstPt; p < lastPt; ++p)
      {
        // Raw shorts encode unsigned 0..65535 offsets; reinterpret as unsigned.
        const auto u = static_cast<float>(static_cast<std::uint16_t>(relLon[p]));
        const auto v = static_cast<float>(static_cast<std::uint16_t>(relLat[p]));
        float lon = lonSW + u * scale;
        float lat = latSW + v * scale;
        if (lon > 180.0F) lon -= 360.0F;
        pl.lons.push_back(lon);
        pl.lats.push_back(lat);
      }
      out.push_back(std::move(pl));
    }
  }
  return out;
}

std::string Coastline::pickFile(const std::string& dir,
                                const std::string& kind,
                                float span)
{
  // Pick the densest resolution still appropriate for the viewport span.
  // c (crude) ≈ 25 km, l (low) ≈ 5 km, i (intermediate) ≈ 1 km, h (high) ≈ 200 m.
  const char* res = "c";
  if (span < 3.0F)
    res = "h";
  else if (span < 15.0F)
    res = "i";
  else if (span < 60.0F)
    res = "l";

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
