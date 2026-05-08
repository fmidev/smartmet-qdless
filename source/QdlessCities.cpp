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

  // Score = log(pop+1) − 2·log(1 + d/100). With a 100 km reference scale
  // and α=2, regional matches dominate when the viewport is regional and
  // population dominates when the viewport is global (every match is
  // ~equally far). Negate so lower-is-better suits std::partial_sort.
  auto score = [&](const City& c) -> double {
    const double lp = std::log(static_cast<double>(c.population) + 1.0);
    if (!useDistance) return lp;
    const double dKm = haversineKm(centerLat, centerLon, c.lat, c.lon);
    return lp - 2.0 * std::log1p(dKm / 100.0);
  };

  // Two-tier ranking: prefix matches win over infix matches first, then
  // (-score) breaks ties. So typing "kou" prefers Kouvola (prefix) over
  // Haikou (infix) regardless of population or distance.
  struct Hit
  {
    int tier;        // 0 = prefix match, 1 = infix only
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
    const bool prefix = lname.starts_with(needle) || lascii.starts_with(needle);
    const bool infix = !prefix && (lname.find(needle) != std::string::npos ||
                                   lascii.find(needle) != std::string::npos);
    if (!prefix && !infix) continue;
    ranked.push_back({prefix ? 0 : 1, -score(c), i});
  }
  std::partial_sort(ranked.begin(),
                    ranked.begin() + std::min(maxResults, ranked.size()), ranked.end(),
                    rank);
  for (std::size_t i = 0; i < std::min(maxResults, ranked.size()); ++i)
    hits.push_back(ranked[i].idx);
  return hits;
}
}  // namespace Qdless
