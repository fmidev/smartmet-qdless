#pragma once

#include <string>
#include <vector>

namespace Qdless
{
struct Polyline
{
  std::vector<float> lons;  // -180..180
  std::vector<float> lats;  // -90..90
};

class Coastline
{
 public:
  // Loads polylines from a binned GSHHS-format NetCDF file (gshhg-gmt-nc4).
  // Works for binned_GSHHS_*.nc, binned_border_*.nc and binned_river_*.nc.
  // Returns empty vector if the file is missing — callers may degrade
  // gracefully when the data package isn't installed.
  //
  // GSHHS files: filter polygons by area + roundness.
  //
  // Top-level polygons (parent == -1, i.e. continents and islands):
  //   - kept iff area >= minIslandAreaKm2
  //
  // Nested polygons (parent != -1, i.e. lakes / islands-in-lakes / ponds):
  //   - kept iff area >= minLakeAreaKm2  AND  4πA/L² >= minLakeRoundness
  //
  // Compactness 4πA/L² is 1.0 for a circle, ~0.785 for a square, near 0 for
  // highly fractal shorelines (e.g. Saimaa).
  //
  // Pass 0 to disable an individual filter. Border / river files ignore all.
  static std::vector<Polyline> read(const std::string& filename,
                                    double minLakeAreaKm2 = 0.0,
                                    double minLakeRoundness = 0.0,
                                    double minIslandAreaKm2 = 0.0);

  // Pick the best-matching resolution file from a directory.
  //   dir   – e.g. "/usr/share/gshhg-gmt-nc4"
  //   kind  – "GSHHS", "border" or "river"
  //   span  – max(lonSpan, latSpan) in degrees
  // Returns "" if no suitable file is found.
  static std::string pickFile(const std::string& dir, const std::string& kind, float span);
};
}  // namespace Qdless
