#pragma once

#include "QdlessDataSource.h"

#include <memory>
#include <string>
#include <vector>

namespace Qdless
{
// Aggregates multiple per-file DataSources into a single time-merged
// DataSource. Designed for cases where each file holds one timestep
// (ODIM HDF, GeoTIFF). The newest file (by filesystem mtime) defines
// the canonical projection and parameter list; files whose grid
// signature does not match the reference are dropped with a stderr
// warning. Time, level, and parameter IDs are taken from the
// reference source; selectTimeIndex() routes to the source that owns
// that time.
class MultiFileSource : public DataSource
{
 public:
  // `paths` is a list of absolute or relative file paths. They are
  // each opened (via DataSource::open) and merged. Throws if no file
  // is openable. Mismatched files are warned to stderr and skipped.
  explicit MultiFileSource(const std::vector<std::string>& paths);
  ~MultiFileSource() override;
  MultiFileSource(const MultiFileSource&) = delete;
  MultiFileSource& operator=(const MultiFileSource&) = delete;
  MultiFileSource(MultiFileSource&&) = delete;
  MultiFileSource& operator=(MultiFileSource&&) = delete;

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

  // Image-mode pass-through: when the reference is a raw image, every
  // sibling shares the same gridSignature (which encodes the image
  // dimensions) and is therefore also a raw image. Forwarding through
  // the reference is enough.
  bool isRawImage() const override;
  Rgb pixelAtUV(double u, double v) const override;

 private:
  // (sourceIdx, sourceLocalTimeIdx) for each global timestep, sorted by
  // valid time across all sources. Most files contribute one timestep
  // but the design accommodates a multi-time GRIB if mixed in.
  struct TimeSlot
  {
    std::size_t source;
    std::size_t localTime;
    NFmiMetTime time;
  };

  // The reference source (newest by mtime) — its projection, parameter
  // list, and grid drive the rest. Always sources[itsRefSource].
  std::vector<std::unique_ptr<DataSource>> itsSources;
  std::vector<std::string> itsPaths;          // parallel to itsSources
  std::size_t itsRefSource = 0;
  std::vector<TimeSlot> itsTimes;             // sorted by .time

  std::size_t itsCurrentTime = 0;
  // Track active param/level so selectTimeIndex can re-apply them on the
  // newly-active source (each source carries its own state).
  int itsCurrentParam = 0;
  std::size_t itsCurrentLevel = 0;

  DataSource& currentSource() { return *itsSources[itsTimes[itsCurrentTime].source]; }
  const DataSource& currentSource() const
  {
    return *itsSources[itsTimes[itsCurrentTime].source];
  }
  DataSource& refSource() { return *itsSources[itsRefSource]; }
  const DataSource& refSource() const { return *itsSources[itsRefSource]; }
};
}  // namespace Qdless
