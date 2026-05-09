%define BINNAME qdless
%define RPMNAME smartmet-%{BINNAME}
Summary: Interactive UTF-8 terminal viewer for SmartMet querydata
Name: %{RPMNAME}
Version: 26.5.9
Release: 3%{?dist}.fmi
License: MIT
Group: Development/Tools
URL: https://github.com/fmidev/smartmet-qdless
Source0: %{name}.tar.gz
BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-buildroot-%(%{__id_u} -n)

# https://fedoraproject.org/wiki/Changes/Broken_RPATH_will_fail_rpmbuild
%global __brp_check_rpaths %{nil}

%if 0%{?rhel} && 0%{rhel} < 9
%define smartmet_boost boost169
%else
%define smartmet_boost boost
%endif

%define smartmet_fmt_min 12.0.0
%define smartmet_fmt_max 13.0.0
%define smartmet_fmt fmt-libs >= %{smartmet_fmt_min}, fmt-libs < %{smartmet_fmt_max}
%define smartmet_fmt_devel fmt-devel >= %{smartmet_fmt_min}, fmt-devel < %{smartmet_fmt_max}

BuildRequires: %{smartmet_boost}-devel
BuildRequires: bzip2-devel
BuildRequires: %{smartmet_fmt_devel}
BuildRequires: gcc-c++
BuildRequires: gdal312-devel
BuildRequires: hdf5-devel >= 1.8.12
BuildRequires: jsoncpp-devel
BuildRequires: libjpeg-devel
BuildRequires: libpng-devel
BuildRequires: make
BuildRequires: ncurses-devel
BuildRequires: netcdf-cxx4-devel
BuildRequires: netcdf-devel >= 4.3.3.1
BuildRequires: rpm-build
BuildRequires: smartmet-library-calculator-devel >= 26.4.13
BuildRequires: smartmet-library-gis-devel >= 26.4.13
BuildRequires: smartmet-library-grid-files-devel >= 26.4.22
BuildRequires: smartmet-library-imagine-devel >= 26.4.13
BuildRequires: smartmet-library-macgyver-devel >= 26.4.13
BuildRequires: smartmet-library-newbase-devel >= 26.2.4
BuildRequires: smartmet-library-smarttools-devel >= 26.4.13
BuildRequires: smartmet-timezones
BuildRequires: zlib-devel

Requires: %{smartmet_boost}-filesystem
Requires: %{smartmet_boost}-iostreams
Requires: %{smartmet_boost}-program-options
Requires: %{smartmet_boost}-regex
Requires: %{smartmet_boost}-system
Requires: %{smartmet_boost}-thread
Requires: bzip2-libs
Requires: %{smartmet_fmt}
Requires: gdal312-libs
Requires: glibc
Requires: gshhg-gmt-nc4
Requires: hdf5 >= 1.8.12
Requires: jsoncpp
Requires: libgcc
Requires: libjpeg
Requires: libpng
Requires: libstdc++
Requires: ncurses-libs
Requires: netcdf >= 4.3.3.1
Requires: netcdf-cxx4
Requires: smartmet-library-calculator >= 26.4.13
Requires: smartmet-library-gis >= 26.4.13
Requires: smartmet-library-grid-files >= 26.4.22
Requires: smartmet-library-imagine >= 26.4.13
Requires: smartmet-library-macgyver >= 26.4.13
Requires: smartmet-library-newbase >= 26.2.4
Requires: smartmet-library-smarttools >= 26.4.13
Requires: smartmet-timezones >= 24.5.27
Requires: zlib

Provides: qdless = %{version}

%description
qdless is an interactive UTF-8 terminal viewer for SmartMet querydata
(.sqd) files. It renders gridded weather data as quadrant-block raster in
the terminal, with coastline / political-border overlay (gshhg-gmt-nc4),
palette-driven colouring (FMI wms-conf colour ramps baked at build time),
animation, mouse-driven panning and zoom, click-to-probe time-series with
a braille-sparkline graph, place search via GeoNames, lat/lon graticule,
wind arrows for u/v components, PNG export, and cross-section views
across pressure / height levels.

%prep
%setup -q -n %{RPMNAME}

%build
make %{_smp_mflags}

%install
%makeinstall

%clean

%files
%defattr(0775,root,root,0775)
%{_bindir}/qdless
%defattr(0664,root,root,0775)
%{_datadir}/smartmet/qdless/qdless.conf
%{_datadir}/smartmet/qdless/palettes/*.json
%{_datadir}/smartmet/qdless/cities1000.tsv

%changelog
* Sat May  9 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.9-3.fmi
- -p accepts a comma-separated list of parameters and picks the layout
  from the count: 1 -> single, 2 -> side-by-side, 3 or 4 -> 2x2; >4 is an
  error. With three parameters the 4th panel clones the first. New
  --layout single|side|quad option overrides the count-derived layout
  (must hold at least as many panels as parameters).

* Sat May  9 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.9-2.fmi
- Multi-panel layouts. F2 cycles single → side-by-side → 2x2; each panel
  has its own parameter, level, and palette while sharing the viewport,
  time, marker, and overlay toggles. Tab / Shift+Tab / digit keys 1–4 /
  mouse click switch the active panel; parameter / level / legend / probe
  / cross-section / PNG export operate on it. Per-panel labels at top-left
  of each panel show "[N] paramName" with the active panel highlighted.

* Sat May  9 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.9-1.fmi
- Timeseries probe: a click outside the chart on the map area now picks a
  new probe location instead of closing the popup; the popup loops at the
  new lat/lon. Keyboard still closes. Off-map clicks are ignored. Also
  handles BUTTON1_CLICKED so clicks register on terminals that deliver an
  atomic click event.

* Fri May  8 2026 Mika Heiskanen <mika.heiskanen@fmi.fi> - 26.5.8-1.fmi
- Initial release. Split out from smartmet-qdtools so the project can
  depend on smartmet-library-grid-files for GRIB / NetCDF input without
  pulling that into qdtools.
