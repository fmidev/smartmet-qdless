// qdless: an interactive UTF-8 viewer for SmartMet querydata files.
//
// Step 2 skeleton: opens a .sqd file, samples the first parameter / first
// time / first level into a quadrant-block raster and dumps one frame to
// stdout. Future steps add the ncurses event loop, panels, animation,
// coastline overlay and mouse-driven probe/timeseries.

#include "QdlessApp.h"

#include <boost/program_options.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

namespace po = boost::program_options;

int main(int argc, char* argv[])
{
  try
  {
    Qdless::Options opts;
    std::string paramArg;
    std::string dirArg;
    std::vector<std::string> positional;
    po::options_description desc("qdless options");
    desc.add_options()                                                                 //
        ("help,h", "show this help message")                                           //
        ("param,p", po::value<std::string>(&paramArg),
         "parameter name; comma-separated for multi-panel (e.g. -p T,RH,P)")           //
        ("layout",
         po::value<std::string>(&opts.layoutOverride),
         "panel layout: single, side, or quad (default: derived from -p)")             //
        ("time,t",
         po::value<int>(&opts.timeIndex)->default_value(0),
         "time index (0-based; -1 = last)")                                            //
        ("level,l",
         po::value<int>(&opts.levelIndex)->default_value(0),
         "level index (0-based; -1 = last)")                                           //
        ("palette",
         po::value<std::string>(&opts.paletteOverride),
         "palette name (overrides config lookup)")                                     //
        ("palette-dir",
         po::value<std::string>(&opts.paletteDir)->default_value(opts.paletteDir),
         "directory of palette JSON files")                                            //
        ("config,c",
         po::value<std::string>(&opts.configFile)->default_value(opts.configFile),
         "qdless.conf path")                                                           //
        ("coastline-dir",
         po::value<std::string>(&opts.coastlineDir)->default_value(opts.coastlineDir),
         "directory of gshhg-gmt-nc4 binned NetCDF files")                             //
        ("no-coastline", po::bool_switch(&opts.noCoastline), "disable coastline overlay") //
        ("no-borders", po::bool_switch(&opts.noBorders), "disable political-border overlay") //
        ("min-lake-area",
         po::value<double>(&opts.minLakeAreaKm2)->default_value(opts.minLakeAreaKm2),
         "minimum lake area in km² to render (continents/islands always shown)") //
        ("min-lake-roundness",
         po::value<double>(&opts.minLakeRoundness)->default_value(opts.minLakeRoundness),
         "minimum compactness 4πA/L² for lakes (1=circle, drops fractal lakes)") //
        ("min-island-area",
         po::value<double>(&opts.minIslandAreaKm2)->default_value(opts.minIslandAreaKm2),
         "minimum island area in km² (continents always shown; 0 disables)") //
        ("dump", po::bool_switch(&opts.dumpAndExit), "render one frame to stdout and exit") //
        ("dir",
         po::value<std::string>(&dirArg),
         "directory whose files (sorted by filename) form the time series") //
        ("pg",
         po::value<std::string>(&opts.pgConn),
         "PostgreSQL/PostGIS connection string (libpq DSN), e.g. "
         "\"host=foo dbname=bar user=baz\"") //
        ("schema",
         po::value<std::string>(&opts.pgSchema),
         "PostGIS schema to filter the layer picker to") //
        ("table",
         po::value<std::string>(&opts.pgTable),
         "PostGIS table (schema.name) — skip the layer picker and open "
         "this table directly") //
        ("file", po::value<std::vector<std::string>>(&positional),
         "input file(s); pass multiple for a multi-time series");

    po::positional_options_description pos;
    pos.add("file", -1);

    po::variables_map vm;
    po::store(po::command_line_parser(argc, argv).options(desc).positional(pos).run(), vm);
    po::notify(vm);

    // Resolve inputs: positionals + --dir contents. Multiple files build a
    // MultiFileSource; one file goes through the single-file fast path.
    if (!dirArg.empty())
    {
      std::error_code ec;
      for (const auto& entry : std::filesystem::directory_iterator(dirArg, ec))
      {
        if (entry.is_regular_file()) positional.push_back(entry.path().string());
      }
      std::sort(positional.begin(), positional.end());
    }
    if (positional.size() == 1) opts.filename = positional.front();
    else if (positional.size() > 1) opts.filenames = std::move(positional);

    if (vm.count("help") != 0U ||
        (opts.filename.empty() && opts.filenames.empty() && opts.pgConn.empty()))
    {
      std::cout << "Usage: qdless [options] <file> [<file> ...]\n"
                << "       qdless [options] --dir <directory>\n"
                << "       qdless [options] --pg \"<dsn>\" "
                   "[--schema <name>] [--table schema.name]\n\n"
                << desc << '\n';
      return vm.count("help") != 0U ? 0 : 1;
    }

    // Split -p on commas. Trim whitespace; drop empty entries.
    if (!paramArg.empty())
    {
      std::size_t start = 0;
      while (start <= paramArg.size())
      {
        const std::size_t comma = paramArg.find(',', start);
        const std::size_t end = (comma == std::string::npos) ? paramArg.size() : comma;
        std::size_t lo = start;
        std::size_t hi = end;
        while (lo < hi && std::isspace(static_cast<unsigned char>(paramArg[lo]))) ++lo;
        while (hi > lo && std::isspace(static_cast<unsigned char>(paramArg[hi - 1]))) --hi;
        if (hi > lo) opts.parameterOverrides.emplace_back(paramArg.substr(lo, hi - lo));
        if (comma == std::string::npos) break;
        start = comma + 1;
      }
    }

    bool dump = opts.dumpAndExit;
    Qdless::App app(std::move(opts));
    return dump ? app.runOnce() : app.runInteractive();
  }
  catch (const std::exception& e)
  {
    std::cerr << "qdless: " << e.what() << '\n';
    return 1;
  }
}
