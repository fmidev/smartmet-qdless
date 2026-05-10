#include "QdlessMultiFileSource.h"

#include <newbase/NFmiMetTime.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace Qdless
{
namespace
{
// File mtime as time_t (seconds since epoch). Used to pick the reference
// file from a heterogeneous batch — the assumption is that the producer
// writes the most-recent grid definition into the most-recent file, so
// any older file with a divergent grid is the one that should be dropped.
std::time_t fileMtime(const std::string& path)
{
  std::error_code ec;
  auto ftime = std::filesystem::last_write_time(path, ec);
  if (ec) return 0;
  // file_time_type → system_clock::time_point. C++20 has file_clock::to_sys
  // but it's not always available; do the rebase the portable way.
  auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
      ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
  return std::chrono::system_clock::to_time_t(sctp);
}

// Convert NFmiMetTime to an int64 sortable key (UTC seconds-since-epoch
// approximation; only used for ordering, not for display).
long long timeKey(const NFmiMetTime& t)
{
  // NFmiMetTime stores year/month/day/hour/minute/second; compose into
  // a packed integer that orders correctly. Avoids depending on the
  // newbase epoch helpers which differ across versions.
  return (static_cast<long long>(t.GetYear()) * 100'00'00'00'00LL +
          static_cast<long long>(t.GetMonth()) * 100'00'00'00LL +
          static_cast<long long>(t.GetDay()) * 100'00'00LL +
          static_cast<long long>(t.GetHour()) * 100'00LL +
          static_cast<long long>(t.GetMin()) * 100LL +
          static_cast<long long>(t.GetSec()));
}
}  // namespace

MultiFileSource::MultiFileSource(const std::vector<std::string>& paths)
{
  if (paths.empty()) throw std::runtime_error("MultiFileSource: no input files");

  // Open every file. Skip files that fail to open (with a warning) so
  // one bad file doesn't poison a 200-file batch. Track each path so
  // diagnostics name the file, not just an index.
  std::vector<std::unique_ptr<DataSource>> sources;
  std::vector<std::string> okPaths;
  std::vector<std::time_t> okMtimes;
  for (const auto& p : paths)
  {
    try
    {
      sources.push_back(DataSource::open(p));
      okPaths.push_back(p);
      okMtimes.push_back(fileMtime(p));
    }
    catch (const std::exception& e)
    {
      std::cerr << "qdless: skipping " << p << ": " << e.what() << "\n";
    }
  }
  if (sources.empty())
    throw std::runtime_error("MultiFileSource: no openable files in batch");

  // Reference = most-recently-modified file. Its projection wins; all
  // other files must match it or be dropped.
  std::size_t refIdx = 0;
  for (std::size_t i = 1; i < okMtimes.size(); ++i)
    if (okMtimes[i] > okMtimes[refIdx]) refIdx = i;
  const std::string refSig = sources[refIdx]->gridSignature();
  // Capture the reference path before we start moving entries out of
  // okPaths — otherwise rejection messages for files later in the loop
  // print an empty path.
  const std::string refPath = okPaths[refIdx];

  // Filter: keep only sources whose grid signature matches the reference.
  std::vector<std::unique_ptr<DataSource>> kept;
  std::vector<std::string> keptPaths;
  std::size_t newRefIdx = 0;
  for (std::size_t i = 0; i < sources.size(); ++i)
  {
    if (sources[i]->gridSignature() == refSig)
    {
      if (i == refIdx) newRefIdx = kept.size();
      keptPaths.push_back(std::move(okPaths[i]));
      kept.push_back(std::move(sources[i]));
    }
    else
    {
      std::cerr << "qdless: skipping " << okPaths[i]
                << ": grid does not match reference (" << refPath << ")\n";
    }
  }
  itsSources = std::move(kept);
  itsPaths = std::move(keptPaths);
  itsRefSource = newRefIdx;

  // Build the global time index. Each source contributes its full time
  // axis; in practice ODIM/GeoTIFF have one timestep per file, but a
  // mixed batch with a multi-time GRIB will fan out correctly.
  for (std::size_t s = 0; s < itsSources.size(); ++s)
  {
    const std::size_t n = itsSources[s]->timeCount();
    for (std::size_t t = 0; t < n; ++t)
    {
      // Cheap: select the time on the source to read its valid time.
      // This mutates the source's state, but we re-set itsCurrentTime
      // right afterwards so it's harmless.
      itsSources[s]->selectTimeIndex(t);
      itsTimes.push_back({s, t, itsSources[s]->currentValidTime()});
    }
  }
  std::sort(itsTimes.begin(), itsTimes.end(),
            [](const TimeSlot& a, const TimeSlot& b) { return timeKey(a.time) < timeKey(b.time); });

  // Initialise current state from the reference source.
  const auto pids = refSource().paramIds();
  itsCurrentParam = pids.empty() ? 0 : pids.front();
  itsCurrentLevel = 0;
  itsCurrentTime = 0;
  // Keep all sources synchronised on (param, level) so subsequent
  // accessor calls return consistent slices regardless of which one
  // happens to be current.
  for (auto& s : itsSources)
  {
    s->selectParamId(itsCurrentParam);
    s->selectLevelIndex(itsCurrentLevel);
  }
  // Point the reference source at its own slot for the very first time
  // (so currentSource() / currentValidTime() are coherent).
  itsSources[itsTimes[0].source]->selectTimeIndex(itsTimes[0].localTime);
}

MultiFileSource::~MultiFileSource() = default;

std::vector<int> MultiFileSource::paramIds() const { return refSource().paramIds(); }
std::string MultiFileSource::paramShortName(int paramId) const
{
  return refSource().paramShortName(paramId);
}
std::string MultiFileSource::paramLongName(int paramId) const
{
  return refSource().paramLongName(paramId);
}
std::string MultiFileSource::paramUnits(int paramId) const
{
  return refSource().paramUnits(paramId);
}
int MultiFileSource::currentParamId() const { return itsCurrentParam; }

bool MultiFileSource::selectParamId(int paramId)
{
  bool any = false;
  for (auto& s : itsSources)
    any = s->selectParamId(paramId) || any;
  if (any) itsCurrentParam = paramId;
  return any;
}

std::size_t MultiFileSource::timeCount() const { return itsTimes.size(); }
std::size_t MultiFileSource::currentTimeIndex() const { return itsCurrentTime; }
void MultiFileSource::selectTimeIndex(std::size_t i)
{
  if (i >= itsTimes.size()) return;
  itsCurrentTime = i;
  auto& src = currentSource();
  src.selectTimeIndex(itsTimes[i].localTime);
  // Re-apply param/level on the now-active source — most backends keep
  // their own state, and a previous selectParamId already broadcast to
  // every source, but level changes plus repeat calls are cheap.
  src.selectParamId(itsCurrentParam);
  src.selectLevelIndex(itsCurrentLevel);
}
NFmiMetTime MultiFileSource::currentValidTime() const
{
  return itsTimes.empty() ? NFmiMetTime() : itsTimes[itsCurrentTime].time;
}
NFmiMetTime MultiFileSource::originTime() const { return refSource().originTime(); }

std::size_t MultiFileSource::levelCount() const { return refSource().levelCount(); }
std::size_t MultiFileSource::currentLevelIndex() const { return itsCurrentLevel; }
void MultiFileSource::selectLevelIndex(std::size_t i)
{
  itsCurrentLevel = i;
  for (auto& s : itsSources) s->selectLevelIndex(i);
}
float MultiFileSource::levelValueAt(std::size_t i) const
{
  return refSource().levelValueAt(i);
}

float MultiFileSource::interpolatedValue(double lat, double lon) const
{
  return currentSource().interpolatedValue(lat, lon);
}
LatLonBox MultiFileSource::boundingBox() const { return refSource().boundingBox(); }
void MultiFileSource::uvToLatLon(double u, double v, double& lat, double& lon) const
{
  refSource().uvToLatLon(u, v, lat, lon);
}
void MultiFileSource::latLonToUV(double lat, double lon, double& u, double& v) const
{
  refSource().latLonToUV(lat, lon, u, v);
}

std::vector<std::pair<std::string, std::string>> MultiFileSource::extraMetadata() const
{
  auto rows = refSource().extraMetadata();
  rows.emplace_back("Files", std::to_string(itsSources.size()));
  rows.emplace_back("Times", std::to_string(itsTimes.size()));
  rows.emplace_back("Reference", itsPaths[itsRefSource]);
  return rows;
}

std::string MultiFileSource::gridSignature() const
{
  return refSource().gridSignature();
}
}  // namespace Qdless
