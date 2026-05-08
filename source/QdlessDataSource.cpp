#include "QdlessDataSource.h"

#include "QdlessGridFilesSource.h"
#include "QdlessQueryDataSource.h"

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
  if (n >= 4 && hdr[0] == 0x89 && hdr[1] == 'H' && hdr[2] == 'D' && hdr[3] == 'F')
    return FileKind::kNetCDF;
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
    case FileKind::kUnknown:
      break;
  }
  throw std::runtime_error("unknown file format: " + filename);
}
}  // namespace Qdless
