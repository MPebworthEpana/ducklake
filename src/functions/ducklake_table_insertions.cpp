#include "functions/ducklake_table_functions.hpp"
#include "storage/ducklake_transaction.hpp"
#include "common/ducklake_util.hpp"
#include "storage/ducklake_transaction_changes.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "storage/ducklake_scan.hpp"
#include "duckdb/planner/tableref/bound_at_clause.hpp"
#include "storage/ducklake_metadata_manager.hpp"
#include "storage/ducklake_metadata_info.hpp"

namespace duckdb {

string GetTableName(const Value &input) {
	if (input.IsNull()) {
		throw BinderException("Table cannot be NULL");
	}
	return input.GetValue<string>();
}

TableCatalogEntry &GetTableEntry(ClientContext &context, Catalog &catalog, const EntryLookupInfo &lookup,
                                 const Value &schema) {
	if (schema.IsNull()) {
		throw BinderException("Schema cannot be NULL");
	}
	auto schema_name = schema.GetValue<string>();
	auto entry = catalog.GetEntry(context, Identifier(schema_name), lookup, OnEntryNotFound::THROW_EXCEPTION);
	if (entry->type != CatalogType::TABLE_ENTRY) {
		throw BinderException("\"%s\" is a %s, not a table. Data change feed functions only support tables.",
		                      lookup.GetEntryName(), CatalogTypeToString(entry->type));
	}
	return entry->Cast<TableCatalogEntry>();
}

BoundAtClause AtClauseFromValue(const Value &input) {
	if (input.IsNull()) {
		throw BinderException("Snapshot identifier cannot be NULL");
	}
	switch (input.type().id()) {
	case LogicalTypeId::BIGINT:
		return BoundAtClause("version", input);
	case LogicalTypeId::TIMESTAMP_TZ:
		return BoundAtClause("timestamp", input);
	default:
		throw InternalException("Unsupported type for At Clause");
	}
}

struct RefBoundInfo {
	DuckLakeRefInfo ref;
	string unit;
};

struct SnapshotBoundArg {
	unique_ptr<BoundAtClause> at_clause;
	//! Lineage branch for merge-base when the bound is a named ref (branch, or tag→owning branch).
	optional_idx branch_id;
};

static RefBoundInfo ResolveRefBound(DuckLakeMetadataManager &manager, const Value &input) {
	if (input.IsNull()) {
		throw BinderException("Snapshot ref cannot be NULL");
	}
	auto ref_name = input.GetValue<string>();
	RefBoundInfo result;
	if (manager.TryResolveRef(ref_name, "branch", result.ref)) {
		result.unit = "branch";
		return result;
	}
	if (manager.TryResolveRef(ref_name, "tag", result.ref)) {
		result.unit = "tag";
		return result;
	}
	throw BinderException("No branch or tag named \"%s\" exists", ref_name);
}

static SnapshotBoundArg ResolveSnapshotBound(DuckLakeMetadataManager &manager, const Value &input) {
	SnapshotBoundArg result;
	if (input.IsNull()) {
		throw BinderException("Snapshot identifier cannot be NULL");
	}
	switch (input.type().id()) {
	case LogicalTypeId::VARCHAR: {
		auto ref = ResolveRefBound(manager, input);
		if (ref.unit == "branch") {
			result.at_clause = make_uniq<BoundAtClause>("branch", input);
			result.branch_id = ref.ref.ref_id;
		} else {
			// Tags pin an exact snapshot. Resolve via version so the end scan picks up the
			// tagged snapshot's owning branch_id (global AT TAG still treats tags as main pins).
			result.at_clause =
			    make_uniq<BoundAtClause>("version", Value::BIGINT(NumericCast<int64_t>(ref.ref.snapshot_id)));
			auto tagged = manager.GetSnapshot(*result.at_clause, SnapshotBound::LOWER_BOUND);
			result.branch_id = tagged->branch_id;
		}
		return result;
	}
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::TIMESTAMP_TZ:
		result.at_clause = make_uniq<BoundAtClause>(AtClauseFromValue(input));
		return result;
	default:
		throw BinderException("Snapshot bounds must be BIGINT, TIMESTAMP WITH TIME ZONE, or a branch/tag name");
	}
}

static unique_ptr<FunctionData> DuckLakeTableChangesBind(ClientContext &context, TableFunctionBindInput &input,
                                                         vector<LogicalType> &return_types, vector<string> &names,
                                                         DuckLakeScanType scan_type) {
	auto &catalog = DuckLakeBaseMetadataFunction::GetCatalog(context, input.inputs[0]);
	auto &transaction = DuckLakeTransaction::Get(context, catalog);
	auto &metadata_manager = transaction.GetMetadataManager();

	auto start_bound = ResolveSnapshotBound(metadata_manager, input.inputs[3]);
	auto end_bound = ResolveSnapshotBound(metadata_manager, input.inputs[4]);

	unique_ptr<BoundAtClause> start_at_clause = std::move(start_bound.at_clause);
	if (start_bound.branch_id.IsValid() && end_bound.branch_id.IsValid() &&
	    start_bound.branch_id.GetIndex() != end_bound.branch_id.GetIndex()) {
		auto ancestor =
		    metadata_manager.GetMergeBaseSnapshot(end_bound.branch_id.GetIndex(), start_bound.branch_id.GetIndex());
		start_at_clause = make_uniq<BoundAtClause>("version", Value::BIGINT(NumericCast<int64_t>(ancestor)));
	}

	auto table_name = GetTableName(input.inputs[2]);
	EntryLookupInfo lookup(CatalogType::TABLE_ENTRY, Identifier(table_name), *end_bound.at_clause, QueryErrorContext());
	auto &table = GetTableEntry(context, catalog, lookup, input.inputs[1]);

	unique_ptr<FunctionData> bind_data;
	input.table_function = table.GetScanFunction(context, bind_data, lookup);

	auto &function_info = input.table_function.function_info->Cast<DuckLakeFunctionInfo>();
	names = function_info.column_names;
	return_types = function_info.column_types;
	function_info.start_snapshot =
	    make_uniq<DuckLakeSnapshot>(transaction.GetSnapshot(*start_at_clause, SnapshotBound::LOWER_BOUND));
	function_info.scan_type = scan_type;
	return bind_data;
}

static unique_ptr<FunctionData> DuckLakeTableInsertionsBind(ClientContext &context, TableFunctionBindInput &input,
                                                            vector<LogicalType> &return_types, vector<string> &names) {
	return DuckLakeTableChangesBind(context, input, return_types, names, DuckLakeScanType::SCAN_INSERTIONS);
}

static unique_ptr<FunctionData> DuckLakeTableDeletionsBind(ClientContext &context, TableFunctionBindInput &input,
                                                           vector<LogicalType> &return_types, vector<string> &names) {
	return DuckLakeTableChangesBind(context, input, return_types, names, DuckLakeScanType::SCAN_DELETIONS);
}

static unique_ptr<GlobalTableFunctionState> DuckLakeChangesInit(ClientContext &context, TableFunctionInitInput &input) {
	throw InternalException("DuckLakeChangesInit should never be called");
}

static void DuckLakeChangesExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	throw InternalException("DuckLakeChangesExecute should never be called");
}

static void AddTableChangesOverloads(TableFunctionSet &set, table_function_t execute, table_function_bind_t bind,
                                     table_function_init_global_t init) {
	vector<LogicalType> snapshot_types {LogicalType::BIGINT, LogicalType::TIMESTAMP_TZ, LogicalType::VARCHAR};
	for (auto &start_type : snapshot_types) {
		for (auto &end_type : snapshot_types) {
			set.AddFunction(TableFunction(
			    {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, start_type, end_type}, execute, bind,
			    init));
		}
	}
}

TableFunctionSet DuckLakeTableInsertionsFunction::GetFunctions() {
	TableFunctionSet set("ducklake_table_insertions");
	AddTableChangesOverloads(set, DuckLakeChangesExecute, DuckLakeTableInsertionsBind, DuckLakeChangesInit);
	return set;
}

TableFunctionSet DuckLakeTableDeletionsFunction::GetFunctions() {
	TableFunctionSet set("ducklake_table_deletions");
	AddTableChangesOverloads(set, DuckLakeChangesExecute, DuckLakeTableDeletionsBind, DuckLakeChangesInit);
	return set;
}
} // namespace duckdb
