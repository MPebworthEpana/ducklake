#include "storage/ducklake_metadata_manager.hpp"
#include "storage/ducklake_transaction.hpp"
#include "storage/ducklake_variant_stats.hpp"
#include "common/ducklake_util.hpp"
#include "duckdb/planner/tableref/bound_at_clause.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/common/sql_identifier.hpp"
#include "duckdb/common/type_visitor.hpp"
#include "storage/ducklake_catalog.hpp"
#include "common/ducklake_types.hpp"
#include "storage/ducklake_schema_entry.hpp"
#include "storage/ducklake_table_entry.hpp"
#include "duckdb.hpp"
#include "duckdb/main/appender.hpp"
#include "metadata_manager/postgres_metadata_manager.hpp"
#include "metadata_manager/quack_metadata_manager.hpp"
#include "metadata_manager/sqlite_metadata_manager.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "storage/ducklake_partition_data.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/common/sql_identifier.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"

#include <algorithm>
#include <tuple>

namespace duckdb {

DuckLakeMetadataManager::DuckLakeMetadataManager(DuckLakeTransaction &transaction) : transaction(transaction) {
}

DuckLakeMetadataManager::~DuckLakeMetadataManager() {
}
optional_ptr<AttachedDatabase> GetDatabase(ClientContext &context, const string &name);

unordered_map<string /* name */, DuckLakeMetadataManager::create_t> DuckLakeMetadataManager::metadata_managers = {
    {"postgres", PostgresMetadataManager::Create}, {"postgres_scanner", PostgresMetadataManager::Create},
    {"quack", QuackMetadataManager::Create},       {"quack_scanner", QuackMetadataManager::Create},
    {"sqlite", SQLiteMetadataManager::Create},     {"sqlite_scanner", SQLiteMetadataManager::Create}};

mutex DuckLakeMetadataManager::metadata_managers_lock;

void DuckLakeMetadataManager::Register(const string &name, DuckLakeMetadataManager::create_t create) {
	lock_guard<mutex> lock(metadata_managers_lock);
	if (metadata_managers.find(name) != metadata_managers.end()) {
		throw InternalException("Metadata manager with name \"%s\" already exists!", name);
	}
	metadata_managers[name] = create;
}

unique_ptr<DuckLakeMetadataManager> DuckLakeMetadataManager::Create(DuckLakeTransaction &transaction) {
	lock_guard<mutex> lock(metadata_managers_lock);
	auto &catalog = transaction.GetCatalog();
	auto catalog_type = catalog.MetadataType();
	auto metadata_manager_iter = metadata_managers.find(catalog_type);
	if (metadata_manager_iter != metadata_managers.end()) {
		auto create = metadata_manager_iter->second;
		return create(transaction);
	}
	if (catalog_type == "sqlite_scanner") {
		return make_uniq<SQLiteMetadataManager>(transaction);
	}
	return make_uniq<DuckLakeMetadataManager>(transaction);
}

bool DuckLakeMetadataManager::ExecuteRetrialsServerSide() const {
	return transaction.GetCatalog().RetrialsServerSide();
}

bool DuckLakeMetadataManager::CanSkipSnapshotFetch(const TransactionChangeInformation &) const {
	return false;
}

void DuckLakeMetadataManager::FlushChangesServerSide(DuckLakeTransaction &, DuckLakeSnapshot,
                                                     const TransactionChangeInformation &,
                                                     const DuckLakeRetryConfig &) {
	throw InternalException("FlushChangesServerSide invoked on a metadata backend without server-side commit support");
}

DuckLakeMetadataManager &DuckLakeMetadataManager::Get(DuckLakeTransaction &transaction) {
	return transaction.GetMetadataManager();
}

bool DuckLakeMetadataManager::TypeIsNativelySupported(const LogicalType &type) {
	return true;
}

bool DuckLakeMetadataManager::SupportsInlining(const LogicalType &type) {
	if (type.id() == LogicalTypeId::GEOMETRY) {
		return false;
	}
	return true;
}

bool DuckLakeMetadataManager::SupportsInliningColumns(const vector<DuckLakeColumnInfo> &columns) {
	for (auto &col : columns) {
		auto col_type = DuckLakeTypes::FromString(col.type);
		if (!SupportsInlining(col_type)) {
			return false;
		}
		if (!col.children.empty() && !SupportsInliningColumns(col.children)) {
			return false;
		}
	}
	return true;
}

bool DuckLakeMetadataManager::CanInlineColumns(const ColumnList &columns) {
	auto max_identifier_length = MaxIdentifierLength();
	for (auto &col : columns.Logical()) {
		if (DuckLakeUtil::IsInlinedSystemColumn(col.Name().GetIdentifierName())) {
			return false;
		}
		if (col.Name().size() > max_identifier_length) {
			return false;
		}
		if (TypeVisitor::Contains(col.Type(), [&](const LogicalType &t) { return !SupportsInlining(t); })) {
			return false;
		}
	}
	return true;
}

bool DuckLakeMetadataManager::CanInlineColumns(const vector<DuckLakeColumnInfo> &columns) {
	auto max_identifier_length = MaxIdentifierLength();
	for (auto &col : columns) {
		if (DuckLakeUtil::IsInlinedSystemColumn(col.name)) {
			return false;
		}
		if (col.name.size() > max_identifier_length) {
			return false;
		}
	}
	return SupportsInliningColumns(columns);
}

FileSystem &DuckLakeMetadataManager::GetFileSystem() {
	return FileSystem::GetFileSystem(transaction.GetCatalog().GetDatabase());
}

string DuckLakeMetadataManager::ListAggregation(const vector<pair<string, string>> &fields) {
	// DuckDB syntax: LIST({'key1': val1, 'key2': val2, ...})
	string fields_part;
	for (auto const &entry : fields) {
		if (!fields_part.empty()) {
			fields_part += ", ";
		}
		fields_part += "'" + entry.first + "': " + entry.second;
	}
	return "LIST({" + fields_part + "})";
}

unique_ptr<QueryResult> DuckLakeMetadataManager::AttachMetadata(const string &attach_query) {
	auto query = attach_query;
	SubstituteCatalogPlaceholders(query);
	return transaction.ExecuteRaw(query);
}

string DuckLakeMetadataManager::MetadataExistsQuery() const {
	return "SELECT NULL FROM {METADATA_CATALOG}.ducklake_metadata LIMIT 1";
}

bool DuckLakeMetadataManager::MetadataExists() {
	auto query = MetadataExistsQuery();
	auto result = Query(query);
	if (result->HasError()) {
		auto &error_obj = result->GetErrorObject();
		if (error_obj.Type() == ExceptionType::CATALOG) {
			// Catalog/schema/table missing means we are attaching a fresh DuckLake.
			return false;
		}
		error_obj.Throw("Failed to probe DuckLake metadata: ");
	}
	return true;
}

void DuckLakeMetadataManager::InitializeDuckLake(bool has_explicit_schema, DuckLakeEncryption encryption) {
	string initialize_query;
	if (has_explicit_schema) {
		// if the schema is user provided create it
		initialize_query += "CREATE SCHEMA IF NOT EXISTS {METADATA_CATALOG};\n";
	}
	initialize_query += GetCreateTableStatements();

	// insert initial data
	auto &ducklake_catalog = transaction.GetCatalog();
	auto &base_data_path = ducklake_catalog.DataPath();
	string data_path = StorePath(base_data_path);
	string encryption_str = encryption == DuckLakeEncryption::ENCRYPTED ? "true" : "false";
	string initial_schema_uuid = transaction.GenerateUUID();
	initialize_query += StringUtil::Format(R"(
INSERT INTO {METADATA_CATALOG}.ducklake_snapshot(snapshot_id, snapshot_time, schema_version, next_catalog_id, next_file_id) VALUES (0, NOW(), 0, 1, 0);
INSERT INTO {METADATA_CATALOG}.ducklake_snapshot_changes VALUES (0, 'created_schema:"main"',  NULL, NULL, NULL);
INSERT INTO {METADATA_CATALOG}.ducklake_metadata (key, value) VALUES ('version', '%s'), ('created_by', 'DuckDB %s'), ('data_path', %s), ('encrypted', '%s');
INSERT INTO {METADATA_CATALOG}.ducklake_schema(schema_id, schema_uuid, begin_snapshot, end_snapshot, schema_name, path, path_is_relative) VALUES (0, '%s'::UUID, 0, NULL, 'main', 'main/', true);
	)",
	                                       GetVersionString(), DuckDB::SourceID(), SQLString(data_path), encryption_str,
	                                       initial_schema_uuid);
	auto result = Execute(initialize_query);
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to initialize DuckLake: ");
	}
}

string DuckLakeMetadataManager::GetDataFileTableStatement() {
	return "CREATE TABLE {METADATA_CATALOG}.ducklake_data_file(data_file_id BIGINT PRIMARY KEY, table_id BIGINT, "
	       "begin_snapshot BIGINT, end_snapshot BIGINT, file_order BIGINT, path VARCHAR, path_is_relative BOOLEAN, "
	       "file_format VARCHAR, record_count BIGINT, file_size_bytes BIGINT, footer_size BIGINT, row_id_start BIGINT, "
	       "partition_id BIGINT, encryption_key VARCHAR,  mapping_id BIGINT, partial_max BIGINT);";
}

string DuckLakeMetadataManager::GetDeleteFileTableStatement() {
	return "CREATE TABLE {METADATA_CATALOG}.ducklake_delete_file(delete_file_id BIGINT PRIMARY KEY, table_id BIGINT, "
	       "begin_snapshot BIGINT, end_snapshot BIGINT, data_file_id BIGINT, path VARCHAR, path_is_relative BOOLEAN, "
	       "format VARCHAR, delete_count BIGINT, file_size_bytes BIGINT, footer_size BIGINT, encryption_key VARCHAR, "
	       "partial_max BIGINT);";
}

string DuckLakeMetadataManager::GetCreateTableStatements() {
	vector<string> statements;
	statements.push_back("CREATE TABLE {METADATA_CATALOG}.ducklake_metadata(key VARCHAR NOT NULL, value VARCHAR NOT "
	                     "NULL, scope VARCHAR, scope_id BIGINT);");
	statements.push_back(
	    "CREATE TABLE {METADATA_CATALOG}.ducklake_snapshot(snapshot_id BIGINT PRIMARY KEY, snapshot_time TIMESTAMPTZ, "
	    "schema_version BIGINT, next_catalog_id BIGINT, next_file_id BIGINT);");
	statements.push_back("CREATE TABLE {METADATA_CATALOG}.ducklake_snapshot_changes(snapshot_id BIGINT PRIMARY KEY, "
	                     "changes_made VARCHAR, author VARCHAR, commit_message VARCHAR, commit_extra_info VARCHAR);");
	statements.push_back(
	    "CREATE TABLE {METADATA_CATALOG}.ducklake_schema(schema_id BIGINT, schema_uuid UUID, "
	    "begin_snapshot BIGINT, end_snapshot BIGINT, schema_name VARCHAR, path VARCHAR, path_is_relative BOOLEAN);");
	statements.push_back(
	    "CREATE TABLE {METADATA_CATALOG}.ducklake_table(table_id BIGINT, table_uuid UUID, begin_snapshot BIGINT, "
	    "end_snapshot BIGINT, schema_id BIGINT, table_name VARCHAR, path VARCHAR, path_is_relative BOOLEAN);");
	statements.push_back("CREATE TABLE {METADATA_CATALOG}.ducklake_view(view_id BIGINT, view_uuid UUID, begin_snapshot "
	                     "BIGINT, end_snapshot BIGINT, schema_id BIGINT, view_name VARCHAR, dialect VARCHAR, sql "
	                     "VARCHAR, column_aliases VARCHAR);");
	statements.push_back("CREATE TABLE {METADATA_CATALOG}.ducklake_tag(object_id BIGINT, begin_snapshot BIGINT, "
	                     "end_snapshot BIGINT, key VARCHAR, value VARCHAR);");
	statements.push_back("CREATE TABLE {METADATA_CATALOG}.ducklake_column_tag(table_id BIGINT, column_id BIGINT, "
	                     "begin_snapshot BIGINT, end_snapshot BIGINT, key VARCHAR, value VARCHAR);");
	statements.push_back(GetDataFileTableStatement());
	statements.push_back("CREATE TABLE {METADATA_CATALOG}.ducklake_file_column_stats(data_file_id BIGINT, table_id "
	                     "BIGINT, column_id BIGINT, column_size_bytes BIGINT, value_count BIGINT, null_count BIGINT, "
	                     "min_value VARCHAR, max_value VARCHAR, contains_nan BOOLEAN, extra_stats VARCHAR);");
	statements.push_back(
	    "CREATE TABLE {METADATA_CATALOG}.ducklake_file_variant_stats(data_file_id BIGINT, table_id BIGINT, column_id "
	    "BIGINT, variant_path VARCHAR, shredded_type VARCHAR, column_size_bytes BIGINT, value_count BIGINT, null_count "
	    "BIGINT, min_value VARCHAR, max_value VARCHAR, contains_nan BOOLEAN, extra_stats VARCHAR);");
	statements.push_back(GetDeleteFileTableStatement());
	statements.push_back("CREATE TABLE {METADATA_CATALOG}.ducklake_column(column_id BIGINT, begin_snapshot BIGINT, "
	                     "end_snapshot BIGINT, table_id BIGINT, column_order BIGINT, column_name VARCHAR, column_type "
	                     "VARCHAR, initial_default VARCHAR, default_value VARCHAR, nulls_allowed BOOLEAN, "
	                     "parent_column BIGINT, default_value_type VARCHAR, default_value_dialect VARCHAR);");
	statements.push_back("CREATE TABLE {METADATA_CATALOG}.ducklake_table_stats(table_id BIGINT, record_count BIGINT, "
	                     "next_row_id BIGINT, file_size_bytes BIGINT);");
	statements.push_back(
	    "CREATE TABLE {METADATA_CATALOG}.ducklake_table_column_stats(table_id BIGINT, column_id BIGINT, contains_null "
	    "BOOLEAN, contains_nan BOOLEAN, min_value VARCHAR, max_value VARCHAR, extra_stats VARCHAR);");
	statements.push_back("CREATE TABLE {METADATA_CATALOG}.ducklake_partition_info(partition_id BIGINT, table_id "
	                     "BIGINT, begin_snapshot BIGINT, end_snapshot BIGINT);");
	statements.push_back("CREATE TABLE {METADATA_CATALOG}.ducklake_partition_column(partition_id BIGINT, table_id "
	                     "BIGINT, partition_key_index BIGINT, column_id BIGINT, transform VARCHAR);");
	statements.push_back("CREATE TABLE {METADATA_CATALOG}.ducklake_file_partition_value(data_file_id BIGINT, table_id "
	                     "BIGINT, partition_key_index BIGINT, partition_value VARCHAR);");
	statements.push_back("CREATE TABLE {METADATA_CATALOG}.ducklake_files_scheduled_for_deletion(data_file_id BIGINT, "
	                     "path VARCHAR, path_is_relative BOOLEAN, schedule_start TIMESTAMPTZ);");
	statements.push_back("CREATE TABLE {METADATA_CATALOG}.ducklake_inlined_data_tables(table_id BIGINT, table_name "
	                     "VARCHAR, schema_version BIGINT);");
	statements.push_back(
	    "CREATE TABLE {METADATA_CATALOG}.ducklake_column_mapping(mapping_id BIGINT, table_id BIGINT, type VARCHAR);");
	statements.push_back("CREATE TABLE {METADATA_CATALOG}.ducklake_name_mapping(mapping_id BIGINT, column_id BIGINT, "
	                     "source_name VARCHAR, target_field_id BIGINT, parent_column BIGINT, is_partition BOOLEAN);");
	statements.push_back("CREATE TABLE {METADATA_CATALOG}.ducklake_schema_versions(begin_snapshot BIGINT, "
	                     "schema_version BIGINT, table_id BIGINT);");
	statements.push_back("CREATE TABLE {METADATA_CATALOG}.ducklake_macro(schema_id BIGINT, macro_id BIGINT, macro_name "
	                     "VARCHAR, begin_snapshot BIGINT, end_snapshot BIGINT);");
	statements.push_back("CREATE TABLE {METADATA_CATALOG}.ducklake_macro_impl(macro_id BIGINT, impl_id BIGINT, dialect "
	                     "VARCHAR, sql VARCHAR, type VARCHAR);");
	statements.push_back(
	    "CREATE TABLE {METADATA_CATALOG}.ducklake_macro_parameters(macro_id BIGINT, impl_id BIGINT,column_id BIGINT, "
	    "parameter_name VARCHAR, parameter_type VARCHAR, default_value VARCHAR, default_value_type VARCHAR);");
	statements.push_back("CREATE TABLE {METADATA_CATALOG}.ducklake_sort_info(sort_id BIGINT, table_id BIGINT, "
	                     "begin_snapshot BIGINT, end_snapshot BIGINT);");
	statements.push_back(
	    "CREATE TABLE {METADATA_CATALOG}.ducklake_sort_expression(sort_id BIGINT, table_id BIGINT, sort_key_index "
	    "BIGINT, expression VARCHAR, dialect VARCHAR, sort_direction VARCHAR, null_order VARCHAR);");
	string result = "\n";
	for (auto &statement : statements) {
		result += statement + "\n";
	}
	return result;
}

string DuckLakeMetadataManager::GetVersionString() {
	constexpr auto VERSION = DuckLakeVersion::V1_0;
	return DuckLakeVersionToString(VERSION);
}

void DuckLakeMetadataManager::MigrateV01() {
	string migrate_query = R"(
ALTER TABLE {METADATA_CATALOG}.ducklake_schema ADD COLUMN path VARCHAR DEFAULT '';
ALTER TABLE {METADATA_CATALOG}.ducklake_schema ADD COLUMN path_is_relative BOOLEAN DEFAULT TRUE;
ALTER TABLE {METADATA_CATALOG}.ducklake_table ADD COLUMN path VARCHAR DEFAULT '';
ALTER TABLE {METADATA_CATALOG}.ducklake_table ADD COLUMN path_is_relative BOOLEAN DEFAULT TRUE;
ALTER TABLE {METADATA_CATALOG}.ducklake_metadata ADD COLUMN scope VARCHAR;
ALTER TABLE {METADATA_CATALOG}.ducklake_metadata ADD COLUMN scope_id BIGINT;
ALTER TABLE {METADATA_CATALOG}.ducklake_data_file ADD COLUMN mapping_id BIGINT;
CREATE TABLE {METADATA_CATALOG}.ducklake_column_mapping(mapping_id BIGINT, table_id BIGINT, type VARCHAR);
CREATE TABLE {METADATA_CATALOG}.ducklake_name_mapping(mapping_id BIGINT, column_id BIGINT, source_name VARCHAR, target_field_id BIGINT, parent_column BIGINT);
UPDATE {METADATA_CATALOG}.ducklake_partition_column SET column_id = (SELECT LIST(column_id ORDER BY column_order) FROM {METADATA_CATALOG}.ducklake_column WHERE table_id = ducklake_partition_column.table_id AND parent_column IS NULL AND end_snapshot IS NULL)[ducklake_partition_column.column_id + 1];
UPDATE {METADATA_CATALOG}.ducklake_metadata SET value = '0.2' WHERE key = 'version';
	)";
	auto result = Execute(migrate_query);
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to migrate DuckLake from v0.1 to v0.2: ");
	}
}

void DuckLakeMetadataManager::ExecuteMigration(string migrate_query, bool allow_failures, const string &from_version,
                                               const string &to_version) {
	if (allow_failures) {
		migrate_query = StringUtil::Replace(migrate_query, "{IF_NOT_EXISTS}", "IF NOT EXISTS");
		migrate_query = StringUtil::Replace(migrate_query, "{IF_EXISTS}", "IF EXISTS");
		migrate_query =
		    StringUtil::Replace(migrate_query, "{WHERE_EMPTY}",
		                        "WHERE NOT EXISTS (SELECT 1 FROM {METADATA_CATALOG}.ducklake_schema_versions);");
	} else {
		// All our place-holders are empty, so if any of these exist it will fail
		migrate_query = StringUtil::Replace(migrate_query, "{IF_NOT_EXISTS}", "");
		migrate_query = StringUtil::Replace(migrate_query, "{IF_EXISTS}", "");
		migrate_query = StringUtil::Replace(migrate_query, "{WHERE_EMPTY}", "");
	}
	auto result = Execute(migrate_query);
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to migrate DuckLake from v" + from_version + " to v" + to_version + ":");
	}
}

void DuckLakeMetadataManager::MigrateV02(bool allow_failures) {
	string migrate_query = R"(
ALTER TABLE {METADATA_CATALOG}.ducklake_name_mapping ADD COLUMN {IF_NOT_EXISTS} is_partition BOOLEAN DEFAULT false;
ALTER TABLE {METADATA_CATALOG}.ducklake_snapshot_changes ADD COLUMN {IF_NOT_EXISTS} author VARCHAR DEFAULT NULL;
ALTER TABLE {METADATA_CATALOG}.ducklake_snapshot_changes ADD COLUMN {IF_NOT_EXISTS} commit_message VARCHAR DEFAULT NULL;
ALTER TABLE {METADATA_CATALOG}.ducklake_snapshot_changes ADD COLUMN {IF_NOT_EXISTS} commit_extra_info VARCHAR DEFAULT NULL;
UPDATE {METADATA_CATALOG}.ducklake_metadata SET value = '0.3' WHERE key = 'version';
CREATE TABLE {IF_NOT_EXISTS} {METADATA_CATALOG}.ducklake_schema_versions(begin_snapshot BIGINT, schema_version BIGINT);
INSERT INTO {METADATA_CATALOG}.ducklake_schema_versions SELECT * FROM (SELECT MIN(snapshot_id), schema_version FROM {METADATA_CATALOG}.ducklake_snapshot GROUP BY schema_version ORDER BY schema_version) t {WHERE_EMPTY};
ALTER TABLE {IF_EXISTS} {METADATA_CATALOG}.ducklake_file_column_statistics RENAME TO ducklake_file_column_stats;
ALTER TABLE {METADATA_CATALOG}.ducklake_file_column_stats ADD COLUMN {IF_NOT_EXISTS} extra_stats VARCHAR DEFAULT NULL;
ALTER TABLE {METADATA_CATALOG}.ducklake_table_column_stats ADD COLUMN {IF_NOT_EXISTS} extra_stats VARCHAR DEFAULT NULL;
	)";
	ExecuteMigration(migrate_query, allow_failures, "0.2", "0.3");
}

void DuckLakeMetadataManager::MigrateV03(bool allow_failures) {
	string migrate_query = R"(
CREATE TABLE {IF_NOT_EXISTS} {METADATA_CATALOG}.ducklake_macro(schema_id BIGINT, macro_id BIGINT, macro_name VARCHAR, begin_snapshot BIGINT, end_snapshot BIGINT);
CREATE TABLE {IF_NOT_EXISTS} {METADATA_CATALOG}.ducklake_macro_impl(macro_id BIGINT, impl_id BIGINT, dialect VARCHAR, sql VARCHAR, type VARCHAR);
CREATE TABLE {IF_NOT_EXISTS} {METADATA_CATALOG}.ducklake_macro_parameters(macro_id BIGINT, impl_id BIGINT,column_id BIGINT, parameter_name VARCHAR, parameter_type VARCHAR, default_value VARCHAR, default_value_type VARCHAR);
ALTER TABLE {METADATA_CATALOG}.ducklake_column ADD COLUMN {IF_NOT_EXISTS} default_value_type VARCHAR DEFAULT 'literal';
UPDATE {METADATA_CATALOG}.ducklake_column SET default_value_type = 'literal' WHERE default_value_type IS NULL;
ALTER TABLE {METADATA_CATALOG}.ducklake_column ADD COLUMN {IF_NOT_EXISTS} default_value_dialect VARCHAR DEFAULT NULL;
CREATE TABLE {IF_NOT_EXISTS} {METADATA_CATALOG}.ducklake_sort_info(sort_id BIGINT, table_id BIGINT, begin_snapshot BIGINT, end_snapshot BIGINT);
CREATE TABLE {IF_NOT_EXISTS} {METADATA_CATALOG}.ducklake_sort_expression(sort_id BIGINT, table_id BIGINT, sort_key_index BIGINT, expression VARCHAR, dialect VARCHAR, sort_direction VARCHAR, null_order VARCHAR);
CREATE TABLE {IF_NOT_EXISTS} {METADATA_CATALOG}.ducklake_file_variant_stats(data_file_id BIGINT, table_id BIGINT, column_id BIGINT, variant_path VARCHAR, shredded_type VARCHAR, column_size_bytes BIGINT, value_count BIGINT, null_count BIGINT, min_value VARCHAR, max_value VARCHAR, contains_nan BOOLEAN, extra_stats VARCHAR);
ALTER TABLE {METADATA_CATALOG}.ducklake_schema_versions ADD COLUMN {IF_NOT_EXISTS} table_id BIGINT;
ALTER TABLE {METADATA_CATALOG}.ducklake_data_file ADD COLUMN {IF_NOT_EXISTS} partial_max BIGINT;
ALTER TABLE {METADATA_CATALOG}.ducklake_delete_file ADD COLUMN {IF_NOT_EXISTS} partial_max BIGINT;
CREATE TEMP TABLE {IF_NOT_EXISTS} __ducklake_partial_max_migration AS
SELECT data_file_id, TRY_CAST(regexp_extract(partial_file_info, 'partial_max:(\d+)', 1) AS BIGINT) AS partial_max
FROM {METADATA_CATALOG}.ducklake_data_file
WHERE partial_file_info IS NOT NULL AND partial_file_info LIKE '%partial_max:%';
ALTER TABLE {METADATA_CATALOG}.ducklake_data_file DROP COLUMN {IF_EXISTS} partial_file_info;
UPDATE {METADATA_CATALOG}.ducklake_data_file AS df
SET partial_max = m.partial_max
FROM __ducklake_partial_max_migration m
WHERE df.data_file_id = m.data_file_id;
DROP TABLE IF EXISTS __ducklake_partial_max_migration;
UPDATE {METADATA_CATALOG}.ducklake_metadata SET value = '0.4' WHERE key = 'version';
	)";
	ExecuteMigration(migrate_query, allow_failures, "0.3", "0.4");

	auto migrate_schema_versions = Execute(R"(
INSERT INTO {METADATA_CATALOG}.ducklake_schema_versions (table_id, begin_snapshot, schema_version)
SELECT t.table_id, sv.begin_snapshot, sv.schema_version
FROM {METADATA_CATALOG}.ducklake_schema_versions sv
JOIN {METADATA_CATALOG}.ducklake_table t
  ON sv.begin_snapshot BETWEEN t.begin_snapshot
                           AND COALESCE(t.end_snapshot, sv.begin_snapshot)
WHERE sv.table_id IS NULL;
)");
	if (migrate_schema_versions->HasError()) {
		if (!allow_failures) {
			migrate_schema_versions->GetErrorObject().Throw(
			    "Failed to migrate schema_versions to per-table tracking: ");
		}
	}
	auto delete_global_entries = Execute(R"(
DELETE FROM {METADATA_CATALOG}.ducklake_schema_versions WHERE table_id IS NULL;
)");
	if (delete_global_entries->HasError()) {
		if (!allow_failures) {
			delete_global_entries->GetErrorObject().Throw("Failed to clean up global schema_versions entries: ");
		}
	}
}

void DuckLakeMetadataManager::MigrateV04() {
	auto result = Execute(R"(
UPDATE {METADATA_CATALOG}.ducklake_metadata SET value = '1.0' WHERE key = 'version';
	)");
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to migrate DuckLake from v0.4 to v1.0: ");
	}
}

void DuckLakeMetadataManager::MigrateV10(bool allow_failures) {
	string migrate_query = R"(
ALTER TABLE {METADATA_CATALOG}.ducklake_data_file ADD COLUMN {IF_NOT_EXISTS} row_group_count BIGINT;
ALTER TABLE {METADATA_CATALOG}.ducklake_delete_file ADD COLUMN {IF_NOT_EXISTS} row_group_count BIGINT;
CREATE TABLE {IF_NOT_EXISTS} {METADATA_CATALOG}.ducklake_view_column_tag(
	view_id BIGINT, column_name VARCHAR, begin_snapshot BIGINT, end_snapshot BIGINT, key VARCHAR, value VARCHAR
);
UPDATE {METADATA_CATALOG}.ducklake_metadata SET value = '1.1-dev1' WHERE key = 'version';
	)";
	ExecuteMigration(migrate_query, allow_failures, "1.0", "1.1-dev1");
}

void DuckLakeMetadataManager::MigrateV11(bool allow_failures) {
	string migrate_query = R"(
CREATE TABLE {IF_NOT_EXISTS} {METADATA_CATALOG}.ducklake_ref(
	ref_id BIGINT PRIMARY KEY,
	ref_name VARCHAR,
	ref_type VARCHAR,
	snapshot_id BIGINT,
	parent_ref_id BIGINT,
	status VARCHAR,
	created_at TIMESTAMPTZ
);
UPDATE {METADATA_CATALOG}.ducklake_metadata SET value = '1.1-dev2' WHERE key = 'version';
	)";
	ExecuteMigration(migrate_query, allow_failures, "1.1-dev1", "1.1-dev2");
}

void DuckLakeMetadataManager::MigrateV12(bool allow_failures) {
	// Phase 2 writable branches: snapshot DAG, lineage, tombstones, branch_id columns.
	// DEFAULT 0 keeps existing rows valid as branch "main".
	string migrate_query = R"(
ALTER TABLE {METADATA_CATALOG}.ducklake_snapshot ADD COLUMN {IF_NOT_EXISTS} branch_id BIGINT DEFAULT 0;
CREATE TABLE {IF_NOT_EXISTS} {METADATA_CATALOG}.ducklake_branch_lineage(
	branch_id BIGINT,
	ancestor_branch_id BIGINT,
	max_visible_snapshot BIGINT
);
ALTER TABLE {METADATA_CATALOG}.ducklake_schema ADD COLUMN {IF_NOT_EXISTS} branch_id BIGINT DEFAULT 0;
ALTER TABLE {METADATA_CATALOG}.ducklake_table ADD COLUMN {IF_NOT_EXISTS} branch_id BIGINT DEFAULT 0;
ALTER TABLE {METADATA_CATALOG}.ducklake_view ADD COLUMN {IF_NOT_EXISTS} branch_id BIGINT DEFAULT 0;
ALTER TABLE {METADATA_CATALOG}.ducklake_column ADD COLUMN {IF_NOT_EXISTS} branch_id BIGINT DEFAULT 0;
ALTER TABLE {METADATA_CATALOG}.ducklake_data_file ADD COLUMN {IF_NOT_EXISTS} branch_id BIGINT DEFAULT 0;
ALTER TABLE {METADATA_CATALOG}.ducklake_delete_file ADD COLUMN {IF_NOT_EXISTS} branch_id BIGINT DEFAULT 0;
ALTER TABLE {METADATA_CATALOG}.ducklake_delete_file ADD COLUMN {IF_NOT_EXISTS} data_file_branch_id BIGINT DEFAULT 0;
ALTER TABLE {METADATA_CATALOG}.ducklake_macro ADD COLUMN {IF_NOT_EXISTS} branch_id BIGINT DEFAULT 0;
ALTER TABLE {METADATA_CATALOG}.ducklake_partition_info ADD COLUMN {IF_NOT_EXISTS} branch_id BIGINT DEFAULT 0;
ALTER TABLE {METADATA_CATALOG}.ducklake_sort_info ADD COLUMN {IF_NOT_EXISTS} branch_id BIGINT DEFAULT 0;
ALTER TABLE {METADATA_CATALOG}.ducklake_table_stats ADD COLUMN {IF_NOT_EXISTS} branch_id BIGINT DEFAULT 0;
ALTER TABLE {METADATA_CATALOG}.ducklake_table_column_stats ADD COLUMN {IF_NOT_EXISTS} branch_id BIGINT DEFAULT 0;
ALTER TABLE {METADATA_CATALOG}.ducklake_inlined_data_tables ADD COLUMN {IF_NOT_EXISTS} branch_id BIGINT DEFAULT 0;
ALTER TABLE {METADATA_CATALOG}.ducklake_schema_versions ADD COLUMN {IF_NOT_EXISTS} branch_id BIGINT DEFAULT 0;
CREATE TABLE {IF_NOT_EXISTS} {METADATA_CATALOG}.ducklake_deletion_schema(
	branch_id BIGINT, ancestor_branch_id BIGINT, object_id BIGINT, deleted_at_snapshot BIGINT
);
CREATE TABLE {IF_NOT_EXISTS} {METADATA_CATALOG}.ducklake_deletion_table(
	branch_id BIGINT, ancestor_branch_id BIGINT, object_id BIGINT, deleted_at_snapshot BIGINT
);
CREATE TABLE {IF_NOT_EXISTS} {METADATA_CATALOG}.ducklake_deletion_view(
	branch_id BIGINT, ancestor_branch_id BIGINT, object_id BIGINT, deleted_at_snapshot BIGINT
);
CREATE TABLE {IF_NOT_EXISTS} {METADATA_CATALOG}.ducklake_deletion_column(
	branch_id BIGINT, ancestor_branch_id BIGINT, object_id BIGINT, deleted_at_snapshot BIGINT
);
CREATE TABLE {IF_NOT_EXISTS} {METADATA_CATALOG}.ducklake_deletion_data_file(
	branch_id BIGINT, ancestor_branch_id BIGINT, object_id BIGINT, deleted_at_snapshot BIGINT
);
CREATE TABLE {IF_NOT_EXISTS} {METADATA_CATALOG}.ducklake_deletion_delete_file(
	branch_id BIGINT, ancestor_branch_id BIGINT, object_id BIGINT, deleted_at_snapshot BIGINT
);
CREATE TABLE {IF_NOT_EXISTS} {METADATA_CATALOG}.ducklake_deletion_macro(
	branch_id BIGINT, ancestor_branch_id BIGINT, object_id BIGINT, deleted_at_snapshot BIGINT
);
CREATE TABLE {IF_NOT_EXISTS} {METADATA_CATALOG}.ducklake_deletion_partition(
	branch_id BIGINT, ancestor_branch_id BIGINT, object_id BIGINT, deleted_at_snapshot BIGINT
);
INSERT INTO {METADATA_CATALOG}.ducklake_branch_lineage
SELECT 0, 0, 9223372036854775807
WHERE NOT EXISTS (SELECT 1 FROM {METADATA_CATALOG}.ducklake_branch_lineage WHERE branch_id = 0 AND ancestor_branch_id = 0);
INSERT INTO {METADATA_CATALOG}.ducklake_ref(ref_id, ref_name, ref_type, snapshot_id, parent_ref_id, status, created_at)
SELECT 0, 'main', 'branch', (SELECT MAX(snapshot_id) FROM {METADATA_CATALOG}.ducklake_snapshot), NULL, 'active', NOW()
WHERE NOT EXISTS (SELECT 1 FROM {METADATA_CATALOG}.ducklake_ref WHERE ref_name = 'main');
UPDATE {METADATA_CATALOG}.ducklake_metadata SET value = '1.1-dev3' WHERE key = 'version';
	)";
	ExecuteMigration(migrate_query, allow_failures, "1.1-dev2", "1.1-dev3");
}

void DuckLakeMetadataManager::MigrateV13(bool allow_failures) {
	string migrate_query = R"(
CREATE TABLE {IF_NOT_EXISTS} {METADATA_CATALOG}.ducklake_ref_log(
	log_id BIGINT PRIMARY KEY,
	ref_id BIGINT,
	ref_name VARCHAR,
	ref_type VARCHAR,
	from_snapshot_id BIGINT,
	to_snapshot_id BIGINT,
	operation VARCHAR,
	recorded_at TIMESTAMPTZ
);
INSERT INTO {METADATA_CATALOG}.ducklake_ref_log
SELECT ref_id, ref_id, ref_name, ref_type, NULL, snapshot_id, 'create', created_at
FROM {METADATA_CATALOG}.ducklake_ref
WHERE status = 'active'
  AND NOT EXISTS (SELECT 1 FROM {METADATA_CATALOG}.ducklake_ref_log);
UPDATE {METADATA_CATALOG}.ducklake_metadata SET value = '1.1-dev4' WHERE key = 'version';
	)";
	ExecuteMigration(migrate_query, allow_failures, "1.1-dev3", "1.1-dev4");
}

idx_t DuckLakeMetadataManager::CreateRef(const string &ref_name, const string &ref_type, idx_t snapshot_id,
                                         optional_idx parent_ref_id) {
	if (!transaction.GetCatalog().SupportsRefs()) {
		throw InvalidInputException(
		    "Named refs (branches/tags) require DuckLake catalog version >= 1.1-dev2. "
		    "Re-ATTACH with AUTOMATIC_MIGRATION TRUE to upgrade.");
	}
	if (ref_type != "branch" && ref_type != "tag") {
		throw InvalidInputException("ref_type must be 'branch' or 'tag', got '%s'", ref_type);
	}
	// Single namespace across branches and tags (git-like)
	auto existing = Query(StringUtil::Format(
	    R"(SELECT ref_id FROM {METADATA_CATALOG}.ducklake_ref WHERE lower(ref_name) = lower(%s) AND status = 'active')",
	    SQLString(ref_name)));
	if (existing->HasError()) {
		existing->GetErrorObject().Throw("Failed to check for existing DuckLake ref: ");
	}
	for (auto &row : *existing) {
		(void)row;
		throw InvalidInputException("A ref named \"%s\" already exists", ref_name);
	}
	auto id_result = Query("SELECT COALESCE(MAX(ref_id), -1) + 1 FROM {METADATA_CATALOG}.ducklake_ref");
	if (id_result->HasError()) {
		id_result->GetErrorObject().Throw("Failed to allocate DuckLake ref id: ");
	}
	idx_t ref_id = 0;
	for (auto &row : *id_result) {
		ref_id = row.GetValue<idx_t>(0);
	}
	string parent_sql = parent_ref_id.IsValid() ? to_string(parent_ref_id.GetIndex()) : "NULL";
	auto result = Execute(StringUtil::Format(
	    R"(INSERT INTO {METADATA_CATALOG}.ducklake_ref VALUES (%llu, %s, %s, %llu, %s, 'active', NOW());)", ref_id,
	    SQLString(ref_name), SQLString(ref_type), snapshot_id, parent_sql));
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to create DuckLake ref: ");
	}
	AppendRefLog(ref_id, ref_name, ref_type, optional_idx(), snapshot_id, "create");
	// Phase 2: when creating a writable branch, populate lineage and seed from parent.
	if (ref_type == "branch" && transaction.GetCatalog().SupportsWritableBranches()) {
		idx_t parent_branch_id = parent_ref_id.IsValid() ? parent_ref_id.GetIndex() : 0;
		// Self row: fully visible into own chain
		string lineage_sql = StringUtil::Format(
		    "INSERT INTO {METADATA_CATALOG}.ducklake_branch_lineage VALUES (%llu, %llu, 9223372036854775807);", ref_id,
		    ref_id);
		// Copy parent's lineage, capping each ancestor at the fork snapshot
		lineage_sql += StringUtil::Format(
		    R"(
INSERT INTO {METADATA_CATALOG}.ducklake_branch_lineage
SELECT %llu, ancestor_branch_id, LEAST(max_visible_snapshot, %llu)
FROM {METADATA_CATALOG}.ducklake_branch_lineage
WHERE branch_id = %llu;)",
		    ref_id, snapshot_id, parent_branch_id);
		// Copy latest-only table stats onto the new branch
		lineage_sql += StringUtil::Format(
		    R"(
INSERT INTO {METADATA_CATALOG}.ducklake_table_stats
SELECT table_id, record_count, next_row_id, file_size_bytes, %llu
FROM {METADATA_CATALOG}.ducklake_table_stats WHERE branch_id = %llu;)",
		    ref_id, parent_branch_id);
		lineage_sql += StringUtil::Format(
		    R"(
INSERT INTO {METADATA_CATALOG}.ducklake_table_column_stats
SELECT table_id, column_id, contains_null, contains_nan, min_value, max_value, extra_stats, %llu
FROM {METADATA_CATALOG}.ducklake_table_column_stats WHERE branch_id = %llu;)",
		    ref_id, parent_branch_id);
		auto lineage_result = Execute(lineage_sql);
		if (lineage_result->HasError()) {
			lineage_result->GetErrorObject().Throw("Failed to initialize DuckLake branch lineage: ");
		}
	}
	return ref_id;
}

void DuckLakeMetadataManager::DropRef(const string &ref_name, const string &ref_type) {
	if (!transaction.GetCatalog().SupportsRefs()) {
		throw InvalidInputException(
		    "Named refs (branches/tags) require DuckLake catalog version >= 1.1-dev2. "
		    "Re-ATTACH with AUTOMATIC_MIGRATION TRUE to upgrade.");
	}
	if (transaction.GetCatalog().SupportsWritableBranches() && ref_type == "branch" &&
	    StringUtil::CIEquals(ref_name, "main")) {
		throw InvalidInputException("Cannot drop the main branch");
	}
	DuckLakeRefInfo existing;
	if (!TryResolveRef(ref_name, ref_type, existing)) {
		throw InvalidInputException("No %s named \"%s\" exists", ref_type, ref_name);
	}
	bool ref_deleted = false;
	if (transaction.GetCatalog().SupportsWritableBranches() && ref_type == "branch") {
		// Refuse drop while child branches still point at this branch
		auto children = Query(StringUtil::Format(
		    R"(SELECT ref_name FROM {METADATA_CATALOG}.ducklake_ref
WHERE parent_ref_id = %llu AND status = 'active' AND ref_type = 'branch')",
		    existing.ref_id));
		if (children->HasError()) {
			children->GetErrorObject().Throw("Failed to check DuckLake branch children: ");
		}
		vector<string> child_names;
		for (auto &row : *children) {
			child_names.push_back(row.GetValue<string>(0));
		}
		if (!child_names.empty()) {
			throw InvalidInputException(
			    "Cannot drop branch \"%s\": child branch(es) still exist (%s). Drop or merge them first.",
			    ref_name, StringUtil::Join(child_names, ", "));
		}

		// Phase 3: reclaim branch-owned snapshots and metadata, then schedule unreachable files.
		auto owned_snaps = Query(StringUtil::Format(
		    "SELECT snapshot_id FROM {METADATA_CATALOG}.ducklake_snapshot WHERE branch_id = %llu ORDER BY snapshot_id",
		    existing.ref_id));
		if (owned_snaps->HasError()) {
			owned_snaps->GetErrorObject().Throw("Failed to list DuckLake branch snapshots for drop: ");
		}
		vector<DuckLakeSnapshotInfo> to_delete;
		for (auto &row : *owned_snaps) {
			DuckLakeSnapshotInfo info;
			info.id = row.GetValue<idx_t>(0);
			info.schema_version = 0;
			info.next_file_id = 0;
			to_delete.push_back(std::move(info));
		}

		AppendRefLog(existing.ref_id, existing.ref_name, existing.ref_type, existing.snapshot_id, optional_idx(), "drop");
		auto delete_ref = Execute(StringUtil::Format(
		    R"(DELETE FROM {METADATA_CATALOG}.ducklake_ref WHERE ref_id = %llu;)", existing.ref_id));
		if (delete_ref->HasError()) {
			delete_ref->GetErrorObject().Throw("Failed to drop DuckLake ref: ");
		}
		ref_deleted = true;

		auto file_candidates = Query(StringUtil::Format(R"(
SELECT data_file_id AS schedule_file_id, data_file_id AS reachability_file_id, path, path_is_relative
FROM {METADATA_CATALOG}.ducklake_data_file
WHERE branch_id = %llu
  AND data_file_id NOT IN (SELECT data_file_id FROM {METADATA_CATALOG}.ducklake_files_scheduled_for_deletion)
UNION ALL
SELECT delete_file_id AS schedule_file_id, data_file_id AS reachability_file_id, path, path_is_relative
FROM {METADATA_CATALOG}.ducklake_delete_file
WHERE branch_id = %llu
  AND delete_file_id NOT IN (SELECT data_file_id FROM {METADATA_CATALOG}.ducklake_files_scheduled_for_deletion)
)",
		                                               existing.ref_id, existing.ref_id));
		if (file_candidates->HasError()) {
			file_candidates->GetErrorObject().Throw("Failed to list branch-owned files for cleanup: ");
		}
		string scheduled_values;
		for (auto &row : *file_candidates) {
			if (FileIsReachable(row.GetValue<idx_t>(1))) {
				continue;
			}
			if (!scheduled_values.empty()) {
				scheduled_values += ", ";
			}
			scheduled_values += StringUtil::Format("(%llu, %s, %s, NOW())", row.GetValue<idx_t>(0),
			                                       SQLString(row.GetValue<string>(2)),
			                                       row.GetValue<bool>(3) ? "true" : "false");
		}

		string schedule_sql;
		if (!scheduled_values.empty()) {
			schedule_sql +=
			    "INSERT INTO {METADATA_CATALOG}.ducklake_files_scheduled_for_deletion VALUES " + scheduled_values + ";\n";
		}
		schedule_sql += StringUtil::Format(R"(
DELETE FROM {METADATA_CATALOG}.ducklake_file_column_stats
WHERE data_file_id IN (SELECT data_file_id FROM {METADATA_CATALOG}.ducklake_data_file WHERE branch_id = %llu);
DELETE FROM {METADATA_CATALOG}.ducklake_file_variant_stats
WHERE data_file_id IN (SELECT data_file_id FROM {METADATA_CATALOG}.ducklake_data_file WHERE branch_id = %llu);
DELETE FROM {METADATA_CATALOG}.ducklake_file_partition_value
WHERE data_file_id IN (SELECT data_file_id FROM {METADATA_CATALOG}.ducklake_data_file WHERE branch_id = %llu);
DELETE FROM {METADATA_CATALOG}.ducklake_delete_file WHERE branch_id = %llu;
DELETE FROM {METADATA_CATALOG}.ducklake_data_file WHERE branch_id = %llu;
DELETE FROM {METADATA_CATALOG}.ducklake_column WHERE branch_id = %llu;
DELETE FROM {METADATA_CATALOG}.ducklake_table WHERE branch_id = %llu;
DELETE FROM {METADATA_CATALOG}.ducklake_view WHERE branch_id = %llu;
DELETE FROM {METADATA_CATALOG}.ducklake_schema WHERE branch_id = %llu;
DELETE FROM {METADATA_CATALOG}.ducklake_macro WHERE branch_id = %llu;
DELETE FROM {METADATA_CATALOG}.ducklake_partition_info WHERE branch_id = %llu;
DELETE FROM {METADATA_CATALOG}.ducklake_sort_info WHERE branch_id = %llu;
DELETE FROM {METADATA_CATALOG}.ducklake_inlined_data_tables WHERE branch_id = %llu;
DELETE FROM {METADATA_CATALOG}.ducklake_schema_versions WHERE branch_id = %llu;
DELETE FROM {METADATA_CATALOG}.ducklake_deletion_schema WHERE branch_id = %llu;
DELETE FROM {METADATA_CATALOG}.ducklake_deletion_table WHERE branch_id = %llu;
DELETE FROM {METADATA_CATALOG}.ducklake_deletion_view WHERE branch_id = %llu;
DELETE FROM {METADATA_CATALOG}.ducklake_deletion_column WHERE branch_id = %llu;
DELETE FROM {METADATA_CATALOG}.ducklake_deletion_data_file WHERE branch_id = %llu;
DELETE FROM {METADATA_CATALOG}.ducklake_deletion_delete_file WHERE branch_id = %llu;
DELETE FROM {METADATA_CATALOG}.ducklake_deletion_macro WHERE branch_id = %llu;
DELETE FROM {METADATA_CATALOG}.ducklake_deletion_partition WHERE branch_id = %llu;
)",
		    existing.ref_id, existing.ref_id, existing.ref_id, existing.ref_id, existing.ref_id,
		    existing.ref_id, existing.ref_id, existing.ref_id, existing.ref_id, existing.ref_id, existing.ref_id,
		    existing.ref_id, existing.ref_id, existing.ref_id, existing.ref_id, existing.ref_id, existing.ref_id,
		    existing.ref_id, existing.ref_id, existing.ref_id, existing.ref_id, existing.ref_id);
		auto schedule = Execute(schedule_sql);
		if (schedule->HasError()) {
			schedule->GetErrorObject().Throw("Failed to reclaim DuckLake branch-owned files: ");
		}

		if (!to_delete.empty()) {
			// Delete snapshot rows + changes (file cleanup for ended intervals already handled above for live files)
			string snapshot_ids;
			for (auto &snap : to_delete) {
				if (!snapshot_ids.empty()) {
					snapshot_ids += ", ";
				}
				snapshot_ids += to_string(snap.id);
			}
			auto snap_del = Execute(StringUtil::Format(
			    R"(
DELETE FROM {METADATA_CATALOG}.ducklake_snapshot_changes WHERE snapshot_id IN (%s);
DELETE FROM {METADATA_CATALOG}.ducklake_snapshot WHERE snapshot_id IN (%s);
)",
			    snapshot_ids, snapshot_ids));
			if (snap_del->HasError()) {
				snap_del->GetErrorObject().Throw("Failed to delete DuckLake branch snapshots: ");
			}
		}
	}
	if (!ref_deleted) {
		AppendRefLog(existing.ref_id, existing.ref_name, existing.ref_type, existing.snapshot_id, optional_idx(), "drop");
		auto result = Execute(StringUtil::Format(
		    R"(DELETE FROM {METADATA_CATALOG}.ducklake_ref WHERE ref_id = %llu;)", existing.ref_id));
		if (result->HasError()) {
			result->GetErrorObject().Throw("Failed to drop DuckLake ref: ");
		}
	}
	if (transaction.GetCatalog().SupportsWritableBranches() && ref_type == "branch") {
		auto cleanup = Execute(StringUtil::Format(
		    R"(DELETE FROM {METADATA_CATALOG}.ducklake_branch_lineage WHERE branch_id = %llu;
DELETE FROM {METADATA_CATALOG}.ducklake_table_stats WHERE branch_id = %llu;
DELETE FROM {METADATA_CATALOG}.ducklake_table_column_stats WHERE branch_id = %llu;)",
		    existing.ref_id, existing.ref_id, existing.ref_id));
		if (cleanup->HasError()) {
			cleanup->GetErrorObject().Throw("Failed to clean up DuckLake branch lineage: ");
		}
	}
}

vector<DuckLakeRefInfo> DuckLakeMetadataManager::GetRefs(const string &ref_type_filter) {
	vector<DuckLakeRefInfo> refs;
	if (!transaction.GetCatalog().SupportsRefs()) {
		return refs;
	}
	string filter;
	if (!ref_type_filter.empty()) {
		filter = StringUtil::Format(" AND ref_type = %s", SQLString(ref_type_filter));
	}
	auto result = Query(StringUtil::Format(
	    R"(SELECT ref_id, ref_name, ref_type, snapshot_id, parent_ref_id, status, created_at
FROM {METADATA_CATALOG}.ducklake_ref WHERE status = 'active'%s ORDER BY ref_name)",
	    filter));
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to list DuckLake refs: ");
	}
	for (auto &row : *result) {
		DuckLakeRefInfo info;
		info.ref_id = row.GetValue<idx_t>(0);
		info.ref_name = row.GetValue<string>(1);
		info.ref_type = row.GetValue<string>(2);
		info.snapshot_id = row.GetValue<idx_t>(3);
		if (!row.IsNull(4)) {
			info.parent_ref_id = row.GetValue<idx_t>(4);
		}
		info.status = row.GetValue<string>(5);
		info.created_at = row.GetValue<timestamp_tz_t>(6);
		refs.push_back(std::move(info));
	}
	return refs;
}

bool DuckLakeMetadataManager::TryResolveRef(const string &ref_name, const string &ref_type, DuckLakeRefInfo &out) {
	if (!transaction.GetCatalog().SupportsRefs()) {
		return false;
	}
	string type_filter;
	if (!ref_type.empty()) {
		type_filter = StringUtil::Format(" AND ref_type = %s", SQLString(ref_type));
	}
	auto result = Query(StringUtil::Format(
	    R"(SELECT ref_id, ref_name, ref_type, snapshot_id, parent_ref_id, status, created_at
FROM {METADATA_CATALOG}.ducklake_ref
WHERE lower(ref_name) = lower(%s) AND status = 'active'%s)",
	    SQLString(ref_name), type_filter));
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to resolve DuckLake ref: ");
	}
	bool found = false;
	for (auto &row : *result) {
		if (found) {
			throw InvalidInputException("Corrupt DuckLake - multiple refs named \"%s\"", ref_name);
		}
		out.ref_id = row.GetValue<idx_t>(0);
		out.ref_name = row.GetValue<string>(1);
		out.ref_type = row.GetValue<string>(2);
		out.snapshot_id = row.GetValue<idx_t>(3);
		if (!row.IsNull(4)) {
			out.parent_ref_id = row.GetValue<idx_t>(4);
		} else {
			out.parent_ref_id = optional_idx();
		}
		out.status = row.GetValue<string>(5);
		out.created_at = row.GetValue<timestamp_tz_t>(6);
		found = true;
	}
	return found;
}

set<idx_t> DuckLakeMetadataManager::GetPinnedSnapshotIds() {
	set<idx_t> pinned;
	if (!transaction.GetCatalog().SupportsRefs()) {
		return pinned;
	}
	// Live ref heads (branches + tags)
	auto result = Query(
	    "SELECT DISTINCT snapshot_id FROM {METADATA_CATALOG}.ducklake_ref WHERE status = 'active'");
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to list pinned DuckLake snapshots: ");
	}
	for (auto &row : *result) {
		pinned.insert(row.GetValue<idx_t>(0));
	}
	// Phase 3: fork-point / lineage caps of live branches must not be expired
	if (transaction.GetCatalog().SupportsWritableBranches()) {
		auto fork_points = Query(R"(
SELECT DISTINCT l.max_visible_snapshot
FROM {METADATA_CATALOG}.ducklake_branch_lineage l
JOIN {METADATA_CATALOG}.ducklake_ref r ON r.ref_id = l.branch_id
WHERE r.status = 'active' AND r.ref_type = 'branch'
  AND l.branch_id != l.ancestor_branch_id
  AND l.max_visible_snapshot < 9223372036854775807
)");
		if (fork_points->HasError()) {
			fork_points->GetErrorObject().Throw("Failed to list DuckLake fork-point snapshots: ");
		}
		for (auto &row : *fork_points) {
			pinned.insert(row.GetValue<idx_t>(0));
		}
	}
	return pinned;
}

void DuckLakeMetadataManager::UpdateBranchHead(idx_t ref_id, idx_t expected_snapshot_id, idx_t new_snapshot_id,
                                               const string &operation) {
	auto result = Execute(StringUtil::Format(
	    R"(UPDATE {METADATA_CATALOG}.ducklake_ref SET snapshot_id = %llu
WHERE ref_id = %llu AND snapshot_id = %llu AND ref_type = 'branch' AND status = 'active';)",
	    new_snapshot_id, ref_id, expected_snapshot_id));
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to advance DuckLake branch head: ");
	}
	// Verify CAS succeeded
	DuckLakeRefInfo out;
	auto check = Query(StringUtil::Format(
	    "SELECT snapshot_id FROM {METADATA_CATALOG}.ducklake_ref WHERE ref_id = %llu", ref_id));
	if (check->HasError()) {
		check->GetErrorObject().Throw("Failed to verify DuckLake branch head update: ");
	}
	bool ok = false;
	for (auto &row : *check) {
		ok = row.GetValue<idx_t>(0) == new_snapshot_id;
	}
	if (!ok) {
		throw TransactionException(
		    "Transaction conflict - branch head changed concurrently (expected snapshot %llu)", expected_snapshot_id);
	}
	auto ref_query = Query(StringUtil::Format(
	    "SELECT ref_name, ref_type FROM {METADATA_CATALOG}.ducklake_ref WHERE ref_id = %llu", ref_id));
	if (ref_query->HasError()) {
		ref_query->GetErrorObject().Throw("Failed to read DuckLake branch ref for history: ");
	}
	for (auto &row : *ref_query) {
		AppendRefLog(ref_id, row.GetValue<string>(0), row.GetValue<string>(1), expected_snapshot_id, new_snapshot_id,
		             operation);
		return;
	}
}

void DuckLakeMetadataManager::AppendRefLog(idx_t ref_id, const string &ref_name, const string &ref_type,
                                           optional_idx from_snapshot_id, optional_idx to_snapshot_id,
                                           const string &operation) {
	if (!transaction.GetCatalog().SupportsRefLog()) {
		return;
	}
	auto id_result = Query("SELECT COALESCE(MAX(log_id), -1) + 1 FROM {METADATA_CATALOG}.ducklake_ref_log");
	if (id_result->HasError()) {
		id_result->GetErrorObject().Throw("Failed to allocate DuckLake ref log id: ");
	}
	idx_t log_id = 0;
	for (auto &row : *id_result) {
		log_id = row.GetValue<idx_t>(0);
	}
	auto from_sql = from_snapshot_id.IsValid() ? to_string(from_snapshot_id.GetIndex()) : "NULL";
	auto to_sql = to_snapshot_id.IsValid() ? to_string(to_snapshot_id.GetIndex()) : "NULL";
	auto result = Execute(StringUtil::Format(
	    R"(INSERT INTO {METADATA_CATALOG}.ducklake_ref_log VALUES (%llu, %llu, %s, %s, %s, %s, %s, NOW());)", log_id,
	    ref_id, SQLString(ref_name), SQLString(ref_type), from_sql, to_sql, SQLString(operation)));
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to append DuckLake ref history: ");
	}
}

idx_t DuckLakeMetadataManager::GetMergeBaseSnapshot(idx_t source_branch_id, idx_t target_branch_id) {
	if (source_branch_id == target_branch_id) {
		throw InvalidInputException("Cannot merge a branch into itself");
	}
	auto result = Query(StringUtil::Format(
	    R"(SELECT max_visible_snapshot FROM {METADATA_CATALOG}.ducklake_branch_lineage
WHERE branch_id = %llu AND ancestor_branch_id = %llu)",
	    source_branch_id, target_branch_id));
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to resolve DuckLake merge base: ");
	}
	for (auto &row : *result) {
		return row.GetValue<idx_t>(0);
	}
	result = Query(StringUtil::Format(
	    R"(SELECT max_visible_snapshot FROM {METADATA_CATALOG}.ducklake_branch_lineage
WHERE branch_id = %llu AND ancestor_branch_id = %llu)",
	    target_branch_id, source_branch_id));
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to resolve DuckLake merge base: ");
	}
	for (auto &row : *result) {
		return row.GetValue<idx_t>(0);
	}
	result = Query(StringUtil::Format(
	    R"(SELECT MAX(LEAST(src.max_visible_snapshot, tgt.max_visible_snapshot))
FROM {METADATA_CATALOG}.ducklake_branch_lineage src
JOIN {METADATA_CATALOG}.ducklake_branch_lineage tgt
  ON src.ancestor_branch_id = tgt.ancestor_branch_id
WHERE src.branch_id = %llu AND tgt.branch_id = %llu)",
	    source_branch_id, target_branch_id));
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to resolve DuckLake merge base: ");
	}
	for (auto &row : *result) {
		if (!row.IsNull(0)) {
			return row.GetValue<idx_t>(0);
		}
	}
	throw InvalidInputException("Cannot merge: branches are unrelated (no shared lineage).");
}

SnapshotChangeInformation DuckLakeMetadataManager::GetBranchChangesSince(idx_t branch_id, idx_t after_snapshot,
                                                                          idx_t through_snapshot) {
	SnapshotChangeInformation aggregated;
	if (through_snapshot <= after_snapshot) {
		return aggregated;
	}
	auto snapshots = GetAllSnapshots(StringUtil::Format(
	    "s.snapshot_id > %llu AND s.snapshot_id <= %llu AND s.branch_id = %llu", after_snapshot, through_snapshot,
	    branch_id));
	for (auto &snapshot : snapshots) {
		if (snapshot.change_info.changes_made.empty()) {
			continue;
		}
		auto parsed = SnapshotChangeInformation::ParseChangesMade(snapshot.change_info.changes_made);
		MergeSnapshotChangeInformation(aggregated, parsed);
	}
	return aggregated;
}

set<DataFileIndex> DuckLakeMetadataManager::GetFilesDeletedOrDroppedInRange(idx_t branch_id,
                                                                              idx_t after_snapshot,
                                                                              idx_t through_snapshot) {
	set<DataFileIndex> result;
	if (through_snapshot <= after_snapshot) {
		return result;
	}
	auto query_result = Query(StringUtil::Format(R"(
SELECT DISTINCT data_file_id FROM (
	SELECT data_file_id FROM {METADATA_CATALOG}.ducklake_delete_file
	WHERE branch_id = %llu AND begin_snapshot > %llu AND begin_snapshot <= %llu
	UNION ALL
	SELECT data_file_id FROM {METADATA_CATALOG}.ducklake_data_file
	WHERE branch_id = %llu AND end_snapshot IS NOT NULL
	  AND end_snapshot > %llu AND end_snapshot <= %llu
	UNION ALL
	SELECT object_id AS data_file_id FROM {METADATA_CATALOG}.ducklake_deletion_data_file
	WHERE branch_id = %llu AND deleted_at_snapshot > %llu AND deleted_at_snapshot <= %llu
)
)",
	                                             branch_id, after_snapshot, through_snapshot, branch_id, after_snapshot,
	                                             through_snapshot, branch_id, after_snapshot, through_snapshot));
	if (query_result->HasError()) {
		query_result->GetErrorObject().Throw("Failed to get files deleted/dropped in range: ");
	}
	for (auto &row : *query_result) {
		result.insert(DataFileIndex(row.GetValue<idx_t>(0)));
	}
	return result;
}

bool DuckLakeMetadataManager::FileIsReachable(idx_t data_file_id) {
	if (!transaction.GetCatalog().SupportsWritableBranches()) {
		auto result = Query(StringUtil::Format(R"(
SELECT 1 FROM {METADATA_CATALOG}.ducklake_data_file
WHERE data_file_id = %llu AND end_snapshot IS NULL
LIMIT 1
)",
		                                       data_file_id));
		if (result->HasError()) {
			result->GetErrorObject().Throw("Failed to check file reachability: ");
		}
		for (auto &row : *result) {
			(void)row;
			return true;
		}
		return false;
	}
	// Reachable if any active branch head (via lineage visibility) or tag-pinned snapshot can see it.
	auto result = Query(StringUtil::Format(R"(
WITH active_refs AS (
	SELECT ref_id, snapshot_id, ref_type FROM {METADATA_CATALOG}.ducklake_ref WHERE status = 'active'
)
SELECT 1
FROM active_refs r
WHERE EXISTS (
	SELECT 1
	FROM {METADATA_CATALOG}.ducklake_data_file data
	WHERE data.data_file_id = %llu
	  AND EXISTS (
	    SELECT 1 FROM {METADATA_CATALOG}.ducklake_branch_lineage l
	    WHERE l.branch_id = CASE WHEN r.ref_type = 'branch' THEN r.ref_id ELSE 0 END
	      AND l.ancestor_branch_id = data.branch_id
	      AND data.begin_snapshot <= LEAST(r.snapshot_id, l.max_visible_snapshot)
	      AND (data.end_snapshot IS NULL OR data.end_snapshot > LEAST(r.snapshot_id, l.max_visible_snapshot))
	  )
	  AND NOT EXISTS (
	    SELECT 1 FROM {METADATA_CATALOG}.ducklake_deletion_data_file del
	    WHERE del.branch_id = CASE WHEN r.ref_type = 'branch' THEN r.ref_id ELSE 0 END
	      AND del.ancestor_branch_id = data.branch_id
	      AND del.object_id = data.data_file_id
	      AND del.deleted_at_snapshot <= r.snapshot_id
	  )
)
OR EXISTS (
	-- Tag pins: treat as snapshot AT on main lineage (branch_id 0) capped at the tag snapshot.
	SELECT 1
	FROM {METADATA_CATALOG}.ducklake_data_file data
	WHERE data.data_file_id = %llu AND r.ref_type = 'tag'
	  AND data.branch_id = 0
	  AND data.begin_snapshot <= r.snapshot_id
	  AND (data.end_snapshot IS NULL OR data.end_snapshot > r.snapshot_id)
)
LIMIT 1
)",
	                                       data_file_id, data_file_id));
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to check file reachability: ");
	}
	for (auto &row : *result) {
		(void)row;
		return true;
	}
	return false;
}

vector<DuckLakeSnapshotInfo> DuckLakeMetadataManager::GetSnapshotsForBranch(idx_t branch_id, const string &filter) {
	string branch_filter = StringUtil::Format(
	    R"(
EXISTS (
  SELECT 1 FROM {METADATA_CATALOG}.ducklake_branch_lineage l
  WHERE l.branch_id = %llu
    AND l.ancestor_branch_id = ducklake_snapshot.branch_id
    AND ducklake_snapshot.snapshot_id <= l.max_visible_snapshot
))",
	    branch_id);
	string full_filter = branch_filter;
	if (!filter.empty()) {
		full_filter = filter + " AND " + branch_filter;
	}
	return GetAllSnapshots(full_filter);
}

string DuckLakeMetadataManager::BuildReownNonTombstoneSQL(idx_t source_branch_id, idx_t target_branch_id) {
	const char *tables[] = {"ducklake_snapshot",
	                        "ducklake_schema",
	                        "ducklake_table",
	                        "ducklake_view",
	                        "ducklake_column",
	                        "ducklake_data_file",
	                        "ducklake_delete_file",
	                        "ducklake_macro",
	                        "ducklake_partition_info",
	                        "ducklake_sort_info",
	                        "ducklake_inlined_data_tables",
	                        "ducklake_schema_versions"};
	string sql;
	for (auto &table : tables) {
		sql += StringUtil::Format(
		    "UPDATE {METADATA_CATALOG}.%s SET branch_id = %llu WHERE branch_id = %llu;\n", table, target_branch_id,
		    source_branch_id);
	}
	sql += StringUtil::Format(
	    R"(
DELETE FROM {METADATA_CATALOG}.ducklake_table_column_stats
WHERE branch_id = %llu
   OR (branch_id = %llu AND table_id IN (
         SELECT DISTINCT table_id FROM {METADATA_CATALOG}.ducklake_data_file WHERE branch_id = %llu));
DELETE FROM {METADATA_CATALOG}.ducklake_table_stats WHERE branch_id = %llu;
INSERT INTO {METADATA_CATALOG}.ducklake_table_stats
SELECT df.table_id, COALESCE(SUM(df.record_count), 0), COALESCE(MAX(COALESCE(df.row_id_start, 0) + df.record_count), 0),
       COALESCE(SUM(df.file_size_bytes), 0), %llu
FROM {METADATA_CATALOG}.ducklake_data_file df
WHERE df.branch_id = %llu AND df.end_snapshot IS NULL
  AND df.table_id NOT IN (SELECT table_id FROM {METADATA_CATALOG}.ducklake_table_stats WHERE branch_id = %llu)
GROUP BY df.table_id;
UPDATE {METADATA_CATALOG}.ducklake_table_stats ts SET
  record_count = (SELECT COALESCE(SUM(df.record_count), 0) FROM {METADATA_CATALOG}.ducklake_data_file df
                  WHERE df.table_id = ts.table_id AND df.branch_id = %llu AND df.end_snapshot IS NULL),
  file_size_bytes = (SELECT COALESCE(SUM(df.file_size_bytes), 0) FROM {METADATA_CATALOG}.ducklake_data_file df
                     WHERE df.table_id = ts.table_id AND df.branch_id = %llu AND df.end_snapshot IS NULL),
  next_row_id = GREATEST(ts.next_row_id,
    (SELECT COALESCE(MAX(COALESCE(df.row_id_start, 0) + df.record_count), 0) FROM {METADATA_CATALOG}.ducklake_data_file df
     WHERE df.table_id = ts.table_id AND df.branch_id = %llu AND df.end_snapshot IS NULL))
WHERE ts.branch_id = %llu;
)",
	    source_branch_id, target_branch_id, target_branch_id, source_branch_id, target_branch_id, target_branch_id,
	    target_branch_id, target_branch_id, target_branch_id, target_branch_id, target_branch_id);
	return sql;
}

string DuckLakeMetadataManager::BuildReownTombstonesSQL(idx_t source_branch_id, idx_t target_branch_id) {
	const char *tables[] = {"ducklake_deletion_schema",  "ducklake_deletion_table",     "ducklake_deletion_view",
	                        "ducklake_deletion_column",  "ducklake_deletion_data_file", "ducklake_deletion_delete_file",
	                        "ducklake_deletion_macro",   "ducklake_deletion_partition"};
	string sql;
	for (auto &table : tables) {
		sql += StringUtil::Format(
		    "UPDATE {METADATA_CATALOG}.%s SET branch_id = %llu WHERE branch_id = %llu;\n", table, target_branch_id,
		    source_branch_id);
	}
	return sql;
}

namespace {

struct ConvertTombstoneKind {
	const char *deletion;
	const char *live;
	const char *live_match_expr; // SQL expr using alias `live`
};

static const ConvertTombstoneKind CONVERT_TOMBSTONE_KINDS[] = {
    {"ducklake_deletion_schema", "ducklake_schema", "live.schema_id"},
    {"ducklake_deletion_table", "ducklake_table", "live.table_id"},
    {"ducklake_deletion_view", "ducklake_view", "live.view_id"},
    {"ducklake_deletion_column", "ducklake_column", "((live.table_id::BIGINT * 4294967296) + live.column_id)"},
    {"ducklake_deletion_data_file", "ducklake_data_file", "live.data_file_id"},
    {"ducklake_deletion_delete_file", "ducklake_delete_file", "live.delete_file_id"},
    {"ducklake_deletion_macro", "ducklake_macro", "live.macro_id"},
    {"ducklake_deletion_partition", "ducklake_partition_info", "live.partition_id"},
};

} // namespace

string DuckLakeMetadataManager::BuildConvertTombstoneSiblingProbeSQL(idx_t source_branch_id, idx_t target_branch_id) {
	string sql;
	// Fail closed if convert would end-date an object still visible to another live branch.
	// Lineage-capped AT reads may still see an end-dated row historically, but end-dating the
	// shared live row is unsafe while a sibling (or other active branch) still depends on it.
	for (auto &kind : CONVERT_TOMBSTONE_KINDS) {
		sql += StringUtil::Format(R"(
SELECT error('merge_tombstone_mode=convert_end_snapshot would break sibling branch visibility for %s object ' ||
             CAST(del.object_id AS VARCHAR) ||
             '; use merge_tombstone_mode=reown_tombstone or merge/drop the sibling first')
FROM {METADATA_CATALOG}.%s del
WHERE del.branch_id = %llu
  AND EXISTS (
    SELECT 1 FROM {METADATA_CATALOG}.%s live
    WHERE %s = del.object_id AND live.end_snapshot IS NULL
      AND (live.branch_id = %llu OR live.branch_id = del.ancestor_branch_id)
      AND EXISTS (
        SELECT 1
        FROM {METADATA_CATALOG}.ducklake_ref r
        JOIN {METADATA_CATALOG}.ducklake_branch_lineage bl
          ON bl.branch_id = r.ref_id AND bl.ancestor_branch_id = live.branch_id
        WHERE r.ref_type = 'branch' AND r.status = 'active'
          AND r.ref_id NOT IN (%llu, %llu)
          AND live.begin_snapshot <= LEAST(r.snapshot_id, bl.max_visible_snapshot)
          AND NOT EXISTS (
            SELECT 1 FROM {METADATA_CATALOG}.%s sib_del
            WHERE sib_del.branch_id = r.ref_id
              AND sib_del.ancestor_branch_id = live.branch_id
              AND sib_del.object_id = del.object_id
              AND sib_del.deleted_at_snapshot <= r.snapshot_id
          )
      )
  );
)",
		                          kind.live, kind.deletion, source_branch_id, kind.live, kind.live_match_expr,
		                          target_branch_id, source_branch_id, target_branch_id, kind.deletion);
	}
	return sql;
}

string DuckLakeMetadataManager::BuildConvertTombstonesSQL(idx_t source_branch_id, idx_t target_branch_id,
                                                          idx_t merge_snapshot) {
	string sql = BuildConvertTombstoneSiblingProbeSQL(source_branch_id, target_branch_id);
	for (auto &kind : CONVERT_TOMBSTONE_KINDS) {
		// End-date live target/ancestor rows covered by source tombstones, then drop those tombstones.
		sql += StringUtil::Format(R"(
UPDATE {METADATA_CATALOG}.%s live
SET end_snapshot = %llu
WHERE live.end_snapshot IS NULL
  AND EXISTS (
    SELECT 1 FROM {METADATA_CATALOG}.%s del
    WHERE del.branch_id = %llu AND del.object_id = %s
      AND (live.branch_id = %llu OR live.branch_id = del.ancestor_branch_id OR live.branch_id = %llu)
  );
DELETE FROM {METADATA_CATALOG}.%s WHERE branch_id = %llu;
)",
		                          kind.live, merge_snapshot, kind.deletion, source_branch_id, kind.live_match_expr,
		                          target_branch_id, source_branch_id, kind.deletion, source_branch_id);
	}
	return sql;
}

namespace {

static bool CreatedMapEmpty(const case_insensitive_map_t<case_insensitive_map_t<string>> &entries) {
	for (auto &entry : entries) {
		if (!entry.second.empty()) {
			return false;
		}
	}
	return true;
}

static bool HasCreatedTables(const SnapshotChangeInformation &changes) {
	return !CreatedMapEmpty(changes.created_tables);
}

static bool HasCreatedSchemas(const SnapshotChangeInformation &changes) {
	return !changes.created_schemas.empty();
}

static bool HasCreatedViews(const SnapshotChangeInformation &changes) {
	for (auto &schema_entry : changes.created_tables) {
		for (auto &entry : schema_entry.second) {
			if (StringUtil::CIEquals(entry.second, "view")) {
				return true;
			}
		}
	}
	return false;
}

static bool HasCreatedMacros(const SnapshotChangeInformation &changes) {
	return !CreatedMapEmpty(changes.created_scalar_macros) || !CreatedMapEmpty(changes.created_table_macros);
}

static bool HasSchemaChangingChanges(const SnapshotChangeInformation &changes) {
	return !changes.created_schemas.empty() || !changes.dropped_schemas.empty() || HasCreatedTables(changes) ||
	       HasCreatedMacros(changes) || !changes.altered_tables.empty() || !changes.altered_views.empty() ||
	       !changes.dropped_tables.empty() || !changes.dropped_views.empty() || !changes.dropped_scalar_macros.empty() ||
	       !changes.dropped_table_macros.empty();
}

static string CherryPickUnsupportedReason(const SnapshotChangeInformation &changes) {
	// Supported apply surface: data-file DML + inlined_insert/inlined_delete + compose-clean DDL.
	// Still fail-closed for flushed_inlined and compaction.
	if (!changes.tables_flushed_inlined.empty()) {
		return "flushed inlined-data changes";
	}
	if (!changes.tables_compacted.empty() || !changes.tables_merge_adjacent.empty() ||
	    !changes.tables_rewrite_delete.empty()) {
		return "compaction changes";
	}
	return string();
}

static void AddChangedTables(set<TableIndex> &tables, const SnapshotChangeInformation &changes) {
	tables.insert(changes.inserted_tables.begin(), changes.inserted_tables.end());
	tables.insert(changes.tables_deleted_from.begin(), changes.tables_deleted_from.end());
	tables.insert(changes.tables_inserted_inlined.begin(), changes.tables_inserted_inlined.end());
	tables.insert(changes.tables_deleted_inlined.begin(), changes.tables_deleted_inlined.end());
}

} // namespace

DuckLakeMergeBranchResult DuckLakeMetadataManager::MergeBranch(const string &source_branch,
                                                               const string &target_branch, bool dry_run,
                                                               const string &merge_tombstone_mode) {
	if (!transaction.GetCatalog().SupportsWritableBranches()) {
		throw InvalidInputException(
		    "ducklake_merge_branch requires DuckLake catalog version >= 1.1-dev3. "
		    "Re-ATTACH with AUTOMATIC_MIGRATION TRUE to upgrade.");
	}
	DuckLakeRefInfo source_ref;
	DuckLakeRefInfo target_ref;
	if (!TryResolveRef(source_branch, "branch", source_ref)) {
		throw InvalidInputException("No branch named \"%s\" exists", source_branch);
	}
	if (!TryResolveRef(target_branch, "branch", target_ref)) {
		throw InvalidInputException("No branch named \"%s\" exists", target_branch);
	}
	if (source_ref.ref_id == target_ref.ref_id) {
		throw InvalidInputException("Cannot merge branch \"%s\" into itself", source_branch);
	}

	DuckLakeMergeBranchResult result;
	result.source_branch = source_ref.ref_name;
	result.target_branch = target_ref.ref_name;
	result.source_head = source_ref.snapshot_id;
	result.target_head = target_ref.snapshot_id;
	result.source_branch_id = source_ref.ref_id;
	result.target_branch_id = target_ref.ref_id;
	result.dry_run = dry_run;
	result.ancestor_snapshot = GetMergeBaseSnapshot(source_ref.ref_id, target_ref.ref_id);

	if (result.source_head <= result.ancestor_snapshot) {
		result.merge_type = "already_up_to_date";
		result.new_target_head = result.target_head;
		result.messages.push_back("Nothing to merge — source has no commits beyond the common ancestor");
		return result;
	}

	bool fast_forward = result.target_head == result.ancestor_snapshot;
	auto source_delta =
	    GetBranchChangesSince(source_ref.ref_id, result.ancestor_snapshot, result.source_head);
	auto target_delta =
	    GetBranchChangesSince(target_ref.ref_id, result.ancestor_snapshot, result.target_head);

	if (!fast_forward) {
		auto conflicts = DetectConflicts(source_delta, target_delta);
		// H2 G5: file-level intersection for overlapping same-table deletes.
		bool both_deleted = false;
		for (auto &table_id : source_delta.tables_deleted_from) {
			if (target_delta.tables_deleted_from.find(table_id) != target_delta.tables_deleted_from.end()) {
				both_deleted = true;
				break;
			}
		}
		if (both_deleted) {
			auto source_files = GetFilesDeletedOrDroppedInRange(source_ref.ref_id, result.ancestor_snapshot,
			                                                    result.source_head);
			auto target_files = GetFilesDeletedOrDroppedInRange(target_ref.ref_id, result.ancestor_snapshot,
			                                                    result.target_head);
			for (auto &file_id : source_files) {
				if (target_files.find(file_id) != target_files.end()) {
					conflicts.push_back(StringUtil::Format("overlapping file-level deletes on file %llu",
					                                       file_id.index));
				}
			}
		}
		if (!conflicts.empty()) {
			result.merge_type = "conflicts";
			result.new_target_head = result.target_head;
			result.messages = std::move(conflicts);
			if (!dry_run) {
				throw TransactionException("Merge conflict merging branch \"%s\" into \"%s\":\n%s", source_branch,
				                           target_branch, StringUtil::Join(result.messages, "\n"));
			}
			return result;
		}
		result.merge_type = "three_way";
	} else {
		result.merge_type = "fast_forward";
	}

	string tombstone_mode = merge_tombstone_mode;
	if (tombstone_mode.empty()) {
		if (!transaction.GetCatalog().TryGetConfigOption("merge_tombstone_mode", tombstone_mode, SchemaIndex(),
		                                                 TableIndex())) {
			tombstone_mode = "convert_end_snapshot";
		}
	}
	tombstone_mode = StringUtil::Lower(tombstone_mode);
	if (tombstone_mode != "convert_end_snapshot" && tombstone_mode != "reown_tombstone") {
		throw InvalidInputException(
		    "merge_tombstone_mode must be 'convert_end_snapshot' or 'reown_tombstone', got \"%s\"", tombstone_mode);
	}

	result.messages.push_back(StringUtil::Format("Merge type: %s", result.merge_type));
	result.messages.push_back(StringUtil::Format("Ancestor snapshot: %llu", result.ancestor_snapshot));
	result.messages.push_back(StringUtil::Format("Source head: %llu", result.source_head));
	result.messages.push_back(StringUtil::Format("Target head: %llu", result.target_head));
	result.messages.push_back(StringUtil::Format("merge_tombstone_mode: %s", tombstone_mode));

	if (dry_run) {
		result.new_target_head = fast_forward ? result.source_head : result.target_head;
		// Probe convert sibling safety without mutating metadata (same checks as apply).
		if (tombstone_mode == "convert_end_snapshot") {
			auto probe =
			    Execute(BuildConvertTombstoneSiblingProbeSQL(source_ref.ref_id, target_ref.ref_id));
			if (probe->HasError()) {
				result.merge_type = "conflicts";
				result.messages.clear();
				result.messages.push_back(probe->GetError());
				return result;
			}
		}
		result.messages.push_back("dry_run=true — no metadata changes applied");
		return result;
	}

	idx_t new_head = result.source_head;
	if (!fast_forward) {
		auto snap_q = Query(StringUtil::Format(
		    R"(SELECT schema_version, next_catalog_id, next_file_id FROM {METADATA_CATALOG}.ducklake_snapshot
WHERE snapshot_id = %llu)",
		    result.target_head));
		if (snap_q->HasError()) {
			snap_q->GetErrorObject().Throw("Failed to read target snapshot for merge: ");
		}
		idx_t schema_version = 0;
		idx_t next_catalog_id = 0;
		idx_t next_file_id = 0;
		bool found = false;
		for (auto &row : *snap_q) {
			schema_version = row.GetValue<idx_t>(0);
			next_catalog_id = row.GetValue<idx_t>(1);
			next_file_id = row.GetValue<idx_t>(2);
			found = true;
		}
		if (!found) {
			throw InvalidInputException("Target branch head snapshot %llu is missing", result.target_head);
		}
		auto src_q = Query(StringUtil::Format(
		    R"(SELECT schema_version, next_catalog_id, next_file_id FROM {METADATA_CATALOG}.ducklake_snapshot
WHERE snapshot_id = %llu)",
		    result.source_head));
		if (!src_q->HasError()) {
			for (auto &row : *src_q) {
				schema_version = MaxValue<idx_t>(schema_version, row.GetValue<idx_t>(0));
				next_catalog_id = MaxValue<idx_t>(next_catalog_id, row.GetValue<idx_t>(1));
				next_file_id = MaxValue<idx_t>(next_file_id, row.GetValue<idx_t>(2));
			}
		}
		auto id_q = Query("SELECT COALESCE(MAX(snapshot_id), -1) + 1 FROM {METADATA_CATALOG}.ducklake_snapshot");
		if (id_q->HasError()) {
			id_q->GetErrorObject().Throw("Failed to allocate merge snapshot id: ");
		}
		for (auto &row : *id_q) {
			new_head = row.GetValue<idx_t>(0);
		}
		string extra = StringUtil::Format("merge_source=%s,merge_source_head=%llu,merge_ancestor=%llu",
		                                  source_ref.ref_name, result.source_head, result.ancestor_snapshot);
		auto ins = Execute(StringUtil::Format(
		    R"(
INSERT INTO {METADATA_CATALOG}.ducklake_snapshot
VALUES (%llu, NOW(), %llu, %llu, %llu, %llu);
INSERT INTO {METADATA_CATALOG}.ducklake_snapshot_changes
VALUES (%llu, %s, NULL, %s, %s);
)",
		    new_head, schema_version, next_catalog_id, next_file_id, target_ref.ref_id, new_head,
		    SQLString(StringUtil::Format("merged_branch:%s", source_ref.ref_name)),
		    SQLString(StringUtil::Format("Merge branch %s into %s", source_ref.ref_name, target_ref.ref_name)),
		    SQLString(extra)));
		if (ins->HasError()) {
			ins->GetErrorObject().Throw("Failed to insert DuckLake merge snapshot: ");
		}
	} else {
		string extra = StringUtil::Format("merge_source=%s,merge_source_head=%llu,merge_ancestor=%llu,merge_type=ff",
		                                  source_ref.ref_name, result.source_head, result.ancestor_snapshot);
		auto book = Execute(StringUtil::Format(
		    R"(
UPDATE {METADATA_CATALOG}.ducklake_snapshot_changes
SET commit_extra_info = CASE
  WHEN commit_extra_info IS NULL OR commit_extra_info = '' THEN %s
  ELSE commit_extra_info || ',' || %s
END
WHERE snapshot_id = %llu;
)",
		    SQLString(extra), SQLString(extra), result.source_head));
		if (book->HasError()) {
			book->GetErrorObject().Throw("Failed to record DuckLake merge bookkeeping: ");
		}
	}

	if (transaction.GetCatalog().GetInliningLayout() == "shared_table") {
		auto inlined_tables =
		    Query("SELECT DISTINCT table_name FROM {METADATA_CATALOG}.ducklake_inlined_data_tables");
		if (inlined_tables->HasError()) {
			inlined_tables->GetErrorObject().Throw("Failed to list shared inlined-data tables during merge: ");
		}
		string reown_inlined_rows;
		for (auto &row : *inlined_tables) {
			reown_inlined_rows += StringUtil::Format(
			    "UPDATE {METADATA_CATALOG}.%s SET branch_id = %llu WHERE branch_id = %llu;\n",
			    SQLIdentifier(row.GetValue<string>(0)), target_ref.ref_id, source_ref.ref_id);
		}
		if (!reown_inlined_rows.empty()) {
			auto inlined_reown = Execute(reown_inlined_rows);
			if (inlined_reown->HasError()) {
				inlined_reown->GetErrorObject().Throw("Failed to re-own shared inlined-data rows during merge: ");
			}
		}
	}

	// Re-own non-tombstone metadata, then apply tombstone policy at the merge snapshot.
	auto reown = Execute(BuildReownNonTombstoneSQL(source_ref.ref_id, target_ref.ref_id));
	if (reown->HasError()) {
		reown->GetErrorObject().Throw("Failed to re-own DuckLake branch metadata during merge: ");
	}
	if (tombstone_mode == "reown_tombstone") {
		auto tomb = Execute(BuildReownTombstonesSQL(source_ref.ref_id, target_ref.ref_id));
		if (tomb->HasError()) {
			tomb->GetErrorObject().Throw("Failed to re-own DuckLake tombstones during merge: ");
		}
	} else {
		auto tomb = Execute(BuildConvertTombstonesSQL(source_ref.ref_id, target_ref.ref_id, new_head));
		if (tomb->HasError()) {
			tomb->GetErrorObject().Throw("Failed to convert DuckLake tombstones during merge: ");
		}
	}

	UpdateBranchHead(target_ref.ref_id, result.target_head, new_head, "merge");

	// Update lineage cap so a later merge of the same branch only takes the delta.
	// Source stays active (Phase 4 repeat-merge); after re-own it owns no snapshots and is droppable.
	auto lineage = Execute(StringUtil::Format(
	    R"(
UPDATE {METADATA_CATALOG}.ducklake_branch_lineage
SET max_visible_snapshot = %llu
WHERE branch_id = %llu AND ancestor_branch_id = %llu;
UPDATE {METADATA_CATALOG}.ducklake_ref SET snapshot_id = %llu WHERE ref_id = %llu;
)",
	    new_head, source_ref.ref_id, target_ref.ref_id, new_head, source_ref.ref_id));
	if (lineage->HasError()) {
		lineage->GetErrorObject().Throw("Failed to update DuckLake branch lineage after merge: ");
	}
	AppendRefLog(source_ref.ref_id, source_ref.ref_name, source_ref.ref_type, source_ref.snapshot_id, new_head, "merge");

	result.new_target_head = new_head;
	result.messages.push_back(StringUtil::Format("New target head: %llu", new_head));
	result.target_branch_id = target_ref.ref_id;
	result.source_branch_id = source_ref.ref_id;
	return result;
}

namespace {

struct CherryPickApplyResult {
	DuckLakeSnapshot snapshot;
	idx_t data_file_count = 0;
	idx_t delete_file_count = 0;
};

static DuckLakeSnapshot ReadMetadataSnapshot(DuckLakeMetadataManager &manager, idx_t id, idx_t branch_id,
                                             const string &operation, const char *what) {
	auto snap_q = manager.Query(StringUtil::Format(
	    R"(SELECT schema_version, next_catalog_id, next_file_id FROM {METADATA_CATALOG}.ducklake_snapshot
WHERE snapshot_id = %llu)",
	    id));
	if (snap_q->HasError()) {
		snap_q->GetErrorObject().Throw(StringUtil::Format("Failed to read %s snapshot for %s: ", what, operation));
	}
	DuckLakeSnapshot snapshot;
	bool found = false;
	for (auto &row : *snap_q) {
		snapshot.snapshot_id = id;
		snapshot.schema_version = row.GetValue<idx_t>(0);
		snapshot.next_catalog_id = row.GetValue<idx_t>(1);
		snapshot.next_file_id = row.GetValue<idx_t>(2);
		snapshot.branch_id = branch_id;
		found = true;
	}
	if (!found) {
		throw InvalidInputException("%s snapshot %llu is missing", what, id);
	}
	return snapshot;
}

static DuckLakeSnapshot ReadRefSnapshot(DuckLakeMetadataManager &manager, const DuckLakeRefInfo &ref,
                                        const string &operation) {
	idx_t branch_id = StringUtil::CIEquals(ref.ref_type, "tag") ? 0 : ref.ref_id;
	return ReadMetadataSnapshot(manager, ref.snapshot_id, branch_id, operation, "ref head");
}

struct CherryPickCreatedObjects {
	set<idx_t> schemas;
	set<idx_t> tables;
	set<idx_t> views;
	set<idx_t> macros;
};

static void MergeCreatedObjects(CherryPickCreatedObjects &target, const CherryPickCreatedObjects &other) {
	target.schemas.insert(other.schemas.begin(), other.schemas.end());
	target.tables.insert(other.tables.begin(), other.tables.end());
	target.views.insert(other.views.begin(), other.views.end());
	target.macros.insert(other.macros.begin(), other.macros.end());
}

static string IdSetToList(const set<idx_t> &ids) {
	string result;
	for (auto &id : ids) {
		if (!result.empty()) {
			result += ", ";
		}
		result += to_string(id);
	}
	return result;
}

template <class T>
static string IndexSetToList(const set<T> &ids) {
	string result;
	for (auto &id : ids) {
		if (!result.empty()) {
			result += ", ";
		}
		result += to_string(id.index);
	}
	return result;
}

static DuckLakeSnapshot ApplySnapshotFor(const DuckLakeRefInfo &target_ref, idx_t new_head, idx_t schema_version,
                                         idx_t next_catalog_id, idx_t next_file_id) {
	DuckLakeSnapshot snapshot;
	snapshot.snapshot_id = new_head;
	snapshot.schema_version = schema_version;
	snapshot.next_catalog_id = next_catalog_id;
	snapshot.next_file_id = next_file_id;
	snapshot.branch_id = target_ref.ref_id;
	return snapshot;
}

static string VisibilityPlaceholderFor(const string &metadata_table) {
	if (metadata_table == "ducklake_schema") {
		return "{VISIBLE_SCHEMA}";
	}
	if (metadata_table == "ducklake_table") {
		return "{VISIBLE_TABLE}";
	}
	if (metadata_table == "ducklake_view") {
		return "{VISIBLE_VIEW}";
	}
	if (metadata_table == "ducklake_macro") {
		return "{VISIBLE_MACRO}";
	}
	throw InternalException("Unsupported cherry-pick visibility table %s", metadata_table);
}

static string AliasForMetadataTable(const string &metadata_table) {
	if (metadata_table == "ducklake_schema") {
		return "sch";
	}
	if (metadata_table == "ducklake_table") {
		return "tbl";
	}
	if (metadata_table == "ducklake_view") {
		return "view";
	}
	if (metadata_table == "ducklake_macro") {
		return "ducklake_macro";
	}
	throw InternalException("Unsupported cherry-pick metadata alias table %s", metadata_table);
}

static bool CherryPickObjectVisible(DuckLakeMetadataManager &manager, const DuckLakeSnapshot &target_snapshot,
                                    const string &metadata_table, const string &id_column, idx_t object_id,
                                    const string &operation) {
	auto alias = AliasForMetadataTable(metadata_table);
	auto visible = manager.Query(target_snapshot, StringUtil::Format(R"(
SELECT 1
FROM {METADATA_CATALOG}.%s %s
WHERE %s.%s = %llu AND %s
LIMIT 1
)",
	                                                              metadata_table, alias, alias, id_column, object_id,
	                                                              VisibilityPlaceholderFor(metadata_table)));
	if (visible->HasError()) {
		visible->GetErrorObject().Throw(StringUtil::Format("Failed to verify %s object visibility: ", operation));
	}
	for (auto &row : *visible) {
		(void)row;
		return true;
	}
	return false;
}

static void CheckCherryPickObjectVisible(DuckLakeMetadataManager &manager, const DuckLakeSnapshot &target_snapshot,
                                         const DuckLakeRefInfo &target_ref, idx_t source_snapshot_id,
                                         const string &operation, const string &object_kind,
                                         const string &metadata_table, const string &id_column, idx_t object_id) {
	if (CherryPickObjectVisible(manager, target_snapshot, metadata_table, id_column, object_id, operation)) {
		return;
	}
	throw InvalidInputException(
	    "Cannot %s snapshot %llu: %s %llu is not visible on target branch \"%s\" at snapshot %llu", operation,
	    source_snapshot_id, object_kind, object_id, target_ref.ref_name, target_snapshot.snapshot_id);
}

static void CheckCherryPickTableVisible(DuckLakeMetadataManager &manager, const DuckLakeSnapshot &target_snapshot,
                                        const DuckLakeRefInfo &target_ref, idx_t source_snapshot_id,
                                        const string &operation, TableIndex table_id) {
	auto visible = manager.Query(target_snapshot, StringUtil::Format(R"(
SELECT 1
FROM {METADATA_CATALOG}.ducklake_table tbl
WHERE tbl.table_id = %llu AND {VISIBLE_TABLE}
LIMIT 1
)",
	                                                             table_id.index));
	if (visible->HasError()) {
		visible->GetErrorObject().Throw(StringUtil::Format("Failed to verify %s table dependency: ", operation));
	}
	for (auto &row : *visible) {
		(void)row;
		return;
	}
	throw InvalidInputException(
	    "Cannot %s snapshot %llu: table %llu is not visible on target branch \"%s\" at snapshot %llu",
	    operation, source_snapshot_id, table_id.index, target_ref.ref_name, target_snapshot.snapshot_id);
}

static void CheckCherryPickDataFileVisible(DuckLakeMetadataManager &manager, const DuckLakeSnapshot &target_snapshot,
                                           const DuckLakeRefInfo &target_ref, idx_t source_snapshot_id,
                                           const string &operation, idx_t data_file_id) {
	auto visible = manager.Query(target_snapshot, StringUtil::Format(R"(
SELECT 1
FROM {METADATA_CATALOG}.ducklake_data_file data
WHERE data.data_file_id = %llu AND {VISIBLE_DATA_FILE}
LIMIT 1
)",
	                                                             data_file_id));
	if (visible->HasError()) {
		visible->GetErrorObject().Throw(StringUtil::Format("Failed to verify %s data-file dependency: ", operation));
	}
	for (auto &row : *visible) {
		(void)row;
		return;
	}
	throw InvalidInputException(
	    "Cannot %s snapshot %llu: source delete depends on data file %llu, which is not visible on target "
	    "branch \"%s\" at snapshot %llu",
	    operation, source_snapshot_id, data_file_id, target_ref.ref_name, target_snapshot.snapshot_id);
}

static void ValidateCherryPickDependencies(DuckLakeMetadataManager &manager, const DuckLakeRefInfo &source_ref,
                                           const DuckLakeRefInfo &target_ref,
                                           const DuckLakeSnapshot &target_snapshot, idx_t source_snapshot_id,
                                           const SnapshotChangeInformation &source_delta, const string &operation,
                                           const set<idx_t> &source_data_files_created_by_range,
                                           const CherryPickCreatedObjects &source_objects_created_by_range) {
	set<TableIndex> touched_tables;
	AddChangedTables(touched_tables, source_delta);
	touched_tables.insert(source_delta.altered_tables.begin(), source_delta.altered_tables.end());
	touched_tables.insert(source_delta.dropped_tables.begin(), source_delta.dropped_tables.end());
	for (auto &table_id : touched_tables) {
		if (source_objects_created_by_range.tables.find(table_id.index) != source_objects_created_by_range.tables.end()) {
			continue;
		}
		CheckCherryPickTableVisible(manager, target_snapshot, target_ref, source_snapshot_id, operation, table_id);
	}

	for (auto &schema_id : source_delta.dropped_schemas) {
		if (source_objects_created_by_range.schemas.find(schema_id.index) != source_objects_created_by_range.schemas.end()) {
			continue;
		}
		CheckCherryPickObjectVisible(manager, target_snapshot, target_ref, source_snapshot_id, operation, "schema",
		                             "ducklake_schema", "schema_id", schema_id.index);
	}
	for (auto &view_id : source_delta.altered_views) {
		if (source_objects_created_by_range.views.find(view_id.index) != source_objects_created_by_range.views.end()) {
			continue;
		}
		CheckCherryPickObjectVisible(manager, target_snapshot, target_ref, source_snapshot_id, operation, "view",
		                             "ducklake_view", "view_id", view_id.index);
	}
	for (auto &view_id : source_delta.dropped_views) {
		if (source_objects_created_by_range.views.find(view_id.index) != source_objects_created_by_range.views.end()) {
			continue;
		}
		CheckCherryPickObjectVisible(manager, target_snapshot, target_ref, source_snapshot_id, operation, "view",
		                             "ducklake_view", "view_id", view_id.index);
	}
	for (auto &macro_id : source_delta.dropped_scalar_macros) {
		if (source_objects_created_by_range.macros.find(macro_id.index) != source_objects_created_by_range.macros.end()) {
			continue;
		}
		CheckCherryPickObjectVisible(manager, target_snapshot, target_ref, source_snapshot_id, operation, "macro",
		                             "ducklake_macro", "macro_id", macro_id.index);
	}
	for (auto &macro_id : source_delta.dropped_table_macros) {
		if (source_objects_created_by_range.macros.find(macro_id.index) != source_objects_created_by_range.macros.end()) {
			continue;
		}
		CheckCherryPickObjectVisible(manager, target_snapshot, target_ref, source_snapshot_id, operation, "macro",
		                             "ducklake_macro", "macro_id", macro_id.index);
	}

	auto missing_delete_dependencies = manager.Query(StringUtil::Format(R"(
SELECT DISTINCT df.data_file_id
FROM {METADATA_CATALOG}.ducklake_delete_file df
WHERE df.branch_id = %llu AND df.begin_snapshot = %llu
  AND NOT EXISTS (
    SELECT 1 FROM {METADATA_CATALOG}.ducklake_data_file data
    WHERE data.branch_id = %llu AND data.begin_snapshot = %llu AND data.data_file_id = df.data_file_id
  )
UNION
SELECT DISTINCT del.object_id
FROM {METADATA_CATALOG}.ducklake_deletion_data_file del
WHERE del.branch_id = %llu AND del.deleted_at_snapshot = %llu
UNION
SELECT DISTINCT data.data_file_id
FROM {METADATA_CATALOG}.ducklake_data_file data
WHERE data.branch_id = %llu AND data.end_snapshot = %llu AND data.begin_snapshot <> %llu
)",
	                                                                 source_ref.ref_id, source_snapshot_id,
	                                                                 source_ref.ref_id, source_snapshot_id,
	                                                                 source_ref.ref_id, source_snapshot_id,
	                                                                 source_ref.ref_id, source_snapshot_id,
	                                                                 source_snapshot_id));
	if (missing_delete_dependencies->HasError()) {
		missing_delete_dependencies->GetErrorObject().Throw(
		    StringUtil::Format("Failed to inspect %s delete dependencies: ", operation));
	}
	for (auto &row : *missing_delete_dependencies) {
		auto data_file_id = row.GetValue<idx_t>(0);
		if (source_data_files_created_by_range.find(data_file_id) != source_data_files_created_by_range.end()) {
			continue;
		}
		CheckCherryPickDataFileVisible(manager, target_snapshot, target_ref, source_snapshot_id, operation, data_file_id);
	}
}

static void AddSourceDataFilesCreatedAt(DuckLakeMetadataManager &manager, const DuckLakeRefInfo &source_ref,
                                        idx_t source_snapshot_id, set<idx_t> &created_files) {
	auto result = manager.Query(StringUtil::Format(R"(
SELECT data_file_id
FROM {METADATA_CATALOG}.ducklake_data_file
WHERE branch_id = %llu AND begin_snapshot = %llu
)",
	                                               source_ref.ref_id, source_snapshot_id));
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to read source data files for transplant dependency validation: ");
	}
	for (auto &row : *result) {
		created_files.insert(row.GetValue<idx_t>(0));
	}
}

static CherryPickCreatedObjects ReadSourceObjectsCreatedAt(DuckLakeMetadataManager &manager,
                                                           const DuckLakeRefInfo &source_ref,
                                                           idx_t source_snapshot_id) {
	CherryPickCreatedObjects created;
	auto result = manager.Query(StringUtil::Format(R"(
SELECT 'schema' AS object_type, schema_id AS object_id
FROM {METADATA_CATALOG}.ducklake_schema
WHERE branch_id = %llu AND begin_snapshot = %llu
UNION ALL
SELECT 'table' AS object_type, table_id AS object_id
FROM {METADATA_CATALOG}.ducklake_table
WHERE branch_id = %llu AND begin_snapshot = %llu
UNION ALL
SELECT 'view' AS object_type, view_id AS object_id
FROM {METADATA_CATALOG}.ducklake_view
WHERE branch_id = %llu AND begin_snapshot = %llu
UNION ALL
SELECT 'macro' AS object_type, macro_id AS object_id
FROM {METADATA_CATALOG}.ducklake_macro
WHERE branch_id = %llu AND begin_snapshot = %llu
)",
	                                             source_ref.ref_id, source_snapshot_id, source_ref.ref_id,
	                                             source_snapshot_id, source_ref.ref_id, source_snapshot_id,
	                                             source_ref.ref_id, source_snapshot_id));
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to read source DDL objects for transplant dependency validation: ");
	}
	for (auto &row : *result) {
		auto type = row.GetValue<string>(0);
		auto id = row.GetValue<idx_t>(1);
		if (type == "schema") {
			created.schemas.insert(id);
		} else if (type == "table") {
			created.tables.insert(id);
		} else if (type == "view") {
			created.views.insert(id);
		} else if (type == "macro") {
			created.macros.insert(id);
		}
	}
	return created;
}

static bool CreatedObjectsContain(const CherryPickCreatedObjects &created, const string &kind, idx_t id) {
	if (kind == "schema") {
		return created.schemas.find(id) != created.schemas.end();
	}
	if (kind == "table") {
		return created.tables.find(id) != created.tables.end();
	}
	if (kind == "view") {
		return created.views.find(id) != created.views.end();
	}
	if (kind == "macro") {
		return created.macros.find(id) != created.macros.end();
	}
	throw InternalException("Unsupported created object kind %s", kind);
}

static void ValidateCherryPickCreatedCatalogRows(DuckLakeMetadataManager &manager, const DuckLakeRefInfo &source_ref,
                                                 const DuckLakeRefInfo &target_ref,
                                                 const DuckLakeSnapshot &target_snapshot, idx_t source_snapshot_id,
                                                 const string &operation, const string &metadata_table,
                                                 const string &id_column, const string &uuid_column,
                                                 const string &schema_column, const string &name_column,
                                                 const string &object_kind,
                                                 const CherryPickCreatedObjects &created_by_range) {
	auto created = manager.Query(StringUtil::Format(R"(
SELECT %s, %s, %s, %s
FROM {METADATA_CATALOG}.%s
WHERE branch_id = %llu AND begin_snapshot = %llu
)",
	                                                id_column, uuid_column.empty() ? "''" : uuid_column,
	                                                schema_column.empty() ? "0" : schema_column, name_column,
	                                                metadata_table, source_ref.ref_id, source_snapshot_id));
	if (created->HasError()) {
		created->GetErrorObject().Throw(StringUtil::Format("Failed to inspect %s created %ss: ", operation,
		                                                    object_kind));
	}
	for (auto &row : *created) {
		auto object_id = row.GetValue<idx_t>(0);
		auto object_uuid = row.GetValue<string>(1);
		auto schema_id = row.GetValue<idx_t>(2);
		auto object_name = row.GetValue<string>(3);

		if (!schema_column.empty() && !CreatedObjectsContain(created_by_range, "schema", schema_id)) {
			CheckCherryPickObjectVisible(manager, target_snapshot, target_ref, source_snapshot_id, operation, "schema",
			                             "ducklake_schema", "schema_id", schema_id);
		}

		bool same_uuid_visible = false;
		if (!uuid_column.empty()) {
			auto alias = AliasForMetadataTable(metadata_table);
			auto visible_id = manager.Query(target_snapshot, StringUtil::Format(R"(
SELECT %s.%s
FROM {METADATA_CATALOG}.%s %s
WHERE %s.%s = %llu AND %s
LIMIT 1
)",
			                                                                 alias, uuid_column, metadata_table, alias,
			                                                                 alias, id_column, object_id,
			                                                                 VisibilityPlaceholderFor(metadata_table)));
			if (visible_id->HasError()) {
				visible_id->GetErrorObject().Throw(
				    StringUtil::Format("Failed to verify %s %s id visibility: ", operation, object_kind));
			}
			for (auto &conflict_row : *visible_id) {
				auto target_uuid = conflict_row.GetValue<string>(0);
				if (target_uuid != object_uuid) {
					throw InvalidInputException(
					    "Cannot %s snapshot %llu: %s id %llu already exists on target branch \"%s\" with a different "
					    "UUID",
					    operation, source_snapshot_id, object_kind, object_id, target_ref.ref_name);
				}
				same_uuid_visible = true;
			}
		}

		if (uuid_column.empty() &&
		    CherryPickObjectVisible(manager, target_snapshot, metadata_table, id_column, object_id, operation)) {
			throw InvalidInputException("Cannot %s snapshot %llu: %s id %llu already exists on target branch \"%s\"",
			                            operation, source_snapshot_id, object_kind, object_id, target_ref.ref_name);
		}

		auto alias = AliasForMetadataTable(metadata_table);
		string schema_filter = schema_column.empty() ? string()
		                                             : StringUtil::Format(" AND %s.%s = %llu", alias, schema_column,
		                                                                  schema_id);
		auto name_conflict = manager.Query(target_snapshot, StringUtil::Format(R"(
SELECT 1
FROM {METADATA_CATALOG}.%s %s
WHERE %s.%s = %s%s AND %s
  AND NOT (%s.%s = %llu%s)
LIMIT 1
)",
		                                                                       metadata_table, alias, alias, name_column,
		                                                                       SQLString(object_name), schema_filter,
		                                                                       VisibilityPlaceholderFor(metadata_table),
		                                                                       alias, id_column, object_id,
		                                                                       same_uuid_visible ? "" : " AND false"));
		if (name_conflict->HasError()) {
			name_conflict->GetErrorObject().Throw(
			    StringUtil::Format("Failed to verify %s %s name uniqueness: ", operation, object_kind));
		}
		for (auto &conflict_row : *name_conflict) {
			(void)conflict_row;
			throw InvalidInputException("Cannot %s snapshot %llu: %s \"%s\" already exists on target branch \"%s\"",
			                            operation, source_snapshot_id, object_kind, object_name, target_ref.ref_name);
		}
	}
}

static void ValidateCherryPickCreatedObjects(DuckLakeMetadataManager &manager, const DuckLakeRefInfo &source_ref,
                                             const DuckLakeRefInfo &target_ref,
                                             const DuckLakeSnapshot &target_snapshot, idx_t source_snapshot_id,
                                             const SnapshotChangeInformation &source_delta, const string &operation,
                                             const CherryPickCreatedObjects &created_by_range) {
	if (HasCreatedSchemas(source_delta)) {
		ValidateCherryPickCreatedCatalogRows(manager, source_ref, target_ref, target_snapshot, source_snapshot_id,
		                                     operation, "ducklake_schema", "schema_id", "schema_uuid", "",
		                                     "schema_name", "schema", created_by_range);
	}
	if (HasCreatedTables(source_delta)) {
		ValidateCherryPickCreatedCatalogRows(manager, source_ref, target_ref, target_snapshot, source_snapshot_id,
		                                     operation, "ducklake_table", "table_id", "table_uuid", "schema_id",
		                                     "table_name", "table", created_by_range);
		if (HasCreatedViews(source_delta)) {
			ValidateCherryPickCreatedCatalogRows(manager, source_ref, target_ref, target_snapshot, source_snapshot_id,
			                                     operation, "ducklake_view", "view_id", "view_uuid", "schema_id",
			                                     "view_name", "view", created_by_range);
		}
	}
	if (HasCreatedMacros(source_delta)) {
		ValidateCherryPickCreatedCatalogRows(manager, source_ref, target_ref, target_snapshot, source_snapshot_id,
		                                     operation, "ducklake_macro", "macro_id", "", "schema_id", "macro_name",
		                                     "macro", created_by_range);
	}
}

static void PrepareCherryPickApplyMaps(DuckLakeMetadataManager &manager) {
	auto create_maps = manager.Execute(R"(
DROP TABLE IF EXISTS __ducklake_cherry_pick_data_file_map;
DROP TABLE IF EXISTS __ducklake_cherry_pick_delete_file_map;
DROP TABLE IF EXISTS __ducklake_cherry_pick_cumulative_data_file_map;
DROP TABLE IF EXISTS __ducklake_cherry_pick_cumulative_delete_file_map;
DROP TABLE IF EXISTS __ducklake_cherry_pick_cumulative_inlined_row_map;
CREATE TEMP TABLE __ducklake_cherry_pick_cumulative_data_file_map(
	old_data_file_id BIGINT,
	new_data_file_id BIGINT
);
CREATE TEMP TABLE __ducklake_cherry_pick_cumulative_delete_file_map(
	old_delete_file_id BIGINT,
	new_delete_file_id BIGINT
);
CREATE TEMP TABLE __ducklake_cherry_pick_cumulative_inlined_row_map(
	table_id BIGINT,
	old_row_id BIGINT,
	new_row_id BIGINT
);
)");
	if (create_maps->HasError()) {
		create_maps->GetErrorObject().Throw("Failed to initialize DuckLake cherry-pick temp maps: ");
	}
}

static void CleanupCherryPickApplyMaps(DuckLakeMetadataManager &manager) {
	auto cleanup = manager.Execute(
	    "DROP TABLE IF EXISTS __ducklake_cherry_pick_data_file_map; "
	    "DROP TABLE IF EXISTS __ducklake_cherry_pick_delete_file_map; "
	    "DROP TABLE IF EXISTS __ducklake_cherry_pick_cumulative_data_file_map; "
	    "DROP TABLE IF EXISTS __ducklake_cherry_pick_cumulative_delete_file_map; "
	    "DROP TABLE IF EXISTS __ducklake_cherry_pick_cumulative_inlined_row_map;");
	if (cleanup->HasError()) {
		cleanup->GetErrorObject().Throw("Failed to clean up DuckLake cherry-pick temp tables: ");
	}
}

struct InlinedTableApplyTarget {
	idx_t table_id = 0;
	idx_t schema_version = 0;
	string source_table_name;
	string target_table_name;
};

static vector<InlinedTableApplyTarget> ResolveInlinedTablesForCherryPick(DuckLakeMetadataManager &manager,
                                                                         const DuckLakeRefInfo &source_ref,
                                                                         const DuckLakeRefInfo &target_ref,
                                                                         const SnapshotChangeInformation &source_delta,
                                                                         idx_t picked_id, bool shared_layout,
                                                                         const string &operation) {
	set<TableIndex> table_ids;
	table_ids.insert(source_delta.tables_inserted_inlined.begin(), source_delta.tables_inserted_inlined.end());
	table_ids.insert(source_delta.tables_deleted_inlined.begin(), source_delta.tables_deleted_inlined.end());
	vector<InlinedTableApplyTarget> result;
	if (table_ids.empty()) {
		return result;
	}

	string table_values;
	for (auto &table_id : table_ids) {
		if (!table_values.empty()) {
			table_values += ", ";
		}
		table_values += StringUtil::Format("(%llu)", table_id.index);
	}

	auto regs = manager.Query(StringUtil::Format(R"(
SELECT table_id, table_name, schema_version, branch_id
FROM {METADATA_CATALOG}.ducklake_inlined_data_tables
WHERE table_id IN (SELECT table_id FROM (VALUES %s) AS t(table_id))
ORDER BY table_id, schema_version, branch_id
)",
	                                             table_values));
	if (regs->HasError()) {
		regs->GetErrorObject().Throw(StringUtil::Format("Failed to resolve %s inlined tables: ", operation));
	}

	// Prefer registrations that actually contain rows for the picked snapshot.
	map<pair<idx_t, idx_t>, InlinedTableApplyTarget> by_table_version;
	for (auto &row : *regs) {
		InlinedTableApplyTarget target;
		target.table_id = row.GetValue<idx_t>(0);
		target.source_table_name = row.GetValue<string>(1);
		target.schema_version = row.GetValue<idx_t>(2);
		auto reg_branch = row.GetValue<idx_t>(3);

		string source_name = target.source_table_name;
		if (!shared_layout) {
			// Per-branch layout: only registrations for the source branch (or main for branch 0).
			if (reg_branch != source_ref.ref_id) {
				continue;
			}
			source_name = DuckLakeMetadataManager::InlinedTableNameFor(target.table_id, target.schema_version,
			                                                           source_ref.ref_id, false);
			target.target_table_name = DuckLakeMetadataManager::InlinedTableNameFor(
			    target.table_id, target.schema_version, target_ref.ref_id, false);
		} else {
			// Shared layout: one physical table; rows are scoped by branch_id column.
			source_name = DuckLakeMetadataManager::InlinedTableNameFor(target.table_id, target.schema_version);
			target.target_table_name = source_name;
		}
		target.source_table_name = source_name;

		string branch_filter =
		    shared_layout ? StringUtil::Format(" AND branch_id = %llu", source_ref.ref_id) : string();
		auto count_q = manager.Query(StringUtil::Format(R"(
SELECT
  (SELECT COUNT(*) FROM {METADATA_CATALOG}.%s WHERE begin_snapshot = %llu%s),
  (SELECT COUNT(*) FROM {METADATA_CATALOG}.%s WHERE end_snapshot = %llu%s)
)",
		                                                SQLIdentifier(source_name), picked_id, branch_filter,
		                                                SQLIdentifier(source_name), picked_id, branch_filter));
		if (count_q->HasError()) {
			// Table may not exist yet for this registration — skip.
			continue;
		}
		auto count_row = count_q->Fetch();
		if (!count_row) {
			continue;
		}
		auto insert_count = count_row->GetValue(0, 0).GetValue<idx_t>();
		auto delete_count = count_row->GetValue(1, 0).GetValue<idx_t>();
		if (insert_count == 0 && delete_count == 0) {
			continue;
		}
		by_table_version[make_pair(target.table_id, target.schema_version)] = std::move(target);
	}

	for (auto &entry : by_table_version) {
		result.push_back(std::move(entry.second));
	}
	return result;
}

static void EnsureTargetInlinedTable(DuckLakeMetadataManager &manager, const InlinedTableApplyTarget &target,
                                     const DuckLakeRefInfo &target_ref, bool shared_layout, const string &operation) {
	if (shared_layout || target.source_table_name == target.target_table_name) {
		return;
	}
	auto ensure = manager.Execute(StringUtil::Format(R"(
CREATE TABLE IF NOT EXISTS {METADATA_CATALOG}.%s AS
SELECT * FROM {METADATA_CATALOG}.%s WHERE 1 = 0;
INSERT INTO {METADATA_CATALOG}.ducklake_inlined_data_tables(table_id, table_name, schema_version, branch_id)
SELECT %llu, %s, %llu, %llu
WHERE NOT EXISTS (
  SELECT 1 FROM {METADATA_CATALOG}.ducklake_inlined_data_tables
  WHERE table_id = %llu AND schema_version = %llu AND branch_id = %llu
);
)",
	                                                 SQLIdentifier(target.target_table_name),
	                                                 SQLIdentifier(target.source_table_name), target.table_id,
	                                                 SQLString(target.target_table_name), target.schema_version,
	                                                 target_ref.ref_id, target.table_id, target.schema_version,
	                                                 target_ref.ref_id));
	if (ensure->HasError()) {
		ensure->GetErrorObject().Throw(StringUtil::Format("Failed to ensure %s target inlined table: ", operation));
	}
}

struct InlinedCherryPickStats {
	map<idx_t, idx_t> inserts_by_table;
	map<idx_t, idx_t> deletes_by_table;
};

static InlinedCherryPickStats ApplyCherryPickInlinedData(DuckLakeMetadataManager &manager,
                                                         const DuckLakeRefInfo &source_ref,
                                                         const DuckLakeRefInfo &target_ref, idx_t picked_id,
                                                         idx_t new_head, const SnapshotChangeInformation &source_delta,
                                                         bool shared_layout, const string &operation) {
	InlinedCherryPickStats stats_delta;
	auto targets = ResolveInlinedTablesForCherryPick(manager, source_ref, target_ref, source_delta, picked_id,
	                                                 shared_layout, operation);
	for (auto &target : targets) {
		EnsureTargetInlinedTable(manager, target, target_ref, shared_layout, operation);

		string source_branch_filter =
		    shared_layout ? StringUtil::Format(" AND src.branch_id = %llu", source_ref.ref_id) : string();
		string target_branch_select =
		    shared_layout ? StringUtil::Format(", CAST(%llu AS BIGINT) AS branch_id", target_ref.ref_id) : string();
		string exclude_cols = shared_layout ? "row_id, begin_snapshot, end_snapshot, branch_id"
		                                    : "row_id, begin_snapshot, end_snapshot";

		// Allocate target row_ids and record old→new mapping for later deletes in this transplant range.
		auto map_insert = manager.Execute(StringUtil::Format(R"(
INSERT INTO __ducklake_cherry_pick_cumulative_inlined_row_map(table_id, old_row_id, new_row_id)
SELECT %llu, src.row_id,
       COALESCE((
         SELECT stats.next_row_id FROM {METADATA_CATALOG}.ducklake_table_stats stats
         WHERE stats.table_id = %llu AND stats.branch_id = %llu
       ), 0) + ROW_NUMBER() OVER (ORDER BY src.row_id) - 1
FROM {METADATA_CATALOG}.%s src
WHERE src.begin_snapshot = %llu%s
RETURNING 1;
)",
		                                                     target.table_id, target.table_id, target_ref.ref_id,
		                                                     SQLIdentifier(target.source_table_name), picked_id,
		                                                     source_branch_filter));
		if (map_insert->HasError()) {
			map_insert->GetErrorObject().Throw(
			    StringUtil::Format("Failed to allocate %s inlined row ids: ", operation));
		}
		idx_t inserted_for_table = 0;
		for (auto &row : *map_insert) {
			(void)row;
			inserted_for_table++;
		}
		stats_delta.inserts_by_table[target.table_id] = inserted_for_table;

		auto src_delete_q = manager.Query(StringUtil::Format(R"(
SELECT COUNT(*) FROM {METADATA_CATALOG}.%s src
WHERE src.end_snapshot = %llu AND src.begin_snapshot <> %llu%s
)",
		                                                     SQLIdentifier(target.source_table_name), picked_id,
		                                                     picked_id, source_branch_filter));
		if (!src_delete_q->HasError()) {
			auto row = src_delete_q->Fetch();
			if (row) {
				stats_delta.deletes_by_table[target.table_id] = row->GetValue(0, 0).GetValue<idx_t>();
			}
		}

		auto copy_inserts = manager.Execute(StringUtil::Format(R"(
INSERT INTO {METADATA_CATALOG}.%s
SELECT map.new_row_id AS row_id,
       CAST(%llu AS BIGINT) AS begin_snapshot,
       CAST(NULL AS BIGINT) AS end_snapshot
       %s,
       src.* EXCLUDE(%s)
FROM {METADATA_CATALOG}.%s src
JOIN __ducklake_cherry_pick_cumulative_inlined_row_map map
  ON map.table_id = %llu AND map.old_row_id = src.row_id
WHERE src.begin_snapshot = %llu%s;
)",
		                                                       SQLIdentifier(target.target_table_name), new_head,
		                                                       target_branch_select, exclude_cols,
		                                                       SQLIdentifier(target.source_table_name),
		                                                       target.table_id, picked_id, source_branch_filter));
		if (copy_inserts->HasError()) {
			copy_inserts->GetErrorObject().Throw(
			    StringUtil::Format("Failed to copy %s inlined inserts: ", operation));
		}

		// Apply inlined-row deletes: prefer remapped ids from this apply/transplant range; else same row_id
		// already visible on the target branch (common when ids coincide after prior picks).
		string target_branch_filter =
		    shared_layout ? StringUtil::Format(" AND target.branch_id = %llu", target_ref.ref_id) : string();
		string source_delete_filter =
		    shared_layout ? StringUtil::Format(" AND source.branch_id = %llu", source_ref.ref_id) : string();
		auto apply_deletes = manager.Execute(StringUtil::Format(R"(
UPDATE {METADATA_CATALOG}.%s target
SET end_snapshot = %llu
FROM {METADATA_CATALOG}.%s source
LEFT JOIN __ducklake_cherry_pick_cumulative_inlined_row_map map
  ON map.table_id = %llu AND map.old_row_id = source.row_id
WHERE source.end_snapshot = %llu
  AND source.begin_snapshot <> %llu%s
  AND target.end_snapshot IS NULL%s
  AND (
    (map.new_row_id IS NOT NULL AND target.row_id = map.new_row_id)
    OR (map.new_row_id IS NULL AND target.row_id = source.row_id)
  );
)",
		                                                        SQLIdentifier(target.target_table_name), new_head,
		                                                        SQLIdentifier(target.source_table_name),
		                                                        target.table_id, picked_id, picked_id,
		                                                        source_delete_filter, target_branch_filter));
		if (apply_deletes->HasError()) {
			apply_deletes->GetErrorObject().Throw(
			    StringUtil::Format("Failed to apply %s inlined deletes: ", operation));
		}
	}

	// Copy file-linked inlined deletes (ducklake_inlined_delete_{table_id}) when present.
	for (auto &table_id : source_delta.tables_deleted_inlined) {
		auto delete_table = DuckLakeMetadataManager::InlinedFileDeletionTableName(table_id);
		auto probe = manager.Query(
		    StringUtil::Format("SELECT 1 FROM {METADATA_CATALOG}.%s LIMIT 0", SQLIdentifier(delete_table)));
		if (probe->HasError()) {
			continue;
		}
		auto copy_file_dels = manager.Execute(StringUtil::Format(R"(
INSERT INTO {METADATA_CATALOG}.%s(file_id, row_id, begin_snapshot)
SELECT COALESCE(datamap.new_data_file_id, prevmap.new_data_file_id, src.file_id),
       src.row_id,
       %llu
FROM {METADATA_CATALOG}.%s src
LEFT JOIN __ducklake_cherry_pick_data_file_map datamap ON datamap.old_data_file_id = src.file_id
LEFT JOIN __ducklake_cherry_pick_cumulative_data_file_map prevmap
       ON prevmap.old_data_file_id = src.file_id
WHERE src.begin_snapshot = %llu
  AND NOT EXISTS (
    SELECT 1 FROM {METADATA_CATALOG}.%s existing
    WHERE existing.file_id = COALESCE(datamap.new_data_file_id, prevmap.new_data_file_id, src.file_id)
      AND existing.row_id = src.row_id
      AND existing.begin_snapshot = %llu
  );
)",
		                                                         SQLIdentifier(delete_table), new_head,
		                                                         SQLIdentifier(delete_table), picked_id,
		                                                         SQLIdentifier(delete_table), new_head));
		if (copy_file_dels->HasError()) {
			copy_file_dels->GetErrorObject().Throw(
			    StringUtil::Format("Failed to copy %s inlined file deletes: ", operation));
		}
	}

	return stats_delta;
}

struct DDLApplyStats {
	idx_t max_catalog_id = 0;
	idx_t max_schema_version = 0;
};

static void ExecuteDDL(DuckLakeMetadataManager &manager, const DuckLakeSnapshot &snapshot, const string &sql,
                       const string &operation, const string &what) {
	if (sql.empty()) {
		return;
	}
	auto query = sql;
	auto result = manager.Execute(snapshot, query);
	if (result->HasError()) {
		result->GetErrorObject().Throw(StringUtil::Format("Failed to apply %s %s: ", operation, what));
	}
}

static string CherryPickDeletionTableFor(const string &metadata_table_name) {
	if (metadata_table_name == "ducklake_schema") {
		return "ducklake_deletion_schema";
	}
	if (metadata_table_name == "ducklake_table") {
		return "ducklake_deletion_table";
	}
	if (metadata_table_name == "ducklake_view") {
		return "ducklake_deletion_view";
	}
	if (metadata_table_name == "ducklake_column") {
		return "ducklake_deletion_column";
	}
	if (metadata_table_name == "ducklake_macro") {
		return "ducklake_deletion_macro";
	}
	throw InternalException("Unsupported cherry-pick tombstone table %s", metadata_table_name);
}

static string CherryPickObjectIdExpression(const string &metadata_table_name, const string &id_name) {
	if (metadata_table_name == "ducklake_column") {
		return "((m.table_id::BIGINT * 4294967296) + m.column_id)";
	}
	if (metadata_table_name == "ducklake_schema") {
		return "m.schema_id";
	}
	if (metadata_table_name == "ducklake_table") {
		return "m.table_id";
	}
	if (metadata_table_name == "ducklake_view") {
		return "m.view_id";
	}
	if (metadata_table_name == "ducklake_macro") {
		return "m.macro_id";
	}
	return "m." + id_name;
}

static string BuildCherryPickEndDateOrTombstoneSQL(const string &metadata_table_name, const string &id_name,
                                                   const string &id_list) {
	if (id_list.empty()) {
		return {};
	}
	auto deletion_table = CherryPickDeletionTableFor(metadata_table_name);
	auto object_id_expr = CherryPickObjectIdExpression(metadata_table_name, id_name);
	return StringUtil::Format(R"(
UPDATE {METADATA_CATALOG}.%s SET end_snapshot = {SNAPSHOT_ID}
WHERE end_snapshot IS NULL AND %s IN (%s) AND branch_id = {BRANCH_ID};

INSERT INTO {METADATA_CATALOG}.%s (branch_id, ancestor_branch_id, object_id, deleted_at_snapshot)
SELECT DISTINCT {BRANCH_ID}, m.branch_id, %s, {SNAPSHOT_ID}
FROM {METADATA_CATALOG}.%s m
WHERE m.end_snapshot IS NULL AND m.%s IN (%s) AND m.branch_id != {BRANCH_ID}
  AND NOT EXISTS (
    SELECT 1 FROM {METADATA_CATALOG}.%s d
    WHERE d.branch_id = {BRANCH_ID} AND d.ancestor_branch_id = m.branch_id
      AND d.object_id = %s AND d.deleted_at_snapshot <= {SNAPSHOT_ID}
  );
)",
	                          metadata_table_name, id_name, id_list, deletion_table, object_id_expr,
	                          metadata_table_name, id_name, id_list, deletion_table, object_id_expr);
}

static void ApplyEndDateOrTombstone(DuckLakeMetadataManager &manager, const DuckLakeSnapshot &snapshot,
                                    const string &metadata_table, const string &id_column, const string &id_list,
                                    const string &operation, const string &what) {
	if (id_list.empty()) {
		return;
	}
	ExecuteDDL(manager, snapshot, BuildCherryPickEndDateOrTombstoneSQL(metadata_table, id_column, id_list), operation,
	           what);
}

static void CopyInlinedRegistrationsForSchemaVersions(DuckLakeMetadataManager &manager, const DuckLakeRefInfo &source_ref,
                                                      const DuckLakeRefInfo &target_ref, const DuckLakeSnapshot &snapshot,
                                                      idx_t table_id, idx_t table_schema_version, bool shared_layout,
                                                      const string &operation) {
	auto regs = manager.Query(StringUtil::Format(R"(
SELECT table_name, schema_version
FROM {METADATA_CATALOG}.ducklake_inlined_data_tables
WHERE table_id = %llu AND branch_id = %llu AND schema_version = %llu
)",
	                                             table_id, source_ref.ref_id, table_schema_version));
	if (regs->HasError()) {
		regs->GetErrorObject().Throw(StringUtil::Format("Failed to read %s inlined registrations: ", operation));
	}
	for (auto &reg_row : *regs) {
		auto source_name = reg_row.GetValue<string>(0);
		auto schema_version = reg_row.GetValue<idx_t>(1);
		string target_name =
		    shared_layout ? source_name
		                  : DuckLakeMetadataManager::InlinedTableNameFor(table_id, schema_version, target_ref.ref_id,
		                                                                false);
		auto ensure = manager.Execute(snapshot, StringUtil::Format(R"(
CREATE TABLE IF NOT EXISTS {METADATA_CATALOG}.%s AS
SELECT * FROM {METADATA_CATALOG}.%s WHERE 1 = 0;
INSERT INTO {METADATA_CATALOG}.ducklake_inlined_data_tables(table_id, table_name, schema_version, branch_id)
SELECT %llu, %s, %llu, %llu
WHERE NOT EXISTS (
  SELECT 1 FROM {METADATA_CATALOG}.ducklake_inlined_data_tables
  WHERE table_id = %llu AND schema_version = %llu AND branch_id = %llu
);
)",
		                                                       SQLIdentifier(target_name), SQLIdentifier(source_name),
		                                                       table_id, SQLString(target_name), schema_version,
		                                                       target_ref.ref_id, table_id, schema_version,
		                                                       target_ref.ref_id));
		if (ensure->HasError()) {
			ensure->GetErrorObject().Throw(StringUtil::Format("Failed to register %s inlined table: ", operation));
		}
	}
}

static DDLApplyStats ApplyCherryPickCreatedSchemas(DuckLakeMetadataManager &manager, const DuckLakeRefInfo &source_ref,
                                                   const DuckLakeSnapshot &snapshot, idx_t picked_id,
                                                   const string &operation) {
	DDLApplyStats stats;
	auto created = manager.Query(StringUtil::Format(R"(
SELECT schema_id
FROM {METADATA_CATALOG}.ducklake_schema
WHERE branch_id = %llu AND begin_snapshot = %llu
ORDER BY schema_id
)",
	                                              source_ref.ref_id, picked_id));
	if (created->HasError()) {
		created->GetErrorObject().Throw(StringUtil::Format("Failed to list %s created schemas: ", operation));
	}
	set<idx_t> ids;
	for (auto &row : *created) {
		auto schema_id = row.GetValue<idx_t>(0);
		ids.insert(schema_id);
		stats.max_catalog_id = MaxValue<idx_t>(stats.max_catalog_id, schema_id + 1);
	}
	auto id_list = IdSetToList(ids);
	ApplyEndDateOrTombstone(manager, snapshot, "ducklake_schema", "schema_id", id_list, operation,
	                        "created schema rewrites");
	if (id_list.empty()) {
		return stats;
	}
	ExecuteDDL(manager, snapshot, StringUtil::Format(R"(
INSERT INTO {METADATA_CATALOG}.ducklake_schema(
    schema_id, schema_uuid, begin_snapshot, end_snapshot, schema_name, path, path_is_relative, branch_id)
SELECT schema_id, schema_uuid, {SNAPSHOT_ID}, NULL, schema_name, path, path_is_relative, {BRANCH_ID}
FROM {METADATA_CATALOG}.ducklake_schema
WHERE branch_id = %llu AND begin_snapshot = %llu;
)",
	                                               source_ref.ref_id, picked_id),
	           operation, "created schemas");
	return stats;
}

struct CreatedTableApplyStats {
	idx_t table_count = 0;
	idx_t max_table_id = 0;
	idx_t max_table_schema_version = 0;
};

static CreatedTableApplyStats ApplyCherryPickCreatedTables(DuckLakeMetadataManager &manager,
                                                           const DuckLakeRefInfo &source_ref,
                                                           const DuckLakeRefInfo &target_ref, idx_t picked_id,
                                                           bool shared_layout, const string &operation,
                                                           const DuckLakeSnapshot &snapshot) {
	CreatedTableApplyStats stats;
	auto created = manager.Query(StringUtil::Format(R"(
SELECT table_id,
       COALESCE((
         SELECT MAX(sv.schema_version)
         FROM {METADATA_CATALOG}.ducklake_schema_versions sv
         WHERE sv.table_id = t.table_id AND sv.begin_snapshot = t.begin_snapshot
       ), (
         SELECT MAX(idt.schema_version)
         FROM {METADATA_CATALOG}.ducklake_inlined_data_tables idt
         WHERE idt.table_id = t.table_id AND idt.branch_id = t.branch_id
       ), %llu)
FROM {METADATA_CATALOG}.ducklake_table t
WHERE t.branch_id = %llu AND t.begin_snapshot = %llu
ORDER BY t.table_id
)",
	                                                picked_id, source_ref.ref_id, picked_id));
	if (created->HasError()) {
		created->GetErrorObject().Throw(StringUtil::Format("Failed to list %s created tables: ", operation));
	}

	vector<pair<idx_t, idx_t>> created_tables;
	for (auto &row : *created) {
		auto table_id = row.GetValue<idx_t>(0);
		auto table_schema_version = row.GetValue<idx_t>(1);
		created_tables.emplace_back(table_id, table_schema_version);
		stats.table_count++;
		stats.max_table_id = MaxValue<idx_t>(stats.max_table_id, table_id);
		stats.max_table_schema_version = MaxValue<idx_t>(stats.max_table_schema_version, table_schema_version);
	}
	if (created_tables.empty()) {
		return stats;
	}

	set<idx_t> created_table_ids;
	for (auto &entry : created_tables) {
		created_table_ids.insert(entry.first);
	}
	ApplyEndDateOrTombstone(manager, snapshot, "ducklake_table", "table_id", IdSetToList(created_table_ids),
	                        operation, "created table rewrites");

	auto copy_tables = manager.Execute(snapshot, StringUtil::Format(R"(
INSERT INTO {METADATA_CATALOG}.ducklake_table(
    table_id, table_uuid, begin_snapshot, end_snapshot, schema_id, table_name, path, path_is_relative, branch_id)
SELECT table_id, table_uuid, {SNAPSHOT_ID}, NULL, schema_id, table_name, path, path_is_relative, {BRANCH_ID}
FROM {METADATA_CATALOG}.ducklake_table
WHERE branch_id = %llu AND begin_snapshot = %llu;

INSERT INTO {METADATA_CATALOG}.ducklake_column(
    column_id, begin_snapshot, end_snapshot, table_id, column_order, column_name, column_type,
    initial_default, default_value, nulls_allowed, parent_column, default_value_type,
    default_value_dialect, branch_id)
SELECT column_id, {SNAPSHOT_ID}, NULL, table_id, column_order, column_name, column_type,
       initial_default, default_value, nulls_allowed, parent_column, default_value_type,
       default_value_dialect, {BRANCH_ID}
FROM {METADATA_CATALOG}.ducklake_column
WHERE branch_id = %llu AND begin_snapshot = %llu;

INSERT INTO {METADATA_CATALOG}.ducklake_table_stats(table_id, record_count, next_row_id, file_size_bytes, branch_id)
SELECT t.table_id, 0, 0, 0, {BRANCH_ID}
FROM {METADATA_CATALOG}.ducklake_table t
WHERE t.branch_id = %llu AND t.begin_snapshot = %llu
  AND NOT EXISTS (
    SELECT 1 FROM {METADATA_CATALOG}.ducklake_table_stats stats
    WHERE stats.table_id = t.table_id AND stats.branch_id = {BRANCH_ID}
  );

INSERT INTO {METADATA_CATALOG}.ducklake_table_column_stats(
    table_id, column_id, contains_null, contains_nan, min_value, max_value, extra_stats, branch_id)
SELECT c.table_id, c.column_id, NULL, NULL, NULL, NULL, NULL, {BRANCH_ID}
FROM {METADATA_CATALOG}.ducklake_column c
WHERE c.branch_id = %llu AND c.begin_snapshot = %llu
  AND NOT EXISTS (
    SELECT 1 FROM {METADATA_CATALOG}.ducklake_table_column_stats stats
    WHERE stats.table_id = c.table_id AND stats.column_id = c.column_id AND stats.branch_id = {BRANCH_ID}
  );
)",
	                                                       source_ref.ref_id, picked_id, source_ref.ref_id, picked_id,
	                                                       source_ref.ref_id, picked_id, source_ref.ref_id, picked_id));
	if (copy_tables->HasError()) {
		copy_tables->GetErrorObject().Throw(StringUtil::Format("Failed to copy %s created tables: ", operation));
	}

	for (auto &entry : created_tables) {
		auto table_id = entry.first;
		auto table_schema_version = entry.second;
		auto schema_versions = manager.Execute(snapshot, StringUtil::Format(R"(
INSERT INTO {METADATA_CATALOG}.ducklake_schema_versions(begin_snapshot, schema_version, table_id, branch_id)
SELECT {SNAPSHOT_ID}, %llu, %llu, {BRANCH_ID}
WHERE NOT EXISTS (
  SELECT 1 FROM {METADATA_CATALOG}.ducklake_schema_versions sv
  WHERE sv.table_id = %llu AND sv.schema_version = %llu AND sv.branch_id = {BRANCH_ID}
);
)",
		                                                           table_schema_version, table_id, table_id,
		                                                           table_schema_version));
		if (schema_versions->HasError()) {
			schema_versions->GetErrorObject().Throw(
			    StringUtil::Format("Failed to copy %s schema_versions: ", operation));
		}

		CopyInlinedRegistrationsForSchemaVersions(manager, source_ref, target_ref, snapshot, table_id,
		                                          table_schema_version, shared_layout, operation);
	}

	return stats;
}

static DDLApplyStats ApplyCherryPickCreatedViews(DuckLakeMetadataManager &manager, const DuckLakeRefInfo &source_ref,
                                                 const DuckLakeSnapshot &snapshot, idx_t picked_id,
                                                 const string &operation) {
	DDLApplyStats stats;
	auto created = manager.Query(StringUtil::Format(R"(
SELECT view_id
FROM {METADATA_CATALOG}.ducklake_view
WHERE branch_id = %llu AND begin_snapshot = %llu
ORDER BY view_id
)",
	                                              source_ref.ref_id, picked_id));
	if (created->HasError()) {
		created->GetErrorObject().Throw(StringUtil::Format("Failed to list %s created views: ", operation));
	}
	set<idx_t> ids;
	for (auto &row : *created) {
		auto view_id = row.GetValue<idx_t>(0);
		ids.insert(view_id);
		stats.max_catalog_id = MaxValue<idx_t>(stats.max_catalog_id, view_id + 1);
	}
	auto id_list = IdSetToList(ids);
	ApplyEndDateOrTombstone(manager, snapshot, "ducklake_view", "view_id", id_list, operation,
	                        "created view rewrites");
	if (id_list.empty()) {
		return stats;
	}
	ExecuteDDL(manager, snapshot, StringUtil::Format(R"(
INSERT INTO {METADATA_CATALOG}.ducklake_view(
    view_id, view_uuid, begin_snapshot, end_snapshot, schema_id, view_name, dialect, sql, column_aliases, branch_id)
SELECT view_id, view_uuid, {SNAPSHOT_ID}, NULL, schema_id, view_name, dialect, sql, column_aliases, {BRANCH_ID}
FROM {METADATA_CATALOG}.ducklake_view
WHERE branch_id = %llu AND begin_snapshot = %llu;

INSERT INTO {METADATA_CATALOG}.ducklake_tag(object_id, begin_snapshot, end_snapshot, key, value)
SELECT object_id, {SNAPSHOT_ID}, NULL, key, value
FROM {METADATA_CATALOG}.ducklake_tag
WHERE object_id IN (%s) AND begin_snapshot = %llu;

INSERT INTO {METADATA_CATALOG}.ducklake_view_column_tag(view_id, column_name, begin_snapshot, end_snapshot, key, value)
SELECT view_id, column_name, {SNAPSHOT_ID}, NULL, key, value
FROM {METADATA_CATALOG}.ducklake_view_column_tag
WHERE view_id IN (%s) AND begin_snapshot = %llu;
)",
	                                               source_ref.ref_id, picked_id, id_list, picked_id, id_list,
	                                               picked_id),
	           operation, "created views");
	return stats;
}

static DDLApplyStats ApplyCherryPickCreatedMacros(DuckLakeMetadataManager &manager, const DuckLakeRefInfo &source_ref,
                                                  const DuckLakeSnapshot &snapshot, idx_t picked_id,
                                                  const string &operation) {
	DDLApplyStats stats;
	auto created = manager.Query(StringUtil::Format(R"(
SELECT macro_id
FROM {METADATA_CATALOG}.ducklake_macro
WHERE branch_id = %llu AND begin_snapshot = %llu
ORDER BY macro_id
)",
	                                              source_ref.ref_id, picked_id));
	if (created->HasError()) {
		created->GetErrorObject().Throw(StringUtil::Format("Failed to list %s created macros: ", operation));
	}
	set<idx_t> ids;
	for (auto &row : *created) {
		auto macro_id = row.GetValue<idx_t>(0);
		ids.insert(macro_id);
		stats.max_catalog_id = MaxValue<idx_t>(stats.max_catalog_id, macro_id + 1);
	}
	auto id_list = IdSetToList(ids);
	ApplyEndDateOrTombstone(manager, snapshot, "ducklake_macro", "macro_id", id_list, operation,
	                        "created macro rewrites");
	if (id_list.empty()) {
		return stats;
	}
	ExecuteDDL(manager, snapshot, StringUtil::Format(R"(
INSERT INTO {METADATA_CATALOG}.ducklake_macro(schema_id, macro_id, macro_name, begin_snapshot, end_snapshot, branch_id)
SELECT schema_id, macro_id, macro_name, {SNAPSHOT_ID}, NULL, {BRANCH_ID}
FROM {METADATA_CATALOG}.ducklake_macro
WHERE branch_id = %llu AND begin_snapshot = %llu;

INSERT INTO {METADATA_CATALOG}.ducklake_macro_impl(macro_id, impl_id, dialect, sql, type)
SELECT macro_id, impl_id, dialect, sql, type
FROM {METADATA_CATALOG}.ducklake_macro_impl impl
WHERE macro_id IN (%s)
  AND NOT EXISTS (
    SELECT 1 FROM {METADATA_CATALOG}.ducklake_macro_impl existing
    WHERE existing.macro_id = impl.macro_id AND existing.impl_id = impl.impl_id
  );

INSERT INTO {METADATA_CATALOG}.ducklake_macro_parameters(
    macro_id, impl_id, column_id, parameter_name, parameter_type, default_value, default_value_type)
SELECT macro_id, impl_id, column_id, parameter_name, parameter_type, default_value, default_value_type
FROM {METADATA_CATALOG}.ducklake_macro_parameters param
WHERE macro_id IN (%s)
  AND NOT EXISTS (
    SELECT 1 FROM {METADATA_CATALOG}.ducklake_macro_parameters existing
    WHERE existing.macro_id = param.macro_id AND existing.impl_id = param.impl_id
      AND existing.column_id = param.column_id
  );
)",
	                                               source_ref.ref_id, picked_id, id_list, id_list),
	           operation, "created macros");
	return stats;
}

static DDLApplyStats ApplyCherryPickAlteredTables(DuckLakeMetadataManager &manager, const DuckLakeRefInfo &source_ref,
                                                  const DuckLakeRefInfo &target_ref, const DuckLakeSnapshot &snapshot,
                                                  idx_t picked_id, const SnapshotChangeInformation &source_delta,
                                                  bool shared_layout, const string &operation) {
	DDLApplyStats stats;
	if (source_delta.altered_tables.empty()) {
		return stats;
	}
	auto table_list = IndexSetToList(source_delta.altered_tables);
	auto dropped_cols = manager.Query(StringUtil::Format(R"(
SELECT ((col.table_id::BIGINT * 4294967296) + col.column_id) AS object_id
FROM {METADATA_CATALOG}.ducklake_column col
WHERE col.branch_id = %llu AND col.end_snapshot = %llu AND col.begin_snapshot <> %llu
  AND col.table_id IN (%s)
UNION
SELECT del.object_id
FROM {METADATA_CATALOG}.ducklake_deletion_column del
WHERE del.branch_id = %llu AND del.deleted_at_snapshot = %llu
  AND CAST(FLOOR(del.object_id / 4294967296) AS BIGINT) IN (%s)
)",
	                                               source_ref.ref_id, picked_id, picked_id, table_list,
	                                               source_ref.ref_id, picked_id, table_list));
	if (dropped_cols->HasError()) {
		dropped_cols->GetErrorObject().Throw(StringUtil::Format("Failed to inspect %s dropped columns: ", operation));
	}
	set<idx_t> dropped_column_object_ids;
	for (auto &row : *dropped_cols) {
		dropped_column_object_ids.insert(row.GetValue<idx_t>(0));
	}
	auto dropped_column_list = IdSetToList(dropped_column_object_ids);
	if (!dropped_column_list.empty()) {
		ExecuteDDL(manager, snapshot, StringUtil::Format(R"(
UPDATE {METADATA_CATALOG}.ducklake_column
SET end_snapshot = {SNAPSHOT_ID}
WHERE end_snapshot IS NULL
  AND branch_id = {BRANCH_ID}
  AND ((table_id::BIGINT * 4294967296) + column_id) IN (%s);

INSERT INTO {METADATA_CATALOG}.ducklake_deletion_column
  (branch_id, ancestor_branch_id, object_id, deleted_at_snapshot)
SELECT DISTINCT {BRANCH_ID}, col.branch_id, ((col.table_id::BIGINT * 4294967296) + col.column_id), {SNAPSHOT_ID}
FROM {METADATA_CATALOG}.ducklake_column col
WHERE col.end_snapshot IS NULL
  AND col.branch_id != {BRANCH_ID}
  AND ((col.table_id::BIGINT * 4294967296) + col.column_id) IN (%s)
  AND NOT EXISTS (
    SELECT 1 FROM {METADATA_CATALOG}.ducklake_deletion_column del
    WHERE del.branch_id = {BRANCH_ID} AND del.ancestor_branch_id = col.branch_id
      AND del.object_id = ((col.table_id::BIGINT * 4294967296) + col.column_id)
      AND del.deleted_at_snapshot <= {SNAPSHOT_ID}
  );
)",
		                                               dropped_column_list, dropped_column_list),
		           operation, "dropped columns");
	}

	auto new_cols = manager.Query(StringUtil::Format(R"(
SELECT DISTINCT table_id, column_id
FROM {METADATA_CATALOG}.ducklake_column
WHERE branch_id = %llu AND begin_snapshot = %llu AND table_id IN (%s)
)",
	                                           source_ref.ref_id, picked_id, table_list));
	if (new_cols->HasError()) {
		new_cols->GetErrorObject().Throw(StringUtil::Format("Failed to inspect %s new columns: ", operation));
	}
	set<idx_t> rewritten_column_object_ids;
	for (auto &row : *new_cols) {
		auto table_id = row.GetValue<idx_t>(0);
		auto column_id = row.GetValue<idx_t>(1);
		rewritten_column_object_ids.insert((table_id * 4294967296ULL) + column_id);
	}
	auto rewritten_column_list = IdSetToList(rewritten_column_object_ids);
	if (!rewritten_column_list.empty()) {
		ExecuteDDL(manager, snapshot, StringUtil::Format(R"(
UPDATE {METADATA_CATALOG}.ducklake_column
SET end_snapshot = {SNAPSHOT_ID}
WHERE end_snapshot IS NULL
  AND branch_id = {BRANCH_ID}
  AND ((table_id::BIGINT * 4294967296) + column_id) IN (%s);

INSERT INTO {METADATA_CATALOG}.ducklake_deletion_column
  (branch_id, ancestor_branch_id, object_id, deleted_at_snapshot)
SELECT DISTINCT {BRANCH_ID}, col.branch_id, ((col.table_id::BIGINT * 4294967296) + col.column_id), {SNAPSHOT_ID}
FROM {METADATA_CATALOG}.ducklake_column col
WHERE col.end_snapshot IS NULL
  AND col.branch_id != {BRANCH_ID}
  AND ((col.table_id::BIGINT * 4294967296) + col.column_id) IN (%s)
  AND NOT EXISTS (
    SELECT 1 FROM {METADATA_CATALOG}.ducklake_deletion_column del
    WHERE del.branch_id = {BRANCH_ID} AND del.ancestor_branch_id = col.branch_id
      AND del.object_id = ((col.table_id::BIGINT * 4294967296) + col.column_id)
      AND del.deleted_at_snapshot <= {SNAPSHOT_ID}
  );
)",
		                                               rewritten_column_list, rewritten_column_list),
		           operation, "rewritten columns");
	}

	ExecuteDDL(manager, snapshot, StringUtil::Format(R"(
INSERT INTO {METADATA_CATALOG}.ducklake_column(
    column_id, begin_snapshot, end_snapshot, table_id, column_order, column_name, column_type,
    initial_default, default_value, nulls_allowed, parent_column, default_value_type,
    default_value_dialect, branch_id)
SELECT column_id, {SNAPSHOT_ID}, NULL, table_id, column_order, column_name, column_type,
       initial_default, default_value, nulls_allowed, parent_column, default_value_type,
       default_value_dialect, {BRANCH_ID}
FROM {METADATA_CATALOG}.ducklake_column
WHERE branch_id = %llu AND begin_snapshot = %llu AND table_id IN (%s);

INSERT INTO {METADATA_CATALOG}.ducklake_table_column_stats(
    table_id, column_id, contains_null, contains_nan, min_value, max_value, extra_stats, branch_id)
SELECT c.table_id, c.column_id, NULL, NULL, NULL, NULL, NULL, {BRANCH_ID}
FROM {METADATA_CATALOG}.ducklake_column c
WHERE c.branch_id = %llu AND c.begin_snapshot = %llu AND c.table_id IN (%s)
  AND NOT EXISTS (
    SELECT 1 FROM {METADATA_CATALOG}.ducklake_table_column_stats stats
    WHERE stats.table_id = c.table_id AND stats.column_id = c.column_id AND stats.branch_id = {BRANCH_ID}
  );

INSERT INTO {METADATA_CATALOG}.ducklake_column_tag(table_id, column_id, begin_snapshot, end_snapshot, key, value)
SELECT table_id, column_id, {SNAPSHOT_ID}, NULL, key, value
FROM {METADATA_CATALOG}.ducklake_column_tag
WHERE begin_snapshot = %llu AND table_id IN (%s);
)",
	                                           source_ref.ref_id, picked_id, table_list, source_ref.ref_id, picked_id,
	                                           table_list, picked_id, table_list),
	           operation, "altered table columns");

	auto schema_versions = manager.Query(StringUtil::Format(R"(
SELECT table_id, schema_version
FROM {METADATA_CATALOG}.ducklake_schema_versions
WHERE branch_id = %llu AND begin_snapshot = %llu AND table_id IN (%s)
ORDER BY table_id, schema_version
)",
	                                                source_ref.ref_id, picked_id, table_list));
	if (schema_versions->HasError()) {
		schema_versions->GetErrorObject().Throw(StringUtil::Format("Failed to list %s schema versions: ", operation));
	}
	for (auto &row : *schema_versions) {
		auto table_id = row.GetValue<idx_t>(0);
		auto schema_version = row.GetValue<idx_t>(1);
		stats.max_schema_version = MaxValue<idx_t>(stats.max_schema_version, schema_version);
		ExecuteDDL(manager, snapshot, StringUtil::Format(R"(
INSERT INTO {METADATA_CATALOG}.ducklake_schema_versions(begin_snapshot, schema_version, table_id, branch_id)
SELECT {SNAPSHOT_ID}, %llu, %llu, {BRANCH_ID}
WHERE NOT EXISTS (
  SELECT 1 FROM {METADATA_CATALOG}.ducklake_schema_versions sv
  WHERE sv.table_id = %llu AND sv.schema_version = %llu AND sv.branch_id = {BRANCH_ID}
);
)",
		                                               schema_version, table_id, table_id, schema_version),
		           operation, "altered table schema version");
		CopyInlinedRegistrationsForSchemaVersions(manager, source_ref, target_ref, snapshot, table_id, schema_version,
		                                          shared_layout, operation);
	}
	return stats;
}

static void ApplyCherryPickAlteredViews(DuckLakeMetadataManager &manager, const DuckLakeRefInfo &source_ref,
                                        const DuckLakeSnapshot &snapshot, idx_t picked_id,
                                        const SnapshotChangeInformation &source_delta, const string &operation) {
	if (source_delta.altered_views.empty()) {
		return;
	}
	auto view_list = IndexSetToList(source_delta.altered_views);
	ExecuteDDL(manager, snapshot, StringUtil::Format(R"(
UPDATE {METADATA_CATALOG}.ducklake_tag
SET end_snapshot = {SNAPSHOT_ID}
WHERE end_snapshot IS NULL
  AND object_id IN (%s)
  AND key IN (
    SELECT key FROM {METADATA_CATALOG}.ducklake_tag
    WHERE begin_snapshot = %llu AND object_id IN (%s)
  );
INSERT INTO {METADATA_CATALOG}.ducklake_tag(object_id, begin_snapshot, end_snapshot, key, value)
SELECT object_id, {SNAPSHOT_ID}, NULL, key, value
FROM {METADATA_CATALOG}.ducklake_tag
WHERE begin_snapshot = %llu AND object_id IN (%s);

UPDATE {METADATA_CATALOG}.ducklake_view_column_tag
SET end_snapshot = {SNAPSHOT_ID}
WHERE end_snapshot IS NULL
  AND view_id IN (%s)
  AND (view_id, column_name, key) IN (
    SELECT view_id, column_name, key FROM {METADATA_CATALOG}.ducklake_view_column_tag
    WHERE begin_snapshot = %llu AND view_id IN (%s)
  );
INSERT INTO {METADATA_CATALOG}.ducklake_view_column_tag(view_id, column_name, begin_snapshot, end_snapshot, key, value)
SELECT view_id, column_name, {SNAPSHOT_ID}, NULL, key, value
FROM {METADATA_CATALOG}.ducklake_view_column_tag
WHERE begin_snapshot = %llu AND view_id IN (%s);
)",
	                                           view_list, picked_id, view_list, picked_id, view_list, view_list,
	                                           picked_id, view_list, picked_id, view_list),
	           operation, "altered views");
}

static void ApplyCherryPickDroppedObjects(DuckLakeMetadataManager &manager, const DuckLakeSnapshot &snapshot,
                                          const SnapshotChangeInformation &source_delta, const string &operation) {
	ExecuteDDL(manager, snapshot, DuckLakeMetadataManager::DropSchemas(source_delta.dropped_schemas), operation,
	           "dropped schemas");
	ExecuteDDL(manager, snapshot, DuckLakeMetadataManager::DropTables(source_delta.dropped_tables, false), operation,
	           "dropped tables");
	ExecuteDDL(manager, snapshot, DuckLakeMetadataManager::DropViews(source_delta.dropped_views, false, true),
	           operation, "dropped views");
	set<MacroIndex> dropped_macros = source_delta.dropped_scalar_macros;
	dropped_macros.insert(source_delta.dropped_table_macros.begin(), source_delta.dropped_table_macros.end());
	ExecuteDDL(manager, snapshot, DuckLakeMetadataManager::DropMacros(dropped_macros), operation, "dropped macros");
}

static CherryPickApplyResult ApplyCherryPickSnapshot(DuckLakeMetadataManager &manager,
                                                     const DuckLakeRefInfo &source_ref,
                                                     const DuckLakeRefInfo &target_ref,
                                                     const DuckLakeSnapshotInfo &picked,
                                                     const DuckLakeSnapshot &target_snapshot,
                                                     const SnapshotChangeInformation &source_delta,
                                                     const string &commit_message, const string &extra,
                                                     const string &operation, bool shared_layout) {
	auto count_q = manager.Query(StringUtil::Format(R"(
SELECT
  (SELECT COUNT(*) FROM {METADATA_CATALOG}.ducklake_data_file
   WHERE branch_id = %llu AND begin_snapshot = %llu),
  (SELECT COUNT(*) FROM {METADATA_CATALOG}.ducklake_delete_file
   WHERE branch_id = %llu AND begin_snapshot = %llu),
  (SELECT COALESCE(MAX(data_file_id) + 1, 0) FROM {METADATA_CATALOG}.ducklake_data_file),
  (SELECT COALESCE(MAX(delete_file_id) + 1, 0) FROM {METADATA_CATALOG}.ducklake_delete_file)
)",
	                                                      source_ref.ref_id, picked.id, source_ref.ref_id, picked.id));
	if (count_q->HasError()) {
		count_q->GetErrorObject().Throw(StringUtil::Format("Failed to size %s file remapping: ", operation));
	}
	auto count_row = count_q->Fetch();
	idx_t data_file_count = count_row->GetValue(0, 0).GetValue<idx_t>();
	idx_t delete_file_count = count_row->GetValue(1, 0).GetValue<idx_t>();
	auto next_data_file_id = count_row->GetValue(2, 0).GetValue<idx_t>();
	auto next_delete_file_id = count_row->GetValue(3, 0).GetValue<idx_t>();
	idx_t next_file_id = MaxValue<idx_t>(target_snapshot.next_file_id, next_data_file_id);
	next_file_id = MaxValue<idx_t>(next_file_id, next_delete_file_id);
	idx_t data_file_base = next_file_id;
	idx_t delete_file_base = data_file_base + data_file_count;
	idx_t new_next_file_id = delete_file_base + delete_file_count;

	auto id_q = manager.Query("SELECT COALESCE(MAX(snapshot_id), -1) + 1 FROM {METADATA_CATALOG}.ducklake_snapshot");
	if (id_q->HasError()) {
		id_q->GetErrorObject().Throw(StringUtil::Format("Failed to allocate %s snapshot id: ", operation));
	}
	idx_t new_head = 0;
	for (auto &row : *id_q) {
		new_head = row.GetValue<idx_t>(0);
	}

	idx_t new_schema_version = target_snapshot.schema_version;
	idx_t new_next_catalog_id = target_snapshot.next_catalog_id;
	if (HasSchemaChangingChanges(source_delta)) {
		new_schema_version = target_snapshot.schema_version + 1;
	}

	auto insert_snapshot = manager.Execute(StringUtil::Format(
	    R"(
INSERT INTO {METADATA_CATALOG}.ducklake_snapshot
VALUES (%llu, NOW(), %llu, %llu, %llu, %llu);
INSERT INTO {METADATA_CATALOG}.ducklake_snapshot_changes
VALUES (%llu, %s, NULL, %s, %s);
)",
	    new_head, new_schema_version, new_next_catalog_id, new_next_file_id, target_ref.ref_id, new_head,
	    SQLString(picked.change_info.changes_made), SQLString(commit_message), SQLString(extra)));
	if (insert_snapshot->HasError()) {
		insert_snapshot->GetErrorObject().Throw(StringUtil::Format("Failed to insert DuckLake %s snapshot: ", operation));
	}

	auto apply_snapshot =
	    ApplySnapshotFor(target_ref, new_head, new_schema_version, new_next_catalog_id, new_next_file_id);

	auto bump_schema_and_catalog = [&](idx_t max_schema_version, idx_t max_catalog_id) {
		new_schema_version = MaxValue<idx_t>(new_schema_version, max_schema_version);
		new_next_catalog_id = MaxValue<idx_t>(new_next_catalog_id, max_catalog_id);
	};

	if (HasCreatedSchemas(source_delta)) {
		auto schema_stats = ApplyCherryPickCreatedSchemas(manager, source_ref, apply_snapshot, picked.id, operation);
		bump_schema_and_catalog(schema_stats.max_schema_version, schema_stats.max_catalog_id);
	}
	if (HasCreatedTables(source_delta)) {
		auto created_stats = ApplyCherryPickCreatedTables(manager, source_ref, target_ref, picked.id, shared_layout,
		                                                  operation, apply_snapshot);
		if (created_stats.table_count > 0) {
			bump_schema_and_catalog(created_stats.max_table_schema_version, created_stats.max_table_id + 1);
		}
	}
	if (HasCreatedViews(source_delta)) {
		auto view_stats = ApplyCherryPickCreatedViews(manager, source_ref, apply_snapshot, picked.id, operation);
		bump_schema_and_catalog(view_stats.max_schema_version, view_stats.max_catalog_id);
	}
	if (HasCreatedMacros(source_delta)) {
		auto macro_stats = ApplyCherryPickCreatedMacros(manager, source_ref, apply_snapshot, picked.id, operation);
		bump_schema_and_catalog(macro_stats.max_schema_version, macro_stats.max_catalog_id);
	}
	auto alter_stats = ApplyCherryPickAlteredTables(manager, source_ref, target_ref, apply_snapshot, picked.id,
	                                                source_delta, shared_layout, operation);
	bump_schema_and_catalog(alter_stats.max_schema_version, alter_stats.max_catalog_id);
	ApplyCherryPickAlteredViews(manager, source_ref, apply_snapshot, picked.id, source_delta, operation);
	ApplyCherryPickDroppedObjects(manager, apply_snapshot, source_delta, operation);

	if (HasSchemaChangingChanges(source_delta)) {
		auto bump = manager.Execute(StringUtil::Format(R"(
UPDATE {METADATA_CATALOG}.ducklake_snapshot
SET schema_version = %llu, next_catalog_id = %llu
WHERE snapshot_id = %llu;
)",
		                                               new_schema_version, new_next_catalog_id, new_head));
		if (bump->HasError()) {
			bump->GetErrorObject().Throw(StringUtil::Format("Failed to bump DuckLake %s schema catalog ids: ",
			                                                operation));
		}
		apply_snapshot.schema_version = new_schema_version;
		apply_snapshot.next_catalog_id = new_next_catalog_id;
	}

	auto apply_sql = StringUtil::Format(
	    R"(
DROP TABLE IF EXISTS __ducklake_cherry_pick_data_file_map;
DROP TABLE IF EXISTS __ducklake_cherry_pick_delete_file_map;
CREATE TEMP TABLE __ducklake_cherry_pick_data_file_map AS
SELECT data_file_id AS old_data_file_id,
       CAST(%llu AS BIGINT) + ROW_NUMBER() OVER (ORDER BY data_file_id) - 1 AS new_data_file_id
FROM {METADATA_CATALOG}.ducklake_data_file
WHERE branch_id = %llu AND begin_snapshot = %llu;
CREATE TEMP TABLE __ducklake_cherry_pick_delete_file_map AS
SELECT delete_file_id AS old_delete_file_id,
       CAST(%llu AS BIGINT) + ROW_NUMBER() OVER (ORDER BY delete_file_id) - 1 AS new_delete_file_id
FROM {METADATA_CATALOG}.ducklake_delete_file
WHERE branch_id = %llu AND begin_snapshot = %llu;

INSERT INTO {METADATA_CATALOG}.ducklake_data_file(
    data_file_id, table_id, begin_snapshot, end_snapshot, file_order, path, path_is_relative,
    file_format, record_count, file_size_bytes, footer_size, row_id_start, partition_id,
    encryption_key, mapping_id, partial_max, row_group_count, branch_id)
SELECT map.new_data_file_id, data.table_id, %llu, NULL, data.file_order, data.path, data.path_is_relative,
       data.file_format, data.record_count, data.file_size_bytes, data.footer_size, data.row_id_start,
       data.partition_id, data.encryption_key, data.mapping_id, data.partial_max, data.row_group_count, %llu
FROM {METADATA_CATALOG}.ducklake_data_file data
JOIN __ducklake_cherry_pick_data_file_map map ON map.old_data_file_id = data.data_file_id;
INSERT INTO {METADATA_CATALOG}.ducklake_file_column_stats
SELECT map.new_data_file_id, stats.table_id, stats.column_id, stats.column_size_bytes, stats.value_count,
       stats.null_count, stats.min_value, stats.max_value, stats.contains_nan, stats.extra_stats
FROM {METADATA_CATALOG}.ducklake_file_column_stats stats
JOIN __ducklake_cherry_pick_data_file_map map ON map.old_data_file_id = stats.data_file_id;
INSERT INTO {METADATA_CATALOG}.ducklake_file_variant_stats
SELECT map.new_data_file_id, stats.table_id, stats.column_id, stats.variant_path, stats.shredded_type,
       stats.column_size_bytes, stats.value_count, stats.null_count, stats.min_value, stats.max_value,
       stats.contains_nan, stats.extra_stats
FROM {METADATA_CATALOG}.ducklake_file_variant_stats stats
JOIN __ducklake_cherry_pick_data_file_map map ON map.old_data_file_id = stats.data_file_id;
INSERT INTO {METADATA_CATALOG}.ducklake_file_partition_value
SELECT map.new_data_file_id, part.table_id, part.partition_key_index, part.partition_value
FROM {METADATA_CATALOG}.ducklake_file_partition_value part
JOIN __ducklake_cherry_pick_data_file_map map ON map.old_data_file_id = part.data_file_id;

INSERT INTO __ducklake_cherry_pick_cumulative_data_file_map
SELECT old_data_file_id, new_data_file_id
FROM __ducklake_cherry_pick_data_file_map;

INSERT INTO {METADATA_CATALOG}.ducklake_delete_file(
    delete_file_id, table_id, begin_snapshot, end_snapshot, data_file_id, path, path_is_relative,
    format, delete_count, file_size_bytes, footer_size, encryption_key, partial_max, row_group_count,
    branch_id, data_file_branch_id)
SELECT delmap.new_delete_file_id, del.table_id, %llu, NULL,
       COALESCE(datamap.new_data_file_id, prevmap.new_data_file_id, del.data_file_id),
       del.path, del.path_is_relative, del.format, del.delete_count, del.file_size_bytes, del.footer_size,
       del.encryption_key, del.partial_max, del.row_group_count, %llu,
       CASE
         WHEN datamap.new_data_file_id IS NOT NULL OR prevmap.new_data_file_id IS NOT NULL THEN %llu
         ELSE del.data_file_branch_id
       END
FROM {METADATA_CATALOG}.ducklake_delete_file del
JOIN __ducklake_cherry_pick_delete_file_map delmap ON delmap.old_delete_file_id = del.delete_file_id
LEFT JOIN __ducklake_cherry_pick_data_file_map datamap ON datamap.old_data_file_id = del.data_file_id
LEFT JOIN __ducklake_cherry_pick_cumulative_data_file_map prevmap
       ON prevmap.old_data_file_id = del.data_file_id;

INSERT INTO __ducklake_cherry_pick_cumulative_delete_file_map
SELECT old_delete_file_id, new_delete_file_id
FROM __ducklake_cherry_pick_delete_file_map;

INSERT INTO {METADATA_CATALOG}.ducklake_deletion_data_file
  (branch_id, ancestor_branch_id, object_id, deleted_at_snapshot)
SELECT %llu,
       CASE WHEN datamap.new_data_file_id IS NOT NULL OR prevmap.new_data_file_id IS NOT NULL
            THEN %llu ELSE del.ancestor_branch_id END,
       COALESCE(datamap.new_data_file_id, prevmap.new_data_file_id, del.object_id),
       %llu
FROM {METADATA_CATALOG}.ducklake_deletion_data_file del
LEFT JOIN __ducklake_cherry_pick_data_file_map datamap ON datamap.old_data_file_id = del.object_id
LEFT JOIN __ducklake_cherry_pick_cumulative_data_file_map prevmap
       ON prevmap.old_data_file_id = del.object_id
WHERE del.branch_id = %llu AND del.deleted_at_snapshot = %llu
  AND NOT EXISTS (
    SELECT 1 FROM {METADATA_CATALOG}.ducklake_deletion_data_file existing
    WHERE existing.branch_id = %llu
      AND existing.ancestor_branch_id = CASE WHEN datamap.new_data_file_id IS NOT NULL
                                             OR prevmap.new_data_file_id IS NOT NULL
                                             THEN %llu ELSE del.ancestor_branch_id END
      AND existing.object_id = COALESCE(datamap.new_data_file_id, prevmap.new_data_file_id, del.object_id)
      AND existing.deleted_at_snapshot <= %llu
  );
INSERT INTO {METADATA_CATALOG}.ducklake_deletion_delete_file
  (branch_id, ancestor_branch_id, object_id, deleted_at_snapshot)
SELECT %llu,
       CASE WHEN delmap.new_delete_file_id IS NOT NULL OR prevdelmap.new_delete_file_id IS NOT NULL
            THEN %llu ELSE del.ancestor_branch_id END,
       COALESCE(delmap.new_delete_file_id, prevdelmap.new_delete_file_id, del.object_id),
       %llu
FROM {METADATA_CATALOG}.ducklake_deletion_delete_file del
LEFT JOIN __ducklake_cherry_pick_delete_file_map delmap ON delmap.old_delete_file_id = del.object_id
LEFT JOIN __ducklake_cherry_pick_cumulative_delete_file_map prevdelmap
       ON prevdelmap.old_delete_file_id = del.object_id
WHERE del.branch_id = %llu AND del.deleted_at_snapshot = %llu
  AND NOT EXISTS (
    SELECT 1 FROM {METADATA_CATALOG}.ducklake_deletion_delete_file existing
    WHERE existing.branch_id = %llu
      AND existing.ancestor_branch_id = CASE WHEN delmap.new_delete_file_id IS NOT NULL
                                             OR prevdelmap.new_delete_file_id IS NOT NULL
                                             THEN %llu ELSE del.ancestor_branch_id END
      AND existing.object_id = COALESCE(delmap.new_delete_file_id, prevdelmap.new_delete_file_id, del.object_id)
      AND existing.deleted_at_snapshot <= %llu
  );

UPDATE {METADATA_CATALOG}.ducklake_data_file target
SET end_snapshot = %llu
FROM {METADATA_CATALOG}.ducklake_data_file source
JOIN __ducklake_cherry_pick_cumulative_data_file_map datamap
  ON datamap.old_data_file_id = source.data_file_id
WHERE source.branch_id = %llu
  AND source.end_snapshot = %llu
  AND source.begin_snapshot <> %llu
  AND target.branch_id = %llu
  AND target.data_file_id = datamap.new_data_file_id
  AND target.end_snapshot IS NULL;

UPDATE {METADATA_CATALOG}.ducklake_delete_file target
SET end_snapshot = %llu
FROM {METADATA_CATALOG}.ducklake_delete_file source
JOIN __ducklake_cherry_pick_cumulative_delete_file_map delmap
  ON delmap.old_delete_file_id = source.delete_file_id
WHERE source.branch_id = %llu
  AND source.end_snapshot = %llu
  AND source.begin_snapshot <> %llu
  AND target.branch_id = %llu
  AND target.delete_file_id = delmap.new_delete_file_id
  AND target.end_snapshot IS NULL;
)",
	    data_file_base, source_ref.ref_id, picked.id, delete_file_base, source_ref.ref_id, picked.id, new_head,
	    target_ref.ref_id, new_head, target_ref.ref_id, target_ref.ref_id, target_ref.ref_id, target_ref.ref_id,
	    new_head, source_ref.ref_id, picked.id, target_ref.ref_id, target_ref.ref_id, new_head, target_ref.ref_id,
	    target_ref.ref_id, new_head, source_ref.ref_id, picked.id, target_ref.ref_id, target_ref.ref_id, new_head,
	    new_head, source_ref.ref_id, picked.id, picked.id, target_ref.ref_id, new_head, source_ref.ref_id, picked.id,
	    picked.id, target_ref.ref_id);
	auto apply = manager.Execute(apply_sql);
	if (apply->HasError()) {
		apply->GetErrorObject().Throw(StringUtil::Format("Failed to apply DuckLake %s metadata: ", operation));
	}

	set<TableIndex> touched_tables;
	AddChangedTables(touched_tables, source_delta);

	// Apply inlined DML before stats so the combined stats UPDATE can include inlined deltas.
	auto inlined_stats = ApplyCherryPickInlinedData(manager, source_ref, target_ref, picked.id, new_head, source_delta,
	                                                shared_layout, operation);

	if (!touched_tables.empty()) {
		string table_values;
		for (auto &table_id : touched_tables) {
			if (!table_values.empty()) {
				table_values += ", ";
			}
			table_values += StringUtil::Format("(%llu)", table_id.index);
		}
		string insert_case = "0";
		string delete_case = "0";
		string next_case = "stats.next_row_id";
		// next_row_id / record_count for inlined rows are applied in dedicated UPDATEs below.
		auto stats = manager.Execute(StringUtil::Format(R"(
INSERT INTO {METADATA_CATALOG}.ducklake_table_stats(table_id, record_count, next_row_id, file_size_bytes, branch_id)
SELECT t.table_id, 0, 0, 0, %llu
FROM (VALUES %s) AS t(table_id)
WHERE NOT EXISTS (
  SELECT 1 FROM {METADATA_CATALOG}.ducklake_table_stats stats
  WHERE stats.table_id = t.table_id AND stats.branch_id = %llu
);
UPDATE {METADATA_CATALOG}.ducklake_table_stats stats
SET record_count = GREATEST(0, stats.record_count
    + COALESCE((SELECT SUM(data.record_count) FROM {METADATA_CATALOG}.ducklake_data_file data
                WHERE data.branch_id = %llu AND data.begin_snapshot = %llu
                  AND data.table_id = stats.table_id), 0)
    - COALESCE((SELECT SUM(del.delete_count) FROM {METADATA_CATALOG}.ducklake_delete_file del
                WHERE del.branch_id = %llu AND del.begin_snapshot = %llu
                  AND del.table_id = stats.table_id), 0)
    - COALESCE((SELECT SUM(data.record_count) FROM {METADATA_CATALOG}.ducklake_data_file data
                WHERE data.branch_id = %llu AND data.end_snapshot = %llu
                  AND data.table_id = stats.table_id), 0)
    + (%s) - (%s)),
    file_size_bytes = GREATEST(0, stats.file_size_bytes
    + COALESCE((SELECT SUM(data.file_size_bytes) FROM {METADATA_CATALOG}.ducklake_data_file data
                WHERE data.branch_id = %llu AND data.begin_snapshot = %llu
                  AND data.table_id = stats.table_id), 0)
    - COALESCE((SELECT SUM(data.file_size_bytes) FROM {METADATA_CATALOG}.ducklake_data_file data
                WHERE data.branch_id = %llu AND data.end_snapshot = %llu
                  AND data.table_id = stats.table_id), 0)),
    next_row_id = GREATEST(
    COALESCE((SELECT MAX(COALESCE(data.row_id_start, 0) + data.record_count)
              FROM {METADATA_CATALOG}.ducklake_data_file data
              WHERE data.branch_id = %llu AND data.begin_snapshot = %llu
                AND data.table_id = stats.table_id), stats.next_row_id),
    %s)
WHERE stats.branch_id = %llu
  AND stats.table_id IN (SELECT table_id FROM (VALUES %s) AS t(table_id));
)",
		                                                 target_ref.ref_id, table_values, target_ref.ref_id,
		                                                 target_ref.ref_id, new_head, target_ref.ref_id, new_head,
		                                                 target_ref.ref_id, new_head, insert_case, delete_case,
		                                                 target_ref.ref_id, new_head, target_ref.ref_id, new_head,
		                                                 target_ref.ref_id, new_head, next_case, target_ref.ref_id,
		                                                 table_values));
		if (stats->HasError()) {
			stats->GetErrorObject().Throw(StringUtil::Format("Failed to update DuckLake %s table stats: ", operation));
		}
		// Dedicated inlined record_count / next_row_id bump (literal deltas — avoids embedding SQL fragments).
		for (auto &entry : inlined_stats.inserts_by_table) {
			if (entry.second == 0) {
				continue;
			}
			auto bump = manager.Execute(StringUtil::Format(R"(
UPDATE {METADATA_CATALOG}.ducklake_table_stats
SET record_count = record_count + %llu,
    next_row_id = GREATEST(next_row_id, COALESCE((
      SELECT MAX(map.new_row_id) + 1 FROM __ducklake_cherry_pick_cumulative_inlined_row_map map
      WHERE map.table_id = %llu
    ), next_row_id))
WHERE table_id = %llu AND branch_id = %llu;
)",
			                                               entry.second, entry.first, entry.first, target_ref.ref_id));
			if (bump->HasError()) {
				bump->GetErrorObject().Throw(
				    StringUtil::Format("Failed to bump DuckLake %s inlined insert stats: ", operation));
			}
		}
		for (auto &entry : inlined_stats.deletes_by_table) {
			if (entry.second == 0) {
				continue;
			}
			auto bump = manager.Execute(StringUtil::Format(R"(
UPDATE {METADATA_CATALOG}.ducklake_table_stats
SET record_count = GREATEST(0, record_count - %llu)
WHERE table_id = %llu AND branch_id = %llu;
)",
			                                               entry.second, entry.first, target_ref.ref_id));
			if (bump->HasError()) {
				bump->GetErrorObject().Throw(
				    StringUtil::Format("Failed to bump DuckLake %s inlined delete stats: ", operation));
			}
		}
	}

	manager.UpdateBranchHead(target_ref.ref_id, target_snapshot.snapshot_id, new_head,
	                         operation == "cherry-pick" ? "cherry_pick" : operation);

	CherryPickApplyResult result;
	result.snapshot.snapshot_id = new_head;
	result.snapshot.schema_version = new_schema_version;
	result.snapshot.next_catalog_id = new_next_catalog_id;
	result.snapshot.next_file_id = new_next_file_id;
	result.snapshot.branch_id = target_ref.ref_id;
	result.data_file_count = data_file_count;
	result.delete_file_count = delete_file_count;
	return result;
}

static void AppendDeleteConflicts(DuckLakeMetadataManager &manager, const DuckLakeRefInfo &source_ref,
                                  const DuckLakeRefInfo &target_ref, idx_t source_after, idx_t source_through,
                                  idx_t target_after, idx_t target_through,
                                  const SnapshotChangeInformation &source_delta,
                                  const SnapshotChangeInformation &target_delta, vector<string> &conflicts) {
	bool both_deleted = false;
	for (auto &table_id : source_delta.tables_deleted_from) {
		if (target_delta.tables_deleted_from.find(table_id) != target_delta.tables_deleted_from.end()) {
			both_deleted = true;
			break;
		}
	}
	if (!both_deleted) {
		return;
	}
	auto source_files = manager.GetFilesDeletedOrDroppedInRange(source_ref.ref_id, source_after, source_through);
	auto target_files = manager.GetFilesDeletedOrDroppedInRange(target_ref.ref_id, target_after, target_through);
	for (auto &file_id : source_files) {
		if (target_files.find(file_id) != target_files.end()) {
			conflicts.push_back(StringUtil::Format("overlapping file-level deletes on file %llu", file_id.index));
		}
	}
}

static bool ResolveBranchRef(DuckLakeMetadataManager &manager, const string &branch_name, DuckLakeRefInfo &out) {
	return manager.TryResolveRef(branch_name, "branch", out);
}

static bool ResolveAnyRef(DuckLakeMetadataManager &manager, const string &ref_name, DuckLakeRefInfo &out) {
	if (manager.TryResolveRef(ref_name, "branch", out)) {
		return true;
	}
	return manager.TryResolveRef(ref_name, "tag", out);
}

static string ColumnSignature(const vector<DuckLakeColumnInfo> &columns, const string &prefix = string()) {
	string result;
	for (auto &column : columns) {
		if (!result.empty()) {
			result += ";";
		}
		auto path = prefix.empty() ? column.name : prefix + "." + column.name;
		result += StringUtil::Format("%llu:%s:%s:%s", column.id.index, path, column.type,
		                             column.nulls_allowed ? "NULL" : "NOT NULL");
		auto child_signature = ColumnSignature(column.children, path);
		if (!child_signature.empty()) {
			result += ";" + child_signature;
		}
	}
	return result;
}

static string JoinName(const string &schema_name, const string &object_name) {
	return schema_name.empty() ? object_name : schema_name + "." + object_name;
}

static string SchemaNameForId(const map<SchemaIndex, string> &schemas, SchemaIndex schema_id) {
	auto entry = schemas.find(schema_id);
	return entry == schemas.end() ? string() : entry->second;
}

static string SchemaUUIDForId(const map<SchemaIndex, string> &schemas, SchemaIndex schema_id) {
	auto entry = schemas.find(schema_id);
	return entry == schemas.end() ? string() : entry->second;
}

} // namespace

DuckLakeCherryPickResult DuckLakeMetadataManager::CherryPick(const string &source_branch, idx_t snapshot_id,
                                                             const string &target_branch, bool dry_run) {
	if (!transaction.GetCatalog().SupportsWritableBranches()) {
		throw InvalidInputException(
		    "ducklake_cherry_pick requires DuckLake catalog version >= 1.1-dev3. "
		    "Re-ATTACH with AUTOMATIC_MIGRATION TRUE to upgrade.");
	}
	DuckLakeRefInfo source_ref;
	DuckLakeRefInfo target_ref;
	if (!ResolveBranchRef(*this, source_branch, source_ref)) {
		throw InvalidInputException("No branch named \"%s\" exists", source_branch);
	}
	if (!ResolveBranchRef(*this, target_branch, target_ref)) {
		throw InvalidInputException("No branch named \"%s\" exists", target_branch);
	}
	if (source_ref.ref_id == target_ref.ref_id) {
		throw InvalidInputException("Cannot cherry-pick from branch \"%s\" onto itself", source_branch);
	}

	DuckLakeCherryPickResult result;
	result.cherry_pick_type = "cherry_pick";
	result.source_branch = source_ref.ref_name;
	result.target_branch = target_ref.ref_name;
	result.source_snapshot = snapshot_id;
	result.source_head = source_ref.snapshot_id;
	result.target_head = target_ref.snapshot_id;
	result.new_target_head = target_ref.snapshot_id;
	result.source_branch_id = source_ref.ref_id;
	result.target_branch_id = target_ref.ref_id;
	result.dry_run = dry_run;
	result.ancestor_snapshot = GetMergeBaseSnapshot(source_ref.ref_id, target_ref.ref_id);

	auto picked_snapshots = GetAllSnapshots(
	    StringUtil::Format("s.snapshot_id = %llu AND s.branch_id = %llu", snapshot_id, source_ref.ref_id));
	if (picked_snapshots.empty()) {
		throw InvalidInputException("Snapshot %llu does not belong to source branch \"%s\"", snapshot_id,
		                            source_ref.ref_name);
	}
	if (snapshot_id > source_ref.snapshot_id) {
		throw InvalidInputException("Snapshot %llu is beyond source branch \"%s\" head %llu", snapshot_id,
		                            source_ref.ref_name, source_ref.snapshot_id);
	}
	auto &picked = picked_snapshots[0];
	auto source_delta = SnapshotChangeInformation::ParseChangesMade(picked.change_info.changes_made);
	auto unsupported = CherryPickUnsupportedReason(source_delta);
	if (!unsupported.empty()) {
		throw NotImplementedException(
		    "ducklake_cherry_pick currently supports DML inserts/deletes (data-file and inlined) and compose-clean "
		    "DDL; snapshot %llu contains %s",
		    snapshot_id, unsupported);
	}

	auto target_delta = GetBranchChangesSince(target_ref.ref_id, result.ancestor_snapshot, target_ref.snapshot_id);
	auto conflicts = DetectConflicts(source_delta, target_delta, ConflictReportStyle::MERGE);
	AppendDeleteConflicts(*this, source_ref, target_ref, snapshot_id == 0 ? 0 : snapshot_id - 1, snapshot_id,
	                      result.ancestor_snapshot, target_ref.snapshot_id, source_delta, target_delta, conflicts);
	if (!conflicts.empty()) {
		result.cherry_pick_type = "conflicts";
		result.messages = std::move(conflicts);
		if (!dry_run) {
			throw TransactionException("Cherry-pick conflict applying snapshot %llu from branch \"%s\" onto \"%s\":\n%s",
			                           snapshot_id, source_branch, target_branch, StringUtil::Join(result.messages, "\n"));
		}
		return result;
	}

	auto target_snapshot = ReadMetadataSnapshot(*this, target_ref.snapshot_id, target_ref.ref_id, "cherry-pick",
	                                           "target head");
	set<idx_t> source_data_files_created_by_range;
	auto source_objects_created_by_range = ReadSourceObjectsCreatedAt(*this, source_ref, snapshot_id);
	ValidateCherryPickCreatedObjects(*this, source_ref, target_ref, target_snapshot, snapshot_id, source_delta,
	                                 "cherry-pick", source_objects_created_by_range);
	ValidateCherryPickDependencies(*this, source_ref, target_ref, target_snapshot, snapshot_id, source_delta,
	                               "cherry-pick", source_data_files_created_by_range,
	                               source_objects_created_by_range);

	result.messages.push_back(StringUtil::Format("Cherry-pick snapshot: %llu", snapshot_id));
	result.messages.push_back(StringUtil::Format("Source branch: %s", source_ref.ref_name));
	result.messages.push_back(StringUtil::Format("Target branch: %s", target_ref.ref_name));
	result.messages.push_back(StringUtil::Format("Ancestor snapshot: %llu", result.ancestor_snapshot));
	result.messages.push_back(StringUtil::Format("Target head: %llu", target_ref.snapshot_id));
	if (dry_run) {
		result.messages.push_back("dry_run=true - no metadata changes applied");
		return result;
	}

	PrepareCherryPickApplyMaps(*this);
	string extra = StringUtil::Format("cherry_pick_source=%s,cherry_pick_snapshot=%llu", source_ref.ref_name,
	                                  snapshot_id);
	bool shared_layout = transaction.GetCatalog().GetInliningLayout() == "shared_table";
	auto applied = ApplyCherryPickSnapshot(
	    *this, source_ref, target_ref, picked, target_snapshot, source_delta,
	    StringUtil::Format("Cherry-pick snapshot %llu from branch %s", snapshot_id, source_ref.ref_name), extra,
	    "cherry-pick", shared_layout);
	CleanupCherryPickApplyMaps(*this);

	result.new_target_head = applied.snapshot.snapshot_id;
	result.messages.push_back(StringUtil::Format("New target head: %llu", applied.snapshot.snapshot_id));
	result.messages.push_back(StringUtil::Format("Copied data files: %llu", applied.data_file_count));
	result.messages.push_back(StringUtil::Format("Copied delete files: %llu", applied.delete_file_count));
	return result;
}

DuckLakeTransplantResult DuckLakeMetadataManager::Transplant(const string &source_branch, idx_t start_snapshot,
                                                             idx_t end_snapshot, const string &target_branch,
                                                             bool dry_run) {
	if (!transaction.GetCatalog().SupportsWritableBranches()) {
		throw InvalidInputException(
		    "ducklake_transplant requires DuckLake catalog version >= 1.1-dev3. "
		    "Re-ATTACH with AUTOMATIC_MIGRATION TRUE to upgrade.");
	}
	if (end_snapshot < start_snapshot) {
		throw InvalidInputException("ducklake_transplant start_snapshot must be <= end_snapshot");
	}

	DuckLakeRefInfo source_ref;
	DuckLakeRefInfo target_ref;
	if (!ResolveBranchRef(*this, source_branch, source_ref)) {
		throw InvalidInputException("No branch named \"%s\" exists", source_branch);
	}
	if (!ResolveBranchRef(*this, target_branch, target_ref)) {
		throw InvalidInputException("No branch named \"%s\" exists", target_branch);
	}
	if (source_ref.ref_id == target_ref.ref_id) {
		throw InvalidInputException("Cannot transplant from branch \"%s\" onto itself", source_branch);
	}
	if (end_snapshot > source_ref.snapshot_id) {
		throw InvalidInputException("Snapshot %llu is beyond source branch \"%s\" head %llu", end_snapshot,
		                            source_ref.ref_name, source_ref.snapshot_id);
	}

	DuckLakeTransplantResult result;
	result.transplant_type = "transplant";
	result.source_branch = source_ref.ref_name;
	result.target_branch = target_ref.ref_name;
	result.start_snapshot = start_snapshot;
	result.end_snapshot = end_snapshot;
	result.source_head = source_ref.snapshot_id;
	result.target_head = target_ref.snapshot_id;
	result.new_target_head = target_ref.snapshot_id;
	result.source_branch_id = source_ref.ref_id;
	result.target_branch_id = target_ref.ref_id;
	result.dry_run = dry_run;
	result.ancestor_snapshot = GetMergeBaseSnapshot(source_ref.ref_id, target_ref.ref_id);

	auto picked_snapshots = GetAllSnapshots(StringUtil::Format(
	    "s.snapshot_id >= %llu AND s.snapshot_id <= %llu AND s.branch_id = %llu", start_snapshot, end_snapshot,
	    source_ref.ref_id));
	if (picked_snapshots.empty()) {
		throw InvalidInputException("No source-owned snapshots from branch \"%s\" in range [%llu, %llu]",
		                            source_ref.ref_name, start_snapshot, end_snapshot);
	}
	if (picked_snapshots.front().id != start_snapshot || picked_snapshots.back().id != end_snapshot) {
		throw InvalidInputException(
		    "Transplant range endpoints must belong to source branch \"%s\" (start %llu, end %llu)",
		    source_ref.ref_name, start_snapshot, end_snapshot);
	}

	SnapshotChangeInformation source_delta;
	vector<SnapshotChangeInformation> snapshot_deltas;
	for (auto &snapshot : picked_snapshots) {
		auto parsed = SnapshotChangeInformation::ParseChangesMade(snapshot.change_info.changes_made);
		auto unsupported_snapshot = CherryPickUnsupportedReason(parsed);
		if (!unsupported_snapshot.empty()) {
			throw NotImplementedException(
			    "ducklake_transplant currently supports DML inserts/deletes (data-file and inlined) and compose-clean "
			    "DDL; snapshot %llu contains %s",
			    snapshot.id, unsupported_snapshot);
		}
		MergeSnapshotChangeInformation(source_delta, parsed);
		snapshot_deltas.push_back(std::move(parsed));
	}
	auto unsupported = CherryPickUnsupportedReason(source_delta);
	if (!unsupported.empty()) {
		throw NotImplementedException(
		    "ducklake_transplant currently supports DML inserts/deletes (data-file and inlined) and compose-clean "
		    "DDL; range [%llu, %llu] contains %s",
		    start_snapshot, end_snapshot, unsupported);
	}

	auto target_delta = GetBranchChangesSince(target_ref.ref_id, result.ancestor_snapshot, target_ref.snapshot_id);
	auto conflicts = DetectConflicts(source_delta, target_delta, ConflictReportStyle::MERGE);
	AppendDeleteConflicts(*this, source_ref, target_ref, start_snapshot == 0 ? 0 : start_snapshot - 1,
	                      end_snapshot, result.ancestor_snapshot, target_ref.snapshot_id, source_delta, target_delta,
	                      conflicts);
	if (!conflicts.empty()) {
		result.transplant_type = "conflicts";
		result.messages = std::move(conflicts);
		if (!dry_run) {
			throw TransactionException(
			    "Transplant conflict applying snapshots %llu..%llu from branch \"%s\" onto \"%s\":\n%s",
			    start_snapshot, end_snapshot, source_branch, target_branch, StringUtil::Join(result.messages, "\n"));
		}
		return result;
	}

	auto target_snapshot = ReadMetadataSnapshot(*this, target_ref.snapshot_id, target_ref.ref_id, "transplant",
	                                           "target head");
	set<idx_t> source_data_files_created_by_range;
	CherryPickCreatedObjects source_objects_created_by_range;
	for (idx_t i = 0; i < picked_snapshots.size(); i++) {
		auto current_created = ReadSourceObjectsCreatedAt(*this, source_ref, picked_snapshots[i].id);
		auto created_for_validation = source_objects_created_by_range;
		MergeCreatedObjects(created_for_validation, current_created);
		ValidateCherryPickCreatedObjects(*this, source_ref, target_ref, target_snapshot, picked_snapshots[i].id,
		                                 snapshot_deltas[i], "transplant", created_for_validation);
		ValidateCherryPickDependencies(*this, source_ref, target_ref, target_snapshot, picked_snapshots[i].id,
		                               snapshot_deltas[i], "transplant", source_data_files_created_by_range,
		                               created_for_validation);
		AddSourceDataFilesCreatedAt(*this, source_ref, picked_snapshots[i].id, source_data_files_created_by_range);
		MergeCreatedObjects(source_objects_created_by_range, current_created);
	}

	result.messages.push_back(StringUtil::Format("Transplant range: %llu..%llu", start_snapshot, end_snapshot));
	result.messages.push_back(StringUtil::Format("Source branch: %s", source_ref.ref_name));
	result.messages.push_back(StringUtil::Format("Target branch: %s", target_ref.ref_name));
	result.messages.push_back(StringUtil::Format("Ancestor snapshot: %llu", result.ancestor_snapshot));
	result.messages.push_back(StringUtil::Format("Target head: %llu", target_ref.snapshot_id));
	if (dry_run) {
		result.snapshots_applied = picked_snapshots.size();
		result.messages.push_back("dry_run=true - no metadata changes applied");
		return result;
	}

	PrepareCherryPickApplyMaps(*this);
	DuckLakeSnapshot current_target = target_snapshot;
	idx_t total_data_files = 0;
	idx_t total_delete_files = 0;
	bool shared_layout = transaction.GetCatalog().GetInliningLayout() == "shared_table";
	for (idx_t i = 0; i < picked_snapshots.size(); i++) {
		auto &picked = picked_snapshots[i];
		string extra = StringUtil::Format(
		    "transplant_source=%s,transplant_start=%llu,transplant_end=%llu,transplant_snapshot=%llu",
		    source_ref.ref_name, start_snapshot, end_snapshot, picked.id);
		auto applied = ApplyCherryPickSnapshot(
		    *this, source_ref, target_ref, picked, current_target, snapshot_deltas[i],
		    StringUtil::Format("Transplant snapshot %llu from branch %s", picked.id, source_ref.ref_name), extra,
		    "transplant", shared_layout);
		current_target = applied.snapshot;
		total_data_files += applied.data_file_count;
		total_delete_files += applied.delete_file_count;
		result.snapshots_applied++;
		result.messages.push_back(
		    StringUtil::Format("Applied source snapshot %llu as target snapshot %llu", picked.id,
		                       applied.snapshot.snapshot_id));
	}
	CleanupCherryPickApplyMaps(*this);

	result.new_target_head = current_target.snapshot_id;
	result.messages.push_back(StringUtil::Format("New target head: %llu", current_target.snapshot_id));
	result.messages.push_back(StringUtil::Format("Copied data files: %llu", total_data_files));
	result.messages.push_back(StringUtil::Format("Copied delete files: %llu", total_delete_files));
	return result;
}

vector<DuckLakeDiffResult> DuckLakeMetadataManager::DiffRefs(const string &ref_a, const string &ref_b) {
	if (!transaction.GetCatalog().SupportsWritableBranches()) {
		throw InvalidInputException(
		    "ducklake_diff requires DuckLake catalog version >= 1.1-dev3. "
		    "Re-ATTACH with AUTOMATIC_MIGRATION TRUE to upgrade.");
	}
	DuckLakeRefInfo a_ref;
	DuckLakeRefInfo b_ref;
	if (!ResolveAnyRef(*this, ref_a, a_ref)) {
		throw InvalidInputException("No branch or tag named \"%s\" exists", ref_a);
	}
	if (!ResolveAnyRef(*this, ref_b, b_ref)) {
		throw InvalidInputException("No branch or tag named \"%s\" exists", ref_b);
	}

	auto a_snapshot = ReadRefSnapshot(*this, a_ref, "diff");
	auto b_snapshot = ReadRefSnapshot(*this, b_ref, "diff");
	auto a_catalog = GetCatalogForSnapshot(a_snapshot);
	auto b_catalog = GetCatalogForSnapshot(b_snapshot);

	map<SchemaIndex, string> a_schema_names;
	map<SchemaIndex, string> b_schema_names;
	map<SchemaIndex, string> a_schema_uuids;
	map<SchemaIndex, string> b_schema_uuids;
	map<string, const DuckLakeSchemaInfo *> a_schemas;
	map<string, const DuckLakeSchemaInfo *> b_schemas;
	for (auto &schema : a_catalog.schemas) {
		a_schema_names[schema.id] = schema.name;
		a_schema_uuids[schema.id] = schema.uuid;
		a_schemas[schema.uuid] = &schema;
	}
	for (auto &schema : b_catalog.schemas) {
		b_schema_names[schema.id] = schema.name;
		b_schema_uuids[schema.id] = schema.uuid;
		b_schemas[schema.uuid] = &schema;
	}

	vector<DuckLakeDiffResult> results;
	auto add_result = [&](string object_type, string schema_name, string object_name, string change, string detail) {
		DuckLakeDiffResult result;
		result.object_type = std::move(object_type);
		result.schema_name = std::move(schema_name);
		result.object_name = std::move(object_name);
		result.change = std::move(change);
		result.detail = std::move(detail);
		results.push_back(std::move(result));
	};

	for (auto &entry : b_schemas) {
		if (a_schemas.find(entry.first) == a_schemas.end()) {
			add_result("schema", entry.second->name, entry.second->name, "added",
			           StringUtil::Format("uuid=%s", entry.first));
		}
	}
	for (auto &entry : a_schemas) {
		if (b_schemas.find(entry.first) == b_schemas.end()) {
			add_result("schema", entry.second->name, entry.second->name, "dropped",
			           StringUtil::Format("uuid=%s", entry.first));
		}
	}
	for (auto &entry : b_schemas) {
		auto a_entry = a_schemas.find(entry.first);
		if (a_entry != a_schemas.end() && a_entry->second->name != entry.second->name) {
			add_result("schema", entry.second->name, entry.second->name, "altered",
			           StringUtil::Format("renamed from %s to %s; uuid=%s", a_entry->second->name,
			                              entry.second->name, entry.first));
		}
	}

	map<string, const DuckLakeTableInfo *> a_tables;
	map<string, const DuckLakeTableInfo *> b_tables;
	for (auto &table : a_catalog.tables) {
		a_tables[table.uuid] = &table;
	}
	for (auto &table : b_catalog.tables) {
		b_tables[table.uuid] = &table;
	}
	for (auto &entry : b_tables) {
		auto schema_name = SchemaNameForId(b_schema_names, entry.second->schema_id);
		if (a_tables.find(entry.first) == a_tables.end()) {
			add_result("table", schema_name, entry.second->name, "added",
			           StringUtil::Format("uuid=%s; name=%s", entry.first, JoinName(schema_name, entry.second->name)));
		}
	}
	for (auto &entry : a_tables) {
		auto schema_name = SchemaNameForId(a_schema_names, entry.second->schema_id);
		if (b_tables.find(entry.first) == b_tables.end()) {
			add_result("table", schema_name, entry.second->name, "dropped",
			           StringUtil::Format("uuid=%s; name=%s", entry.first, JoinName(schema_name, entry.second->name)));
		}
	}
	for (auto &entry : b_tables) {
		auto a_entry = a_tables.find(entry.first);
		if (a_entry == a_tables.end()) {
			continue;
		}
		auto a_table = a_entry->second;
		auto b_table = entry.second;
		auto a_schema_name = SchemaNameForId(a_schema_names, a_table->schema_id);
		auto b_schema_name = SchemaNameForId(b_schema_names, b_table->schema_id);
		auto a_schema_uuid = SchemaUUIDForId(a_schema_uuids, a_table->schema_id);
		auto b_schema_uuid = SchemaUUIDForId(b_schema_uuids, b_table->schema_id);
		auto a_columns = ColumnSignature(a_table->columns);
		auto b_columns = ColumnSignature(b_table->columns);
		vector<string> details;
		if (a_table->name != b_table->name || a_schema_uuid != b_schema_uuid) {
			details.push_back(StringUtil::Format("renamed/moved from %s to %s",
			                                     JoinName(a_schema_name, a_table->name),
			                                     JoinName(b_schema_name, b_table->name)));
		}
		if (a_columns != b_columns) {
			details.push_back(StringUtil::Format("columns changed from [%s] to [%s]", a_columns, b_columns));
		}
		if (!details.empty()) {
			details.push_back(StringUtil::Format("uuid=%s", entry.first));
			add_result("table", b_schema_name, b_table->name, "altered", StringUtil::Join(details, "; "));
		}
	}

	map<string, const DuckLakeViewInfo *> a_views;
	map<string, const DuckLakeViewInfo *> b_views;
	for (auto &view : a_catalog.views) {
		a_views[view.uuid] = &view;
	}
	for (auto &view : b_catalog.views) {
		b_views[view.uuid] = &view;
	}
	for (auto &entry : b_views) {
		auto schema_name = SchemaNameForId(b_schema_names, entry.second->schema_id);
		if (a_views.find(entry.first) == a_views.end()) {
			add_result("view", schema_name, entry.second->name, "added",
			           StringUtil::Format("uuid=%s; name=%s", entry.first, JoinName(schema_name, entry.second->name)));
		}
	}
	for (auto &entry : a_views) {
		auto schema_name = SchemaNameForId(a_schema_names, entry.second->schema_id);
		if (b_views.find(entry.first) == b_views.end()) {
			add_result("view", schema_name, entry.second->name, "dropped",
			           StringUtil::Format("uuid=%s; name=%s", entry.first, JoinName(schema_name, entry.second->name)));
		}
	}
	for (auto &entry : b_views) {
		auto a_entry = a_views.find(entry.first);
		if (a_entry == a_views.end()) {
			continue;
		}
		auto a_view = a_entry->second;
		auto b_view = entry.second;
		auto a_schema_name = SchemaNameForId(a_schema_names, a_view->schema_id);
		auto b_schema_name = SchemaNameForId(b_schema_names, b_view->schema_id);
		auto a_schema_uuid = SchemaUUIDForId(a_schema_uuids, a_view->schema_id);
		auto b_schema_uuid = SchemaUUIDForId(b_schema_uuids, b_view->schema_id);
		vector<string> details;
		if (a_view->name != b_view->name || a_schema_uuid != b_schema_uuid) {
			details.push_back(StringUtil::Format("renamed/moved from %s to %s",
			                                     JoinName(a_schema_name, a_view->name),
			                                     JoinName(b_schema_name, b_view->name)));
		}
		if (a_view->dialect != b_view->dialect || a_view->sql != b_view->sql ||
		    a_view->column_aliases != b_view->column_aliases) {
			details.push_back("definition changed");
		}
		if (!details.empty()) {
			details.push_back(StringUtil::Format("uuid=%s", entry.first));
			add_result("view", b_schema_name, b_view->name, "altered", StringUtil::Join(details, "; "));
		}
	}

	std::sort(results.begin(), results.end(), [](const DuckLakeDiffResult &a, const DuckLakeDiffResult &b) {
		return std::tie(a.object_type, a.schema_name, a.object_name, a.change, a.detail) <
		       std::tie(b.object_type, b.schema_name, b.object_name, b.change, b.detail);
	});
	return results;
}

namespace {

struct InlinedLayoutRegistration {
	idx_t table_id;
	string table_name;
	idx_t schema_version;
	idx_t branch_id;
};

static string NormalizeInliningLayout(const string &layout) {
	auto normalized = StringUtil::Lower(layout);
	if (normalized != "shared_table" && normalized != "per_branch_table") {
		throw InvalidInputException("inlining_layout must be 'shared_table' or 'per_branch_table', got \"%s\"", layout);
	}
	return normalized;
}

static string ConversionTempTableName(idx_t table_id, idx_t schema_version) {
	return StringUtil::Format("__ducklake_convert_inlined_%llu_%llu", table_id, schema_version);
}

} // namespace

DuckLakeConvertInliningLayoutResult DuckLakeMetadataManager::ConvertInliningLayout(const string &target_layout_p,
                                                                                   bool dry_run) {
	if (!transaction.GetCatalog().SupportsWritableBranches()) {
		throw InvalidInputException(
		    "ducklake_convert_inlining_layout requires DuckLake catalog version >= 1.1-dev3. "
		    "Re-ATTACH with AUTOMATIC_MIGRATION TRUE to upgrade.");
	}
	auto target_layout = NormalizeInliningLayout(target_layout_p);
	auto source_layout = transaction.GetCatalog().GetInliningLayout();
	DuckLakeConvertInliningLayoutResult result;
	result.source_layout = source_layout;
	result.target_layout = target_layout;
	result.dry_run = dry_run;
	if (source_layout == target_layout) {
		result.messages.push_back(StringUtil::Format("inlining_layout is already %s", target_layout));
		return result;
	}

	auto registrations_result = Query(R"(
SELECT table_id, table_name, schema_version, branch_id
FROM {METADATA_CATALOG}.ducklake_inlined_data_tables
ORDER BY table_id, schema_version, branch_id, table_name
)");
	if (registrations_result->HasError()) {
		registrations_result->GetErrorObject().Throw("Failed to read inlined-data registrations in DuckLake: ");
	}
	vector<InlinedLayoutRegistration> registrations;
	for (auto &row : *registrations_result) {
		registrations.push_back({row.GetValue<idx_t>(0), row.GetValue<string>(1), row.GetValue<idx_t>(2),
		                         row.GetValue<idx_t>(3)});
	}

	string sql;
	set<string> copied_targets;
	set<string> dropped_sources;

	if (source_layout == "shared_table" && target_layout == "per_branch_table") {
		map<string, string> shared_main_temps;
		for (auto &reg : registrations) {
			set<idx_t> branch_ids;
			branch_ids.insert(reg.branch_id);
			auto branch_result =
			    Query(StringUtil::Format("SELECT DISTINCT branch_id FROM {METADATA_CATALOG}.%s",
			                             SQLIdentifier(reg.table_name)));
			if (branch_result->HasError()) {
				branch_result->GetErrorObject().Throw("Failed to read shared inlined-data table branches: ");
			}
			for (auto &row : *branch_result) {
				branch_ids.insert(row.GetValue<idx_t>(0));
			}
			for (auto branch_id : branch_ids) {
				auto target_name = InlinedTableNameFor(reg.table_id, reg.schema_version, branch_id, false);
				auto physical_target_name = target_name;
				if (branch_id == 0 && target_name == reg.table_name) {
					physical_target_name = ConversionTempTableName(reg.table_id, reg.schema_version);
					shared_main_temps[reg.table_name] = physical_target_name;
				}
				auto target_key = StringUtil::Format("%llu:%llu:%llu", reg.table_id, reg.schema_version, branch_id);
				if (copied_targets.insert(target_key).second) {
					auto count_result = Query(StringUtil::Format(
					    "SELECT COUNT(*) FROM {METADATA_CATALOG}.%s WHERE branch_id = %llu",
					    SQLIdentifier(reg.table_name), branch_id));
					if (count_result->HasError()) {
						count_result->GetErrorObject().Throw("Failed to count shared inlined-data rows: ");
					}
					auto row_count = count_result->Fetch()->GetValue(0, 0).GetValue<idx_t>();
					result.messages.push_back(StringUtil::Format("copy %llu rows from %s(branch %llu) to %s", row_count,
					                                             reg.table_name, branch_id, target_name));
					sql += StringUtil::Format(R"(
CREATE OR REPLACE TABLE {METADATA_CATALOG}.%s AS
SELECT row_id, begin_snapshot, end_snapshot, * EXCLUDE(row_id, begin_snapshot, end_snapshot, branch_id)
FROM {METADATA_CATALOG}.%s
WHERE branch_id = %llu;
)",
					                          SQLIdentifier(physical_target_name), SQLIdentifier(reg.table_name),
					                          branch_id);
				}
				sql += StringUtil::Format(R"(
UPDATE {METADATA_CATALOG}.ducklake_inlined_data_tables
SET table_name = %s
WHERE table_id = %llu AND schema_version = %llu AND branch_id = %llu;
INSERT INTO {METADATA_CATALOG}.ducklake_inlined_data_tables(table_id, table_name, schema_version, branch_id)
SELECT %llu, %s, %llu, %llu
WHERE NOT EXISTS (
  SELECT 1 FROM {METADATA_CATALOG}.ducklake_inlined_data_tables
  WHERE table_id = %llu AND schema_version = %llu AND branch_id = %llu
);
)",
				                          SQLString(target_name), reg.table_id, reg.schema_version, branch_id,
				                          reg.table_id, SQLString(target_name), reg.schema_version, branch_id,
				                          reg.table_id, reg.schema_version, branch_id);
			}
			if (dropped_sources.insert(reg.table_name).second) {
				result.messages.push_back(StringUtil::Format("drop shared physical table %s", reg.table_name));
				sql += StringUtil::Format("DROP TABLE IF EXISTS {METADATA_CATALOG}.%s;\n",
				                          SQLIdentifier(reg.table_name));
				auto temp_entry = shared_main_temps.find(reg.table_name);
				if (temp_entry != shared_main_temps.end()) {
					sql += StringUtil::Format("ALTER TABLE {METADATA_CATALOG}.%s RENAME TO %s;\n",
					                          SQLIdentifier(temp_entry->second), SQLIdentifier(reg.table_name));
				}
			}
		}
	} else if (source_layout == "per_branch_table" && target_layout == "shared_table") {
		map<pair<idx_t, idx_t>, vector<InlinedLayoutRegistration>> by_table_version;
		for (auto &reg : registrations) {
			by_table_version[make_pair(reg.table_id, reg.schema_version)].push_back(reg);
		}
		for (auto &entry : by_table_version) {
			auto table_id = entry.first.first;
			auto schema_version = entry.first.second;
			auto shared_name = InlinedTableNameFor(table_id, schema_version);
			auto temp_name = ConversionTempTableName(table_id, schema_version);
			auto &regs = entry.second;
			if (regs.empty()) {
				continue;
			}
			result.messages.push_back(StringUtil::Format("create shared physical table %s", shared_name));
			sql += StringUtil::Format(R"(
CREATE OR REPLACE TABLE {METADATA_CATALOG}.%s AS
SELECT row_id, begin_snapshot, end_snapshot, CAST(%llu AS BIGINT) AS branch_id,
       * EXCLUDE(row_id, begin_snapshot, end_snapshot)
FROM {METADATA_CATALOG}.%s
WHERE 1 = 0;
)",
			                          SQLIdentifier(temp_name), regs[0].branch_id, SQLIdentifier(regs[0].table_name));
			for (auto &reg : regs) {
				auto count_result =
				    Query(StringUtil::Format("SELECT COUNT(*) FROM {METADATA_CATALOG}.%s",
				                             SQLIdentifier(reg.table_name)));
				if (count_result->HasError()) {
					count_result->GetErrorObject().Throw("Failed to count per-branch inlined-data rows: ");
				}
				auto row_count = count_result->Fetch()->GetValue(0, 0).GetValue<idx_t>();
				result.messages.push_back(StringUtil::Format("copy %llu rows from %s as branch %llu", row_count,
				                                             reg.table_name, reg.branch_id));
				sql += StringUtil::Format(R"(
INSERT INTO {METADATA_CATALOG}.%s
SELECT row_id, begin_snapshot, end_snapshot, CAST(%llu AS BIGINT) AS branch_id,
       * EXCLUDE(row_id, begin_snapshot, end_snapshot)
FROM {METADATA_CATALOG}.%s;
UPDATE {METADATA_CATALOG}.ducklake_inlined_data_tables
SET table_name = %s
WHERE table_id = %llu AND schema_version = %llu AND branch_id = %llu;
)",
				                          SQLIdentifier(temp_name), reg.branch_id, SQLIdentifier(reg.table_name),
				                          SQLString(shared_name), reg.table_id, reg.schema_version, reg.branch_id);
				if (dropped_sources.insert(reg.table_name).second) {
					sql += StringUtil::Format("DROP TABLE IF EXISTS {METADATA_CATALOG}.%s;\n",
					                          SQLIdentifier(reg.table_name));
				}
			}
			sql += StringUtil::Format("ALTER TABLE {METADATA_CATALOG}.%s RENAME TO %s;\n", SQLIdentifier(temp_name),
			                          SQLIdentifier(shared_name));
		}
	} else {
		throw InternalException("Unsupported inlining_layout conversion from %s to %s", source_layout, target_layout);
	}

	result.messages.push_back(StringUtil::Format("%s set inlining_layout to %s",
	                                             dry_run ? "would" : "will", target_layout));
	if (dry_run) {
		return result;
	}
	if (!sql.empty()) {
		auto exec_result = Execute(sql);
		if (exec_result->HasError()) {
			exec_result->GetErrorObject().Throw("Failed to convert inlining layout in DuckLake: ");
		}
	}
	DuckLakeConfigOption config_option;
	config_option.option.key = "inlining_layout";
	config_option.option.value = target_layout;
	auto config_result = Execute(StringUtil::Format(R"(
UPDATE {METADATA_CATALOG}.ducklake_metadata
SET value = %s
WHERE key = 'inlining_layout' AND scope IS NULL;
INSERT INTO {METADATA_CATALOG}.ducklake_metadata(key, value, scope, scope_id)
SELECT 'inlining_layout', %s, NULL, NULL
WHERE NOT EXISTS (
  SELECT 1 FROM {METADATA_CATALOG}.ducklake_metadata WHERE key = 'inlining_layout' AND scope IS NULL
);
)",
	                                             SQLString(target_layout), SQLString(target_layout)));
	if (config_result->HasError()) {
		config_result->GetErrorObject().Throw("Failed to update inlining_layout option in DuckLake: ");
	}
	transaction.GetCatalog().SetConfigOption(config_option);
	for (auto &reg : registrations) {
		transaction.GetCatalog().InvalidateSchemaCache(reg.schema_version);
	}
	ClearCache();
	return result;
}

DuckLakeMetadata DuckLakeMetadataManager::LoadDuckLake() {
	auto result = Query(R"(
SELECT key, value, scope, scope_id FROM {METADATA_CATALOG}.ducklake_metadata
)");
	if (result->HasError()) {
		// preserve the original error in case the fallback also fails
		auto original_error = result->GetErrorObject().RawMessage();
		// we might be loading from a v0.1 database - if so we don't have scope yet
		result = Query(R"(
SELECT key, value FROM {METADATA_CATALOG}.ducklake_metadata
)");
		if (result->HasError()) {
			auto fallback_error = result->GetErrorObject().RawMessage();
			throw IOException("Failed to load existing DuckLake: %s\nFollowed by: %s", original_error, fallback_error);
		}
	}
	DuckLakeMetadata metadata;
	for (auto &row : *result) {
		DuckLakeTag tag;
		tag.key = row.GetValue<string>(0);
		tag.value = row.GetValue<string>(1);
		if (result->ColumnCount() == 2 || row.IsNull(2)) {
			// scope is NULL: global tag
			// global tag
			metadata.tags.push_back(std::move(tag));
			continue;
		}
		auto scope = row.GetValue<string>(2);
		if (scope == "schema") {
			// schema-level setting
			DuckLakeSchemaSetting schema_setting;
			schema_setting.schema_id = SchemaIndex(row.GetValue<idx_t>(3));
			schema_setting.tag = std::move(tag);
			metadata.schema_settings.push_back(std::move(schema_setting));
			continue;
		}
		if (scope == "table") {
			// table-level setting
			DuckLakeTableSetting table_setting;
			table_setting.table_id = TableIndex(row.GetValue<idx_t>(3));
			table_setting.tag = std::move(tag);
			metadata.table_settings.push_back(std::move(table_setting));
			continue;
		}
		throw InvalidInputException("Unsupported setting scope %s - only schema/table are supported", scope);
	}
	return metadata;
}

static bool AddChildColumn(vector<DuckLakeColumnInfo> &columns, FieldIndex parent_id, DuckLakeColumnInfo &column_info) {
	for (auto &col : columns) {
		if (col.id == parent_id) {
			col.children.push_back(std::move(column_info));
			return true;
		}
		if (AddChildColumn(col.children, parent_id, column_info)) {
			return true;
		}
	}
	return false;
}

vector<DuckLakeTag> DuckLakeMetadataManager::LoadTags(const Value &tag_map) {
	vector<DuckLakeTag> result;
	for (auto &tag : ListValue::GetChildren(tag_map)) {
		auto &struct_children = StructValue::GetChildren(tag);
		if (struct_children[1].IsNull()) {
			continue;
		}
		DuckLakeTag tag_info;
		tag_info.key = struct_children[0].ToString();
		tag_info.value = struct_children[1].ToString();
		result.push_back(std::move(tag_info));
	}
	return result;
}

vector<DuckLakeViewColumnTag> DuckLakeMetadataManager::LoadViewColumnTags(const Value &list) {
	vector<DuckLakeViewColumnTag> result;
	if (list.IsNull()) {
		return result;
	}
	for (auto &val : ListValue::GetChildren(list)) {
		auto &struct_children = StructValue::GetChildren(val);
		DuckLakeViewColumnTag tag;
		tag.column_name = struct_children[0].ToString();
		tag.key = struct_children[1].ToString();
		if (struct_children.size() > 2) {
			tag.value = struct_children[2].IsNull() ? Value() : struct_children[2];
		} else {
			tag.value = Value();
		}
		result.push_back(std::move(tag));
	}
	return result;
}

vector<DuckLakeInlinedTableInfo> DuckLakeMetadataManager::LoadInlinedDataTables(const Value &list) {
	vector<DuckLakeInlinedTableInfo> result;
	for (auto &val : ListValue::GetChildren(list)) {
		auto &struct_children = StructValue::GetChildren(val);
		DuckLakeInlinedTableInfo inlined_data_table;
		inlined_data_table.table_name = StringValue::Get(struct_children[0]);
		inlined_data_table.schema_version = struct_children[1].GetValue<idx_t>();
		result.push_back(std::move(inlined_data_table));
	}
	return result;
}

vector<DuckLakeMacroImplementation> DuckLakeMetadataManager::LoadMacroImplementations(const Value &list) {
	vector<DuckLakeMacroImplementation> result;
	for (auto &val : ListValue::GetChildren(list)) {
		auto &struct_children = StructValue::GetChildren(val);
		DuckLakeMacroImplementation impl_info;
		impl_info.dialect = StringValue::Get(struct_children[0]);
		impl_info.sql = StringValue::Get(struct_children[1]);
		impl_info.type = StringValue::Get(struct_children[2]);
		auto param_list = struct_children[3].GetValue<Value>();
		if (!param_list.IsNull()) {
			for (auto &param_value : ListValue::GetChildren(param_list)) {
				auto &param_struct_children = StructValue::GetChildren(param_value);
				DuckLakeMacroParameters param;
				param.parameter_name = StringValue::Get(param_struct_children[0]);
				param.parameter_type = StringValue::Get(param_struct_children[1]);
				param.default_value = StringValue::Get(param_struct_children[2]);
				param.default_value_type = StringValue::Get(param_struct_children[3]);
				impl_info.parameters.push_back(std::move(param));
			}
		}

		result.push_back(std::move(impl_info));
	}
	return result;
}

idx_t DuckLakeMetadataManager::GetBeginSnapshotForTable(TableIndex table_id) {
	string query = R"(
SELECT begin_snapshot
FROM {METADATA_CATALOG}.ducklake_table
WHERE table_id = {TABLE_ID})";
	query = StringUtil::Replace(query, "{TABLE_ID}", to_string(table_id.index)).c_str();
	auto result = Query(query);
	for (auto &row : *result) {
		return row.GetValue<idx_t>(0);
	}
	throw InternalException("Table %llu does not exist", table_id.index);
}

idx_t DuckLakeMetadataManager::GetBeginSnapshotForSchemaVersion(TableIndex table_id, idx_t schema_version) {
	string query = R"(
SELECT begin_snapshot
FROM {METADATA_CATALOG}.ducklake_schema_versions
WHERE table_id = {TABLE_ID} AND schema_version = {SCHEMA_VERSION})";
	query = StringUtil::Replace(query, "{TABLE_ID}", to_string(table_id.index));
	query = StringUtil::Replace(query, "{SCHEMA_VERSION}", to_string(schema_version));
	auto result = Query(query);
	for (auto &row : *result) {
		return row.GetValue<idx_t>(0);
	}
	// We need to fallback to GetBeginSnapshotForTable if this table doesnt have an alter yet
	return GetBeginSnapshotForTable(table_id);
}

idx_t DuckLakeMetadataManager::GetEffectiveInlinedReadSnapshot(DuckLakeSnapshot snapshot,
                                                               const string &inlined_table_name) {
	// Without writable branches there is no lineage cap — use the snapshot as-is.
	if (!transaction.GetCatalog().SupportsWritableBranches()) {
		return snapshot.snapshot_id;
	}
	// Cap visibility of ancestor-owned inlined rows at the fork point (max_visible_snapshot).
	auto result = Query(snapshot, StringUtil::Format(R"(
SELECT LEAST(%llu::BIGINT, COALESCE(bl.max_visible_snapshot, %llu::BIGINT))
FROM {METADATA_CATALOG}.ducklake_inlined_data_tables idt
LEFT JOIN {METADATA_CATALOG}.ducklake_branch_lineage bl
  ON bl.branch_id = {BRANCH_ID} AND bl.ancestor_branch_id = idt.branch_id
WHERE idt.table_name = %s
LIMIT 1;
)",
	                                                 snapshot.snapshot_id, snapshot.snapshot_id,
	                                                 SQLString(inlined_table_name)));
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to resolve effective inlined-data snapshot in DuckLake: ");
	}
	for (auto &row : *result) {
		return row.GetValue<idx_t>(0);
	}
	return snapshot.snapshot_id;
}

string DuckLakeMetadataManager::GetNetDataFileRowCountSql(TableIndex table_id, const string &inlined_deletion_table) {
	// Compute sum(record_count) - sum(delete_count) - inlined_deletions in a single query.
	// Delete files are only counted if their corresponding data file is still visible.
	// (When a data file's end_snapshot is set — e.g. TRUNCATE — associated deletes don't count.)
	string inlined_deletion_subquery = "0";
	if (!inlined_deletion_table.empty()) {
		inlined_deletion_subquery = StringUtil::Format(R"(
COALESCE((SELECT COUNT(*) FROM {METADATA_CATALOG}.%s del
          JOIN {METADATA_CATALOG}.ducklake_data_file data ON del.file_id = data.data_file_id
          WHERE del.begin_snapshot <= {SNAPSHOT_ID}
            AND data.table_id = {TABLE_ID}
            AND {SNAPSHOT_ID} >= data.begin_snapshot
            AND ({SNAPSHOT_ID} < data.end_snapshot OR data.end_snapshot IS NULL)), 0))",
		                                               inlined_deletion_table);
	}
	string query = StringUtil::Format(R"(
SELECT
  COALESCE((SELECT SUM(record_count) FROM {METADATA_CATALOG}.ducklake_data_file
            WHERE table_id = {TABLE_ID}
              AND {SNAPSHOT_ID} >= begin_snapshot
              AND ({SNAPSHOT_ID} < end_snapshot OR end_snapshot IS NULL)), 0)
  -
  COALESCE((SELECT SUM(del.delete_count) FROM {METADATA_CATALOG}.ducklake_delete_file del
            JOIN {METADATA_CATALOG}.ducklake_data_file data ON del.data_file_id = data.data_file_id
            WHERE del.table_id = {TABLE_ID}
              AND {SNAPSHOT_ID} >= del.begin_snapshot
              AND ({SNAPSHOT_ID} < del.end_snapshot OR del.end_snapshot IS NULL)
              AND {SNAPSHOT_ID} >= data.begin_snapshot
              AND ({SNAPSHOT_ID} < data.end_snapshot OR data.end_snapshot IS NULL)), 0)
  -
  %s)",
	                                  inlined_deletion_subquery);
	return StringUtil::Replace(query, "{TABLE_ID}", to_string(table_id.index));
}

idx_t DuckLakeMetadataManager::GetNetDataFileRowCount(TableIndex table_id, DuckLakeSnapshot snapshot) {
	auto query = GetNetDataFileRowCountSql(table_id, GetInlinedDeletionTableName(table_id, snapshot));
	auto result = Query(snapshot, query);
	for (auto &row : *result) {
		return row.GetValue<idx_t>(0);
	}
	return 0;
}

static string SharedInlinedVisibilityPredicate(const string &alias) {
	auto prefix = alias.empty() ? string() : alias + ".";
	return StringUtil::Format(R"(EXISTS (
  SELECT 1 FROM {METADATA_CATALOG}.ducklake_branch_lineage l
  WHERE l.branch_id = {BRANCH_ID}
    AND l.ancestor_branch_id = %sbranch_id
    AND %sbegin_snapshot <= LEAST({SNAPSHOT_ID}, l.max_visible_snapshot)
    AND (%send_snapshot IS NULL OR %send_snapshot > LEAST({SNAPSHOT_ID}, l.max_visible_snapshot))
))",
	                          prefix, prefix, prefix, prefix);
}

string DuckLakeMetadataManager::GetNetInlinedRowCountSql(const string &inlined_table_name, bool shared_layout) {
	auto filter = shared_layout ? " AND " + SharedInlinedVisibilityPredicate("inlined_data") : "";
	return StringUtil::Format(R"(
SELECT COUNT(*)
FROM {METADATA_CATALOG}.%s inlined_data
WHERE {SNAPSHOT_ID} >= begin_snapshot
  AND ({SNAPSHOT_ID} < end_snapshot OR end_snapshot IS NULL)%s)",
	                          SQLIdentifier(inlined_table_name), filter);
}

idx_t DuckLakeMetadataManager::GetNetInlinedRowCount(const string &inlined_table_name, DuckLakeSnapshot snapshot) {
	const bool shared_layout =
	    transaction.GetCatalog().SupportsWritableBranches() && transaction.GetCatalog().GetInliningLayout() == "shared_table";
	DuckLakeSnapshot read_snapshot = snapshot;
	if (!shared_layout) {
		read_snapshot.snapshot_id = GetEffectiveInlinedReadSnapshot(snapshot, inlined_table_name);
	}
	auto result = Query(read_snapshot, GetNetInlinedRowCountSql(inlined_table_name, shared_layout));
	for (auto &row : *result) {
		return row.GetValue<idx_t>(0);
	}
	return 0;
}

string DuckLakeMetadataManager::GetTableColumnSchemaSql(TableIndex table_id) {
	// Return the full flattened schema: top-level roots (parent_column IS NULL) and every nested leaf, each with its
	// own column_id (== FieldIndex) and its own leaf column_type. Callers distinguish roots from leaves via
	// parent_column. column_order == column_id for every row, so the ordering stays deterministic.
	return StringUtil::Format(R"(
SELECT column_id, column_name, column_type, parent_column
FROM {METADATA_CATALOG}.ducklake_column
WHERE table_id = %d
  AND {SNAPSHOT_ID} >= begin_snapshot
  AND ({SNAPSHOT_ID} < end_snapshot OR end_snapshot IS NULL)
ORDER BY column_order)",
	                          table_id.index);
}

string DuckLakeMetadataManager::GetInlinedTableNamesSql(TableIndex table_id) {
	return StringUtil::Format(R"(
SELECT DISTINCT table_name
FROM {METADATA_CATALOG}.ducklake_inlined_data_tables
WHERE table_id = %d)",
	                          table_id.index);
}

DuckLakeCatalogInfo DuckLakeMetadataManager::GetCatalogForSnapshot(DuckLakeSnapshot snapshot) {
	auto &ducklake_catalog = transaction.GetCatalog();
	return BuildCatalogForSnapshot(
	    snapshot, [this](DuckLakeSnapshot s, string q) { return Query(s, q); }, ducklake_catalog.DataPath(),
	    ducklake_catalog.Separator(), ducklake_catalog.SupportsViewColumnTags());
}

DuckLakeCatalogInfo DuckLakeMetadataManager::BuildCatalogForSnapshot(
    DuckLakeSnapshot snapshot, const std::function<unique_ptr<QueryResult>(DuckLakeSnapshot, string)> &query_executor,
    const string &base_data_path, const string &separator, bool load_view_column_tags) {
	DuckLakeCatalogInfo catalog;
	// load the schema information
	auto result = query_executor(snapshot, R"(
SELECT schema_id, schema_uuid::VARCHAR, schema_name, path, path_is_relative
FROM {METADATA_CATALOG}.ducklake_schema sch
WHERE {VISIBLE_SCHEMA}
)");
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to get schema information from DuckLake: ");
	}
	map<SchemaIndex, idx_t> schema_map;
	for (auto &row : *result) {
		DuckLakeSchemaInfo schema;
		schema.id = SchemaIndex(row.GetValue<uint64_t>(0));
		schema.uuid = row.GetValue<string>(1);
		schema.name = row.GetValue<string>(2);
		if (row.IsNull(3)) {
			// no path provided - fallback to base data path
			schema.path = base_data_path;
		} else {
			// path is provided - load it
			DuckLakePath path;
			path.path = row.GetValue<string>(3);
			path.path_is_relative = row.GetValue<bool>(4);

			schema.path = FromRelativePath(path, base_data_path, separator);
		}
		schema_map[schema.id] = catalog.schemas.size();
		catalog.schemas.push_back(std::move(schema));
	}

	static const vector<pair<string, string>> TAG_FIELDS = {
	    {"key", "key"},
	    {"value", "value"},
	};
	static const vector<pair<string, string>> INLINED_DATA_TABLES_FIELDS = {
	    {"name", "table_name"},
	    {"schema_version", "schema_version"},
	};
	static const vector<pair<string, string>> VIEW_COLUMN_TAG_FIELDS = {
	    {"column_name", "column_name"},
	    {"key", "key"},
	    {"value", "value"},
	};

	// load the table information
	result = query_executor(snapshot,
	                        StringUtil::Format(R"(
SELECT schema_id, tbl.table_id, table_uuid::VARCHAR, table_name,
	(
		SELECT %s
		FROM {METADATA_CATALOG}.ducklake_tag tag
		WHERE object_id=table_id AND
		      {VISIBLE_TAG}
	) AS tag,
	(
		SELECT %s
		FROM (
			SELECT DISTINCT table_name, schema_version
			FROM {METADATA_CATALOG}.ducklake_inlined_data_tables inlined_data_tables
			WHERE inlined_data_tables.table_id = tbl.table_id
			  {BRANCH_INLINED_TABLE_FILTER}
		) inlined_data_tables
	) AS inlined_data_tables,
	path, path_is_relative,
	col.column_id, column_name, column_type, initial_default, default_value, nulls_allowed, parent_column,
	(
		SELECT %s
		FROM {METADATA_CATALOG}.ducklake_column_tag col_tag
		WHERE col_tag.table_id=tbl.table_id AND col_tag.column_id=col.column_id AND
		      {VISIBLE_COLUMN_TAG}
	) AS column_tags, default_value_type
FROM {METADATA_CATALOG}.ducklake_table tbl
LEFT JOIN {METADATA_CATALOG}.ducklake_column col USING (table_id)
WHERE {VISIBLE_TABLE}
  AND (({VISIBLE_COLUMN}) OR column_id IS NULL)
ORDER BY table_id, parent_column NULLS FIRST, column_order
)",
	                                           ListAggregation(TAG_FIELDS), ListAggregation(INLINED_DATA_TABLES_FIELDS),
	                                           ListAggregation(TAG_FIELDS)));
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to get table information from DuckLake: ");
	}
	const idx_t COLUMN_INDEX_START = 8;
	auto &tables = catalog.tables;
	for (auto &row : *result) {
		auto table_id = TableIndex(row.GetValue<uint64_t>(1));

		// check if this column belongs to the current table or not
		if (tables.empty() || tables.back().id != table_id) {
			// new table
			DuckLakeTableInfo table_info;
			table_info.id = table_id;
			table_info.schema_id = SchemaIndex(row.GetValue<uint64_t>(0));
			table_info.uuid = row.GetValue<string>(2);
			table_info.name = row.GetValue<string>(3);
			if (!row.IsNull(4)) {
				auto tags = row.GetValue<Value>(4);
				table_info.tags = LoadTags(tags);
			}
			if (!row.IsNull(5)) {
				auto inlined_data_tables = row.GetValue<Value>(5);
				table_info.inlined_data_tables = LoadInlinedDataTables(inlined_data_tables);
			}
			// find the schema
			auto schema_entry = schema_map.find(table_info.schema_id);
			if (schema_entry == schema_map.end()) {
				throw InvalidInputException(
				    "Failed to load DuckLake - table with id %d references schema id %d that does not exist",
				    table_info.id.index, table_info.schema_id.index);
			}
			auto &schema = catalog.schemas[schema_entry->second];
			if (row.IsNull(6)) {
				// no path provided - fallback to schema path
				table_info.path = schema.path;
			} else {
				// path is provided - load it
				DuckLakePath path;
				path.path = row.GetValue<string>(6);
				path.path_is_relative = row.GetValue<bool>(7);

				table_info.path = FromRelativePath(path, schema.path, separator);
			}
			tables.push_back(std::move(table_info));
		}
		auto &table_entry = tables.back();
		if (row.GetValue<Value>(COLUMN_INDEX_START).IsNull()) {
			throw InvalidInputException("Failed to load DuckLake - Table entry \"%s\" does not have any columns",
			                            table_entry.name);
		}
		DuckLakeColumnInfo column_info;
		column_info.id = FieldIndex(row.GetValue<uint64_t>(COLUMN_INDEX_START));
		column_info.name = row.GetValue<string>(COLUMN_INDEX_START + 1);
		column_info.type = row.GetValue<string>(COLUMN_INDEX_START + 2);
		if (!row.IsNull(COLUMN_INDEX_START + 3)) {
			column_info.initial_default = Value(row.GetValue<string>(COLUMN_INDEX_START + 3));
		}
		if (!row.IsNull(COLUMN_INDEX_START + 4)) {
			auto value = row.GetValue<string>(COLUMN_INDEX_START + 4);
			if (value == "NULL") {
				column_info.default_value = Value();
			} else {
				column_info.default_value = Value(value);
			}
		}
		if (!row.IsNull(COLUMN_INDEX_START + 8)) {
			column_info.default_value_type = row.GetValue<string>(COLUMN_INDEX_START + 8);
		}
		column_info.nulls_allowed = row.GetValue<bool>(COLUMN_INDEX_START + 5);
		if (!row.IsNull(COLUMN_INDEX_START + 7)) {
			auto tags = row.GetValue<Value>(COLUMN_INDEX_START + 7);
			column_info.tags = LoadTags(tags);
		}

		if (row.IsNull(COLUMN_INDEX_START + 6)) {
			// base column - add the column to this table
			table_entry.columns.push_back(std::move(column_info));
		} else {
			auto parent_id = FieldIndex(row.GetValue<idx_t>(COLUMN_INDEX_START + 6));
			if (!AddChildColumn(table_entry.columns, parent_id, column_info)) {
				throw InvalidInputException("Failed to load DuckLake - Could not find parent column for column %s",
				                            column_info.name);
			}
		}
	}
	// load view information
	auto view_column_tags_select = load_view_column_tags ? StringUtil::Format(R"(,
	(
		SELECT %s
		FROM {METADATA_CATALOG}.ducklake_view_column_tag vct
		WHERE vct.view_id=view.view_id AND
		      {VISIBLE_VIEW_COLUMN_TAG}
	) AS view_column_tags)",
	                                                                          ListAggregation(VIEW_COLUMN_TAG_FIELDS))
	                                                     : ",\n\tNULL AS view_column_tags";
	result = query_executor(snapshot, StringUtil::Format(R"(
SELECT view_id, view_uuid, schema_id, view_name, dialect, sql, column_aliases,
	(
		SELECT %s
		FROM {METADATA_CATALOG}.ducklake_tag tag
		WHERE object_id=view_id AND
		      {VISIBLE_TAG}
	) AS tag%s
FROM {METADATA_CATALOG}.ducklake_view view
WHERE {VISIBLE_VIEW}
)",
	                                                     ListAggregation(TAG_FIELDS), view_column_tags_select));
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to get partition information from DuckLake: ");
	}
	auto &views = catalog.views;
	for (auto &row : *result) {
		DuckLakeViewInfo view_info;
		view_info.id = TableIndex(row.GetValue<uint64_t>(0));
		view_info.uuid = row.GetValue<string>(1);
		view_info.schema_id = SchemaIndex(row.GetValue<uint64_t>(2));
		view_info.name = row.GetValue<string>(3);
		view_info.dialect = row.GetValue<string>(4);
		view_info.sql = row.GetValue<string>(5);
		view_info.column_aliases = DuckLakeUtil::ParseQuotedList(row.GetValue<string>(6));
		if (!row.IsNull(7)) {
			auto tags = row.GetValue<Value>(7);
			view_info.tags = LoadTags(tags);
		}
		if (!row.IsNull(8)) {
			view_info.column_tags = LoadViewColumnTags(row.GetValue<Value>(8));
		}
		views.push_back(std::move(view_info));
	}

	static const vector<pair<string, string>> MACRO_PARAM_FIELDS = {{"parameter_name", "parameter_name"},
	                                                                {"parameter_type", "parameter_type"},
	                                                                {"default_value", "default_value"},
	                                                                {"default_value_type", "default_value_type"}};
	auto macro_param_query = StringUtil::Format(R"(
		(
		SELECT %s
		FROM {METADATA_CATALOG}.ducklake_macro_parameters
		WHERE ducklake_macro_impl.macro_id = ducklake_macro_parameters.macro_id
		AND ducklake_macro_impl.impl_id = ducklake_macro_parameters.impl_id
		)
	)",
	                                            ListAggregation(MACRO_PARAM_FIELDS));
	const vector<pair<string, string>> MACRO_IMPL_FIELDS = {
	    {"dialect", "dialect"}, {"sql", "sql"}, {"type", "type"}, {"params", macro_param_query}};

	// load macro information
	result = query_executor(snapshot, StringUtil::Format(R"(
SELECT schema_id, ducklake_macro.macro_id, macro_name, (
		SELECT %s
		FROM {METADATA_CATALOG}.ducklake_macro_impl
		WHERE ducklake_macro.macro_id = ducklake_macro_impl.macro_id
	) AS impl
FROM {METADATA_CATALOG}.ducklake_macro
WHERE {VISIBLE_MACRO}
)",
	                                                     ListAggregation(MACRO_IMPL_FIELDS)));
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to get macro information from DuckLake: ");
	}
	auto &macros = catalog.macros;
	for (auto &row : *result) {
		DuckLakeMacroInfo macro_info;
		macro_info.schema_id = SchemaIndex(row.GetValue<uint64_t>(0));
		macro_info.macro_id = MacroIndex(row.GetValue<uint64_t>(1));
		macro_info.macro_name = row.GetValue<string>(2);
		auto macro_implementations = row.GetValue<Value>(3);
		macro_info.implementations = LoadMacroImplementations(macro_implementations);
		macros.push_back(std::move(macro_info));
	}

	// load partition information
	result = query_executor(snapshot, R"(
SELECT partition_id, part.table_id, partition_key_index, column_id, transform
FROM {METADATA_CATALOG}.ducklake_partition_info part
JOIN {METADATA_CATALOG}.ducklake_partition_column part_col USING (partition_id)
WHERE {VISIBLE_PARTITION}
ORDER BY part.table_id, partition_id, partition_key_index
)");
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to get partition information from DuckLake: ");
	}
	auto &partitions = catalog.partitions;
	for (auto &row : *result) {
		auto partition_id = row.GetValue<uint64_t>(0);
		auto table_id = TableIndex(row.GetValue<uint64_t>(1));

		if (partitions.empty() || partitions.back().table_id != table_id) {
			DuckLakePartitionInfo partition_info;
			partition_info.id = partition_id;
			partition_info.table_id = table_id;
			partitions.push_back(std::move(partition_info));
		}
		auto &partition_entry = partitions.back();

		DuckLakePartitionFieldInfo partition_field;
		partition_field.partition_key_index = row.GetValue<uint64_t>(2);
		partition_field.field_id = FieldIndex(row.GetValue<uint64_t>(3));
		partition_field.transform = row.GetValue<string>(4);
		partition_entry.fields.push_back(std::move(partition_field));
	}

	// load sort information
	result = query_executor(snapshot, R"(
SELECT sort.sort_id, sort.table_id, sort_expr.sort_key_index, sort_expr.expression, sort_expr.dialect, sort_expr.sort_direction, sort_expr.null_order
FROM {METADATA_CATALOG}.ducklake_sort_info sort
JOIN {METADATA_CATALOG}.ducklake_sort_expression sort_expr USING (sort_id)
WHERE {VISIBLE_SORT}
ORDER BY sort.table_id, sort.sort_id, sort_expr.sort_key_index
)");
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to get sort information from DuckLake: ");
	}
	auto &sorts = catalog.sorts;
	for (auto &row : *result) {
		auto sort_id = row.GetValue<uint64_t>(0);
		auto table_id = TableIndex(row.GetValue<uint64_t>(1));

		if (sorts.empty() || sorts.back().table_id != table_id) {
			DuckLakeSortInfo sort_info;
			sort_info.id = sort_id;
			sort_info.table_id = table_id;
			sorts.push_back(std::move(sort_info));
		}
		auto &sort_entry = sorts.back();

		DuckLakeSortFieldInfo sort_field;
		sort_field.sort_key_index = row.GetValue<uint64_t>(2);
		sort_field.expression = row.GetValue<string>(3);
		sort_field.dialect = row.GetValue<string>(4);

		auto sort_direction_str = row.GetValue<string>(5);
		sort_field.sort_direction =
		    (StringUtil::CIEquals(sort_direction_str, "DESC") ? OrderType::DESCENDING : OrderType::ASCENDING);

		auto null_order_str = row.GetValue<string>(6);
		sort_field.null_order = (StringUtil::CIEquals(null_order_str, "NULLS_FIRST") ? OrderByNullType::NULLS_FIRST
		                                                                             : OrderByNullType::NULLS_LAST);
		sort_entry.fields.push_back(std::move(sort_field));
	}

	return catalog;
}

template <class ROW>
void TransformGlobalStatsRow(const ROW &row, vector<DuckLakeGlobalStatsInfo> &global_stats, idx_t from_column = 0) {
	auto table_id = TableIndex(row.template GetValue<uint64_t>(0 + from_column));

	if (global_stats.empty() || global_stats.back().table_id != table_id) {
		DuckLakeGlobalStatsInfo new_entry;
		new_entry.table_id = table_id;
		new_entry.initialized = true;
		new_entry.record_count = row.template GetValue<uint64_t>(2 + from_column);
		new_entry.next_row_id = row.template GetValue<uint64_t>(3 + from_column);
		new_entry.table_size_bytes = row.template GetValue<uint64_t>(4 + from_column);
		global_stats.push_back(std::move(new_entry));
	}

	auto &stats_entry = global_stats.back();

	if (row.IsNull(1 + from_column)) {
		// table_stats row with no matching ducklake_table_column_stats row, we keep the
		// table-level stats (record_count/next_row_id) without adding a column-stats entry.
		return;
	}

	DuckLakeGlobalColumnStatsInfo column_stats;
	column_stats.column_id = FieldIndex(row.template GetValue<uint64_t>(1 + from_column));

	const idx_t COLUMN_STATS_START = 5 + from_column;

	if (row.IsNull(COLUMN_STATS_START)) {
		column_stats.has_contains_null = false;
	} else {
		column_stats.has_contains_null = true;
		column_stats.contains_null = row.template GetValue<bool>(COLUMN_STATS_START);
	}

	if (row.IsNull(COLUMN_STATS_START + 1)) {
		column_stats.has_contains_nan = false;
	} else {
		column_stats.has_contains_nan = true;
		column_stats.contains_nan = row.template GetValue<bool>(COLUMN_STATS_START + 1);
	}

	if (row.IsNull(COLUMN_STATS_START + 2)) {
		column_stats.has_min = false;
	} else {
		column_stats.has_min = true;
		column_stats.min_val = row.template GetValue<string>(COLUMN_STATS_START + 2);
	}

	if (row.IsNull(COLUMN_STATS_START + 3)) {
		column_stats.has_max = false;
	} else {
		column_stats.has_max = true;
		column_stats.max_val = row.template GetValue<string>(COLUMN_STATS_START + 3);
	}

	if (row.IsNull(COLUMN_STATS_START + 4)) {
		column_stats.has_extra_stats = false;
	} else {
		column_stats.has_extra_stats = true;
		column_stats.extra_stats = row.template GetValue<string>(COLUMN_STATS_START + 4);
	}

	stats_entry.column_stats.push_back(std::move(column_stats));
}

vector<DuckLakeGlobalStatsInfo> TransformGlobalStats(QueryResult &result) {
	if (result.HasError()) {
		result.GetErrorObject().Throw("Failed to get global stats information from DuckLake: ");
	}

	vector<DuckLakeGlobalStatsInfo> global_stats;

	for (auto &row : result) {
		TransformGlobalStatsRow(row, global_stats);
	}

	return global_stats;
}

string DuckLakeMetadataManager::GlobalTableStatsQuery() {
	// Pure all-tables template (only {METADATA_CATALOG} is substituted by the caller; it is NOT run
	// through StringUtil::Format). It must NOT contain a printf placeholder such as `WHERE table_id =
	// %llu` - the server-side commit path (DuckLakeServerSideCommit::ReadExistingTableStats) executes
	// the returned SQL verbatim, so a stray %llu would reach the parser and fail every server-side
	// commit. The single-table GetGlobalTableStats() below keeps its own StringUtil::Format query.
	// {BRANCH_STATS_FILTER} is expanded by ExpandBranchAwarePlaceholders (empty on pre-1.1-dev3).
	return R"(
SELECT table_id, column_id, record_count, next_row_id, file_size_bytes, contains_null, contains_nan, min_value, max_value, extra_stats
FROM {METADATA_CATALOG}.ducklake_table_stats
LEFT JOIN {METADATA_CATALOG}.ducklake_table_column_stats USING (table_id{BRANCH_ID_JOIN})
WHERE record_count IS NOT NULL
  AND file_size_bytes IS NOT NULL
  {BRANCH_STATS_FILTER}
ORDER BY table_id;
)";
}

vector<DuckLakeGlobalStatsInfo> DuckLakeMetadataManager::ParseGlobalTableStats(QueryResult &result) {
	return TransformGlobalStats(result);
}

vector<DuckLakeGlobalStatsInfo> DuckLakeMetadataManager::GetGlobalTableStats(DuckLakeSnapshot snapshot,
                                                                             TableIndex table_id) {
	string query = StringUtil::Format(R"(
SELECT table_id, column_id, record_count, next_row_id, file_size_bytes, contains_null, contains_nan, min_value, max_value, extra_stats
FROM {METADATA_CATALOG}.ducklake_table_stats
LEFT JOIN {METADATA_CATALOG}.ducklake_table_column_stats USING (table_id{BRANCH_ID_JOIN})
WHERE table_id = %llu
  AND record_count IS NOT NULL
  AND file_size_bytes IS NOT NULL
  {BRANCH_STATS_FILTER}
ORDER BY table_id;
)",
	                                  table_id.index);

	auto result = Query(snapshot, query);
	return TransformGlobalStats(*result);
}

string DuckLakeMetadataManager::GetFileSelectList(const string &prefix) {
	static const vector<string> column_list {
	    "path", "path_is_relative", "file_size_bytes", "footer_size", "encryption_key",
	};

	auto count = column_list.size();
	if (!IsEncrypted()) {
		count -= 1;
	}

	auto result = StringUtil::Join(column_list, count, ", ", [&prefix](const string &column) {
		return prefix + "." + column + " AS " + prefix + "_" + column;
	});

	return result;
}

string DuckLakeMetadataManager::GetDeleteFileSelectList(const string &prefix) {
	return GetFileSelectList(prefix) + ", " + prefix + ".format AS " + prefix + "_format";
}

template <class T>
DuckLakeFileData DuckLakeMetadataManager::ReadDataFile(DuckLakeTableEntry &table, T &row, idx_t &col_idx,
                                                       bool is_encrypted) {
	DuckLakeFileData data;
	if (row.IsNull(col_idx)) {
		// file is not there
		col_idx += 4;
		if (is_encrypted) {
			col_idx++;
		}
		return data;
	}
	DuckLakePath path;
	path.path = row.template GetValue<string>(col_idx++);
	path.path_is_relative = row.template GetValue<bool>(col_idx++);

	data.path = FromRelativePath(path, table.DataPath());
	data.file_size_bytes = row.template GetValue<idx_t>(col_idx++);
	if (!row.IsNull(col_idx)) {
		data.footer_size = row.template GetValue<idx_t>(col_idx);
	}
	col_idx++;
	if (is_encrypted) {
		if (row.IsNull(col_idx)) {
			throw InvalidInputException("Database is encrypted, but file %s does not have an encryption key",
			                            data.path);
		}
		data.encryption_key = Blob::FromBase64(row.template GetValue<string>(col_idx++));
	}
	return data;
}

template <class T>
DuckLakeFileData DuckLakeMetadataManager::ReadDeleteFile(DuckLakeTableEntry &table, T &row, idx_t &col_idx,
                                                         bool is_encrypted) {
	auto data = ReadDataFile(table, row, col_idx, is_encrypted);
	if (!row.IsNull(col_idx)) {
		data.format = DeleteFileFormatFromString(row.template GetValue<string>(col_idx));
	}
	col_idx++;
	return data;
}

static void SetSnapshotFilter(const DuckLakeSnapshot &snapshot, idx_t max_partial_file_snapshot,
                              DuckLakeFileListEntry &file_entry) {
	if (max_partial_file_snapshot <= snapshot.snapshot_id) {
		// all snapshot ids are included for this snapshot - skip filtering
		return;
	}
	file_entry.snapshot_filter_max = snapshot.snapshot_id;
}

static bool IsSimpleFilterSubject(const Expression &expr) {
	return expr.GetExpressionClass() == ExpressionClass::BOUND_REF ||
	       expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF;
}

string DuckLakeMetadataManager::GenerateFilterFromTableFilter(const ExpressionFilter &filter, const LogicalType &type,
                                                              unordered_set<string> &referenced_stats) {
	if (!filter.expr) {
		return string();
	}
	return GenerateFilterFromExpression(*filter.expr, &type, referenced_stats);
}

bool DuckLakeMetadataManager::ValueIsFinite(const Value &val) {
	if (val.type().id() != LogicalTypeId::FLOAT && val.type().id() != LogicalTypeId::DOUBLE) {
		return true;
	}
	double constant_val = val.GetValue<double>();
	return Value::IsFinite(constant_val);
}

string DuckLakeMetadataManager::CastValueToTarget(const Value &val, const LogicalType &type) {
	if (type.IsNumeric() && ValueIsFinite(val)) {
		// for (finite) numerics we directly emit the number
		return val.ToString();
	}
	// convert to a string
	return DuckLakeUtil::SQLLiteralToString(val.ToString());
}

string DuckLakeMetadataManager::CastStatsToTarget(const string &stats, const LogicalType &type) {
	// we need to cast numerics and temporals for correct comparison
	if (RequiresValueComparison(type)) {
		return "TRY_CAST(" + stats + " AS " + type.ToString() + ")";
	}
	return stats;
}

string DuckLakeMetadataManager::CastColumnToTarget(const string &column, const LogicalType &type) {
	// ANSI CAST(...) — same reason as elsewhere: SQLite rejects `::` casts.
	return "CAST(" + column + " AS " + type.ToString() + ")";
}

string DuckLakeMetadataManager::GenerateConstantFilter(ExpressionType comparison_type, const Value &constant,
                                                       const LogicalType &type,
                                                       unordered_set<string> &referenced_stats) {
	auto constant_str = CastValueToTarget(constant, type);
	if (constant_str.find('\0') != string::npos) {
		return string();
	}
	auto min_value = CastStatsToTarget("min_value", type);
	auto max_value = CastStatsToTarget("max_value", type);
	switch (comparison_type) {
	case ExpressionType::COMPARE_EQUAL:
		// x = constant
		// this can only be true if "constant BETWEEN min AND max"
		referenced_stats.insert("min_value");
		referenced_stats.insert("max_value");
		return StringUtil::Format("%s BETWEEN %s AND %s", constant_str, min_value, max_value);
	case ExpressionType::COMPARE_NOTEQUAL:
		// x <> constant
		// this can only be false if "constant = min AND constant = max" (i.e. min = max = constant)
		referenced_stats.insert("min_value");
		referenced_stats.insert("max_value");
		return StringUtil::Format("NOT (%s = %s AND %s = %s)", min_value, constant_str, max_value, constant_str);
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		// x >= constant
		// this can only be true if "max >= C"
		referenced_stats.insert("max_value");
		return StringUtil::Format("%s >= %s", max_value, constant_str);
	case ExpressionType::COMPARE_GREATERTHAN:
		// x > constant
		// this can only be true if "max > C"
		referenced_stats.insert("max_value");
		return StringUtil::Format("%s > %s", max_value, constant_str);
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		// x <= constant
		// this can only be true if "min <= C"
		referenced_stats.insert("min_value");
		return StringUtil::Format("%s <= %s", min_value, constant_str);
	case ExpressionType::COMPARE_LESSTHAN:
		// x < constant
		// this can only be true if "min < C"
		referenced_stats.insert("min_value");
		return StringUtil::Format("%s < %s", min_value, constant_str);
	default:
		// unsupported
		return string();
	}
}

string DuckLakeMetadataManager::GenerateConstantFilterDouble(ExpressionType comparison_type, const Value &constant,
                                                             const LogicalType &type,
                                                             unordered_set<string> &referenced_stats) {
	double constant_val = constant.GetValue<double>();
	bool constant_is_nan = Value::IsNan(constant_val);
	switch (comparison_type) {
	case ExpressionType::COMPARE_EQUAL:
		// x = constant
		if (constant_is_nan) {
			// x = NAN - check for `contains_nan`
			referenced_stats.insert("contains_nan");
			return "contains_nan";
		}
		// else check as if this is a numeric
		return GenerateConstantFilter(comparison_type, constant, type, referenced_stats);
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
	case ExpressionType::COMPARE_GREATERTHAN: {
		if (constant_is_nan) {
			// skip these filters if the constant is nan
			// note that > and >= we can actually handle since nan is the biggest value
			// (>= is equal to =, > is always false)
			return string();
		}
		// generate the numeric filter
		string filter = GenerateConstantFilter(comparison_type, constant, type, referenced_stats);
		if (filter.empty()) {
			return string();
		}
		// since NaN is bigger than anything - we also need to check for contains_nan
		referenced_stats.insert("contains_nan");
		return filter + " OR contains_nan";
	}
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
	case ExpressionType::COMPARE_LESSTHAN:
		if (constant_is_nan) {
			// skip these filters if the constant is nan
			return string();
		}
		// these are equivalent to the numeric filter
		return GenerateConstantFilter(comparison_type, constant, type, referenced_stats);
	case ExpressionType::COMPARE_NOTEQUAL: {
		// x <> constant
		if (constant_is_nan) {
			// x <> NaN is true for every non-NaN value and false only for NaN.
			// proving a file can be pruned would require knowing all its values are NaN, which min/max
			// cannot express - so never prune.
			return string();
		}
		// generate the numeric filter
		string filter = GenerateConstantFilter(comparison_type, constant, type, referenced_stats);
		if (filter.empty()) {
			return string();
		}
		// NaN <> constant is always true, so we must also keep files that contain NaN
		referenced_stats.insert("contains_nan");
		return filter + " OR contains_nan";
	}
	default:
		// unsupported
		return string();
	}
}

string DuckLakeMetadataManager::GenerateFilterFromExpression(const Expression &expr, const LogicalType *type,
                                                             unordered_set<string> &referenced_stats) {
	if (BoundComparisonExpression::IsComparison(expr)) {
		auto &comparison = expr.Cast<BoundFunctionExpression>();
		auto &left = BoundComparisonExpression::Left(comparison);
		auto &right = BoundComparisonExpression::Right(comparison);
		const BoundConstantExpression *constant_expr = nullptr;
		auto comparison_type = comparison.GetExpressionType();
		if (IsSimpleFilterSubject(left) && right.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
			constant_expr = &right.Cast<BoundConstantExpression>();
		} else if (IsSimpleFilterSubject(right) && left.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
			constant_expr = &left.Cast<BoundConstantExpression>();
			comparison_type = FlipComparisonExpression(comparison_type);
		} else {
			return string();
		}
		const auto &target_type = type ? *type : constant_expr->GetValue().type();
		switch (target_type.id()) {
		case LogicalTypeId::BLOB:
			return string();
		case LogicalTypeId::FLOAT:
		case LogicalTypeId::DOUBLE:
			return GenerateConstantFilterDouble(comparison_type, constant_expr->GetValue(), target_type,
			                                    referenced_stats);
		default:
			return GenerateConstantFilter(comparison_type, constant_expr->GetValue(), target_type, referenced_stats);
		}
	}
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_OPERATOR: {
		auto &op_expr = expr.Cast<BoundOperatorExpression>();
		switch (expr.GetExpressionType()) {
		case ExpressionType::OPERATOR_IS_NULL:
			if (op_expr.GetChildren().size() != 1 || !IsSimpleFilterSubject(*op_expr.GetChildren()[0])) {
				return string();
			}
			referenced_stats.insert("null_count");
			return "null_count > 0";
		case ExpressionType::OPERATOR_IS_NOT_NULL:
			if (op_expr.GetChildren().size() != 1 || !IsSimpleFilterSubject(*op_expr.GetChildren()[0])) {
				return string();
			}
			referenced_stats.insert("value_count");
			return "value_count > 0";
		case ExpressionType::COMPARE_IN: {
			if (op_expr.GetChildren().size() < 2 || !IsSimpleFilterSubject(*op_expr.GetChildren()[0])) {
				return string();
			}
			string result;
			for (idx_t i = 1; i < op_expr.GetChildren().size(); i++) {
				auto &child = *op_expr.GetChildren()[i];
				if (child.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
					return string();
				}
				auto &constant_value = child.Cast<BoundConstantExpression>().GetValue();
				if (constant_value.IsNull()) {
					return string();
				}
				const auto &target_type = type ? *type : constant_value.type();
				string next_filter;
				switch (target_type.id()) {
				case LogicalTypeId::BLOB:
					return string();
				case LogicalTypeId::FLOAT:
				case LogicalTypeId::DOUBLE:
					next_filter = GenerateConstantFilterDouble(ExpressionType::COMPARE_EQUAL, constant_value,
					                                           target_type, referenced_stats);
					break;
				default:
					next_filter = GenerateConstantFilter(ExpressionType::COMPARE_EQUAL, constant_value, target_type,
					                                     referenced_stats);
					break;
				}
				if (next_filter.empty()) {
					return string();
				}
				if (!result.empty()) {
					result += " OR ";
				}
				result += "(" + next_filter + ")";
			}
			return result;
		}
		default:
			return string();
		}
	}
	case ExpressionClass::BOUND_CONJUNCTION: {
		auto &conjunction = expr.Cast<BoundConjunctionExpression>();
		string result;
		const auto conjunction_type = expr.GetExpressionType();
		for (auto &child : conjunction.GetChildren()) {
			auto child_str = GenerateFilterFromExpression(*child, type, referenced_stats);
			if (child_str.empty()) {
				if (conjunction_type == ExpressionType::CONJUNCTION_OR) {
					return string();
				}
				continue;
			}
			if (!result.empty()) {
				result += conjunction_type == ExpressionType::CONJUNCTION_OR ? " OR " : " AND ";
			}
			result += "(" + child_str + ")";
		}
		return result;
	}
	case ExpressionClass::BOUND_FUNCTION: {
		auto &func = expr.Cast<BoundFunctionExpression>();
		if (func.Function().GetName() == OptionalFilterScalarFun::NAME && func.BindInfo()) {
			auto &data = func.BindInfo()->Cast<OptionalFilterFunctionData>();
			return data.child_filter_expr
			           ? GenerateFilterFromExpression(*data.child_filter_expr, type, referenced_stats)
			           : string();
		}
		if (func.Function().GetName() == SelectivityOptionalFilterScalarFun::NAME && func.BindInfo()) {
			auto &data = func.BindInfo()->Cast<SelectivityOptionalFilterFunctionData>();
			return data.child_filter_expr
			           ? GenerateFilterFromExpression(*data.child_filter_expr, type, referenced_stats)
			           : string();
		}
		return string();
	}
	default:
		return string();
	}
}

string DuckLakeMetadataManager::GenerateFilterPushdown(const ExpressionFilter &filter,
                                                       unordered_set<string> &referenced_stats) {
	if (!filter.expr) {
		return string();
	}
	return GenerateFilterFromExpression(*filter.expr, nullptr, referenced_stats);
}

FilterSQLResult DuckLakeMetadataManager::ConvertFilterPushdownToSQL(const FilterPushdownInfo &filter_info) {
	FilterSQLResult result;
	string conditions;

	for (const auto &entry : filter_info.column_filters) {
		const auto &column_filter = entry.second;

		unordered_set<string> referenced_stats;
		auto filter_condition = GenerateFilterPushdown(*column_filter.table_filter, referenced_stats);

		if (filter_condition.empty()) {
			continue;
		}

		string cte_name = StringUtil::Format("col_%d_stats", column_filter.column_field_index);

		string null_checks;
		for (const auto &stat : referenced_stats) {
			null_checks += stat + " IS NULL OR ";
		}

		const bool needs_value_count_guard =
		    referenced_stats.count("min_value") > 0 || referenced_stats.count("max_value") > 0;
		if (needs_value_count_guard) {
			referenced_stats.insert("value_count");
		}

		if (!conditions.empty()) {
			conditions += " AND ";
		}
		// Files that have no stats entry for this column (i.e., written before the column was added) must
		// NOT be pruned, we cannot determine filter satisfaction without stats.
		if (needs_value_count_guard) {
			conditions += StringUtil::Format("(data.data_file_id NOT IN (SELECT data_file_id FROM %s) OR "
			                                 "data.data_file_id IN (SELECT data_file_id FROM %s WHERE "
			                                 "(value_count IS NULL OR value_count > 0) AND (%s(%s))))",
			                                 cte_name, cte_name, null_checks.c_str(), filter_condition.c_str());
		} else {
			conditions += StringUtil::Format("(data.data_file_id NOT IN (SELECT data_file_id FROM %s) OR "
			                                 "data.data_file_id IN (SELECT data_file_id FROM %s WHERE %s(%s)))",
			                                 cte_name, cte_name, null_checks.c_str(), filter_condition.c_str());
		}

		CTERequirement req(column_filter.column_field_index, referenced_stats);
		req.reference_count = 2;
		result.required_ctes.emplace(column_filter.column_field_index, std::move(req));
	}

	result.where_conditions = conditions;
	return result;
}

string DuckLakeMetadataManager::GenerateFileColumnStatsCTEBody(const CTERequirement &req, TableIndex table_id) {
	string select_list = "data_file_id";
	for (const auto &stat : req.referenced_stats) {
		select_list += ", " + stat;
	}
	return StringUtil::Format("  SELECT %s\n"
	                          "  FROM {METADATA_CATALOG}.ducklake_file_column_stats\n"
	                          "  WHERE column_id = %d AND table_id = %d\n",
	                          select_list, req.column_field_index, table_id.index);
}

string
DuckLakeMetadataManager::GenerateCTESectionFromRequirements(const unordered_map<idx_t, CTERequirement> &requirements,
                                                            TableIndex table_id) {
	if (requirements.empty()) {
		return "";
	}

	string cte_section = "WITH ";
	bool first_cte = true;

	for (const auto &entry : requirements) {
		const auto &req = entry.second;

		if (!first_cte) {
			cte_section += ",\n";
		}
		first_cte = false;

		string materialized_hint = (req.reference_count > 1) ? " AS MATERIALIZED" : " AS NOT MATERIALIZED";
		cte_section += StringUtil::Format("col_%d_stats%s (\n", req.column_field_index, materialized_hint.c_str());
		cte_section += GenerateFileColumnStatsCTEBody(req, table_id);
		cte_section += ")";
	}

	return cte_section + "\n";
}

FilterPushdownQueryComponents
DuckLakeMetadataManager::GenerateFilterPushdownComponents(const FilterPushdownInfo &filter_info,
                                                          DuckLakeTableEntry &table) {
	FilterPushdownQueryComponents result;

	auto table_id = table.GetTableId();

	if (filter_info.column_filters.empty()) {
		return result;
	}

	auto filter_result = ConvertFilterPushdownToSQL(filter_info);
	result.cte_section = GenerateCTESectionFromRequirements(filter_result.required_ctes, table_id);
	result.where_clause = filter_result.where_conditions;

	return result;
}

struct DynamicFilterColumn {
	idx_t column_field_index;
	ExpressionType comparison_type;
	LogicalType column_type;
};

//! Fold a single constant through the bucket() transform, returning the resulting partition_value
//! string ("6", "3", ...) that matches what the writer stores. Returns empty optional on any failure
//! (cast error, evaluation error, NULL) so the caller can skip this filter and fall back to zone maps.
static optional_idx FoldBucketValue(ClientContext &context, const Value &constant, const LogicalType &col_type,
                                    idx_t bucket_count, string &out_partition_value) {
	if (constant.IsNull()) {
		return optional_idx();
	}
	Value casted;
	if (!constant.DefaultTryCastAs(col_type, casted, nullptr)) {
		return optional_idx();
	}
	auto const_expr = make_uniq<BoundConstantExpression>(std::move(casted));
	auto bucket_expr = DuckLakePartitionUtils::ApplyBucketTransform(context, std::move(const_expr), bucket_count);
	Value result;
	if (!ExpressionExecutor::TryEvaluateScalar(context, *bucket_expr, result)) {
		return optional_idx();
	}
	if (result.IsNull()) {
		return optional_idx();
	}
	out_partition_value = result.ToString();
	return optional_idx(1);
}

//! Walk a bound filter expression and, for each foldable equality / IN-list constant, append the
//! resulting partition_value string to `out`. Returns true if at least one valid constant was collected.
//! Unsupported shapes (range comparisons, OR-conjunctions, dynamic filters, etc.) return false
//! and pruning is skipped for this column — the existing zone-map path handles correctness.
static bool CollectBucketEqualityValues(ClientContext &context, const Expression &expr, const LogicalType &col_type,
                                        idx_t bucket_count, vector<string> &out) {
	// col = constant
	if (BoundComparisonExpression::IsComparison(expr)) {
		auto &comparison = expr.Cast<BoundFunctionExpression>();
		if (comparison.GetExpressionType() != ExpressionType::COMPARE_EQUAL) {
			return false;
		}
		auto &left = BoundComparisonExpression::Left(comparison);
		auto &right = BoundComparisonExpression::Right(comparison);
		const BoundConstantExpression *constant_expr = nullptr;
		if (IsSimpleFilterSubject(left) && right.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
			constant_expr = &right.Cast<BoundConstantExpression>();
		} else if (IsSimpleFilterSubject(right) && left.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
			constant_expr = &left.Cast<BoundConstantExpression>();
		} else {
			return false;
		}
		string partition_value;
		if (!FoldBucketValue(context, constant_expr->GetValue(), col_type, bucket_count, partition_value).IsValid()) {
			return false;
		}
		out.push_back(std::move(partition_value));
		return true;
	}
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_OPERATOR: {
		// col IN (C1, C2, ...)
		if (expr.GetExpressionType() != ExpressionType::COMPARE_IN) {
			return false;
		}
		auto &op_expr = expr.Cast<BoundOperatorExpression>();
		if (op_expr.GetChildren().size() < 2 || !IsSimpleFilterSubject(*op_expr.GetChildren()[0])) {
			return false;
		}
		for (idx_t i = 1; i < op_expr.GetChildren().size(); i++) {
			auto &child = *op_expr.GetChildren()[i];
			if (child.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
				continue;
			}
			string partition_value;
			if (!FoldBucketValue(context, child.Cast<BoundConstantExpression>().GetValue(), col_type, bucket_count,
			                     partition_value)
			         .IsValid()) {
				continue;
			}
			out.push_back(std::move(partition_value));
		}
		return !out.empty();
	}
	case ExpressionClass::BOUND_CONJUNCTION: {
		// Only AND conjunctions give a valid (tighter) prune. A child equality on this column suffices;
		// over-inclusion from contradictory ANDs (a = 1 AND a = 2) is correctness-safe — the residual filter
		// still runs. OR-conjunctions could miss matching buckets, so they are left to the zone-map path.
		if (expr.GetExpressionType() != ExpressionType::CONJUNCTION_AND) {
			return false;
		}
		auto &conjunction = expr.Cast<BoundConjunctionExpression>();
		for (auto &child : conjunction.GetChildren()) {
			CollectBucketEqualityValues(context, *child, col_type, bucket_count, out);
		}
		return !out.empty();
	}
	case ExpressionClass::BOUND_FUNCTION: {
		// unwrap optional-filter markers, same as GenerateFilterFromExpression
		auto &func = expr.Cast<BoundFunctionExpression>();
		if (func.Function().GetName() == OptionalFilterScalarFun::NAME && func.BindInfo()) {
			auto &data = func.BindInfo()->Cast<OptionalFilterFunctionData>();
			return data.child_filter_expr &&
			       CollectBucketEqualityValues(context, *data.child_filter_expr, col_type, bucket_count, out);
		}
		if (func.Function().GetName() == SelectivityOptionalFilterScalarFun::NAME && func.BindInfo()) {
			auto &data = func.BindInfo()->Cast<SelectivityOptionalFilterFunctionData>();
			return data.child_filter_expr &&
			       CollectBucketEqualityValues(context, *data.child_filter_expr, col_type, bucket_count, out);
		}
		return false;
	}
	default:
		return false;
	}
}

string DuckLakeMetadataManager::BuildBucketPartitionPruningClause(DuckLakeTableEntry &table,
                                                                  const FilterPushdownInfo &filter_info) {
	auto partition_data = table.GetPartitionData();
	if (!partition_data) {
		return string();
	}
	auto context_ptr = transaction.context.lock();
	if (!context_ptr) {
		return string();
	}
	auto &context = *context_ptr;
	auto table_id = table.GetTableId();
	string result;

	for (auto &field : partition_data->fields) {
		if (field.transform.type != DuckLakeTransformType::BUCKET) {
			continue;
		}
		auto it = filter_info.column_filters.find(field.field_id.index);
		if (it == filter_info.column_filters.end()) {
			continue;
		}
		const auto &col_filter = it->second;
		if (!col_filter.table_filter || !col_filter.table_filter->expr) {
			continue;
		}

		vector<string> bucket_values;
		if (!CollectBucketEqualityValues(context, *col_filter.table_filter->expr, col_filter.column_type,
		                                 field.transform.bucket_count, bucket_values)) {
			continue;
		}
		if (bucket_values.empty()) {
			continue;
		}

		string in_list;
		for (auto &v : bucket_values) {
			if (!in_list.empty()) {
				in_list += ", ";
			}
			in_list += StringUtil::Format("%s", SQLString(v));
		}
		string clause = StringUtil::Format(
		    "data.data_file_id IN (SELECT data_file_id FROM {METADATA_CATALOG}.ducklake_file_partition_value "
		    "WHERE table_id = %d AND partition_key_index = %d AND partition_value IN (%s))",
		    table_id.index, field.partition_key_index, in_list);

		if (!result.empty()) {
			result += " AND ";
		}
		result += clause;
	}
	return result;
}

vector<DuckLakeFileListEntry> DuckLakeMetadataManager::GetFilesForTable(DuckLakeTableEntry &table,
                                                                        DuckLakeSnapshot snapshot,
                                                                        const FilterPushdownInfo *filter_info) {
	auto table_id = table.GetTableId();

	// If we have Top-N dynamic filter pushdown, include file-level min/max stats for pruning and ordering
	vector<DynamicFilterColumn> dynamic_filter_columns;
	if (filter_info) {
		for (auto &entry : filter_info->column_filters) {
			auto &col_filter = entry.second;
			auto dynamic_filter_data = DuckLakeUtil::GetOptionalDynamicFilterData(*col_filter.table_filter);
			if (dynamic_filter_data) {
				ExpressionType comparison_type;
				{
					lock_guard<mutex> l(dynamic_filter_data->lock);
					comparison_type = dynamic_filter_data->comparison_type;
				}
				dynamic_filter_columns.push_back(
				    {col_filter.column_field_index, comparison_type, col_filter.column_type});
			}
		}
	}

	string stats_select_list;
	string stats_join_list;
	string order_by_clause;
	for (idx_t i = 0; i < dynamic_filter_columns.size(); i++) {
		auto &dfc = dynamic_filter_columns[i];
		auto alias = StringUtil::Format("stats_%d", NumericCast<int64_t>(i));
		stats_select_list += StringUtil::Format(", %s.min_value, %s.max_value", alias.c_str(), alias.c_str());
		stats_join_list += StringUtil::Format(
		    "\nLEFT JOIN {METADATA_CATALOG}.ducklake_file_column_stats %s ON %s.data_file_id = data.data_file_id AND "
		    "%s.table_id = data.table_id AND %s.column_id = %d",
		    alias.c_str(), alias.c_str(), alias.c_str(), alias.c_str(), NumericCast<int64_t>(dfc.column_field_index));

		// Generate ORDER BY clause to optimize Top-N queries - order files by their min/max stats
		// so we find satisfying rows early and can skip remaining files via dynamic filter pruning.
		// We only order by the first dynamic filter column: Top-N typically has a single ordering column,
		// and multiple columns would have conflicting requirements (e.g., ORDER BY a DESC, b ASC).
		if (order_by_clause.empty()) {
			const bool seeking_high_values = dfc.comparison_type == ExpressionType::COMPARE_GREATERTHAN ||
			                                 dfc.comparison_type == ExpressionType::COMPARE_GREATERTHANOREQUALTO;
			const bool seeking_low_values = dfc.comparison_type == ExpressionType::COMPARE_LESSTHAN ||
			                                dfc.comparison_type == ExpressionType::COMPARE_LESSTHANOREQUALTO;
			if (seeking_high_values) {
				// For DESC Top-N (seeking high values), order by max_value DESC so files with highest values come first
				auto cast_expr = CastStatsToTarget(alias + ".max_value", dfc.column_type);
				order_by_clause = StringUtil::Format("\nORDER BY %s DESC NULLS LAST", cast_expr);
			} else if (seeking_low_values) {
				// For ASC Top-N (seeking low values), order by min_value ASC so files with lowest values come first
				auto cast_expr = CastStatsToTarget(alias + ".min_value", dfc.column_type);
				order_by_clause = StringUtil::Format("\nORDER BY %s ASC NULLS LAST", cast_expr);
			}
		}
	}

	string select_list = "data.data_file_id, " + GetFileSelectList("data") +
	                     ", data.row_id_start, data.begin_snapshot, data.partial_max, data.mapping_id, " +
	                     GetDeleteFileSelectList("del") + stats_select_list;

	string query;
	string where_clause;

	// Generate CTE section and WHERE clause if we have filter pushdown info
	if (filter_info && !filter_info->column_filters.empty()) {
		auto components = GenerateFilterPushdownComponents(*filter_info, table);
		query = components.cte_section;
		where_clause = components.where_clause;

		// Add bucket-partition pruning for equality / IN-list predicates on bucket()-partitioned columns.
		// Composes with the zone-map clause above — pruning narrows files, zone maps stay as a backstop.
		if (table.GetPartitionData()) {
			string bucket_clause = BuildBucketPartitionPruningClause(table, *filter_info);
			if (!bucket_clause.empty()) {
				if (!where_clause.empty()) {
					where_clause += " AND ";
				}
				where_clause += bucket_clause;
			}
		}
	}

	// Add base query
	query += StringUtil::Format(R"(
SELECT %s
FROM {METADATA_CATALOG}.ducklake_data_file data
%s
LEFT JOIN (
    SELECT *
    FROM {METADATA_CATALOG}.ducklake_delete_file df
    WHERE df.table_id=%d AND {VISIBLE_DELETE_FILE}
    ) del ON del.data_file_id = data.data_file_id
WHERE data.table_id=%d AND {VISIBLE_DATA_FILE}
		)",
	                            select_list, stats_join_list, table_id.index, table_id.index);

	// Add WHERE clause from filters if it was generated
	if (!where_clause.empty()) {
		query += "\nAND " + where_clause;
	}
	// Add ORDER BY clause for Top-N optimization if generated
	query += order_by_clause;
	auto result = Query(snapshot, query);
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to get data file list from DuckLake: ");
	}

	// Query inlined file deletions for this table
	auto inlined_deletions = ReadInlinedFileDeletions(table_id, snapshot);

	vector<DuckLakeFileListEntry> files;
	for (auto &row : *result) {
		DuckLakeFileListEntry file_entry;
		idx_t col_idx = 0;
		file_entry.file_id = DataFileIndex(row.GetValue<idx_t>(col_idx++));
		file_entry.file = ReadDataFile(table, row, col_idx, IsEncrypted());
		if (!row.IsNull(col_idx)) {
			file_entry.row_id_start = row.GetValue<idx_t>(col_idx);
		}
		col_idx++;
		file_entry.snapshot_id = row.GetValue<idx_t>(col_idx++);
		if (!row.IsNull(col_idx)) {
			auto partial_max = row.GetValue<idx_t>(col_idx);
			SetSnapshotFilter(snapshot, partial_max, file_entry);
		}
		col_idx++;
		if (!row.IsNull(col_idx)) {
			file_entry.mapping_id = MappingIndex(row.GetValue<idx_t>(col_idx));
		}
		col_idx++;
		file_entry.delete_file = ReadDeleteFile(table, row, col_idx, IsEncrypted());
		for (auto &dfc : dynamic_filter_columns) {
			string min_val;
			string max_val;
			if (!row.IsNull(col_idx)) {
				min_val = row.GetValue<string>(col_idx);
			}
			col_idx++;
			if (!row.IsNull(col_idx)) {
				max_val = row.GetValue<string>(col_idx);
			}
			col_idx++;
			file_entry.column_min_max.emplace(dfc.column_field_index,
			                                  make_pair(std::move(min_val), std::move(max_val)));
		}

		// Populate inlined file deletions for this file
		auto del_entry = inlined_deletions.find(file_entry.file_id.index);
		if (del_entry != inlined_deletions.end()) {
			file_entry.inlined_file_deletions = std::move(del_entry->second);
		}

		files.push_back(std::move(file_entry));
	}
	return files;
}

vector<DuckLakeFileListEntry> DuckLakeMetadataManager::GetTableInsertions(DuckLakeTableEntry &table,
                                                                          DuckLakeSnapshot start_snapshot,
                                                                          DuckLakeSnapshot end_snapshot) {
	auto table_id = table.GetTableId();
	string select_list = GetFileSelectList("data") +
	                     ", data.row_id_start, data.begin_snapshot, data.partial_max, data.mapping_id, " +
	                     GetDeleteFileSelectList("del");
	string branch_filter;
	if (transaction.GetCatalog().SupportsWritableBranches()) {
		branch_filter = " AND data.branch_id = {BRANCH_ID}";
	}
	// Files either match the exact snapshot range
	// Or they have partial_max set, which means they are a file with many snapshot ids, and might contain
	// the snapshot we need
	auto query =
	    StringUtil::Format(R"(
SELECT %s
FROM {METADATA_CATALOG}.ducklake_data_file data, (
	SELECT
		CAST(NULL AS VARCHAR) path,
		CAST(NULL AS BOOLEAN) path_is_relative,
		CAST(NULL AS BIGINT) file_size_bytes,
		CAST(NULL AS BIGINT) footer_size,
		CAST(NULL AS VARCHAR) encryption_key,
		CAST(NULL AS VARCHAR) format
) del
WHERE data.table_id=%d AND data.begin_snapshot <= {SNAPSHOT_ID} AND (
	(data.begin_snapshot >= %d) OR
	(data.partial_max IS NOT NULL AND data.partial_max >= %d)
)%s;
		)",
	                       select_list, table_id.index, start_snapshot.snapshot_id, start_snapshot.snapshot_id,
	                       branch_filter);

	auto result = Query(end_snapshot, query);
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to get table insertion file list from DuckLake: ");
	}
	vector<DuckLakeFileListEntry> files;
	for (auto &row : *result) {
		DuckLakeFileListEntry file_entry;
		idx_t col_idx = 0;
		file_entry.file = ReadDataFile(table, row, col_idx, IsEncrypted());
		if (!row.IsNull(col_idx)) {
			file_entry.row_id_start = row.GetValue<idx_t>(col_idx);
		}
		col_idx++;
		auto begin_snapshot = row.GetValue<idx_t>(col_idx++);
		file_entry.snapshot_id = begin_snapshot;
		if (!row.IsNull(col_idx)) {
			auto partial_max = row.GetValue<idx_t>(col_idx);
			// Set upper bound filter if partial_max > end_snapshot
			SetSnapshotFilter(end_snapshot, partial_max, file_entry);
			// Set lower bound filter if begin_snapshot < start_snapshot
			// This means the file contains rows from before start_snapshot that we need to filter out
			if (begin_snapshot < start_snapshot.snapshot_id) {
				file_entry.snapshot_filter_min = start_snapshot.snapshot_id;
			}
		}
		col_idx++;
		if (!row.IsNull(col_idx)) {
			file_entry.mapping_id = MappingIndex(row.GetValue<idx_t>(col_idx));
		}
		col_idx++;
		file_entry.delete_file = ReadDeleteFile(table, row, col_idx, IsEncrypted());
		files.push_back(std::move(file_entry));
	}
	return files;
}

vector<DuckLakeDeleteScanEntry> DuckLakeMetadataManager::GetTableDeletions(DuckLakeTableEntry &table,
                                                                           DuckLakeSnapshot start_snapshot,
                                                                           DuckLakeSnapshot end_snapshot) {
	auto table_id = table.GetTableId();
	string select_list = "data.data_file_id, " + GetFileSelectList("data") +
	                     ", data.row_id_start, data.record_count, data.mapping_id, " +
	                     GetDeleteFileSelectList("current_delete") + ", " + GetDeleteFileSelectList("previous_delete");

	// Check if we have an inlined deletion table for this table (usually cached, no DB hit)
	auto inlined_table_name = GetInlinedDeletionTableName(table_id, end_snapshot);
	bool has_inlined_table = !inlined_table_name.empty();
	string delete_event_branch_filter;
	string data_event_branch_filter;
	if (transaction.GetCatalog().SupportsWritableBranches()) {
		delete_event_branch_filter = " AND branch_id = {BRANCH_ID}";
		data_event_branch_filter = " AND branch_id = {BRANCH_ID}";
	}

	// Build the query with optional CTE for inlined deletions
	// Deletes come in four flavors:
	// 1. Deletes stored in the ducklake_delete_file table (partial deletes)
	// 2. Data files being deleted entirely through setting end_snapshot (full file deletes)
	// 3. Inlined file deletions stored in the metadata database
	// 4. Branch tombstones for inherited files (ducklake_deletion_data_file)
	// For all deletes, we need to obtain any PREVIOUS deletes as well to exclude rows already deleted
	string query;

	// Add CTE for aggregated inlined deletions if the table exists
	if (has_inlined_table) {
		query = StringUtil::Format(R"(
WITH inlined_dels AS (
	SELECT file_id,
	       LIST(STRUCT_PACK(row_id := row_id, snapshot_id := begin_snapshot)) as deletions,
	       MIN(begin_snapshot) as min_snapshot
	FROM {METADATA_CATALOG}.%s
	WHERE begin_snapshot >= %d AND begin_snapshot <= {SNAPSHOT_ID}
	GROUP BY file_id
),
main_results AS (
)",
		                           inlined_table_name, start_snapshot.snapshot_id);
	} else {
		query = "WITH main_results AS (\n";
	}

	// Main query: partial deletes from delete_file table and full file deletes
	query += StringUtil::Format(R"(
SELECT %s, current_delete.begin_snapshot FROM (
	SELECT data_file_id, begin_snapshot, path, path_is_relative, file_size_bytes, footer_size, encryption_key, format
	FROM {METADATA_CATALOG}.ducklake_delete_file
	WHERE table_id = %d AND begin_snapshot <= {SNAPSHOT_ID}%s
) AS current_delete
LEFT JOIN LATERAL (
	SELECT DISTINCT ON (data_file_id)
		data_file_id,
		path,
		path_is_relative,
		file_size_bytes,
		footer_size,
		encryption_key,
		format
	FROM {METADATA_CATALOG}.ducklake_delete_file
	WHERE table_id = %d AND begin_snapshot < %d%s
	ORDER BY data_file_id, begin_snapshot DESC
) AS previous_delete
USING (data_file_id)
JOIN (
	SELECT *
	FROM {METADATA_CATALOG}.ducklake_data_file data
	WHERE table_id = %d
) AS data
USING (data_file_id)

UNION ALL

SELECT %s, data.end_snapshot FROM (
	SELECT *
	FROM {METADATA_CATALOG}.ducklake_data_file
	WHERE table_id = %d AND end_snapshot >= %d AND end_snapshot <= {SNAPSHOT_ID}%s
) AS data
LEFT JOIN LATERAL (
	SELECT DISTINCT ON (data_file_id)
		data_file_id,
		path,
		path_is_relative,
		file_size_bytes,
		footer_size,
		encryption_key,
		format
	FROM {METADATA_CATALOG}.ducklake_delete_file
	WHERE table_id = %d AND begin_snapshot < data.end_snapshot
	ORDER BY data_file_id, begin_snapshot DESC
) AS previous_delete
USING (data_file_id), (
	SELECT CAST(NULL AS VARCHAR) AS path,
		CAST(NULL AS BOOLEAN) AS path_is_relative,
		CAST(NULL AS BIGINT) AS file_size_bytes,
		CAST(NULL AS BIGINT) AS footer_size,
		CAST(NULL AS VARCHAR) AS encryption_key,
		CAST(NULL AS VARCHAR) format
) current_delete
)",
	                            select_list, table_id.index, delete_event_branch_filter, table_id.index,
	                            start_snapshot.snapshot_id, delete_event_branch_filter, table_id.index, select_list,
	                            table_id.index, start_snapshot.snapshot_id, data_event_branch_filter, table_id.index);

	if (transaction.GetCatalog().SupportsWritableBranches()) {
		// Branch-local drop of an inherited file is recorded as a tombstone, not end_snapshot.
		query += StringUtil::Format(R"(
UNION ALL

SELECT %s, data.deleted_at_snapshot FROM (
	SELECT data.*, del.deleted_at_snapshot
	FROM {METADATA_CATALOG}.ducklake_deletion_data_file del
	JOIN {METADATA_CATALOG}.ducklake_data_file data ON data.data_file_id = del.object_id
	WHERE del.branch_id = {BRANCH_ID}
	  AND del.deleted_at_snapshot >= %d
	  AND del.deleted_at_snapshot <= {SNAPSHOT_ID}
	  AND data.table_id = %d
) AS data
LEFT JOIN LATERAL (
	SELECT DISTINCT ON (data_file_id)
		data_file_id,
		path,
		path_is_relative,
		file_size_bytes,
		footer_size,
		encryption_key,
		format
	FROM {METADATA_CATALOG}.ducklake_delete_file
	WHERE table_id = %d AND begin_snapshot < data.deleted_at_snapshot
	ORDER BY data_file_id, begin_snapshot DESC
) AS previous_delete
USING (data_file_id), (
	SELECT CAST(NULL AS VARCHAR) AS path,
		CAST(NULL AS BOOLEAN) AS path_is_relative,
		CAST(NULL AS BIGINT) AS file_size_bytes,
		CAST(NULL AS BIGINT) AS footer_size,
		CAST(NULL AS VARCHAR) AS encryption_key,
		CAST(NULL AS VARCHAR) format
) current_delete
)",
		                            select_list, start_snapshot.snapshot_id, table_id.index, table_id.index);
	}


	if (has_inlined_table) {
		string null_file_cols = "CAST(NULL AS VARCHAR) AS path, CAST(NULL AS BOOLEAN) AS path_is_relative, CAST(NULL "
		                        "AS BIGINT) AS file_size_bytes, CAST(NULL AS BIGINT) AS footer_size";
		if (IsEncrypted()) {
			null_file_cols += ", CAST(NULL AS VARCHAR) AS encryption_key";
		}
		null_file_cols += ", NULL format";
		query += StringUtil::Format(R"(
UNION ALL

SELECT data.data_file_id, %s, data.row_id_start, data.record_count, data.mapping_id,
       %s,
       %s,
       inlined_dels.min_snapshot
FROM {METADATA_CATALOG}.ducklake_data_file data
JOIN inlined_dels ON data.data_file_id = inlined_dels.file_id
WHERE data.table_id = %d
  AND data.data_file_id NOT IN (
      SELECT data_file_id FROM {METADATA_CATALOG}.ducklake_delete_file
      WHERE table_id = %d AND begin_snapshot <= {SNAPSHOT_ID}
  )
  AND (data.end_snapshot IS NULL OR data.end_snapshot < %d OR data.end_snapshot > {SNAPSHOT_ID})
)",
		                            GetFileSelectList("data"), null_file_cols, null_file_cols, table_id.index,
		                            table_id.index, start_snapshot.snapshot_id);
	}

	// Close the main_results CTE and do the final SELECT with LEFT JOIN on inlined_dels
	if (has_inlined_table) {
		query += R"(
)
SELECT main_results.*, inlined_dels.deletions
FROM main_results
LEFT JOIN inlined_dels ON main_results.data_file_id = inlined_dels.file_id
)";
	} else {
		query += R"(
)
SELECT main_results.*, NULL as deletions
FROM main_results
)";
	}

	auto result = Query(end_snapshot, query);
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to get table deletion file list from DuckLake: ");
	}

	// Build entries from the unified query result
	vector<DuckLakeDeleteScanEntry> files;
	for (auto &row : *result) {
		DuckLakeDeleteScanEntry entry;
		idx_t col_idx = 0;
		auto file_id = row.GetValue<idx_t>(col_idx++);
		entry.file_id = DataFileIndex(file_id);
		entry.file = ReadDataFile(table, row, col_idx, IsEncrypted());
		if (!row.IsNull(col_idx)) {
			entry.row_id_start = row.GetValue<idx_t>(col_idx);
		}
		col_idx++;
		entry.row_count = row.GetValue<idx_t>(col_idx++);
		if (!row.IsNull(col_idx)) {
			entry.mapping_id = MappingIndex(row.GetValue<idx_t>(col_idx));
		}
		col_idx++;
		entry.delete_file = ReadDeleteFile(table, row, col_idx, IsEncrypted());
		entry.previous_delete_file = ReadDeleteFile(table, row, col_idx, IsEncrypted());
		entry.snapshot_id = row.GetValue<idx_t>(col_idx++);
		// store the snapshot range for filtering embedded snapshot IDs
		entry.start_snapshot = start_snapshot.snapshot_id;
		entry.end_snapshot = end_snapshot.snapshot_id;

		// Parse inlined file deletions from the LIST column (last column)
		if (!row.IsNull(col_idx)) {
			auto deletions_list = row.GetValue<Value>(col_idx);
			auto &list_children = ListValue::GetChildren(deletions_list);
			for (auto &child : list_children) {
				auto &struct_children = StructValue::GetChildren(child);
				auto row_id = struct_children[0].GetValue<idx_t>();
				auto snapshot_id = struct_children[1].GetValue<idx_t>();
				entry.inlined_file_deletions[row_id] = snapshot_id;
			}
		}

		files.push_back(std::move(entry));
	}

	return files;
}

vector<DuckLakeFileListExtendedEntry>
DuckLakeMetadataManager::GetExtendedFilesForTable(DuckLakeTableEntry &table, DuckLakeSnapshot snapshot,
                                                  const FilterPushdownInfo *filter_info) {
	auto table_id = table.GetTableId();
	string select_list = GetFileSelectList("data") + ", data.row_id_start, data.mapping_id, " +
	                     GetDeleteFileSelectList("del") + ", del.begin_snapshot";

	string query;
	string where_clause;

	// Generate CTE section and WHERE clause if we have filter pushdown info
	if (filter_info && !filter_info->column_filters.empty()) {
		auto components = GenerateFilterPushdownComponents(*filter_info, table);
		query = components.cte_section;
		where_clause = components.where_clause;

		// Add bucket-partition pruning (see GetFilesForTable for rationale).
		if (table.GetPartitionData()) {
			string bucket_clause = BuildBucketPartitionPruningClause(table, *filter_info);
			if (!bucket_clause.empty()) {
				if (!where_clause.empty()) {
					where_clause += " AND ";
				}
				where_clause += bucket_clause;
			}
		}
	}

	// Add base query
	query += StringUtil::Format(R"(
SELECT data.data_file_id, del.delete_file_id, data.record_count, %s
FROM {METADATA_CATALOG}.ducklake_data_file data
LEFT JOIN (
	SELECT *
    FROM {METADATA_CATALOG}.ducklake_delete_file df
    WHERE df.table_id=%d AND {VISIBLE_DELETE_FILE}
    ) del USING (data_file_id)
WHERE data.table_id=%d AND {VISIBLE_DATA_FILE}
		)",
	                            select_list, table_id.index, table_id.index);

	// Add WHERE clause from filters if it was generated
	if (!where_clause.empty()) {
		query += "\nAND " + where_clause;
	}

	auto result = Query(snapshot, query);
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to get extended data file list from DuckLake: ");
	}
	vector<DuckLakeFileListExtendedEntry> files;
	for (auto &row : *result) {
		DuckLakeFileListExtendedEntry file_entry;
		file_entry.file_id = DataFileIndex(row.GetValue<idx_t>(0));
		if (!row.IsNull(1)) {
			file_entry.delete_file_id = DataFileIndex(row.GetValue<idx_t>(1));
		}
		file_entry.row_count = row.GetValue<idx_t>(2);
		idx_t col_idx = 3;
		file_entry.file = ReadDataFile(table, row, col_idx, IsEncrypted());
		if (!row.IsNull(col_idx)) {
			file_entry.row_id_start = row.GetValue<idx_t>(col_idx);
		}
		col_idx++;
		if (!row.IsNull(col_idx)) {
			file_entry.mapping_id = MappingIndex(row.GetValue<idx_t>(col_idx));
		}
		col_idx++;
		file_entry.delete_file = ReadDeleteFile(table, row, col_idx, IsEncrypted());
		if (!row.IsNull(col_idx)) {
			file_entry.delete_file_begin_snapshot = row.GetValue<idx_t>(col_idx);
		}
		col_idx++;
		files.push_back(std::move(file_entry));
	}
	return files;
}

vector<DuckLakeCompactionFileEntry> DuckLakeMetadataManager::GetFilesForCompaction(DuckLakeTableEntry &table,
                                                                                   CompactionType type,
                                                                                   double deletion_threshold,
                                                                                   DuckLakeSnapshot snapshot,
                                                                                   DuckLakeFileSizeOptions options) {
	auto table_id = table.GetTableId();
	// Determine the effective max file size threshold for filtering
	idx_t effective_max_file_size =
	    options.max_file_size.IsValid() ? options.max_file_size.GetIndex() : options.target_file_size;
	string data_select_list = "data.data_file_id, data.record_count, data.row_id_start, data.begin_snapshot, "
	                          "data.end_snapshot, data.mapping_id, sr.schema_version , data.partial_max, "
	                          "data.partition_id, partition_info.keys, " +
	                          GetFileSelectList("data");
	string delete_select_list = "del.data_file_id AS del_data_file_id,"
	                            "del.delete_file_id AS del_delete_file_id, "
	                            "del.delete_count, "
	                            "del.begin_snapshot AS del_begin_snapshot, "
	                            "del.end_snapshot AS del_end_snapshot, "
	                            "del.partial_max AS del_partial_max, " +
	                            GetDeleteFileSelectList("del");
	string select_list = data_select_list + ", " + delete_select_list;
	string deletion_threshold_clause;
	if (type == CompactionType::REWRITE_DELETES) {
		// Filter current data files in SQL, then apply the delete threshold in C++ so we can include
		// metadata-only inlined file deletions as rewrite candidates.
		deletion_threshold_clause = " AND data.end_snapshot is null";
	}
	// Add file size filtering for MERGE_ADJACENT_TABLES compaction
	string file_size_filter_clause;
	if (type == CompactionType::MERGE_ADJACENT_TABLES) {
		if (options.min_file_size.IsValid()) {
			file_size_filter_clause +=
			    StringUtil::Format(" AND data.file_size_bytes >= %llu", options.min_file_size.GetIndex());
		}
		file_size_filter_clause += StringUtil::Format(" AND data.file_size_bytes < %llu", effective_max_file_size);
		file_size_filter_clause += " AND data.end_snapshot IS NULL";
	}
	auto query = StringUtil::Format(R"(
WITH snapshot_ranges AS (
  SELECT
    begin_snapshot,
    COALESCE(
      LEAD(begin_snapshot) OVER (ORDER BY begin_snapshot),
      9223372036854775807
    ) AS end_snapshot,
	schema_version
	FROM {METADATA_CATALOG}.ducklake_schema_versions
	WHERE table_id=%d
	ORDER BY begin_snapshot
)
SELECT %s
FROM {METADATA_CATALOG}.ducklake_data_file data
LEFT JOIN snapshot_ranges sr
  ON data.begin_snapshot >= sr.begin_snapshot AND data.begin_snapshot < sr.end_snapshot
LEFT JOIN (
	SELECT *
    FROM {METADATA_CATALOG}.ducklake_delete_file
    WHERE table_id=%d
) del USING (data_file_id)
LEFT JOIN (
   SELECT data_file_id, ARRAY_AGG(partition_value ORDER BY partition_key_index) keys
   FROM {METADATA_CATALOG}.ducklake_file_partition_value
   GROUP BY data_file_id
) partition_info USING (data_file_id)
WHERE data.table_id=%d %s%s
ORDER BY data.begin_snapshot, data.row_id_start, data.data_file_id, del.begin_snapshot
		)",
	                                table_id.index, select_list, table_id.index, table_id.index,
	                                deletion_threshold_clause, file_size_filter_clause);
	auto result = Query(query);
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to get compaction file list from DuckLake: ");
	}
	vector<DuckLakeCompactionFileEntry> files;
	for (auto &row : *result) {
		idx_t col_idx = 0;
		DuckLakeCompactionFileEntry new_entry;
		// parse the data file
		new_entry.file.id = DataFileIndex(row.GetValue<idx_t>(col_idx++));
		new_entry.file.row_count = row.GetValue<idx_t>(col_idx++);
		if (!row.IsNull(col_idx)) {
			new_entry.file.row_id_start = row.GetValue<idx_t>(col_idx);
		}
		col_idx++;
		new_entry.file.begin_snapshot = row.GetValue<idx_t>(col_idx++);
		new_entry.file.end_snapshot = row.IsNull(col_idx) ? optional_idx() : row.GetValue<idx_t>(col_idx);
		col_idx++;
		if (!row.IsNull(col_idx)) {
			new_entry.file.mapping_id = MappingIndex(row.GetValue<idx_t>(col_idx));
		}
		col_idx++;
		new_entry.schema_version = row.GetValue<idx_t>(col_idx++);
		if (!row.IsNull(col_idx)) {
			new_entry.max_partial_file_snapshot = row.GetValue<idx_t>(col_idx);
		}
		col_idx++;
		new_entry.file.partition_id = row.IsNull(col_idx) ? optional_idx() : row.GetValue<idx_t>(col_idx);
		col_idx++;
		if (!row.IsNull(col_idx)) {
			auto list_val = row.GetValue<Value>(col_idx);
			for (auto &entry : ListValue::GetChildren(list_val)) {
				new_entry.file.partition_values.push_back(entry);
			}
		}
		col_idx++;
		new_entry.file.data = ReadDataFile(table, row, col_idx, IsEncrypted());
		if (files.empty() || files.back().file.id != new_entry.file.id) {
			// new file - push it into the file list
			files.push_back(std::move(new_entry));
		}
		auto &file_entry = files.back();
		// parse the delete file (if any)
		if (row.IsNull(col_idx)) {
			// no delete file
			continue;
		}
		DuckLakeCompactionDeleteFileData delete_file;
		delete_file.id = DataFileIndex(row.GetValue<idx_t>(col_idx++));
		delete_file.delete_file_id = DataFileIndex(row.GetValue<idx_t>(col_idx++));
		delete_file.row_count = row.GetValue<idx_t>(col_idx++);
		delete_file.begin_snapshot = row.GetValue<idx_t>(col_idx++);
		delete_file.end_snapshot = row.IsNull(col_idx) ? optional_idx() : row.GetValue<idx_t>(col_idx);
		col_idx++;
		if (!row.IsNull(col_idx)) {
			delete_file.max_snapshot = row.GetValue<idx_t>(col_idx);
		}
		col_idx++;
		delete_file.data = ReadDeleteFile(table, row, col_idx, IsEncrypted());
		file_entry.delete_files.push_back(std::move(delete_file));
	}

	if (type == CompactionType::REWRITE_DELETES) {
		// Full row-ID payload needed to compute delete ratio and perform the rewrite.
		auto inlined_deletions = ReadInlinedFileDeletions(table_id, snapshot);
		for (auto &file : files) {
			auto entry = inlined_deletions.find(file.file.id.index);
			if (entry != inlined_deletions.end()) {
				file.inlined_file_deletions = std::move(entry->second);
				file.has_inlined_deletions = true;
			}
		}
	} else {
		// Cheap existence-only check — avoids fetching every deleted row_id for non-rewrite paths.
		vector<idx_t> file_ids;
		file_ids.reserve(files.size());
		for (auto &file : files) {
			file_ids.push_back(file.file.id.index);
		}
		auto files_with_deletions = GetFileIdsWithInlinedDeletions(table_id, snapshot, file_ids);
		for (auto &file : files) {
			if (files_with_deletions.count(file.file.id.index)) {
				file.has_inlined_deletions = true;
			}
		}
	}

	if (type == CompactionType::REWRITE_DELETES) {
		for (idx_t file_idx = 0; file_idx < files.size(); file_idx++) {
			auto &file = files[file_idx];
			idx_t active_delete_count = 0;
			if (!file.delete_files.empty() && !file.delete_files.back().end_snapshot.IsValid()) {
				active_delete_count = file.delete_files.back().row_count;
			}
			auto total_delete_count = active_delete_count + file.inlined_file_deletions.size();
			if (file.file.row_count > 0) {
				file.delete_ratio = static_cast<double>(total_delete_count) / static_cast<double>(file.file.row_count);
			}
			if (total_delete_count == 0 || file.delete_ratio < deletion_threshold) {
				files.erase_at(file_idx);
				file_idx--;
			}
		}
	}

	return files;
}

template <class T>
string GenerateIDList(const set<T> &dropped_entries) {
	string dropped_id_list;
	for (auto &dropped_id : dropped_entries) {
		if (!dropped_id_list.empty()) {
			dropped_id_list += ", ";
		}
		dropped_id_list += to_string(dropped_id.index);
	}
	return dropped_id_list;
}

template <class T>
string DuckLakeMetadataManager::FlushDrop(const string &metadata_table_name, const string &id_name,
                                          const set<T> &dropped_entries, bool enforce_branch_ownership) {
	if (dropped_entries.empty()) {
		return {};
	}
	auto dropped_id_list = GenerateIDList(dropped_entries);
	if (!enforce_branch_ownership) {
		// Tags and similar tables have no branch_id — end-date unconditionally.
		return StringUtil::Format(
		    R"(UPDATE {METADATA_CATALOG}.%s SET end_snapshot = {SNAPSHOT_ID} WHERE end_snapshot IS NULL AND %s IN (%s);)",
		    metadata_table_name, id_name, dropped_id_list);
	}
	return EndDateOrTombstone(metadata_table_name, id_name, dropped_id_list);
}

string DuckLakeMetadataManager::DeletionTableFor(const string &metadata_table_name) {
	if (metadata_table_name == "ducklake_schema") {
		return "ducklake_deletion_schema";
	}
	if (metadata_table_name == "ducklake_table") {
		return "ducklake_deletion_table";
	}
	if (metadata_table_name == "ducklake_view") {
		return "ducklake_deletion_view";
	}
	if (metadata_table_name == "ducklake_column") {
		return "ducklake_deletion_column";
	}
	if (metadata_table_name == "ducklake_data_file") {
		return "ducklake_deletion_data_file";
	}
	if (metadata_table_name == "ducklake_delete_file") {
		return "ducklake_deletion_delete_file";
	}
	if (metadata_table_name == "ducklake_macro") {
		return "ducklake_deletion_macro";
	}
	if (metadata_table_name == "ducklake_partition_info") {
		return "ducklake_deletion_partition";
	}
	// sort_info and tags have no tombstone table
	return string();
}

string DuckLakeMetadataManager::EndDateOrTombstone(const string &metadata_table_name, const string &id_name,
                                                   const string &id_list) {
	if (id_list.empty()) {
		return {};
	}
	auto deletion_table = DeletionTableFor(metadata_table_name);
	string batch;
	// Own rows: end-date (filter by id_name — may be table_id for cascaded drops)
	batch += StringUtil::Format(
	    R"(UPDATE {METADATA_CATALOG}.%s SET end_snapshot = {SNAPSHOT_ID}
WHERE end_snapshot IS NULL AND %s IN (%s) AND branch_id = {BRANCH_ID};)",
	    metadata_table_name, id_name, id_list);
	if (deletion_table.empty()) {
		// No tombstone table (e.g. sort_info) — refuse inherited mutations rather than silently
		// end-dating an ancestor-owned row.
		batch += StringUtil::Format(
		    R"(SELECT error('Cannot drop or alter objects inherited from another branch for table %s')
WHERE EXISTS (
	SELECT 1 FROM {METADATA_CATALOG}.%s
	WHERE end_snapshot IS NULL AND %s IN (%s) AND branch_id != {BRANCH_ID}
);)",
		    metadata_table_name, metadata_table_name, id_name, id_list);
		return batch;
	}
	// Tombstone object_id must match LineageIntervalVisibility's object id column, which is the
	// row's primary identity — not always the same as the drop filter (e.g. DROP TABLE cascades
	// filter columns by table_id but tombstones column_id).
	string object_id_expr;
	if (metadata_table_name == "ducklake_column") {
		// Pack table_id into high bits — column_id alone is not globally unique.
		object_id_expr = "((m.table_id::BIGINT * 4294967296) + m.column_id)";
	} else if (metadata_table_name == "ducklake_data_file") {
		object_id_expr = "m.data_file_id";
	} else if (metadata_table_name == "ducklake_delete_file") {
		object_id_expr = "m.delete_file_id";
	} else if (metadata_table_name == "ducklake_partition_info") {
		object_id_expr = "m.partition_id";
	} else if (metadata_table_name == "ducklake_schema") {
		object_id_expr = "m.schema_id";
	} else if (metadata_table_name == "ducklake_table") {
		object_id_expr = "m.table_id";
	} else if (metadata_table_name == "ducklake_view") {
		object_id_expr = "m.view_id";
	} else if (metadata_table_name == "ducklake_macro") {
		object_id_expr = "m.macro_id";
	} else {
		object_id_expr = "m." + id_name;
	}
	// Inherited rows: write a tombstone so lineage anti-join hides them on this branch only.
	batch += StringUtil::Format(
	    R"(
INSERT INTO {METADATA_CATALOG}.%s (branch_id, ancestor_branch_id, object_id, deleted_at_snapshot)
SELECT DISTINCT {BRANCH_ID}, m.branch_id, %s, {SNAPSHOT_ID}
FROM {METADATA_CATALOG}.%s m
WHERE m.end_snapshot IS NULL AND m.%s IN (%s) AND m.branch_id != {BRANCH_ID}
  AND NOT EXISTS (
    SELECT 1 FROM {METADATA_CATALOG}.%s d
    WHERE d.branch_id = {BRANCH_ID} AND d.ancestor_branch_id = m.branch_id
      AND d.object_id = %s AND d.deleted_at_snapshot <= {SNAPSHOT_ID}
  );)",
	    deletion_table, object_id_expr, metadata_table_name, id_name, id_list, deletion_table, object_id_expr);
	return batch;
}

string DuckLakeMetadataManager::DropSchemas(const set<SchemaIndex> &ids) {
	return FlushDrop("ducklake_schema", "schema_id", ids, true);
}

string DuckLakeMetadataManager::DropTables(const set<TableIndex> &ids, bool renamed) {
	string batch_query = FlushDrop("ducklake_table", "table_id", ids, true);
	if (renamed == false) {
		batch_query += FlushDrop("ducklake_partition_info", "table_id", ids, true);
		batch_query += FlushDrop("ducklake_column", "table_id", ids, true);
		// Tags have no branch_id column; end-dating them is scoped by the parent table drop above.
		batch_query += FlushDrop("ducklake_column_tag", "table_id", ids, false);
		batch_query += FlushDrop("ducklake_data_file", "table_id", ids, true);
		batch_query += FlushDrop("ducklake_delete_file", "table_id", ids, true);
		batch_query += FlushDrop("ducklake_tag", "object_id", ids, false);
		batch_query += FlushDrop("ducklake_sort_info", "table_id", ids, true);
	}
	return batch_query;
}

string DuckLakeMetadataManager::DropViews(const set<TableIndex> &ids, bool renamed, bool drop_view_column_tags) {
	string batch_query = FlushDrop("ducklake_view", "view_id", ids, true);
	if (!renamed) {
		batch_query += FlushDrop("ducklake_tag", "object_id", ids, false);
		if (drop_view_column_tags) {
			batch_query += FlushDrop("ducklake_view_column_tag", "view_id", ids, false);
		}
	}
	return batch_query;
}

void DuckLakeMetadataManager::SubstituteCatalogPlaceholders(string &query) const {
	auto &ducklake_catalog = transaction.GetCatalog();
	auto catalog_identifier = DuckLakeUtil::SQLIdentifierToString(ducklake_catalog.MetadataDatabaseName());
	auto catalog_literal = DuckLakeUtil::SQLLiteralToString(ducklake_catalog.MetadataDatabaseName());
	auto schema_identifier = DuckLakeUtil::SQLIdentifierToString(ducklake_catalog.MetadataSchemaName());
	auto schema_identifier_escaped = StringUtil::Replace(schema_identifier, "'", "''");
	auto schema_literal = DuckLakeUtil::SQLLiteralToString(ducklake_catalog.MetadataSchemaName());
	auto metadata_path = DuckLakeUtil::SQLLiteralToString(ducklake_catalog.MetadataPath());
	auto data_path = DuckLakeUtil::SQLLiteralToString(ducklake_catalog.DataPath());

	query = StringUtil::Replace(query, "{METADATA_CATALOG_NAME_LITERAL}", catalog_literal);
	query = StringUtil::Replace(query, "{METADATA_CATALOG_NAME_IDENTIFIER}", catalog_identifier);
	query = StringUtil::Replace(query, "{METADATA_SCHEMA_NAME_LITERAL}", schema_literal);
	query = StringUtil::Replace(query, "{METADATA_CATALOG}", catalog_identifier + "." + schema_identifier);
	query = StringUtil::Replace(query, "{METADATA_SCHEMA_ESCAPED}", schema_identifier_escaped);
	query = StringUtil::Replace(query, "{METADATA_PATH}", metadata_path);
	query = StringUtil::Replace(query, "{DATA_PATH}", data_path);
}

string DuckLakeMetadataManager::ClassicIntervalVisibility(const string &alias) {
	D_ASSERT(!alias.empty());
	string prefix = alias + ".";
	return StringUtil::Format(
	    "{SNAPSHOT_ID} >= %sbegin_snapshot AND ({SNAPSHOT_ID} < %send_snapshot OR %send_snapshot IS NULL)", prefix,
	    prefix, prefix);
}

string DuckLakeMetadataManager::LineageIntervalVisibility(const string &alias, const string &object_id_column,
                                                          const string &deletion_table) {
	if (alias.empty()) {
		throw InternalException("LineageIntervalVisibility requires a non-empty table alias "
		                        "(unqualified branch_id binds incorrectly inside EXISTS subqueries)");
	}
	string prefix = alias + ".";
	string visible = StringUtil::Format(
	    R"(EXISTS (
	SELECT 1 FROM {METADATA_CATALOG}.ducklake_branch_lineage __dl_lin
	WHERE __dl_lin.branch_id = {BRANCH_ID}
	  AND __dl_lin.ancestor_branch_id = %sbranch_id
	  AND %sbegin_snapshot <= LEAST({SNAPSHOT_ID}, __dl_lin.max_visible_snapshot)
	  AND (%send_snapshot IS NULL OR %send_snapshot > LEAST({SNAPSHOT_ID}, __dl_lin.max_visible_snapshot))
))",
	    prefix, prefix, prefix, prefix);
	if (!deletion_table.empty() && !object_id_column.empty()) {
		visible += StringUtil::Format(
		    R"( AND NOT EXISTS (
	SELECT 1 FROM {METADATA_CATALOG}.%s __dl_del
	WHERE __dl_del.branch_id = {BRANCH_ID}
	  AND __dl_del.ancestor_branch_id = %sbranch_id
	  AND __dl_del.object_id = %s%s
	  AND __dl_del.deleted_at_snapshot <= {SNAPSHOT_ID}
))",
		    deletion_table, prefix, prefix, object_id_column);
	}
	return visible;
}

string DuckLakeMetadataManager::ColumnLineageIntervalVisibility(const string &alias) {
	// column_id is only unique within a table; pack table_id into the high 32 bits of object_id.
	if (alias.empty()) {
		throw InternalException("ColumnLineageIntervalVisibility requires a non-empty table alias");
	}
	string prefix = alias + ".";
	return StringUtil::Format(
	    R"(EXISTS (
	SELECT 1 FROM {METADATA_CATALOG}.ducklake_branch_lineage __dl_lin
	WHERE __dl_lin.branch_id = {BRANCH_ID}
	  AND __dl_lin.ancestor_branch_id = %sbranch_id
	  AND %sbegin_snapshot <= LEAST({SNAPSHOT_ID}, __dl_lin.max_visible_snapshot)
	  AND (%send_snapshot IS NULL OR %send_snapshot > LEAST({SNAPSHOT_ID}, __dl_lin.max_visible_snapshot))
) AND NOT EXISTS (
	SELECT 1 FROM {METADATA_CATALOG}.ducklake_deletion_column __dl_del
	WHERE __dl_del.branch_id = {BRANCH_ID}
	  AND __dl_del.ancestor_branch_id = %sbranch_id
	  AND __dl_del.object_id = ((%stable_id::BIGINT * 4294967296) + %scolumn_id)
	  AND __dl_del.deleted_at_snapshot <= {SNAPSHOT_ID}
))",
	    prefix, prefix, prefix, prefix, prefix, prefix, prefix);
}

void DuckLakeMetadataManager::ExpandBranchAwarePlaceholders(string &query, bool supports_writable_branches) {
	static const string WRITABLE_ONLY_START = "{WRITABLE_ONLY_START}";
	static const string WRITABLE_ONLY_END = "{WRITABLE_ONLY_END}";
	if (supports_writable_branches) {
		query = StringUtil::Replace(query, WRITABLE_ONLY_START, "");
		query = StringUtil::Replace(query, WRITABLE_ONLY_END, "");
		query = StringUtil::Replace(query, "{BRANCH_ID_COL}", ", branch_id");
		query = StringUtil::Replace(query, "{BRANCH_ID_VAL}", ", {BRANCH_ID}");
		query = StringUtil::Replace(query, "{BRANCH_ID_JOIN}", ", branch_id");
		query = StringUtil::Replace(query, "{BRANCH_STATS_FILTER}", " AND branch_id = {BRANCH_ID}");
		query = StringUtil::Replace(query, "{BRANCH_OWNED_FILTER}", " AND branch_id = {BRANCH_ID}");
		query = StringUtil::Replace(query, "{VISIBLE_SCHEMA}",
		                            LineageIntervalVisibility("sch", "schema_id", "ducklake_deletion_schema"));
		query = StringUtil::Replace(query, "{VISIBLE_TABLE}",
		                            LineageIntervalVisibility("tbl", "table_id", "ducklake_deletion_table"));
		// column_id is per-table, so tombstones pack (table_id << 32) | column_id
		query = StringUtil::Replace(query, "{VISIBLE_COLUMN}", ColumnLineageIntervalVisibility("col"));
		query = StringUtil::Replace(query, "{VISIBLE_VIEW}",
		                            LineageIntervalVisibility("view", "view_id", "ducklake_deletion_view"));
		query = StringUtil::Replace(
		    query, "{VISIBLE_MACRO}",
		    LineageIntervalVisibility("ducklake_macro", "macro_id", "ducklake_deletion_macro"));
		query = StringUtil::Replace(query, "{VISIBLE_PARTITION}",
		                            LineageIntervalVisibility("part", "partition_id", "ducklake_deletion_partition"));
		query = StringUtil::Replace(query, "{VISIBLE_SORT}", LineageIntervalVisibility("sort", "", ""));
		query = StringUtil::Replace(query, "{VISIBLE_DATA_FILE}",
		                            LineageIntervalVisibility("data", "data_file_id", "ducklake_deletion_data_file"));
		query = StringUtil::Replace(
		    query, "{VISIBLE_DELETE_FILE}",
		    LineageIntervalVisibility("df", "delete_file_id", "ducklake_deletion_delete_file"));
		query = StringUtil::Replace(query, "{VISIBLE_TAG}", ClassicIntervalVisibility("tag"));
		query = StringUtil::Replace(query, "{VISIBLE_COLUMN_TAG}", ClassicIntervalVisibility("col_tag"));
		query = StringUtil::Replace(query, "{VISIBLE_VIEW_COLUMN_TAG}", ClassicIntervalVisibility("vct"));
		// Only expose inlined tables registered for this branch or its lineage ancestors.
		query = StringUtil::Replace(query, "{BRANCH_INLINED_TABLE_FILTER}", R"(
		  AND (
		    inlined_data_tables.branch_id = {BRANCH_ID}
		    OR inlined_data_tables.branch_id IN (
		      SELECT ancestor_branch_id FROM {METADATA_CATALOG}.ducklake_branch_lineage
		      WHERE branch_id = {BRANCH_ID}
		    )
		  ))");
	} else {
		// Drop sections that only apply when writable-branch schema is present.
		idx_t start_pos;
		while ((start_pos = query.find(WRITABLE_ONLY_START)) != string::npos) {
			auto end_pos = query.find(WRITABLE_ONLY_END, start_pos);
			if (end_pos == string::npos) {
				break;
			}
			query.erase(start_pos, end_pos + WRITABLE_ONLY_END.size() - start_pos);
		}
		query = StringUtil::Replace(query, "{BRANCH_ID_COL}", "");
		query = StringUtil::Replace(query, "{BRANCH_ID_VAL}", "");
		query = StringUtil::Replace(query, "{BRANCH_ID_JOIN}", "");
		query = StringUtil::Replace(query, "{BRANCH_STATS_FILTER}", "");
		query = StringUtil::Replace(query, "{BRANCH_OWNED_FILTER}", "");
		query = StringUtil::Replace(query, "{BRANCH_INLINED_TABLE_FILTER}", "");
		query = StringUtil::Replace(query, "{VISIBLE_SCHEMA}", ClassicIntervalVisibility("sch"));
		query = StringUtil::Replace(query, "{VISIBLE_TABLE}", ClassicIntervalVisibility("tbl"));
		query = StringUtil::Replace(query, "{VISIBLE_COLUMN}", ClassicIntervalVisibility("col"));
		query = StringUtil::Replace(query, "{VISIBLE_VIEW}", ClassicIntervalVisibility("view"));
		query = StringUtil::Replace(query, "{VISIBLE_MACRO}", ClassicIntervalVisibility("ducklake_macro"));
		query = StringUtil::Replace(query, "{VISIBLE_PARTITION}", ClassicIntervalVisibility("part"));
		query = StringUtil::Replace(query, "{VISIBLE_SORT}", ClassicIntervalVisibility("sort"));
		query = StringUtil::Replace(query, "{VISIBLE_DATA_FILE}", ClassicIntervalVisibility("data"));
		query = StringUtil::Replace(query, "{VISIBLE_DELETE_FILE}", ClassicIntervalVisibility("df"));
		query = StringUtil::Replace(query, "{VISIBLE_TAG}", ClassicIntervalVisibility("tag"));
		query = StringUtil::Replace(query, "{VISIBLE_COLUMN_TAG}", ClassicIntervalVisibility("col_tag"));
		query = StringUtil::Replace(query, "{VISIBLE_VIEW_COLUMN_TAG}", ClassicIntervalVisibility("vct"));
	}
}

void DuckLakeMetadataManager::SubstituteSnapshotPlaceholders(DuckLakeSnapshot snapshot, string &query) const {
	auto &commit_info = transaction.GetCommitInfo();
	ExpandBranchAwarePlaceholders(query, transaction.GetCatalog().SupportsWritableBranches());
	query = StringUtil::Replace(query, "{SNAPSHOT_ID}", to_string(snapshot.snapshot_id));
	query = StringUtil::Replace(query, "{SCHEMA_VERSION}", to_string(snapshot.schema_version));
	query = StringUtil::Replace(query, "{NEXT_CATALOG_ID}", to_string(snapshot.next_catalog_id));
	query = StringUtil::Replace(query, "{NEXT_FILE_ID}", to_string(snapshot.next_file_id));
	query = StringUtil::Replace(query, "{BRANCH_ID}", to_string(snapshot.branch_id));
	query = StringUtil::Replace(query, "{AUTHOR}", commit_info.author.ToSQLString());
	query = StringUtil::Replace(query, "{COMMIT_MESSAGE}", commit_info.commit_message.ToSQLString());
	query = StringUtil::Replace(query, "{COMMIT_EXTRA_INFO}", commit_info.commit_extra_info.ToSQLString());
}

unique_ptr<QueryResult> DuckLakeMetadataManager::Execute(DuckLakeSnapshot snapshot, string &query) {
	return Query(snapshot, query);
}

unique_ptr<QueryResult> DuckLakeMetadataManager::Execute(string &query) {
	return Query(query);
}

unique_ptr<QueryResult> DuckLakeMetadataManager::Query(DuckLakeSnapshot snapshot, string &query) {
	SubstituteSnapshotPlaceholders(snapshot, query);
	return Query(query);
}

unique_ptr<QueryResult> DuckLakeMetadataManager::Query(string &query) {
	SubstituteCatalogPlaceholders(query);
	return transaction.ExecuteRaw(query);
}

unique_ptr<QueryResult> DuckLakeMetadataManager::Execute(DuckLakeSnapshot snapshot, string &&query) {
	return Execute(snapshot, query);
}

unique_ptr<QueryResult> DuckLakeMetadataManager::Execute(string &&query) {
	return Execute(query);
}

unique_ptr<QueryResult> DuckLakeMetadataManager::Query(DuckLakeSnapshot snapshot, string &&query) {
	return Query(snapshot, query);
}

unique_ptr<QueryResult> DuckLakeMetadataManager::Query(string &&query) {
	return Query(query);
}

string DuckLakeMetadataManager::DropMacros(const set<MacroIndex> &ids) {
	return FlushDrop("ducklake_macro", "macro_id", ids, true);
}
string DuckLakeMetadataManager::WriteNewSchemas(const vector<DuckLakeSchemaInfo> &new_schemas,
                                                const vector<DuckLakePath> &resolved_paths) {
	if (new_schemas.empty()) {
		throw InternalException("No schemas to create - should be handled elsewhere");
	}
	if (resolved_paths.size() != new_schemas.size()) {
		throw InternalException("WriteNewSchemas: resolved_paths size mismatch");
	}
	string schema_insert_sql;
	for (idx_t i = 0; i < new_schemas.size(); ++i) {
		auto &new_schema = new_schemas[i];
		auto &path = resolved_paths[i];
		if (!schema_insert_sql.empty()) {
			schema_insert_sql += ",";
		}
		auto schema_id = new_schema.id.index;
		schema_insert_sql += StringUtil::Format("(%d, '%s', {SNAPSHOT_ID}, NULL, %s, %s, %s{BRANCH_ID_VAL})", schema_id,
		                                        new_schema.uuid, SQLString(new_schema.name), SQLString(path.path),
		                                        path.path_is_relative ? "true" : "false");
	}
	return "INSERT INTO {METADATA_CATALOG}.ducklake_schema(schema_id, schema_uuid, begin_snapshot, end_snapshot, "
	       "schema_name, path, path_is_relative{BRANCH_ID_COL}) VALUES " +
	       schema_insert_sql + ";";
}

string GetExpressionType(ParsedExpression &expression) {
	switch (expression.GetExpressionType()) {
	case ExpressionType::OPERATOR_CAST: {
		auto &cast_expression = expression.Cast<CastExpression>();
		if (cast_expression.Child().GetExpressionType() == ExpressionType::VALUE_CONSTANT) {
			return "literal";
		}
		return "expression";
	}
	case ExpressionType::VALUE_CONSTANT:
		return "literal";
	default:
		return "expression";
	}
}

static void ColumnToSQLRecursive(const DuckLakeColumnInfo &column, TableIndex table_id, optional_idx parent,
                                 string &result) {
	if (!result.empty()) {
		result += ",";
	}
	string parent_idx = DuckLakeUtil::OptionalIdxOrNull(parent);

	string initial_default_val =
	    !column.initial_default.IsNull() ? SQLString::ToString(column.initial_default.ToString()) : "NULL";

	string default_val = "'NULL'";
	string default_val_system = "'duckdb'";
	string default_val_type = "'" + column.default_value_type + "'";

	if (!column.default_value.IsNull()) {
		auto value = column.default_value.GetValue<string>();
		if (column.default_value_type == "literal") {
			default_val = SQLString::ToString(value);
		} else if (column.default_value_type == "expression") {
			if (value.empty()) {
				default_val = "''";
			} else {
				auto sql_expr = Parser::ParseExpressionList(column.default_value.GetValue<string>());
				if (sql_expr.size() != 1) {
					throw InternalException("Expected a single expression");
				}
				default_val = SQLString::ToString(sql_expr[0]->ToString());
			}
		} else {
			throw InvalidInputException("Expression type %s not implemented for default value",
			                            column.default_value_type);
		}
	}

	auto column_id = column.id.index;
	auto column_order = column_id;

	result += StringUtil::Format("(%d, {SNAPSHOT_ID}, NULL, %d, %d, %s, %s, %s, %s, %s, %s, %s, %s{BRANCH_ID_VAL})",
	                             column_id, table_id.index, column_order, SQLString(column.name),
	                             SQLString(column.type), initial_default_val, default_val,
	                             column.nulls_allowed ? "true" : "false", parent_idx, default_val_type,
	                             default_val_system);
	for (auto &child : column.children) {
		ColumnToSQLRecursive(child, table_id, column_id, result);
	}
}

string DuckLakeMetadataManager::GetColumnTypeInternal(const LogicalType &column_type) {
	return column_type.ToString();
}

string DuckLakeMetadataManager::GetColumnType(const DuckLakeColumnInfo &col) {
	auto column_type = DuckLakeTypes::FromString(col.type);
	if (!TypeIsNativelySupported(column_type)) {
		if (!column_type.IsNested()) {
			return GetColumnTypeInternal(column_type);
		}
		return "VARCHAR";
	}
	switch (column_type.id()) {
	case LogicalTypeId::STRUCT: {
		string result;
		for (auto &child : col.children) {
			if (!result.empty()) {
				result += ", ";
			}
			result += StringUtil::Format("%s %s", SQLIdentifier(child.name), GetColumnType(child));
		}
		return "STRUCT(" + result + ")";
	}
	case LogicalTypeId::LIST: {
		return GetColumnType(col.children[0]) + "[]";
	}
	case LogicalTypeId::MAP: {
		return StringUtil::Format("MAP(%s, %s)", GetColumnType(col.children[0]), GetColumnType(col.children[1]));
	}
	default:
		if (!col.children.empty()) {
			// This is a nested structure that we currently do not support.
			throw NotImplementedException("Unsupported nested type %s in DuckLakeMetadataManager::GetColumnType",
			                              col.type);
		}
		return GetColumnTypeInternal(column_type);
	}
}

string DuckLakeMetadataManager::InlinedTableNameFor(idx_t table_id, idx_t schema_version) {
	return StringUtil::Format("ducklake_inlined_data_%d_%d", table_id, schema_version);
}

string DuckLakeMetadataManager::InlinedTableNameFor(idx_t table_id, idx_t schema_version, idx_t branch_id,
                                                   bool shared_layout) {
	if (shared_layout || branch_id == 0) {
		return InlinedTableNameFor(table_id, schema_version);
	}
	// H3 G4: per-branch physical tables keep non-main inlined rows isolated.
	return StringUtil::Format("ducklake_inlined_data_%d_%d_b%d", table_id, schema_version, branch_id);
}

string DuckLakeMetadataManager::InlinedTableDdlSql(const string &table_name, const string &column_defs,
                                                   bool shared_layout) {
	auto branch_column = shared_layout ? "branch_id BIGINT, " : "";
	return StringUtil::Format("CREATE TABLE IF NOT EXISTS {METADATA_CATALOG}.%s(row_id BIGINT, begin_snapshot BIGINT, "
	                          "end_snapshot BIGINT, %s%s);",
	                          SQLIdentifier(table_name), branch_column, column_defs);
}

string DuckLakeMetadataManager::InlinedTableRegistrationTuple(idx_t table_id, const string &table_name,
                                                              idx_t schema_version) {
	return StringUtil::Format("(%d, %s, %d{BRANCH_ID_VAL})", table_id, SQLString(table_name), schema_version);
}

string DuckLakeMetadataManager::LatestInlinedTableQuery(idx_t table_id, bool shared_layout) {
	auto branch_filter = shared_layout ? "" : "{BRANCH_STATS_FILTER}";
	return StringUtil::Format(
	    "SELECT table_name, schema_version FROM {METADATA_CATALOG}.ducklake_inlined_data_tables "
	    "WHERE table_id = %d%s AND schema_version = ("
	    "  SELECT MAX(schema_version) FROM {METADATA_CATALOG}.ducklake_inlined_data_tables "
	    "  WHERE table_id = %d%s)",
	    table_id, branch_filter, table_id, branch_filter);
}

string DuckLakeMetadataManager::GetInlinedTableQuery(const DuckLakeTableInfo &table, const string &table_name) {
	string column_defs;
	for (auto &col : table.columns) {
		if (!column_defs.empty()) {
			column_defs += ", ";
		}
		column_defs += StringUtil::Format("%s %s", SQLIdentifier(col.name), GetColumnType(col));
	}
	// We created a table here, flag we need to clear our cache at commit
	MarkPendingCacheClear();
	return InlinedTableDdlSql(table_name, column_defs, transaction.GetCatalog().GetInliningLayout() == "shared_table");
}

string DuckLakeMetadataManager::WriteNewTables(const vector<DuckLakeTableInfo> &new_tables,
                                               const vector<DuckLakePath> &resolved_paths) {
	if (new_tables.empty()) {
		return {};
	}
	if (resolved_paths.size() != new_tables.size()) {
		throw InternalException("WriteNewTables: resolved_paths size mismatch");
	}

	string column_insert_sql;
	string table_insert_sql;

	for (idx_t i = 0; i < new_tables.size(); ++i) {
		auto &table = new_tables[i];
		auto &path = resolved_paths[i];
		if (!table_insert_sql.empty()) {
			table_insert_sql += ", ";
		}
		auto schema_id = table.schema_id.index;
		table_insert_sql += StringUtil::Format("(%d, '%s', {SNAPSHOT_ID}, NULL, %d, %s, %s, %s{BRANCH_ID_VAL})",
		                                       table.id.index, table.uuid, schema_id, SQLString(table.name),
		                                       SQLString(path.path), path.path_is_relative ? "true" : "false");
		for (auto &column : table.columns) {
			ColumnToSQLRecursive(column, table.id, optional_idx(), column_insert_sql);
		}
	}
	string batch_query;
	// Batch table and column inserts into a single multi-statement query
	if (!table_insert_sql.empty()) {
		batch_query += "INSERT INTO {METADATA_CATALOG}.ducklake_table(table_id, table_uuid, begin_snapshot, "
		               "end_snapshot, schema_id, table_name, path, path_is_relative{BRANCH_ID_COL}) VALUES " +
		               table_insert_sql + ";";
	}
	if (!column_insert_sql.empty()) {
		batch_query +=
		    "INSERT INTO {METADATA_CATALOG}.ducklake_column(column_id, begin_snapshot, end_snapshot, table_id, "
		    "column_order, column_name, column_type, initial_default, default_value, nulls_allowed, parent_column, "
		    "default_value_type, default_value_dialect{BRANCH_ID_COL}) VALUES " +
		    column_insert_sql + ";";
	}

	return batch_query;
}

string DuckLakeMetadataManager::GetInlinedTableQueries(DuckLakeSnapshot commit_snapshot, const DuckLakeTableInfo &table,
                                                       string &inlined_tables, string &inlined_table_queries) {
	const bool shared_layout = transaction.GetCatalog().GetInliningLayout() == "shared_table";
	string inlined_table_name =
	    InlinedTableNameFor(table.id.index, commit_snapshot.schema_version, commit_snapshot.branch_id, shared_layout);
	if (!inlined_tables.empty()) {
		inlined_tables += ", ";
	}
	inlined_tables += InlinedTableRegistrationTuple(table.id.index, inlined_table_name, commit_snapshot.schema_version);
	if (!inlined_table_queries.empty()) {
		inlined_table_queries += "\n";
	}
	inlined_table_queries += GetInlinedTableQuery(table, inlined_table_name);
	return inlined_table_name;
}

string DuckLakeMetadataManager::WriteNewInlinedTables(DuckLakeSnapshot commit_snapshot,
                                                      const vector<DuckLakeTableInfo> &new_tables) {
	auto &catalog = transaction.GetCatalog();
	string inlined_tables;
	string inlined_table_queries;
	for (auto &table : new_tables) {
		if (catalog.DataInliningRowLimit(table.schema_id, table.id) == 0 || IsTransactionLocal(table.id)) {
			// not inlining for this table or inlining is for a table on this transaction, hence handled there - skip it
			continue;
		}
		// If columns are empty (e.g., for renamed tables), fetch them from the catalog
		const DuckLakeTableInfo *table_ptr = &table;
		DuckLakeTableInfo table_with_columns;
		if (table.columns.empty()) {
			auto current_snapshot = transaction.GetSnapshot();
			auto table_entry = catalog.GetEntryById(transaction, current_snapshot, table.id);
			if (table_entry) {
				auto &tbl = table_entry->Cast<DuckLakeTableEntry>();
				table_with_columns = table;
				table_with_columns.columns = tbl.GetTableColumns();
				table_ptr = &table_with_columns;
			}
		}
		// FIXME: we are skipping columns that have conflicting names, we should resolve this
		if (!CanInlineColumns(table_ptr->columns)) {
			continue;
		}
		GetInlinedTableQueries(commit_snapshot, *table_ptr, inlined_tables, inlined_table_queries);
	}
	if (inlined_tables.empty()) {
		return {};
	}
	string batch_query;
	// Batch both INSERT queries into a single multi-statement query to reduce round-trips
	batch_query +=
	    "INSERT INTO {METADATA_CATALOG}.ducklake_inlined_data_tables(table_id, table_name, schema_version{BRANCH_ID_COL}) "
	    "VALUES " +
	    inlined_tables + ";";
	batch_query += inlined_table_queries;
	return batch_query;
}

string DuckLakeMetadataManager::WriteNewMacros(const vector<DuckLakeMacroInfo> &new_macros) {
	string batch_query;
	for (auto &macro : new_macros) {
		// Insert in the macro table
		batch_query += StringUtil::Format(R"(
INSERT INTO {METADATA_CATALOG}.ducklake_macro(schema_id, macro_id, macro_name, begin_snapshot, end_snapshot{BRANCH_ID_COL}) values(%llu,%llu,%s,{SNAPSHOT_ID}, NULL{BRANCH_ID_VAL});
)",
		                                  macro.schema_id.index, macro.macro_id.index, SQLString(macro.macro_name));
		// Insert in the implementation table
		for (idx_t impl_id = 0; impl_id < macro.implementations.size(); ++impl_id) {
			auto &impl = macro.implementations[impl_id];
			batch_query += StringUtil::Format(R"(
INSERT INTO {METADATA_CATALOG}.ducklake_macro_impl values(%llu,%llu,%s,%s,%s);
)",
			                                  macro.macro_id.index, impl_id, SQLString(impl.dialect),
			                                  SQLString(impl.sql), SQLString(impl.type));

			for (idx_t param_id = 0; param_id < impl.parameters.size(); ++param_id) {
				// Insert in the parameter table
				auto &param = impl.parameters[param_id];
				batch_query +=
				    StringUtil::Format(R"(
INSERT INTO {METADATA_CATALOG}.ducklake_macro_parameters values(%llu,%llu,%llu,%s,%s,%s,%s);
)",
				                       macro.macro_id.index, impl_id, param_id, SQLString(param.parameter_name),
				                       SQLString(param.parameter_type), SQLString(param.default_value.ToString()),
				                       SQLString(param.default_value_type));
			}
		}
	}
	return batch_query;
}

string DuckLakeMetadataManager::WriteDroppedColumns(const vector<DuckLakeDroppedColumn> &dropped_columns) {
	if (dropped_columns.empty()) {
		return {};
	}
	string dropped_cols;
	for (auto &dropped_col : dropped_columns) {
		if (!dropped_cols.empty()) {
			dropped_cols += ", ";
		}
		dropped_cols += StringUtil::Format("(%d, %d)", dropped_col.table_id.index, dropped_col.field_id.index);
	}
	// Own columns: end-date. Inherited columns: write column tombstones (H1).
	// Duplicate VALUES in each statement — CTEs do not span multi-statement batches.
	return StringUtil::Format(R"(
UPDATE {METADATA_CATALOG}.ducklake_column
SET end_snapshot = {SNAPSHOT_ID}
FROM (VALUES %s) AS dropped_cols(tid, cid)
WHERE table_id=tid AND column_id=cid AND end_snapshot IS NULL AND branch_id = {BRANCH_ID};
INSERT INTO {METADATA_CATALOG}.ducklake_deletion_column (branch_id, ancestor_branch_id, object_id, deleted_at_snapshot)
SELECT DISTINCT {BRANCH_ID}, c.branch_id, ((c.table_id::BIGINT * 4294967296) + c.column_id), {SNAPSHOT_ID}
FROM (VALUES %s) AS d(tid, cid)
JOIN {METADATA_CATALOG}.ducklake_column c ON c.table_id = d.tid AND c.column_id = d.cid
WHERE c.end_snapshot IS NULL AND c.branch_id != {BRANCH_ID}
  AND NOT EXISTS (
    SELECT 1 FROM {METADATA_CATALOG}.ducklake_deletion_column del
    WHERE del.branch_id = {BRANCH_ID} AND del.ancestor_branch_id = c.branch_id
      AND del.object_id = ((c.table_id::BIGINT * 4294967296) + c.column_id)
      AND del.deleted_at_snapshot <= {SNAPSHOT_ID}
  );
)",
	                          dropped_cols, dropped_cols);
}

string DuckLakeMetadataManager::WriteNewColumns(const vector<DuckLakeNewColumn> &new_columns) {
	if (new_columns.empty()) {
		return {};
	}
	string column_insert_sql;
	for (auto &new_col : new_columns) {
		ColumnToSQLRecursive(new_col.column_info, new_col.table_id, new_col.parent_idx, column_insert_sql);
	}

	// insert column entries
	return "INSERT INTO {METADATA_CATALOG}.ducklake_column(column_id, begin_snapshot, end_snapshot, table_id, "
	       "column_order, column_name, column_type, initial_default, default_value, nulls_allowed, parent_column, "
	       "default_value_type, default_value_dialect{BRANCH_ID_COL}) VALUES " +
	       column_insert_sql + ";";
}

string DuckLakeMetadataManager::WriteNewViews(const vector<DuckLakeViewInfo> &new_views) {
	string view_insert_sql;
	for (auto &view : new_views) {
		if (!view_insert_sql.empty()) {
			view_insert_sql += ", ";
		}
		auto schema_id = view.schema_id.index;
		view_insert_sql += StringUtil::Format("(%d, '%s', {SNAPSHOT_ID}, NULL, %d, %s, %s, %s, %s{BRANCH_ID_VAL})",
		                                      view.id.index, view.uuid, schema_id, SQLString(view.name),
		                                      SQLString(view.dialect), SQLString(view.sql),
		                                      SQLString(DuckLakeUtil::ToQuotedList(view.column_aliases)));
	}
	if (!view_insert_sql.empty()) {
		// insert table entries
		return "INSERT INTO {METADATA_CATALOG}.ducklake_view(view_id, view_uuid, begin_snapshot, end_snapshot, "
		       "schema_id, view_name, dialect, sql, column_aliases{BRANCH_ID_COL}) VALUES " +
		       view_insert_sql + ";";
	}
	return {};
}

string DuckLakeMetadataManager::WriteNewInlinedData(DuckLakeSnapshot &commit_snapshot,
                                                    const vector<DuckLakeInlinedDataInfo> &new_data,
                                                    const vector<DuckLakeTableInfo> &new_tables,
                                                    const vector<DuckLakeTableInfo> &new_inlined_data_tables_result) {
	string batch_query;
	if (new_data.empty()) {
		return batch_query;
	}

	auto context_ptr = transaction.context.lock();
	auto &context = *context_ptr;
	const bool shared_layout = transaction.GetCatalog().GetInliningLayout() == "shared_table";
	for (auto &entry : new_data) {
		string inlined_table_name;
		for (auto &inlined_table : new_inlined_data_tables_result) {
			if (inlined_table.id == entry.table_id) {
				inlined_table_name = InlinedTableNameFor(inlined_table.id.index, commit_snapshot.schema_version,
				                                        commit_snapshot.branch_id, shared_layout);
			}
		}
		if (inlined_table_name.empty()) {
			// get the latest table to insert into
			auto it = insert_inlined_table_name_cache.find(entry.table_id.index);
			if (it != insert_inlined_table_name_cache.end()) {
				inlined_table_name = it->second;
			}
		}
		if (inlined_table_name.empty()) {
			auto query = LatestInlinedTableQuery(entry.table_id.index, shared_layout) + ";";
			auto result = Query(commit_snapshot, query);
			for (auto &row : *result) {
				inlined_table_name = row.GetValue<string>(0);
				insert_inlined_table_name_cache[entry.table_id.index] = inlined_table_name;
			}
		}

		DuckLakeTableInfo table_info;
		if (inlined_table_name.empty()) {
			// no inlined table yet - create a new one
			// first fetch the table info
			auto current_snapshot = transaction.GetSnapshot();
			auto table_entry = transaction.GetCatalog().GetEntryById(transaction, current_snapshot, entry.table_id);
			if (table_entry) {
				auto &table = table_entry->Cast<DuckLakeTableEntry>();
				table_info = table.GetTableInfo();
				table_info.columns = table.GetTableColumns();
			} else {
				// We try from our added tables
				bool found = false;
				for (auto &new_table : new_tables) {
					if (new_table.id == entry.table_id) {
						table_info = new_table;
						found = true;
					}
				}
				if (!found) {
					throw InternalException("Writing inlined data for a table that cannot be found in the catalog");
				}
			}
			// write the new inlined table
			string inlined_tables;
			string inlined_table_queries;
			commit_snapshot.schema_version++;
			inlined_table_name =
			    GetInlinedTableQueries(commit_snapshot, table_info, inlined_tables, inlined_table_queries);
			batch_query +=
			    "INSERT INTO {METADATA_CATALOG}.ducklake_inlined_data_tables(table_id, table_name, "
			    "schema_version{BRANCH_ID_COL}) VALUES " +
			    inlined_tables + ";";
			batch_query += inlined_table_queries;
		}

		// Build one cell list per row, then defer formatting to the shared helper.
		// FIXME: we can do a much faster append than this
		const bool has_preserved_row_ids = entry.data->HasPreservedRowIds();
		vector<string> cells_per_row;
		for (auto &chunk : entry.data->data->Chunks()) {
			for (idx_t r = 0; r < chunk.size(); r++) {
				cells_per_row.push_back(DuckLakeUtil::ChunkRowToSQL(*this, context, chunk, r));
			}
		}
		batch_query += FormatInlinedDataInsert(inlined_table_name, entry.row_id_start, has_preserved_row_ids,
		                                       has_preserved_row_ids ? &entry.data->row_ids : nullptr, cells_per_row,
		                                       shared_layout);
	}
	return batch_query;
}

string DuckLakeMetadataManager::FormatInlinedDataInsert(const string &inlined_table_name, idx_t row_id_start,
                                                        bool has_preserved_row_ids, const vector<int64_t> *row_ids,
                                                        const vector<string> &cells_per_row, bool shared_layout) {
	if (cells_per_row.empty()) {
		return string();
	}
	idx_t row_id = row_id_start;
	string values;
	for (idx_t i = 0; i < cells_per_row.size(); i++) {
		int64_t emit_rid;
		if (has_preserved_row_ids) {
			int64_t staged_rid = (*row_ids)[i];
			emit_rid =
			    DuckLakeConstants::IsTransactionLocalRowId(staged_rid) ? static_cast<int64_t>(row_id++) : staged_rid;
		} else {
			emit_rid = static_cast<int64_t>(row_id++);
		}
		if (!values.empty()) {
			values += ", ";
		}
		if (shared_layout) {
			values += StringUtil::Format("(%lld, {SNAPSHOT_ID}, NULL, {BRANCH_ID}, %s)", emit_rid, cells_per_row[i]);
		} else {
			values += StringUtil::Format("(%lld, {SNAPSHOT_ID}, NULL, %s)", emit_rid, cells_per_row[i]);
		}
	}
	return StringUtil::Format("INSERT INTO {METADATA_CATALOG}.%s VALUES %s;", SQLIdentifier(inlined_table_name),
	                          values);
}

string DuckLakeMetadataManager::WriteNewInlinedDeletes(const vector<DuckLakeDeletedInlinedDataInfo> &new_deletes,
                                                       bool shared_layout) {
	string batch_queries;
	if (new_deletes.empty()) {
		return batch_queries;
	}
	for (auto &entry : new_deletes) {
		// get a list of all deleted row-ids for this table
		string row_id_list;
		for (auto &deleted_id : entry.deleted_row_ids) {
			if (!row_id_list.empty()) {
				row_id_list += ", ";
			}
			row_id_list += StringUtil::Format("(%d)", deleted_id);
		}
		// overwrite the snapshot for the old tags
		batch_queries += StringUtil::Format(R"(
WITH deleted_row_list(deleted_row_id) AS (
VALUES %s
)
UPDATE {METADATA_CATALOG}.%s
SET end_snapshot = {SNAPSHOT_ID}
FROM deleted_row_list
WHERE row_id=deleted_row_id AND end_snapshot IS NULL AND begin_snapshot != {SNAPSHOT_ID}%s;
)",
		                                    row_id_list, SQLIdentifier(entry.table_name),
		                                    shared_layout ? " AND branch_id = {BRANCH_ID}" : "");
	}
	return batch_queries;
}

string DuckLakeMetadataManager::InlinedFileDeletionTableName(TableIndex table_id) {
	return StringUtil::Format("ducklake_inlined_delete_%d", table_id.index);
}

string
DuckLakeMetadataManager::WriteNewInlinedFileDeletesSql(const vector<DuckLakeInlinedFileDeletionInfo> &new_deletes,
                                                       bool &created_new_table) {
	created_new_table = false;
	string batch_queries;
	if (new_deletes.empty()) {
		return batch_queries;
	}
	set<idx_t> created_tables;
	for (auto &entry : new_deletes) {
		auto table_name = InlinedFileDeletionTableName(entry.table_id);
		if (created_tables.insert(entry.table_id.index).second) {
			batch_queries += StringUtil::Format("CREATE TABLE IF NOT EXISTS {METADATA_CATALOG}.%s"
			                                    "(file_id BIGINT, row_id BIGINT, begin_snapshot BIGINT);\n",
			                                    table_name);
			created_new_table = true;
		}
		string values;
		for (auto &file_entry : entry.file_deletions.file_deletes) {
			auto file_id = file_entry.first;
			for (auto &row_id : file_entry.second) {
				if (!values.empty()) {
					values += ", ";
				}
				values += StringUtil::Format("(%d, %d, {SNAPSHOT_ID})", file_id, row_id);
			}
		}
		batch_queries += StringUtil::Format("INSERT INTO {METADATA_CATALOG}.%s VALUES %s;\n", table_name, values);
	}
	return batch_queries;
}

string DuckLakeMetadataManager::WriteNewInlinedFileDeletesSqlBatch(
    const vector<DuckLakeInlinedFileDeletionInfo> &new_deletes) {
	bool created_new_table = false;
	auto batch_queries = WriteNewInlinedFileDeletesSql(new_deletes, created_new_table);
	if (created_new_table) {
		// We create a table here, flag we need to clear our cache at commit
		MarkPendingCacheClear();
	}
	return batch_queries;
}

string DuckLakeMetadataManager::WriteNewInlinedFileDeletes(DuckLakeSnapshot &commit_snapshot,
                                                           const vector<DuckLakeInlinedFileDeletionInfo> &new_deletes) {
	if (new_deletes.empty()) {
		return string();
	}
	// Ensure each per-table deletion table exists (side effect; goes through transaction + cache).
	for (auto &entry : new_deletes) {
		GetInlinedDeletionTableName(entry.table_id, commit_snapshot, true);
	}
	return WriteNewInlinedFileDeletesSqlBatch(new_deletes);
}

void DuckLakeMetadataManager::ClearInlinedTableCaches() {
	insert_inlined_table_name_cache.clear();
	delete_inlined_table_cache.clear();
}

map<idx_t, set<idx_t>> DuckLakeMetadataManager::ReadInlinedFileDeletions(TableIndex table_id,
                                                                         DuckLakeSnapshot snapshot) {
	map<idx_t, set<idx_t>> result;
	auto inlined_table_name = GetInlinedDeletionTableName(table_id, snapshot);
	if (inlined_table_name.empty()) {
		return result;
	}
	auto query = StringUtil::Format("SELECT file_id, row_id FROM {METADATA_CATALOG}.%s WHERE begin_snapshot <= "
	                                "{SNAPSHOT_ID}",
	                                inlined_table_name);
	auto query_result = Query(snapshot, query);
	if (query_result->HasError()) {
		query_result->GetErrorObject().Throw("Failed to read inlined file deletions from DuckLake: ");
	}
	for (auto &row : *query_result) {
		auto file_id = row.GetValue<idx_t>(0);
		auto row_id = row.GetValue<idx_t>(1);
		result[file_id].insert(row_id);
	}
	return result;
}

// FIXME: We should probably cache this..
unordered_set<idx_t> DuckLakeMetadataManager::GetFileIdsWithInlinedDeletions(TableIndex table_id,
                                                                             DuckLakeSnapshot snapshot,
                                                                             const vector<idx_t> &file_ids) {
	unordered_set<idx_t> result;
	if (file_ids.empty()) {
		return result;
	}
	auto inlined_table_name = GetInlinedDeletionTableName(table_id, snapshot);
	if (inlined_table_name.empty()) {
		return result;
	}
	// Build the IN clause with file IDs
	string file_id_list;
	for (auto &file_id : file_ids) {
		if (!file_id_list.empty()) {
			file_id_list += ", ";
		}
		file_id_list += to_string(file_id);
	}
	auto query = StringUtil::Format("SELECT DISTINCT file_id FROM {METADATA_CATALOG}.%s WHERE file_id IN (%s) AND "
	                                "begin_snapshot <= {SNAPSHOT_ID}",
	                                inlined_table_name, file_id_list);
	auto query_result = Query(snapshot, query);
	if (query_result->HasError()) {
		query_result->GetErrorObject().Throw("Failed to read inlined file deletion IDs from DuckLake: ");
	}
	for (auto &row : *query_result) {
		result.insert(row.GetValue<idx_t>(0));
	}
	return result;
}

map<idx_t, unordered_map<idx_t, idx_t>>
DuckLakeMetadataManager::ReadInlinedFileDeletionsForRange(TableIndex table_id, DuckLakeSnapshot start_snapshot,
                                                          DuckLakeSnapshot end_snapshot) {
	map<idx_t, unordered_map<idx_t, idx_t>> result;
	auto inlined_table_name = GetInlinedDeletionTableName(table_id, end_snapshot);
	if (inlined_table_name.empty()) {
		return result;
	}
	auto query = StringUtil::Format("SELECT file_id, row_id, begin_snapshot FROM {METADATA_CATALOG}.%s "
	                                "WHERE begin_snapshot >= %d AND begin_snapshot <= {SNAPSHOT_ID}",
	                                inlined_table_name, start_snapshot.snapshot_id);
	auto query_result = Query(end_snapshot, query);
	if (query_result->HasError()) {
		query_result->GetErrorObject().Throw("Failed to read inlined file deletions for range from DuckLake: ");
	}
	for (auto &row : *query_result) {
		auto file_id = row.GetValue<idx_t>(0);
		auto row_id = row.GetValue<idx_t>(1);
		auto snapshot_id = row.GetValue<idx_t>(2);
		result[file_id][row_id] = snapshot_id;
	}
	return result;
}

string DuckLakeMetadataManager::GetInlinedDeletionTableName(TableIndex table_id, DuckLakeSnapshot snapshot,
                                                            bool create_if_not_exists) {
	// The table name is always deterministic
	string table_name = InlinedFileDeletionTableName(table_id);

	// Check per-transaction cache first (covers tables created in this transaction)
	if (delete_inlined_table_cache.find(table_id.index) != delete_inlined_table_cache.end()) {
		return table_name;
	}

	// Check catalog-level cache (persists across transactions)
	auto &catalog = transaction.GetCatalog();
	auto cache_result = catalog.CheckInlinedDeletionTableCache(table_id, snapshot);
	if (cache_result == InlinedDeletionCacheResult::EXISTS) {
		return table_name; // known to exist (committed)
	}
	if (cache_result == InlinedDeletionCacheResult::DOES_NOT_EXIST && !create_if_not_exists) {
		return string(); // known to not exist
	}

	if (create_if_not_exists) {
		auto create_query = StringUtil::Format(
		    "CREATE TABLE IF NOT EXISTS {METADATA_CATALOG}.%s(file_id BIGINT, row_id BIGINT, begin_snapshot BIGINT);",
		    table_name);
		auto create_result = Execute(snapshot, create_query);
		if (create_result->HasError()) {
			create_result->GetErrorObject().Throw("Failed to create inlined deletion table: ");
		}
		// Only cache per-transaction — the CREATE is transactional and may be rolled back
		delete_inlined_table_cache.insert(table_id.index);
		ClearCache();
		return table_name;
	}

	// Read path: table visibility implies it was committed, safe to cache at catalog level
	auto query = StringUtil::Format("SELECT NULL FROM {METADATA_CATALOG}.%s LIMIT 1", table_name);
	auto result = Query(snapshot, query);
	// TODO: Using the error state to check for existence here is fragile.
	// Even if the table exists, a transient error in the catalog query would lead us to assume it does not exist.
	// Maybe persist the existence of the deletion inlining table on the table metadata instead?
	if (!result->HasError()) {
		delete_inlined_table_cache.insert(table_id.index);
		catalog.CacheInlinedDeletionTableResult(table_id, snapshot, true);
		return table_name;
	}
	catalog.CacheInlinedDeletionTableResult(table_id, snapshot, false);
	return string();
}

shared_ptr<DuckLakeInlinedData> DuckLakeMetadataManager::TransformInlinedData(QueryResult &result,
                                                                              const vector<LogicalType> &) {
	if (result.HasError()) {
		result.GetErrorObject().Throw("Failed to read inlined data from DuckLake: ");
	}

	auto context = transaction.context.lock();
	auto data = make_uniq<ColumnDataCollection>(*context, result.types);
	while (true) {
		auto chunk = result.Fetch();
		if (!chunk) {
			break;
		}
		data->Append(*chunk);
	}
	auto inlined_data = make_shared_ptr<DuckLakeInlinedData>();
	inlined_data->data = std::move(data);
	return inlined_data;
}

static string GetProjection(const vector<string> &columns_to_read) {
	string result;
	idx_t i = 1;
	for (auto &entry : columns_to_read) {
		if (!result.empty()) {
			result += ", ";
		}
		// alias to avoid duplicate name in PG
		result += entry + StringUtil::Format(" AS col%d", i);
		i++;
	}
	return result;
}

unique_ptr<QueryResult> DuckLakeMetadataManager::ReadInlinedData(DuckLakeSnapshot snapshot,
                                                                 const string &inlined_table_name,
                                                                 const vector<string> &columns_to_read) {
	auto projection = GetProjection(columns_to_read);
	const bool shared_layout =
	    transaction.GetCatalog().SupportsWritableBranches() && transaction.GetCatalog().GetInliningLayout() == "shared_table";
	// Cap at lineage max_visible so post-fork ancestor inserts stay hidden on child branches.
	DuckLakeSnapshot read_snapshot = snapshot;
	if (!shared_layout) {
		read_snapshot.snapshot_id = GetEffectiveInlinedReadSnapshot(snapshot, inlined_table_name);
	}
	auto shared_filter = shared_layout ? " AND " + SharedInlinedVisibilityPredicate("inlined_data") : "";
	auto result = Query(read_snapshot, StringUtil::Format(R"(
SELECT %s
FROM {METADATA_CATALOG}.%s inlined_data
WHERE {SNAPSHOT_ID} >= begin_snapshot AND ({SNAPSHOT_ID} < end_snapshot OR end_snapshot IS NULL)
%s
ORDER BY row_id;)",
	                                                      projection, SQLIdentifier(inlined_table_name), shared_filter));
	return result;
}

unique_ptr<QueryResult> DuckLakeMetadataManager::ReadInlinedDataInsertions(DuckLakeSnapshot start_snapshot,
                                                                           DuckLakeSnapshot end_snapshot,
                                                                           const string &inlined_table_name,
                                                                           const vector<string> &columns_to_read) {
	auto projection = GetProjection(columns_to_read);
	const bool shared_layout =
	    transaction.GetCatalog().SupportsWritableBranches() && transaction.GetCatalog().GetInliningLayout() == "shared_table";
	DuckLakeSnapshot read_end = end_snapshot;
	if (!shared_layout) {
		read_end.snapshot_id = GetEffectiveInlinedReadSnapshot(end_snapshot, inlined_table_name);
	}
	auto shared_filter = shared_layout ? " AND inlined_data.branch_id = {BRANCH_ID}" : "";
	auto result = Query(read_end, StringUtil::Format(R"(
SELECT %s
FROM {METADATA_CATALOG}.%s inlined_data
WHERE inlined_data.begin_snapshot >= %d AND inlined_data.begin_snapshot <= {SNAPSHOT_ID}
%s;)",
	                                                 projection, SQLIdentifier(inlined_table_name),
	                                                 start_snapshot.snapshot_id, shared_filter));
	return result;
}

unique_ptr<QueryResult> DuckLakeMetadataManager::ReadInlinedDataDeletions(DuckLakeSnapshot start_snapshot,
                                                                          DuckLakeSnapshot end_snapshot,
                                                                          const string &inlined_table_name,
                                                                          const vector<string> &columns_to_read) {
	auto projection = GetProjection(columns_to_read);
	const bool shared_layout =
	    transaction.GetCatalog().SupportsWritableBranches() && transaction.GetCatalog().GetInliningLayout() == "shared_table";
	DuckLakeSnapshot read_end = end_snapshot;
	if (!shared_layout) {
		read_end.snapshot_id = GetEffectiveInlinedReadSnapshot(end_snapshot, inlined_table_name);
	}
	auto shared_filter = shared_layout ? " AND inlined_data.branch_id = {BRANCH_ID}" : "";
	auto result = Query(read_end, StringUtil::Format(R"(
SELECT %s
FROM {METADATA_CATALOG}.%s inlined_data
WHERE inlined_data.end_snapshot >= %d AND inlined_data.end_snapshot <= {SNAPSHOT_ID}
%s;)",
	                                                 projection, SQLIdentifier(inlined_table_name),
	                                                 start_snapshot.snapshot_id, shared_filter));
	return result;
}

unique_ptr<QueryResult> DuckLakeMetadataManager::ReadAllInlinedDataForFlush(DuckLakeSnapshot snapshot,
                                                                            const string &inlined_table_name,
                                                                            const vector<string> &columns_to_read) {
	auto projection = GetProjection(columns_to_read);
	const bool shared_layout =
	    transaction.GetCatalog().SupportsWritableBranches() && transaction.GetCatalog().GetInliningLayout() == "shared_table";
	auto shared_filter = shared_layout ? " AND branch_id = {BRANCH_ID}" : "";
	auto result = Query(snapshot, StringUtil::Format(R"(
SELECT %s
FROM {METADATA_CATALOG}.%s inlined_data
WHERE {SNAPSHOT_ID} >= begin_snapshot%s
ORDER BY row_id, begin_snapshot;)",
	                                                 projection, SQLIdentifier(inlined_table_name), shared_filter));
	return result;
}

string DuckLakeMetadataManager::ReadInlinedDataAggregatesSql(const string &inlined_table_name,
                                                             const string &select_list, bool shared_layout) {
	auto shared_filter = shared_layout ? " AND " + SharedInlinedVisibilityPredicate("inlined_data") : "";
	return StringUtil::Format(R"(
SELECT %s
FROM {METADATA_CATALOG}.%s inlined_data
WHERE {SNAPSHOT_ID} >= begin_snapshot AND ({SNAPSHOT_ID} < end_snapshot OR end_snapshot IS NULL)%s;
)",
	                          select_list, SQLIdentifier(inlined_table_name), shared_filter);
}

string DuckLakeMetadataManager::ReadFileColumnStatsForTableSql(TableIndex table_id) {
	return StringUtil::Format(R"(
SELECT data.data_file_id, data.record_count, data.file_size_bytes,
       stats.column_id, stats.value_count, stats.null_count, stats.min_value, stats.max_value,
       stats.contains_nan, stats.extra_stats
FROM {METADATA_CATALOG}.ducklake_data_file data
LEFT JOIN {METADATA_CATALOG}.ducklake_file_column_stats stats ON stats.data_file_id = data.data_file_id
WHERE data.table_id = %d
  AND {SNAPSHOT_ID} >= data.begin_snapshot
  AND ({SNAPSHOT_ID} < data.end_snapshot OR data.end_snapshot IS NULL)
ORDER BY data.data_file_id;
)",
	                          table_id.index);
}

string DuckLakeMetadataManager::GetPathForSchema(SchemaIndex schema_id,
                                                 vector<DuckLakeSchemaInfo> &new_schemas_result) {
	for (auto &schema : new_schemas_result) {
		if (schema_id == schema.id) {
			DuckLakePath path;
			path.path = schema.path;
			path.path_is_relative = false;
			return FromRelativePath(path);
		}
	}
	auto result = Query(StringUtil::Format(R"(
SELECT path, path_is_relative
FROM {METADATA_CATALOG}.ducklake_schema
WHERE schema_id = %d;)",
	                                       schema_id.index));
	for (auto &row : *result) {
		DuckLakePath path;
		path.path = row.GetValue<string>(0);
		path.path_is_relative = row.GetValue<bool>(1);
		return FromRelativePath(path);
	}
	throw InvalidInputException("Failed to get path for schema with id %d - schema not found in metadata catalog",
	                            schema_id.index);
}

bool DuckLakeMetadataManager::IsColumnCreatedWithTable(const string &table_name, const string &column_name) {
	auto result = Query(StringUtil::Format(R"(
SELECT TRUE
FROM {METADATA_CATALOG}.ducklake_table t
INNER JOIN {METADATA_CATALOG}.ducklake_column c
  ON c.table_id = t.table_id
 WHERE c.column_name = %s AND
 t.table_name = %s AND c.begin_snapshot = t.begin_snapshot AND c.end_snapshot IS NULL;
)",
	                                       SQLString(column_name), SQLString(table_name)));
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to get schema information from DuckLake: ");
	}
	// We are only interested if this returns any rows or not
	return result->Fetch() != nullptr;
}

string DuckLakeMetadataManager::GetPathForTable(TableIndex table_id, const vector<DuckLakeTableInfo> &new_tables,
                                                const vector<DuckLakeSchemaInfo> &new_schemas_result) {
	for (const auto &new_table : new_tables) {
		if (new_table.id == table_id) {
			// This is a table not yet in the catalog
			auto result = Query(StringUtil::Format(R"(
SELECT s.path, s.path_is_relative
FROM {METADATA_CATALOG}.ducklake_schema s
WHERE schema_id = %d;)",
			                                       new_table.schema_id.index));
			for (auto &row : *result) {
				DuckLakePath schema_path;
				schema_path.path = row.GetValue<string>(0);
				schema_path.path_is_relative = row.GetValue<bool>(1);
				auto resolved_schema_path = FromRelativePath(schema_path);

				DuckLakePath table_path;
				table_path.path = new_table.path;
				table_path.path_is_relative = false;
				return FromRelativePath(table_path, resolved_schema_path);
			}
			for (auto &schema : new_schemas_result) {
				if (schema.id == new_table.schema_id) {
					DuckLakePath schema_path;
					schema_path.path = schema.path;
					schema_path.path_is_relative = false;
					auto resolved_schema_path = FromRelativePath(schema_path);

					DuckLakePath table_path;
					table_path.path = new_table.path;
					table_path.path_is_relative = false;
					return FromRelativePath(table_path, resolved_schema_path);
				}
			}
		}
	}
	auto result = Query(StringUtil::Format(R"(
SELECT
	s.path AS s_path,
	s.path_is_relative AS s_path_is_relative,
	t.path AS t_path,
	t.path_is_relative AS t_path_is_relative
FROM {METADATA_CATALOG}.ducklake_schema s
JOIN {METADATA_CATALOG}.ducklake_table t
USING (schema_id)
WHERE table_id = %d;)",
	                                       table_id.index));
	for (auto &row : *result) {
		DuckLakePath schema_path;
		schema_path.path = row.GetValue<string>(0);
		schema_path.path_is_relative = row.GetValue<bool>(1);
		auto resolved_schema_path = FromRelativePath(schema_path);

		DuckLakePath table_path;
		table_path.path = row.GetValue<string>(2);
		table_path.path_is_relative = row.GetValue<bool>(3);
		return FromRelativePath(table_path, resolved_schema_path);
	}

	throw InvalidInputException("Failed to get path for table with id %d - table not found in metadata catalog",
	                            table_id.index);
}

string DuckLakeMetadataManager::GetPath(SchemaIndex schema_id, vector<DuckLakeSchemaInfo> &new_schemas_result) {
	lock_guard<mutex> guard(paths_lock);
	// get the path from the list of cached paths
	auto entry = schema_paths.find(schema_id);
	if (entry != schema_paths.end()) {
		return entry->second;
	}
	// get the path from the current snapshot if possible
	// otherwise fetch it from the metadata catalog
	auto &catalog = transaction.GetCatalog();
	auto schema = catalog.GetEntryById(transaction, transaction.GetSnapshot(), schema_id);
	string path;
	if (schema) {
		path = schema->Cast<DuckLakeSchemaEntry>().DataPath();
	} else {
		path = GetPathForSchema(schema_id, new_schemas_result);
	}
	schema_paths.emplace(schema_id, path);
	return path;
}

string DuckLakeMetadataManager::GetPath(TableIndex table_id, const vector<DuckLakeTableInfo> &new_tables,
                                        const vector<DuckLakeSchemaInfo> &new_schemas_result) {
	lock_guard<mutex> guard(paths_lock);
	// get the path from the list of cached paths
	auto entry = table_paths.find(table_id);
	if (entry != table_paths.end()) {
		return entry->second;
	}
	// get the path from the current snapshot if possible
	auto &catalog = transaction.GetCatalog();
	auto table = catalog.GetEntryById(transaction, transaction.GetSnapshot(), table_id);
	string path;
	if (table) {
		path = table->Cast<DuckLakeTableEntry>().DataPath();
	} else {
		path = GetPathForTable(table_id, new_tables, new_schemas_result);
	}
	table_paths.emplace(table_id, path);
	return path;
}

DuckLakePath DuckLakeMetadataManager::GetRelativePath(const string &path) {
	auto &data_path = transaction.GetCatalog().DataPath();
	return GetRelativePath(path, data_path);
}

DuckLakePath DuckLakeMetadataManager::GetRelativePath(SchemaIndex schema_id, const string &path,
                                                      vector<DuckLakeSchemaInfo> &new_schemas_result) {
	return GetRelativePath(path, GetPath(schema_id, new_schemas_result));
}

DuckLakePath DuckLakeMetadataManager::GetRelativePath(TableIndex table_id, const string &path,
                                                      const vector<DuckLakeTableInfo> &new_tables,
                                                      vector<DuckLakeSchemaInfo> &new_schemas_result) {
	return GetRelativePath(path, GetPath(table_id, new_tables, new_schemas_result));
}

DuckLakePath DuckLakeMetadataManager::GetRelativePath(const string &path, const string &data_path) {
	DuckLakePath result;
	if (StringUtil::StartsWith(path, data_path)) {
		result.path = path.substr(data_path.size());
		result.path_is_relative = true;
	} else {
		result.path = path;
		result.path_is_relative = false;
	}
	result.path = StorePath(std::move(result.path));
	return result;
}

string DuckLakeMetadataManager::GetPathSeparator(const string &path) {
	auto &catalog = transaction.GetCatalog();
	if (!catalog.DataPath().empty()) {
		// use the cached separator from the catalog
		return catalog.Separator();
	}
	// if catalog is not loaded, use the file system
	return GetFileSystem().PathSeparator(path);
}

string DuckLakeMetadataManager::StorePath(string path) {
	auto separator = GetPathSeparator(path);
	if (separator == "/") {
		return path;
	}
	return StringUtil::Replace(path, separator, "/");
}

string DuckLakeMetadataManager::LoadPath(string path) {
	auto separator = GetPathSeparator(path);
	if (separator == "/") {
		return path;
	}
	return StringUtil::Replace(path, "/", separator);
}

string DuckLakeMetadataManager::FromRelativePath(const DuckLakePath &path, const string &base_path) {
	if (!path.path_is_relative) {
		return LoadPath(path.path);
	}
	return LoadPath(base_path + path.path);
}

string DuckLakeMetadataManager::FromRelativePath(const DuckLakePath &path) {
	return FromRelativePath(path, transaction.GetCatalog().DataPath());
}

string DuckLakeMetadataManager::FromRelativePath(TableIndex table_id, const DuckLakePath &path) {
	return FromRelativePath(path, GetPath(table_id, {}, {}));
}

string DuckLakeMetadataManager::StorePath(string path, const string &separator) {
	if (separator == "/") {
		return path;
	}
	return StringUtil::Replace(path, separator, "/");
}

string DuckLakeMetadataManager::LoadPath(string path, const string &separator) {
	if (separator == "/") {
		return path;
	}
	return StringUtil::Replace(path, "/", separator);
}

string DuckLakeMetadataManager::FromRelativePath(const DuckLakePath &path, const string &base_path,
                                                 const string &separator) {
	if (!path.path_is_relative) {
		return LoadPath(path.path, separator);
	}
	return LoadPath(base_path + path.path, separator);
}

DuckLakePath DuckLakeMetadataManager::GetRelativePath(const string &path, const string &data_path,
                                                      const string &separator) {
	DuckLakePath result;
	if (StringUtil::StartsWith(path, data_path)) {
		result.path = path.substr(data_path.size());
		result.path_is_relative = true;
	} else {
		result.path = path;
		result.path_is_relative = false;
	}
	result.path = StorePath(std::move(result.path), separator);
	return result;
}

string DuckLakeMetadataManager::GetPathForSchema(SchemaIndex schema_id,
                                                 const vector<DuckLakeSchemaInfo> &new_schemas_result,
                                                 const std::function<unique_ptr<QueryResult>(string)> &query_executor,
                                                 const string &base_data_path, const string &separator) {
	for (auto &schema : new_schemas_result) {
		if (schema_id == schema.id) {
			DuckLakePath path;
			path.path = schema.path;
			path.path_is_relative = false;
			return FromRelativePath(path, base_data_path, separator);
		}
	}
	auto query = StringUtil::Format(R"(
SELECT path, path_is_relative
FROM {METADATA_CATALOG}.ducklake_schema
WHERE schema_id = %d;)",
	                                schema_id.index);
	auto result = query_executor(query);
	for (auto &row : *result) {
		DuckLakePath path;
		path.path = row.GetValue<string>(0);
		path.path_is_relative = row.GetValue<bool>(1);
		return FromRelativePath(path, base_data_path, separator);
	}
	throw InvalidInputException("Failed to get path for schema with id %d - schema not found in metadata catalog",
	                            schema_id.index);
}

string DuckLakeMetadataManager::GetPathForTable(TableIndex table_id, const vector<DuckLakeTableInfo> &new_tables,
                                                const vector<DuckLakeSchemaInfo> &new_schemas_result,
                                                const std::function<unique_ptr<QueryResult>(string)> &query_executor,
                                                const string &base_data_path, const string &separator) {
	for (const auto &new_table : new_tables) {
		if (new_table.id == table_id) {
			// new table - resolve its schema first
			for (auto &schema : new_schemas_result) {
				if (schema.id == new_table.schema_id) {
					DuckLakePath schema_path;
					schema_path.path = schema.path;
					schema_path.path_is_relative = false;
					auto resolved_schema_path = FromRelativePath(schema_path, base_data_path, separator);
					DuckLakePath table_path;
					table_path.path = new_table.path;
					table_path.path_is_relative = false;
					return FromRelativePath(table_path, resolved_schema_path, separator);
				}
			}
			auto schema_query = StringUtil::Format(R"(
SELECT s.path, s.path_is_relative
FROM {METADATA_CATALOG}.ducklake_schema s
WHERE schema_id = %d;)",
			                                       new_table.schema_id.index);
			auto result = query_executor(schema_query);
			for (auto &row : *result) {
				DuckLakePath schema_path;
				schema_path.path = row.GetValue<string>(0);
				schema_path.path_is_relative = row.GetValue<bool>(1);
				auto resolved_schema_path = FromRelativePath(schema_path, base_data_path, separator);
				DuckLakePath table_path;
				table_path.path = new_table.path;
				table_path.path_is_relative = false;
				return FromRelativePath(table_path, resolved_schema_path, separator);
			}
		}
	}
	auto query = StringUtil::Format(R"(
SELECT
	s.path AS s_path,
	s.path_is_relative AS s_path_is_relative,
	t.path AS t_path,
	t.path_is_relative AS t_path_is_relative
FROM {METADATA_CATALOG}.ducklake_schema s
JOIN {METADATA_CATALOG}.ducklake_table t
USING (schema_id)
WHERE table_id = %d;)",
	                                table_id.index);
	auto result = query_executor(query);
	for (auto &row : *result) {
		DuckLakePath schema_path;
		schema_path.path = row.GetValue<string>(0);
		schema_path.path_is_relative = row.GetValue<bool>(1);
		auto resolved_schema_path = FromRelativePath(schema_path, base_data_path, separator);
		DuckLakePath table_path;
		table_path.path = row.GetValue<string>(2);
		table_path.path_is_relative = row.GetValue<bool>(3);
		return FromRelativePath(table_path, resolved_schema_path, separator);
	}
	throw InvalidInputException("Failed to get path for table with id %d - table not found in metadata catalog",
	                            table_id.index);
}

DuckLakePath
DuckLakeMetadataManager::GetRelativePath(SchemaIndex schema_id, const string &path,
                                         const vector<DuckLakeSchemaInfo> &new_schemas_result,
                                         const std::function<unique_ptr<QueryResult>(string)> &query_executor,
                                         const string &base_data_path, const string &separator) {
	return GetRelativePath(
	    path, GetPathForSchema(schema_id, new_schemas_result, query_executor, base_data_path, separator), separator);
}

DuckLakePath
DuckLakeMetadataManager::GetRelativePath(TableIndex table_id, const string &path,
                                         const vector<DuckLakeTableInfo> &new_tables,
                                         const vector<DuckLakeSchemaInfo> &new_schemas_result,
                                         const std::function<unique_ptr<QueryResult>(string)> &query_executor,
                                         const string &base_data_path, const string &separator) {
	return GetRelativePath(
	    path, GetPathForTable(table_id, new_tables, new_schemas_result, query_executor, base_data_path, separator),
	    separator);
}

// Optimized version using DuckDB Appender API for much faster inserts
string DuckLakeMetadataManager::WriteNewDataFilesWithAppender(DuckLakeSnapshot &commit_snapshot,
                                                              const vector<DuckLakeFileInfo> &new_files,
                                                              const vector<DuckLakeTableInfo> &new_tables,
                                                              vector<DuckLakeSchemaInfo> &new_schemas_result) {
	auto &catalog = transaction.GetCatalog();
	auto &connection = transaction.GetConnection();
	const auto &db_name = catalog.MetadataDatabaseName();
	auto schema_name = catalog.MetadataSchemaName();
	if (schema_name.empty()) {
		schema_name = "main";
	}

	// Create appenders for each table
	Appender data_file_appender(connection, Identifier(db_name), Identifier(schema_name), "ducklake_data_file");
	Appender column_stats_appender(connection, Identifier(db_name), Identifier(schema_name),
	                               "ducklake_file_column_stats");
	Appender partition_value_appender(connection, Identifier(db_name), Identifier(schema_name),
	                                  "ducklake_file_partition_value");
	Appender variant_stats_appender(connection, Identifier(db_name), Identifier(schema_name),
	                                "ducklake_file_variant_stats");

	bool write_row_group_count = catalog.SupportsRowGroupCount();
	for (auto &file : new_files) {
		auto data_file_index = static_cast<int64_t>(file.id.index);
		auto table_id = static_cast<int64_t>(file.table_id.index);
		int64_t begin_snapshot_val = file.begin_snapshot.IsValid()
		                                 ? static_cast<int64_t>(file.begin_snapshot.GetIndex())
		                                 : static_cast<int64_t>(commit_snapshot.snapshot_id);
		auto path = GetRelativePath(file.table_id, file.file_name, new_tables, new_schemas_result);

		// ducklake_data_file columns:
		// data_file_id, table_id, begin_snapshot, end_snapshot, file_order, path, path_is_relative,
		// file_format, record_count, file_size_bytes, footer_size, row_id_start, partition_id,
		// encryption_key, mapping_id, partial_max, row_group_count (>= 1.1)
		data_file_appender.BeginRow();
		data_file_appender.Append<int64_t>(data_file_index);                            // data_file_id
		data_file_appender.Append<int64_t>(table_id);                                   // table_id
		data_file_appender.Append<int64_t>(begin_snapshot_val);                         // begin_snapshot
		data_file_appender.Append(Value());                                             // end_snapshot (NULL)
		data_file_appender.Append(Value());                                             // file_order (NULL)
		data_file_appender.Append<string_t>(string_t(path.path));                       // path
		data_file_appender.Append<bool>(path.path_is_relative);                         // path_is_relative
		data_file_appender.Append<string_t>(string_t("parquet"));                       // file_format
		data_file_appender.Append<int64_t>(static_cast<int64_t>(file.row_count));       // record_count
		data_file_appender.Append<int64_t>(static_cast<int64_t>(file.file_size_bytes)); // file_size_bytes
		if (file.footer_size.IsValid()) {
			data_file_appender.Append<int64_t>(static_cast<int64_t>(file.footer_size.GetIndex())); // footer_size
		} else {
			data_file_appender.Append(Value());
		}
		if (file.row_id_start.IsValid()) {
			data_file_appender.Append<int64_t>(static_cast<int64_t>(file.row_id_start.GetIndex())); // row_id_start
		} else {
			data_file_appender.Append(Value());
		}
		if (file.partition_id.IsValid()) {
			data_file_appender.Append<int64_t>(static_cast<int64_t>(file.partition_id.GetIndex())); // partition_id
		} else {
			data_file_appender.Append(Value());
		}
		if (!file.encryption_key.empty()) {
			data_file_appender.Append<string_t>(
			    string_t(Blob::ToBase64(string_t(file.encryption_key)))); // encryption_key
		} else {
			data_file_appender.Append(Value());
		}
		if (file.mapping_id.IsValid()) {
			data_file_appender.Append<int64_t>(static_cast<int64_t>(file.mapping_id.index)); // mapping_id
		} else {
			data_file_appender.Append(Value());
		}
		if (file.max_partial_file_snapshot.IsValid()) {
			data_file_appender.Append<int64_t>(
			    static_cast<int64_t>(file.max_partial_file_snapshot.GetIndex())); // partial_max
		} else {
			data_file_appender.Append(Value());
		}
		if (write_row_group_count) {
			if (file.row_group_count.IsValid()) {
				data_file_appender.Append<int64_t>(static_cast<int64_t>(file.row_group_count.GetIndex()));
			} else {
				data_file_appender.Append(Value());
			}
		}
		if (transaction.GetCatalog().SupportsWritableBranches()) {
			data_file_appender.Append<int64_t>(static_cast<int64_t>(commit_snapshot.branch_id)); // branch_id
		}
		data_file_appender.EndRow();

		// Column stats - using typed values directly
		for (auto &column_stats_entry : file.column_stats) {
			auto column_id = static_cast<int64_t>(column_stats_entry.first.index);
			auto &stats = column_stats_entry.second;

			// ducklake_file_column_stats columns:
			// data_file_id, table_id, column_id, column_size_bytes, value_count, null_count,
			// min_value, max_value, contains_nan, extra_stats
			column_stats_appender.BeginRow();
			column_stats_appender.Append<int64_t>(data_file_index);
			column_stats_appender.Append<int64_t>(table_id);
			column_stats_appender.Append<int64_t>(column_id);
			column_stats_appender.Append<int64_t>(static_cast<int64_t>(stats.column_size_bytes));

			// value_count and null_count
			if (stats.has_null_count && stats.has_num_values && stats.null_count <= stats.num_values) {
				column_stats_appender.Append<int64_t>(static_cast<int64_t>(stats.num_values - stats.null_count));
				column_stats_appender.Append<int64_t>(static_cast<int64_t>(stats.null_count));
			} else {
				column_stats_appender.Append(Value());
				column_stats_appender.Append(Value());
			}

			// min_value and max_value
			if (stats.has_min) {
				column_stats_appender.Append<string_t>(string_t(stats.min));
			} else {
				column_stats_appender.Append(Value());
			}
			if (stats.has_max) {
				column_stats_appender.Append<string_t>(string_t(stats.max));
			} else {
				column_stats_appender.Append(Value());
			}

			// contains_nan
			if (stats.has_contains_nan) {
				column_stats_appender.Append<bool>(stats.contains_nan);
			} else {
				column_stats_appender.Append(Value());
			}

			// extra_stats
			string extra_stats_str;
			if (stats.extra_stats && stats.extra_stats->TrySerialize(extra_stats_str)) {
				// TrySerialize wraps the JSON in single quotes for SQL - strip them for Appender
				if (extra_stats_str.size() >= 2 && extra_stats_str.front() == '\'' && extra_stats_str.back() == '\'') {
					extra_stats_str = extra_stats_str.substr(1, extra_stats_str.size() - 2);
				}
				column_stats_appender.Append<string_t>(string_t(extra_stats_str));
			} else {
				column_stats_appender.Append(Value());
			}
			column_stats_appender.EndRow();

			// Variant stats from extra_stats
			if (stats.extra_stats && stats.extra_stats->GetStatsType() == DuckLakeExtraStatsType::VARIANT) {
				auto &variant_extra = static_cast<DuckLakeColumnVariantStats &>(*stats.extra_stats);
				for (auto &variant_entry : variant_extra.shredded_field_stats) {
					auto &field_stats = variant_entry.second.field_stats;

					// ducklake_file_variant_stats columns:
					// data_file_id, table_id, column_id, variant_path, shredded_type, column_size_bytes,
					// value_count, null_count, min_value, max_value, contains_nan, extra_stats
					variant_stats_appender.BeginRow();
					variant_stats_appender.Append<int64_t>(data_file_index);
					variant_stats_appender.Append<int64_t>(table_id);
					variant_stats_appender.Append<int64_t>(column_id);
					variant_stats_appender.Append<string_t>(string_t(variant_entry.first));
					variant_stats_appender.Append<string_t>(
					    string_t(DuckLakeTypes::ToString(variant_entry.second.shredded_type)));
					variant_stats_appender.Append<int64_t>(static_cast<int64_t>(field_stats.column_size_bytes));

					if (field_stats.has_null_count && field_stats.has_num_values &&
					    field_stats.null_count <= field_stats.num_values) {
						variant_stats_appender.Append<int64_t>(
						    static_cast<int64_t>(field_stats.num_values - field_stats.null_count));
						variant_stats_appender.Append<int64_t>(static_cast<int64_t>(field_stats.null_count));
					} else {
						variant_stats_appender.Append(Value());
						variant_stats_appender.Append(Value());
					}

					if (field_stats.has_min) {
						variant_stats_appender.Append<string_t>(string_t(field_stats.min));
					} else {
						variant_stats_appender.Append(Value());
					}
					if (field_stats.has_max) {
						variant_stats_appender.Append<string_t>(string_t(field_stats.max));
					} else {
						variant_stats_appender.Append(Value());
					}

					if (field_stats.has_contains_nan) {
						variant_stats_appender.Append<bool>(field_stats.contains_nan);
					} else {
						variant_stats_appender.Append(Value());
					}

					string field_extra_stats_str;
					if (field_stats.extra_stats && field_stats.extra_stats->TrySerialize(field_extra_stats_str)) {
						// TrySerialize wraps the JSON in single quotes for SQL - strip them for Appender
						if (field_extra_stats_str.size() >= 2 && field_extra_stats_str.front() == '\'' &&
						    field_extra_stats_str.back() == '\'') {
							field_extra_stats_str = field_extra_stats_str.substr(1, field_extra_stats_str.size() - 2);
						}
						variant_stats_appender.Append<string_t>(string_t(field_extra_stats_str));
					} else {
						variant_stats_appender.Append(Value());
					}
					variant_stats_appender.EndRow();
				}
			}
		}

		// Partition values
		if (file.partition_id.IsValid() == file.partition_values.empty()) {
			throw InternalException("File should either not be partitioned, or have partition values");
		}
		for (auto &part_val : file.partition_values) {
			// ducklake_file_partition_value columns:
			// data_file_id, table_id, partition_key_index, partition_value
			partition_value_appender.BeginRow();
			partition_value_appender.Append<int64_t>(data_file_index);
			partition_value_appender.Append<int64_t>(table_id);
			partition_value_appender.Append<int64_t>(static_cast<int64_t>(part_val.partition_column_idx));
			if (part_val.partition_value.IsNull()) {
				partition_value_appender.Append(Value());
			} else {
				partition_value_appender.Append(part_val.partition_value);
			}
			partition_value_appender.EndRow();
		}
	}

	// Explicitly close appenders
	data_file_appender.Close();
	column_stats_appender.Close();
	partition_value_appender.Close();
	variant_stats_appender.Close();

	return "";
}

bool DuckLakeMetadataManager::TryAppendDataFiles(DuckLakeSnapshot &commit_snapshot,
                                                 const vector<DuckLakeFileInfo> &new_files,
                                                 const vector<DuckLakeTableInfo> &new_tables,
                                                 vector<DuckLakeSchemaInfo> &new_schemas_result) {
	if (!SupportsAppender() || new_files.empty()) {
		return false;
	}
	WriteNewDataFilesWithAppender(commit_snapshot, new_files, new_tables, new_schemas_result);
	return true;
}

string DuckLakeMetadataManager::WriteNewDataFiles(DuckLakeSnapshot &commit_snapshot,
                                                  const vector<DuckLakeFileInfo> &new_files,
                                                  const vector<DuckLakeTableInfo> &new_tables,
                                                  vector<DuckLakeSchemaInfo> &new_schemas_result) {
	if (new_files.empty()) {
		return string();
	}
	// Use optimized appender path for DuckDB metadata (much faster for large inserts)
	if (SupportsAppender()) {
		return WriteNewDataFilesWithAppender(commit_snapshot, new_files, new_tables, new_schemas_result);
	}
	vector<DuckLakePath> resolved_paths;
	resolved_paths.reserve(new_files.size());
	for (auto &file : new_files) {
		resolved_paths.push_back(GetRelativePath(file.table_id, file.file_name, new_tables, new_schemas_result));
	}
	return WriteNewDataFilesSqlBatch(new_files, resolved_paths, transaction.GetCatalog().SupportsRowGroupCount());
}

string DuckLakeMetadataManager::WriteNewDataFilesSqlBatch(const vector<DuckLakeFileInfo> &new_files,
                                                          const vector<DuckLakePath> &resolved_paths,
                                                          bool write_row_group_count) {
	if (new_files.empty()) {
		return string();
	}
	D_ASSERT(new_files.size() == resolved_paths.size());
	string data_file_insert_query;
	string column_stats_insert_query;
	string variant_stats_insert_query;
	string partition_insert_query;

	for (idx_t i = 0; i < new_files.size(); i++) {
		auto &file = new_files[i];
		auto &path = resolved_paths[i];
		if (!data_file_insert_query.empty()) {
			data_file_insert_query += ",";
		}
		auto row_id = DuckLakeUtil::OptionalIdxOrNull(file.row_id_start);
		auto partition_id = DuckLakeUtil::OptionalIdxOrNull(file.partition_id);
		auto begin_snapshot =
		    file.begin_snapshot.IsValid() ? to_string(file.begin_snapshot.GetIndex()) : "{SNAPSHOT_ID}";
		auto data_file_index = file.id.index;
		auto table_id = file.table_id.index;
		auto encryption_key = DuckLakeUtil::EncryptionKeyLiteral(file.encryption_key);
		string partial_max = DuckLakeUtil::OptionalIdxOrNull(file.max_partial_file_snapshot);
		string footer_size = DuckLakeUtil::OptionalIdxOrNull(file.footer_size);
		string mapping = DuckLakeUtil::MappingIdOrNull(file.mapping_id);
		data_file_insert_query += StringUtil::Format(
		    "(%d, %d, %s, NULL, NULL, %s, %s, 'parquet', %d, %d, %s, %s, %s, %s, %s, %s", data_file_index, table_id,
		    begin_snapshot, SQLString(path.path), path.path_is_relative ? "true" : "false", file.row_count,
		    file.file_size_bytes, footer_size, row_id, partition_id, encryption_key, mapping, partial_max);
		if (write_row_group_count) {
			data_file_insert_query += ", " + DuckLakeUtil::OptionalIdxOrNull(file.row_group_count);
		}
		data_file_insert_query += "{BRANCH_ID_VAL})";
		for (auto &raw_stats : file.column_stats) {
			auto column_stats = DuckLakeColumnStatsInfo::FromColumnStats(raw_stats.first, raw_stats.second);
			if (!column_stats_insert_query.empty()) {
				column_stats_insert_query += ",";
			}
			auto column_id = column_stats.column_id.index;
			column_stats_insert_query += StringUtil::Format(
			    "(%d, %d, %d, %s, %s, %s, %s, %s, %s, %s)", data_file_index, table_id, column_id,
			    column_stats.column_size_bytes, column_stats.value_count, column_stats.null_count, column_stats.min_val,
			    column_stats.max_val, column_stats.contains_nan, column_stats.extra_stats);
			for (auto &variant_stats : column_stats.variant_stats) {
				if (!variant_stats_insert_query.empty()) {
					variant_stats_insert_query += ",";
				}
				auto &field_stats = variant_stats.field_stats;
				variant_stats_insert_query += StringUtil::Format(
				    "(%d, %d, %d, %s, %s, %s, %s, %s, %s, %s, %s, %s)", data_file_index, table_id, column_id,
				    SQLString(variant_stats.field_name), SQLString(variant_stats.shredded_type),
				    field_stats.column_size_bytes, field_stats.value_count, field_stats.null_count, field_stats.min_val,
				    field_stats.max_val, field_stats.contains_nan, field_stats.extra_stats);
			}
		}
		if (file.partition_id.IsValid() == file.partition_values.empty()) {
			throw InternalException("File should either not be partitioned, or have partition values");
		}
		for (auto &part_val : file.partition_values) {
			if (!partition_insert_query.empty()) {
				partition_insert_query += ",";
			}
			string partition_val;
			if (part_val.partition_value.IsNull()) {
				partition_val = "NULL";
			} else {
				partition_val = StringUtil::Format("%s", SQLString(part_val.partition_value.ToString()));
			}
			partition_insert_query += StringUtil::Format("(%d, %d, %d, %s)", data_file_index, table_id,
			                                             part_val.partition_column_idx, partition_val);
		}
	}
	if (data_file_insert_query.empty()) {
		throw InternalException("No files found!?");
	}

	// insert the data files
	string batch_query;
	batch_query +=
	    StringUtil::Format("INSERT INTO {METADATA_CATALOG}.ducklake_data_file(data_file_id, table_id, begin_snapshot, "
	                       "end_snapshot, file_order, path, path_is_relative, file_format, record_count, "
	                       "file_size_bytes, footer_size, row_id_start, partition_id, encryption_key, mapping_id, "
	                       "partial_max%s{BRANCH_ID_COL}) VALUES %s;",
	                       write_row_group_count ? ", row_group_count" : "", data_file_insert_query);

	// insert the column stats
	batch_query += StringUtil::Format("INSERT INTO {METADATA_CATALOG}.ducklake_file_column_stats VALUES %s;",
	                                  column_stats_insert_query);
	if (!partition_insert_query.empty()) {
		// insert the partition values
		batch_query += StringUtil::Format("INSERT INTO {METADATA_CATALOG}.ducklake_file_partition_value VALUES %s;",
		                                  partition_insert_query);
	}
	if (!variant_stats_insert_query.empty()) {
		batch_query += StringUtil::Format("INSERT INTO {METADATA_CATALOG}.ducklake_file_variant_stats VALUES %s;",
		                                  variant_stats_insert_query);
	}
	return batch_query;
}

string DuckLakeMetadataManager::DropDataFiles(const set<DataFileIndex> &dropped_files) {
	return FlushDrop("ducklake_data_file", "data_file_id", dropped_files, true);
}

string DuckLakeMetadataManager::DropDeleteFiles(const set<DataFileIndex> &dropped_files) {
	return FlushDrop("ducklake_delete_file", "data_file_id", dropped_files, true);
}

string
DuckLakeMetadataManager::DeleteOverwrittenDeleteFiles(const vector<DuckLakeOverwrittenDeleteFile> &overwritten_files,
                                                      const vector<DuckLakePath> &resolved_paths) {
	if (overwritten_files.empty()) {
		return {};
	}
	if (resolved_paths.size() != overwritten_files.size()) {
		throw InternalException("DeleteOverwrittenDeleteFiles: resolved_paths size mismatch");
	}
	string deleted_file_ids;
	string scheduled_deletions;
	for (idx_t i = 0; i < overwritten_files.size(); ++i) {
		auto &file = overwritten_files[i];
		auto &path = resolved_paths[i];
		if (!deleted_file_ids.empty()) {
			deleted_file_ids += ", ";
		}
		deleted_file_ids += to_string(file.delete_file_id.index);

		if (!scheduled_deletions.empty()) {
			scheduled_deletions += ", ";
		}
		scheduled_deletions += StringUtil::Format("(%d, %s, %s, NOW())", file.delete_file_id.index,
		                                          SQLString(path.path), path.path_is_relative ? "true" : "false");
	}

	string batch_query;
	// delete the old delete file metadata records
	batch_query += StringUtil::Format(R"(
DELETE FROM {METADATA_CATALOG}.ducklake_delete_file
WHERE delete_file_id IN (%s);
)",
	                                  deleted_file_ids);
	// schedule the old files for disk deletion
	batch_query +=
	    "INSERT INTO {METADATA_CATALOG}.ducklake_files_scheduled_for_deletion VALUES " + scheduled_deletions + ";";
	return batch_query;
}

string DuckLakeMetadataManager::WriteNewDeleteFiles(const vector<DuckLakeDeleteFileInfo> &new_files,
                                                    const vector<DuckLakePath> &resolved_paths,
                                                    bool write_row_group_count) {
	if (new_files.empty()) {
		return {};
	}
	if (resolved_paths.size() != new_files.size()) {
		throw InternalException("WriteNewDeleteFiles: resolved_paths size mismatch");
	}
	string delete_file_insert_query;
	string delete_file_ids;
	for (idx_t i = 0; i < new_files.size(); ++i) {
		auto &file = new_files[i];
		auto &path = resolved_paths[i];
		if (!delete_file_insert_query.empty()) {
			delete_file_insert_query += ",";
			delete_file_ids += ", ";
		}
		auto delete_file_index = file.id.index;
		auto table_id = file.table_id.index;
		auto data_file_index = file.data_file_id.index;
		auto encryption_key = DuckLakeUtil::EncryptionKeyLiteral(file.encryption_key);
		// Use explicit begin_snapshot if set (for flush operations), otherwise use commit snapshot
		string begin_snapshot_str =
		    file.begin_snapshot.IsValid() ? std::to_string(file.begin_snapshot.GetIndex()) : "{SNAPSHOT_ID}";
		string partial_max = DuckLakeUtil::OptionalIdxOrNull(file.max_snapshot);
		delete_file_insert_query += StringUtil::Format(
		    "(%d, %d, %s, NULL,  %d, %s, %s, %s, %d, %d, %d, %s, %s", delete_file_index, table_id, begin_snapshot_str,
		    data_file_index, SQLString(path.path), path.path_is_relative ? "true" : "false",
		    SQLString(DeleteFileFormatToString(file.format)), file.delete_count, file.file_size_bytes, file.footer_size,
		    encryption_key, partial_max);
		if (write_row_group_count) {
			delete_file_insert_query += ", " + DuckLakeUtil::OptionalIdxOrNull(file.row_group_count);
		}
		delete_file_insert_query += "{BRANCH_ID_VAL})";
		delete_file_ids += to_string(delete_file_index);
	}

	// insert the data files
	string batch = StringUtil::Format(
	    "INSERT INTO {METADATA_CATALOG}.ducklake_delete_file(delete_file_id, table_id, begin_snapshot, end_snapshot, "
	    "data_file_id, path, path_is_relative, format, delete_count, file_size_bytes, footer_size, encryption_key, "
	    "partial_max%s{BRANCH_ID_COL}) VALUES %s;",
	    write_row_group_count ? ", row_group_count" : "", delete_file_insert_query);
	// Wire data_file_branch_id from the targeted data file's owning branch (H1).
	// Stripped entirely on pre-writable-branch catalogs (column does not exist).
	batch += StringUtil::Format(R"(
{WRITABLE_ONLY_START}
UPDATE {METADATA_CATALOG}.ducklake_delete_file AS del
SET data_file_branch_id = (
	SELECT COALESCE(df.branch_id, del.branch_id, 0)
	FROM {METADATA_CATALOG}.ducklake_data_file df
	WHERE df.data_file_id = del.data_file_id
	ORDER BY df.begin_snapshot DESC
	LIMIT 1
)
WHERE del.delete_file_id IN (%s);
{WRITABLE_ONLY_END}
)",
	                            delete_file_ids);
	return batch;
}

vector<DuckLakeColumnMappingInfo> DuckLakeMetadataManager::GetColumnMappings(optional_idx start_from) {
	string filter;
	if (start_from.IsValid()) {
		filter = "WHERE mapping_id >= " + to_string(start_from.GetIndex());
	}
	auto result = Query(StringUtil::Format(R"(
SELECT mapping_id, table_id, type, column_id, source_name, target_field_id, parent_column, is_partition
FROM {METADATA_CATALOG}.ducklake_column_mapping
JOIN {METADATA_CATALOG}.ducklake_name_mapping USING (mapping_id)
%s
ORDER BY mapping_id, parent_column NULLS FIRST
)",
	                                       filter));
	vector<DuckLakeColumnMappingInfo> column_maps;
	for (auto &row : *result) {
		MappingIndex mapping_id(row.GetValue<idx_t>(0));
		if (column_maps.empty() || column_maps.back().mapping_id != mapping_id) {
			DuckLakeColumnMappingInfo mapping_info;
			mapping_info.mapping_id = mapping_id;
			mapping_info.table_id = TableIndex(row.GetValue<idx_t>(1));
			mapping_info.map_type = row.GetValue<string>(2);
			column_maps.push_back(std::move(mapping_info));
		}
		auto &mapping_info = column_maps.back();
		DuckLakeNameMapColumnInfo name_map_column;
		name_map_column.column_id = row.GetValue<idx_t>(3);
		name_map_column.source_name = row.GetValue<string>(4);
		name_map_column.target_field_id = FieldIndex(row.GetValue<idx_t>(5));
		if (!row.IsNull(6)) {
			name_map_column.parent_column = row.GetValue<idx_t>(6);
		}
		name_map_column.hive_partition = row.GetValue<bool>(7);
		mapping_info.map_columns.push_back(std::move(name_map_column));
	}
	return column_maps;
}

string DuckLakeMetadataManager::WriteNewColumnMappings(const vector<DuckLakeColumnMappingInfo> &new_column_mappings) {
	string column_mapping_insert_query;
	string name_map_insert_query;
	for (auto &column_mapping : new_column_mappings) {
		if (!column_mapping_insert_query.empty()) {
			column_mapping_insert_query += ", ";
		}
		column_mapping_insert_query +=
		    StringUtil::Format("(%d, %d, %s)", column_mapping.mapping_id.index, column_mapping.table_id.index,
		                       SQLString(column_mapping.map_type));
		for (auto &name_map_column : column_mapping.map_columns) {
			if (!name_map_insert_query.empty()) {
				name_map_insert_query += ", ";
			}
			string parent_column = DuckLakeUtil::OptionalIdxOrNull(name_map_column.parent_column);
			string is_partition = name_map_column.hive_partition ? "true" : "false";
			name_map_insert_query +=
			    StringUtil::Format("(%d, %d, %s, %d, %s, %s)", column_mapping.mapping_id.index,
			                       name_map_column.column_id, SQLString(name_map_column.source_name),
			                       name_map_column.target_field_id.index, parent_column, is_partition);
		}
	}
	string batch_query;
	batch_query += "INSERT INTO {METADATA_CATALOG}.ducklake_column_mapping VALUES " + column_mapping_insert_query + ";";
	batch_query += "INSERT INTO {METADATA_CATALOG}.ducklake_name_mapping VALUES " + name_map_insert_query + ";";
	return batch_query;
}

string DuckLakeMetadataManager::InsertSnapshotSql(bool with_branch_id) {
	if (with_branch_id) {
		return R"(INSERT INTO {METADATA_CATALOG}.ducklake_snapshot VALUES ({SNAPSHOT_ID}, NOW(), {SCHEMA_VERSION}, {NEXT_CATALOG_ID}, {NEXT_FILE_ID}, {BRANCH_ID});)";
	}
	return R"(INSERT INTO {METADATA_CATALOG}.ducklake_snapshot VALUES ({SNAPSHOT_ID}, NOW(), {SCHEMA_VERSION}, {NEXT_CATALOG_ID}, {NEXT_FILE_ID});)";
}

static string SQLStringOrNull(const string &str) {
	if (str.empty()) {
		return "NULL";
	}
	return SQLString::ToString(str);
}

string DuckLakeMetadataManager::WriteSnapshotChangesSql(const SnapshotChangeInfo &change_info,
                                                        const DuckLakeSnapshotCommit &commit_info) {
	return StringUtil::Format(
	    R"(INSERT INTO {METADATA_CATALOG}.ducklake_snapshot_changes VALUES ({SNAPSHOT_ID}, %s, %s, %s, %s);)",
	    SQLStringOrNull(change_info.changes_made), commit_info.author.ToSQLString(),
	    commit_info.commit_message.ToSQLString(), commit_info.commit_extra_info.ToSQLString());
}

string DuckLakeMetadataManager::GetSnapshotAndStatsAndChangesQuery(bool filter_by_branch) {
	string changes_filter = "c.snapshot_id > {SNAPSHOT_ID}";
	string stats_filter;
	if (filter_by_branch) {
		changes_filter +=
		    " AND c.snapshot_id IN (SELECT snapshot_id FROM {METADATA_CATALOG}.ducklake_snapshot WHERE "
		    "branch_id = {BRANCH_ID})";
		stats_filter = " AND ducklake_table_stats.branch_id = {BRANCH_ID}";
	}
	return StringUtil::Format(R"(
SELECT
    snapshot_id,
    schema_version,
    next_catalog_id,
    next_file_id,
    COALESCE((
            SELECT STRING_AGG(changes_made, ',')
            FROM {METADATA_CATALOG}.ducklake_snapshot_changes c
            WHERE %s
            ),'') AS changes,
    NULL AS table_id,
    NULL AS column_id,
    NULL AS record_count,
    NULL AS next_row_id,
    NULL AS file_size_bytes,
    NULL AS contains_null,
    NULL AS contains_nan,
    NULL AS min_value,
    NULL AS max_value,
    NULL AS extra_stats
    FROM {METADATA_CATALOG}.ducklake_snapshot
    WHERE snapshot_id = (
        SELECT MAX(snapshot_id)
        FROM {METADATA_CATALOG}.ducklake_snapshot)
UNION ALL
SELECT
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    table_id,
    column_id,
    record_count,
    next_row_id,
    file_size_bytes,
    contains_null,
    contains_nan,
    min_value,
    max_value,
    extra_stats
FROM {METADATA_CATALOG}.ducklake_table_stats
LEFT JOIN {METADATA_CATALOG}.ducklake_table_column_stats
    USING (table_id%s)
WHERE record_count IS NOT NULL
    AND file_size_bytes IS NOT NULL
    %s
ORDER BY table_id NULLS FIRST;
	)",
	                          changes_filter, filter_by_branch ? ", branch_id" : "", stats_filter);
}

SnapshotChangeInfo DuckLakeMetadataManager::ParseSnapshotAndStatsAndChanges(QueryResult &result,
                                                                            SnapshotAndStats &current_snapshot) {
	SnapshotChangeInfo change_info;
	bool first_row = true;
	for (auto &row : result) {
		if (first_row) {
			current_snapshot.snapshot.snapshot_id = row.GetValue<idx_t>(0);
			current_snapshot.snapshot.schema_version = row.GetValue<idx_t>(1);
			current_snapshot.snapshot.next_catalog_id = row.GetValue<idx_t>(2);
			current_snapshot.snapshot.next_file_id = row.GetValue<idx_t>(3);
			change_info.changes_made = row.GetValue<string>(4);
		} else {
			TransformGlobalStatsRow(row, current_snapshot.stats, 5);
		}
		first_row = false;
	}
	return change_info;
}

SnapshotChangeInfo
DuckLakeMetadataManager::GetSnapshotAndStatsAndChanges(SnapshotAndStats &current_snapshot,
                                                       const std::function<unique_ptr<QueryResult>(string)> &executor,
                                                       bool filter_by_branch) {
	auto result = executor(GetSnapshotAndStatsAndChangesQuery(filter_by_branch));
	return ParseSnapshotAndStatsAndChanges(*result, current_snapshot);
}

unique_ptr<DuckLakeSnapshot> DuckLakeMetadataManager::ParseSnapshot(QueryResult &result) {
	unique_ptr<DuckLakeSnapshot> snapshot;
	for (auto &row : result) {
		if (snapshot) {
			throw InvalidInputException("Corrupt DuckLake - multiple snapshots returned from database");
		}
		auto snapshot_id = row.GetValue<idx_t>(0);
		auto schema_version = row.GetValue<idx_t>(1);
		auto next_catalog_id = row.GetValue<idx_t>(2);
		auto next_file_id = row.GetValue<idx_t>(3);
		snapshot = make_uniq<DuckLakeSnapshot>(snapshot_id, schema_version, next_catalog_id, next_file_id);
	}
	return snapshot;
}

string DuckLakeMetadataManager::LatestSnapshotQuery() {
	return R"(SELECT snapshot_id, schema_version, next_catalog_id, next_file_id FROM {METADATA_CATALOG}.ducklake_snapshot WHERE snapshot_id = (SELECT MAX(snapshot_id) FROM {METADATA_CATALOG}.ducklake_snapshot);)";
}

string DuckLakeMetadataManager::MainBranchSnapshotQuery() {
	return R"(SELECT snapshot_id, schema_version, next_catalog_id, next_file_id
FROM {METADATA_CATALOG}.ducklake_snapshot
WHERE snapshot_id = (
	SELECT snapshot_id FROM {METADATA_CATALOG}.ducklake_ref
	WHERE lower(ref_name) = 'main' AND ref_type = 'branch' AND status = 'active'
);)";
}

string DuckLakeMetadataManager::GetLatestSnapshotQuery() const {
	if (transaction.GetCatalog().SupportsWritableBranches()) {
		return MainBranchSnapshotQuery();
	}
	return LatestSnapshotQuery();
}

unique_ptr<DuckLakeSnapshot> DuckLakeMetadataManager::GetSnapshot() {
	unique_ptr<QueryResult> result;
	if (transaction.HasActiveBranch() && transaction.GetCatalog().SupportsWritableBranches()) {
		// Read at the active branch head (may lag the global MAX while other branches advance).
		result = Query(StringUtil::Format(
		    R"(SELECT snapshot_id, schema_version, next_catalog_id, next_file_id
FROM {METADATA_CATALOG}.ducklake_snapshot
WHERE snapshot_id = %llu;)",
		    transaction.GetActiveBranchHeadSnapshot()));
	} else {
		result = Query(GetLatestSnapshotQuery());
	}
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to query most recent snapshot for DuckLake: ");
	}
	auto snapshot = ParseSnapshot(*result);
	if (!snapshot) {
		throw InvalidInputException("No snapshot found in DuckLake");
	}
	snapshot->branch_id = transaction.GetActiveBranchId();
	return snapshot;
}

unique_ptr<DuckLakeSnapshot> DuckLakeMetadataManager::GetSnapshot(BoundAtClause &at_clause, SnapshotBound bound) {
	auto &unit = at_clause.Unit();
	auto &val = at_clause.GetValue();
	unique_ptr<QueryResult> result;
	const string timestamp_order = bound == SnapshotBound::LOWER_BOUND ? "ASC" : "DESC";
	const string timestamp_condition = bound == SnapshotBound::LOWER_BOUND ? ">" : "<";
	if (StringUtil::CIEquals(unit, "version")) {
		if (transaction.GetCatalog().SupportsWritableBranches()) {
			result = Query(StringUtil::Format(R"(
SELECT snapshot_id, schema_version, next_catalog_id, next_file_id, COALESCE(branch_id, 0)
FROM {METADATA_CATALOG}.ducklake_snapshot
WHERE snapshot_id = %llu;)",
			                                  val.DefaultCastAs(LogicalType::UBIGINT).GetValue<idx_t>()));
		} else {
			result = Query(StringUtil::Format(R"(
SELECT snapshot_id, schema_version, next_catalog_id, next_file_id
FROM {METADATA_CATALOG}.ducklake_snapshot
WHERE snapshot_id = %llu;)",
			                                  val.DefaultCastAs(LogicalType::UBIGINT).GetValue<idx_t>()));
		}
	} else if (StringUtil::CIEquals(unit, "timestamp")) {
		if (transaction.GetCatalog().SupportsWritableBranches()) {
			result = Query(StringUtil::Format(
			    R"(
SELECT snapshot_id, schema_version, next_catalog_id, next_file_id, COALESCE(branch_id, 0)
FROM {METADATA_CATALOG}.ducklake_snapshot
WHERE snapshot_id = (
	SELECT snapshot_id
	FROM {METADATA_CATALOG}.ducklake_snapshot
	WHERE snapshot_time::TIMESTAMPTZ %s= %s
	ORDER BY snapshot_time::TIMESTAMPTZ %s
	LIMIT 1);)",
			    timestamp_condition, val.DefaultCastAs(LogicalType::VARCHAR).ToSQLString(), timestamp_order));
		} else {
			result = Query(StringUtil::Format(
			    R"(
SELECT snapshot_id, schema_version, next_catalog_id, next_file_id
FROM {METADATA_CATALOG}.ducklake_snapshot
WHERE snapshot_id = (
	SELECT snapshot_id
	FROM {METADATA_CATALOG}.ducklake_snapshot
	WHERE snapshot_time::TIMESTAMPTZ %s= %s
	ORDER BY snapshot_time::TIMESTAMPTZ %s
	LIMIT 1);)",
			    timestamp_condition, val.DefaultCastAs(LogicalType::VARCHAR).ToSQLString(), timestamp_order));
		}
	} else if (StringUtil::CIEquals(unit, "branch") || StringUtil::CIEquals(unit, "tag")) {
		// Resolve named ref → snapshot_id. For writable branches, also load branch_id for lineage reads.
		auto ref_type = StringUtil::Lower(unit);
		if (transaction.GetCatalog().SupportsWritableBranches() && StringUtil::CIEquals(unit, "branch")) {
			result = Query(StringUtil::Format(
			    R"(
SELECT s.snapshot_id, s.schema_version, s.next_catalog_id, s.next_file_id, r.ref_id
FROM {METADATA_CATALOG}.ducklake_ref r
JOIN {METADATA_CATALOG}.ducklake_snapshot s ON s.snapshot_id = r.snapshot_id
WHERE lower(r.ref_name) = lower(%s) AND r.ref_type = %s AND r.status = 'active';)",
			    val.DefaultCastAs(LogicalType::VARCHAR).ToSQLString(), SQLString(ref_type)));
		} else {
			result = Query(StringUtil::Format(
			    R"(
SELECT s.snapshot_id, s.schema_version, s.next_catalog_id, s.next_file_id
FROM {METADATA_CATALOG}.ducklake_ref r
JOIN {METADATA_CATALOG}.ducklake_snapshot s ON s.snapshot_id = r.snapshot_id
WHERE lower(r.ref_name) = lower(%s) AND r.ref_type = %s AND r.status = 'active';)",
			    val.DefaultCastAs(LogicalType::VARCHAR).ToSQLString(), SQLString(ref_type)));
		}
	} else {
		throw InvalidInputException("Unsupported AT clause unit - %s", unit);
	}
	if (result->HasError()) {
		result->GetErrorObject().Throw(StringUtil::Format(
		    "Failed to query snapshot at %s %s for DuckLake: ", StringUtil::Lower(unit), val.ToString()));
	}
	unique_ptr<DuckLakeSnapshot> snapshot;
	for (auto &row : *result) {
		if (snapshot) {
			throw InvalidInputException("Corrupt DuckLake - multiple snapshots returned from database");
		}
		snapshot = make_uniq<DuckLakeSnapshot>(row.GetValue<idx_t>(0), row.GetValue<idx_t>(1), row.GetValue<idx_t>(2),
		                                       row.GetValue<idx_t>(3));
		if (transaction.GetCatalog().SupportsWritableBranches() && result->ColumnCount() > 4 && !row.IsNull(4)) {
			snapshot->branch_id = row.GetValue<idx_t>(4);
		}
	}
	if (!snapshot) {
		throw InvalidInputException("No snapshot found at %s %s", StringUtil::Lower(unit), val.ToString());
	}
	return snapshot;
}

static unordered_map<idx_t, DuckLakePartitionInfo>
GetNewPartitions(const vector<DuckLakePartitionInfo> &old_partitions,
                 const vector<DuckLakePartitionInfo> &new_partitions) {
	unordered_map<idx_t, DuckLakePartitionInfo> new_partition_map;

	for (auto &partition : new_partitions) {
		new_partition_map[partition.table_id.index] = partition;
	}

	unordered_set<idx_t> old_partition_set;
	for (auto &partition : old_partitions) {
		old_partition_set.insert(partition.table_id.index);
		if (new_partition_map.find(partition.table_id.index) != new_partition_map.end()) {
			if (new_partition_map[partition.table_id.index] == partition) {
				// If a new partition already exists in an old partition, it's a nop, we can remove it
				new_partition_map.erase(partition.table_id.index);
			}
		}
	}

	vector<idx_t> partition_ids_to_erase;
	for (auto &partition : new_partitions) {
		if (old_partition_set.find(partition.table_id.index) == old_partition_set.end() && partition.fields.empty()) {
			// If a map does not exist on the old partition and the partition has no fields, this is an reset over
			// and empty partition definition, hence also a nop
			partition_ids_to_erase.push_back(partition.table_id.index);
		}
	}
	for (auto &id : partition_ids_to_erase) {
		new_partition_map.erase(id);
	}
	return new_partition_map;
}

string DuckLakeMetadataManager::WriteNewPartitionKeys(const vector<DuckLakePartitionInfo> &existing_partitions,
                                                      const vector<DuckLakePartitionInfo> &new_partitions) {
	if (new_partitions.empty()) {
		return {};
	}

	string old_partition_table_ids;
	string new_partition_values;
	string insert_partition_cols;

	auto new_partition_map = GetNewPartitions(existing_partitions, new_partitions);
	if (new_partition_map.empty()) {
		return {};
	}
	for (auto &new_partition : new_partition_map) {
		// set old partition data as no longer valid
		if (!old_partition_table_ids.empty()) {
			old_partition_table_ids += ", ";
		}
		old_partition_table_ids += to_string(new_partition.second.table_id.index);
		if (!new_partition.second.id.IsValid()) {
			// dropping partition data - we don't need to do anything
			return {};
		}
		auto partition_id = new_partition.second.id.GetIndex();
		if (!new_partition_values.empty()) {
			new_partition_values += ", ";
		}
		new_partition_values +=
		    StringUtil::Format(R"((%d, %d, {SNAPSHOT_ID}, NULL))", partition_id, new_partition.second.table_id.index);
		for (auto &field : new_partition.second.fields) {
			if (!insert_partition_cols.empty()) {
				insert_partition_cols += ", ";
			}
			insert_partition_cols +=
			    StringUtil::Format("(%d, %d, %d, %d, %s)", partition_id, new_partition.second.table_id.index,
			                       field.partition_key_index, field.field_id.index, SQLString(field.transform));
		}
	}

	// update old partition information for any tables that have been altered
	auto update_partition_query = StringUtil::Format(R"(
UPDATE {METADATA_CATALOG}.ducklake_partition_info
SET end_snapshot = {SNAPSHOT_ID}
WHERE table_id IN (%s) AND end_snapshot IS NULL
;)",
	                                                 old_partition_table_ids);
	string batch_query = update_partition_query;

	if (!new_partition_values.empty()) {
		new_partition_values =
		    "INSERT INTO {METADATA_CATALOG}.ducklake_partition_info(partition_id, table_id, begin_snapshot, "
		    "end_snapshot) VALUES " +
		    new_partition_values + ";";
		batch_query += new_partition_values;
	}
	if (!insert_partition_cols.empty()) {
		insert_partition_cols =
		    "INSERT INTO {METADATA_CATALOG}.ducklake_partition_column VALUES " + insert_partition_cols + ";";
		batch_query += insert_partition_cols;
	}
	return batch_query;
}

void CheckTableSortEqual(const vector<DuckLakeSortInfo> &old_sorts,
                         unordered_map<idx_t, DuckLakeSortInfo> &new_sort_map) {
	for (auto &sort : old_sorts) {
		if (new_sort_map.find(sort.table_id.index) != new_sort_map.end()) {
			if (new_sort_map[sort.table_id.index] == sort) {
				// If a new sort already exists in an old sort, it's a nop, we can remove it
				new_sort_map.erase(sort.table_id.index);
			}
		}
	}
}

void CheckTableSortReset(const unordered_set<idx_t> &old_sort_set, const vector<DuckLakeSortInfo> &new_sorts,
                         unordered_map<idx_t, DuckLakeSortInfo> &new_sort_map) {
	vector<idx_t> sort_ids_to_erase;
	for (auto &sort : new_sorts) {
		if (old_sort_set.find(sort.table_id.index) == old_sort_set.end() && sort.fields.empty()) {
			// If a map does not exist on the old sort and the sort has no fields, this is an reset over
			// an empty sort definition, hence also a nop
			sort_ids_to_erase.push_back(sort.table_id.index);
		}
	}
	for (auto &id : sort_ids_to_erase) {
		new_sort_map.erase(id);
	}
}

static unordered_map<idx_t, DuckLakeSortInfo> GetNewSorts(const vector<DuckLakeSortInfo> &old_sorts,
                                                          const vector<DuckLakeSortInfo> &new_sorts) {
	unordered_map<idx_t, DuckLakeSortInfo> new_sort_map;
	for (auto &sort : new_sorts) {
		new_sort_map[sort.table_id.index] = sort;
	}
	unordered_set<idx_t> old_sort_set;
	for (auto &sort : old_sorts) {
		old_sort_set.insert(sort.table_id.index);
	}
	CheckTableSortEqual(old_sorts, new_sort_map);
	CheckTableSortReset(old_sort_set, new_sorts, new_sort_map);

	return new_sort_map;
}

string DuckLakeMetadataManager::WriteNewSortKeys(const vector<DuckLakeSortInfo> &existing_sorts,
                                                 const vector<DuckLakeSortInfo> &new_sorts) {
	if (new_sorts.empty()) {
		return {};
	}

	string old_sort_table_ids;
	string new_sort_values;
	string new_sort_expressions;

	// Do not update if they are the same
	auto new_sort_map = GetNewSorts(existing_sorts, new_sorts);
	if (new_sort_map.empty()) {
		return {};
	}
	for (auto &new_sort : new_sort_map) {
		// set old partition data as no longer valid
		if (!old_sort_table_ids.empty()) {
			old_sort_table_ids += ", ";
		}
		old_sort_table_ids += to_string(new_sort.second.table_id.index);

		if (!new_sort.second.id.IsValid()) {
			// dropping sort data - skip adding new values but continue to set end_snapshot on old sort
			continue;
		}
		auto sort_id = new_sort.second.id.GetIndex();

		if (!new_sort_values.empty()) {
			new_sort_values += ", ";
		}
		new_sort_values +=
		    StringUtil::Format(R"((%d, %d, {SNAPSHOT_ID}, NULL))", sort_id, new_sort.second.table_id.index);

		for (auto &field : new_sort.second.fields) {
			if (!new_sort_expressions.empty()) {
				new_sort_expressions += ", ";
			}
			string sort_direction = (field.sort_direction == OrderType::DESCENDING ? "DESC" : "ASC");
			string null_order = (field.null_order == OrderByNullType::NULLS_FIRST ? "NULLS_FIRST" : "NULLS_LAST");
			new_sort_expressions +=
			    StringUtil::Format("(%d, %d, %d, %s, %s, %s, %s)", sort_id, new_sort.second.table_id.index,
			                       field.sort_key_index, SQLString(field.expression), SQLString(field.dialect),
			                       SQLString(sort_direction), SQLString(null_order));
		}
	}
	// update old sort information for any tables that have been altered
	auto update_sort_query = StringUtil::Format(R"(
UPDATE {METADATA_CATALOG}.ducklake_sort_info
SET end_snapshot = {SNAPSHOT_ID}
WHERE table_id IN (%s) AND end_snapshot IS NULL
;)",
	                                            old_sort_table_ids);
	string batch_query = update_sort_query;
	if (!new_sort_values.empty()) {
		new_sort_values = "INSERT INTO {METADATA_CATALOG}.ducklake_sort_info(sort_id, table_id, begin_snapshot, "
		                  "end_snapshot) VALUES " +
		                  new_sort_values + ";";
		batch_query += new_sort_values;
	}
	if (!new_sort_expressions.empty()) {
		new_sort_expressions =
		    "INSERT INTO {METADATA_CATALOG}.ducklake_sort_expression VALUES " + new_sort_expressions + ";";
		batch_query += new_sort_expressions;
	}

	return batch_query;
}

string DuckLakeMetadataManager::WriteNewTags(const vector<DuckLakeTagInfo> &new_tags) {
	if (new_tags.empty()) {
		return {};
	}
	// update old tags (if there were any)
	// get a list of all tags
	string tags_list;
	for (auto &tag : new_tags) {
		if (!tags_list.empty()) {
			tags_list += ", ";
		}
		tags_list += StringUtil::Format("(%d, %s)", tag.id, SQLString(tag.key));
	}

	// overwrite the snapshot for the old tags
	string batch_query = StringUtil::Format(R"(
WITH overwritten_tags(tid, key) AS (
VALUES %s
)
UPDATE {METADATA_CATALOG}.ducklake_tag
SET end_snapshot = {SNAPSHOT_ID}
FROM overwritten_tags
WHERE object_id=tid AND ducklake_tag.key=overwritten_tags.key AND end_snapshot IS NULL
;)",
	                                        tags_list);

	// now insert the new tags
	string new_tag_query;
	for (auto &tag : new_tags) {
		if (!new_tag_query.empty()) {
			new_tag_query += ", ";
		}
		new_tag_query += StringUtil::Format("(%d, {SNAPSHOT_ID}, NULL, %s, %s)", tag.id, SQLString(tag.key),
		                                    tag.value.ToSQLString());
	}

	new_tag_query = "INSERT INTO {METADATA_CATALOG}.ducklake_tag VALUES " + new_tag_query + ";";
	batch_query += new_tag_query;
	return batch_query;
}

template <class T, class OverwrittenValues, class NewValues>
static string WriteNewColumnTagBatch(const vector<T> &new_tags, const string &cte_columns, const string &metadata_table,
                                     const string &update_condition, OverwrittenValues overwritten_values,
                                     NewValues new_values) {
	if (new_tags.empty()) {
		return {};
	}
	string tags_list;
	for (auto &tag : new_tags) {
		if (!tags_list.empty()) {
			tags_list += ", ";
		}
		tags_list += overwritten_values(tag);
	}

	string batch_query = StringUtil::Format(R"(
WITH overwritten_tags(%s) AS (
VALUES %s
)
UPDATE {METADATA_CATALOG}.%s
SET end_snapshot = {SNAPSHOT_ID}
FROM overwritten_tags
WHERE %s AND end_snapshot IS NULL
;)",
	                                        cte_columns, tags_list, metadata_table, update_condition);

	string new_tag_query;
	for (auto &tag : new_tags) {
		if (!new_tag_query.empty()) {
			new_tag_query += ", ";
		}
		new_tag_query += new_values(tag);
	}

	batch_query += StringUtil::Format("INSERT INTO {METADATA_CATALOG}.%s VALUES %s;", metadata_table, new_tag_query);
	return batch_query;
}

string DuckLakeMetadataManager::WriteNewColumnTags(const vector<DuckLakeColumnTagInfo> &new_tags) {
	return WriteNewColumnTagBatch(
	    new_tags, "tid, cid, key", "ducklake_column_tag",
	    "table_id=tid AND column_id=cid AND ducklake_column_tag.key=overwritten_tags.key",
	    [](const DuckLakeColumnTagInfo &tag) {
		    return StringUtil::Format("(%d, %d, %s)", tag.table_id.index, tag.field_index.index, SQLString(tag.key));
	    },
	    [](const DuckLakeColumnTagInfo &tag) {
		    return StringUtil::Format("(%d, %d, {SNAPSHOT_ID}, NULL, %s, %s)", tag.table_id.index,
		                              tag.field_index.index, SQLString(tag.key), tag.value.ToSQLString());
	    });
}

string DuckLakeMetadataManager::WriteNewViewColumnTags(const vector<DuckLakeViewColumnTagInfo> &new_tags) {
	return WriteNewColumnTagBatch(
	    new_tags, "vid, col, k", "ducklake_view_column_tag",
	    "view_id=vid AND ducklake_view_column_tag.column_name=overwritten_tags.col AND "
	    "ducklake_view_column_tag.key=overwritten_tags.k",
	    [](const DuckLakeViewColumnTagInfo &tag) {
		    return StringUtil::Format("(%d, %s, %s)", tag.view_id.index, SQLString(tag.column_name),
		                              SQLString(tag.key));
	    },
	    [](const DuckLakeViewColumnTagInfo &tag) {
		    return StringUtil::Format("(%d, %s, {SNAPSHOT_ID}, NULL, %s, %s)", tag.view_id.index,
		                              SQLString(tag.column_name), SQLString(tag.key), tag.value.ToSQLString());
	    });
}

struct ColumnStatsSQL {
	string contains_null;
	string contains_nan;
	string min_val;
	string max_val;
	string extra_stats;

	static ColumnStatsSQL FromColumnStats(const DuckLakeGlobalColumnStatsInfo &col_stats) {
		ColumnStatsSQL result;
		result.contains_null = col_stats.has_contains_null ? (col_stats.contains_null ? "true" : "false") : "NULL";
		result.contains_nan = col_stats.has_contains_nan ? (col_stats.contains_nan ? "true" : "false") : "NULL";
		result.min_val = col_stats.has_min ? DuckLakeUtil::StatsToString(col_stats.min_val) : "NULL";
		result.max_val = col_stats.has_max ? DuckLakeUtil::StatsToString(col_stats.max_val) : "NULL";
		result.extra_stats = col_stats.has_extra_stats ? col_stats.extra_stats : "NULL";
		return result;
	}
};

string DuckLakeMetadataManager::UpdateGlobalTableStatsSql(const DuckLakeGlobalStatsInfo &stats) {
	string batch_query;

	if (!stats.initialized) {
		string column_stats_values;
		for (auto &col_stats : stats.column_stats) {
			if (!column_stats_values.empty()) {
				column_stats_values += ",";
			}
			auto sql = ColumnStatsSQL::FromColumnStats(col_stats);
			column_stats_values += StringUtil::Format(
			    "(%d, %d, %s, %s, %s, %s, %s{BRANCH_ID_VAL})", stats.table_id.index, col_stats.column_id.index,
			    sql.contains_null, sql.contains_nan, sql.min_val, sql.max_val, sql.extra_stats);
		}
		batch_query += StringUtil::Format(
		    "INSERT INTO {METADATA_CATALOG}.ducklake_table_stats(table_id, record_count, "
		    "next_row_id, file_size_bytes{BRANCH_ID_COL}) VALUES (%d, %d, %d, %d{BRANCH_ID_VAL});",
		    stats.table_id.index, stats.record_count, stats.next_row_id, stats.table_size_bytes);
		batch_query += StringUtil::Format(
		    "INSERT INTO {METADATA_CATALOG}.ducklake_table_column_stats(table_id, column_id, contains_null, "
		    "contains_nan, min_value, max_value, extra_stats{BRANCH_ID_COL}) VALUES %s;",
		    column_stats_values);
	} else {
		// stats have been initialized - update them
		batch_query += StringUtil::Format(
		    "UPDATE {METADATA_CATALOG}.ducklake_table_stats SET record_count=%d, file_size_bytes=%d, "
		    "next_row_id=%d WHERE table_id=%d{BRANCH_STATS_FILTER};",
		    stats.record_count, stats.table_size_bytes, stats.next_row_id, stats.table_id.index);
		// Emit one plain UPDATE per column rather than a single
		// `UPDATE ... FROM (VALUES ...)`. A DuckDB-backed metadata catalog
		// corrupts string payloads when an UPDATE pulls its new values from a
		// multi-row VALUES list whose VARCHAR column mixes NULLs with long
		// (heap-allocated) strings - exactly the shape produced by geometry
		// (and other) extra_stats. Per-column UPDATEs sidestep that bug and
		// work uniformly across DuckDB, Postgres and SQLite backends.
		// Use ANSI CAST(... AS BOOLEAN) instead of the PostgreSQL-flavored
		// `::boolean` operator: both DuckDB and Postgres accept ANSI CAST,
		// and SQLite's parser rejects `::` outright (SQLITE_ERROR:
		// unrecognized token ":").
		for (auto &col_stats : stats.column_stats) {
			auto sql = ColumnStatsSQL::FromColumnStats(col_stats);
			batch_query += StringUtil::Format(
			    "UPDATE {METADATA_CATALOG}.ducklake_table_column_stats "
			    "SET contains_null=CAST(%s AS BOOLEAN), contains_nan=CAST(%s AS BOOLEAN), min_value=%s, max_value=%s, "
			    "extra_stats=%s WHERE table_id=%d AND column_id=%d{BRANCH_STATS_FILTER};",
			    sql.contains_null, sql.contains_nan, sql.min_val, sql.max_val, sql.extra_stats, stats.table_id.index,
			    col_stats.column_id.index);
		}
	}
	return batch_query;
}

template <class T>
static timestamp_tz_t GetTimestampTZFromRow(ClientContext &context, const T &row, idx_t col_idx) {
	auto val = row.GetChunk().GetValue(col_idx, row.GetRowInChunk());
	return val.CastAs(context, LogicalType::TIMESTAMP_TZ).template GetValue<timestamp_tz_t>();
}

vector<DuckLakeSnapshotInfo> DuckLakeMetadataManager::GetAllSnapshots(const string &filter) {
	bool with_branch = transaction.GetCatalog().SupportsWritableBranches();
	string qualified_filter = filter;
	if (with_branch && !filter.empty()) {
		qualified_filter = StringUtil::Replace(qualified_filter, "ducklake_snapshot.branch_id", "s.branch_id");
		qualified_filter = StringUtil::Replace(qualified_filter, "ducklake_snapshot.snapshot_id", "s.snapshot_id");
		// Prefer already-qualified s.* from callers; upgrade bare names carefully.
		if (qualified_filter.find("s.snapshot_id") == string::npos &&
		    qualified_filter.find("s.branch_id") == string::npos) {
			qualified_filter = StringUtil::Replace(qualified_filter, "snapshot_id", "s.snapshot_id");
			qualified_filter = StringUtil::Replace(qualified_filter, "snapshot_time", "s.snapshot_time");
			qualified_filter = StringUtil::Replace(qualified_filter, "branch_id", "s.branch_id");
		}
	}
	string query;
	if (with_branch) {
		query = StringUtil::Format(R"(
SELECT s.snapshot_id, s.snapshot_time, s.schema_version, s.next_file_id, c.changes_made, c.author, c.commit_message,
       c.commit_extra_info, s.branch_id, r.ref_name
FROM {METADATA_CATALOG}.ducklake_snapshot s
LEFT JOIN {METADATA_CATALOG}.ducklake_snapshot_changes c USING (snapshot_id)
LEFT JOIN {METADATA_CATALOG}.ducklake_ref r ON r.ref_id = s.branch_id AND r.ref_type = 'branch'
%s %s
ORDER BY s.snapshot_id
)",
		                           qualified_filter.empty() ? "" : "WHERE", qualified_filter);
	} else {
		query = StringUtil::Format(R"(
SELECT snapshot_id, snapshot_time, schema_version, next_file_id, changes_made, author, commit_message, commit_extra_info
FROM {METADATA_CATALOG}.ducklake_snapshot
LEFT JOIN {METADATA_CATALOG}.ducklake_snapshot_changes USING (snapshot_id)
%s %s
ORDER BY snapshot_id
)",
		                           filter.empty() ? "" : "WHERE", filter);
	}
	auto res = Query(query);
	if (res->HasError()) {
		res->GetErrorObject().Throw("Failed to get snapshot information from DuckLake: ");
	}
	auto context = transaction.context.lock();
	vector<DuckLakeSnapshotInfo> snapshots;

	for (auto &row : *res) {
		DuckLakeSnapshotInfo snapshot_info;
		snapshot_info.id = row.GetValue<idx_t>(0);
		snapshot_info.time = GetTimestampTZFromRow(*context, row, 1);
		snapshot_info.schema_version = row.GetValue<idx_t>(2);
		snapshot_info.next_file_id = row.GetValue<idx_t>(3);
		snapshot_info.change_info.changes_made = row.IsNull(4) ? string() : row.GetValue<string>(4);
		snapshot_info.author = row.GetChunk().GetValue(5, row.GetRowInChunk());
		snapshot_info.commit_message = row.GetChunk().GetValue(6, row.GetRowInChunk());
		snapshot_info.commit_extra_info = row.GetChunk().GetValue(7, row.GetRowInChunk());
		if (with_branch && res->ColumnCount() > 8) {
			if (!row.IsNull(8)) {
				snapshot_info.branch_id = row.GetValue<idx_t>(8);
			}
			if (!row.IsNull(9)) {
				snapshot_info.branch_name = row.GetValue<string>(9);
			}
		}
		snapshots.push_back(std::move(snapshot_info));
	}
	return snapshots;
}

vector<DuckLakeFileForCleanup> DuckLakeMetadataManager::GetOldFilesForCleanup(const string &filter) {
	auto query = R"(
SELECT data_file_id, path, path_is_relative, schedule_start
FROM {METADATA_CATALOG}.ducklake_files_scheduled_for_deletion
)" + filter;
	auto res = Query(query);
	if (res->HasError()) {
		res->GetErrorObject().Throw("Failed to get files scheduled for deletion from DuckLake: ");
	}
	auto context = transaction.context.lock();
	vector<DuckLakeFileForCleanup> result;
	for (auto &row : *res) {
		DuckLakeFileForCleanup info;
		info.id = DataFileIndex(row.GetValue<idx_t>(0));
		DuckLakePath path;
		path.path = row.GetValue<string>(1);
		path.path_is_relative = row.GetValue<bool>(2);
		info.path = FromRelativePath(path);
		info.time = GetTimestampTZFromRow(*context, row, 3);
		result.push_back(std::move(info));
	}
	return result;
}

string DuckLakeMetadataManager::GetKnownFilesForCleanupQuery(const string &separator) const {
	auto query = R"(SELECT REPLACE(
           CASE
               WHEN NOT file_relative THEN file_path
               ELSE CASE
                        WHEN NOT table_relative THEN table_path || file_path
                        ELSE CASE
                                 WHEN NOT schema_relative THEN schema_path || table_path || file_path
                                 ELSE {DATA_PATH} || schema_path || table_path || file_path
                             END
                   END
           END,
           '\',
           '/'
       ) AS full_path
FROM
  (SELECT s.path AS schema_path, t.path AS table_path, file_path, s.path_is_relative AS schema_relative, t.path_is_relative AS table_relative, file_relative FROM (
    SELECT f.path AS file_path, f.path_is_relative AS file_relative, table_id
    FROM {METADATA_CATALOG}.ducklake_data_file f
    UNION ALL
    SELECT f.path AS file_path, f.path_is_relative AS file_relative, table_id
    FROM {METADATA_CATALOG}.ducklake_delete_file f
  ) AS f
   JOIN {METADATA_CATALOG}.ducklake_table t ON f.table_id = t.table_id
   JOIN {METADATA_CATALOG}.ducklake_schema s ON t.schema_id = s.schema_id) AS r
UNION ALL
SELECT REPLACE(
    CASE
        WHEN NOT f.path_is_relative THEN f.path
        ELSE {DATA_PATH} || f.path
    END ,
           '\',
           '/'
) AS full_path
FROM {METADATA_CATALOG}.ducklake_files_scheduled_for_deletion f
)";
	return StringUtil::Replace(query, "{SEPARATOR}", separator);
}

vector<DuckLakeFileForCleanup> DuckLakeMetadataManager::GetOrphanFilesForCleanup(const string &filter,
                                                                                 const string &separator) {
	auto known_files_query = GetKnownFilesForCleanupQuery(separator);
	auto known_files_res = Query(known_files_query);
	if (known_files_res->HasError()) {
		known_files_res->GetErrorObject().Throw("Failed to get files scheduled for deletion from DuckLake: ");
	}

	vector<string> known_files;
	for (auto &row : *known_files_res) {
		known_files.push_back(row.GetValue<string>(0));
	}

	const string temp_table = "__ducklake_known_cleanup_files";
	auto temp_table_identifier = DuckLakeUtil::SQLIdentifierToString(temp_table);

	auto create_temp_query =
	    StringUtil::Format("CREATE OR REPLACE TEMPORARY TABLE %s(full_path VARCHAR)", temp_table_identifier);
	auto create_temp_res = transaction.ExecuteRaw(create_temp_query);
	if (create_temp_res->HasError()) {
		create_temp_res->GetErrorObject().Throw("Failed to create temporary file list for DuckLake cleanup: ");
	}

	auto drop_temp_table = [&]() {
		try {
			auto drop_temp_query = StringUtil::Format("DROP TABLE IF EXISTS %s", temp_table_identifier);
			transaction.ExecuteRaw(drop_temp_query);
		} catch (...) {
		}
	};

	try {
		Appender appender(transaction.GetConnection(), Identifier(temp_table));
		for (auto &known_file : known_files) {
			appender.AppendRow(known_file.c_str());
		}
		appender.Close();

		auto query = StringUtil::Format(R"(SELECT filename
FROM read_blob({DATA_PATH} || '**') files
WHERE (suffix(filename, '.parquet') OR suffix(filename, '.puffin'))
AND NOT EXISTS (
	SELECT 1 FROM %s known_files WHERE known_files.full_path = REPLACE(files.filename, '\', '/')
)
%s)",
		                                temp_table_identifier, filter);
		SubstituteCatalogPlaceholders(query);
		auto res = transaction.ExecuteRaw(query);
		if (res->HasError()) {
			res->GetErrorObject().Throw("Failed to get files scheduled for deletion from DuckLake: ");
		}

		vector<DuckLakeFileForCleanup> result;
		for (auto &row : *res) {
			DuckLakeFileForCleanup info;
			info.path = row.GetValue<string>(0);
			result.push_back(std::move(info));
		}
		drop_temp_table();
		return result;
	} catch (...) {
		drop_temp_table();
		throw;
	}
}

vector<DuckLakeFileForCleanup> DuckLakeMetadataManager::GetFilesForCleanup(const string &filter, CleanupType type,
                                                                           const string &separator) {
	switch (type) {
	case CleanupType::OLD_FILES:
		return GetOldFilesForCleanup(filter);
	case CleanupType::ORPHANED_FILES:
		return GetOrphanFilesForCleanup(filter, separator);
	default:
		throw InternalException("CleanupType in DuckLakeMetadataManager::GetFilesForCleanup is not valid");
	}
}

void DuckLakeMetadataManager::RemoveFilesScheduledForCleanup(const vector<DuckLakeFileForCleanup> &cleaned_up_files) {
	string deleted_file_ids;
	for (auto &file : cleaned_up_files) {
		if (!deleted_file_ids.empty()) {
			deleted_file_ids += ", ";
		}
		deleted_file_ids += to_string(file.id.index);
	}
	auto result = Execute(StringUtil::Format(R"(
DELETE FROM {METADATA_CATALOG}.ducklake_files_scheduled_for_deletion
WHERE data_file_id IN (%s);
)",
	                                         deleted_file_ids));
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to delete scheduled cleanup files in DuckLake: ");
	}
}

idx_t DuckLakeMetadataManager::GetNextColumnId(TableIndex table_id) {
	auto result = Query(StringUtil::Format(R"(
	SELECT MAX(column_id)
	FROM {METADATA_CATALOG}.ducklake_column
	WHERE table_id=%d
)",
	                                       table_id.index));
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to get next column id in DuckLake: ");
	}
	for (auto &row : *result) {
		if (row.IsNull(0)) {
			break;
		}
		return row.GetValue<idx_t>(00) + 1;
	}
	throw InternalException("Invalid result for GetNextColumnId");
}

string DuckLakeMetadataManager::WriteMergeAdjacent(const vector<DuckLakeCompactedFileInfo> &compactions,
                                                   const vector<DuckLakePath> &resolved_paths) {
	if (compactions.empty()) {
		return {};
	}
	if (resolved_paths.size() != compactions.size()) {
		throw InternalException("WriteMergeAdjacent: resolved_paths size mismatch");
	}
	string deleted_file_ids;
	string scheduled_deletions;
	for (idx_t i = 0; i < compactions.size(); ++i) {
		auto &compaction = compactions[i];
		auto &path = resolved_paths[i];
		D_ASSERT(!compaction.path.empty());
		if (!deleted_file_ids.empty()) {
			deleted_file_ids += ", ";
			scheduled_deletions += ", ";
		}
		deleted_file_ids += to_string(compaction.source_id.index);
		scheduled_deletions += StringUtil::Format("(%d, %s, %s, NOW())", compaction.source_id.index,
		                                          SQLString(path.path), path.path_is_relative ? "true" : "false");
	}
	// H1: end-date branch-owned inputs; tombstone ancestor-owned inputs (never mutate ancestor rows).
	string batch_query = EndDateOrTombstone("ducklake_data_file", "data_file_id", deleted_file_ids);
	// Clear auxiliary metadata only for branch-owned sources we are about to hard-delete.
	vector<string> tables_to_delete_from {"ducklake_file_column_stats", "ducklake_delete_file",
	                                      "ducklake_file_partition_value", "ducklake_file_variant_stats"};
	for (auto &delete_from_tbl : tables_to_delete_from) {
		batch_query += StringUtil::Format(R"(
DELETE FROM {METADATA_CATALOG}.%s
WHERE data_file_id IN (%s)
  AND data_file_id IN (
	SELECT data_file_id FROM {METADATA_CATALOG}.ducklake_data_file
	WHERE data_file_id IN (%s) AND branch_id = {BRANCH_ID}
  );
)",
		                                  delete_from_tbl, deleted_file_ids, deleted_file_ids);
	}
	// Hard-delete branch-owned data_file rows (merge-adjacent historically removed them for GC).
	// Ancestor-owned rows stay; only the tombstone hides them on this branch.
	batch_query += StringUtil::Format(R"(
DELETE FROM {METADATA_CATALOG}.ducklake_data_file
WHERE data_file_id IN (%s) AND branch_id = {BRANCH_ID};
INSERT INTO {METADATA_CATALOG}.ducklake_files_scheduled_for_deletion
SELECT v.file_id, v.path, v.path_is_relative, NOW()
FROM (VALUES %s) AS v(file_id, path, path_is_relative, _ignored)
WHERE NOT EXISTS (
	SELECT 1 FROM {METADATA_CATALOG}.ducklake_data_file df WHERE df.data_file_id = v.file_id
);
)",
	                                  deleted_file_ids, scheduled_deletions);
	return batch_query;
}

string DuckLakeMetadataManager::WriteDeleteRewrites(const vector<DuckLakeCompactedFileInfo> &compactions) {
	if (compactions.empty()) {
		return {};
	}
	unordered_map<idx_t, idx_t> table_idx_last_snapshot;
	for (idx_t i = compactions.size(); i > 0; i--) {
		auto &compaction = compactions[i - 1];
		if (table_idx_last_snapshot.find(compaction.table_index.index) == table_idx_last_snapshot.end()) {
			table_idx_last_snapshot[compaction.table_index.index] = compaction.rewrite_snapshot.GetIndex();
		}
	}

	string batch_query;
	for (idx_t i = 0; i < compactions.size(); ++i) {
		auto &compaction = compactions[i];
		D_ASSERT(!compaction.path.empty());
		auto snap = table_idx_last_snapshot[compaction.table_index.index];
		if (compaction.delete_file_id.IsValid() && !compaction.delete_file_end_snapshot.IsValid()) {
			// Own delete files: end-date. Inherited: tombstone.
			batch_query += StringUtil::Format(R"(
UPDATE {METADATA_CATALOG}.ducklake_delete_file SET end_snapshot = %llu
WHERE delete_file_id = %llu AND branch_id = {BRANCH_ID};
INSERT INTO {METADATA_CATALOG}.ducklake_deletion_delete_file
  (branch_id, ancestor_branch_id, object_id, deleted_at_snapshot)
SELECT {BRANCH_ID}, df.branch_id, df.delete_file_id, %llu
FROM {METADATA_CATALOG}.ducklake_delete_file df
WHERE df.delete_file_id = %llu AND df.branch_id != {BRANCH_ID} AND df.end_snapshot IS NULL
  AND NOT EXISTS (
    SELECT 1 FROM {METADATA_CATALOG}.ducklake_deletion_delete_file d
    WHERE d.branch_id = {BRANCH_ID} AND d.ancestor_branch_id = df.branch_id
      AND d.object_id = df.delete_file_id AND d.deleted_at_snapshot <= %llu
  );
)",
			                                  snap, compaction.delete_file_id.index, snap,
			                                  compaction.delete_file_id.index, snap);
		}
		// Own data files: end-date. Inherited: tombstone (never end-date ancestor rows).
		batch_query += StringUtil::Format(R"(
UPDATE {METADATA_CATALOG}.ducklake_data_file SET end_snapshot = %llu
WHERE data_file_id = %llu AND branch_id = {BRANCH_ID};
INSERT INTO {METADATA_CATALOG}.ducklake_deletion_data_file
  (branch_id, ancestor_branch_id, object_id, deleted_at_snapshot)
SELECT {BRANCH_ID}, df.branch_id, df.data_file_id, %llu
FROM {METADATA_CATALOG}.ducklake_data_file df
WHERE df.data_file_id = %llu AND df.branch_id != {BRANCH_ID} AND df.end_snapshot IS NULL
  AND NOT EXISTS (
    SELECT 1 FROM {METADATA_CATALOG}.ducklake_deletion_data_file d
    WHERE d.branch_id = {BRANCH_ID} AND d.ancestor_branch_id = df.branch_id
      AND d.object_id = df.data_file_id AND d.deleted_at_snapshot <= %llu
  );
)",
		                                  snap, compaction.source_id.index, snap, compaction.source_id.index, snap);
		if (compaction.new_id.IsValid()) {
			batch_query +=
			    StringUtil::Format(R"(
UPDATE {METADATA_CATALOG}.ducklake_data_file SET begin_snapshot = %llu
WHERE data_file_id = %llu;
)",
			                       snap, compaction.new_id.index);
		}
	}
	return batch_query;
}

string DuckLakeMetadataManager::WriteCompactions(const vector<DuckLakeCompactedFileInfo> &compactions,
                                                 CompactionType type, const vector<DuckLakePath> &resolved_paths) {
	switch (type) {
	case CompactionType::MERGE_ADJACENT_TABLES:
		return WriteMergeAdjacent(compactions, resolved_paths);
	case CompactionType::REWRITE_DELETES:
		return WriteDeleteRewrites(compactions);
	default:
		throw InternalException("DuckLakeMetadataManager::WriteCompactions: CompactionType is not accepted");
	}
}

void DuckLakeMetadataManager::DeleteSnapshots(const vector<DuckLakeSnapshotInfo> &snapshots) {
	unique_ptr<QueryResult> result;
	// first delete the actual snapshots
	string snapshot_ids;
	for (auto &snapshot : snapshots) {
		if (!snapshot_ids.empty()) {
			snapshot_ids += ", ";
		}
		snapshot_ids += to_string(snapshot.id);
	}

	vector<TableIndex> stats_table_ids;
	result = Query("SELECT DISTINCT table_id FROM {METADATA_CATALOG}.ducklake_table_stats;");
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to list table stats for cache invalidation in DuckLake: ");
	}
	for (auto &row : *result) {
		stats_table_ids.push_back(TableIndex(row.GetValue<idx_t>(0)));
	}

	vector<string> tables_to_delete_from {"ducklake_snapshot", "ducklake_snapshot_changes"};
	for (auto &delete_tbl : tables_to_delete_from) {
		result = Execute(StringUtil::Format(R"(
DELETE FROM {METADATA_CATALOG}.%s
WHERE snapshot_id IN (%s);
)",
		                                    delete_tbl, snapshot_ids));
		if (result->HasError()) {
			result->GetErrorObject().Throw("Failed to delete snapshots in DuckLake: ");
		}
	}
	// get a list of tables that are no longer required after these deletions
	result = Query(R"(
SELECT table_id
FROM {METADATA_CATALOG}.ducklake_table t
WHERE end_snapshot IS NOT NULL AND NOT EXISTS (
    SELECT snapshot_id
    FROM {METADATA_CATALOG}.ducklake_snapshot
    WHERE snapshot_id >= begin_snapshot AND snapshot_id < end_snapshot
)
AND NOT EXISTS (
    SELECT 1
    FROM {METADATA_CATALOG}.ducklake_table t2
    WHERE t2.table_id = t.table_id
      AND (t2.end_snapshot IS NULL OR  EXISTS (SELECT snapshot_id
    FROM {METADATA_CATALOG}.ducklake_snapshot
    WHERE  snapshot_id >= begin_snapshot AND snapshot_id < t2.end_snapshot))
  );)");

	vector<TableIndex> cleanup_tables;
	for (auto &row : *result) {
		cleanup_tables.push_back(TableIndex(row.GetValue<idx_t>(0)));
	}
	string deleted_table_ids;
	for (auto &table_id : cleanup_tables) {
		if (!deleted_table_ids.empty()) {
			deleted_table_ids += ", ";
		}
		deleted_table_ids += to_string(table_id.index);
	}

	// get a list of files that are no longer required after these deletions
	string table_id_filter;
	if (!deleted_table_ids.empty()) {
		table_id_filter = StringUtil::Format("table_id IN (%s) OR", deleted_table_ids);
	}

	result = Query(StringUtil::Format(R"(
SELECT data_file_id, table_id, path, path_is_relative
FROM {METADATA_CATALOG}.ducklake_data_file
WHERE %s (end_snapshot IS NOT NULL AND NOT EXISTS(
    SELECT snapshot_id
    FROM {METADATA_CATALOG}.ducklake_snapshot
    WHERE snapshot_id >= begin_snapshot AND snapshot_id < end_snapshot
));)",
	                                  table_id_filter));
	vector<DuckLakeFileForCleanup> cleanup_files;
	for (auto &row : *result) {
		DuckLakeFileForCleanup info;
		info.id = DataFileIndex(row.GetValue<idx_t>(0));
		TableIndex table_id(row.GetValue<idx_t>(1));
		DuckLakePath path;
		path.path = row.GetValue<string>(2);
		path.path_is_relative = row.GetValue<bool>(3);
		info.path = FromRelativePath(table_id, path);
		if (FileIsReachable(info.id.index)) {
			continue;
		}

		cleanup_files.push_back(std::move(info));
	}
	string deleted_file_ids;
	if (!cleanup_files.empty()) {
		string files_scheduled_for_cleanup;
		for (auto &file : cleanup_files) {
			if (!deleted_file_ids.empty()) {
				deleted_file_ids += ", ";
			}
			deleted_file_ids += to_string(file.id.index);

			if (!files_scheduled_for_cleanup.empty()) {
				files_scheduled_for_cleanup += ", ";
			}
			auto path = GetRelativePath(file.path);
			files_scheduled_for_cleanup += StringUtil::Format(
			    "(%d, %s, %s, NOW())", file.id.index, SQLString(path.path), path.path_is_relative ? "true" : "false");
		}

		// delete the data files
		tables_to_delete_from = {"ducklake_data_file", "ducklake_file_column_stats", "ducklake_file_variant_stats",
		                         "ducklake_file_partition_value"};
		for (auto &delete_tbl : tables_to_delete_from) {
			result = Execute(StringUtil::Format(R"(
DELETE FROM {METADATA_CATALOG}.%s
WHERE data_file_id IN (%s);
)",
			                                    delete_tbl, deleted_file_ids));
			if (result->HasError()) {
				result->GetErrorObject().Throw("Failed to delete old data file information in DuckLake: ");
			}
		}
		// insert the to-be-cleaned-up files
		result = Execute(StringUtil::Format(R"(
INSERT INTO {METADATA_CATALOG}.ducklake_files_scheduled_for_deletion
VALUES %s;
)",
		                                    files_scheduled_for_cleanup));
		if (result->HasError()) {
			result->GetErrorObject().Throw("Failed to schedule files for clean-up in DuckLake: ");
		}
	}

	// get a list of delete files that are no longer required after these deletions
	string file_id_filter;
	if (!deleted_file_ids.empty()) {
		file_id_filter = StringUtil::Format("data_file_id IN (%s) OR", deleted_file_ids);
	}

	result = Query(StringUtil::Format(R"(
SELECT delete_file_id, table_id, path, path_is_relative, data_file_id
FROM {METADATA_CATALOG}.ducklake_delete_file
WHERE %s %s (end_snapshot IS NOT NULL AND NOT EXISTS(
    SELECT snapshot_id
    FROM {METADATA_CATALOG}.ducklake_snapshot
    WHERE snapshot_id >= begin_snapshot AND snapshot_id < end_snapshot
));)",
	                                  table_id_filter, file_id_filter));
	vector<DuckLakeFileForCleanup> cleanup_deletes;
	for (auto &row : *result) {
		DuckLakeFileForCleanup info;
		info.id = DataFileIndex(row.GetValue<idx_t>(0));
		TableIndex table_id(row.GetValue<idx_t>(1));

		DuckLakePath path;
		path.path = row.GetValue<string>(2);
		path.path_is_relative = row.GetValue<bool>(3);
		info.path = FromRelativePath(table_id, path);
		if (FileIsReachable(row.GetValue<idx_t>(4))) {
			continue;
		}

		cleanup_deletes.push_back(std::move(info));
	}
	if (!cleanup_deletes.empty()) {
		string deleted_delete_ids;
		string files_scheduled_for_cleanup;
		for (auto &file : cleanup_deletes) {
			if (!deleted_delete_ids.empty()) {
				deleted_delete_ids += ", ";
			}
			deleted_delete_ids += to_string(file.id.index);

			if (!files_scheduled_for_cleanup.empty()) {
				files_scheduled_for_cleanup += ", ";
			}
			auto path = GetRelativePath(file.path);
			files_scheduled_for_cleanup += StringUtil::Format(
			    "(%d, %s, %s, NOW())", file.id.index, SQLString(path.path), path.path_is_relative ? "true" : "false");
		}
		// delete the delete files
		result = Execute(StringUtil::Format(R"(
DELETE FROM {METADATA_CATALOG}.ducklake_delete_file
WHERE delete_file_id IN (%s);
)",
		                                    deleted_delete_ids));
		if (result->HasError()) {
			result->GetErrorObject().Throw("Failed to delete old delete file information in DuckLake: ");
		}
		// insert the to-be-cleaned-up files
		result = Execute(StringUtil::Format(R"(
INSERT INTO {METADATA_CATALOG}.ducklake_files_scheduled_for_deletion
VALUES %s;
)",
		                                    files_scheduled_for_cleanup));
		if (result->HasError()) {
			result->GetErrorObject().Throw("Failed to schedule files for clean-up in DuckLake: ");
		}
	}

	// delete based on table id -> ducklake_table_stats, ducklake_table_column_stats, ducklake_partition_info
	if (!deleted_table_ids.empty()) {
		// Collect orphaned_inlined tables
		vector<string> inlined_tables_to_drop;
		{
			auto result = Query(StringUtil::Format(R"(
SELECT table_name
FROM {METADATA_CATALOG}.ducklake_inlined_data_tables
WHERE table_id IN (%s);)",
			                                       deleted_table_ids));
			if (result->HasError()) {
				result->GetErrorObject().Throw("Failed to read ducklake_inlined_data_tables for cleanup in DuckLake: ");
			}
			for (auto &row : *result) {
				inlined_tables_to_drop.push_back(row.GetValue<string>(0));
			}
		}

		tables_to_delete_from = {
		    "ducklake_table",           "ducklake_table_stats",         "ducklake_table_column_stats",
		    "ducklake_partition_info",  "ducklake_partition_column",    "ducklake_column",
		    "ducklake_column_tag",      "ducklake_sort_info",           "ducklake_sort_expression",
		    "ducklake_schema_versions", "ducklake_inlined_data_tables", "ducklake_column_mapping"};
		for (auto &delete_tbl : tables_to_delete_from) {
			auto result = Execute(StringUtil::Format(R"(
DELETE FROM {METADATA_CATALOG}.%s
WHERE table_id IN (%s);)",
			                                         delete_tbl, deleted_table_ids));
			if (result->HasError()) {
				result->GetErrorObject().Throw("Failed to delete from " + delete_tbl + " in DuckLake: ");
			}
		}

		// Drop orphaned inlined tables
		for (auto &inlined_table_name : inlined_tables_to_drop) {
			auto result = Execute(
			    StringUtil::Format("DROP TABLE IF EXISTS {METADATA_CATALOG}.%s;", SQLIdentifier(inlined_table_name)));
			if (result->HasError()) {
				result->GetErrorObject().Throw("Failed to drop inlined-data table in DuckLake: ");
			}
		}
	}

	// delete any views, schemas, macros, etc that are no longer referenced
	tables_to_delete_from = {"ducklake_schema", "ducklake_view", "ducklake_tag", "ducklake_macro"};
	if (transaction.GetCatalog().SupportsViewColumnTags()) {
		tables_to_delete_from.insert(tables_to_delete_from.begin() + 2, "ducklake_view_column_tag");
	}
	for (auto &delete_tbl : tables_to_delete_from) {
		auto result = Execute(StringUtil::Format(R"(
DELETE FROM {METADATA_CATALOG}.%s
WHERE end_snapshot IS NOT NULL AND NOT EXISTS(
    SELECT snapshot_id
    FROM {METADATA_CATALOG}.ducklake_snapshot
    WHERE snapshot_id >= begin_snapshot AND snapshot_id < end_snapshot
);)",
		                                         delete_tbl));
		if (result->HasError()) {
			result->GetErrorObject().Throw("Failed to delete from " + delete_tbl + " in DuckLake: ");
		}
	}

	// clean up macro implementation and parameters for deleted macros
	tables_to_delete_from = {"ducklake_macro_impl", "ducklake_macro_parameters"};
	for (auto &delete_tbl : tables_to_delete_from) {
		auto result = Execute(StringUtil::Format(R"(
DELETE FROM {METADATA_CATALOG}.%s tbl
WHERE NOT EXISTS (
    SELECT 1 FROM {METADATA_CATALOG}.ducklake_macro m
    WHERE m.macro_id = tbl.macro_id
);)",
		                                         delete_tbl));
		if (result->HasError()) {
			result->GetErrorObject().Throw("Failed to delete from " + delete_tbl + " in DuckLake: ");
		}
	}

	auto &catalog = transaction.GetCatalog();

	// clean up name mappings for deleted column mappings
	{
		auto result = Query(R"(
SELECT DISTINCT tbl.mapping_id
FROM {METADATA_CATALOG}.ducklake_name_mapping tbl
WHERE NOT EXISTS (
    SELECT 1 FROM {METADATA_CATALOG}.ducklake_column_mapping m
    WHERE m.mapping_id = tbl.mapping_id
);)");
		if (result->HasError()) {
			result->GetErrorObject().Throw("Failed to list deleted name mappings in DuckLake: ");
		}
		for (auto &row : *result) {
			transaction.DeferNameMapCacheInvalidation(MappingIndex(row.GetValue<idx_t>(0)));
		}

		result = Execute(R"(
DELETE FROM {METADATA_CATALOG}.ducklake_name_mapping tbl
WHERE NOT EXISTS (
    SELECT 1 FROM {METADATA_CATALOG}.ducklake_column_mapping m
    WHERE m.mapping_id = tbl.mapping_id
);)");
		if (result->HasError()) {
			result->GetErrorObject().Throw("Failed to delete from ducklake_name_mapping in DuckLake: ");
		}
	}

	for (auto &snapshot : snapshots) {
		for (auto &table_id : stats_table_ids) {
			catalog.InvalidateTableStatsCache(snapshot.next_file_id, table_id);
		}
	}
}

void DuckLakeMetadataManager::DropEmptySupersededInlinedTables() {
	// Find inlined tables that have been superseded by a newer schema version.
	auto targets = Query(R"(
SELECT idt.table_id, idt.schema_version, idt.table_name
FROM {METADATA_CATALOG}.ducklake_inlined_data_tables idt
WHERE idt.schema_version < (
    SELECT MAX(idt2.schema_version)
    FROM {METADATA_CATALOG}.ducklake_inlined_data_tables idt2
    WHERE idt2.table_id = idt.table_id
);)");
	if (targets->HasError()) {
		targets->GetErrorObject().Throw("Failed to identify superseded inlined-data tables in DuckLake: ");
	}
	// Only drop tables that are actually empty (data was flushed to files).
	string drops;
	for (auto &row : *targets) {
		auto table_id = row.GetValue<idx_t>(0);
		auto schema_version = row.GetValue<idx_t>(1);
		auto table_name = row.GetValue<string>(2);
		auto count_result =
		    Query(StringUtil::Format("SELECT COUNT(*) FROM {METADATA_CATALOG}.%s", SQLIdentifier(table_name)));
		if (count_result->HasError()) {
			count_result->GetErrorObject().Throw(
			    "Failed to check emptiness of superseded inlined-data table in DuckLake: ");
		}
		if ((*count_result->begin()).GetValue<idx_t>(0) != 0) {
			continue;
		}
		drops += StringUtil::Format(
		    "DELETE FROM {METADATA_CATALOG}.ducklake_inlined_data_tables WHERE table_id=%d AND schema_version=%d;"
		    "DROP TABLE IF EXISTS {METADATA_CATALOG}.%s;",
		    table_id, schema_version, SQLIdentifier(table_name));
	}
	if (drops.empty()) {
		return;
	}
	auto res = Execute(drops);
	if (res->HasError()) {
		res->GetErrorObject().Throw("Failed to drop superseded inlined-data tables in DuckLake: ");
	}
	// We also need to invalidate the existing schema versions in our catalog
	auto &catalog = transaction.GetCatalog();
	auto snapshot_versions = Query("SELECT DISTINCT schema_version FROM {METADATA_CATALOG}.ducklake_snapshot;");
	if (snapshot_versions->HasError()) {
		snapshot_versions->GetErrorObject().Throw("Failed to list schema versions for cache invalidation: ");
	}
	for (auto &row : *snapshot_versions) {
		catalog.InvalidateSchemaCache(row.GetValue<idx_t>(0));
	}
}

void DuckLakeMetadataManager::DeleteInlinedData(const DuckLakeInlinedTableInfo &inlined_table) {
	auto result = Execute(StringUtil::Format(R"(
		DELETE FROM {METADATA_CATALOG}.%s
)",
	                                         SQLIdentifier(inlined_table.table_name)));
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to delete inlined data in DuckLake from table " +
		                               inlined_table.table_name + ": ");
	}
}

void DuckLakeMetadataManager::DeleteFlushedInlinedData(const DuckLakeInlinedTableInfo &inlined_table,
                                                       idx_t flush_snapshot_id) {
	const bool shared_layout =
	    transaction.GetCatalog().SupportsWritableBranches() && transaction.GetCatalog().GetInliningLayout() == "shared_table";
	string branch_filter;
	if (shared_layout) {
		branch_filter = StringUtil::Format(" AND branch_id = %llu", transaction.GetSnapshot().branch_id);
	}
	auto result = Execute(StringUtil::Format(R"(
		DELETE FROM {METADATA_CATALOG}.%s WHERE begin_snapshot <= %d%s
)",
	                                         SQLIdentifier(inlined_table.table_name), flush_snapshot_id, branch_filter));
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to delete flushed inlined data in DuckLake from table " +
		                               inlined_table.table_name + ": ");
	}
}

string
DuckLakeMetadataManager::GenerateDeleteFlushedInlinedData(const vector<FlushedInlinedTableInfo> &flushed_tables,
                                                          bool shared_layout) {
	string result;
	for (auto &flushed : flushed_tables) {
		result += StringUtil::Format("DELETE FROM {METADATA_CATALOG}.%s WHERE begin_snapshot <= %d%s;\n",
		                             SQLIdentifier(flushed.inlined_table.table_name), flushed.flush_snapshot_id,
		                             shared_layout ? " AND branch_id = {BRANCH_ID}" : "");
	}
	return result;
}

string DuckLakeMetadataManager::InsertNewSchema(const DuckLakeSnapshot &snapshot, const set<TableIndex> &table_ids) {
	if (table_ids.empty()) {
		return {};
	}
	string result;
	for (auto &table_id : table_ids) {
		result += StringUtil::Format(
		    R"(INSERT INTO {METADATA_CATALOG}.ducklake_schema_versions(begin_snapshot, schema_version, table_id) VALUES (%d,%d,%d);)",
		    snapshot.snapshot_id, snapshot.schema_version, table_id.index);
	}
	return result;
}

vector<DuckLakeTableSizeInfo> DuckLakeMetadataManager::GetTableSizes(DuckLakeSnapshot snapshot) {
	vector<DuckLakeTableSizeInfo> table_sizes;
	auto result = Query(snapshot, R"(
SELECT
	schema_id, table_id, table_name, table_uuid,
	data_file_info.file_count AS data_file_count,
	data_file_info.total_file_size AS data_total_size,
	delete_file_info.file_count AS delete_file_count,
	delete_file_info.total_file_size AS delete_total_size
FROM {METADATA_CATALOG}.ducklake_table tbl, LATERAL (
	SELECT COUNT(*) file_count, SUM(file_size_bytes) total_file_size
	FROM {METADATA_CATALOG}.ducklake_data_file df
	WHERE df.table_id = tbl.table_id AND {SNAPSHOT_ID} >= begin_snapshot AND ({SNAPSHOT_ID} < end_snapshot OR end_snapshot IS NULL)
) data_file_info, LATERAL (
	SELECT COUNT(*) file_count, SUM(file_size_bytes) total_file_size
	FROM {METADATA_CATALOG}.ducklake_delete_file df
	WHERE df.table_id = tbl.table_id AND {SNAPSHOT_ID} >= begin_snapshot AND ({SNAPSHOT_ID} < end_snapshot OR end_snapshot IS NULL)
) delete_file_info
WHERE {SNAPSHOT_ID} >= begin_snapshot AND ({SNAPSHOT_ID} < end_snapshot OR end_snapshot IS NULL)
)");
	for (auto &row : *result) {
		DuckLakeTableSizeInfo table_size;
		table_size.schema_id = SchemaIndex(row.GetValue<idx_t>(0));
		table_size.table_id = TableIndex(row.GetValue<idx_t>(1));
		table_size.table_name = row.GetValue<string>(2);
		table_size.table_uuid = row.GetValue<string>(3);
		if (!row.IsNull(4)) {
			table_size.file_count = row.GetValue<idx_t>(4);
		}
		if (!row.IsNull(5)) {
			table_size.file_size_bytes = row.GetValue<idx_t>(5);
		}
		if (!row.IsNull(6)) {
			table_size.delete_file_count = row.GetValue<idx_t>(6);
		}
		if (!row.IsNull(7)) {
			table_size.delete_file_size_bytes = row.GetValue<idx_t>(7);
		}
		table_sizes.push_back(std::move(table_size));
	}
	return table_sizes;
}

void DuckLakeMetadataManager::SetConfigOption(const DuckLakeConfigOption &option) {
	// check if the option already exists
	auto &option_key = option.option.key;
	auto &option_value = option.option.value;
	if (option_key == "inlining_layout" && option_value != transaction.GetCatalog().GetInliningLayout()) {
		auto unsafe_layout = Query(R"(
SELECT 1
FROM {METADATA_CATALOG}.ducklake_inlined_data_tables
WHERE branch_id != 0 OR table_name LIKE '%\_b%' ESCAPE '\'
LIMIT 1
)");
		if (unsafe_layout->HasError()) {
			unsafe_layout->GetErrorObject().Throw("Failed to validate inlining_layout change in DuckLake: ");
		}
		for (auto &row : *unsafe_layout) {
			(void)row;
			throw InvalidInputException(
			    "Cannot change inlining_layout after non-main or per-branch inlined data tables have been created; "
			    "use ducklake.convert_inlining_layout(..., dry_run := true) to inspect a conversion first");
		}
		auto tables_result = Query(R"(
SELECT DISTINCT table_name
FROM {METADATA_CATALOG}.ducklake_inlined_data_tables
)");
		if (tables_result->HasError()) {
			tables_result->GetErrorObject().Throw("Failed to validate inlining_layout change in DuckLake: ");
		}
		for (auto &row : *tables_result) {
			auto table_name = row.GetValue<string>(0);
			auto rows_result = Query(StringUtil::Format(
			    "SELECT 1 FROM {METADATA_CATALOG}.%s WHERE branch_id != 0 LIMIT 1", SQLIdentifier(table_name)));
			if (rows_result->HasError()) {
				continue;
			}
			for (auto &physical_row : *rows_result) {
				(void)physical_row;
				throw InvalidInputException(
				    "Cannot change inlining_layout after non-main or per-branch inlined data tables have been "
				    "created; use ducklake.convert_inlining_layout(..., dry_run := true) to inspect a conversion "
				    "first");
			}
		}
	}
	string scope;
	string scope_id;
	string scope_filter;
	if (option.table_id.IsValid()) {
		scope = "'table'";
		scope_id = to_string(option.table_id.index);
		scope_filter = StringUtil::Format("scope = 'table' AND scope_id = %d", option.table_id.index);
	} else if (option.schema_id.IsValid()) {
		scope = "'schema'";
		scope_id = to_string(option.schema_id.index);
		scope_filter = StringUtil::Format("scope = 'schema' AND scope_id = %d", option.schema_id.index);
	} else {
		scope = "NULL";
		scope_id = "NULL";
		scope_filter = "scope IS NULL";
	}
	auto result = Query(StringUtil::Format(R"(
SELECT COUNT(*)
FROM {METADATA_CATALOG}.ducklake_metadata
WHERE key = %s AND %s
)",
	                                       SQLString(option_key), scope_filter));

	auto count = result->Fetch()->GetValue(0, 0).GetValue<idx_t>();
	if (count == 0) {
		// option does not yet exist - insert the value
		result = Execute(StringUtil::Format(R"(
INSERT INTO {METADATA_CATALOG}.ducklake_metadata VALUES (%s, %s, %s, %s)
)",
		                                    SQLString(option_key), SQLString(option_value), scope, scope_id));
	} else {
		// option already exists - update it
		result = Execute(StringUtil::Format(R"(
UPDATE {METADATA_CATALOG}.ducklake_metadata SET value=%s WHERE key=%s AND %s
)",
		                                    SQLString(option_value), SQLString(option_key), scope_filter));
	}
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to insert config option in DuckLake: ");
	}
}

bool DuckLakeMetadataManager::IsEncrypted() const {
	return transaction.GetCatalog().Encryption() == DuckLakeEncryption::ENCRYPTED;
}

} // namespace duckdb
