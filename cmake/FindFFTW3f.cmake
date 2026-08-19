# Locate the single-precision FFTW library and expose FFTW3f::fftw3f.
# Supports pkg-config, Homebrew prefixes, and conventional Unix prefixes.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(PC_FFTW3F QUIET fftw3f)
endif()

set(_FFTW3F_HINTS)
if(DEFINED ENV{HOMEBREW_PREFIX})
  list(APPEND _FFTW3F_HINTS "$ENV{HOMEBREW_PREFIX}")
endif()
list(APPEND _FFTW3F_HINTS /opt/homebrew /usr/local)

find_path(FFTW3f_INCLUDE_DIR
  NAMES fftw3.h
  HINTS ${PC_FFTW3F_INCLUDE_DIRS} ${_FFTW3F_HINTS}
  PATH_SUFFIXES include
)
find_library(FFTW3f_LIBRARY
  NAMES fftw3f
  HINTS ${PC_FFTW3F_LIBRARY_DIRS} ${_FFTW3F_HINTS}
  PATH_SUFFIXES lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FFTW3f
  REQUIRED_VARS FFTW3f_LIBRARY FFTW3f_INCLUDE_DIR
)

if(FFTW3f_FOUND AND NOT TARGET FFTW3f::fftw3f)
  add_library(FFTW3f::fftw3f UNKNOWN IMPORTED)
  set_target_properties(FFTW3f::fftw3f PROPERTIES
    IMPORTED_LOCATION "${FFTW3f_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${FFTW3f_INCLUDE_DIR}"
  )
endif()

mark_as_advanced(FFTW3f_INCLUDE_DIR FFTW3f_LIBRARY)
