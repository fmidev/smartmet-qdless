#pragma once

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
  // name/asciiname), ranked by population descending, capped at maxResults.
  std::vector<std::size_t> search(const std::string& query, std::size_t maxResults) const;

  const City& at(std::size_t i) const { return itsCities[i]; }

 private:
  std::vector<City> itsCities;
};
}  // namespace Qdless
