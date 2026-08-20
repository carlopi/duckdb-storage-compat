cmake_minimum_required(VERSION 3.16)

set(TARGET_NAME storage_compat)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})
set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(ExternalProject)
include_directories(src src/include)

# ---------------------------------------------------------------------------
# The GUEST: a second DuckDB - the one that understands the newer storage format.
#
# It is embedded in this extension and reached ONLY through its C API. Two build
# properties are load-bearing, and both fail SILENTLY if skipped:
#
#   1. hidden visibility, so its ~22k `duckdb::` C++ symbols never enter the export
#      table. DUCKDB_API is a no-op on unix while DUCKDB_C_API carries
#      visibility("default"), so this hides the C++ API and keeps the C API with no
#      source changes.
#   2. the archives are then collapsed into ONE relocatable object with every
#      non-C-API symbol made *local*. Hidden alone is not enough: hidden means
#      "private external", which still satisfies references from other objects in
#      the same link - so the guest would capture the HOST's duckdb:: calls and run
#      the newer engine's code over the host's objects.
#
# Built here rather than by a helper script, so the standard `make release` works.
# ---------------------------------------------------------------------------
set(STORAGE_COMPAT_GUEST_SRC "${CMAKE_CURRENT_SOURCE_DIR}/duckdb-guest"
    CACHE PATH "Source checkout of the guest DuckDB")

if(NOT EXISTS "${STORAGE_COMPAT_GUEST_SRC}/src/include/duckdb.h")
  message(FATAL_ERROR
    "guest DuckDB sources not found at ${STORAGE_COMPAT_GUEST_SRC}\n"
    "  git submodule update --init --recursive")
endif()

set(GUEST_BUILD ${CMAKE_CURRENT_BINARY_DIR}/guest-build)
set(GUEST_ARCHIVES
  ${GUEST_BUILD}/src/libduckdb_static.a
  ${GUEST_BUILD}/extension/libduckdb_generated_extension_loader.a
  ${GUEST_BUILD}/extension/parquet/libparquet_extension.a
  ${GUEST_BUILD}/extension/core_functions/libcore_functions_extension.a)
# NB: do NOT add third_party/*.a - libduckdb_static.a already bundles them, and
# pulling them in again yields duplicate duckdb_miniz:: symbols.

ExternalProject_Add(storage_compat_guest
  SOURCE_DIR ${STORAGE_COMPAT_GUEST_SRC}
  BINARY_DIR ${GUEST_BUILD}
  CMAKE_GENERATOR ${CMAKE_GENERATOR}
  CMAKE_ARGS
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_CXX_VISIBILITY_PRESET=hidden
    -DCMAKE_C_VISIBILITY_PRESET=hidden
    -DCMAKE_VISIBILITY_INLINES_HIDDEN=ON
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DBUILD_UNITTESTS=OFF
    -DBUILD_SHELL=OFF
    -DENABLE_EXTENSION_AUTOLOADING=0
    -DENABLE_EXTENSION_AUTOINSTALL=0
    -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
    -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
    -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}
    -DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}
  # duckdb_static alone is NOT self-contained: it leaves
  # duckdb::ExtensionHelper::RegisterLinkedExtensions undefined, which would then bind
  # to the HOST DuckDB. The generated loader and the in-tree extensions supply it.
  BUILD_COMMAND ${CMAKE_COMMAND} --build ${GUEST_BUILD} --target duckdb_static
                                 duckdb_generated_extension_loader
                                 parquet_extension core_functions_extension
  BUILD_BYPRODUCTS ${GUEST_ARCHIVES}
  INSTALL_COMMAND ""
  USES_TERMINAL_BUILD TRUE)

set(GUEST_BLOB ${CMAKE_CURRENT_BINARY_DIR}/guest_blob.o)

if(APPLE)
  if(CMAKE_OSX_ARCHITECTURES)
    set(SC_ARCH ${CMAKE_OSX_ARCHITECTURES})
  else()
    set(SC_ARCH ${CMAKE_HOST_SYSTEM_PROCESSOR})
  endif()
elseif(UNIX)
  set(SC_ARCH ${CMAKE_SYSTEM_PROCESSOR})
  # CI toolchains often live outside PATH (e.g. gcc-toolset), so look next to the
  # compiler as well as in the usual places. CMAKE_OBJCOPY is set by CMake when it
  # knows the toolchain's binutils.
  get_filename_component(SC_COMPILER_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
  find_program(SC_OBJCOPY
    NAMES ${CMAKE_OBJCOPY} objcopy llvm-objcopy gobjcopy
    HINTS "${SC_COMPILER_DIR}" "${SC_COMPILER_DIR}/../bin")
  if(NOT SC_OBJCOPY)
    message(FATAL_ERROR
      "storage_compat: objcopy not found (looked in PATH and beside ${CMAKE_CXX_COMPILER}).\n"
      "It is needed to localize the embedded guest's symbols.")
  endif()
  message(STATUS "storage_compat: using objcopy ${SC_OBJCOPY}")
else()
  message(FATAL_ERROR "storage_compat: only macOS and Linux are supported; see README 'Platform support'.")
endif()

# The ONLY translation unit compiled against the guest headers. It must never see a
# host header: both duckdb.h copies declare the same C symbols.
add_library(sc_guest_shim OBJECT src/sc_guest.cpp)
add_dependencies(sc_guest_shim storage_compat_guest)
target_include_directories(sc_guest_shim PRIVATE
  ${STORAGE_COMPAT_GUEST_SRC}/src/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
# NOT hidden: the sc:: boundary symbols must stay visible so the blob step can keep
# them (everything else in the blob is localized).
set_target_properties(sc_guest_shim PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The shell lives in a script, not inline here: quoting through
# CMake -> Ninja/Make -> sh mangles it (an earlier inline version silently turned
# awk '{print $NF}' into '$($NF)' and produced an empty export list).
add_custom_command(
  OUTPUT ${GUEST_BLOB}
  DEPENDS storage_compat_guest sc_guest_shim ${CMAKE_CURRENT_SOURCE_DIR}/scripts/localize_guest.sh
  COMMAND ${CMAKE_COMMAND} -E env OBJCOPY=${SC_OBJCOPY}
          MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}
          ${CMAKE_CURRENT_SOURCE_DIR}/scripts/localize_guest.sh
          ${GUEST_BLOB} ${SC_ARCH} $<TARGET_OBJECTS:sc_guest_shim> ${GUEST_ARCHIVES}
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  VERBATIM)

add_custom_target(storage_compat_guest_blob DEPENDS ${GUEST_BLOB})

# The host half: sees only host headers, plus the POD boundary in src/sc_guest_api.hpp.
set(EXTENSION_SOURCES src/storage_compat_extension.cpp)

# Loadable only. See extension_config.cmake: a static link would put two DuckDBs into
# libduckdb and every duckdb:: static data member would be defined twice.
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

foreach(t ${LOADABLE_EXTENSION_NAME})
  add_dependencies(${t} storage_compat_guest_blob)
  target_link_libraries(${t} ${GUEST_BLOB})
endforeach()

