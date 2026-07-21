#include "functions/ducklake_table_functions.hpp"
#include "storage/ducklake_transaction.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_table_entry.hpp"
#include "common/ducklake_util.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

struct DuckLakeRequireUnchangedData final : public TableFunctionData {
	DuckLakeRequireUnchangedData(Catalog &catalog, CommitPrecondition precondition_p)
	    : catalog(catalog), precondition(std::move(precondition_p)) {
	}

	Catalog &catalog;
	CommitPrecondition precondition;
};

struct DuckLakeRequireUnchangedState final : public GlobalTableFunctionState {
	bool finished = false;
};

unique_ptr<GlobalTableFunctionState> DuckLakeRequireUnchangedInit(ClientContext &context,
                                                                  TableFunctionInitInput &input) {
	return make_uniq<DuckLakeRequireUnchangedState>();
}

static TableIndex ResolveTableName(ClientContext &context, Catalog &catalog, const string &qualified_name) {
	string schema_name = DEFAULT_SCHEMA;
	string table_name = qualified_name;
	auto dot = qualified_name.find('.');
	if (dot != string::npos) {
		schema_name = qualified_name.substr(0, dot);
		table_name = qualified_name.substr(dot + 1);
	}
	EntryLookupInfo table_lookup(CatalogType::TABLE_ENTRY, Identifier(table_name), nullptr, QueryErrorContext());
	auto table_entry = catalog.GetEntry(context, Identifier(schema_name), table_lookup, OnEntryNotFound::THROW_EXCEPTION);
	return table_entry->Cast<DuckLakeTableEntry>().GetTableId();
}

static unique_ptr<FunctionData> DuckLakeRequireUnchangedBind(ClientContext &context, TableFunctionBindInput &input,
                                                             vector<LogicalType> &return_types, vector<string> &names) {
	auto &catalog = DuckLakeBaseMetadataFunction::GetCatalog(context, input.inputs[0]);
	return_types.push_back(LogicalType::BOOLEAN);
	names.push_back("Success");

	CommitPrecondition precondition;
	auto since_entry = input.named_parameters.find("since_snapshot");
	if (since_entry == input.named_parameters.end() || since_entry->second.IsNull()) {
		throw BinderException("ducklake_require_unchanged requires since_snapshot => <snapshot_id>");
	}
	precondition.since_snapshot = since_entry->second.DefaultCastAs(LogicalType::UBIGINT).GetValue<idx_t>();

	auto tables_entry = input.named_parameters.find("tables");
	if (tables_entry != input.named_parameters.end() && !tables_entry->second.IsNull()) {
		for (auto &table_val : ListValue::GetChildren(tables_entry->second)) {
			if (table_val.IsNull()) {
				continue;
			}
			auto qualified = StringValue::Get(table_val);
			precondition.table_names.push_back(qualified);
			precondition.table_ids.push_back(ResolveTableName(context, catalog, qualified));
		}
		if (precondition.table_ids.empty()) {
			throw BinderException("ducklake_require_unchanged: tables list must contain at least one table name");
		}
	}
	return make_uniq<DuckLakeRequireUnchangedData>(catalog, std::move(precondition));
}

void DuckLakeRequireUnchangedExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<DuckLakeRequireUnchangedState>();
	auto &bind_data = data_p.bind_data->Cast<DuckLakeRequireUnchangedData>();
	if (state.finished) {
		return;
	}
	auto &transaction = DuckLakeTransaction::Get(context, bind_data.catalog);
	transaction.AddCommitPrecondition(bind_data.precondition);
	state.finished = true;
	output.data[0].Append(Value::BOOLEAN(true));
	output.SetChildCardinality(1);
}

DuckLakeRequireUnchangedFunction::DuckLakeRequireUnchangedFunction()
    : TableFunction("ducklake_require_unchanged", {LogicalType::VARCHAR}, DuckLakeRequireUnchangedExecute,
                    DuckLakeRequireUnchangedBind, DuckLakeRequireUnchangedInit) {
	named_parameters["since_snapshot"] = LogicalType::UBIGINT;
	named_parameters["tables"] = LogicalType::LIST(LogicalType::VARCHAR);
}

} // namespace duckdb
