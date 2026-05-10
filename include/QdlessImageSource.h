#pragma once

#include "QdlessDataSource.h"
#include "QdlessPalette.h"

#include <string>
#include <vector>

namespace Qdless
{
// DataSource for raw images with no spatial georeference (PNG/WebP/
// JPEG/GIF/BMP). Used for viewing pre-rendered radar / satellite
// products over slow links where X11 is impractical. The image fills
// the viewport (u, v) ∈ [0, 1]² directly; pan and zoom work in image
// coordinates. Coastlines, borders, graticule, cities, probe popup,
// place search, and cross-section are all suppressed by App when
// `isRawImage()` is true — RGB triplets have no scalar interpretation.
//
// Time is parsed from a leading YYYYMMDDhhmm in the filename basename
// (UTC by convention; meteorological end-of-period); falls back to
// filesystem mtime. One image == one timestep; multi-file animation
// works through MultiFileSource as for any other backend.
class ImageSource : public DataSource
{
 public:
  explicit ImageSource(const std::string& filename);
  ~ImageSource() override;
  ImageSource(const ImageSource&) = delete;
  ImageSource& operator=(const ImageSource&) = delete;
  ImageSource(ImageSource&&) = delete;
  ImageSource& operator=(ImageSource&&) = delete;

  std::vector<int> paramIds() const override;
  std::string paramShortName(int paramId) const override;
  std::string paramLongName(int paramId) const override;
  std::string paramUnits(int paramId) const override;
  int currentParamId() const override;
  bool selectParamId(int paramId) override;

  std::size_t timeCount() const override;
  std::size_t currentTimeIndex() const override;
  void selectTimeIndex(std::size_t i) override;
  NFmiMetTime currentValidTime() const override;
  NFmiMetTime originTime() const override;

  std::size_t levelCount() const override;
  std::size_t currentLevelIndex() const override;
  void selectLevelIndex(std::size_t i) override;
  float levelValueAt(std::size_t i) const override;

  // Always returns NaN — image pixels carry no scalar value. The
  // renderer goes through pixelAtUV() in image mode instead.
  float interpolatedValue(double lat, double lon) const override;
  LatLonBox boundingBox() const override;
  // Identity mapping: u=lon, v=1-lat. Lets the existing viewport math
  // (which still calls these on every redraw) produce stable values
  // without throwing. The "lat/lon" displayed in the status line is
  // synthetic but at least monotonic and tied to the cursor position.
  void uvToLatLon(double u, double v, double& lat, double& lon) const override;
  void latLonToUV(double lat, double lon, double& u, double& v) const override;

  std::vector<std::pair<std::string, std::string>> extraMetadata() const override;
  std::string gridSignature() const override;

  bool isRawImage() const override { return true; }
  Rgb pixelAtUV(double u, double v) const override;

 private:
  std::string itsFilename;
  std::vector<Rgb> itsPixels;  // row-major, top-to-bottom, length = nx*ny
  std::size_t itsNx = 0;
  std::size_t itsNy = 0;
  std::string itsFormat;       // PNG / WebP / JPEG / GIF / BMP — for the popup
  NFmiMetTime itsValidTime;
};
}  // namespace Qdless
