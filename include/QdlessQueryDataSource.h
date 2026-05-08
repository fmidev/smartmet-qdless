#pragma once

#include "QdlessDataSource.h"

#include <memory>
#include <string>
#include <vector>

class NFmiQueryData;
class NFmiFastQueryInfo;

namespace Qdless
{
// DataSource backed by newbase NFmiFastQueryInfo. Used for SmartMet's
// native QueryData (.sqd) files. The fastest path; preserves any
// QueryData-specific behaviour.
class QueryDataSource : public DataSource
{
 public:
  explicit QueryDataSource(const std::string& filename);
  ~QueryDataSource() override;
  QueryDataSource(const QueryDataSource&) = delete;
  QueryDataSource& operator=(const QueryDataSource&) = delete;
  QueryDataSource(QueryDataSource&&) = delete;
  QueryDataSource& operator=(QueryDataSource&&) = delete;

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

 private:
  std::unique_ptr<NFmiQueryData> itsData;
  std::unique_ptr<NFmiFastQueryInfo> itsInfo;
  std::vector<int> itsParamIds;
};
}  // namespace Qdless
