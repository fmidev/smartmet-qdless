#pragma once

#include "QdlessPalette.h"

#include <cstdint>
#include <memory>
#include <string>

class NFmiMetTime;

namespace Fmi
{
namespace date_time
{
class DateTime;
}
using DateTime = date_time::DateTime;
}  // namespace Fmi

namespace Qdless
{
// Sunlight / twilight geometry for the day-night overlay (key [u]).
//
// Every elevation angle comes from macgyver's NOAA solar model
// (Fmi::Astronomy::solar_position in macgyver/Astronomy.h) — the same
// astronomy the rest of the SmartMet stack uses for sunrise / sunset and
// solar-radiation work, so a qdless terminator lines up with what the
// server computes for the same instant.
namespace Solar
{
// Twilight classification of a solar elevation angle. Boundaries are the
// standard ones, measured for the centre of the sun's disc:
//   > 0°          daylight
//   0° .. -6°     civil twilight        (horizon still clearly visible)
//   -6° .. -12°   nautical twilight     (sea horizon still discernible)
//   -12° .. -18°  astronomical twilight (sky not yet fully dark)
//   < -18°        night
enum class Zone : std::uint8_t
{
  Day,
  Civil,
  Nautical,
  Astronomical,
  Night,
};

// Elevation-angle boundaries in degrees, in Zone order.
constexpr double kCivilEdgeDeg = 0.0;
constexpr double kNauticalEdgeDeg = -6.0;
constexpr double kAstronomicalEdgeDeg = -12.0;
constexpr double kNightEdgeDeg = -18.0;

// The point where the sun stands at zenith. `lat` is the solar declination,
// `lon` the subsolar meridian. Only used for the status-bar label.
struct Subsolar
{
  double lat = 0;
  double lon = 0;
  bool valid = false;
};

// The sun at one instant. Holds the converted timestamp so a frame's worth
// of elevation queries doesn't rebuild it per sample; every query delegates
// to macgyver.
class Sky
{
 public:
  Sky();  // invalid
  explicit Sky(const NFmiMetTime& utc);
  Sky(const Sky& other);
  Sky& operator=(const Sky& other);
  Sky(Sky&&) noexcept;
  Sky& operator=(Sky&&) noexcept;
  ~Sky();

  // False for a placeholder valid time (year < 1900), i.e. a file with no
  // usable time axis — there is no instant to put the sun at.
  bool valid() const { return itsTime != nullptr; }

  // Solar elevation in degrees at (lat, lon), from macgyver. Returns 90
  // (i.e. "broad daylight", overlay invisible) when the sky is invalid so
  // callers can stay branch-free.
  double elevationDeg(double lat, double lon) const;

  // The subsolar point, solved lazily as the longitude that maximises
  // macgyver's elevation at the declination's latitude.
  const Subsolar& subsolar() const;

 private:
  // Fmi::DateTime by pointer so this header stays free of macgyver's
  // date_time machinery (it is included by QdlessApp.h, i.e. everywhere).
  std::unique_ptr<Fmi::DateTime> itsTime;
  mutable Subsolar itsSubsolar;
  mutable bool itsSubsolarSolved = false;
};

// Zone for an elevation angle, and its human-readable name.
Zone zoneFor(double elevationDeg);
const char* zoneName(Zone z);

// Darken `base` for the given zone: blend it toward the zone's grey. Day
// returns `base` untouched, so the sunlit side keeps full data legibility.
//
// The darkening is neutral (toward dark grey, never toward blue): a shadow
// must not move the data's hue, or a night-side cell stops matching the
// palette legend. A transparent `base` (no data / below palette range) comes
// back as an opaque grey instead, so the twilight bands read as areas even
// where the field has no values. Every weight and colour lives in one table
// in the implementation.
Rgb shade(Rgb base, Zone z);

// "23.2°N 180.0°E" — the subsolar point, for status messages.
std::string subsolarLabel(const Subsolar& sun);
}  // namespace Solar
}  // namespace Qdless
