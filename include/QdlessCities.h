#pragma once

#include <limits>
#include <string>
#include <vector>

namespace Qdless
{
struct City
{
  std::string name;       // localised name (UTF-8)
  std::string asciiname;  // ASCII fallback for matching
  std::string country;    // 2-letter ISO code
  float lat = 0;
  float lon = 0;
  int population = 0;
};

class CityIndex
{
 public:
  // Load a TSV with columns: name, asciiname, lat, lon, country, population
  // Returns true on success. Already-loaded indexes are replaced.
  bool load(const std::string& path);
  bool empty() const { return itsCities.empty(); }
  std::size_t size() const { return itsCities.size(); }

  // Returns city indices matching `query` (case-insensitive substring on
  // name/asciiname), capped at maxResults. Ranking:
  //   * If `centerLat` / `centerLon` are finite, score by
  //     log(pop+1) − 2·log(1 + d_km/100) — population minus a logarithmic
  //     penalty for great-circle distance from (centerLat, centerLon).
  //     This biases the result toward places near the current viewport,
  //     so typing "kou" while looking at Scandinavia surfaces Kouvola
  //     ahead of every Chinese metropolis ending in "kou".
  //   * If the centre is left as NaN (default), rank by population alone.
  std::vector<std::size_t> search(
      const std::string& query, std::size_t maxResults,
      double centerLat = std::numeric_limits<double>::quiet_NaN(),
      double centerLon = std::numeric_limits<double>::quiet_NaN()) const;

  const City& at(std::size_t i) const { return itsCities[i]; }

  // Return the index of the closest city to (lat, lon) in great-circle
  // distance, capped at maxKm (so a North Atlantic phenomenon doesn't
  // get tagged with a distant European city). Returns SIZE_MAX when no
  // city is within range. Linear scan over the loaded set (≤200k
  // entries — sub-millisecond for typical 100k-row cities1000.tsv).
  std::size_t nearestCity(double lat, double lon, double maxKm = 1500.0) const;

 private:
  std::vector<City> itsCities;
};
}  // namespace Qdless
