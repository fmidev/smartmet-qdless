#pragma once

#include "QdlessDataSource.h"

#include <memory>
#include <string>
#include <vector>

class NFmiArea;

namespace Fmi
{
namespace HDF5
{
class Hdf5File;
}
}  // namespace Fmi

namespace Qdless
{
// DataSource for EUMETNET OPERA ODIM HDF5 2D composites
// (/what/object ∈ {IMAGE, COMP, CVOL}). One file == one parameter, one
// timestep, one level. Polar volumes (object=PVOL) are not supported.
//
// Reuses qdtools' Hdf5File reader (GDAL HDF5 driver) and h5toqd's
// quantity → newbase parameter mapping. Applies gain*raw + offset on read,
// and maps both `nodata` and `undetect` to NaN so out-of-range cells render
// transparent (radar dBZ can legitimately be negative; clear-air should not
// paint a colour band).
class OdimSource : public DataSource
{
 public:
  explicit OdimSource(const std::string& filename);
  ~OdimSource() override;
  OdimSource(const OdimSource&) = delete;
  OdimSource& operator=(const OdimSource&) = delete;
  OdimSource(OdimSource&&) = delete;
  OdimSource& operator=(OdimSource&&) = delete;

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

  // True if the file at `filename` is an ODIM HDF5 (vs a NetCDF4 file
  // sharing the same HDF5 magic). Probes for /what/object presence.
  static bool isOdim(const std::string& filename);

 private:
  std::string itsFilename;
  std::unique_ptr<NFmiArea> itsArea;  // projection + grid extent
  std::vector<float> itsValues;       // row-major, top-to-bottom, length = nx*ny
  std::size_t itsNx = 0;
  std::size_t itsNy = 0;
  int itsParamId = 0;                 // newbase enum
  std::string itsParamUnits;
  std::string itsQuantity;            // ODIM quantity (TH, ACRR, RATE, ...)
  std::string itsProduct;             // ODIM product (PCAPPI, MAX, RR, ...)
  std::string itsObject;              // /what/object value
  std::string itsProjDef;             // /where/projdef
  NFmiMetTime itsValidTime;
  double itsLevelValue = 0;           // prodpar (e.g. 1000 m for CAPPI height)
  double itsGain = 1.0;
  double itsOffset = 0.0;
  double itsNodata = 0.0;
  double itsUndetect = 0.0;
};
}  // namespace Qdless
