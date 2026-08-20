cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME storage_compat)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})
set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src)

# ---------------------------------------------------------------------------
# The GUEST: a second DuckDB, the one that understands the newer storage format.
#
# It is embedded in this extension and reached ONLY through its C API. Two build
# properties are load-bearing, not stylistic:
#
#   1. it is compiled with hidden visibility, so its ~22k `duckdb::` C++ symbols
#      never enter the export table. DUCKDB_API is a no-op on unix while
#      DUCKDB_C_API carries visibility("default"), so this hides the C++ API and
#      keeps the C API - exactly what we need, with no source changes.
#   2. its archives are then collapsed into ONE relocatable object with every
#      non-C-API symbol made *local*. Hidden visibility alone is NOT enough:
#      hidden means "private external", which still satisfies references from
#      other objects in the same link, so the guest would capture the host's
#      duckdb:: calls and run 2.x code on 1.5 objects.
#
# Point STORAGE_COMPAT_GUEST_SRC/BUILD at a checkout+build produced by
# scripts/build_guest.sh.
# ---------------------------------------------------------------------------
# Defaults to the pinned `duckdb-guest` submodule; override to try another checkout.
set(STORAGE_COMPAT_GUEST_SRC "${CMAKE_CURRENT_SOURCE_DIR}/duckdb-guest"
    CACHE PATH "Source checkout of the guest DuckDB")
set(STORAGE_COMPAT_GUEST_BUILD "${CMAKE_CURRENT_BINARY_DIR}/guest"
    CACHE PATH "Build dir produced by scripts/build_guest.sh")

if(NOT EXISTS "${STORAGE_COMPAT_GUEST_SRC}/src/include/duckdb.h")
  message(FATAL_ERROR
    "guest DuckDB sources not found at ${STORAGE_COMPAT_GUEST_SRC}\n"
    "  git submodule update --init --recursive")
endif()
if(NOT EXISTS "${STORAGE_COMPAT_GUEST_BUILD}/src/libduckdb_static.a")
  message(FATAL_ERROR
    "guest DuckDB not built at ${STORAGE_COMPAT_GUEST_BUILD}\n"
    "  ./scripts/build_guest.sh   # uses the pinned submodule by default")
endif()

set(GUEST_BLOB ${CMAKE_CURRENT_BINARY_DIR}/guest_blob.o)
set(GUEST_CAPI_LIST ${CMAKE_CURRENT_BINARY_DIR}/guest_capi_symbols.txt)

set(GUEST_ARCHIVES
  ${STORAGE_COMPAT_GUEST_BUILD}/src/libduckdb_static.a
  ${STORAGE_COMPAT_GUEST_BUILD}/extension/libduckdb_generated_extension_loader.a
  ${STORAGE_COMPAT_GUEST_BUILD}/extension/parquet/libparquet_extension.a
  ${STORAGE_COMPAT_GUEST_BUILD}/extension/core_functions/libcore_functions_extension.a)
# NB: do NOT add third_party/*.a - libduckdb_static.a already bundles them and
# -all_load would then report duplicate duckdb_miniz:: symbols.

if(APPLE)
  # The export list must be the EXACT C-API symbol set: a `_duckdb_*` wildcard also
  # matches the hidden _duckdb_fsst_decompress and ld refuses to export a hidden symbol.
  add_custom_command(
    OUTPUT ${GUEST_CAPI_LIST}
    COMMAND sh -c "nm -gU '${STORAGE_COMPAT_GUEST_BUILD}/src/libduckdb.dylib' | awk '$$2 ~ /^[TDS]$$/ {print $$3}' | grep '^_duckdb_' | sort -u > '${GUEST_CAPI_LIST}'"
    VERBATIM)
  add_custom_command(
    OUTPUT ${GUEST_BLOB}
    DEPENDS ${GUEST_CAPI_LIST}
    COMMAND ld -r -arch ${CMAKE_HOST_SYSTEM_PROCESSOR}
            -platform_version macos ${CMAKE_OSX_DEPLOYMENT_TARGET_OR_DEFAULT} ${CMAKE_OSX_DEPLOYMENT_TARGET_OR_DEFAULT}
            -o ${GUEST_BLOB} -all_load ${GUEST_ARCHIVES}
            -exported_symbols_list ${GUEST_CAPI_LIST}
    COMMAND sh -c "test \"$$(nm -g '${GUEST_BLOB}' | grep -c ' [TDSB] __ZN6duckdb' || true)\" = 0 || (echo 'FATAL: guest blob still exports duckdb:: C++ symbols' && exit 1)"
    COMMAND sh -c "test \"$$(nm -u '${GUEST_BLOB}' | grep -c '6duckdb' || true)\" = 0 || (echo 'FATAL: guest blob has undefined duckdb:: symbols - they would bind to the HOST' && exit 1)"
    VERBATIM)
elseif(UNIX)
  # ELF equivalent: -r plus --localize-hidden demotes the hidden symbols to local.
  add_custom_command(
    OUTPUT ${GUEST_BLOB}
    COMMAND ${CMAKE_LINKER} -r --whole-archive ${GUEST_ARCHIVES} --no-whole-archive -o ${GUEST_BLOB}.tmp
    COMMAND ${CMAKE_OBJCOPY} --localize-hidden ${GUEST_BLOB}.tmp ${GUEST_BLOB}
    COMMAND sh -c "test \"$$(nm -g --defined-only '${GUEST_BLOB}' | grep -c '_ZN6duckdb' || true)\" = 0 || (echo 'FATAL: guest blob still exports duckdb:: C++ symbols' && exit 1)"
    VERBATIM)
else()
  message(FATAL_ERROR "storage_compat: only macOS and Linux are supported today; see README 'Platform support'.")
endif()

add_custom_target(storage_compat_guest_blob DEPENDS ${GUEST_BLOB})

# The guest shim is the ONLY translation unit compiled against the guest headers.
# It must never see a host header: both duckdb.h copies declare the same C symbols.
add_library(sc_guest_shim OBJECT src/sc_guest.cpp)
target_include_directories(sc_guest_shim PRIVATE ${STORAGE_COMPAT_GUEST_SRC}/src/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(sc_guest_shim PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden)

# The host half. Sees only host headers, plus the POD boundary in src/sc_guest_api.hpp.
set(EXTENSION_SOURCES src/storage_compat_extension.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

foreach(t ${EXTENSION_NAME} ${LOADABLE_EXTENSION_NAME})
  add_dependencies(${t} storage_compat_guest_blob)
  target_sources(${t} PRIVATE $<TARGET_OBJECTS:sc_guest_shim>)
  target_link_libraries(${t} ${GUEST_BLOB})
endforeach()

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
