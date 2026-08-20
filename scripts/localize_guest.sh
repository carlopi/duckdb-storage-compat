#!/usr/bin/env bash
# Collapse the guest DuckDB *and the shim that drives it* into ONE relocatable object
# that exports only the `sc::` boundary functions. Everything else - the guest's C++
# API and its C API - becomes local.
#
#   localize_guest.sh <out.o> <arch> <shim.o> <archive> [archive...]
#
# Why the C API must be local too: the static build links this object INTO
# libduckdb.dylib, where the host defines the very same duckdb_* C symbols. Exporting
# them gives "duplicate symbol _duckdb_bind_varchar". The shim's references to them
# resolve inside this object, so nothing needs them visible.
#
# And why any of this: building the guest with -fvisibility=hidden is not enough.
# Hidden means "private external", which still satisfies references from other objects
# in the same link - so the guest would capture the HOST's `duckdb::` calls and run the
# newer engine's code over the host's objects. Both failure modes are silent, hence the
# assertions at the end.
set -euo pipefail
OUT="${1:?output object}"; ARCH="${2:?arch}"; SHIM="${3:?shim object}"; shift 3
ARCHIVES=("$@")
WORK="$(dirname "$OUT")"

case "$(uname -s)" in
Darwin)
  # keep exactly the sc:: boundary the host half calls
  nm -g "$SHIM" | grep ' T __ZN2sc' | sed 's/.* //' | sort -u > "$WORK/sc_keep.txt"
  n=$(wc -l < "$WORK/sc_keep.txt" | tr -d ' ')
  [ "$n" -gt 0 ] || { echo "FATAL: no sc:: boundary symbols found in $SHIM"; exit 1; }
  echo "storage_compat: guest blob will export $n sc:: symbols and nothing else"
  MACOS_VER="${MACOSX_DEPLOYMENT_TARGET:-$(sw_vers -productVersion 2>/dev/null || echo 11.0)}"
  ld -r -arch "$ARCH" -platform_version macos "$MACOS_VER" "$MACOS_VER" \
     -o "$OUT" -all_load "${ARCHIVES[@]}" "$SHIM" -exported_symbols_list "$WORK/sc_keep.txt"
  cpp_exported=$(nm -g "$OUT" | grep -c ' [TDSB] __ZN6duckdb' || true)
  capi_exported=$(nm -g "$OUT" | grep -cE ' [TDSB] _duckdb_[a-z]' || true)
  undef=$(nm -u "$OUT" | grep -c '6duckdb' || true)
  ;;
Linux)
  nm -g --defined-only "$SHIM" | grep ' T _ZN2sc' | sed 's/.* //' | sort -u > "$WORK/sc_keep.txt"
  n=$(wc -l < "$WORK/sc_keep.txt" | tr -d ' ')
  [ "$n" -gt 0 ] || { echo "FATAL: no sc:: boundary symbols found in $SHIM"; exit 1; }
  echo "storage_compat: guest blob will export $n sc:: symbols and nothing else"
  echo "storage_compat: ld=$(command -v ld) objcopy=${OBJCOPY:-objcopy}"
  ld -r --whole-archive "${ARCHIVES[@]}" --no-whole-archive "$SHIM" -o "$OUT.tmp"
  "${OBJCOPY:-objcopy}" --keep-global-symbols="$WORK/sc_keep.txt" "$OUT.tmp" "$OUT"
  rm -f "$OUT.tmp"
  cpp_exported=$(nm -g --defined-only "$OUT" | grep -c '_ZN6duckdb' || true)
  capi_exported=$(nm -g --defined-only "$OUT" | grep -cE ' duckdb_[a-z]' || true)
  undef=$(nm -u "$OUT" | grep -c '_ZN6duckdb' || true)
  ;;
*)
  echo "FATAL: unsupported platform $(uname -s)"; exit 1;;
esac

[ "$cpp_exported" = "0" ] || { echo "FATAL: blob exports $cpp_exported duckdb:: C++ symbols; they would collide with the host"; exit 1; }
[ "$capi_exported" = "0" ] || { echo "FATAL: blob exports $capi_exported duckdb_* C symbols; the static build links this into libduckdb itself and they would collide"; exit 1; }
[ "$undef" = "0" ]        || { echo "FATAL: blob has $undef undefined duckdb:: symbols; they would bind to the HOST"; exit 1; }
echo "storage_compat: guest blob OK (only sc:: exported; 0 duckdb C/C++ exports; 0 undefined)"
