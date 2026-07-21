#include "functions/ducklake_table_functions.hpp"
#include "storage/ducklake_transaction.hpp"
#include "storage/ducklake_catalog.hpp"
#include "duckdb/planner/tableref/bound_at_clause.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

static void EnsureSupportsRefs(Catalog &catalog) {
	auto &ducklake_catalog = catalog.Cast<DuckLakeCatalog>();
	if (!ducklake_catalog.SupportsRefs()) {
		throw InvalidInputException(
		    "Named refs (branches/tags) require DuckLake catalog version >= 1.1-dev2. "
		    "Re-ATTACH with AUTOMATIC_MIGRATION TRUE to upgrade.");
	}
}

static idx_t ResolveSnapshotForRef(ClientContext &context, Catalog &catalog, TableFunctionBindInput &input) {
	auto &transaction = DuckLakeTransaction::Get(context, catalog);
	auto version_entry = input.named_parameters.find("snapshot_version");
	auto time_entry = input.named_parameters.find("snapshot_time");
	if (version_entry != input.named_parameters.end() && time_entry != input.named_parameters.end()) {
		throw BinderException("Cannot specify both snapshot_version and snapshot_time");
	}
	if (version_entry != input.named_parameters.end() && !version_entry->second.IsNull()) {
		BoundAtClause at("version", version_entry->second.DefaultCastAs(LogicalType::BIGINT));
		return transaction.GetSnapshot(&at).snapshot_id;
	}
	if (time_entry != input.named_parameters.end() && !time_entry->second.IsNull()) {
		BoundAtClause at("timestamp", time_entry->second.DefaultCastAs(LogicalType::TIMESTAMP_TZ));
		return transaction.GetSnapshot(&at).snapshot_id;
	}
	return transaction.GetSnapshot().snapshot_id;
}

//===--------------------------------------------------------------------===//
// ducklake_create_branch / ducklake_create_tag
//===--------------------------------------------------------------------===//
struct CreateRefBindData : public TableFunctionData {
	CreateRefBindData(Catalog &catalog, string name_p, string type_p, idx_t snapshot_id_p)
	    : catalog(catalog), ref_name(std::move(name_p)), ref_type(std::move(type_p)), snapshot_id(snapshot_id_p) {
	}
	Catalog &catalog;
	string ref_name;
	string ref_type;
	idx_t snapshot_id;
};

struct CreateRefState : public GlobalTableFunctionState {
	bool finished = false;
};

static unique_ptr<GlobalTableFunctionState> CreateRefInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<CreateRefState>();
}

static unique_ptr<FunctionData> CreateBranchBind(ClientContext &context, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<string> &names) {
	auto &catalog = DuckLakeBaseMetadataFunction::GetCatalog(context, input.inputs[0]);
	EnsureSupportsRefs(catalog);
	return_types.push_back(LogicalType::BOOLEAN);
	names.push_back("Success");
	auto snapshot_id = ResolveSnapshotForRef(context, catalog, input);
	return make_uniq<CreateRefBindData>(catalog, StringValue::Get(input.inputs[1]), "branch", snapshot_id);
}

static unique_ptr<FunctionData> CreateTagBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
	auto &catalog = DuckLakeBaseMetadataFunction::GetCatalog(context, input.inputs[0]);
	EnsureSupportsRefs(catalog);
	return_types.push_back(LogicalType::BOOLEAN);
	names.push_back("Success");
	auto snapshot_id = ResolveSnapshotForRef(context, catalog, input);
	return make_uniq<CreateRefBindData>(catalog, StringValue::Get(input.inputs[1]), "tag", snapshot_id);
}

static void CreateRefExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<CreateRefState>();
	auto &bind_data = data_p.bind_data->Cast<CreateRefBindData>();
	if (state.finished) {
		return;
	}
	auto &transaction = DuckLakeTransaction::Get(context, bind_data.catalog);
	optional_idx parent_ref_id;
	if (bind_data.ref_type == "branch" &&
	    bind_data.catalog.Cast<DuckLakeCatalog>().SupportsWritableBranches()) {
		// Fork from the currently active branch (or main).
		parent_ref_id = transaction.GetActiveBranchId();
	}
	transaction.GetMetadataManager().CreateRef(bind_data.ref_name, bind_data.ref_type, bind_data.snapshot_id,
	                                           parent_ref_id);
	state.finished = true;
	output.data[0].Append(Value::BOOLEAN(true));
	output.SetChildCardinality(1);
}

DuckLakeCreateBranchFunction::DuckLakeCreateBranchFunction()
    : TableFunction("ducklake_create_branch", {LogicalType::VARCHAR, LogicalType::VARCHAR}, CreateRefExecute,
                    CreateBranchBind, CreateRefInit) {
	named_parameters["snapshot_version"] = LogicalType::BIGINT;
	named_parameters["snapshot_time"] = LogicalType::TIMESTAMP_TZ;
}

DuckLakeCreateTagFunction::DuckLakeCreateTagFunction()
    : TableFunction("ducklake_create_tag", {LogicalType::VARCHAR, LogicalType::VARCHAR}, CreateRefExecute, CreateTagBind,
                    CreateRefInit) {
	named_parameters["snapshot_version"] = LogicalType::BIGINT;
	named_parameters["snapshot_time"] = LogicalType::TIMESTAMP_TZ;
}

//===--------------------------------------------------------------------===//
// ducklake_drop_branch / ducklake_drop_tag
//===--------------------------------------------------------------------===//
struct DropRefBindData : public TableFunctionData {
	DropRefBindData(Catalog &catalog, string name_p, string type_p)
	    : catalog(catalog), ref_name(std::move(name_p)), ref_type(std::move(type_p)) {
	}
	Catalog &catalog;
	string ref_name;
	string ref_type;
};

struct DropRefState : public GlobalTableFunctionState {
	bool finished = false;
};

static unique_ptr<GlobalTableFunctionState> DropRefInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<DropRefState>();
}

static unique_ptr<FunctionData> DropBranchBind(ClientContext &context, TableFunctionBindInput &input,
                                               vector<LogicalType> &return_types, vector<string> &names) {
	auto &catalog = DuckLakeBaseMetadataFunction::GetCatalog(context, input.inputs[0]);
	EnsureSupportsRefs(catalog);
	return_types.push_back(LogicalType::BOOLEAN);
	names.push_back("Success");
	return make_uniq<DropRefBindData>(catalog, StringValue::Get(input.inputs[1]), "branch");
}

static unique_ptr<FunctionData> DropTagBind(ClientContext &context, TableFunctionBindInput &input,
                                            vector<LogicalType> &return_types, vector<string> &names) {
	auto &catalog = DuckLakeBaseMetadataFunction::GetCatalog(context, input.inputs[0]);
	EnsureSupportsRefs(catalog);
	return_types.push_back(LogicalType::BOOLEAN);
	names.push_back("Success");
	return make_uniq<DropRefBindData>(catalog, StringValue::Get(input.inputs[1]), "tag");
}

static void DropRefExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<DropRefState>();
	auto &bind_data = data_p.bind_data->Cast<DropRefBindData>();
	if (state.finished) {
		return;
	}
	auto &transaction = DuckLakeTransaction::Get(context, bind_data.catalog);
	transaction.GetMetadataManager().DropRef(bind_data.ref_name, bind_data.ref_type);
	state.finished = true;
	output.data[0].Append(Value::BOOLEAN(true));
	output.SetChildCardinality(1);
}

DuckLakeDropBranchFunction::DuckLakeDropBranchFunction()
    : TableFunction("ducklake_drop_branch", {LogicalType::VARCHAR, LogicalType::VARCHAR}, DropRefExecute, DropBranchBind,
                    DropRefInit) {
}

DuckLakeDropTagFunction::DuckLakeDropTagFunction()
    : TableFunction("ducklake_drop_tag", {LogicalType::VARCHAR, LogicalType::VARCHAR}, DropRefExecute, DropTagBind,
                    DropRefInit) {
}

//===--------------------------------------------------------------------===//
// ducklake_refs
//===--------------------------------------------------------------------===//
static unique_ptr<FunctionData> DuckLakeRefsBind(ClientContext &context, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<string> &names) {
	auto &catalog = DuckLakeBaseMetadataFunction::GetCatalog(context, input.inputs[0]);
	EnsureSupportsRefs(catalog);
	names.emplace_back("ref_name");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("ref_type");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("snapshot_id");
	return_types.emplace_back(LogicalType::UBIGINT);
	names.emplace_back("created_at");
	return_types.emplace_back(LogicalType::TIMESTAMP_TZ);

	auto result = make_uniq<MetadataBindData>();
	auto &transaction = DuckLakeTransaction::Get(context, catalog);
	auto refs = transaction.GetMetadataManager().GetRefs();
	for (auto &ref : refs) {
		vector<Value> row;
		row.emplace_back(ref.ref_name);
		row.emplace_back(ref.ref_type);
		row.push_back(Value::UBIGINT(ref.snapshot_id));
		row.push_back(Value::TIMESTAMPTZ(ref.created_at));
		result->rows.push_back(std::move(row));
	}
	return std::move(result);
}

DuckLakeRefsFunction::DuckLakeRefsFunction() : DuckLakeBaseMetadataFunction("ducklake_refs", DuckLakeRefsBind) {
}

//===--------------------------------------------------------------------===//
// ducklake_use_branch (Phase 2)
//===--------------------------------------------------------------------===//
struct UseBranchBindData : public TableFunctionData {
	UseBranchBindData(Catalog &catalog, string name_p) : catalog(catalog), branch_name(std::move(name_p)) {
	}
	Catalog &catalog;
	string branch_name;
};

struct UseBranchState : public GlobalTableFunctionState {
	bool finished = false;
};

static unique_ptr<GlobalTableFunctionState> UseBranchInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<UseBranchState>();
}

static unique_ptr<FunctionData> UseBranchBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
	auto &catalog = DuckLakeBaseMetadataFunction::GetCatalog(context, input.inputs[0]);
	auto &ducklake_catalog = catalog.Cast<DuckLakeCatalog>();
	if (!ducklake_catalog.SupportsWritableBranches()) {
		throw InvalidInputException(
		    "ducklake_use_branch requires DuckLake catalog version >= 1.1-dev3. "
		    "Re-ATTACH with AUTOMATIC_MIGRATION TRUE to upgrade.");
	}
	return_types.push_back(LogicalType::BOOLEAN);
	names.push_back("Success");
	return make_uniq<UseBranchBindData>(catalog, StringValue::Get(input.inputs[1]));
}

static void UseBranchExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<UseBranchState>();
	auto &bind_data = data_p.bind_data->Cast<UseBranchBindData>();
	if (state.finished) {
		return;
	}
	auto &transaction = DuckLakeTransaction::Get(context, bind_data.catalog);
	DuckLakeRefInfo ref;
	if (!transaction.GetMetadataManager().TryResolveRef(bind_data.branch_name, "branch", ref)) {
		throw InvalidInputException("No branch named \"%s\" exists", bind_data.branch_name);
	}
	transaction.SetActiveBranch(ref.ref_id, ref.ref_name, ref.snapshot_id);
	state.finished = true;
	output.data[0].Append(Value::BOOLEAN(true));
	output.SetChildCardinality(1);
}

DuckLakeUseBranchFunction::DuckLakeUseBranchFunction()
    : TableFunction("ducklake_use_branch", {LogicalType::VARCHAR, LogicalType::VARCHAR}, UseBranchExecute, UseBranchBind,
                    UseBranchInit) {
}

} // namespace duckdb
