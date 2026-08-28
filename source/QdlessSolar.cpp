#include "QdlessSolar.h"

#include <macgyver/Astronomy.h>
#include <macgyver/DateTime.h>
#include <newbase/NFmiMetTime.h>

#include <fmt/format.h>

#include <algorithm>
#include <cmath>

namespace Qdless
{
namespace Solar
{
namespace
{
// Wrap a longitude into [-180, 180) — macgyver's solar model documents that
// range for its `lon` argument, and the subsolar search below walks past the
// ends of it.
double wrapLon(double lon)
{
  double x = std::fmod(lon + 180.0, 360.0);
  if (x < 0.0)
    x += 360.0;
  return x - 180.0;
}

// Per-zone shading recipe: `weight` blends the underlying colour toward
// `grey`, and `bare` is the opaque fill used where there is no data at all.
//
// Grey rather than night-blue on purpose: the point of the overlay is to say
// "unlit here", and a colour cast would move the data's hue away from the
// palette legend. The ramp stays gentle through civil twilight and only goes
// really dark past the astronomical edge.
struct ZoneStyle
{
  float weight;
  Rgb grey;
  Rgb bare;
};

const ZoneStyle& styleFor(Zone z)
{
  static const ZoneStyle kStyles[5] = {
      {0.00F, {0, 0, 0}, {0, 0, 0, true}},  // Day — untouched
      {0.25F, {30, 30, 30}, {58, 58, 58}},  // Civil
      {0.42F, {24, 24, 24}, {44, 44, 44}},  // Nautical
      {0.56F, {19, 19, 19}, {32, 32, 32}},  // Astronomical
      {0.68F, {14, 14, 14}, {22, 22, 22}},  // Night
  };
  return kStyles[static_cast<std::size_t>(z)];
}

std::uint8_t blend(std::uint8_t base, std::uint8_t tint, float w)
{
  const float v = static_cast<float>(base) * (1.0F - w) + static_cast<float>(tint) * w;
  return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F));
}
}  // namespace

Sky::Sky() = default;
Sky::~Sky() = default;
Sky::Sky(Sky&&) noexcept = default;
Sky& Sky::operator=(Sky&&) noexcept = default;

Sky::Sky(const NFmiMetTime& utc)
{
  // Year 0 is newbase's "no time at all" — there is no instant to put the sun
  // at, so the Sky stays invalid and the overlay renders nothing. Genuine
  // pre-2000 valid times (reanalyses) are accepted as-is.
  if (utc.GetYear() < 1900)
    return;
  itsTime = std::make_unique<Fmi::DateTime>(
      Fmi::Date(utc.GetYear(), utc.GetMonth(), utc.GetDay()),
      Fmi::Hours(utc.GetHour()) + Fmi::Minutes(utc.GetMin()) + Fmi::Seconds(utc.GetSec()));
}

Sky::Sky(const Sky& other)
    : itsSubsolar(other.itsSubsolar), itsSubsolarSolved(other.itsSubsolarSolved)
{
  if (other.itsTime != nullptr)
    itsTime = std::make_unique<Fmi::DateTime>(*other.itsTime);
}

Sky& Sky::operator=(const Sky& other)
{
  if (this != &other)
  {
    itsTime = other.itsTime != nullptr ? std::make_unique<Fmi::DateTime>(*other.itsTime) : nullptr;
    itsSubsolar = other.itsSubsolar;
    itsSubsolarSolved = other.itsSubsolarSolved;
  }
  return *this;
}

double Sky::elevationDeg(double lat, double lon) const
{
  if (itsTime == nullptr)
    return 90.0;  // no time: report broad daylight so the overlay vanishes
  // macgyver truncates |lat| to 89.8 itself; the longitude range is ours to
  // respect. The returned elevation is refraction-corrected, which is the
  // apparent angle an observer would measure — the right quantity for a
  // "where is it light" display.
  return Fmi::Astronomy::solar_position(*itsTime, wrapLon(lon), std::clamp(lat, -90.0, 90.0))
      .elevation;
}

const Subsolar& Sky::subsolar() const
{
  if (itsSubsolarSolved)
    return itsSubsolar;
  itsSubsolarSolved = true;
  if (itsTime == nullptr)
    return itsSubsolar;

  const double dec = Fmi::Astronomy::solar_position(*itsTime, 0.0, 0.0).declination;

  // Solve for the subsolar longitude as an argmax over macgyver's own
  // elevation rather than re-deriving the equation of time: at a fixed
  // latitude the elevation depends on longitude only through
  // cos(λ − λs), which is strictly unimodal around λs on the whole circle.
  // (Refraction is monotone in the geometric angle, so correcting for it
  // does not move the maximum.)
  auto elevationAt = [&](double lon)
  { return Fmi::Astronomy::solar_position(*itsTime, wrapLon(lon), dec).elevation; };

  double best = -180.0;
  double bestE = -1e30;
  for (int k = 0; k < 72; ++k)  // 5° coarse scan brackets the peak to ±2.5°
  {
    const double lon = -180.0 + 5.0 * k;
    const double e = elevationAt(lon);
    if (e > bestE)
    {
      bestE = e;
      best = lon;
    }
  }
  // Four ×10 refinements: ±2.5° → 0.0005°. Each pass scans around the
  // previous winner, which only moves once the pass is done (scanning off a
  // moving centre would drift).
  double step = 5.0;
  for (int pass = 0; pass < 4; ++pass)
  {
    step /= 10.0;
    double passBest = best;
    for (int k = -10; k <= 10; ++k)
    {
      const double lon = best + step * k;
      const double e = elevationAt(lon);
      if (e > bestE)
      {
        bestE = e;
        passBest = lon;
      }
    }
    best = passBest;
  }

  itsSubsolar.lat = dec;
  itsSubsolar.lon = wrapLon(best);
  itsSubsolar.valid = true;
  return itsSubsolar;
}

Zone zoneFor(double elevationDeg)
{
  if (elevationDeg > kCivilEdgeDeg)
    return Zone::Day;
  if (elevationDeg > kNauticalEdgeDeg)
    return Zone::Civil;
  if (elevationDeg > kAstronomicalEdgeDeg)
    return Zone::Nautical;
  if (elevationDeg > kNightEdgeDeg)
    return Zone::Astronomical;
  return Zone::Night;
}

const char* zoneName(Zone z)
{
  switch (z)
  {
    case Zone::Day:
      return "day";
    case Zone::Civil:
      return "civil twilight";
    case Zone::Nautical:
      return "nautical twilight";
    case Zone::Astronomical:
      return "astronomical twilight";
    case Zone::Night:
      return "night";
  }
  return "?";
}

Rgb shade(Rgb base, Zone z)
{
  if (z == Zone::Day)
    return base;
  const ZoneStyle& s = styleFor(z);
  if (base.transparent)
    return s.bare;
  Rgb out = base;
  out.r = blend(base.r, s.grey.r, s.weight);
  out.g = blend(base.g, s.grey.g, s.weight);
  out.b = blend(base.b, s.grey.b, s.weight);
  return out;
}

std::string subsolarLabel(const Subsolar& sun)
{
  if (!sun.valid)
    return "n/a";
  return fmt::format("{:.1f}°{} {:.1f}°{}",
                     std::abs(sun.lat),
                     sun.lat >= 0 ? "N" : "S",
                     std::abs(sun.lon),
                     sun.lon >= 0 ? "E" : "W");
}
}  // namespace Solar
}  // namespace Qdless
