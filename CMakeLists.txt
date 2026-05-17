cmake_minimum_required(VERSION 3.20)
project(jp2k-bench LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(JP2KBENCH_LTO          "Enable IPO/LTO for the whole build"          ON)
option(JP2KBENCH_NATIVE       "Add -march=native"                            ON)
option(JP2KBENCH_BUILD_GROK   "Build Grok adapter if grok source is present" ON)

# Where to find decoder sources. Both default to third_party/ populated by
# scripts/setup.sh, but either can be pointed at a fork.
set(JP2KBENCH_OPENJPEG_SOURCE "${CMAKE_SOURCE_DIR}/third_party/openjpeg"
    CACHE PATH "Path to OpenJPEG source tree")
set(JP2KBENCH_GROK_SOURCE     "${CMAKE_SOURCE_DIR}/third_party/grok"
    CACHE PATH "Path to Grok source tree")

include(cmake/Flags.cmake)

if(JP2KBENCH_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_msg)
  if(_ipo_ok)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    message(STATUS "LTO/IPO enabled")
  else()
    message(WARNING "LTO requested but not supported: ${_ipo_msg}")
  endif()
endif()

# --- OpenJPEG ---------------------------------------------------------------
if(NOT EXISTS "${JP2KBENCH_OPENJPEG_SOURCE}/CMakeLists.txt")
  message(FATAL_ERROR
    "OpenJPEG source not found at ${JP2KBENCH_OPENJPEG_SOURCE}. "
    "Run scripts/setup.sh or pass -DJP2KBENCH_OPENJPEG_SOURCE=/path/to/fork.")
endif()

set(BUILD_SHARED_LIBS    OFF CACHE BOOL "" FORCE)
set(BUILD_THIRDPARTY     OFF CACHE BOOL "" FORCE)
set(BUILD_CODEC          OFF CACHE BOOL "" FORCE)
set(BUILD_DOC            OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING        OFF CACHE BOOL "" FORCE)
set(BUILD_PKGCONFIG_FILES OFF CACHE BOOL "" FORCE)
add_subdirectory(${JP2KBENCH_OPENJPEG_SOURCE} ${CMAKE_BINARY_DIR}/_openjpeg EXCLUDE_FROM_ALL)

# --- Grok (optional) --------------------------------------------------------
set(JP2KBENCH_HAVE_GROK OFF)
if(JP2KBENCH_BUILD_GROK AND EXISTS "${JP2KBENCH_GROK_SOURCE}/CMakeLists.txt")
  set(GRK_BUILD_CODEC       OFF CACHE BOOL "" FORCE)
  set(GRK_BUILD_LIBPNG      OFF CACHE BOOL "" FORCE)
  set(GRK_BUILD_LIBTIFF     OFF CACHE BOOL "" FORCE)
  set(GRK_BUILD_LCMS2       OFF CACHE BOOL "" FORCE)
  set(GRK_BUILD_DOC         OFF CACHE BOOL "" FORCE)
  set(GRK_BUILD_TESTING     OFF CACHE BOOL "" FORCE)
  set(GRK_BUILD_PKGCONFIG_FILES OFF CACHE BOOL "" FORCE)
  add_subdirectory(${JP2KBENCH_GROK_SOURCE} ${CMAKE_BINARY_DIR}/_grok EXCLUDE_FROM_ALL)
  set(JP2KBENCH_HAVE_GROK ON)
  message(STATUS "Grok adapter: ENABLED (${JP2KBENCH_GROK_SOURCE})")
else()
  message(STATUS "Grok adapter: disabled (set JP2KBENCH_GROK_SOURCE to enable)")
endif()

include(cmake/Versions.cmake)

# --- Bench executable -------------------------------------------------------
add_executable(jp2k-bench
  src/main.cpp
  src/bench.cpp
  src/json_out.cpp
  src/env_capture.cpp
  src/rss.cpp
  src/verify.cpp
  src/adapter_openjpeg.cpp
)
target_include_directories(jp2k-bench PRIVATE
  src
  ${CMAKE_BINARY_DIR}/generated)
jp2kbench_apply_flags(jp2k-bench)

# openp2 is the library name openjpeg exposes (openjp2 in newer versions)
target_link_libraries(jp2k-bench PRIVATE openjp2)
target_include_directories(jp2k-bench PRIVATE
  ${JP2KBENCH_OPENJPEG_SOURCE}/src/lib/openjp2
  ${CMAKE_BINARY_DIR}/_openjpeg/src/lib/openjp2)

if(JP2KBENCH_HAVE_GROK)
  target_sources(jp2k-bench PRIVATE src/adapter_grok.cpp)
  target_compile_definitions(jp2k-bench PRIVATE JP2KBENCH_HAVE_GROK=1)
  # In Grok v20 the public library target is `grokj2k` and its
  # PUBLIC include directory (src/lib/core, which contains grok.h)
  # propagates automatically via target_include_directories.  The old
  # `grokj2k_static` target / `src/lib/jp2` header path no longer exist.
  target_link_libraries(jp2k-bench PRIVATE grokj2k)
  # grok.h itself lives in src/lib/core, but it #includes "grk_config.h"
  # which is a generated header placed in the build tree.
  target_include_directories(jp2k-bench PRIVATE
    ${JP2KBENCH_GROK_SOURCE}/src/lib/core
    ${CMAKE_BINARY_DIR}/_grok/src/lib/core)
endif()

# Thread support
find_package(Threads REQUIRED)
target_link_libraries(jp2k-bench PRIVATE Threads::Threads)
