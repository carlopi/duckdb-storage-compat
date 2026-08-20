// GUEST SIDE - DuckDB 2.0.0, reached only through its C API.
// Includes the guest's duckdb.h and NOTHING from the host.
#include "duckdb.h"
#include "sc_guest_api.hpp"
#include <cstring>

namespace sc {

struct Db {
	duckdb_database db;
	duckdb_connection con;
};

struct Scan {
	duckdb_result res;
	duckdb_data_chunk chunk = nullptr;
	bool has_result = false;
};

static PType MapType(duckdb_type t) {
	switch (t) {
	case DUCKDB_TYPE_BOOLEAN:      return PType::BOOL;
	case DUCKDB_TYPE_TINYINT:      return PType::I8;
	case DUCKDB_TYPE_SMALLINT:     return PType::I16;
	case DUCKDB_TYPE_INTEGER:      return PType::I32;
	case DUCKDB_TYPE_BIGINT:       return PType::I64;
	case DUCKDB_TYPE_HUGEINT:      return PType::I128;
	case DUCKDB_TYPE_UTINYINT:     return PType::U8;
	case DUCKDB_TYPE_USMALLINT:    return PType::U16;
	case DUCKDB_TYPE_UINTEGER:     return PType::U32;
	case DUCKDB_TYPE_UBIGINT:      return PType::U64;
	case DUCKDB_TYPE_UHUGEINT:     return PType::U128;
	case DUCKDB_TYPE_FLOAT:        return PType::FLT;
	case DUCKDB_TYPE_DOUBLE:       return PType::DBL;
	case DUCKDB_TYPE_VARCHAR:      return PType::VARCHAR;
	case DUCKDB_TYPE_BLOB:         return PType::BLOB;
	case DUCKDB_TYPE_DATE:         return PType::DATE;
	case DUCKDB_TYPE_TIME:         return PType::TIME;
	case DUCKDB_TYPE_TIMESTAMP_S:  return PType::TIMESTAMP_SEC;
	case DUCKDB_TYPE_TIMESTAMP_MS: return PType::TIMESTAMP_MS;
	case DUCKDB_TYPE_TIMESTAMP:    return PType::TIMESTAMP_US;
	case DUCKDB_TYPE_TIMESTAMP_NS: return PType::TIMESTAMP_NS;
	case DUCKDB_TYPE_TIMESTAMP_TZ: return PType::TIMESTAMP_TZ;
	case DUCKDB_TYPE_TIME_TZ:      return PType::TIME_TZ;
	case DUCKDB_TYPE_INTERVAL:     return PType::INTERVAL;
	case DUCKDB_TYPE_UUID:         return PType::UUID;
	default:                       return PType::UNSUPPORTED;
	}
}

static bool RunQuery(Db *db, const std::string &sql, duckdb_result &res, std::string &err) {
	if (duckdb_query(db->con, sql.c_str(), &res) != DuckDBSuccess) {
		auto e = duckdb_result_error(&res);
		err = e ? e : "unknown guest error";
		duckdb_destroy_result(&res);
		return false;
	}
	return true;
}

static std::string GetStr(duckdb_data_chunk chunk, idx_t col, idx_t row, bool &is_null) {
	auto v = duckdb_data_chunk_get_vector(chunk, col);
	auto mask = duckdb_vector_get_validity(v);
	if (mask && !(mask[row / 64] & (1ULL << (row % 64)))) { is_null = true; return std::string(); }
	is_null = false;
	auto s = static_cast<duckdb_string_t *>(duckdb_vector_get_data(v))[row];
	return (s.value.inlined.length <= 12) ? std::string(s.value.inlined.inlined, s.value.inlined.length)
	                                      : std::string(s.value.pointer.ptr, s.value.pointer.length);
}

Db *Open(const std::string &path, const std::string &memory_limit, std::string &err) {
	duckdb_config cfg;
	duckdb_create_config(&cfg);
	duckdb_set_config(cfg, "access_mode", "READ_ONLY");
	// no second thread pool: the guest decompresses and hands over on the calling thread
	duckdb_set_config(cfg, "threads", "1");
	if (!memory_limit.empty()) {
		duckdb_set_config(cfg, "memory_limit", memory_limit.c_str());
	}
	auto db = new Db();
	char *open_err = nullptr;
	if (duckdb_open_ext(path.c_str(), &db->db, cfg, &open_err) != DuckDBSuccess) {
		err = open_err ? open_err : "failed to open database";
		if (open_err) { duckdb_free(open_err); }
		duckdb_destroy_config(&cfg);
		delete db;
		return nullptr;
	}
	duckdb_destroy_config(&cfg);
	duckdb_connect(db->db, &db->con);
	return db;
}

void Close(Db *db) {
	if (!db) { return; }
	duckdb_disconnect(&db->con);
	duckdb_close(&db->db);
	delete db;
}

std::vector<std::string> ListSchemas(Db *db, std::string &err) {
	std::vector<std::string> result;
	duckdb_result res;
	if (!RunQuery(db, "SELECT schema_name FROM duckdb_schemas() WHERE NOT internal ORDER BY 1", res, err)) {
		return result;
	}
	while (auto chunk = duckdb_fetch_chunk(res)) {
		auto n = duckdb_data_chunk_get_size(chunk);
		for (idx_t i = 0; i < n; i++) {
			bool is_null;
			auto s = GetStr(chunk, 0, i, is_null);
			if (!is_null) { result.push_back(s); }
		}
		duckdb_destroy_data_chunk(&chunk);
	}
	duckdb_destroy_result(&res);
	return result;
}

std::vector<TableDesc> ListTables(Db *db, std::string &err) {
	std::vector<TableDesc> result;
	duckdb_result res;
	// tables AND views - both are readable through the guest's planner
	const char *sql =
	    "SELECT table_schema, table_name, column_name, data_type "
	    "FROM information_schema.columns ORDER BY table_schema, table_name, ordinal_position";
	if (!RunQuery(db, sql, res, err)) { return result; }
	while (auto chunk = duckdb_fetch_chunk(res)) {
		auto n = duckdb_data_chunk_get_size(chunk);
		for (idx_t i = 0; i < n; i++) {
			bool dummy;
			auto sch = GetStr(chunk, 0, i, dummy);
			auto tbl = GetStr(chunk, 1, i, dummy);
			auto col = GetStr(chunk, 2, i, dummy);
			auto typ = GetStr(chunk, 3, i, dummy);
			if (result.empty() || result.back().schema != sch || result.back().name != tbl) {
				TableDesc td;
				td.schema = sch;
				td.name = tbl;
				result.push_back(td);
			}
			ColumnDesc cd;
			cd.name = col;
			cd.type_name = typ;
			cd.ptype = PType::UNSUPPORTED; // filled in at scan time from the real logical type
			result.back().columns.push_back(cd);
		}
		duckdb_destroy_data_chunk(&chunk);
	}
	duckdb_destroy_result(&res);
	return result;
}

bool DescribeTable(Db *db, TableDesc &td, std::string &err) {
	// Resolve real physical types positionally, preserving the type_name strings that
	// information_schema already gave us (the C API has no type->string rendering).
	std::string sql = "SELECT * FROM \"" + td.schema + "\".\"" + td.name + "\" LIMIT 0";
	duckdb_result res;
	if (!RunQuery(db, sql, res, err)) { return false; }
	auto n = duckdb_column_count(&res);
	if (n != td.columns.size()) {
		duckdb_destroy_result(&res);
		err = "column count mismatch for " + td.schema + "." + td.name;
		return false;
	}
	for (idx_t i = 0; i < n; i++) {
		td.columns[i].ptype = MapType(duckdb_column_type(&res, i));
	}
	duckdb_destroy_result(&res);
	return true;
}

Scan *ScanBegin(Db *db, const std::string &sql, std::string &err) {
	auto s = new Scan();
	if (!RunQuery(db, sql, s->res, err)) { delete s; return nullptr; }
	s->has_result = true;
	return s;
}

void ScanEnd(Scan *s) {
	if (!s) { return; }
	if (s->chunk) { duckdb_destroy_data_chunk(&s->chunk); }
	if (s->has_result) { duckdb_destroy_result(&s->res); }
	delete s;
}

uint64_t ScanNext(Scan *s, std::string &err) {
	if (s->chunk) { duckdb_destroy_data_chunk(&s->chunk); s->chunk = nullptr; }
	s->chunk = duckdb_fetch_chunk(s->res);
	if (!s->chunk) { return 0; }
	return duckdb_data_chunk_get_size(s->chunk);
}

const void *ColData(Scan *s, uint64_t col) {
	return duckdb_vector_get_data(duckdb_data_chunk_get_vector(s->chunk, col));
}

const uint64_t *ColValidity(Scan *s, uint64_t col) {
	return duckdb_vector_get_validity(duckdb_data_chunk_get_vector(s->chunk, col));
}

void ColString(Scan *s, uint64_t col, uint64_t row, const char **ptr, uint64_t *len) {
	auto v = duckdb_data_chunk_get_vector(s->chunk, col);
	auto &sv = static_cast<duckdb_string_t *>(duckdb_vector_get_data(v))[row];
	if (sv.value.inlined.length <= 12) {
		*ptr = sv.value.inlined.inlined;
		*len = sv.value.inlined.length;
	} else {
		*ptr = sv.value.pointer.ptr;
		*len = sv.value.pointer.length;
	}
}

std::string StorageVersion(Db *db) {
	std::string err;
	duckdb_result res;
	const char *sql = "SELECT tags['storage_version'] FROM duckdb_databases() "
	                  "WHERE database_name NOT IN ('system','temp') LIMIT 1";
	if (!RunQuery(db, sql, res, err)) { return std::string(); }
	std::string out;
	while (auto chunk = duckdb_fetch_chunk(res)) {
		if (duckdb_data_chunk_get_size(chunk) > 0) {
			bool is_null;
			auto s = GetStr(chunk, 0, 0, is_null);
			if (!is_null) { out = s; }
		}
		duckdb_destroy_data_chunk(&chunk);
	}
	duckdb_destroy_result(&res);
	return out;
}

const char *GuestVersion() { return duckdb_library_version(); }

} // namespace sc
