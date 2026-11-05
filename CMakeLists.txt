cmake_minimum_required(VERSION 3.25)

# The version lives in one place, core/include/xcat/version.h, so the nxdk
# Makefile build (which doesn't use CMake) sees the same number.
file(STRINGS core/include/xcat/version.h _xc_version_lines
  REGEX "^#define XC_VERSION_(MAJOR|MINOR|PATCH) +[0-9]+$")
foreach(_line IN LISTS _xc_version_lines)
  string(REGEX MATCH "XC_VERSION_([A-Z]+) +([0-9]+)" _ "${_line}")
  set(_xc_version_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
endforeach()

project(xiso_cartographer
  VERSION ${_xc_version_MAJOR}.${_xc_version_MINOR}.${_xc_version_PATCH}
  LANGUAGES C)

option(XCAT_BUILD_TESTS "Build the host unit tests" ON)
option(XCAT_WERROR "Treat compiler warnings as errors" ON)
option(XCAT_SANITIZE "Build with AddressSanitizer (+ UBSan where supported)" OFF)
option(XCAT_FUZZ "Also build libFuzzer targets (clang on Linux/macOS only)" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/XcatCompilerOptions.cmake)
xcat_enable_sanitizers()
xcat_enable_fuzzing()

add_subdirectory(core)
add_subdirectory(platform/host)
add_subdirectory(platform/smb)
add_subdirectory(tools/xcat-cli)

if(XCAT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(core/tests)
endif()
# After core/tests, whose test helpers (xcat_test_target, xcat_test_env) it uses.
add_subdirectory(app/ui)
