#pragma once

#include <newbase/NFmiMetTime.h>

#include <memory>
#include <string>
#include <vector>

namespace Qdless
{
// Lat/lon bounding box for the data extent.
struct LatLonBox
{
  double minLat = -90;
  double maxLat = 90;
  double minLon = -180;
  double maxLon = 180;
};

// Format-agnostic interface to a gridded weather file (.sqd / .grib /
// .grib2 / .nc). Concrete backends wrap NFmiFastQueryInfo (newbase) or
// SmartMet::GRID::GridFile (grid-files).
class DataSource
{
 public:
  virtual ~DataSource() = default;

  // Auto-detects the format and returns a DataSource. Throws on error.
  static std::unique_ptr<DataSource> open(const std::string& filename);

  // Parameter access — IDs use newbase / FMI numeric enums.
  virtual std::vector<int> paramIds() const = 0;
  virtual std::string paramShortName(int paramId) const = 0;
  virtual std::string paramLongName(int paramId) const = 0;
  // Unit string as given by the underlying file (e.g. "K", "m/s", "%").
  // Empty if unknown.
  virtual std::string paramUnits(int paramId) const = 0;
  virtual int currentParamId() const = 0;
  virtual bool selectParamId(int paramId) = 0;

  // Time axis.
  virtual std::size_t timeCount() const = 0;
  virtual std::size_t currentTimeIndex() const = 0;
  virtual void selectTimeIndex(std::size_t i) = 0;
  virtual NFmiMetTime currentValidTime() const = 0;

  // Level axis.
  virtual std::size_t levelCount() const = 0;
  virtual std::size_t currentLevelIndex() const = 0;
  virtual void selectLevelIndex(std::size_t i) = 0;
  virtual float levelValueAt(std::size_t i) const = 0;

  // Sample the currently-selected (param, time, level) slice at a given
  // lat/lon. Returns kFloatMissing or non-finite for missing / out-of-grid.
  virtual float interpolatedValue(double lat, double lon) const = 0;

  // Lat/lon bounding box of the data extent (rectangle covering all grid
  // points; exact for lat/lon grids, approximate for projected grids).
  virtual LatLonBox boundingBox() const = 0;
};
}  // namespace Qdless
