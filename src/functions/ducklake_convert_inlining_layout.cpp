#include "functions/ducklake_table_functions.hpp"
#include "storage/ducklake_transaction.hpp"
#include "storage/ducklake_catalog.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

struct ConvertInliningLayoutBindData : public TableFunctionData {
	ConvertInliningLayoutBindData(Catalog &catalog, string target_layout, bool dry_run)
	    : catalog(catalog), target_layout(std::move(target_layout)), dry_run(dry_run) {
	}

	Catalog &catalog;
	string target_layout;
	bool dry_run;
	DuckLakeConvertInliningLayoutResult result;
	bool computed = false;
};

struct ConvertInliningLayoutState : public GlobalTableFunctionState {
	idx_t offset = 0;
};

static unique_ptr<GlobalTableFunctionState> ConvertInliningLayoutInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<ConvertInliningLayoutState>();
}

static unique_ptr<FunctionData> ConvertInliningLayoutBind(ClientContext &context, TableFunctionBindInput &input,
                                                          vector<LogicalType> &return_types, vector<string> &names) {
	auto &catalog = DuckLakeBaseMetadataFunction::GetCatalog(context, input.inputs[0]);
	auto &ducklake_catalog = catalog.Cast<DuckLakeCatalog>();
	if (!ducklake_catalog.SupportsWritableBranches()) {
		throw InvalidInputException(
		    "ducklake_convert_inlining_layout requires DuckLake catalog version >= 1.1-dev3. "
		    "Re-ATTACH with AUTOMATIC_MIGRATION TRUE to upgrade.");
	}

	names.emplace_back("source_layout");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("target_layout");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("dry_run");
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("message");
	return_types.emplace_back(LogicalType::VARCHAR);

	bool dry_run = false;
	auto dry_entry = input.named_parameters.find("dry_run");
	if (dry_entry != input.named_parameters.end() && !dry_entry->second.IsNull()) {
		dry_run = BooleanValue::Get(dry_entry->second);
	}
	return make_uniq<ConvertInliningLayoutBindData>(catalog, StringValue::Get(input.inputs[1]), dry_run);
}

static void ConvertInliningLayoutExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->CastNoConst<ConvertInliningLayoutBindData>();
	auto &state = data_p.global_state->Cast<ConvertInliningLayoutState>();
	if (!bind_data.computed) {
		auto &transaction = DuckLakeTransaction::Get(context, bind_data.catalog);
		bind_data.result =
		    transaction.GetMetadataManager().ConvertInliningLayout(bind_data.target_layout, bind_data.dry_run);
		bind_data.computed = true;
	}

	auto &result = bind_data.result;
	if (result.messages.empty()) {
		result.messages.push_back("no inlined data tables to convert");
	}
	if (state.offset >= result.messages.size()) {
		return;
	}

	idx_t count = 0;
	while (state.offset < result.messages.size() && count < STANDARD_VECTOR_SIZE) {
		output.data[0].Append(Value(result.source_layout));
		output.data[1].Append(Value(result.target_layout));
		output.data[2].Append(Value::BOOLEAN(result.dry_run));
		output.data[3].Append(Value(result.messages[state.offset++]));
		count++;
	}
	output.SetChildCardinality(count);
}

DuckLakeConvertInliningLayoutFunction::DuckLakeConvertInliningLayoutFunction()
    : TableFunction("ducklake_convert_inlining_layout", {LogicalType::VARCHAR, LogicalType::VARCHAR},
                    ConvertInliningLayoutExecute, ConvertInliningLayoutBind, ConvertInliningLayoutInit) {
	named_parameters["dry_run"] = LogicalType::BOOLEAN;
}

} // namespace duckdb
