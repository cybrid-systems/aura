# AuraCoverage.cmake — Issue #1933 LLVM source-based code coverage
#
# Enable with:
#   cmake -B build_coverage -DAURA_ENABLE_COVERAGE=ON \
#         -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang
# or CMake preset "coverage".
#
# Flags: Clang source-based coverage (-fprofile-instr-generate -fcoverage-mapping).
# Profiles are written via LLVM_PROFILE_FILE at runtime; merge with llvm-profdata
# and report with llvm-cov (see ./build.py coverage --html).

option(AURA_ENABLE_COVERAGE
    "Instrument with LLVM source-based coverage (#1933)" OFF)

if(NOT AURA_ENABLE_COVERAGE)
    return()
endif()

# Prefer Clang for source-based coverage (gcov-style works with GCC but
# the AC targets llvm-cov HTML/JSON).
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(WARNING
        "AURA_ENABLE_COVERAGE=ON but CMAKE_CXX_COMPILER_ID=${CMAKE_CXX_COMPILER_ID}. "
        "Source-based coverage works best with Clang; continuing with "
        "-fprofile-instr-generate/-fcoverage-mapping if the compiler accepts them.")
endif()

set(_AURA_COV_FLAGS
    "-fprofile-instr-generate"
    "-fcoverage-mapping"
    "-fno-omit-frame-pointer"
    "-g")
string(REPLACE ";" " " _AURA_COV_FLAGS_STR "${_AURA_COV_FLAGS}")

# Append rather than overwrite so callers can still pass -O1 etc.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${_AURA_COV_FLAGS_STR}" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${_AURA_COV_FLAGS_STR}" CACHE STRING "" FORCE)
set(CMAKE_EXE_LINKER_FLAGS
    "${CMAKE_EXE_LINKER_FLAGS} -fprofile-instr-generate" CACHE STRING "" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS
    "${CMAKE_SHARED_LINKER_FLAGS} -fprofile-instr-generate" CACHE STRING "" FORCE)
set(CMAKE_MODULE_LINKER_FLAGS
    "${CMAKE_MODULE_LINKER_FLAGS} -fprofile-instr-generate" CACHE STRING "" FORCE)

# Export for build.py / scripts
set(AURA_COVERAGE_ENABLED ON CACHE BOOL "Coverage instrumentation active" FORCE)
message(STATUS "AURA coverage (#1933): ${_AURA_COV_FLAGS_STR}")
