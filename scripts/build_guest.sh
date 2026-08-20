#!/usr/bin/env bash
# Build the GUEST DuckDB - the one that understands the newer storage format - so that
# it exposes ONLY its C API and keeps every C++ symbol internal.
#
#   ./scripts/build_guest.sh <guest-duckdb-src> <guest-build-dir> [jobs]
#
# Hidden visibility is not cosmetic. The extension also links the HOST DuckDB's C++ API;
# both libraries define the same `duckdb::` symbols with different object layouts, so a
# visible guest symbol can capture a host-facing call and silently run 2.x code over 1.5
# objects (observed: "INTERNAL Error: Missing DB manager").
set -euo pipefail
SRC="${1:?path to the guest duckdb source checkout}"
OUT="${2:?output build directory}"
JOBS="${3:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"

cmake -S "$SRC" -B "$OUT" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_VISIBILITY_PRESET=hidden \
  -DCMAKE_C_VISIBILITY_PRESET=hidden \
  -DCMAKE_VISIBILITY_INLINES_HIDDEN=ON \
  -DBUILD_UNITTESTS=OFF
cmake --build "$OUT" --target duckdb duckdb_static -j "$JOBS"

lib="$OUT/src/libduckdb.dylib"; [ -f "$lib" ] || lib="$OUT/src/libduckdb.so"
cpp=$(nm -gU "$lib" 2>/dev/null | grep -c 'ZN6duckdb' || true)
capi=$(nm -gU "$lib" 2>/dev/null | grep -c '_duckdb_' || true)
echo
echo "guest export check:  duckdb:: C++ = $cpp (must be 0)   C-API = $capi (must be > 0)"
[ "$cpp" = "0" ] || { echo "FATAL: guest still exports C++ symbols; hidden visibility did not apply"; exit 1; }
