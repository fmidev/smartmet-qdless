#include "QdlessCities.h"

#include <algorithm>
#include <cmath>
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

// Great-circle distance in km between two lat/lon points.
double haversineKm(double lat1, double lon1, double lat2, double lon2)
{
  constexpr double R = 6371.0;
  constexpr double deg = M_PI / 180.0;
  const double dLat = (lat2 - lat1) * deg;
  const double dLon = (lon2 - lon1) * deg;
  const double a = std::sin(dLat / 2) * std::sin(dLat / 2) +
                   std::cos(lat1 * deg) * std::cos(lat2 * deg) *
                       std::sin(dLon / 2) * std::sin(dLon / 2);
  return R * 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
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

std::vector<std::size_t> CityIndex::search(const std::string& query, std::size_t maxResults,
                                           double centerLat, double centerLon) const
{
  std::vector<std::size_t> hits;
  if (query.empty() || itsCities.empty()) return hits;
  const std::string needle = lowercased(query);
  const bool useDistance = std::isfinite(centerLat) && std::isfinite(centerLon);

  // Score = log(pop+1) − 3·log(1 + d/100). With a 100 km reference scale
  // and α=3, regional matches dominate strongly when the viewport is
  // regional (a 32k city at 100 km beats a 4M city at 2000 km), and
  // population still wins in a global view (every match is ~equally far).
  // Negate so lower-is-better suits std::partial_sort.
  auto score = [&](const City& c) -> double {
    const double lp = std::log(static_cast<double>(c.population) + 1.0);
    if (!useDistance) return lp;
    const double dKm = haversineKm(centerLat, centerLon, c.lat, c.lon);
    return lp - 3.0 * std::log1p(dKm / 100.0);
  };

  // True if `needle` matches at position 0 or just after a word separator
  // (space, hyphen, slash, period). So "york" matches "New York" (prefix
  // of the second word) and "saint" matches "Saint-Étienne", but "imat"
  // does NOT match "Orimattila" because it's mid-word.
  auto matchesWordPrefix = [&](const std::string& s) -> int {
    // Returns 0 if needle is a prefix of the whole string,
    //         1 if it's a prefix of a non-leading word,
    //        -1 if no match.
    if (s.compare(0, needle.size(), needle) == 0) return 0;
    for (std::size_t i = 1; i + needle.size() <= s.size(); ++i)
    {
      const char prev = s[i - 1];
      if ((prev == ' ' || prev == '-' || prev == '/' || prev == '.') &&
          s.compare(i, needle.size(), needle) == 0)
        return 1;
    }
    return -1;
  };

  // Two-tier ranking: prefix-of-name wins over prefix-of-word; within each
  // tier (-score) breaks ties so locality + population still matter. Plain
  // infix matches ("imat" inside "Orimattila") don't match at all.
  struct Hit
  {
    int tier;        // 0 = prefix of name, 1 = prefix of word
    double negScore;
    std::size_t idx;
  };
  auto rank = [](const Hit& a, const Hit& b) {
    if (a.tier != b.tier) return a.tier < b.tier;
    return a.negScore < b.negScore;
  };

  // Linear scan: for ~170k cities this is well below 100 ms on modern CPUs.
  std::vector<Hit> ranked;
  ranked.reserve(256);
  for (std::size_t i = 0; i < itsCities.size(); ++i)
  {
    const auto& c = itsCities[i];
    const std::string lname = lowercased(c.name);
    const std::string lascii = lowercased(c.asciiname);
    const int t1 = matchesWordPrefix(lname);
    const int t2 = matchesWordPrefix(lascii);
    int tier = -1;
    if (t1 == 0 || t2 == 0) tier = 0;
    else if (t1 == 1 || t2 == 1) tier = 1;
    if (tier < 0) continue;
    ranked.push_back({tier, -score(c), i});
  }
  std::partial_sort(ranked.begin(),
                    ranked.begin() + std::min(maxResults, ranked.size()), ranked.end(),
                    rank);
  for (std::size_t i = 0; i < std::min(maxResults, ranked.size()); ++i)
    hits.push_back(ranked[i].idx);
  return hits;
}
}  // namespace Qdless
