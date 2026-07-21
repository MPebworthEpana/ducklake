#include "metadata_manager/ducklake_metadata_manager_v1_1.hpp"
#include "metadata_manager/sqlite_metadata_manager.hpp"
#include "metadata_manager/postgres_metadata_manager.hpp"
#include "metadata_manager/quack_metadata_manager.hpp"
#include "common/ducklake_version.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_transaction.hpp"

namespace duckdb {

template <typename Base>
string DuckLakeMetadataManagerV1_1<Base>::GetDataFileTableStatement() {
	string stmt =
	    "CREATE TABLE {METADATA_CATALOG}.ducklake_data_file(data_file_id BIGINT PRIMARY KEY, table_id BIGINT, "
	    "begin_snapshot BIGINT, end_snapshot BIGINT, file_order BIGINT, path VARCHAR, path_is_relative BOOLEAN, "
	    "file_format VARCHAR, record_count BIGINT, file_size_bytes BIGINT, footer_size BIGINT, row_id_start BIGINT, "
	    "partition_id BIGINT, encryption_key VARCHAR, mapping_id BIGINT, partial_max BIGINT, row_group_count BIGINT";
	if (Base::transaction.GetCatalog().GetDuckLakeVersion() >= DuckLakeVersion::V1_1_DEV_3) {
		stmt += ", branch_id BIGINT DEFAULT 0";
	}
	stmt += ");";
	return stmt;
}

template <typename Base>
string DuckLakeMetadataManagerV1_1<Base>::GetDeleteFileTableStatement() {
	string stmt =
	    "CREATE TABLE {METADATA_CATALOG}.ducklake_delete_file(delete_file_id BIGINT PRIMARY KEY, table_id BIGINT, "
	    "begin_snapshot BIGINT, end_snapshot BIGINT, data_file_id BIGINT, path VARCHAR, path_is_relative BOOLEAN, "
	    "format VARCHAR, delete_count BIGINT, file_size_bytes BIGINT, footer_size BIGINT, encryption_key VARCHAR, "
	    "partial_max BIGINT, row_group_count BIGINT";
	if (Base::transaction.GetCatalog().GetDuckLakeVersion() >= DuckLakeVersion::V1_1_DEV_3) {
		stmt += ", branch_id BIGINT DEFAULT 0, data_file_branch_id BIGINT DEFAULT 0";
	}
	stmt += ");";
	return stmt;
}

template <typename Base>
string DuckLakeMetadataManagerV1_1<Base>::GetCreateTableStatements() {
	auto result = Base::GetCreateTableStatements();
	result += "CREATE TABLE {METADATA_CATALOG}.ducklake_view_column_tag(view_id BIGINT, column_name VARCHAR, "
	          "begin_snapshot BIGINT, end_snapshot BIGINT, key VARCHAR, value VARCHAR);\n";
	auto version = Base::transaction.GetCatalog().GetDuckLakeVersion();
	if (version >= DuckLakeVersion::V1_1_DEV_2) {
		result += "CREATE TABLE {METADATA_CATALOG}.ducklake_ref(ref_id BIGINT PRIMARY KEY, ref_name VARCHAR, ref_type "
		          "VARCHAR, snapshot_id BIGINT, parent_ref_id BIGINT, status VARCHAR, created_at TIMESTAMPTZ);\n";
	}
	if (version >= DuckLakeVersion::V1_1_DEV_3) {
		result += "ALTER TABLE {METADATA_CATALOG}.ducklake_snapshot ADD COLUMN branch_id BIGINT DEFAULT 0;\n";
		result += "ALTER TABLE {METADATA_CATALOG}.ducklake_schema ADD COLUMN branch_id BIGINT DEFAULT 0;\n";
		result += "ALTER TABLE {METADATA_CATALOG}.ducklake_table ADD COLUMN branch_id BIGINT DEFAULT 0;\n";
		result += "ALTER TABLE {METADATA_CATALOG}.ducklake_view ADD COLUMN branch_id BIGINT DEFAULT 0;\n";
		result += "ALTER TABLE {METADATA_CATALOG}.ducklake_column ADD COLUMN branch_id BIGINT DEFAULT 0;\n";
		result += "ALTER TABLE {METADATA_CATALOG}.ducklake_macro ADD COLUMN branch_id BIGINT DEFAULT 0;\n";
		result += "ALTER TABLE {METADATA_CATALOG}.ducklake_partition_info ADD COLUMN branch_id BIGINT DEFAULT 0;\n";
		result += "ALTER TABLE {METADATA_CATALOG}.ducklake_sort_info ADD COLUMN branch_id BIGINT DEFAULT 0;\n";
		result += "ALTER TABLE {METADATA_CATALOG}.ducklake_table_stats ADD COLUMN branch_id BIGINT DEFAULT 0;\n";
		result += "ALTER TABLE {METADATA_CATALOG}.ducklake_table_column_stats ADD COLUMN branch_id BIGINT DEFAULT 0;\n";
		result += "ALTER TABLE {METADATA_CATALOG}.ducklake_inlined_data_tables ADD COLUMN branch_id BIGINT DEFAULT 0;\n";
		result += "ALTER TABLE {METADATA_CATALOG}.ducklake_schema_versions ADD COLUMN branch_id BIGINT DEFAULT 0;\n";
		result +=
		    "CREATE TABLE {METADATA_CATALOG}.ducklake_branch_lineage(branch_id BIGINT, ancestor_branch_id BIGINT, "
		    "max_visible_snapshot BIGINT);\n";
		result +=
		    "CREATE TABLE {METADATA_CATALOG}.ducklake_deletion_schema(branch_id BIGINT, ancestor_branch_id BIGINT, "
		    "object_id BIGINT, deleted_at_snapshot BIGINT);\n";
		result += "CREATE TABLE {METADATA_CATALOG}.ducklake_deletion_table(branch_id BIGINT, ancestor_branch_id BIGINT, "
		          "object_id BIGINT, deleted_at_snapshot BIGINT);\n";
		result += "CREATE TABLE {METADATA_CATALOG}.ducklake_deletion_view(branch_id BIGINT, ancestor_branch_id BIGINT, "
		          "object_id BIGINT, deleted_at_snapshot BIGINT);\n";
		result +=
		    "CREATE TABLE {METADATA_CATALOG}.ducklake_deletion_column(branch_id BIGINT, ancestor_branch_id BIGINT, "
		    "object_id BIGINT, deleted_at_snapshot BIGINT);\n";
		result +=
		    "CREATE TABLE {METADATA_CATALOG}.ducklake_deletion_data_file(branch_id BIGINT, ancestor_branch_id BIGINT, "
		    "object_id BIGINT, deleted_at_snapshot BIGINT);\n";
		result += "CREATE TABLE {METADATA_CATALOG}.ducklake_deletion_delete_file(branch_id BIGINT, ancestor_branch_id "
		          "BIGINT, object_id BIGINT, deleted_at_snapshot BIGINT);\n";
		result += "CREATE TABLE {METADATA_CATALOG}.ducklake_deletion_macro(branch_id BIGINT, ancestor_branch_id BIGINT, "
		          "object_id BIGINT, deleted_at_snapshot BIGINT);\n";
		result +=
		    "CREATE TABLE {METADATA_CATALOG}.ducklake_deletion_partition(branch_id BIGINT, ancestor_branch_id BIGINT, "
		    "object_id BIGINT, deleted_at_snapshot BIGINT);\n";
		result += "INSERT INTO {METADATA_CATALOG}.ducklake_branch_lineage VALUES (0, 0, 9223372036854775807);\n";
		result +=
		    "INSERT INTO {METADATA_CATALOG}.ducklake_ref VALUES (0, 'main', 'branch', 0, NULL, 'active', NOW());\n";
	}
	return result;
}

template <typename Base>
string DuckLakeMetadataManagerV1_1<Base>::GetVersionString() {
	return DuckLakeVersionToString(Base::transaction.GetCatalog().GetDuckLakeVersion());
}

// explicit instantiations for all backends
template class DuckLakeMetadataManagerV1_1<DuckLakeMetadataManager>;
template class DuckLakeMetadataManagerV1_1<SQLiteMetadataManager>;
template class DuckLakeMetadataManagerV1_1<PostgresMetadataManager>;
template class DuckLakeMetadataManagerV1_1<QuackMetadataManager>;

} // namespace duckdb
