#include "QdlessDataSource.h"

#include <fmt/format.h>

#include "QdlessCatalog.h"
#include "QdlessGdalRasterSource.h"
#include "QdlessGridFilesSource.h"
#include "QdlessImageSource.h"
#include "QdlessOdimSource.h"
#include "QdlessOdimVolumeSource.h"
#include "QdlessQueryDataSource.h"
#include "QdlessShapeSource.h"

#include <newbase/NFmiGlobals.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <gdal_priv.h>
#include <sstream>
#include <stdexcept>
#include <unistd.h>
#include <vector>

namespace Qdless
{
namespace
{
// grid-files writes "#### Geometry not found ####" (plus a suggested
// geometry-definition line) directly to stdout when a NetCDF grid is not
// registered in its geometry table. We probe exactly that condition to
// decide whether to fall back to GDAL, so silence fd 1 for the probe's
// duration — otherwise the message corrupts the rendered terminal frame
// (once per unregistered NetCDF parameter).
class StdoutSilencer
{
 public:
  StdoutSilencer()
  {
    std::fflush(stdout);
    itsSaved = ::dup(1);
    const int devnull = ::open("/dev/null", O_WRONLY);
    if (devnull >= 0)
    {
      ::dup2(devnull, 1);
      ::close(devnull);
    }
  }
  ~StdoutSilencer()
  {
    std::fflush(stdout);
    if (itsSaved >= 0)
    {
      ::dup2(itsSaved, 1);
      ::close(itsSaved);
    }
  }
  StdoutSilencer(const StdoutSilencer&) = delete;
  StdoutSilencer& operator=(const StdoutSilencer&) = delete;

 private:
  int itsSaved = -1;
};
enum class FileKind
{
  kQueryData,
  kGrib,
  kNetCDF,
  kHdf5,
  kGdalRaster,
  kImage,
  kShapefile,
  kUnknown,
};

FileKind detectKind(const std::string& filename)
{
  std::ifstream in(filename, std::ios::binary);
  if (!in) throw std::runtime_error("cannot open: " + filename);
  unsigned char hdr[16] = {};
  in.read(reinterpret_cast<char*>(hdr), sizeof(hdr));
  const std::streamsize n = in.gcount();
  if (n >= 4 && hdr[0] == 'G' && hdr[1] == 'R' && hdr[2] == 'I' && hdr[3] == 'B')
    return FileKind::kGrib;
  if (n >= 3 && hdr[0] == 'C' && hdr[1] == 'D' && hdr[2] == 'F') return FileKind::kNetCDF;
  // HDF5 magic is shared by NetCDF4 and ODIM. Disambiguate at open time
  // (cheap probe of /what/object) rather than guessing from the magic.
  if (n >= 4 && hdr[0] == 0x89 && hdr[1] == 'H' && hdr[2] == 'D' && hdr[3] == 'F')
    return FileKind::kHdf5;
  // TIFF magic: II*\0 (0x49 0x49 0x2A 0x00) or MM\0* (0x4D 0x4D 0x00 0x2A).
  // Require all four bytes — the bare II/MM BOM is also valid for many other
  // file types and we don't want false positives.
  if (n >= 4 && hdr[0] == 0x49 && hdr[1] == 0x49 && hdr[2] == 0x2A && hdr[3] == 0x00)
    return FileKind::kGdalRaster;
  if (n >= 4 && hdr[0] == 0x4D && hdr[1] == 0x4D && hdr[2] == 0x00 && hdr[3] == 0x2A)
    return FileKind::kGdalRaster;
  // Raw image formats. These have no spatial georeference — qdless renders
  // them in image-only mode (pixels straight to screen, overlays
  // suppressed). Detection is by magic only so we don't have to trial-open
  // every file with GDAL.
  // PNG: 89 50 4E 47 0D 0A 1A 0A
  if (n >= 4 && hdr[0] == 0x89 && hdr[1] == 'P' && hdr[2] == 'N' && hdr[3] == 'G')
    return FileKind::kImage;
  // JPEG: FF D8 FF
  if (n >= 3 && hdr[0] == 0xFF && hdr[1] == 0xD8 && hdr[2] == 0xFF)
    return FileKind::kImage;
  // GIF: "GIF87a" / "GIF89a"
  if (n >= 4 && hdr[0] == 'G' && hdr[1] == 'I' && hdr[2] == 'F' && hdr[3] == '8')
    return FileKind::kImage;
  // BMP: "BM"
  if (n >= 2 && hdr[0] == 'B' && hdr[1] == 'M')
    return FileKind::kImage;
  // WebP: "RIFF" .... "WEBP"
  if (n >= 12 && hdr[0] == 'R' && hdr[1] == 'I' && hdr[2] == 'F' && hdr[3] == 'F' &&
      hdr[8] == 'W' && hdr[9] == 'E' && hdr[10] == 'B' && hdr[11] == 'P')
    return FileKind::kImage;
  // ESRI shapefile: file code 9994 stored big-endian at offset 0.
  // Only the .shp file has this header; .shx / .dbf / .prj have
  // different magic and would not be passed as the qdless input.
  if (n >= 4 && hdr[0] == 0x00 && hdr[1] == 0x00 && hdr[2] == 0x27 && hdr[3] == 0x0A)
    return FileKind::kShapefile;
  // Fall through: assume newbase QueryData.
  return FileKind::kQueryData;
}

// One s3fs fuse mount as described by fstab. Files under `mountpoint` are
// objects in an S3 bucket; s3fs keeps a local on-disk copy of every object it
// has fetched under `cacheDir` (the use_cache= option), and those copies are
// ordinary files that can be memory-mapped directly — far cheaper than
// round-tripping the bucket through the fuse layer.
struct S3fsMount
{
  std::string mountpoint;  // e.g. "/gem-data"
  std::string bucket;      // device tail after '#', e.g. "gem-data"
  std::string cacheDir;    // use_cache= value
};

// Parse fstab once, returning the s3fs mounts that declare a use_cache dir,
// sorted most-specific-mountpoint-first so nested mounts resolve correctly.
// Empty (a no-op) on a machine with no s3fs mounts or no fstab.
const std::vector<S3fsMount>& s3fsMounts()
{
  static const std::vector<S3fsMount> mounts = []
  {
    std::vector<S3fsMount> out;
    std::ifstream in(MasalaCatalog::fstabPath());
    std::string line;
    while (std::getline(in, line))
    {
      // The device field legitimately contains '#' ("s3fs#bucket"), so only a
      // leading '#' marks a comment.
      const std::size_t first = line.find_first_not_of(" \t");
      if (first == std::string::npos || line[first] == '#') continue;
      std::istringstream ls(line);
      std::string device, mountpoint, fstype, options;
      if (!(ls >> device >> mountpoint >> fstype >> options)) continue;
      if (device.rfind("s3fs", 0) != 0) continue;  // not an s3fs entry
      S3fsMount m;
      m.mountpoint = mountpoint;
      if (const std::size_t hash = device.find('#'); hash != std::string::npos)
        m.bucket = device.substr(hash + 1);
      // Pull use_cache=<dir> out of the comma-separated options.
      std::stringstream os(options);
      std::string opt;
      while (std::getline(os, opt, ','))
      {
        constexpr std::string_view key = "use_cache=";
        if (opt.rfind(key, 0) == 0) m.cacheDir = opt.substr(key.size());
      }
      if (!m.mountpoint.empty() && !m.cacheDir.empty()) out.push_back(std::move(m));
    }
    std::sort(out.begin(), out.end(), [](const S3fsMount& a, const S3fsMount& b)
              { return a.mountpoint.size() > b.mountpoint.size(); });
    return out;
  }();
  return mounts;
}
}  // namespace

// GDAL/OGR last-resort probe. The fast magic-byte checks above route
// every format with a fixed signature (GRIB, NetCDF, TIFF, PNG, ...) to
// its specialised backend. Anything that didn't match gets one more
// chance here: ask GDAL whether it has a driver for the file, in either
// vector or raster role. OGR-vector is tried first because its "no, I
// don't recognise this" failure mode is faster (raster open will
// attempt georef recovery on a JPEG and similar speculative work).
// Returns nullptr if neither role recognises the file; callers fall
// back to QueryData and let that backend's own loader speak last.
std::unique_ptr<DataSource> tryGdalOpen(const std::string& filename)
{
  GDALAllRegister();
  if (auto* ds = static_cast<GDALDataset*>(GDALOpenEx(
          filename.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr)))
  {
    const int n = ds->GetLayerCount();
    GDALClose(ds);
    if (n > 0)
      return std::make_unique<ShapeSource>(filename);
  }
  if (auto* ds = static_cast<GDALDataset*>(GDALOpenEx(
          filename.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr)))
  {
    const int n = ds->GetRasterCount();
    GDALClose(ds);
    if (n > 0)
      return std::make_unique<GdalRasterSource>(filename);
  }
  return nullptr;
}

std::string DataSource::localCachePath(const std::string& path)
{
  const auto& mounts = s3fsMounts();
  if (mounts.empty()) return path;
  for (const auto& m : mounts)
  {
    // path must equal the mountpoint or sit strictly beneath it.
    if (path.size() <= m.mountpoint.size() ||
        path.compare(0, m.mountpoint.size(), m.mountpoint) != 0 ||
        path[m.mountpoint.size()] != '/')
      continue;
    const std::string rel = path.substr(m.mountpoint.size());  // includes leading '/'
    // s3fs stores cached objects as <use_cache>/<bucket>/<key>; some setups
    // fold the bucket name into use_cache already, so accept either layout and
    // use whichever local copy actually exists.
    std::error_code ec;
    for (const std::string& cand : {m.cacheDir + "/" + m.bucket + rel, m.cacheDir + rel})
      if (std::filesystem::is_regular_file(cand, ec)) return cand;
    return path;  // under an s3fs mount but not cached yet — read via fuse
  }
  return path;
}

std::unique_ptr<DataSource> DataSource::open(const std::string& requestedPath)
{
  // Redirect s3fs-backed paths to their local on-disk cache copy when one
  // exists, so detection + decoding mmap a local file instead of the fuse
  // mount. A no-op off s3fs or for a not-yet-cached object.
  const std::string filename = localCachePath(requestedPath);
  switch (detectKind(filename))
  {
    case FileKind::kQueryData:
      // QueryData has no magic; this is the magic-byte sniff's last guess.
      // Before committing to QD's loader (whose error messages are opaque
      // for non-QD inputs), try GDAL/OGR — picks up GeoPackage, KML,
      // GeoJSON, JPEG2000, COG, NITF, etc. that the fast checks missed.
      if (auto src = tryGdalOpen(filename))
        return src;
      return std::make_unique<QueryDataSource>(filename);
    case FileKind::kGrib:
      return std::make_unique<GridFilesSource>(filename);
    case FileKind::kNetCDF:
    {
      // grid-files is the primary NetCDF reader (handles levels / times /
      // multiple params for grids registered in its geometry table). But CF
      // NetCDF with a bare lon/lat axis grid that isn't registered yields no
      // geometry ("Geometry not found") and renders blank — wave / ocean
      // model output (WAM, NEMO) is exactly this. For those, fall back to
      // GDAL's CF reader, which georeferences the regular lat/lon grid
      // directly.
      std::unique_ptr<GridFilesSource> src;
      bool resolvable = false;
      {
        // Silence over construction + probe: grid-files emits the
        // "Geometry not found" banner from inside indexMessages too, not
        // only the explicit geometry probe.
        StdoutSilencer silence;
        src = std::make_unique<GridFilesSource>(filename);
        resolvable = src->geometryResolvable();
      }
      if (!resolvable)
      {
        // Go straight to the raster reader rather than tryGdalOpen(): GDAL's
        // netCDF driver also advertises an OGR *vector* layer for these
        // files, which tryGdalOpen would prefer and then fail to extent.
        try
        {
          return std::make_unique<GdalRasterSource>(filename);
        }
        catch (const std::exception&)
        {
          // GDAL couldn't georeference it either — keep the grid-files
          // source so the caller still gets a (blank) source with metadata
          // rather than a hard failure.
        }
      }
      return src;
    }
    case FileKind::kHdf5:
      // ODIM-H5 polar volume: separate backend that handles per-sweep
      // geometry and exposes elevations as levels. Probe before the 2D
      // OdimSource branch because both probes succeed for PVOL files.
      if (OdimVolumeSource::isVolume(filename))
        return std::make_unique<OdimVolumeSource>(filename);
      if (OdimSource::isOdim(filename))
        return std::make_unique<OdimSource>(filename);
      // NetCDF4 (HDF5 magic, no ODIM /what/object). Hand off to grid-files.
      return std::make_unique<GridFilesSource>(filename);
    case FileKind::kGdalRaster:
      return std::make_unique<GdalRasterSource>(filename);
    case FileKind::kImage:
      return std::make_unique<ImageSource>(filename);
    case FileKind::kShapefile:
      return std::make_unique<ShapeSource>(filename);
    case FileKind::kUnknown:
      break;
  }
  if (auto src = tryGdalOpen(filename))
    return src;
  throw std::runtime_error("unknown file format: " + requestedPath);
}

void DataSource::uvToLatLon(double u, double v, double& lat, double& lon) const
{
  // Default: interpolate inside the lat/lon bounding box. v=0 is the top
  // (max-lat) edge so the image-coord convention matches projected backends.
  const auto bbox = boundingBox();
  lat = bbox.maxLat - v * (bbox.maxLat - bbox.minLat);
  lon = bbox.minLon + u * (bbox.maxLon - bbox.minLon);
}

void DataSource::latLonToUV(double lat, double lon, double& u, double& v) const
{
  const auto bbox = boundingBox();
  const double latSpan = bbox.maxLat - bbox.minLat;
  const double lonSpan = bbox.maxLon - bbox.minLon;
  u = lonSpan > 0 ? (lon - bbox.minLon) / lonSpan : 0.0;
  v = latSpan > 0 ? (bbox.maxLat - lat) / latSpan : 0.0;
}

void DataSource::latLonToUVBatch(const std::vector<float>& lats,
                                 const std::vector<float>& lons,
                                 std::vector<float>& us,
                                 std::vector<float>& vs) const
{
  const std::size_t n = lats.size();
  us.resize(n);
  vs.resize(n);
  for (std::size_t i = 0; i < n; ++i)
  {
    double u = 0;
    double v = 0;
    latLonToUV(lats[i], lons[i], u, v);
    us[i] = static_cast<float>(u);
    vs[i] = static_cast<float>(v);
  }
}

std::string DataSource::levelLabel(std::size_t i) const
{
  return fmt::format("{:g}", levelValueAt(i));
}

std::vector<DataSource::LevelGroup> DataSource::levelGroupsForParam(int /*paramId*/) const
{
  // Default: one synthetic group containing the existing flat list.
  // Subclasses that know the level type override; this keeps Shape /
  // Image / Gdal / Odim sources working unchanged.
  LevelGroup g;
  g.typeId = 0;  // kFmiNoLevelType
  g.typeName = "Levels";
  g.values.reserve(levelCount());
  for (std::size_t i = 0; i < levelCount(); ++i)
    g.values.push_back(levelValueAt(i));
  g.ascendsWithValue = levelsAscendWithValue();
  return {g};
}

std::string DataSource::levelTypeName(int typeId)
{
  // FmiLevelType (newbase/NFmiLevelType.h) numeric constants. Names are
  // chosen for terseness in the level picker's section headers.
  switch (typeId)
  {
    case 1: return "Surface";
    case 50: return "Sounding";
    case 51: return "Amdar";
    case 100: return "Pressure (hPa)";
    case 102: return "Mean sea level";
    case 103: return "Altitude (m)";
    case 105: return "Height (m)";
    case 109: return "Hybrid";
    case 120: return "Flight level";
    case 160: return "Depth (m)";
    case 169: return "Road class 1";
    case 170: return "Road class 2";
    case 171: return "Road class 3";
    case 1001: return "SYNOP";
    default: return typeId == 0 ? "Levels" : fmt::format("Level type {}", typeId);
  }
}

std::string DataSource::formatLevelByType(int typeId, float value)
{
  switch (typeId)
  {
    case 1:  // Ground surface
      return "Surface";
    case 100:  // Pressure
      return fmt::format("{:g} hPa", value);
    case 103:  // Altitude
    case 105:  // Height
    case 160:  // Depth
      return fmt::format("{:g} m", value);
    case 120:  // Flight level
      return fmt::format("FL{:g}", value);
    case 102:  // Mean sea level
      return "MSL";
    default:
      return fmt::format("{:g}", value);
  }
}

float DataSource::levelToHeightMeters(int typeId, float levelValue)
{
  if (!std::isfinite(levelValue))
    return kFloatMissing;
  switch (typeId)
  {
    case 100:  // Pressure (hPa) → ISA hypsometric height. Valid (to within a
               // few hundred metres) from 1000 hPa to the low-stratosphere
               // levels NWP files carry, which is plenty for a viewer.
    {
      if (levelValue <= 0.0F)
        return kFloatMissing;
      return 44330.0F * (1.0F - std::pow(levelValue / 1013.25F, 0.1902632F));
    }
    case 103:  // Altitude (m)
    case 105:  // Height (m)
      return levelValue;
    default:
      // Hybrid (109), depth (160), ground (1), unknown: no self-contained
      // geometric height.
      return kFloatMissing;
  }
}

bool DataSource::sampleVolume(const std::function<void(const VolumeSample&)>& cb) const
{
  if (!isVolumetric())
    return false;

  // Coarse geographic lattice over the data extent. Each node samples one
  // column (all levels of the active param at this lat/lon) — for backends
  // whose columns read a file per level, the per-column cost is amortised by
  // the slice cache, and the lattice keeps the total emission count bounded
  // regardless of the native grid size.
  const auto bb = boundingBox();
  const double latSpan = bb.maxLat - bb.minLat;
  const double lonSpan = bb.maxLon - bb.minLon;
  if (latSpan <= 0.0 || lonSpan <= 0.0)
    return false;

  // Aim for ~120 columns across the wider axis; the shorter axis scales to
  // keep cells roughly square. ~120×90 columns × tens of levels is a few
  // hundred thousand emissions — comfortably interactive.
  constexpr int kMaxAcross = 120;
  const double aspect = lonSpan / latSpan;
  int nx = kMaxAcross;
  int ny = kMaxAcross;
  if (aspect >= 1.0)
    ny = std::max(2, static_cast<int>(std::lround(kMaxAcross / aspect)));
  else
    nx = std::max(2, static_cast<int>(std::lround(kMaxAcross * aspect)));

  bool any = false;
  for (int j = 0; j < ny; ++j)
  {
    const double lat = bb.minLat + (latSpan * j) / (ny - 1);
    for (int i = 0; i < nx; ++i)
    {
      const double lon = bb.minLon + (lonSpan * i) / (nx - 1);
      const ColumnProfile col = sampleColumnProfile(lat, lon);
      const std::size_t n = std::min(col.heightsM.size(), col.values.size());
      for (std::size_t k = 0; k < n; ++k)
      {
        const float h = col.heightsM[k];
        const float v = col.values[k];
        if (h == kFloatMissing || v == kFloatMissing || !std::isfinite(h) || !std::isfinite(v))
          continue;
        cb(VolumeSample{lat, lon, static_cast<double>(h), v});
        any = true;
      }
    }
  }
  return any;
}

std::string DataSource::gridSignature() const
{
  // Default: lat/lon bbox to 6 decimals (~10 cm). Adequate for
  // unprojected sources; backends with a known projection should
  // override with something more discriminating.
  const auto bbox = boundingBox();
  char buf[160];
  std::snprintf(buf, sizeof(buf), "bbox:%.6f,%.6f,%.6f,%.6f",
                bbox.minLat, bbox.maxLat, bbox.minLon, bbox.maxLon);
  return buf;
}
}  // namespace Qdless
