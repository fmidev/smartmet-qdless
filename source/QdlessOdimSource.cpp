#include "QdlessOdimSource.h"

#include "Hdf5File.h"

#include <gis/ProjInfo.h>
#include <macgyver/StringConversion.h>
#include <newbase/NFmiArea.h>
#include <newbase/NFmiEnumConverter.h>
#include <newbase/NFmiParameterName.h>
#include <newbase/NFmiPoint.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace Qdless
{
namespace
{
// Mirrors h5toqd's opera_name_to_newbase, trimmed to the (product, quantity)
// combinations we expect from 2D composites. Returns 0 if unknown so the
// caller can fall back to displaying the literal quantity string.
int operaToNewbase(const std::string& product, const std::string& quantity)
{
  if (product == "PPI" || product == "CAPPI" || product == "PCAPPI")
  {
    if (quantity == "TH" || quantity == "DBZ") return kFmiReflectivity;
    if (quantity == "DBZH") return kFmiCorrectedReflectivity;
    if (quantity == "VRAD") return kFmiRadialVelocity;
    if (quantity == "WRAD" || quantity == "W") return kFmiSpectralWidth;
    if (quantity == "RATE") return kFmiPrecipitationRate;
  }
  else if (product == "ETOP")
  {
    if (quantity == "HGHT") return kFmiEchoTop;
  }
  else if (product == "MAX")
  {
    if (quantity == "TH") return kFmiReflectivity;
    if (quantity == "DBZH") return kFmiCorrectedReflectivity;
  }
  else if (product == "RR")
  {
    if (quantity == "ACRR") return kFmiPrecipitationAmount;
    if (quantity == "RATE") return kFmiPrecipitationRate;
  }
  else if (product == "VIL")
  {
    if (quantity == "ACRR") return kFmiPrecipitationAmount;
  }
  else if (product == "COMP")
  {
    if (quantity == "RATE") return kFmiPrecipitationRate;
    if (quantity == "BRDR") return kFmiRadarBorder;
    if (quantity == "TH") return kFmiReflectivity;
    if (quantity == "DBZH") return kFmiCorrectedReflectivity;
    if (quantity == "ACRR") return kFmiPrecipitationAmount;
    if (quantity == "QIND") return kFmiSignalQualityIndex;
  }
  // Quantity-only fallbacks (don't depend on product). Many local products
  // (LMR, LTB, SMV, PAC, SRI, MAX, ETOP, …) reuse standard quantities.
  if (quantity == "TH" || quantity == "DBZ") return kFmiReflectivity;
  if (quantity == "DBZH") return kFmiCorrectedReflectivity;
  if (quantity == "VRAD") return kFmiRadialVelocity;
  if (quantity == "WRAD" || quantity == "W") return kFmiSpectralWidth;
  if (quantity == "RATE") return kFmiPrecipitationRate;
  if (quantity == "ACRR") return kFmiPrecipitationAmount;
  if (quantity == "HGHT") return kFmiEchoTop;
  return 0;
}

// Standard SI units for the common ODIM quantities. The files don't carry
// unit strings, but the OPERA ODIM spec fixes the units per quantity.
std::string operaUnits(const std::string& quantity)
{
  if (quantity == "TH" || quantity == "DBZ" || quantity == "DBZH") return "dBZ";
  if (quantity == "VRAD") return "m/s";
  if (quantity == "WRAD" || quantity == "W") return "m/s";
  if (quantity == "RATE") return "mm/h";
  if (quantity == "ACRR") return "mm";
  if (quantity == "HGHT") return "km";
  if (quantity == "ZDR") return "dB";
  return {};
}

// Parse "YYYYMMDD" + "HHMMSS" → NFmiMetTime. Files are always UTC by spec.
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
}  // namespace

bool OdimSource::isOdim(const std::string& filename)
{
  // Cheap probe: open via GDAL HDF5 driver and look for /what/object. If the
  // file is a NetCDF4 (no /what group), this returns false.
  try
  {
    Fmi::HDF5::Hdf5File f(filename);
    return f.is_attribute<std::string>("/what", "object");
  }
  catch (...)
  {
    return false;
  }
}

OdimSource::OdimSource(const std::string& filename) : itsFilename(filename)
{
  Fmi::HDF5::Hdf5File file(filename);

  itsObject = file.get_attribute<std::string>("/what", "object");
  if (itsObject != "IMAGE" && itsObject != "COMP" && itsObject != "CVOL")
    throw std::runtime_error("ODIM object='" + itsObject + "' not supported (only IMAGE/COMP/CVOL)");

  // Geometry from /where + /where/projdef. Strip x_0/y_0 like h5toqd does so
  // the corner coordinates align with the projection origin.
  itsProjDef = file.get_attribute<std::string>("/where", "projdef");
  Fmi::ProjInfo proj(itsProjDef);
  proj.erase("x_0");
  proj.erase("y_0");
  std::string projdef = proj.projStr();
  std::string sphere = proj.inverseProjStr();

  itsNx = static_cast<std::size_t>(file.get_attribute<long>("/where", "xsize"));
  itsNy = static_cast<std::size_t>(file.get_attribute<long>("/where", "ysize"));

  // Two corner conventions: Latvian-style (UL,LR) and FMI-style (LL,UR).
  if (file.is_attribute<double>("/where", "LL_lon"))
  {
    double LL_lon = file.get_attribute<double>("/where", "LL_lon");
    double LL_lat = file.get_attribute<double>("/where", "LL_lat");
    double UR_lon = file.get_attribute<double>("/where", "UR_lon");
    double UR_lat = file.get_attribute<double>("/where", "UR_lat");
    itsArea.reset(NFmiArea::CreateFromCorners(
        projdef, sphere, NFmiPoint(LL_lon, LL_lat), NFmiPoint(UR_lon, UR_lat)));
  }
  else
  {
    double UL_lon = file.get_attribute<double>("/where", "UL_lon");
    double UL_lat = file.get_attribute<double>("/where", "UL_lat");
    double LR_lon = file.get_attribute<double>("/where", "LR_lon");
    double LR_lat = file.get_attribute<double>("/where", "LR_lat");
    itsArea.reset(NFmiArea::CreateFromReverseCorners(
        projdef, sphere, NFmiPoint(UL_lon, UL_lat), NFmiPoint(LR_lon, LR_lat)));
  }
  if (!itsArea) throw std::runtime_error("failed to construct projection for " + filename);

  // Time. /what/date + /what/time is the canonical valid time (end of
  // accumulation for time-integrated products, observation time otherwise).
  std::string date = file.get_attribute<std::string>("/what", "date");
  std::string time = file.get_attribute<std::string>("/what", "time");
  itsValidTime = parseOdimTime(date, time);

  // Per-dataset metadata. We always look at /dataset1 — the only dataset for
  // 2D composites. Polar volumes (PVOL) have /dataset{1..N} per elevation
  // and are filtered out by the object check above.
  itsQuantity = file.get_attribute<std::string>("/dataset1/what", "quantity");
  itsProduct = file.get_attribute<std::string>("/dataset1/what", "product");
  itsGain = file.get_attribute<double>("/dataset1/what", "gain");
  itsOffset = file.get_attribute<double>("/dataset1/what", "offset");
  itsNodata = file.get_attribute<double>("/dataset1/what", "nodata");
  itsUndetect = file.get_attribute<double>("/dataset1/what", "undetect");
  if (auto pp = file.get_optional_attribute<double>("/dataset1/what", "prodpar"))
    itsLevelValue = *pp;

  itsParamId = operaToNewbase(itsProduct, itsQuantity);
  itsParamUnits = operaUnits(itsQuantity);

  // Read the raw raster as float, then unscale. GDAL handles the byte-order
  // conversion (the dbz.h5 sample is U16BE) and integer→float promotion.
  // Path "/dataset1" identifies the group; Hdf5File::read_dataset finds the
  // single sub-raster inside it (works for both single-image files and
  // GDAL-subdataset-style files).
  std::vector<float> raw = file.read_dataset<float>("/dataset1");
  if (raw.size() != itsNx * itsNy)
    throw std::runtime_error("ODIM dataset size " + std::to_string(raw.size()) + " != " +
                             std::to_string(itsNx) + "x" + std::to_string(itsNy));

  itsValues.resize(raw.size());
  const float nan = std::numeric_limits<float>::quiet_NaN();
  for (std::size_t i = 0; i < raw.size(); ++i)
  {
    const double v = raw[i];
    // Both nodata (no measurement) and undetect (clear-air below detection
    // threshold) render transparent: radar values can legitimately be
    // negative, so painting clear-air as zero would mislead.
    if (v == itsNodata || v == itsUndetect)
      itsValues[i] = nan;
    else
      itsValues[i] = static_cast<float>(itsGain * v + itsOffset);
  }
}

OdimSource::~OdimSource() = default;

std::vector<int> OdimSource::paramIds() const
{
  return {itsParamId};
}

std::string OdimSource::paramShortName(int /*paramId*/) const
{
  if (itsParamId != 0)
  {
    NFmiEnumConverter conv;
    auto name = conv.ToString(itsParamId);
    if (!name.empty()) return name;
  }
  // Unknown quantity: fall back to the ODIM string (e.g. "ACRR", "TH").
  return itsQuantity.empty() ? std::string{"Unknown"} : itsQuantity;
}

std::string OdimSource::paramLongName(int paramId) const { return paramShortName(paramId); }

std::string OdimSource::paramUnits(int /*paramId*/) const { return itsParamUnits; }

int OdimSource::currentParamId() const { return itsParamId; }

bool OdimSource::selectParamId(int paramId) { return paramId == itsParamId; }

std::size_t OdimSource::timeCount() const { return 1; }
std::size_t OdimSource::currentTimeIndex() const { return 0; }
void OdimSource::selectTimeIndex(std::size_t /*i*/) {}
NFmiMetTime OdimSource::currentValidTime() const { return itsValidTime; }
NFmiMetTime OdimSource::originTime() const { return itsValidTime; }

std::size_t OdimSource::levelCount() const { return 1; }
std::size_t OdimSource::currentLevelIndex() const { return 0; }
void OdimSource::selectLevelIndex(std::size_t /*i*/) {}
float OdimSource::levelValueAt(std::size_t /*i*/) const
{
  return static_cast<float>(itsLevelValue);
}

float OdimSource::interpolatedValue(double lat, double lon) const
{
  if (!itsArea || itsValues.empty())
    return std::numeric_limits<float>::quiet_NaN();
  // Map (lat,lon) to fractional grid (col, row). u=0 is left edge, v=0 is
  // top edge (NFmiArea image-coord convention). Out-of-grid → NaN.
  const NFmiPoint world = itsArea->LatLonToWorldXY(NFmiPoint(lon, lat));
  const NFmiPoint xy = itsArea->WorldXYToXY(world);
  const double w = itsArea->Width();
  const double h = itsArea->Height();
  if (w <= 0 || h <= 0) return std::numeric_limits<float>::quiet_NaN();
  const double u = xy.X() / w;
  const double v = xy.Y() / h;
  if (!(u >= 0 && u <= 1 && v >= 0 && v <= 1))
    return std::numeric_limits<float>::quiet_NaN();

  // Nearest-neighbour. Radar data is naturally pixelated and bilinear
  // interpolation across nodata/undetect cells produces misleading averages.
  long col = static_cast<long>(u * static_cast<double>(itsNx));
  long row = static_cast<long>(v * static_cast<double>(itsNy));
  if (col < 0) col = 0;
  if (row < 0) row = 0;
  if (col >= static_cast<long>(itsNx)) col = static_cast<long>(itsNx) - 1;
  if (row >= static_cast<long>(itsNy)) row = static_cast<long>(itsNy) - 1;
  return itsValues[static_cast<std::size_t>(row) * itsNx + static_cast<std::size_t>(col)];
}

void OdimSource::uvToLatLon(double u, double v, double& lat, double& lon) const
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

void OdimSource::latLonToUV(double lat, double lon, double& u, double& v) const
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

LatLonBox OdimSource::boundingBox() const
{
  LatLonBox b;
  if (!itsArea) return b;

  // Sample around the perimeter (same approach as QueryDataSource): for a
  // projected grid, the four corners can underestimate the latitudinal
  // span — e.g. an azimuthal-equidistant grid centered far from a corner
  // can reach a latitude that no corner does.
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

std::vector<std::pair<std::string, std::string>> OdimSource::extraMetadata() const
{
  std::vector<std::pair<std::string, std::string>> rows;
  rows.emplace_back("Format", "ODIM HDF5");
  rows.emplace_back("Object", itsObject);
  rows.emplace_back("Product", itsProduct);
  rows.emplace_back("Quantity", itsQuantity);
  rows.emplace_back("Grid size", std::to_string(itsNx) + "x" + std::to_string(itsNy));
  if (!itsProjDef.empty()) rows.emplace_back("Projection", itsProjDef);
  rows.emplace_back("Gain", Fmi::to_string(itsGain));
  rows.emplace_back("Offset", Fmi::to_string(itsOffset));
  rows.emplace_back("Nodata", Fmi::to_string(itsNodata));
  rows.emplace_back("Undetect", Fmi::to_string(itsUndetect));
  return rows;
}

std::string OdimSource::gridSignature() const
{
  // Two ODIM files often share one projdef (a network-wide projection
  // anchored on a producer's reference radar) but crop a different
  // (UL,LR) window. Same projdef + same dimensions ≠ same grid — we
  // also need the corner coords or the aggregator would render the
  // wrong area for any time step but the reference.
  char corners[160];
  if (itsArea)
  {
    const auto tl = itsArea->TopLeftLatLon();
    const auto br = itsArea->BottomRightLatLon();
    std::snprintf(corners, sizeof(corners), "%.4f,%.4f-%.4f,%.4f",
                  tl.Y(), tl.X(), br.Y(), br.X());
  }
  else
  {
    corners[0] = '?';
    corners[1] = '\0';
  }
  return std::string("odim:") + itsProjDef + "|" + std::to_string(itsNx) + "x" +
         std::to_string(itsNy) + "|" + corners;
}
}  // namespace Qdless
