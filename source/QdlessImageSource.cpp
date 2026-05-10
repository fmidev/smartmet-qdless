#include "QdlessImageSource.h"

#include <cpl_error.h>
#include <gdal_priv.h>
#include <webp/demux.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
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
    // timeStep=1 keeps minute resolution (60-minute default snaps
    // 14:30 → 15:00). NFmiMetTime also hardcodes SetSec(0) inside its
    // NearestMetTime helper, so re-apply seconds post-construction.
    NFmiMetTime r(yy, mm, dd, h, mi, /*sec=*/0, /*timeStep=*/1);
    r.SetSec(se);
    return r;
  }
  catch (...)
  {
    return NFmiMetTime();
  }
}

NFmiMetTime mtimeUtc(const std::string& filename)
{
  std::error_code ec;
  auto ftime = std::filesystem::last_write_time(filename, ec);
  if (ec) return NFmiMetTime();
  const auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
      ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
  const auto t = std::chrono::system_clock::to_time_t(sctp);
  std::tm utc{};
  gmtime_r(&t, &utc);
  NFmiMetTime r(static_cast<short>(utc.tm_year + 1900),
                static_cast<short>(utc.tm_mon + 1),
                static_cast<short>(utc.tm_mday),
                static_cast<short>(utc.tm_hour),
                static_cast<short>(utc.tm_min),
                /*sec=*/0,
                /*timeStep=*/1);
  r.SetSec(static_cast<short>(utc.tm_sec));
  return r;
}

NFmiMetTime parseTimeFromName(const std::string& filename)
{
  const std::string base = std::filesystem::path(filename).filename().string();
  // Locate a 12- or 14-digit run anywhere in the basename. Producers
  // vary on placement: some prefix the file with the timestamp
  // (`202605081430_RR.png`), others put it in the middle
  // (`HAV_202605081430_RR.png`). The {12,14} length bound avoids
  // false matches against shorter numeric tokens (versions, indices).
  static const std::regex re(R"((\d{12,14}))");
  std::smatch m;
  if (std::regex_search(base, m, re))
    return parseUtcStamp(m[1]);
  return mtimeUtc(filename);
}

// Add `addMs` milliseconds to `t`, returning a new NFmiMetTime. Used
// to lay animated-WebP frames out on a timeline starting at the
// file's mtime so the time-bar scrolls smoothly during playback.
NFmiMetTime addMillis(const NFmiMetTime& t, int addMs)
{
  if (t.GetYear() == 0) return t;
  // Compose to time_t in UTC, add the offset, decompose. NFmiMetTime
  // doesn't expose a direct +duration helper that respects sub-minute
  // precision so we do it through std::tm.
  std::tm tm{};
  tm.tm_year = t.GetYear() - 1900;
  tm.tm_mon = t.GetMonth() - 1;
  tm.tm_mday = t.GetDay();
  tm.tm_hour = t.GetHour();
  tm.tm_min = t.GetMin();
  tm.tm_sec = t.GetSec();
  // timegm interprets tm as UTC; portable on glibc.
  std::time_t epoch = timegm(&tm);
  epoch += addMs / 1000;
  std::tm out{};
  gmtime_r(&epoch, &out);
  // NFmiMetTime's NearestMetTime hardcodes SetSec(0) at every
  // construction, so frames 2s, 4s, 6s … apart all collapse to
  // HH:MM:00 if we go through the seconds-bearing constructor.
  // Instead, construct with se=0 and reapply seconds afterwards.
  NFmiMetTime r(static_cast<short>(out.tm_year + 1900),
                static_cast<short>(out.tm_mon + 1),
                static_cast<short>(out.tm_mday),
                static_cast<short>(out.tm_hour),
                static_cast<short>(out.tm_min),
                /*sec=*/0,
                /*timeStep=*/1);
  r.SetSec(static_cast<short>(out.tm_sec));
  return r;
}

// Read an entire file into memory. Used by the WebP animated path
// because libwebpdemux's API takes a buffer, not a path.
std::vector<std::uint8_t> slurp(const std::string& path)
{
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) return {};
  const auto sz = static_cast<std::streamsize>(in.tellg());
  in.seekg(0, std::ios::beg);
  std::vector<std::uint8_t> buf(static_cast<std::size_t>(sz));
  if (sz > 0 && !in.read(reinterpret_cast<char*>(buf.data()), sz)) return {};
  return buf;
}

// Try the libwebpdemux animated-WebP path. Returns true iff `filename`
// is a multi-frame WebP and decoding succeeded; on success populates
// `frames`, `times`, `nx`, `ny` and writes "WebP (animated, N frames)"
// to `format`. On false the caller falls back to GDAL.
bool tryDecodeAnimatedWebP(const std::string& filename,
                           std::vector<std::vector<Rgb>>& frames,
                           std::vector<NFmiMetTime>& times,
                           std::size_t& nx, std::size_t& ny,
                           std::string& format)
{
  auto buf = slurp(filename);
  if (buf.size() < 16) return false;
  // Cheap gate: only feed real WebP bytes to the demuxer (it would
  // accept and reject anything else, but the malloc + parse is wasted
  // work on a non-WebP).
  if (!(buf[0] == 'R' && buf[1] == 'I' && buf[2] == 'F' && buf[3] == 'F' &&
        buf[8] == 'W' && buf[9] == 'E' && buf[10] == 'B' && buf[11] == 'P'))
    return false;

  WebPData data{buf.data(), buf.size()};
  WebPAnimDecoderOptions opts;
  if (!WebPAnimDecoderOptionsInit(&opts)) return false;
  opts.color_mode = MODE_RGBA;
  WebPAnimDecoder* dec = WebPAnimDecoderNew(&data, &opts);
  if (dec == nullptr) return false;

  WebPAnimInfo info{};
  if (!WebPAnimDecoderGetInfo(dec, &info))
  {
    WebPAnimDecoderDelete(dec);
    return false;
  }
  // A single-frame WebP is also reported by WebPAnimDecoder, but we
  // want the GDAL path for those (it's already shipped, well-tested,
  // and exercises any extra features like ICC profile metadata that
  // qdless might want to read in the future). Only take the demuxer
  // path when there's more than one frame.
  if (info.frame_count <= 1)
  {
    WebPAnimDecoderDelete(dec);
    return false;
  }

  nx = info.canvas_width;
  ny = info.canvas_height;
  const std::size_t pixels = nx * ny;
  const NFmiMetTime base = mtimeUtc(filename);
  frames.reserve(info.frame_count);
  times.reserve(info.frame_count);

  while (WebPAnimDecoderHasMoreFrames(dec))
  {
    std::uint8_t* rgba = nullptr;
    int timestampMs = 0;
    if (!WebPAnimDecoderGetNext(dec, &rgba, &timestampMs)) break;
    std::vector<Rgb> frame(pixels);
    for (std::size_t i = 0; i < pixels; ++i)
    {
      Rgb px;
      px.r = rgba[i * 4 + 0];
      px.g = rgba[i * 4 + 1];
      px.b = rgba[i * 4 + 2];
      // Alpha < 128 → transparent (matches the static-image path).
      if (rgba[i * 4 + 3] < 128) px.transparent = true;
      frame[i] = px;
    }
    frames.push_back(std::move(frame));
    times.push_back(addMillis(base, timestampMs));
  }
  WebPAnimDecoderDelete(dec);

  format = "WebP (animated, " + std::to_string(frames.size()) + " frames)";
  return true;
}
}  // namespace

ImageSource::ImageSource(const std::string& filename) : itsFilename(filename)
{
  // Animated WebP: every frame is a separate timestep. We decode all
  // frames eagerly because libwebpdemux is sequential — random access
  // by time index would otherwise be O(N) per click. For the radar
  // scales we target (at most a few hundred frames at a few MB each)
  // memory cost is acceptable.
  if (tryDecodeAnimatedWebP(filename, itsFrames, itsFrameTimes, itsNx, itsNy, itsFormat))
  {
    itsValidTime = itsFrameTimes.empty() ? mtimeUtc(filename) : itsFrameTimes.front();
    return;
  }

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
  // Static image: build a single frame into a local buffer, push it
  // onto itsFrames at the end. Mirrors the animated-WebP path's
  // structure so pixelAtUV / timeCount can stay agnostic.
  std::vector<Rgb> pixels(pixelCount);

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
      pixels[i] = lut[idx[i]];
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
      pixels[i] = px;
    }
  }

  itsValidTime = parseTimeFromName(filename);
  itsFrames.push_back(std::move(pixels));
  itsFrameTimes.push_back(itsValidTime);
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

std::size_t ImageSource::timeCount() const { return itsFrames.size(); }
std::size_t ImageSource::currentTimeIndex() const { return itsCurrentFrame; }
void ImageSource::selectTimeIndex(std::size_t i)
{
  if (i < itsFrames.size()) itsCurrentFrame = i;
}
NFmiMetTime ImageSource::currentValidTime() const
{
  return itsCurrentFrame < itsFrameTimes.size() ? itsFrameTimes[itsCurrentFrame] : itsValidTime;
}
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
  if (itsFrames.empty() || itsCurrentFrame >= itsFrames.size() ||
      itsNx == 0 || itsNy == 0)
    return Rgb{0, 0, 0, true};
  if (u < 0 || u >= 1 || v < 0 || v >= 1)
    return Rgb{0, 0, 0, true};

  const auto& frame = itsFrames[itsCurrentFrame];

  // Sample at the pixel-CENTRE convention so u/v=0 hits the centre
  // of pixel 0 and u/v=1 hits the centre of pixel N-1, with edge
  // bands handled by clamping. Bilinear when zoomed out (one screen
  // cell covers many source pixels: averaging avoids moiré) AND
  // when zoomed in heavily (one source pixel covers many screen
  // cells: avoids the blocky aliased look the previous nearest
  // sampler produced).
  const double fx = u * static_cast<double>(itsNx) - 0.5;
  const double fy = v * static_cast<double>(itsNy) - 0.5;
  const long x0 = static_cast<long>(std::floor(fx));
  const long y0 = static_cast<long>(std::floor(fy));
  const double dx = fx - static_cast<double>(x0);
  const double dy = fy - static_cast<double>(y0);

  auto fetch = [&](long x, long y) -> Rgb {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= static_cast<long>(itsNx)) x = static_cast<long>(itsNx) - 1;
    if (y >= static_cast<long>(itsNy)) y = static_cast<long>(itsNy) - 1;
    return frame[static_cast<std::size_t>(y) * itsNx +
                 static_cast<std::size_t>(x)];
  };
  const Rgb p00 = fetch(x0, y0);
  const Rgb p10 = fetch(x0 + 1, y0);
  const Rgb p01 = fetch(x0, y0 + 1);
  const Rgb p11 = fetch(x0 + 1, y0 + 1);

  // Transparency is binary: any of the four neighbours opaque means
  // the result is opaque (with the opaque ones' colours blended).
  // If all four are transparent, the output is transparent — keeps
  // anti-aliased PNG edges from leaking colour past their alpha.
  const int opaqueCount = (p00.transparent ? 0 : 1) + (p10.transparent ? 0 : 1) +
                          (p01.transparent ? 0 : 1) + (p11.transparent ? 0 : 1);
  if (opaqueCount == 0) return Rgb{0, 0, 0, true};

  // Re-weight so transparent neighbours don't darken the bilinear
  // average. A neighbour with `transparent=true` contributes 0 to
  // the weight sum AND 0 to the colour sum.
  const double w00 = (p00.transparent ? 0.0 : (1 - dx) * (1 - dy));
  const double w10 = (p10.transparent ? 0.0 : dx * (1 - dy));
  const double w01 = (p01.transparent ? 0.0 : (1 - dx) * dy);
  const double w11 = (p11.transparent ? 0.0 : dx * dy);
  const double wsum = w00 + w10 + w01 + w11;
  if (wsum == 0.0) return Rgb{0, 0, 0, true};
  const double inv = 1.0 / wsum;
  auto chan = [&](std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d) {
    const double v = (w00 * a + w10 * b + w01 * c + w11 * d) * inv;
    if (v <= 0) return std::uint8_t{0};
    if (v >= 255) return std::uint8_t{255};
    return static_cast<std::uint8_t>(v + 0.5);
  };
  Rgb out;
  out.r = chan(p00.r, p10.r, p01.r, p11.r);
  out.g = chan(p00.g, p10.g, p01.g, p11.g);
  out.b = chan(p00.b, p10.b, p01.b, p11.b);
  return out;
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
