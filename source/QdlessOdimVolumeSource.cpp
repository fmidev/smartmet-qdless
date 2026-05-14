#include "QdlessOdimVolumeSource.h"

#include "Hdf5File.h"

#include <fmt/format.h>
#include <gis/SpatialReference.h>
#include <macgyver/StringConversion.h>
#include <newbase/NFmiArea.h>
#include <newbase/NFmiEnumConverter.h>
#include <newbase/NFmiParameterName.h>
#include <newbase/NFmiPoint.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace Qdless
{
namespace
{
// Same mapping as OdimSource (kept local to avoid coupling). For PVOL the
// product is typically "SCAN" so quantity-only fallbacks decide.
int operaToNewbase(const std::string& product, const std::string& quantity)
{
  if (product == "PPI" || product == "CAPPI" || product == "PCAPPI" || product == "SCAN")
  {
    if (quantity == "TH" || quantity == "DBZ") return kFmiReflectivity;
    if (quantity == "DBZH") return kFmiCorrectedReflectivity;
    if (quantity == "VRAD" || quantity == "VRADH") return kFmiRadialVelocity;
    if (quantity == "WRAD" || quantity == "W") return kFmiSpectralWidth;
    if (quantity == "RATE") return kFmiPrecipitationRate;
  }
  if (quantity == "TH" || quantity == "DBZ") return kFmiReflectivity;
  if (quantity == "DBZH") return kFmiCorrectedReflectivity;
  if (quantity == "VRAD" || quantity == "VRADH") return kFmiRadialVelocity;
  if (quantity == "WRAD" || quantity == "W") return kFmiSpectralWidth;
  if (quantity == "RATE") return kFmiPrecipitationRate;
  if (quantity == "ACRR") return kFmiPrecipitationAmount;
  if (quantity == "HGHT") return kFmiEchoTop;
  return 0;
}

std::string operaUnits(const std::string& quantity)
{
  if (quantity == "TH" || quantity == "DBZ" || quantity == "DBZH") return "dBZ";
  if (quantity == "VRAD" || quantity == "VRADH") return "m/s";
  if (quantity == "WRAD" || quantity == "W") return "m/s";
  if (quantity == "RATE") return "mm/h";
  if (quantity == "ACRR") return "mm";
  if (quantity == "HGHT") return "km";
  if (quantity == "ZDR") return "dB";
  return {};
}

NFmiMetTime parseOdimTime(const std::string& date, const std::string& time)
{
  if (date.size() < 8 || time.size() < 6) return NFmiMetTime();
  short yy = std::stoi(date.substr(0, 4));
  short mm = std::stoi(date.substr(4, 2));
  short dd = std::stoi(date.substr(6, 2));
  short h = std::stoi(time.substr(0, 2));
  short mi = std::stoi(time.substr(2, 2));
  short se = std::stoi(time.substr(4, 2));
  return NFmiMetTime(yy, mm, dd, h, mi, se);
}

// Enumerate /dataset1, /dataset2, … in the file. ODIM does not guarantee
// they are contiguously numbered, so we discover them from the dataset
// list rather than counting until a gap.
std::vector<std::string> discoverSweepGroups(const Fmi::HDF5::Hdf5File& file)
{
  // get_datasets() returns raster paths like "/dataset3/data1/data". Strip
  // to the top-level dataset group and de-duplicate.
  std::vector<std::string> groups;
  for (const auto& path : file.get_datasets())
  {
    if (path.size() < 9 || path.compare(0, 8, "/dataset") != 0) continue;
    const auto slash = path.find('/', 1);  // first '/' past the leading one
    std::string g = (slash == std::string::npos) ? path : path.substr(0, slash);
    groups.push_back(std::move(g));
  }
  std::sort(groups.begin(), groups.end());
  groups.erase(std::unique(groups.begin(), groups.end()), groups.end());
  return groups;
}
}  // namespace

bool OdimVolumeSource::isVolume(const std::string& filename)
{
  try
  {
    Fmi::HDF5::Hdf5File f(filename);
    if (!f.is_attribute<std::string>("/what", "object")) return false;
    return f.get_attribute<std::string>("/what", "object") == "PVOL";
  }
  catch (...)
  {
    return false;
  }
}

OdimVolumeSource::OdimVolumeSource(const std::string& filename) : itsFilename(filename)
{
  Fmi::HDF5::Hdf5File file(filename);

  const auto object = file.get_attribute<std::string>("/what", "object");
  if (object != "PVOL")
    throw std::runtime_error("OdimVolumeSource expects /what/object='PVOL', got '" + object + "'");

  itsRadarLat = file.get_attribute<double>("/where", "lat");
  itsRadarLon = file.get_attribute<double>("/where", "lon");
  if (auto h = file.get_optional_attribute<double>("/where", "height"))
    itsRadarHeight = *h;
  if (auto s = file.get_optional_attribute<std::string>("/what", "source"))
    itsSourceTag = *s;

  // Top-level valid time. /datasetN/what carries per-sweep timing but the
  // file-level /what time is what the rest of qdless wants for animation.
  const std::string date = file.get_attribute<std::string>("/what", "date");
  const std::string time = file.get_attribute<std::string>("/what", "time");
  itsValidTime = parseOdimTime(date, time);

  // Pick the active quantity from the lowest-elevation sweep's first data
  // sub-group. Operational SCAN files store the same quantity layout across
  // sweeps (DBZH in data1, VRAD in data2, …); selecting from /dataset1
  // matches what users see first.
  const auto sweepGroups = discoverSweepGroups(file);
  if (sweepGroups.empty())
    throw std::runtime_error("ODIM PVOL has no /datasetN groups: " + filename);

  itsQuantity = file.get_attribute<std::string>(sweepGroups.front() + "/data1/what", "quantity");
  itsProduct = file.get_attribute<std::string>(sweepGroups.front() + "/what", "product");
  itsParamId = operaToNewbase(itsProduct, itsQuantity);
  itsParamUnits = operaUnits(itsQuantity);

  // Build each sweep. We accept sweeps whose /data1/what/quantity matches
  // itsQuantity; sweeps that lack it are skipped (rare — typically every
  // sweep has the same quantity in /data1).
  itsSweeps.reserve(sweepGroups.size());
  for (const auto& g : sweepGroups)
  {
    const std::string dataGroup = g + "/data1";
    const std::string whatGroup = dataGroup + "/what";
    const std::string whereGroup = g + "/where";

    auto q = file.get_optional_attribute<std::string>(whatGroup, "quantity");
    if (!q || *q != itsQuantity) continue;

    Sweep s;
    s.elangle = file.get_attribute<double>(whereGroup, "elangle");
    s.nrays = static_cast<std::size_t>(file.get_attribute<long>(whereGroup, "nrays"));
    s.nbins = static_cast<std::size_t>(file.get_attribute<long>(whereGroup, "nbins"));
    s.rscale = file.get_attribute<double>(whereGroup, "rscale");
    if (auto rs = file.get_optional_attribute<double>(whereGroup, "rstart"))
      s.rstart = *rs * 1000.0;  // ODIM rstart is in km; rscale in metres (spec quirk)
    s.gain = file.get_attribute<double>(whatGroup, "gain");
    s.offset = file.get_attribute<double>(whatGroup, "offset");
    s.nodata = file.get_attribute<double>(whatGroup, "nodata");
    s.undetect = file.get_attribute<double>(whatGroup, "undetect");
    s.raw = file.read_dataset<float>(dataGroup);
    if (s.raw.size() != s.nrays * s.nbins)
      throw std::runtime_error("ODIM PVOL sweep " + g + " size " +
                               std::to_string(s.raw.size()) + " != " +
                               std::to_string(s.nrays) + "x" + std::to_string(s.nbins));

    const double sweepRange = s.rstart + static_cast<double>(s.nbins) * s.rscale;
    itsMaxRange = std::max(itsMaxRange, sweepRange);
    itsSweeps.push_back(std::move(s));
  }

  if (itsSweeps.empty())
    throw std::runtime_error("ODIM PVOL has no sweeps with quantity '" + itsQuantity + "'");

  // Sweeps come from file order. Sort by elevation so the level UI counts
  // from horizon upward, matching standard radar product conventions.
  std::sort(itsSweeps.begin(), itsSweeps.end(),
            [](const Sweep& a, const Sweep& b) { return a.elangle < b.elangle; });

  // Azimuthal-equidistant projection centred on the radar. The viewport
  // span equals 2·maxRange so the entire sweep just fits. AEQD preserves
  // distance and azimuth from the centre, which is exactly the radar's
  // polar coordinate system → interpolatedValue can recover (range,
  // azimuth) by atan2/hypot of the world XY.
  const std::string proj4 = fmt::format(
      "+proj=aeqd +lat_0={:.6f} +lon_0={:.6f} +R=6371000 +units=m +no_defs",
      itsRadarLat, itsRadarLon);
  Fmi::SpatialReference sr(proj4);
  Fmi::SpatialReference wgs84(4326);
  const NFmiPoint centerLatLon(itsRadarLon, itsRadarLat);
  const double size = 2.0 * itsMaxRange;
  itsArea.reset(NFmiArea::CreateFromCenter(sr, wgs84, centerLatLon, size, size));
  if (!itsArea) throw std::runtime_error("failed to construct AEQD projection for " + filename);
}

OdimVolumeSource::~OdimVolumeSource() = default;

std::vector<int> OdimVolumeSource::paramIds() const { return {itsParamId}; }

std::string OdimVolumeSource::paramShortName(int /*paramId*/) const
{
  if (itsParamId != 0)
  {
    NFmiEnumConverter conv;
    auto name = conv.ToString(itsParamId);
    if (!name.empty()) return name;
  }
  return itsQuantity.empty() ? std::string{"Unknown"} : itsQuantity;
}

std::string OdimVolumeSource::paramLongName(int paramId) const { return paramShortName(paramId); }

std::string OdimVolumeSource::paramUnits(int /*paramId*/) const { return itsParamUnits; }

int OdimVolumeSource::currentParamId() const { return itsParamId; }

bool OdimVolumeSource::selectParamId(int paramId) { return paramId == itsParamId; }

std::size_t OdimVolumeSource::timeCount() const { return 1; }
std::size_t OdimVolumeSource::currentTimeIndex() const { return 0; }
void OdimVolumeSource::selectTimeIndex(std::size_t /*i*/) {}
NFmiMetTime OdimVolumeSource::currentValidTime() const { return itsValidTime; }
NFmiMetTime OdimVolumeSource::originTime() const { return itsValidTime; }

std::size_t OdimVolumeSource::levelCount() const { return itsSweeps.size(); }
std::size_t OdimVolumeSource::currentLevelIndex() const { return itsCurrentLevel; }

void OdimVolumeSource::selectLevelIndex(std::size_t i)
{
  if (i < itsSweeps.size()) itsCurrentLevel = i;
}

float OdimVolumeSource::levelValueAt(std::size_t i) const
{
  if (i >= itsSweeps.size()) return 0.f;
  return static_cast<float>(itsSweeps[i].elangle);
}

float OdimVolumeSource::interpolatedValue(double lat, double lon) const
{
  if (!itsArea || itsCurrentLevel >= itsSweeps.size())
    return std::numeric_limits<float>::quiet_NaN();

  const Sweep& s = itsSweeps[itsCurrentLevel];
  if (s.raw.empty()) return std::numeric_limits<float>::quiet_NaN();

  // AEQD centred on the radar → world XY are metres east/north of the
  // radar. World X = east, world Y = north for the +proj=aeqd ordering
  // newbase uses.
  const NFmiPoint world = itsArea->LatLonToWorldXY(NFmiPoint(lon, lat));
  const double x = world.X();
  const double y = world.Y();
  const double rGround = std::hypot(x, y);

  // Convert ground range to slant range for this elevation. Flat-Earth
  // approximation: r_s ≈ r_g / cos(α). Good to a few percent for α ≤ 45°
  // and ranges ≤ 250 km — adequate for terminal-resolution rendering.
  const double cosE = std::cos(s.elangle * M_PI / 180.0);
  if (cosE <= 0) return std::numeric_limits<float>::quiet_NaN();
  const double rSlant = rGround / cosE;
  if (rSlant < s.rstart) return std::numeric_limits<float>::quiet_NaN();

  // Bin index along the ray.
  const long bin = static_cast<long>((rSlant - s.rstart) / s.rscale);
  if (bin < 0 || bin >= static_cast<long>(s.nbins))
    return std::numeric_limits<float>::quiet_NaN();

  // Azimuth from north, clockwise. atan2(east, north) gives the angle
  // measured from the +y (north) axis turning toward +x (east), i.e.
  // exactly the radar azimuth convention.
  double az = std::atan2(x, y) * 180.0 / M_PI;
  if (az < 0) az += 360.0;
  const double rayWidth = 360.0 / static_cast<double>(s.nrays);
  long ray = static_cast<long>(az / rayWidth);
  if (ray < 0) ray = 0;
  if (ray >= static_cast<long>(s.nrays)) ray = static_cast<long>(s.nrays) - 1;

  const float v = s.raw[static_cast<std::size_t>(ray) * s.nbins +
                        static_cast<std::size_t>(bin)];
  const double dv = v;
  // nodata: no measurement. undetect: clear-air below detection threshold.
  // Both render transparent — see the matching note in OdimSource.
  if (dv == s.nodata || dv == s.undetect) return std::numeric_limits<float>::quiet_NaN();
  return static_cast<float>(s.gain * dv + s.offset);
}

void OdimVolumeSource::uvToLatLon(double u, double v, double& lat, double& lon) const
{
  if (!itsArea)
  {
    DataSource::uvToLatLon(u, v, lat, lon);
    return;
  }
  const NFmiPoint xy(u * itsArea->Width(), v * itsArea->Height());
  const NFmiPoint world = itsArea->XYToWorldXY(xy);
  const NFmiPoint ll = itsArea->WorldXYToLatLon(world);
  lat = ll.Y();
  lon = ll.X();
}

void OdimVolumeSource::latLonToUV(double lat, double lon, double& u, double& v) const
{
  if (!itsArea)
  {
    DataSource::latLonToUV(lat, lon, u, v);
    return;
  }
  const NFmiPoint world = itsArea->LatLonToWorldXY(NFmiPoint(lon, lat));
  const NFmiPoint xy = itsArea->WorldXYToXY(world);
  const double w = itsArea->Width();
  const double h = itsArea->Height();
  u = w > 0 ? xy.X() / w : 0.0;
  v = h > 0 ? xy.Y() / h : 0.0;
}

LatLonBox OdimVolumeSource::boundingBox() const
{
  LatLonBox b;
  if (!itsArea) return b;

  // AEQD around the radar: the latitudinal extreme can sit between
  // corners (e.g. the northern edge crosses through the centre of the
  // top side, well above the corner). Sample the perimeter to capture it.
  b.minLat = std::numeric_limits<double>::infinity();
  b.maxLat = -std::numeric_limits<double>::infinity();
  b.minLon = std::numeric_limits<double>::infinity();
  b.maxLon = -std::numeric_limits<double>::infinity();
  const double w = itsArea->Width();
  const double h = itsArea->Height();
  auto add = [&](double x, double y) {
    NFmiPoint world = itsArea->XYToWorldXY(NFmiPoint(x, y));
    NFmiPoint ll = itsArea->WorldXYToLatLon(world);
    b.minLat = std::min(b.minLat, ll.Y());
    b.maxLat = std::max(b.maxLat, ll.Y());
    b.minLon = std::min(b.minLon, ll.X());
    b.maxLon = std::max(b.maxLon, ll.X());
  };
  constexpr int kSamples = 60;
  for (int i = 0; i <= kSamples; ++i)
  {
    const double t = static_cast<double>(i) / kSamples;
    add(0, t * h);
    add(w, t * h);
    add(t * w, 0);
    add(t * w, h);
  }
  return b;
}

std::vector<std::pair<std::string, std::string>> OdimVolumeSource::extraMetadata() const
{
  std::vector<std::pair<std::string, std::string>> rows;
  rows.emplace_back("Format", "ODIM HDF5 PVOL");
  rows.emplace_back("Source", itsSourceTag);
  rows.emplace_back("Product", itsProduct);
  rows.emplace_back("Quantity", itsQuantity);
  rows.emplace_back("Radar lat/lon",
                    fmt::format("{:.4f}, {:.4f}", itsRadarLat, itsRadarLon));
  rows.emplace_back("Radar height", fmt::format("{:g} m", itsRadarHeight));
  rows.emplace_back("Sweeps", std::to_string(itsSweeps.size()));
  if (!itsSweeps.empty())
  {
    const auto& lo = itsSweeps.front();
    const auto& hi = itsSweeps.back();
    rows.emplace_back("Elevation range",
                      fmt::format("{:g}° → {:g}°", lo.elangle, hi.elangle));
    rows.emplace_back("Rays × bins",
                      fmt::format("{} × {}", lo.nrays, lo.nbins));
    rows.emplace_back("Range gate", fmt::format("{:g} m", lo.rscale));
  }
  rows.emplace_back("Max range", fmt::format("{:g} km", itsMaxRange / 1000.0));
  return rows;
}

std::string OdimVolumeSource::gridSignature() const
{
  // The AEQD origin (radar position) plus the max range fully describes
  // the rendering grid; two PVOL files from the same radar share a grid
  // even if their sweep counts differ.
  char buf[160];
  std::snprintf(buf, sizeof(buf), "odim-pvol:%.5f,%.5f,%.0f",
                itsRadarLat, itsRadarLon, itsMaxRange);
  return buf;
}
}  // namespace Qdless
