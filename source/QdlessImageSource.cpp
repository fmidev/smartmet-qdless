#include "QdlessImageSource.h"

#include <cpl_error.h>
#include <gdal_priv.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <regex>
#include <stdexcept>

namespace Qdless
{
namespace
{
NFmiMetTime parseUtcStamp(const std::string& s)
{
  if (s.size() < 12) return NFmiMetTime();
  try
  {
    short yy = static_cast<short>(std::stoi(s.substr(0, 4)));
    short mm = static_cast<short>(std::stoi(s.substr(4, 2)));
    short dd = static_cast<short>(std::stoi(s.substr(6, 2)));
    short h = static_cast<short>(std::stoi(s.substr(8, 2)));
    short mi = static_cast<short>(std::stoi(s.substr(10, 2)));
    short se = (s.size() >= 14) ? static_cast<short>(std::stoi(s.substr(12, 2))) : 0;
    return NFmiMetTime(yy, mm, dd, h, mi, se);
  }
  catch (...)
  {
    return NFmiMetTime();
  }
}

NFmiMetTime parseTimeFromName(const std::string& filename)
{
  const std::string base = std::filesystem::path(filename).filename().string();
  static const std::regex re(R"(^(\d{12,14}))");
  std::smatch m;
  if (std::regex_search(base, m, re))
    return parseUtcStamp(m[1]);
  std::error_code ec;
  auto ftime = std::filesystem::last_write_time(filename, ec);
  if (ec) return NFmiMetTime();
  const auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
      ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
  const auto t = std::chrono::system_clock::to_time_t(sctp);
  std::tm utc{};
  gmtime_r(&t, &utc);
  return NFmiMetTime(static_cast<short>(utc.tm_year + 1900),
                     static_cast<short>(utc.tm_mon + 1),
                     static_cast<short>(utc.tm_mday),
                     static_cast<short>(utc.tm_hour),
                     static_cast<short>(utc.tm_min),
                     static_cast<short>(utc.tm_sec));
}
}  // namespace

ImageSource::ImageSource(const std::string& filename) : itsFilename(filename)
{
  GDALAllRegister();
  auto* ds = static_cast<GDALDataset*>(GDALOpen(filename.c_str(), GA_ReadOnly));
  if (!ds) throw std::runtime_error("GDAL failed to open: " + filename);
  struct Guard
  {
    GDALDataset* p;
    ~Guard() { if (p) GDALClose(p); }
  } guard{ds};

  itsNx = static_cast<std::size_t>(ds->GetRasterXSize());
  itsNy = static_cast<std::size_t>(ds->GetRasterYSize());
  if (itsNx == 0 || itsNy == 0) throw std::runtime_error("empty image: " + filename);

  if (auto* drv = ds->GetDriver(); drv != nullptr)
  {
    if (const char* sn = drv->GetDescription(); sn != nullptr) itsFormat = sn;
  }

  // Pick the right reader for the PNG/WebP/... colour model:
  //   - Paletted (1 band, GCI_PaletteIndex, has ColorTable): index → RGB(A).
  //   - Truecolour RGB / RGBA (3 / 4 bands): channel-per-band.
  //   - Grayscale (1 band, no palette): replicate single channel to all three.
  // Alpha (band 4 in RGBA, or palette entry c4 in paletted) below 128 maps
  // to Rgb::transparent so semi/transparent pixels don't paint a colour.
  const int nBands = ds->GetRasterCount();
  if (nBands < 1) throw std::runtime_error("no raster bands: " + filename);

  const std::size_t pixelCount = itsNx * itsNy;
  itsPixels.resize(pixelCount);

  auto readBand = [&](int idx, std::vector<std::uint8_t>& dst) {
    dst.resize(pixelCount);
    GDALRasterBand* b = ds->GetRasterBand(idx);
    if (!b) throw std::runtime_error("missing band " + std::to_string(idx) + ": " + filename);
    CPLErr err = b->RasterIO(GF_Read, 0, 0, static_cast<int>(itsNx), static_cast<int>(itsNy),
                             dst.data(), static_cast<int>(itsNx), static_cast<int>(itsNy),
                             GDT_Byte, 0, 0);
    if (err != CE_None) throw std::runtime_error("RasterIO failed: " + filename);
  };

  GDALRasterBand* band1 = ds->GetRasterBand(1);
  GDALColorTable* ct = nBands == 1 ? band1->GetColorTable() : nullptr;
  const bool paletted = (ct != nullptr) &&
                        (band1->GetColorInterpretation() == GCI_PaletteIndex);

  if (paletted)
  {
    // Pre-flatten the palette to a 256-entry Rgb table indexed by byte
    // value. Out-of-range indices (palette has fewer than 256 entries)
    // render transparent.
    std::array<Rgb, 256> lut{};
    for (auto& e : lut) e.transparent = true;
    const int n = ct->GetColorEntryCount();
    for (int k = 0; k < n && k < 256; ++k)
    {
      GDALColorEntry e;
      ct->GetColorEntryAsRGB(k, &e);
      Rgb px;
      px.r = static_cast<std::uint8_t>(e.c1);
      px.g = static_cast<std::uint8_t>(e.c2);
      px.b = static_cast<std::uint8_t>(e.c3);
      px.transparent = (e.c4 < 128);
      lut[static_cast<std::size_t>(k)] = px;
    }
    std::vector<std::uint8_t> idx;
    readBand(1, idx);
    for (std::size_t i = 0; i < pixelCount; ++i)
      itsPixels[i] = lut[idx[i]];
  }
  else
  {
    std::vector<std::uint8_t> rband;
    std::vector<std::uint8_t> gband;
    std::vector<std::uint8_t> bband;
    std::vector<std::uint8_t> aband;
    readBand(1, rband);
    if (nBands >= 3)
    {
      readBand(2, gband);
      readBand(3, bband);
    }
    if (nBands >= 4) readBand(4, aband);

    for (std::size_t i = 0; i < pixelCount; ++i)
    {
      Rgb px;
      if (nBands >= 3)
      {
        px.r = rband[i];
        px.g = gband[i];
        px.b = bband[i];
      }
      else
      {
        px.r = px.g = px.b = rband[i];
      }
      if (nBands >= 4 && aband[i] < 128) px.transparent = true;
      itsPixels[i] = px;
    }
  }

  itsValidTime = parseTimeFromName(filename);
}

ImageSource::~ImageSource() = default;

std::vector<int> ImageSource::paramIds() const { return {1}; }
std::string ImageSource::paramShortName(int /*paramId*/) const
{
  // Just the basename — the "parameter" of an image is the image itself.
  return std::filesystem::path(itsFilename).filename().string();
}
std::string ImageSource::paramLongName(int paramId) const { return paramShortName(paramId); }
std::string ImageSource::paramUnits(int /*paramId*/) const { return {}; }
int ImageSource::currentParamId() const { return 1; }
bool ImageSource::selectParamId(int paramId) { return paramId == 1; }

std::size_t ImageSource::timeCount() const { return 1; }
std::size_t ImageSource::currentTimeIndex() const { return 0; }
void ImageSource::selectTimeIndex(std::size_t /*i*/) {}
NFmiMetTime ImageSource::currentValidTime() const { return itsValidTime; }
NFmiMetTime ImageSource::originTime() const { return itsValidTime; }

std::size_t ImageSource::levelCount() const { return 1; }
std::size_t ImageSource::currentLevelIndex() const { return 0; }
void ImageSource::selectLevelIndex(std::size_t /*i*/) {}
float ImageSource::levelValueAt(std::size_t /*i*/) const { return 0; }

float ImageSource::interpolatedValue(double /*lat*/, double /*lon*/) const
{
  return std::numeric_limits<float>::quiet_NaN();
}

LatLonBox ImageSource::boundingBox() const
{
  // Synthetic unit-square bbox so the renderer's viewport math has
  // something coherent. The values are not physically meaningful.
  LatLonBox b;
  b.minLat = 0;
  b.maxLat = 1;
  b.minLon = 0;
  b.maxLon = 1;
  return b;
}

void ImageSource::uvToLatLon(double u, double v, double& lat, double& lon) const
{
  // Identity mapping with v=0 = top (matching all other backends).
  lat = 1.0 - v;
  lon = u;
}

void ImageSource::latLonToUV(double lat, double lon, double& u, double& v) const
{
  u = lon;
  v = 1.0 - lat;
}

Rgb ImageSource::pixelAtUV(double u, double v) const
{
  if (itsPixels.empty() || itsNx == 0 || itsNy == 0)
    return Rgb{0, 0, 0, true};
  if (u < 0 || u >= 1 || v < 0 || v >= 1)
    return Rgb{0, 0, 0, true};
  // Nearest neighbour. Bilinear blending between adjacent pixels would
  // smudge the producer's antialiasing in unpredictable ways — for a
  // pre-rendered image the user expects pixel-faithful display.
  const auto col = static_cast<std::size_t>(u * static_cast<double>(itsNx));
  const auto row = static_cast<std::size_t>(v * static_cast<double>(itsNy));
  return itsPixels[row * itsNx + col];
}

std::vector<std::pair<std::string, std::string>> ImageSource::extraMetadata() const
{
  std::vector<std::pair<std::string, std::string>> rows;
  rows.emplace_back("Format", itsFormat.empty() ? std::string{"image"} : itsFormat);
  rows.emplace_back("Image size", std::to_string(itsNx) + "x" + std::to_string(itsNy));
  return rows;
}

std::string ImageSource::gridSignature() const
{
  // Pure dimension-based: two images of different sizes shouldn't be
  // animated together, but two same-size images can be regardless of
  // content. Includes format so a PNG and a WebP of the same dims aren't
  // accidentally treated as a single animation.
  return std::string("img:") + itsFormat + "|" + std::to_string(itsNx) + "x" +
         std::to_string(itsNy);
}
}  // namespace Qdless
