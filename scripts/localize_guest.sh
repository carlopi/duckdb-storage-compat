#!/usr/bin/env bash
# Collapse the guest DuckDB's archives into ONE relocatable object that exports only
# the C API, with every other symbol made *local*.
#
#   localize_guest.sh <out.o> <arch> <archive> [archive...]
#
# Why this exists: building the guest with -fvisibility=hidden is NOT enough. Hidden
# means "private external", which still satisfies references from other objects in the
# same link - so the guest would capture the HOST DuckDB's `duckdb::` calls and run the
# newer engine's code over the host's objects. Both failure modes are silent, which is
# why this script asserts the result instead of trusting it.
set -euo pipefail
OUT="${1:?output object}"; ARCH="${2:?arch}"; shift 2
ARCHIVES=("$@")
WORK="$(dirname "$OUT")"

case "$(uname -s)" in
Darwin)
  # The export list must be EXACTLY the C API. A '_duckdb_*' pattern will not do: it
  # also matches third-party fsst entry points, some of which are hidden, and ld
  # refuses to export a hidden symbol. So take symbols that are external somewhere,
  # minus any that are hidden anywhere.
  nm -m "${ARCHIVES[0]}" | grep ' _duckdb_' > "$WORK/sc_syms.txt" || true
  grep 'private external' "$WORK/sc_syms.txt" | sed 's/.* //' | sort -u > "$WORK/sc_hidden.txt" || true
  grep ' external ' "$WORK/sc_syms.txt" | grep -v 'private external' | sed 's/.* //' | sort -u > "$WORK/sc_ext.txt" || true
  comm -23 "$WORK/sc_ext.txt" "$WORK/sc_hidden.txt" > "$WORK/sc_capi.txt"
  n=$(wc -l < "$WORK/sc_capi.txt" | tr -d ' ')
  [ "$n" -gt 100 ] || { echo "FATAL: derived only $n C-API symbols; expected the full C API"; exit 1; }
  echo "storage_compat: exporting $n C-API symbols from the guest blob"
  MACOS_VER="$(sw_vers -productVersion 2>/dev/null || echo 11.0)"
  ld -r -arch "$ARCH" -platform_version macos "$MACOS_VER" "$MACOS_VER" \
     -o "$OUT" -all_load "${ARCHIVES[@]}" -exported_symbols_list "$WORK/sc_capi.txt"
  exported=$(nm -g "$OUT" | grep -c ' [TDSB] __ZN6duckdb' || true)
  undef=$(nm -u "$OUT" | grep -c '6duckdb' || true)
  ;;
Linux)
  ld -r --whole-archive "${ARCHIVES[@]}" --no-whole-archive -o "$OUT.tmp"
  # hidden visibility produces STV_HIDDEN; --localize-hidden makes those local
  "${OBJCOPY:-objcopy}" --localize-hidden "$OUT.tmp" "$OUT"
  rm -f "$OUT.tmp"
  exported=$(nm -g --defined-only "$OUT" | grep -c '_ZN6duckdb' || true)
  undef=$(nm -u "$OUT" | grep -c '_ZN6duckdb' || true)
  capi=$(nm -g --defined-only "$OUT" | grep -c ' duckdb_' || true)
  echo "storage_compat: guest blob exports $capi C-API symbols"
  ;;
*)
  echo "FATAL: unsupported platform $(uname -s)"; exit 1;;
esac

# These two are the whole point. Each fails silently if left unchecked.
[ "$exported" = "0" ] || { echo "FATAL: guest blob exports $exported duckdb:: C++ symbols; they would collide with the host"; exit 1; }
[ "$undef" = "0" ]    || { echo "FATAL: guest blob has $undef undefined duckdb:: symbols; they would bind to the HOST"; exit 1; }
echo "storage_compat: guest blob OK (0 exported C++ symbols, 0 undefined)"
