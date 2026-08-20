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
  # On Linux DuckDB links the HOST duckdb statically into every loadable extension
  # (EXTENSION_STATIC_BUILD), so this object ends up beside a second copy of
  # libduckdb_static.a. Localizing is not sufficient there: symbols that are COMDAT
  # group signatures (`.data.rel.ro.local._ZN...[_ZN...]`, e.g.
  # duckdb::TableCatalogEntry::Name) cannot be localized - objcopy keeps a group
  # signature global so the linker can dedupe the group - and they then collide as
  # "multiple definition".
  #
  # So RENAME instead: every symbol this object defines, except the sc:: boundary,
  # gets a prefix. Internal references are relocations against those same symbol
  # table entries, so they follow the rename; the host's duckdb symbols keep their
  # original names and nothing overlaps. Undefined symbols (libc, libstdc++) are not
  # listed and so are untouched.
  nm -g --defined-only "$SHIM" | grep -E ' [TWD] _ZN2sc' | sed 's/.* //' | sort -u > "$WORK/sc_keep.txt"
  n=$(wc -l < "$WORK/sc_keep.txt" | tr -d ' ')
  [ "$n" -gt 0 ] || { echo "FATAL: no sc:: boundary symbols found in $SHIM"; nm -g --defined-only "$SHIM" | head -20; exit 1; }
  echo "storage_compat: guest blob will export $n sc:: symbols and nothing else"
  echo "storage_compat: ld=$(command -v ld) objcopy=${OBJCOPY:-objcopy}"

  ld -r --whole-archive "${ARCHIVES[@]}" --no-whole-archive "$SHIM" -o "$OUT.tmp"

  nm --defined-only "$OUT.tmp" | awk 'NF>=3 {print $3}' | sort -u \
    | grep -v '^_ZN2sc' | grep -v '^$' \
    | awk '{print $1" scguest_"$1}' > "$WORK/sc_rename.txt"
  r=$(wc -l < "$WORK/sc_rename.txt" | tr -d ' ')
  echo "storage_compat: renaming $r embedded symbols out of the host's way"
  "${OBJCOPY:-objcopy}" --redefine-syms="$WORK/sc_rename.txt" "$OUT.tmp" "$OUT"
  rm -f "$OUT.tmp"

  # Nothing the host also defines may remain under its original name.
  leaked=$(nm --defined-only "$OUT" | awk 'NF>=3 {print $3}' \
           | grep -Ec '^(_Z.*[0-9]duckdb|duckdb_[a-z])' || true)
  cpp_exported=$leaked
  capi_exported=0
  undef=$(nm -u "$OUT" | grep -c '_ZN6duckdb' || true)
  if [ "$leaked" != "0" ]; then
    echo "---- still defined under a host-colliding name (first 20) ----"
    nm --defined-only "$OUT" | awk 'NF>=3 {print $3}' | grep -E '^(_Z.*[0-9]duckdb|duckdb_[a-z])' | head -20
  fi
  kept=$(nm -g --defined-only "$OUT" | grep -c ' _ZN2sc' || true)
  [ "$kept" = "$n" ] || { echo "FATAL: expected $n sc:: symbols to survive, found $kept"; exit 1; }
  ;;
*)
  echo "FATAL: unsupported platform $(uname -s)"; exit 1;;
esac

[ "$cpp_exported" = "0" ] || { echo "FATAL: blob exports $cpp_exported duckdb:: C++ symbols; they would collide with the host"; exit 1; }
[ "$capi_exported" = "0" ] || { echo "FATAL: blob exports $capi_exported duckdb_* C symbols; the static build links this into libduckdb itself and they would collide"; exit 1; }
[ "$undef" = "0" ]        || { echo "FATAL: blob has $undef undefined duckdb:: symbols; they would bind to the HOST"; exit 1; }
echo "storage_compat: guest blob OK (only sc:: exported; 0 duckdb C/C++ exports; 0 undefined)"
