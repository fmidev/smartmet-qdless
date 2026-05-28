#pragma once

#include "QdlessDataSource.h"

#include <memory>
#include <string>
#include <vector>

class NFmiArea;

namespace Qdless
{
// DataSource for any raster GDAL can open with an axis-aligned geotransform
// and a known SRS. Originally a GeoTIFF-only path; broadened so JPEG2000,
// COG, NITF, HDF4 subdatasets, GeoPackage rasters, etc. all flow through
// the same code. grid-files keeps owning GRIB and NetCDF — they have a
// richer time/level model than the single-frame view here.
//
// One file == one parameter, one timestep, one level. The valid time and
// the gain/offset/nodata/undetect quartet are read from the GDAL_METADATA
// item when present — FMI radar GeoTIFFs encode an XML blob there in
// ODIM-style. When the metadata is missing, the valid time falls back to a
// leading YYYYMMDDHHMM in the filename, then to mtime, all UTC. Convention
// is end-of-accumulation per meteorological practice.
class GdalRasterSource : public DataSource
{
 public:
  explicit GdalRasterSource(const std::string& filename);
  ~GdalRasterSource() override;
  GdalRasterSource(const GdalRasterSource&) = delete;
  GdalRasterSource& operator=(const GdalRasterSource&) = delete;
  GdalRasterSource(GdalRasterSource&&) = delete;
  GdalRasterSource& operator=(GdalRasterSource&&) = delete;

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

  float interpolatedValue(double lat, double lon) const override;
  LatLonBox boundingBox() const override;
  void uvToLatLon(double u, double v, double& lat, double& lon) const override;
  void latLonToUV(double lat, double lon, double& u, double& v) const override;

  std::vector<std::pair<std::string, std::string>> extraMetadata() const override;
  std::string gridSignature() const override;

 private:
  std::string itsFilename;
  std::unique_ptr<NFmiArea> itsArea;
  std::vector<float> itsValues;
  std::size_t itsNx = 0;
  std::size_t itsNy = 0;
  int itsParamId = 0;        // 0 == use literal paramName
  std::string itsParamName;  // derived from filename if no enum match
  std::string itsParamUnits;
  std::string itsLabel;      // suffix after the YYYYMMDDHHMM_ prefix, sans ".tif"
  std::string itsWkt;        // for the metadata popup
  std::string itsTemporalType;     // "Past" / "Future" / "Instant" if metadata present
  std::string itsAccumulation;     // e.g. "1 h" if metadata present
  NFmiMetTime itsValidTime;
  double itsGain = 1.0;
  double itsOffset = 0.0;
  double itsNodata = 0.0;
  double itsUndetect = 0.0;
  bool itsHasNodata = false;
  bool itsHasUndetect = false;
  // GeoTransform (a..f) from GDAL: x = a + col*b + row*c, y = d + col*e + row*f.
  // We only support axis-aligned transforms (b ≠ 0, c = 0, e = 0, f ≠ 0).
  double itsOriginX = 0;
  double itsOriginY = 0;
  double itsPixelW = 0;
  double itsPixelH = 0;  // negative when row 0 is the northern edge
};
}  // namespace Qdless
