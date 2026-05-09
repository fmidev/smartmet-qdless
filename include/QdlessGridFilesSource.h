#pragma once

#include "QdlessDataSource.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

// Forward-declare GRID::GridFile to keep grid-files headers out of this
// public header (they pull in OGR + a lot of transitives).
namespace SmartMet
{
namespace GRID
{
class GridFile;
class Message;
}  // namespace GRID
}  // namespace SmartMet

namespace Qdless
{
// DataSource backed by SmartMet::GRID::GridFile, used for GRIB1/2 and
// NetCDF input (and also, in principle, QueryData — though we prefer the
// native newbase path for .sqd because it's faster and projection-aware).
class GridFilesSource : public DataSource
{
 public:
  explicit GridFilesSource(const std::string& filename);
  ~GridFilesSource() override;
  GridFilesSource(const GridFilesSource&) = delete;
  GridFilesSource& operator=(const GridFilesSource&) = delete;
  GridFilesSource(GridFilesSource&&) = delete;
  GridFilesSource& operator=(GridFilesSource&&) = delete;

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

  // Map viewport (u,v) to lat/lon through the file's grid geometry, like
  // QueryDataSource does with NFmiArea. This avoids the antimeridian-wrap
  // bug in the base-class default that interpolates over a lat/lon bbox:
  // on a global grid `getGridLatLonArea` reports both edges as the same
  // wrapped longitude, collapsing the bbox to a thin sliver. Going through
  // grid coordinates also gives correct rendering for projected grids.
  void uvToLatLon(double u, double v, double& lat, double& lon) const override;
  void latLonToUV(double lat, double lon, double& u, double& v) const override;

  std::vector<std::pair<std::string, std::string>> extraMetadata() const override;

  // One-time initialisation of grid-files' parameter mapping. Safe to
  // call multiple times; only the first call has effect. Searches several
  // standard locations for grid-files.conf.
  static void ensureGridDef();

 private:
  // Build the (param, time, level) index from the messages in the file.
  void indexMessages();
  // Select the message matching the current (param, time, level).
  SmartMet::GRID::Message* currentMessage() const;
  // Resolve the grid dims and j-axis orientation. v=0 (top) maps to max
  // latitude, but grid_j=0 may be either north or south depending on the
  // file's scanning mode; sample one column to find out. Cached after first
  // call; returns false if the grid is missing or has < 2x2 dimensions.
  bool ensureGridGeometry() const;

  std::unique_ptr<SmartMet::GRID::GridFile> itsFile;
  std::vector<int> itsParamIds;            // newbase IDs (resolved via name)
  std::vector<std::string> itsParamUnits;  // parallel to itsParamIds
  // Native parameter name from the file (NetCDF variable name / GRIB
  // shortName) — used as the display name for the parameter so we don't
  // fall through to grid-files' fmi-id table, which uses a different ID
  // namespace from newbase (e.g. ID 13 = Humidity in newbase but
  // GROWDEV-D in grid-files; a cross-namespace lookup mis-renders RH).
  std::vector<std::string> itsParamNativeNames;
  std::vector<NFmiMetTime> itsTimes;  // sorted, unique (for display)
  std::vector<long> itsTimesT;        // parallel: unix seconds (for indexing)
  std::vector<float> itsLevels;       // sorted, unique
  // Map (newbaseParamId, timeIdx, levelIdx) → message index.
  std::map<std::tuple<int, std::size_t, std::size_t>, std::size_t> itsIndex;

  int itsCurrentParam = 0;
  std::size_t itsCurrentTime = 0;
  std::size_t itsCurrentLevel = 0;

  // Cached grid geometry, populated by ensureGridGeometry().
  mutable bool itsGeometryCached = false;
  mutable bool itsGeometryValid = false;
  mutable unsigned itsNx = 0;
  mutable unsigned itsNy = 0;
  // True if grid_j=0 is at the northern edge (j increases southward).
  // False if grid_j=0 is at the southern edge (j increases northward).
  mutable bool itsScanFromNorth = false;
};
}  // namespace Qdless
