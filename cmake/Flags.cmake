# Centralized optimization flags. Applied to the bench executable only; we
# rely on the bundled libraries' own CMake to honor CMAKE_BUILD_TYPE=Release
# and LTO via CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE.
#
# Goal: identical, aggressive flags for everything we measure. If you change
# anything here, rebuild from scratch.

function(jp2kbench_apply_flags target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /O2 /DNDEBUG /Oi /Ot /GL)
    target_link_options(${target} PRIVATE /LTCG)
  else()
    target_compile_options(${target} PRIVATE
      -O3
      -DNDEBUG
      -fno-plt
      -fomit-frame-pointer
      -funroll-loops
      -ffp-contract=fast
    )
    if(JP2KBENCH_NATIVE)
      target_compile_options(${target} PRIVATE -march=native -mtune=native)
    endif()
  endif()
endfunction()

# Apply -O3 / -march=native to bundled decoder libraries too. We don't
# control their CMakeLists, but appending to CMAKE_C_FLAGS_RELEASE applies
# globally before add_subdirectory.
if(NOT MSVC)
  set(_extra "-O3 -DNDEBUG -fno-plt -fomit-frame-pointer -funroll-loops -ffp-contract=fast")
  if(JP2KBENCH_NATIVE)
    set(_extra "${_extra} -march=native -mtune=native")
  endif()
  set(CMAKE_C_FLAGS_RELEASE   "${CMAKE_C_FLAGS_RELEASE} ${_extra}" CACHE STRING "" FORCE)
  set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} ${_extra}" CACHE STRING "" FORCE)
endif()
