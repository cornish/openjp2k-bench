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

# Where to find decoder sources. All three default to third_party/ populated
# by scripts/setup.sh; any can be pointed at a local fork.
set(JP2KBENCH_OPENJPEG_SOURCE "${CMAKE_SOURCE_DIR}/third_party/openjpeg"
    CACHE PATH "Path to vanilla OpenJPEG source tree (baseline)")
set(JP2KBENCH_OPENJP2K_SOURCE "${CMAKE_SOURCE_DIR}/third_party/openjp2k"
    CACHE PATH "Path to openjp2k fork source tree (decoder under test)")
set(JP2KBENCH_GROK_SOURCE     "${CMAKE_SOURCE_DIR}/third_party/grok"
    CACHE PATH "Path to Grok source tree")

include(cmake/Flags.cmake)
include(ExternalProject)

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

# --- OpenJPEG / openjp2k ----------------------------------------------------
# Both libraries expose the same opj_* C ABI. To bench them side-by-side in
# one process we build each as a separately-installed shared library and
# dlopen them at runtime with RTLD_LOCAL | RTLD_DEEPBIND so their symbol sets
# stay isolated (see src/opj_dyn.{h,cpp}). Neither library is linked into
# jp2k-bench directly — only the build dependency is preserved.

if(NOT EXISTS "${JP2KBENCH_OPENJPEG_SOURCE}/CMakeLists.txt")
  message(FATAL_ERROR
    "OpenJPEG source not found at ${JP2KBENCH_OPENJPEG_SOURCE}. Run scripts/setup.sh.")
endif()
if(NOT EXISTS "${JP2KBENCH_OPENJP2K_SOURCE}/CMakeLists.txt")
  message(FATAL_ERROR
    "openjp2k source not found at ${JP2KBENCH_OPENJP2K_SOURCE}. Run scripts/setup.sh.")
endif()

set(_opj_install   "${CMAKE_BINARY_DIR}/_openjpeg_install")
set(_oj2k_install  "${CMAKE_BINARY_DIR}/_openjp2k_install")
set(_opj_soname    "libopenjp2.so.7")
set(_opj_so        "${_opj_install}/lib/${_opj_soname}")
set(_oj2k_so       "${_oj2k_install}/lib/${_opj_soname}")

# Common CMake args for both subbuilds. BUILD_SHARED_LIBS=ON so each produces
# its own .so; everything else off (no codec exe, no thirdparty/png/tiff, no
# tests/docs/pkgconfig).
set(_opj_common_args
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
    -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
    -DCMAKE_INSTALL_LIBDIR=lib
    -DBUILD_SHARED_LIBS=ON
    -DBUILD_STATIC_LIBS=OFF
    -DBUILD_CODEC=OFF
    -DBUILD_THIRDPARTY=OFF
    -DBUILD_DOC=OFF
    -DBUILD_TESTING=OFF
    -DBUILD_PKGCONFIG_FILES=OFF)

ExternalProject_Add(openjpeg_ext
  SOURCE_DIR        "${JP2KBENCH_OPENJPEG_SOURCE}"
  BINARY_DIR        "${CMAKE_BINARY_DIR}/_openjpeg_build"
  INSTALL_DIR       "${_opj_install}"
  CMAKE_ARGS        ${_opj_common_args} -DCMAKE_INSTALL_PREFIX=${_opj_install}
  BUILD_BYPRODUCTS  "${_opj_so}"
  USES_TERMINAL_BUILD TRUE
  UPDATE_DISCONNECTED TRUE
  DOWNLOAD_COMMAND  ""
)

ExternalProject_Add(openjp2k_ext
  SOURCE_DIR        "${JP2KBENCH_OPENJP2K_SOURCE}"
  BINARY_DIR        "${CMAKE_BINARY_DIR}/_openjp2k_build"
  INSTALL_DIR       "${_oj2k_install}"
  CMAKE_ARGS        ${_opj_common_args} -DCMAKE_INSTALL_PREFIX=${_oj2k_install}
  BUILD_BYPRODUCTS  "${_oj2k_so}"
  USES_TERMINAL_BUILD TRUE
  UPDATE_DISCONNECTED TRUE
  DOWNLOAD_COMMAND  ""
)

# Stub opj_config.h so adapter sources can include <openjpeg.h> from the
# in-tree header location without configuring the upstream project. Only
# version macros live there; openjpeg.h itself never reads them.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
file(WRITE "${CMAKE_BINARY_DIR}/generated/opj_config.h"
  "#ifndef OPJ_CONFIG_H_INCLUDED\n"
  "#define OPJ_CONFIG_H_INCLUDED\n"
  "#define OPJ_VERSION_MAJOR 2\n"
  "#define OPJ_VERSION_MINOR 5\n"
  "#define OPJ_VERSION_BUILD 3\n"
  "#endif\n")

# --- Grok (optional, still statically linked) ------------------------------
# Grok lives in a different symbol namespace (grk_*), so no clash; keep it
# statically linked in.
set(JP2KBENCH_HAVE_GROK OFF)
if(JP2KBENCH_BUILD_GROK AND EXISTS "${JP2KBENCH_GROK_SOURCE}/CMakeLists.txt")
  set(BUILD_SHARED_LIBS    OFF CACHE BOOL "" FORCE)
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
  src/adapter_opj.cpp
  src/opj_dyn.cpp
)
add_dependencies(jp2k-bench openjpeg_ext openjp2k_ext)
# Override grok's RUNTIME_OUTPUT_DIRECTORY hijack so the binary lands in
# ${BUILD_DIR}/jp2k-bench as scripts and CI expect.
set_target_properties(jp2k-bench PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

target_include_directories(jp2k-bench PRIVATE
  src
  ${CMAKE_BINARY_DIR}/generated
  ${JP2KBENCH_OPENJPEG_SOURCE}/src/lib/openjp2)
jp2kbench_apply_flags(jp2k-bench)

# Absolute paths of the two .so files baked in as compile-time strings.
target_compile_definitions(jp2k-bench PRIVATE
  "JP2KBENCH_OPENJPEG_SO=\"${_opj_so}\""
  "JP2KBENCH_OPENJP2K_SO=\"${_oj2k_so}\"")

target_link_libraries(jp2k-bench PRIVATE ${CMAKE_DL_LIBS})

if(JP2KBENCH_HAVE_GROK)
  target_sources(jp2k-bench PRIVATE src/adapter_grok.cpp)
  target_compile_definitions(jp2k-bench PRIVATE JP2KBENCH_HAVE_GROK=1)
  target_link_libraries(jp2k-bench PRIVATE grokj2k)
  target_include_directories(jp2k-bench PRIVATE
    ${JP2KBENCH_GROK_SOURCE}/src/lib/core
    ${CMAKE_BINARY_DIR}/_grok/src/lib/core)
endif()

find_package(Threads REQUIRED)
target_link_libraries(jp2k-bench PRIVATE Threads::Threads)
