//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/ducklake_transaction_changes.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/transaction/transaction.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/reference_map.hpp"
#include "common/ducklake_snapshot.hpp"
#include "common/index.hpp"
#include "duckdb/common/types/value_map.hpp"

namespace duckdb {
class CatalogEntry;
class DuckLakeSchemaEntry;

struct TransactionChangeInformation {
	case_insensitive_set_t created_schemas;
	map<SchemaIndex, reference<DuckLakeSchemaEntry>> dropped_schemas;
	case_insensitive_map_t<reference_set_t<CatalogEntry>> created_tables;
	case_insensitive_map_t<reference_set_t<CatalogEntry>> created_scalar_macros;
	case_insensitive_map_t<reference_set_t<CatalogEntry>> created_table_macros;

	set<TableIndex> altered_tables;
	set<TableIndex> altered_tables_with_schema_version_changes;
	set<TableIndex> altered_views;
	set<TableIndex> dropped_tables;
	set<TableIndex> dropped_views;
	set<MacroIndex> dropped_scalar_macros;
	set<MacroIndex> dropped_table_macros;
	set<TableIndex> tables_inserted_into;
	set<TableIndex> tables_deleted_from;
	set<TableIndex> tables_inserted_inlined;
	set<TableIndex> tables_deleted_inlined;
	set<TableIndex> tables_flushed_inlined;
	set<TableIndex> tables_compacted;
	set<TableIndex> tables_merge_adjacent;
	set<TableIndex> tables_rewrite_delete;
};

struct SnapshotChangeInformation {
	case_insensitive_set_t created_schemas;
	set<SchemaIndex> dropped_schemas;
	case_insensitive_map_t<case_insensitive_map_t<string>> created_tables;
	case_insensitive_map_t<case_insensitive_map_t<string>> created_scalar_macros;
	case_insensitive_map_t<case_insensitive_map_t<string>> created_table_macros;
	set<TableIndex> altered_tables;
	set<TableIndex> altered_views;
	set<TableIndex> dropped_tables;
	set<TableIndex> dropped_views;
	set<MacroIndex> dropped_scalar_macros;
	set<MacroIndex> dropped_table_macros;
	set<TableIndex> inserted_tables;
	set<TableIndex> tables_deleted_from;
	set<TableIndex> tables_compacted;
	set<TableIndex> tables_merge_adjacent;
	set<TableIndex> tables_rewrite_delete;
	set<TableIndex> tables_inserted_inlined;
	set<TableIndex> tables_deleted_inlined;
	set<TableIndex> tables_flushed_inlined;
	static SnapshotChangeInformation ParseChangesMade(const string &changes_made);
};

//! Merge `other` into `target` (set-union of all change categories).
void MergeSnapshotChangeInformation(SnapshotChangeInformation &target, const SnapshotChangeInformation &other);

//! Adapt transaction-local change info into the snapshot shape used by DetectConflicts (G3).
SnapshotChangeInformation FromTransactionChanges(const TransactionChangeInformation &changes);

//! Symmetric conflict detection for three-way merge (and reusable by OCC callers).
//! Returns human-readable conflict messages; empty means no conflicts.
//! Same-table appends on both sides are NOT conflicts (compose).
//! Overlapping same-table deletes are NOT reported here — callers that care about
//! delete-vs-delete must apply file-level intersection (merge) or OCC enrichment.
vector<string> DetectConflicts(const SnapshotChangeInformation &source_changes,
                               const SnapshotChangeInformation &target_changes);

} // namespace duckdb
