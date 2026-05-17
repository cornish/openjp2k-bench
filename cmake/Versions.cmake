# Resolve library version strings and commit SHAs at configure time, then
# render src/build_info.h from src/build_info.h.in. Re-run when configure
# is re-run; not a build-time dependency.

function(_jp2kbench_git_describe out_var path)
  set(_sha "unknown")
  if(EXISTS "${path}/.git")
    execute_process(
      COMMAND git -C "${path}" rev-parse --short=12 HEAD
      OUTPUT_VARIABLE _sha
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
      set(_sha "unknown")
    endif()
  endif()
  set(${out_var} "${_sha}" PARENT_SCOPE)
endfunction()

# OpenJPEG: version comes from its CMake (OPENJPEG_VERSION) once added.
_jp2kbench_git_describe(JP2KBENCH_OPENJPEG_COMMIT_STR "${JP2KBENCH_OPENJPEG_SOURCE}")
if(DEFINED OPENJPEG_VERSION)
  set(JP2KBENCH_OPENJPEG_VERSION_STR "${OPENJPEG_VERSION}")
else()
  set(JP2KBENCH_OPENJPEG_VERSION_STR "unknown")
endif()

# Grok
if(JP2KBENCH_HAVE_GROK)
  _jp2kbench_git_describe(JP2KBENCH_GROK_COMMIT_STR "${JP2KBENCH_GROK_SOURCE}")
  if(DEFINED GRK_VERSION)
    set(JP2KBENCH_GROK_VERSION_STR "${GRK_VERSION}")
  else()
    set(JP2KBENCH_GROK_VERSION_STR "unknown")
  endif()
else()
  set(JP2KBENCH_GROK_COMMIT_STR  "n/a")
  set(JP2KBENCH_GROK_VERSION_STR "n/a")
endif()

# Compile flags string. Best-effort: the actual per-target flags are applied
# inside jp2kbench_apply_flags(), so reconstruct the same string here.
if(MSVC)
  set(JP2KBENCH_COMPILE_FLAGS_STR "/O2 /DNDEBUG /Oi /Ot /GL /LTCG")
else()
  set(JP2KBENCH_COMPILE_FLAGS_STR
      "-O3 -DNDEBUG -fno-plt -fomit-frame-pointer -funroll-loops -ffp-contract=fast")
  if(JP2KBENCH_NATIVE)
    set(JP2KBENCH_COMPILE_FLAGS_STR
        "${JP2KBENCH_COMPILE_FLAGS_STR} -march=native -mtune=native")
  endif()
endif()

configure_file(
  ${CMAKE_SOURCE_DIR}/src/build_info.h.in
  ${CMAKE_BINARY_DIR}/generated/build_info.h
  @ONLY)
