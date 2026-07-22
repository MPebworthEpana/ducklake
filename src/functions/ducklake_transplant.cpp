#include "functions/ducklake_table_functions.hpp"
#include "storage/ducklake_transaction.hpp"
#include "storage/ducklake_catalog.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

struct TransplantBindData : public TableFunctionData {
	TransplantBindData(Catalog &catalog, string source_p, idx_t start_snapshot_p, idx_t end_snapshot_p,
	                   string target_p, bool dry_run_p)
	    : catalog(catalog), source_branch(std::move(source_p)), start_snapshot(start_snapshot_p),
	      end_snapshot(end_snapshot_p), target_branch(std::move(target_p)), dry_run(dry_run_p) {
	}

	Catalog &catalog;
	string source_branch;
	idx_t start_snapshot;
	idx_t end_snapshot;
	string target_branch;
	bool dry_run;
	DuckLakeTransplantResult result;
	bool computed = false;
};

struct TransplantState : public GlobalTableFunctionState {
	idx_t offset = 0;
};

static unique_ptr<GlobalTableFunctionState> TransplantInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<TransplantState>();
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

static unique_ptr<FunctionData> TransplantBind(ClientContext &context, TableFunctionBindInput &input,
                                               vector<LogicalType> &return_types, vector<string> &names) {
	auto &catalog = DuckLakeBaseMetadataFunction::GetCatalog(context, input.inputs[0]);
	auto &ducklake_catalog = catalog.Cast<DuckLakeCatalog>();
	if (!ducklake_catalog.SupportsWritableBranches()) {
		throw InvalidInputException(
		    "ducklake_transplant requires DuckLake catalog version >= 1.1-dev3. "
		    "Re-ATTACH with AUTOMATIC_MIGRATION TRUE to upgrade.");
	}

	names.emplace_back("transplant_type");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("source_branch");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("target_branch");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("start_snapshot");
	return_types.emplace_back(LogicalType::UBIGINT);
	names.emplace_back("end_snapshot");
	return_types.emplace_back(LogicalType::UBIGINT);
	names.emplace_back("snapshots_applied");
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
	auto start_snapshot = input.inputs[2].DefaultCastAs(LogicalType::UBIGINT).GetValue<idx_t>();
	auto end_snapshot = input.inputs[3].DefaultCastAs(LogicalType::UBIGINT).GetValue<idx_t>();
	string target = ResolveTargetBranch(context, catalog, input);
	return make_uniq<TransplantBindData>(catalog, std::move(source), start_snapshot, end_snapshot, std::move(target),
	                                     dry_run);
}

static void TransplantExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->CastNoConst<TransplantBindData>();
	auto &state = data_p.global_state->Cast<TransplantState>();
	if (!bind_data.computed) {
		auto &transaction = DuckLakeTransaction::Get(context, bind_data.catalog);
		bind_data.result = transaction.GetMetadataManager().Transplant(
		    bind_data.source_branch, bind_data.start_snapshot, bind_data.end_snapshot, bind_data.target_branch,
		    bind_data.dry_run);
		bind_data.computed = true;
		if (!bind_data.dry_run && bind_data.result.transplant_type != "conflicts" && transaction.HasActiveBranch()) {
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
		result.messages.push_back(result.transplant_type);
	}
	if (state.offset >= result.messages.size()) {
		return;
	}

	idx_t count = 0;
	while (state.offset < result.messages.size() && count < STANDARD_VECTOR_SIZE) {
		output.data[0].Append(Value(result.transplant_type));
		output.data[1].Append(Value(result.source_branch));
		output.data[2].Append(Value(result.target_branch));
		output.data[3].Append(Value::UBIGINT(result.start_snapshot));
		output.data[4].Append(Value::UBIGINT(result.end_snapshot));
		output.data[5].Append(Value::UBIGINT(result.snapshots_applied));
		output.data[6].Append(Value::UBIGINT(result.target_head));
		output.data[7].Append(Value::UBIGINT(result.new_target_head));
		output.data[8].Append(Value::BOOLEAN(result.dry_run));
		output.data[9].Append(Value(result.messages[state.offset++]));
		count++;
	}
	output.SetChildCardinality(count);
}

DuckLakeTransplantFunction::DuckLakeTransplantFunction()
    : TableFunction("ducklake_transplant",
                    {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::UBIGINT, LogicalType::UBIGINT},
                    TransplantExecute, TransplantBind, TransplantInit) {
	named_parameters["target"] = LogicalType::VARCHAR;
	named_parameters["dry_run"] = LogicalType::BOOLEAN;
}

} // namespace duckdb
