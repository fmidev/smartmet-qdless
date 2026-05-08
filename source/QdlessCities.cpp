#include "QdlessCities.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>

namespace Qdless
{
namespace
{
std::string lowercased(std::string_view s)
{
  std::string out(s.size(), '\0');
  std::transform(s.begin(), s.end(), out.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return out;
}
}  // namespace

bool CityIndex::load(const std::string& path)
{
  itsCities.clear();
  std::ifstream in(path);
  if (!in) return false;
  std::string line;
  while (std::getline(in, line))
  {
    // 6 tab-separated fields: name, asciiname, lat, lon, cc, population
    std::size_t prev = 0;
    std::vector<std::string_view> fields;
    fields.reserve(6);
    for (std::size_t i = 0; i < line.size(); ++i)
    {
      if (line[i] == '\t')
      {
        fields.emplace_back(line.data() + prev, i - prev);
        prev = i + 1;
      }
    }
    fields.emplace_back(line.data() + prev, line.size() - prev);
    if (fields.size() < 6) continue;

    City c;
    c.name = std::string(fields[0]);
    c.asciiname = std::string(fields[1]);
    c.lat = std::strtof(std::string(fields[2]).c_str(), nullptr);
    c.lon = std::strtof(std::string(fields[3]).c_str(), nullptr);
    c.country = std::string(fields[4]);
    c.population = std::atoi(std::string(fields[5]).c_str());
    itsCities.push_back(std::move(c));
  }
  return !itsCities.empty();
}

std::vector<std::size_t> CityIndex::search(const std::string& query,
                                           std::size_t maxResults) const
{
  std::vector<std::size_t> hits;
  if (query.empty() || itsCities.empty()) return hits;
  const std::string needle = lowercased(query);

  // Linear scan: for ~170k cities this is well below 100 ms on modern CPUs.
  // Collect matches with population for ranking.
  std::vector<std::pair<int, std::size_t>> ranked;  // -population, idx
  ranked.reserve(256);
  for (std::size_t i = 0; i < itsCities.size(); ++i)
  {
    const auto& c = itsCities[i];
    const std::string lname = lowercased(c.name);
    const std::string lascii = lowercased(c.asciiname);
    if (lname.find(needle) != std::string::npos ||
        lascii.find(needle) != std::string::npos)
      ranked.emplace_back(-c.population, i);
  }
  std::partial_sort(ranked.begin(),
                    ranked.begin() + std::min(maxResults, ranked.size()), ranked.end());
  for (std::size_t i = 0; i < std::min(maxResults, ranked.size()); ++i)
    hits.push_back(ranked[i].second);
  return hits;
}
}  // namespace Qdless
