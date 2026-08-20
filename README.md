# storage_compat

Read a DuckDB database written in a **newer storage format** than your DuckDB supports.

```console
$ duckdb -c "ATTACH 'file200.db' AS db;"
IO Error: Trying to read a database file with version number 999, but we can only
read versions between 64 and 68.
```

```sql
LOAD storage_compat;
ATTACH 'storage_compat:file200.db' AS db;

SELECT * FROM db.events ORDER BY id;
┌───────┬─────────┬────────┬───────────┬──────────────────────────────┐
│  id   │  name   │ score  │   tags    │             who              │
│ int32 │ varchar │ double │  int32[]  │ struct(x integer, y varchar) │
├───────┼─────────┼────────┼───────────┼──────────────────────────────┤
│     1 │ alpha   │    1.5 │ [1, 2, 3] │ {'x': 7, 'y': nested}        │
│     2 │ beta    │    2.5 │ [4]       │ {'x': 8, 'y': z}             │
│     3 │ NULL    │   NULL │ NULL      │ NULL                         │
└───────┴─────────┴────────┴───────────┴──────────────────────────────┘
```

## How it works

The extension **embeds the newer DuckDB** and drives it only through its C API. The
newer engine reads its own format, so results are correct by construction rather than
by reimplementing a reader; the extension converts the chunks it hands back into the
host engine's vectors.

Three layers, and the separation is load-bearing rather than tidy:

| file | includes | role |
|---|---|---|
| `src/sc_guest_api.hpp` | neither engine | POD boundary: own type enum, plain structs |
| `src/sc_guest.cpp` | **guest** `duckdb.h` only | drives the newer DuckDB via its C API |
| `src/storage_compat_extension.cpp` | **host** `duckdb.hpp` only | StorageExtension, Catalog, scan |

The two `.cpp` files never see each other's headers: both `duckdb.h` copies declare the
same C symbols, and both engines declare `namespace duckdb`.

## It never guesses

A column whose type the host cannot represent stays visible in the catalog, named for
what it actually is:

```sql
SHOW ALL TABLES;
-- future | [id, label, ts] | [INTEGER, VARCHAR, 'UNSUPPORTED(TIMESTAMPTZ_NS)']

SELECT id, label FROM db.future;   -- works
SELECT * FROM db.future;           -- refused, with the reason:
```

```
Not implemented Error: storage_compat: cannot read "main.future": column "ts" has type
TIMESTAMPTZ_NS, which this DuckDB version cannot represent.
Readable columns: id, label. Select those explicitly instead of *.
"db" is of storage format v2.0.0+ while this DuckDB is v1.5.5 - consider upgrading your
DuckDB to interact with it natively.
See db.storage_compat_tables() for the full inventory.
```

```sql
SELECT * FROM db.storage_compat_tables() WHERE NOT readable;
-- main | future | ts | TIMESTAMPTZ_NS | false | column "ts" has type ...
```

Attaching is always read-only: every write path is refused and the file is left
byte-identical. The embedded engine is opened read-only as well.

## Building

The extension needs a second DuckDB checkout — the *guest*, whose storage format you
want to read.

```sh
git clone --recurse-submodules https://github.com/TODO-your-org/duckdb-storage-compat
cd duckdb-storage-compat

# 1. build the guest with everything but its C API hidden
./scripts/build_guest.sh /path/to/newer-duckdb build/guest

# 2. build the extension against it
GEN=ninja \
EXTRA_CMAKE_VARIABLES="-DSTORAGE_COMPAT_GUEST_SRC=/path/to/newer-duckdb -DSTORAGE_COMPAT_GUEST_BUILD=$PWD/build/guest" \
make release
```

`./demo.sh <host-duckdb> <guest-duckdb> <storage_compat.duckdb_extension>` runs a full
walkthrough.

### Why the build is unusual

Two properties of the guest build are load-bearing, and both fail *silently* if skipped:

1. **Hidden visibility.** `DUCKDB_API` is a no-op on unix while `DUCKDB_C_API` carries
   `visibility("default")`, so `-fvisibility=hidden` hides the ~22k `duckdb::` C++
   symbols and keeps the ~549 C-API ones — with no source changes.
2. **Localization.** Hidden is not enough: it means *private external*, which still
   satisfies references from other objects in the same link. Without collapsing the
   guest into one object with those symbols made **local**, the guest captures the
   host's `duckdb::` calls and runs the newer engine's code over the host's objects.
   Observed symptom: `INTERNAL Error: Missing DB manager`.

Related trap, for anyone tempted to `dlopen` the guest instead: on macOS `RTLD_LOCAL`
does **not** isolate two DuckDBs, because Mach-O coalesces C++ vague-linkage symbols
across images at runtime regardless of the two-level namespace.

The CMake asserts both properties and fails the build if either regresses.

## Platform support

| platform | status |
|---|---|
| `osx_arm64`, `osx_amd64` | supported (`ld -r -exported_symbols_list`) |
| `linux_amd64`, `linux_arm64` | implemented via `ld -r` + `objcopy --localize-hidden`, **not yet verified** |
| Windows, WASM | not supported — no symbol-localization step implemented |

## Size

This extension contains a complete second DuckDB, so it is tens of MB. That is
inherent: correctness comes from shipping the engine that owns the format.

## License

MIT
