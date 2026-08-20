// The guest/host boundary. Deliberately contains NO duckdb type of either engine:
// both sides include this, and neither can see the other's headers.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace sc {

// Our own physical-type tag, independent of either engine's enums.
enum class PType : uint8_t {
	BOOL, I8, I16, I32, I64, I128, U8, U16, U32, U64, U128,
	FLT, DBL, VARCHAR, BLOB, DATE, TIME, TIMESTAMP_SEC, TIMESTAMP_MS, TIMESTAMP_US, TIMESTAMP_NS,
	TIMESTAMP_TZ, TIME_TZ, INTERVAL, DECIMAL16, DECIMAL32, DECIMAL64, DECIMAL128, UUID,
	UNSUPPORTED
};

struct ColumnDesc {
	std::string name;
	std::string type_name; // the guest's own rendering, e.g. "STRUCT(x INTEGER)"
	PType ptype;
};

struct TableDesc {
	std::string schema;
	std::string name;
	std::vector<ColumnDesc> columns;
};

// Opaque handles owned by the guest side.
struct Db;
struct Scan;

Db *Open(const std::string &path, const std::string &memory_limit, std::string &err);
void Close(Db *db);
std::vector<std::string> ListSchemas(Db *db, std::string &err);
std::vector<TableDesc> ListTables(Db *db, std::string &err);
//! Resolve real physical types by preparing `SELECT * FROM t LIMIT 0` on the guest.
bool DescribeTable(Db *db, TableDesc &td, std::string &err);

Scan *ScanBegin(Db *db, const std::string &sql, std::string &err);
void ScanEnd(Scan *s);
//! Advance to the next chunk. Returns rows in the chunk, 0 when exhausted.
uint64_t ScanNext(Scan *s, std::string &err);
//! Raw flat data pointer + validity mask for a column of the current chunk.
const void *ColData(Scan *s, uint64_t col);
const uint64_t *ColValidity(Scan *s, uint64_t col);
//! VARCHAR/BLOB accessor - keeps duckdb_string_t on the guest side.
void ColString(Scan *s, uint64_t col, uint64_t row, const char **ptr, uint64_t *len);

const char *GuestVersion();
//! The storage version the FILE was written with, e.g. "v2.0.0+".
std::string StorageVersion(Db *db);

} // namespace sc
