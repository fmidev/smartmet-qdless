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

  std::size_t levelCount() const override;
  std::size_t currentLevelIndex() const override;
  void selectLevelIndex(std::size_t i) override;
  float levelValueAt(std::size_t i) const override;

  float interpolatedValue(double lat, double lon) const override;
  LatLonBox boundingBox() const override;

  // One-time initialisation of grid-files' parameter mapping. Safe to
  // call multiple times; only the first call has effect. Searches several
  // standard locations for grid-files.conf.
  static void ensureGridDef();

 private:
  // Build the (param, time, level) index from the messages in the file.
  void indexMessages();
  // Select the message matching the current (param, time, level).
  SmartMet::GRID::Message* currentMessage() const;

  std::unique_ptr<SmartMet::GRID::GridFile> itsFile;
  std::vector<int> itsParamIds;        // newbase IDs (resolved via name)
  std::vector<std::string> itsParamUnits;  // parallel to itsParamIds
  std::vector<NFmiMetTime> itsTimes;   // sorted, unique
  std::vector<float> itsLevels;        // sorted, unique
  // Map (newbaseParamId, timeIdx, levelIdx) → message index.
  std::map<std::tuple<int, std::size_t, std::size_t>, std::size_t> itsIndex;

  int itsCurrentParam = 0;
  std::size_t itsCurrentTime = 0;
  std::size_t itsCurrentLevel = 0;
};
}  // namespace Qdless
