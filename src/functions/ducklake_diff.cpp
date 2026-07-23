#include "functions/ducklake_table_functions.hpp"
#include "storage/ducklake_transaction.hpp"
#include "storage/ducklake_catalog.hpp"

namespace duckdb {

struct DiffBindData : public TableFunctionData {
	DiffBindData(Catalog &catalog, string ref_a_p, string ref_b_p)
	    : catalog(catalog), ref_a(std::move(ref_a_p)), ref_b(std::move(ref_b_p)) {
	}

	Catalog &catalog;
	string ref_a;
	string ref_b;
	vector<DuckLakeDiffResult> results;
	bool computed = false;
};

struct DiffState : public GlobalTableFunctionState {
	idx_t offset = 0;
};

static unique_ptr<GlobalTableFunctionState> DiffInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<DiffState>();
}

static unique_ptr<FunctionData> DiffBind(ClientContext &context, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, vector<string> &names) {
	auto &catalog = DuckLakeBaseMetadataFunction::GetCatalog(context, input.inputs[0]);
	auto &ducklake_catalog = catalog.Cast<DuckLakeCatalog>();
	if (!ducklake_catalog.SupportsWritableBranches()) {
		throw InvalidInputException(
		    "ducklake_diff requires DuckLake catalog version >= 1.1-dev3. "
		    "Re-ATTACH with AUTOMATIC_MIGRATION TRUE to upgrade.");
	}

	names.emplace_back("object_type");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("schema_name");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("object_name");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("change");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("detail");
	return_types.emplace_back(LogicalType::VARCHAR);

	string ref_a = StringValue::Get(input.inputs[1]);
	string ref_b = StringValue::Get(input.inputs[2]);
	return make_uniq<DiffBindData>(catalog, std::move(ref_a), std::move(ref_b));
}

static void DiffExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->CastNoConst<DiffBindData>();
	auto &state = data_p.global_state->Cast<DiffState>();
	if (!bind_data.computed) {
		auto &transaction = DuckLakeTransaction::Get(context, bind_data.catalog);
		bind_data.results = transaction.GetMetadataManager().DiffRefs(bind_data.ref_a, bind_data.ref_b);
		bind_data.computed = true;
	}
	if (state.offset >= bind_data.results.size()) {
		return;
	}

	idx_t count = 0;
	while (state.offset < bind_data.results.size() && count < STANDARD_VECTOR_SIZE) {
		auto &result = bind_data.results[state.offset++];
		output.data[0].Append(Value(result.object_type));
		output.data[1].Append(Value(result.schema_name));
		output.data[2].Append(Value(result.object_name));
		output.data[3].Append(Value(result.change));
		output.data[4].Append(Value(result.detail));
		count++;
	}
	output.SetChildCardinality(count);
}

DuckLakeDiffFunction::DuckLakeDiffFunction()
    : TableFunction("ducklake_diff", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
                    DiffExecute, DiffBind, DiffInit) {
}

} // namespace duckdb
