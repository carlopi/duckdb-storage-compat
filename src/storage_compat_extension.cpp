// HOST SIDE - a v1.5.x loadable extension. Includes ONLY host headers.
//   LOAD storage_compat;
//   ATTACH 'storage_compat:file200.db' AS db;
#include "duckdb.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/transaction/transaction_manager.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"

#include "sc_guest_api.hpp"

namespace duckdb {

//! The one line both the read and the write refusal end with, so the remedy is
//! phrased identically wherever a user hits the version boundary.
static string VersionHint(const string &db_name, const string &storage_version) {
	auto format = storage_version.empty() ? string("a newer format") : storage_version;
	return StringUtil::Format("\"%s\" is of storage format %s while this DuckDB is %s - consider "
	                          "upgrading your DuckDB to interact with it natively.",
	                          db_name, format, DuckDB::LibraryVersion());
}

static LogicalType MapPType(sc::PType t) {
	switch (t) {
	case sc::PType::BOOL:          return LogicalType::BOOLEAN;
	case sc::PType::I8:            return LogicalType::TINYINT;
	case sc::PType::I16:           return LogicalType::SMALLINT;
	case sc::PType::I32:           return LogicalType::INTEGER;
	case sc::PType::I64:           return LogicalType::BIGINT;
	case sc::PType::I128:          return LogicalType::HUGEINT;
	case sc::PType::U8:            return LogicalType::UTINYINT;
	case sc::PType::U16:           return LogicalType::USMALLINT;
	case sc::PType::U32:           return LogicalType::UINTEGER;
	case sc::PType::U64:           return LogicalType::UBIGINT;
	case sc::PType::U128:          return LogicalType::UHUGEINT;
	case sc::PType::FLT:           return LogicalType::FLOAT;
	case sc::PType::DBL:           return LogicalType::DOUBLE;
	case sc::PType::VARCHAR:       return LogicalType::VARCHAR;
	case sc::PType::BLOB:          return LogicalType::BLOB;
	case sc::PType::DATE:          return LogicalType::DATE;
	case sc::PType::TIME:          return LogicalType::TIME;
	case sc::PType::TIMESTAMP_SEC: return LogicalType::TIMESTAMP_S;
	case sc::PType::TIMESTAMP_MS:  return LogicalType::TIMESTAMP_MS;
	case sc::PType::TIMESTAMP_US:  return LogicalType::TIMESTAMP;
	case sc::PType::TIMESTAMP_NS:  return LogicalType::TIMESTAMP_NS;
	case sc::PType::TIMESTAMP_TZ:  return LogicalType::TIMESTAMP_TZ;
	case sc::PType::TIME_TZ:       return LogicalType::TIME_TZ;
	case sc::PType::INTERVAL:      return LogicalType::INTERVAL;
	case sc::PType::UUID:          return LogicalType::UUID;
	default:                       return LogicalType::INVALID;
	}
}

//===--------------------------------------------------------------------===//
// scan table function
//===--------------------------------------------------------------------===//
struct ScScanBind : public TableFunctionData {
	sc::Db *db;
	string from_clause;
	vector<LogicalType> types;
	vector<string> names;
	//! columns the guest renders as text because we do not transfer them natively yet
	vector<bool> via_text;
	//! per column: empty if readable, otherwise why this engine cannot read it
	vector<string> unsupported_reason;
	string table_label;
	string catalog_label;
	string storage_version;
};

struct ScScanState : public GlobalTableFunctionState {
	sc::Scan *scan = nullptr;
	//! output slot -> bind column index; INVALID_INDEX for virtual columns (rowid / count(*))
	vector<idx_t> projection;
	//! VARCHAR staging vectors for text-transferred columns. These MUST outlive the
	//! output chunk: the cast result can reference their string heap, and a consumer
	//! that materialises (join, ORDER BY) reads it after this call returns.
	vector<unique_ptr<Vector>> staging;
	~ScScanState() override { if (scan) { sc::ScanEnd(scan); } }
	idx_t MaxThreads() const override { return 1; }
};

static unique_ptr<GlobalTableFunctionState> ScScanInit(ClientContext &, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<ScScanBind>();
	auto state = make_uniq<ScScanState>();

	// Projection pushdown: ask the guest only for the columns actually needed.
	// Virtual columns (rowid, and the empty column that count(*) projects) have no
	// counterpart on the guest and are filled in on this side.
	string select_list;
	for (auto col_id : input.column_ids) {
		if (col_id >= bind.types.size()) {
			state->projection.push_back(DConstants::INVALID_INDEX);
			continue;
		}
		if (!bind.unsupported_reason[col_id].empty()) {
			// Visible in the catalog, but reading it would mean inventing a value.
			// SELECT * lands here too, so say what CAN be selected instead of just refusing.
			vector<string> readable;
			for (idx_t i = 0; i < bind.names.size(); i++) {
				if (bind.unsupported_reason[i].empty()) {
					readable.push_back(bind.names[i]);
				}
			}
			auto hint = readable.empty()
			                ? string("No column of this table can be read by this DuckDB version.")
			                : ("Readable columns: " + StringUtil::Join(readable, ", ") +
			                   ". Select those explicitly instead of *.");
			throw NotImplementedException(
			    "storage_compat: cannot read \"%s\": %s.\n%s\n%s\n"
			    "See %s.storage_compat_tables() for the full inventory.",
			    bind.table_label, bind.unsupported_reason[col_id], hint,
			    VersionHint(bind.catalog_label, bind.storage_version), bind.catalog_label);
		}
		state->projection.push_back(col_id);
		if (!select_list.empty()) { select_list += ", "; }
		select_list += bind.via_text[col_id] ? ("CAST(\"" + bind.names[col_id] + "\" AS VARCHAR)")
		                                     : ("\"" + bind.names[col_id] + "\"");
	}
	if (select_list.empty()) {
		select_list = "1"; // count(*) - we only need the row count back
	}
	state->staging.resize(state->projection.size());
	for (idx_t i = 0; i < state->projection.size(); i++) {
		auto bind_col = state->projection[i];
		if (bind_col != DConstants::INVALID_INDEX && bind.via_text[bind_col]) {
			state->staging[i] = make_uniq<Vector>(LogicalType::VARCHAR);
		}
	}
	string sql = "SELECT " + select_list + " FROM " + bind.from_clause;
	string err;
	state->scan = sc::ScanBegin(bind.db, sql, err);
	if (!state->scan) {
		throw IOException("storage_compat: %s", err);
	}
	return std::move(state);
}

static void ScScanFunc(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &bind = input.bind_data->Cast<ScScanBind>();
	auto &state = input.global_state->Cast<ScScanState>();
	string err;
	auto count = sc::ScanNext(state.scan, err);
	if (count == 0) {
		output.SetCardinality(0);
		return;
	}
	idx_t guest_col = 0;
	for (idx_t c = 0; c < state.projection.size(); c++) {
		auto bind_col = state.projection[c];
		if (bind_col == DConstants::INVALID_INDEX) {
			// virtual column: nothing to read from the guest
			output.data[c].SetVectorType(VectorType::CONSTANT_VECTOR);
			ConstantVector::SetNull(output.data[c], true);
			continue;
		}
		auto c_src = guest_col++;
		auto mask = sc::ColValidity(state.scan, c_src);
		auto &type = bind.types[bind_col];
		// A column the guest rendered as text: stage it as VARCHAR, then let the HOST cast it.
		// If the host cannot perform that cast the query errors - it never guesses.
		bool via_text = bind.via_text[bind_col];
		if (via_text) {
			// reset reuses the vector but drops last chunk's strings
			state.staging[c]->Initialize(true, STANDARD_VECTOR_SIZE);
		}
		auto &vec = via_text ? *state.staging[c] : output.data[c];
		auto data = sc::ColData(state.scan, c_src);
		if (via_text || type.id() == LogicalTypeId::VARCHAR || type.id() == LogicalTypeId::BLOB) {
			auto out = FlatVector::GetData<string_t>(vec);
			for (idx_t i = 0; i < count; i++) {
				// the string_t of a NULL row is undefined - never dereference it
				if (mask && !(mask[i / 64] & (1ULL << (i % 64)))) {
					continue;
				}
				const char *ptr; uint64_t len;
				sc::ColString(state.scan, c_src, i, &ptr, &len);
				out[i] = StringVector::AddStringOrBlob(vec, ptr, len);
			}
		} else {
			// fixed-width physical types are laid out identically on both sides - the C API
			// documents this layout, so a raw copy is contract, not a layout guess
			memcpy(FlatVector::GetData(vec), data, GetTypeIdSize(type.InternalType()) * count);
		}
		if (mask) {
			auto &validity = FlatVector::Validity(vec);
			validity.EnsureWritable();
			for (idx_t i = 0; i < count; i++) {
				if (!(mask[i / 64] & (1ULL << (i % 64)))) { validity.SetInvalid(i); }
			}
		}
		if (via_text) {
			VectorOperations::Cast(context, *state.staging[c], output.data[c], count);
		}
	}
	output.SetCardinality(count);
}

//===--------------------------------------------------------------------===//
// db.storage_compat_tables() - the truthful view of what the file holds
//===--------------------------------------------------------------------===//
struct ScInfoRow {
	string schema;
	string table_name;
	string column_name;
	string guest_type; //! the type as DuckDB 2.0.0 itself names it
	string reason;     //! empty when readable
};

struct ScInfoSource : public TableFunctionInfo {
	explicit ScInfoSource(vector<ScInfoRow> &rows) : rows(rows) {
	}
	vector<ScInfoRow> &rows;
};

struct ScInfoBind : public TableFunctionData {
	vector<ScInfoRow> *rows;
};

struct ScInfoState : public GlobalTableFunctionState {
	idx_t offset = 0;
};

static unique_ptr<FunctionData> ScInfoBindFn(ClientContext &, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<string> &names) {
	names = {"schema_name", "table_name", "column_name", "guest_type", "readable", "reason"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::BOOLEAN, LogicalType::VARCHAR};
	auto bind = make_uniq<ScInfoBind>();
	bind->rows = &input.info->Cast<ScInfoSource>().rows;
	return std::move(bind);
}

static unique_ptr<GlobalTableFunctionState> ScInfoInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<ScInfoState>();
}

static void ScInfoFunc(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &bind = input.bind_data->Cast<ScInfoBind>();
	auto &state = input.global_state->Cast<ScInfoState>();
	auto &rows = *bind.rows;
	idx_t count = 0;
	while (state.offset < rows.size() && count < STANDARD_VECTOR_SIZE) {
		auto &r = rows[state.offset++];
		output.SetValue(0, count, Value(r.schema));
		output.SetValue(1, count, Value(r.table_name));
		output.SetValue(2, count, Value(r.column_name));
		output.SetValue(3, count, Value(r.guest_type));
		output.SetValue(4, count, Value::BOOLEAN(r.reason.empty()));
		output.SetValue(5, count, r.reason.empty() ? Value(LogicalType::VARCHAR) : Value(r.reason));
		count++;
	}
	output.SetCardinality(count);
}

//===--------------------------------------------------------------------===//
// catalog entries
//===--------------------------------------------------------------------===//
class ScCatalog;

class ScTableEntry : public TableCatalogEntry {
public:
	ScTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info, sc::Db *db, string sql_p,
	             vector<bool> via_text_p, vector<string> unsupported_p, string label_p,
	             string storage_version_p)
	    : TableCatalogEntry(catalog, schema, info), db(db), from_clause(std::move(sql_p)),
	      via_text(std::move(via_text_p)), unsupported_reason(std::move(unsupported_p)),
	      table_label(std::move(label_p)), storage_version(std::move(storage_version_p)) {
	}
	sc::Db *db;
	string from_clause;
	vector<bool> via_text;
	vector<string> unsupported_reason;
	string table_label;
	string storage_version;

	unique_ptr<BaseStatistics> GetStatistics(ClientContext &, column_t) override { return nullptr; }
	TableStorageInfo GetStorageInfo(ClientContext &) override { return TableStorageInfo(); }
	TableFunction GetScanFunction(ClientContext &, unique_ptr<FunctionData> &bind_data) override {
		auto bind = make_uniq<ScScanBind>();
		bind->db = db;
		bind->from_clause = from_clause;
		bind->via_text = via_text;
		bind->unsupported_reason = unsupported_reason;
		bind->table_label = table_label;
		bind->catalog_label = catalog.GetName();
		bind->storage_version = storage_version;
		for (auto &col : columns.Logical()) {
			bind->types.push_back(col.Type());
			bind->names.push_back(col.Name());
		}
		bind_data = std::move(bind);
		TableFunction fn("storage_compat_scan", {}, ScScanFunc, nullptr, ScScanInit);
		fn.projection_pushdown = true;
		return fn;
	}
};

class ScSchemaEntry : public SchemaCatalogEntry {
public:
	ScSchemaEntry(Catalog &catalog, CreateSchemaInfo &info) : SchemaCatalogEntry(catalog, info) {
	}
	case_insensitive_map_t<unique_ptr<CatalogEntry>> tables;
	unique_ptr<CatalogEntry> info_function;

	void Scan(ClientContext &, CatalogType type, const std::function<void(CatalogEntry &)> &cb) override {
		Scan(type, cb);
	}
	void Scan(CatalogType type, const std::function<void(CatalogEntry &)> &cb) override {
		if (type == CatalogType::TABLE_FUNCTION_ENTRY) {
			if (info_function) { cb(*info_function); }
			return;
		}
		if (type != CatalogType::TABLE_ENTRY) { return; }
		for (auto &e : tables) { cb(*e.second); }
	}
	optional_ptr<CatalogEntry> LookupEntry(CatalogTransaction, const EntryLookupInfo &lookup) override {
		if (lookup.GetCatalogType() == CatalogType::TABLE_FUNCTION_ENTRY) {
			if (info_function && StringUtil::CIEquals(lookup.GetEntryName(), "storage_compat_tables")) {
				return info_function.get();
			}
			return nullptr;
		}
		if (lookup.GetCatalogType() != CatalogType::TABLE_ENTRY) { return nullptr; }
		auto it = tables.find(lookup.GetEntryName());
		return it == tables.end() ? nullptr : it->second.get();
	}
	//! set by the catalog once it knows the attachment name and the file's format
	string readonly_msg = "storage_compat databases are read-only";
	void ReadOnly() const {
		throw NotImplementedException(readonly_msg);
	}
	optional_ptr<CatalogEntry> CreateIndex(CatalogTransaction, CreateIndexInfo &, TableCatalogEntry &) override { ReadOnly(); return nullptr; }
	optional_ptr<CatalogEntry> CreateFunction(CatalogTransaction, CreateFunctionInfo &) override { ReadOnly(); return nullptr; }
	optional_ptr<CatalogEntry> CreateTable(CatalogTransaction, BoundCreateTableInfo &) override { ReadOnly(); return nullptr; }
	optional_ptr<CatalogEntry> CreateView(CatalogTransaction, CreateViewInfo &) override { ReadOnly(); return nullptr; }
	optional_ptr<CatalogEntry> CreateSequence(CatalogTransaction, CreateSequenceInfo &) override { ReadOnly(); return nullptr; }
	optional_ptr<CatalogEntry> CreateTableFunction(CatalogTransaction, CreateTableFunctionInfo &) override { ReadOnly(); return nullptr; }
	optional_ptr<CatalogEntry> CreateCopyFunction(CatalogTransaction, CreateCopyFunctionInfo &) override { ReadOnly(); return nullptr; }
	optional_ptr<CatalogEntry> CreatePragmaFunction(CatalogTransaction, CreatePragmaFunctionInfo &) override { ReadOnly(); return nullptr; }
	optional_ptr<CatalogEntry> CreateCollation(CatalogTransaction, CreateCollationInfo &) override { ReadOnly(); return nullptr; }
	optional_ptr<CatalogEntry> CreateType(CatalogTransaction, CreateTypeInfo &) override { ReadOnly(); return nullptr; }
	void DropEntry(ClientContext &, DropInfo &) override { ReadOnly(); }
	void Alter(CatalogTransaction, AlterInfo &) override { ReadOnly(); }
};


//===--------------------------------------------------------------------===//
// transaction manager (read-only, trivial)
//===--------------------------------------------------------------------===//
class ScTransaction : public Transaction {
public:
	ScTransaction(TransactionManager &manager, ClientContext &context) : Transaction(manager, context) {
	}
};

class ScTransactionManager : public TransactionManager {
public:
	explicit ScTransactionManager(AttachedDatabase &db) : TransactionManager(db) {
	}
	Transaction &StartTransaction(ClientContext &context) override {
		auto tx = make_uniq<ScTransaction>(*this, context);
		auto &result = *tx;
		lock_guard<mutex> l(lock);
		transactions[result] = std::move(tx);
		return result;
	}
	ErrorData CommitTransaction(ClientContext &, Transaction &transaction) override {
		lock_guard<mutex> l(lock);
		transactions.erase(transaction);
		return ErrorData();
	}
	void RollbackTransaction(Transaction &transaction) override {
		lock_guard<mutex> l(lock);
		transactions.erase(transaction);
	}
	void Checkpoint(ClientContext &, bool) override {
	}

private:
	mutex lock;
	reference_map_t<Transaction, unique_ptr<ScTransaction>> transactions;
};

//===--------------------------------------------------------------------===//
// catalog
//===--------------------------------------------------------------------===//
class ScCatalog : public Catalog {
public:
	ScCatalog(AttachedDatabase &db_p, string path_p) : Catalog(db_p), path(std::move(path_p)) {
	}
	~ScCatalog() override {
		if (guest) { sc::Close(guest); }
	}

	string path;
	string readonly_msg = "storage_compat databases are read-only";
	sc::Db *guest = nullptr;
	//! storage version the FILE carries, for error messages
	string storage_version;
	case_insensitive_map_t<unique_ptr<ScSchemaEntry>> schemas;
	//! every column the guest has, readable or not - the source for storage_compat_tables()
	vector<ScInfoRow> info_rows;

	void Initialize(bool) override {
	}

	void LoadSchema(ClientContext &context) {
		string err;
		guest = sc::Open(path, "", err);
		if (!guest) {
			throw IOException("storage_compat: could not open '%s': %s", path, err);
		}
		storage_version = sc::StorageVersion(guest);
		auto tables = sc::ListTables(guest, err);
		if (!err.empty()) {
			throw IOException("storage_compat: %s", err);
		}
		for (auto &td : tables) {
			if (!sc::DescribeTable(guest, td, err)) {
				continue; // not readable through the guest planner - skip rather than fail the ATTACH
			}
			auto &schema = GetOrCreateSchema(td.schema);
			CreateTableInfo info(INVALID_CATALOG, td.schema, td.name);
			vector<bool> via_text;
			vector<string> unsupported_reason;
			for (auto &col : td.columns) {
				auto native = MapPType(col.ptype);
				LogicalType type;
				bool text = false;
				string reason;
				if (native.id() != LogicalTypeId::INVALID) {
					type = native;
				} else {
					// No native transfer for this physical type. Ask the host to parse the
					// guest's own rendering of it; if the host cannot represent that type at
					// all, the table is not exposed as readable - we never invent a type.
					try {
						type = TransformStringToLogicalType(col.type_name, context);
					} catch (const std::exception &) {
						// Keep the column VISIBLE so the table still shows up in
						// SHOW ALL TABLES / .tables. The placeholder must be a real
						// LogicalType, so we alias it: the catalog then names the guest
						// type instead of silently claiming VARCHAR, and reading it is
						// still refused below.
						type = LogicalType::VARCHAR;
						type.SetAlias("UNSUPPORTED(" + col.type_name + ")");
						reason = "column \"" + col.name + "\" has type " + col.type_name +
						         ", which this DuckDB version cannot represent";
					}
					text = reason.empty();
				}
				via_text.push_back(text);
				unsupported_reason.push_back(reason);
				info_rows.push_back(ScInfoRow {td.schema, td.name, col.name, col.type_name, reason});
				info.columns.AddColumn(ColumnDefinition(col.name, type));
			}
			string from_clause = "\"" + td.schema + "\".\"" + td.name + "\"";
			auto entry = make_uniq<ScTableEntry>(*this, schema, info, guest, from_clause, via_text,
			                                     unsupported_reason, td.schema + "." + td.name,
			                                     storage_version);
			schema.tables[td.name] = std::move(entry);
		}
		BuildReadOnlyMessage();
		RegisterInfoFunction();
	}

	void BuildReadOnlyMessage() {
		readonly_msg = StringUtil::Format("storage_compat: \"%s\" is attached read-only.\n%s", GetName(),
		                                  VersionHint(GetName(), storage_version));
		for (auto &s : schemas) {
			s.second->readonly_msg = readonly_msg;
		}
	}

	//! expose db.storage_compat_tables() inside every schema of THIS attached database
	void RegisterInfoFunction() {
		for (auto &s : schemas) {
			TableFunction fn("storage_compat_tables", {}, ScInfoFunc, ScInfoBindFn, ScInfoInit);
			fn.function_info = make_shared_ptr<ScInfoSource>(info_rows);
			CreateTableFunctionInfo cinfo(fn);
			cinfo.schema = s.first;
			s.second->info_function = make_uniq<TableFunctionCatalogEntry>(*this, *s.second, cinfo);
		}
	}

	ScSchemaEntry &GetOrCreateSchema(const string &name) {
		auto it = schemas.find(name);
		if (it != schemas.end()) {
			return *it->second;
		}
		CreateSchemaInfo info;
		info.schema = name;
		auto entry = make_uniq<ScSchemaEntry>(*this, info);
		auto &result = *entry;
		schemas[name] = std::move(entry);
		return result;
	}

	string GetCatalogType() override {
		return "storage_compat";
	}
	optional_ptr<CatalogEntry> CreateSchema(CatalogTransaction, CreateSchemaInfo &) override {
		throw NotImplementedException(readonly_msg);
	}
	optional_ptr<SchemaCatalogEntry> LookupSchema(CatalogTransaction, const EntryLookupInfo &lookup,
	                                              OnEntryNotFound if_not_found) override {
		auto it = schemas.find(lookup.GetEntryName());
		if (it != schemas.end()) {
			return it->second.get();
		}
		if (if_not_found == OnEntryNotFound::RETURN_NULL) {
			return nullptr;
		}
		throw CatalogException("Schema \"%s\" not found in storage_compat database", lookup.GetEntryName());
	}
	void ScanSchemas(ClientContext &, std::function<void(SchemaCatalogEntry &)> callback) override {
		for (auto &s : schemas) {
			callback(*s.second);
		}
	}
	PhysicalOperator &PlanCreateTableAs(ClientContext &, PhysicalPlanGenerator &, LogicalCreateTable &,
	                                    PhysicalOperator &) override {
		throw NotImplementedException(readonly_msg);
	}
	PhysicalOperator &PlanInsert(ClientContext &, PhysicalPlanGenerator &, LogicalInsert &,
	                             optional_ptr<PhysicalOperator>) override {
		throw NotImplementedException(readonly_msg);
	}
	PhysicalOperator &PlanDelete(ClientContext &, PhysicalPlanGenerator &, LogicalDelete &,
	                             PhysicalOperator &) override {
		throw NotImplementedException(readonly_msg);
	}
	PhysicalOperator &PlanUpdate(ClientContext &, PhysicalPlanGenerator &, LogicalUpdate &,
	                             PhysicalOperator &) override {
		throw NotImplementedException(readonly_msg);
	}
	DatabaseSize GetDatabaseSize(ClientContext &) override {
		DatabaseSize size;
		return size;
	}
	bool InMemory() override {
		return false;
	}
	string GetDBPath() override {
		return path;
	}
	void DropSchema(ClientContext &, DropInfo &) override {
		throw NotImplementedException(readonly_msg);
	}
};

//===--------------------------------------------------------------------===//
// storage extension + entry points
//===--------------------------------------------------------------------===//
static unique_ptr<Catalog> ScAttach(optional_ptr<StorageExtensionInfo>, ClientContext &context, AttachedDatabase &db,
                                    const string &, AttachInfo &info, AttachOptions &) {
	auto catalog = make_uniq<ScCatalog>(db, info.path);
	catalog->LoadSchema(context);
	return std::move(catalog);
}

static unique_ptr<TransactionManager> ScCreateTransactionManager(optional_ptr<StorageExtensionInfo>,
                                                                 AttachedDatabase &db, Catalog &) {
	return make_uniq<ScTransactionManager>(db);
}

class ScStorageExtension : public StorageExtension {
public:
	ScStorageExtension() {
		attach = ScAttach;
		create_transaction_manager = ScCreateTransactionManager;
	}
};

static void LoadInternal(ExtensionLoader &loader) {
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	StorageExtension::Register(config, "storage_compat", make_shared_ptr<ScStorageExtension>());
}

} // namespace duckdb

extern "C" {
DUCKDB_EXTENSION_API void storage_compat_duckdb_cpp_init(duckdb::ExtensionLoader &loader) {
	duckdb::LoadInternal(loader);
}
DUCKDB_EXTENSION_API const char *storage_compat_version() {
	return duckdb::DuckDB::LibraryVersion();
}
}
