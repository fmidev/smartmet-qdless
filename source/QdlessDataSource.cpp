#include "QdlessDataSource.h"

#include <fmt/format.h>

#include "QdlessGeoTiffSource.h"
#include "QdlessGridFilesSource.h"
#include "QdlessImageSource.h"
#include "QdlessOdimSource.h"
#include "QdlessOdimVolumeSource.h"
#include "QdlessQueryDataSource.h"
#include "QdlessShapeSource.h"

#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace Qdless
{
namespace
{
enum class FileKind
{
  kQueryData,
  kGrib,
  kNetCDF,
  kHdf5,
  kGeoTiff,
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
    return FileKind::kGeoTiff;
  if (n >= 4 && hdr[0] == 0x4D && hdr[1] == 0x4D && hdr[2] == 0x00 && hdr[3] == 0x2A)
    return FileKind::kGeoTiff;
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
}  // namespace

std::unique_ptr<DataSource> DataSource::open(const std::string& filename)
{
  switch (detectKind(filename))
  {
    case FileKind::kQueryData:
      return std::make_unique<QueryDataSource>(filename);
    case FileKind::kGrib:
    case FileKind::kNetCDF:
      return std::make_unique<GridFilesSource>(filename);
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
    case FileKind::kGeoTiff:
      return std::make_unique<GeoTiffSource>(filename);
    case FileKind::kImage:
      return std::make_unique<ImageSource>(filename);
    case FileKind::kShapefile:
      return std::make_unique<ShapeSource>(filename);
    case FileKind::kUnknown:
      break;
  }
  throw std::runtime_error("unknown file format: " + filename);
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

std::string DataSource::levelLabel(std::size_t i) const
{
  return fmt::format("{:g}", levelValueAt(i));
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
