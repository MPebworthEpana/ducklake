#include "functions/ducklake_table_functions.hpp"
#include "storage/ducklake_transaction.hpp"
#include "storage/ducklake_catalog.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

struct CherryPickBindData : public TableFunctionData {
	CherryPickBindData(Catalog &catalog, string source_p, idx_t snapshot_id_p, string target_p, bool dry_run_p)
	    : catalog(catalog), source_branch(std::move(source_p)), snapshot_id(snapshot_id_p),
	      target_branch(std::move(target_p)), dry_run(dry_run_p) {
	}

	Catalog &catalog;
	string source_branch;
	idx_t snapshot_id;
	string target_branch;
	bool dry_run;
	DuckLakeCherryPickResult result;
	bool computed = false;
};

struct CherryPickState : public GlobalTableFunctionState {
	idx_t offset = 0;
};

static unique_ptr<GlobalTableFunctionState> CherryPickInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<CherryPickState>();
}

static string ResolveTargetBranch(ClientContext &context, Catalog &catalog, TableFunctionBindInput &input) {
	auto target_entry = input.named_parameters.find("target");
	if (target_entry != input.named_parameters.end() && !target_entry->second.IsNull()) {
		return StringValue::Get(target_entry->second);
	}
	auto &ducklake_catalog = catalog.Cast<DuckLakeCatalog>();
	if (ducklake_catalog.HasSessionBranch(context)) {
		return ducklake_catalog.GetSessionBranchName(context);
	}
	return "main";
}

static unique_ptr<FunctionData> CherryPickBind(ClientContext &context, TableFunctionBindInput &input,
                                               vector<LogicalType> &return_types, vector<string> &names) {
	auto &catalog = DuckLakeBaseMetadataFunction::GetCatalog(context, input.inputs[0]);
	auto &ducklake_catalog = catalog.Cast<DuckLakeCatalog>();
	if (!ducklake_catalog.SupportsWritableBranches()) {
		throw InvalidInputException(
		    "ducklake_cherry_pick requires DuckLake catalog version >= 1.1-dev3. "
		    "Re-ATTACH with AUTOMATIC_MIGRATION TRUE to upgrade.");
	}

	names.emplace_back("cherry_pick_type");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("source_branch");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("target_branch");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("source_snapshot");
	return_types.emplace_back(LogicalType::UBIGINT);
	names.emplace_back("ancestor_snapshot");
	return_types.emplace_back(LogicalType::UBIGINT);
	names.emplace_back("source_head");
	return_types.emplace_back(LogicalType::UBIGINT);
	names.emplace_back("target_head");
	return_types.emplace_back(LogicalType::UBIGINT);
	names.emplace_back("new_target_head");
	return_types.emplace_back(LogicalType::UBIGINT);
	names.emplace_back("dry_run");
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("message");
	return_types.emplace_back(LogicalType::VARCHAR);

	bool dry_run = false;
	auto dry_entry = input.named_parameters.find("dry_run");
	if (dry_entry != input.named_parameters.end() && !dry_entry->second.IsNull()) {
		dry_run = BooleanValue::Get(dry_entry->second);
	}

	string source = StringValue::Get(input.inputs[1]);
	auto snapshot_id = input.inputs[2].DefaultCastAs(LogicalType::UBIGINT).GetValue<idx_t>();
	string target = ResolveTargetBranch(context, catalog, input);
	return make_uniq<CherryPickBindData>(catalog, std::move(source), snapshot_id, std::move(target), dry_run);
}

static void CherryPickExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->CastNoConst<CherryPickBindData>();
	auto &state = data_p.global_state->Cast<CherryPickState>();
	if (!bind_data.computed) {
		auto &transaction = DuckLakeTransaction::Get(context, bind_data.catalog);
		bind_data.result = transaction.GetMetadataManager().CherryPick(
		    bind_data.source_branch, bind_data.snapshot_id, bind_data.target_branch, bind_data.dry_run);
		bind_data.computed = true;
		// Advance the session branch head so subsequent reads see post-pick state.
		if (!bind_data.dry_run && bind_data.result.cherry_pick_type != "conflicts" && transaction.HasActiveBranch()) {
			auto active = transaction.GetActiveBranchId();
			if (active == bind_data.result.target_branch_id || active == bind_data.result.source_branch_id) {
				transaction.SetActiveBranch(active,
				                           active == bind_data.result.target_branch_id
				                               ? bind_data.result.target_branch
				                               : bind_data.result.source_branch,
				                           active == bind_data.result.target_branch_id
				                               ? bind_data.result.new_target_head
				                               : bind_data.result.source_head);
			}
		}
	}

	auto &result = bind_data.result;
	if (result.messages.empty()) {
		result.messages.push_back(result.cherry_pick_type);
	}
	if (state.offset >= result.messages.size()) {
		return;
	}

	idx_t count = 0;
	while (state.offset < result.messages.size() && count < STANDARD_VECTOR_SIZE) {
		output.data[0].Append(Value(result.cherry_pick_type));
		output.data[1].Append(Value(result.source_branch));
		output.data[2].Append(Value(result.target_branch));
		output.data[3].Append(Value::UBIGINT(result.source_snapshot));
		output.data[4].Append(Value::UBIGINT(result.ancestor_snapshot));
		output.data[5].Append(Value::UBIGINT(result.source_head));
		output.data[6].Append(Value::UBIGINT(result.target_head));
		output.data[7].Append(Value::UBIGINT(result.new_target_head));
		output.data[8].Append(Value::BOOLEAN(result.dry_run));
		output.data[9].Append(Value(result.messages[state.offset++]));
		count++;
	}
	output.SetChildCardinality(count);
}

DuckLakeCherryPickFunction::DuckLakeCherryPickFunction()
    : TableFunction("ducklake_cherry_pick",
                    {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::UBIGINT}, CherryPickExecute,
                    CherryPickBind, CherryPickInit) {
	named_parameters["target"] = LogicalType::VARCHAR;
	named_parameters["dry_run"] = LogicalType::BOOLEAN;
}

} // namespace duckdb
