#include "storage/ducklake_transaction_changes.hpp"
#include "common/ducklake_util.hpp"
#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/common/enums/catalog_type.hpp"

namespace duckdb {

namespace {

enum class ChangeType {
	CREATED_TABLE,
	CREATED_VIEW,
	CREATED_SCHEMA,
	DROPPED_SCHEMA,
	DROPPED_TABLE,
	DROPPED_VIEW,
	INSERTED_INTO_TABLE,
	DELETED_FROM_TABLE,
	INSERTED_INTO_TABLE_INLINED,
	DELETED_FROM_TABLE_INLINED,
	FLUSHED_INLINE_DATA_FOR_TABLE,
	ALTERED_TABLE,
	ALTERED_VIEW,
	COMPACTED_TABLE,
	MERGE_ADJACENT,
	REWRITE_DELETE,
	CREATED_SCALAR_MACRO,
	CREATED_TABLE_MACRO,
	DROPPED_SCALAR_MACRO,
	DROPPED_TABLE_MACRO,
	CREATED_TYPE,
	DROPPED_TYPE,
	MERGED_BRANCH
};

struct ParsedChange {
	ChangeType change_type;
	string change_value;
};

ChangeType ParseChangeType(const string &changes_made, idx_t &pos) {
	idx_t start_pos = pos;
	for (; pos < changes_made.size(); pos++) {
		if (changes_made[pos] == ':') {
			break;
		}
	}
	auto change_type_str = changes_made.substr(start_pos, pos - start_pos);
	if (StringUtil::CIEquals(change_type_str, "created_table")) {
		return ChangeType::CREATED_TABLE;
	} else if (StringUtil::CIEquals(change_type_str, "created_view")) {
		return ChangeType::CREATED_VIEW;
	} else if (StringUtil::CIEquals(change_type_str, "created_scalar_macro")) {
		return ChangeType::CREATED_SCALAR_MACRO;
	} else if (StringUtil::CIEquals(change_type_str, "created_table_macro")) {
		return ChangeType::CREATED_TABLE_MACRO;
	} else if (StringUtil::CIEquals(change_type_str, "created_schema")) {
		return ChangeType::CREATED_SCHEMA;
	} else if (StringUtil::CIEquals(change_type_str, "dropped_schema")) {
		return ChangeType::DROPPED_SCHEMA;
	} else if (StringUtil::CIEquals(change_type_str, "dropped_table")) {
		return ChangeType::DROPPED_TABLE;
	} else if (StringUtil::CIEquals(change_type_str, "dropped_view")) {
		return ChangeType::DROPPED_VIEW;
	} else if (StringUtil::CIEquals(change_type_str, "inserted_into_table")) {
		return ChangeType::INSERTED_INTO_TABLE;
	} else if (StringUtil::CIEquals(change_type_str, "dropped_scalar_macro")) {
		return ChangeType::DROPPED_SCALAR_MACRO;
	} else if (StringUtil::CIEquals(change_type_str, "dropped_table_macro")) {
		return ChangeType::DROPPED_TABLE_MACRO;
	} else if (StringUtil::CIEquals(change_type_str, "altered_table")) {
		return ChangeType::ALTERED_TABLE;
	} else if (StringUtil::CIEquals(change_type_str, "altered_view")) {
		return ChangeType::ALTERED_VIEW;
	} else if (StringUtil::CIEquals(change_type_str, "deleted_from_table")) {
		return ChangeType::DELETED_FROM_TABLE;
	} else if (StringUtil::CIEquals(change_type_str, "compacted_table")) {
		return ChangeType::COMPACTED_TABLE;
	} else if (StringUtil::CIEquals(change_type_str, "merge_adjacent")) {
		return ChangeType::MERGE_ADJACENT;
	} else if (StringUtil::CIEquals(change_type_str, "rewrite_delete")) {
		return ChangeType::REWRITE_DELETE;
	} else if (StringUtil::CIEquals(change_type_str, "inlined_insert")) {
		return ChangeType::INSERTED_INTO_TABLE_INLINED;
	} else if (StringUtil::CIEquals(change_type_str, "inlined_delete")) {
		return ChangeType::DELETED_FROM_TABLE_INLINED;
	} else if (StringUtil::CIEquals(change_type_str, "flushed_inlined") ||
	           StringUtil::CIEquals(change_type_str, "inline_flush")) {
		return ChangeType::FLUSHED_INLINE_DATA_FOR_TABLE;
	} else if (StringUtil::CIEquals(change_type_str, "merged_branch")) {
		return ChangeType::MERGED_BRANCH;
	} else if (StringUtil::CIEquals(change_type_str, "created_type")) {
		return ChangeType::CREATED_TYPE;
	} else if (StringUtil::CIEquals(change_type_str, "dropped_type")) {
		return ChangeType::DROPPED_TYPE;
	} else {
		throw InvalidInputException("Unsupported change type %s", change_type_str);
	}
}

string ParseChangeValue(const string &changes_made, idx_t &pos) {
	// parse until we find an unquoted comma
	bool in_quotes = false;
	idx_t start_pos = pos;
	for (; pos < changes_made.size(); pos++) {
		if (!in_quotes && changes_made[pos] == ',') {
			// found an unquoted comma
			break;
		}
		if (changes_made[pos] == '"') {
			in_quotes = !in_quotes;
		}
	}
	return changes_made.substr(start_pos, pos - start_pos);
}

ParsedChange ParseChangeEntry(const string &changes_made, idx_t &pos) {
	ParsedChange info;
	info.change_type = ParseChangeType(changes_made, pos);
	if (pos >= changes_made.size() || changes_made[pos] != ':') {
		throw InvalidInputException("Expected a colon after the change type");
	}
	pos++;
	info.change_value = ParseChangeValue(changes_made, pos);
	return info;
}

vector<ParsedChange> ParseChangesList(const string &changes_made) {
	vector<ParsedChange> result;
	idx_t pos = 0;
	while (pos < changes_made.size()) {
		result.push_back(ParseChangeEntry(changes_made, pos));
		if (pos >= changes_made.size()) {
			break;
		}
		if (changes_made[pos] != ',') {
			throw InvalidInputException("Expected a comma separating the change entry");
		}
		pos++;
	}
	return result;
}

} // namespace

SnapshotChangeInformation SnapshotChangeInformation::ParseChangesMade(const string &changes_made) {
	auto change_list = ParseChangesList(changes_made);

	SnapshotChangeInformation result;
	for (auto &entry : change_list) {
		switch (entry.change_type) {
		case ChangeType::CREATED_TABLE: {
			auto catalog_value = DuckLakeUtil::ParseCatalogEntry(entry.change_value);
			result.created_tables[catalog_value.schema].insert(make_pair(std::move(catalog_value.name), "table"));
			break;
		}
		case ChangeType::CREATED_SCALAR_MACRO: {
			auto catalog_value = DuckLakeUtil::ParseCatalogEntry(entry.change_value);
			result.created_scalar_macros[catalog_value.schema].insert(
			    make_pair(std::move(catalog_value.name), "scalar_macro"));
			break;
		}
		case ChangeType::CREATED_TABLE_MACRO: {
			auto catalog_value = DuckLakeUtil::ParseCatalogEntry(entry.change_value);
			result.created_table_macros[catalog_value.schema].insert(
			    make_pair(std::move(catalog_value.name), "table_macro"));
			break;
		}
		case ChangeType::CREATED_VIEW: {
			auto catalog_value = DuckLakeUtil::ParseCatalogEntry(entry.change_value);
			result.created_tables[catalog_value.schema].insert(make_pair(std::move(catalog_value.name), "view"));
			break;
		}
		case ChangeType::CREATED_SCHEMA: {
			idx_t pos = 0;
			auto schema_name = DuckLakeUtil::ParseQuotedValue(entry.change_value, pos);
			result.created_schemas.insert(std::move(schema_name));
			break;
		}
		case ChangeType::DROPPED_SCHEMA:
			result.dropped_schemas.insert(SchemaIndex(StringUtil::ToUnsigned(entry.change_value)));
			break;
		case ChangeType::DROPPED_TABLE:
			result.dropped_tables.insert(TableIndex(StringUtil::ToUnsigned(entry.change_value)));
			break;
		case ChangeType::DROPPED_SCALAR_MACRO:
			result.dropped_scalar_macros.insert(MacroIndex(StringUtil::ToUnsigned(entry.change_value)));
			break;
		case ChangeType::DROPPED_TABLE_MACRO:
			result.dropped_table_macros.insert(MacroIndex(StringUtil::ToUnsigned(entry.change_value)));
			break;
		case ChangeType::DROPPED_VIEW:
			result.dropped_views.insert(TableIndex(StringUtil::ToUnsigned(entry.change_value)));
			break;
		case ChangeType::INSERTED_INTO_TABLE:
			result.inserted_tables.insert(TableIndex(StringUtil::ToUnsigned(entry.change_value)));
			break;
		case ChangeType::DELETED_FROM_TABLE:
			result.tables_deleted_from.insert(TableIndex(StringUtil::ToUnsigned(entry.change_value)));
			break;
		case ChangeType::ALTERED_TABLE:
			result.altered_tables.insert(TableIndex(StringUtil::ToUnsigned(entry.change_value)));
			break;
		case ChangeType::ALTERED_VIEW:
			result.altered_views.insert(TableIndex(StringUtil::ToUnsigned(entry.change_value)));
			break;
		case ChangeType::COMPACTED_TABLE:
			result.tables_compacted.insert(TableIndex(StringUtil::ToUnsigned(entry.change_value)));
			break;
		case ChangeType::MERGE_ADJACENT:
			result.tables_merge_adjacent.insert(TableIndex(StringUtil::ToUnsigned(entry.change_value)));
			break;
		case ChangeType::REWRITE_DELETE:
			result.tables_rewrite_delete.insert(TableIndex(StringUtil::ToUnsigned(entry.change_value)));
			break;
		case ChangeType::INSERTED_INTO_TABLE_INLINED:
			result.tables_inserted_inlined.insert(TableIndex(StringUtil::ToUnsigned(entry.change_value)));
			break;
		case ChangeType::DELETED_FROM_TABLE_INLINED:
			result.tables_deleted_inlined.insert(TableIndex(StringUtil::ToUnsigned(entry.change_value)));
			break;
		case ChangeType::FLUSHED_INLINE_DATA_FOR_TABLE:
			result.tables_flushed_inlined.insert(TableIndex(StringUtil::ToUnsigned(entry.change_value)));
			break;
		case ChangeType::MERGED_BRANCH:
			result.merged_branches.insert(entry.change_value);
			break;
		case ChangeType::CREATED_TYPE:
		case ChangeType::DROPPED_TYPE:
			// Schema-version-only markers for CREATE/DROP TYPE; no conflict tracking yet.
			break;
		default:
			throw InternalException("Unsupported change type in ParseChangesMade");
		}
	}
	return result;
}

namespace {

template <class T>
void MergeSet(set<T> &target, const set<T> &other) {
	target.insert(other.begin(), other.end());
}

void MergeCreatedMap(case_insensitive_map_t<case_insensitive_map_t<string>> &target,
                     const case_insensitive_map_t<case_insensitive_map_t<string>> &other) {
	for (auto &schema_entry : other) {
		auto &dest = target[schema_entry.first];
		for (auto &name_entry : schema_entry.second) {
			dest.insert(name_entry);
		}
	}
}

string FormatIndexConflict(ConflictReportStyle style, idx_t index, const char *action, const char *other_action) {
	if (style == ConflictReportStyle::TRANSACTION) {
		// Preserve OCC wording matched by concurrent / transaction conflict tests.
		return StringUtil::Format("attempting to %s with index \"%llu\" - but another transaction has %s", action,
		                          index, other_action);
	}
	return StringUtil::Format("Merge conflict - both branches %s object with index %llu (%s)", action, index,
	                          other_action);
}

string FormatNameConflict(ConflictReportStyle style, const string &name, const char *action,
                          const char *other_action) {
	if (style == ConflictReportStyle::TRANSACTION) {
		return StringUtil::Format("attempting to %s with name \"%s\" - but another transaction has %s", action, name,
		                          other_action);
	}
	return StringUtil::Format("Merge conflict - both branches %s \"%s\"", action, name);
}

string FormatCreatedConflict(ConflictReportStyle style, const string &schema, const string &name, const char *action) {
	if (style == ConflictReportStyle::TRANSACTION) {
		return StringUtil::Format("attempting to create %s \"%s\" in schema \"%s\" - but this entry has been created by "
		                          "another transaction already",
		                          action, name, schema);
	}
	return StringUtil::Format("Merge conflict - both branches %s \"%s\".\"%s\"", action, schema, name);
}

string FormatCrossConflict(ConflictReportStyle style, idx_t index, const char *source_op, const char *target_op) {
	if (style == ConflictReportStyle::TRANSACTION) {
		return StringUtil::Format("attempting to %s with index \"%llu\" - but another transaction has %s", source_op,
		                          index, target_op);
	}
	return StringUtil::Format("Merge conflict - source %s while target %s object with index %llu", source_op, target_op,
	                          index);
}

template <class T>
void CollectIndexConflicts(vector<string> &conflicts, const set<T> &source, const set<T> &target, const char *action,
                           const char *other_action, ConflictReportStyle style) {
	for (auto &idx : source) {
		if (target.find(idx) != target.end()) {
			conflicts.push_back(FormatIndexConflict(style, idx.index, action, other_action));
		}
	}
}

void CollectNameConflicts(vector<string> &conflicts, const case_insensitive_set_t &source,
                          const case_insensitive_set_t &target, const char *action, const char *other_action,
                          ConflictReportStyle style) {
	for (auto &name : source) {
		if (target.find(name) != target.end()) {
			conflicts.push_back(FormatNameConflict(style, name, action, other_action));
		}
	}
}

void CollectCreatedEntryConflicts(vector<string> &conflicts,
                                  const case_insensitive_map_t<case_insensitive_map_t<string>> &source,
                                  const case_insensitive_map_t<case_insensitive_map_t<string>> &target,
                                  const char *action, ConflictReportStyle style) {
	for (auto &schema_entry : source) {
		auto tgt_schema = target.find(schema_entry.first);
		if (tgt_schema == target.end()) {
			continue;
		}
		for (auto &name_entry : schema_entry.second) {
			auto tgt_name = tgt_schema->second.find(name_entry.first);
			if (tgt_name != tgt_schema->second.end()) {
				conflicts.push_back(FormatCreatedConflict(style, schema_entry.first, name_entry.first, action));
			}
		}
	}
}

template <class T>
void CollectCrossConflicts(vector<string> &conflicts, const set<T> &source, const set<T> &target, const char *source_op,
                           const char *target_op, ConflictReportStyle style) {
	for (auto &idx : source) {
		if (target.find(idx) != target.end()) {
			conflicts.push_back(FormatCrossConflict(style, idx.index, source_op, target_op));
		}
	}
}

} // namespace

void MergeSnapshotChangeInformation(SnapshotChangeInformation &target, const SnapshotChangeInformation &other) {
	for (auto &schema : other.created_schemas) {
		target.created_schemas.insert(schema);
	}
	MergeSet(target.dropped_schemas, other.dropped_schemas);
	MergeCreatedMap(target.created_tables, other.created_tables);
	MergeCreatedMap(target.created_scalar_macros, other.created_scalar_macros);
	MergeCreatedMap(target.created_table_macros, other.created_table_macros);
	MergeSet(target.altered_tables, other.altered_tables);
	MergeSet(target.altered_views, other.altered_views);
	MergeSet(target.dropped_tables, other.dropped_tables);
	MergeSet(target.dropped_views, other.dropped_views);
	MergeSet(target.dropped_scalar_macros, other.dropped_scalar_macros);
	MergeSet(target.dropped_table_macros, other.dropped_table_macros);
	MergeSet(target.inserted_tables, other.inserted_tables);
	MergeSet(target.tables_deleted_from, other.tables_deleted_from);
	MergeSet(target.tables_compacted, other.tables_compacted);
	MergeSet(target.tables_merge_adjacent, other.tables_merge_adjacent);
	MergeSet(target.tables_rewrite_delete, other.tables_rewrite_delete);
	MergeSet(target.tables_inserted_inlined, other.tables_inserted_inlined);
	MergeSet(target.tables_deleted_inlined, other.tables_deleted_inlined);
	MergeSet(target.tables_flushed_inlined, other.tables_flushed_inlined);
	for (auto &branch : other.merged_branches) {
		target.merged_branches.insert(branch);
	}
}

SnapshotChangeInformation FromTransactionChanges(const TransactionChangeInformation &changes) {
	SnapshotChangeInformation result;
	result.created_schemas = changes.created_schemas;
	for (auto &entry : changes.dropped_schemas) {
		result.dropped_schemas.insert(entry.first);
	}
	for (auto &schema_entry : changes.created_tables) {
		auto &schema_name = schema_entry.first;
		for (auto &table_ref : schema_entry.second) {
			auto &table = table_ref.get();
			string type = table.type == CatalogType::TABLE_ENTRY ? "table" : "view";
			result.created_tables[schema_name].emplace(table.name.GetIdentifierName(), type);
		}
	}
	for (auto &schema_entry : changes.created_scalar_macros) {
		auto &schema_name = schema_entry.first;
		for (auto &macro_ref : schema_entry.second) {
			result.created_scalar_macros[schema_name].emplace(macro_ref.get().name.GetIdentifierName(), "macro");
		}
	}
	for (auto &schema_entry : changes.created_table_macros) {
		auto &schema_name = schema_entry.first;
		for (auto &macro_ref : schema_entry.second) {
			result.created_table_macros[schema_name].emplace(macro_ref.get().name.GetIdentifierName(), "macro");
		}
	}
	result.altered_tables = changes.altered_tables;
	result.altered_views = changes.altered_views;
	result.dropped_tables = changes.dropped_tables;
	result.dropped_views = changes.dropped_views;
	result.dropped_scalar_macros = changes.dropped_scalar_macros;
	result.dropped_table_macros = changes.dropped_table_macros;
	result.inserted_tables = changes.tables_inserted_into;
	result.tables_deleted_from = changes.tables_deleted_from;
	result.tables_compacted = changes.tables_compacted;
	result.tables_merge_adjacent = changes.tables_merge_adjacent;
	result.tables_rewrite_delete = changes.tables_rewrite_delete;
	result.tables_inserted_inlined = changes.tables_inserted_inlined;
	result.tables_deleted_inlined = changes.tables_deleted_inlined;
	result.tables_flushed_inlined = changes.tables_flushed_inlined;
	return result;
}

vector<string> DetectConflicts(const SnapshotChangeInformation &source_changes,
                               const SnapshotChangeInformation &target_changes, ConflictReportStyle style) {
	vector<string> conflicts;
	const bool merge = style == ConflictReportStyle::MERGE;

	// Verb pairs: MERGE uses short ops; TRANSACTION uses OCC "attempting to X / another has Y" phrasing.
	const char *drop_table_action = merge ? "dropped table" : "drop table";
	const char *drop_view_action = merge ? "dropped view" : "drop view";
	const char *drop_schema_action = merge ? "dropped schema" : "drop schema";
	const char *drop_macro_action = merge ? "dropped macro" : "drop macro";
	const char *drop_already = merge ? "drop-vs-drop" : "dropped it already";
	const char *create_schema_action = merge ? "created schema" : "create schema";
	const char *create_schema_other = merge ? "created schema" : "created a schema with this name already";
	const char *alter_table_action = merge ? "altered table" : "alter table";
	const char *alter_view_action = merge ? "altered view" : "alter view";
	const char *alter_other = merge ? "schema-evolution divergence" : "altered it";
	const char *insert_action = merge ? "inserted into" : "insert into table";
	const char *inlined_insert_action = merge ? "inlined-inserted into" : "insert into table";
	const char *delete_action = merge ? "deleted from" : "delete from table";
	const char *inlined_delete_action = merge ? "inlined-deleted from" : "delete from table";
	const char *compact_action = merge ? "compacted" : "compact table";
	const char *alter_action = merge ? "altered" : "alter table";
	const char *dropped_it = merge ? "dropped" : "dropped it";
	const char *altered_it = merge ? "altered" : "altered it";
	const char *deleted_from_it = merge ? "deleted from" : "deleted from it";
	const char *inserted_into_it = merge ? "inserted into" : "inserted into it";
	const char *compacted_it = merge ? "compacted" : "compacted it";
	const char *inlined_delete_other = merge ? "overlapping inlined deletes" : "deleted from it";
	const char *compaction_other = merge ? "compaction-vs-compaction" : "compacted it";

	CollectIndexConflicts(conflicts, source_changes.dropped_tables, target_changes.dropped_tables, drop_table_action,
	                      drop_already, style);
	CollectIndexConflicts(conflicts, source_changes.dropped_views, target_changes.dropped_views, drop_view_action,
	                      drop_already, style);
	CollectIndexConflicts(conflicts, source_changes.dropped_schemas, target_changes.dropped_schemas, drop_schema_action,
	                      drop_already, style);
	CollectIndexConflicts(conflicts, source_changes.dropped_scalar_macros, target_changes.dropped_scalar_macros,
	                      drop_macro_action, drop_already, style);
	CollectIndexConflicts(conflicts, source_changes.dropped_table_macros, target_changes.dropped_table_macros,
	                      drop_macro_action, drop_already, style);

	CollectNameConflicts(conflicts, source_changes.created_schemas, target_changes.created_schemas, create_schema_action,
	                     create_schema_other, style);
	CollectCreatedEntryConflicts(conflicts, source_changes.created_tables, target_changes.created_tables,
	                             "created table/view", style);
	CollectCreatedEntryConflicts(conflicts, source_changes.created_scalar_macros, target_changes.created_scalar_macros,
	                             "created scalar macro", style);
	CollectCreatedEntryConflicts(conflicts, source_changes.created_table_macros, target_changes.created_table_macros,
	                             "created table macro", style);

	// Schema-evolution / alter divergence on the same object
	CollectIndexConflicts(conflicts, source_changes.altered_tables, target_changes.altered_tables, alter_table_action,
	                      alter_other, style);
	CollectIndexConflicts(conflicts, source_changes.altered_views, target_changes.altered_views, alter_view_action,
	                      alter_other, style);

	// Insert vs drop/alter/delete (source = local for TRANSACTION)
	CollectCrossConflicts(conflicts, source_changes.inserted_tables, target_changes.dropped_tables, insert_action,
	                      dropped_it, style);
	CollectCrossConflicts(conflicts, source_changes.inserted_tables, target_changes.altered_tables, insert_action,
	                      altered_it, style);
	CollectCrossConflicts(conflicts, source_changes.inserted_tables, target_changes.tables_deleted_from, insert_action,
	                      deleted_from_it, style);
	CollectCrossConflicts(conflicts, source_changes.tables_inserted_inlined, target_changes.dropped_tables,
	                      inlined_insert_action, dropped_it, style);
	CollectCrossConflicts(conflicts, source_changes.tables_inserted_inlined, target_changes.altered_tables,
	                      inlined_insert_action, altered_it, style);

	// Delete vs alter/compact/insert
	CollectCrossConflicts(conflicts, source_changes.tables_deleted_from, target_changes.dropped_tables, delete_action,
	                      dropped_it, style);
	CollectCrossConflicts(conflicts, source_changes.tables_deleted_from, target_changes.altered_tables, delete_action,
	                      altered_it, style);
	CollectCrossConflicts(conflicts, source_changes.tables_deleted_from, target_changes.tables_merge_adjacent,
	                      delete_action, compacted_it, style);
	CollectCrossConflicts(conflicts, source_changes.tables_deleted_from, target_changes.tables_rewrite_delete,
	                      delete_action, compacted_it, style);
	CollectCrossConflicts(conflicts, source_changes.tables_deleted_from, target_changes.inserted_tables, delete_action,
	                      inserted_into_it, style);

	// Overlapping inlined deletes remain table-level; file-level parquet deletes are handled by
	// merge callers via GetFilesDeletedOrDroppedInRange / OCC enrichment.
	CollectIndexConflicts(conflicts, source_changes.tables_deleted_inlined, target_changes.tables_deleted_inlined,
	                      inlined_delete_action, inlined_delete_other, style);

	// Compaction vs compaction / delete
	CollectIndexConflicts(conflicts, source_changes.tables_merge_adjacent, target_changes.tables_merge_adjacent,
	                      compact_action, compaction_other, style);
	CollectIndexConflicts(conflicts, source_changes.tables_rewrite_delete, target_changes.tables_rewrite_delete,
	                      compact_action, compaction_other, style);
	CollectCrossConflicts(conflicts, source_changes.tables_merge_adjacent, target_changes.tables_deleted_from,
	                      compact_action, deleted_from_it, style);
	CollectCrossConflicts(conflicts, source_changes.tables_rewrite_delete, target_changes.tables_deleted_from,
	                      compact_action, deleted_from_it, style);

	// Alter vs drop
	CollectCrossConflicts(conflicts, source_changes.altered_tables, target_changes.dropped_tables, alter_action,
	                      dropped_it, style);
	CollectCrossConflicts(conflicts, source_changes.altered_views, target_changes.dropped_views,
	                      merge ? "altered" : "alter view", dropped_it, style);

	if (merge) {
		// Merge needs the reverse directions for symmetric branch comparison.
		CollectCrossConflicts(conflicts, target_changes.inserted_tables, source_changes.dropped_tables, "inserted into",
		                      "dropped", style);
		CollectCrossConflicts(conflicts, target_changes.inserted_tables, source_changes.altered_tables, "inserted into",
		                      "altered", style);
		CollectCrossConflicts(conflicts, target_changes.inserted_tables, source_changes.tables_deleted_from,
		                      "inserted into", "deleted from", style);
		CollectCrossConflicts(conflicts, target_changes.tables_inserted_inlined, source_changes.dropped_tables,
		                      "inlined-inserted into", "dropped", style);
		CollectCrossConflicts(conflicts, target_changes.tables_inserted_inlined, source_changes.altered_tables,
		                      "inlined-inserted into", "altered", style);
		CollectCrossConflicts(conflicts, target_changes.tables_deleted_from, source_changes.dropped_tables,
		                      "deleted from", "dropped", style);
		CollectCrossConflicts(conflicts, target_changes.tables_deleted_from, source_changes.altered_tables,
		                      "deleted from", "altered", style);
		CollectCrossConflicts(conflicts, target_changes.tables_deleted_from, source_changes.tables_merge_adjacent,
		                      "deleted from", "compacted", style);
		CollectCrossConflicts(conflicts, target_changes.tables_deleted_from, source_changes.tables_rewrite_delete,
		                      "deleted from", "compacted", style);
		CollectCrossConflicts(conflicts, target_changes.tables_deleted_from, source_changes.inserted_tables,
		                      "deleted from", "inserted into", style);
		CollectCrossConflicts(conflicts, target_changes.tables_merge_adjacent, source_changes.tables_deleted_from,
		                      "compacted", "deleted from", style);
		CollectCrossConflicts(conflicts, target_changes.tables_rewrite_delete, source_changes.tables_deleted_from,
		                      "compacted", "deleted from", style);
		CollectCrossConflicts(conflicts, target_changes.altered_tables, source_changes.dropped_tables, "altered",
		                      "dropped", style);
		CollectCrossConflicts(conflicts, target_changes.altered_views, source_changes.dropped_views, "altered",
		                      "dropped", style);
	} else {
		// OCC extras that DetectConflicts owns for local inlined deletes / compaction.
		CollectCrossConflicts(conflicts, source_changes.tables_deleted_inlined, target_changes.dropped_tables,
		                      "delete from table", "dropped it", style);
		CollectCrossConflicts(conflicts, source_changes.tables_deleted_inlined, target_changes.altered_tables,
		                      "delete from table", "altered it", style);
		CollectCrossConflicts(conflicts, source_changes.tables_deleted_inlined, target_changes.inserted_tables,
		                      "delete from table", "inserted into it", style);
		CollectCrossConflicts(conflicts, source_changes.tables_deleted_inlined, target_changes.tables_inserted_inlined,
		                      "delete from table", "inserted into it", style);
	}

	// Same-table double-append is intentionally NOT a conflict (compose semantics).
	return conflicts;
}

} // namespace duckdb
