#pragma once

// Pattern-match the loaded weather data against a small library of
// detectors and suggest the viewing mode that brings the phenomenon
// out most clearly. The detectors are cheap (a few ms each on a
// coarse 64×32 lat/lon sample) so the whole sweep runs on every
// parameter/level/file change and the highest-scoring hint is
// surfaced in the status bar.
//
// Detectors implemented:
//   1) Tropical convection / MJO — precipitation/OLR/cloud spike near
//      the equator → "longitude Hovmöller along the equator"
//   2) Cyclones / hurricanes      — MSL pressure minima with steep
//      gradient → "pan to the low; animate"
//   3) Fronts                     — temperature gradient peak →
//      "cross-section perpendicular to the front"
//   4) Jet streams                — upper-level wind speed > 40 m/s →
//      "cycle to 200–300 hPa, browse layers"
//   5) Atmospheric blocks         — geopotential at ~500 hPa with low
//      temporal variance at a high value → "time loop"
//   6) Static field               — temporal variance near zero
//      everywhere → "field is static — Hovmöller will be flat"
//
// The detectors are heuristic — meteorologists may want to retune the
// thresholds. Each hint is muteable (status bar only, no popups), so
// false positives are easy to dismiss.

#include <string>
#include <vector>

namespace Qdless
{
class DataSource;

struct PhenomenonHint
{
  std::string message;       // single-line status text, empty = nothing detected
  std::string suggestion;    // suggested next action (key combo / view mode)
  int score = 0;             // 0 = no detection; higher = more confident
  // Optional anchor that the caller can jump to.
  double anchorLat = 0;
  double anchorLon = 0;
  bool hasAnchor = false;
};

// Run all applicable detectors against the currently-selected
// (param, level, time) of `src` and return the highest-scoring hint.
// Returns an empty hint (score == 0) when nothing fires above
// threshold.
//
// The function disturbs the source's time index while it samples but
// always restores the original value before returning.
PhenomenonHint detectPhenomena(DataSource& src);

}  // namespace Qdless
