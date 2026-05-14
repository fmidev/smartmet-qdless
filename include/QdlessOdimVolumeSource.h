#pragma once

#include "QdlessDataSource.h"

#include <memory>
#include <string>
#include <vector>

class NFmiArea;

namespace Qdless
{
// DataSource for ODIM-H5 polar volumes (/what/object == "PVOL"). Each
// /datasetN group is one elevation sweep; we expose all sweeps as levels
// whose level value is the elevation angle in degrees. Sweeps appear in
// elangle order (low → high), independent of the order they happen to be
// stored in the file.
//
// Rendering path: PPI per elevation. interpolatedValue(lat,lon) projects
// to an azimuthal-equidistant grid centred on the radar, converts the
// resulting ground range to slant range via r_s = r_g / cos(elangle), and
// looks up the matching (ray, bin) in the polar raster of the active
// sweep. Out-of-range and out-of-cone samples return NaN so the renderer
// paints them transparent.
//
// One quantity per file for now (the quantity of /dataset1/data1 — usually
// DBZH for operational scans). Multi-quantity files can be supported later
// by populating paramIds()/selectParamId() with all unique quantities and
// switching the per-sweep raster pointer in `selectParamId`.
class OdimVolumeSource : public DataSource
{
 public:
  explicit OdimVolumeSource(const std::string& filename);
  ~OdimVolumeSource() override;
  OdimVolumeSource(const OdimVolumeSource&) = delete;
  OdimVolumeSource& operator=(const OdimVolumeSource&) = delete;
  OdimVolumeSource(OdimVolumeSource&&) = delete;
  OdimVolumeSource& operator=(OdimVolumeSource&&) = delete;

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
  bool levelsAscendWithValue() const override { return true; }

  float interpolatedValue(double lat, double lon) const override;
  LatLonBox boundingBox() const override;
  void uvToLatLon(double u, double v, double& lat, double& lon) const override;
  void latLonToUV(double lat, double lon, double& u, double& v) const override;

  std::vector<std::pair<std::string, std::string>> extraMetadata() const override;
  std::string gridSignature() const override;

  // True if the file is an ODIM HDF5 with /what/object == "PVOL".
  static bool isVolume(const std::string& filename);

 private:
  struct Sweep
  {
    double elangle = 0;        // elevation angle, degrees
    std::size_t nrays = 0;     // azimuth bins (typically 360)
    std::size_t nbins = 0;     // range bins along ray
    double rscale = 1;         // bin length, meters
    double rstart = 0;         // start range, meters (usually 0)
    double gain = 1;
    double offset = 0;
    double nodata = 0;
    double undetect = 0;
    std::vector<float> raw;    // raw values, row-major (ray, bin), length nrays*nbins
  };

  std::string itsFilename;
  std::unique_ptr<NFmiArea> itsArea;  // AEQD centred on the radar
  std::vector<Sweep> itsSweeps;       // sorted by elangle ascending
  double itsRadarLat = 0;
  double itsRadarLon = 0;
  double itsRadarHeight = 0;          // metres above sea level
  double itsMaxRange = 0;             // metres; AEQD half-extent
  std::size_t itsCurrentLevel = 0;
  int itsParamId = 0;                 // newbase enum (0 = unknown)
  std::string itsParamUnits;
  std::string itsQuantity;            // e.g. "DBZH"
  std::string itsProduct;             // /datasetN/what/product, e.g. "SCAN"
  std::string itsSourceTag;           // /what/source, e.g. "WMO:26422"
  NFmiMetTime itsValidTime;
};
}  // namespace Qdless
