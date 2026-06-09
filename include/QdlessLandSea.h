#pragma once

#include <string>
#include <vector>

namespace Qdless
{
// Land/sea classifier built from a binned GSHHS-format NetCDF file
// (gshhg-gmt-nc4) — the same data qdless already loads for coastlines.
//
// Why this is possible without re-implementing GMT's bin-polygon assembly:
// the binned segments are *continuous across bin walls* (one bin's exit point
// is the neighbouring bin's entry point on the shared wall), so concatenating
// every segment reproduces the globally-closed GSHHS polygons. That lets a
// plain even-odd scanline fill — keyed on each segment's GSHHS level
// (1=land, 2=lake, 3=island-in-lake, 4=pond; odd ⇒ land) — rasterise a correct
// land mask. No corner-node levels, no entry/exit wall codes, no stitching.
//
// The one wrinkle is polygons that enclose a pole (Antarctica): on a cyclic
// latitude circle they cross an *odd* number of times, so the per-row baseline
// "inside-ness" is taken from the parity of the total crossing count, which
// makes the pole interior fill correctly with no special-casing.
class LandSea
{
 public:
  // Build the mask from `filename` at `rows` latitude rows (×2 longitude
  // columns). Returns false (and leaves the mask empty / invalid) if the file
  // is missing or unreadable, so callers degrade gracefully.
  bool build(const std::string& filename, int rows);

  bool valid() const { return itsRows > 0 && itsCols > 0; }

  // True if (lat,lon) is land. lat in [-90,90], lon in [-180,180].
  // Returns false when the mask is invalid.
  bool isLand(double lat, double lon) const;

  int rows() const { return itsRows; }
  int cols() const { return itsCols; }

 private:
  int itsRows = 0;
  int itsCols = 0;
  std::vector<unsigned char> itsMask;  // rows*cols, 1 = land, 0 = sea
};
}  // namespace Qdless
