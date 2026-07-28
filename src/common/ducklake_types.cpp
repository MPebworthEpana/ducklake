#include "common/ducklake_types.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/common/array.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/type_visitor.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/common/sql_identifier.hpp"
#include "duckdb/parser/keyword_helper.hpp"

namespace duckdb {

struct DefaultType {
	const char *name;
	LogicalTypeId id;
};

using ducklake_type_array = std::array<DefaultType, 33>;

static constexpr const ducklake_type_array DUCKLAKE_TYPES {{{"boolean", LogicalTypeId::BOOLEAN},
                                                            {"int8", LogicalTypeId::TINYINT},
                                                            {"int16", LogicalTypeId::SMALLINT},
                                                            {"int32", LogicalTypeId::INTEGER},
                                                            {"int64", LogicalTypeId::BIGINT},
                                                            {"int128", LogicalTypeId::HUGEINT},
                                                            {"uint8", LogicalTypeId::UTINYINT},
                                                            {"uint16", LogicalTypeId::USMALLINT},
                                                            {"uint32", LogicalTypeId::UINTEGER},
                                                            {"uint64", LogicalTypeId::UBIGINT},
                                                            {"uint128", LogicalTypeId::UHUGEINT},
                                                            {"float32", LogicalTypeId::FLOAT},
                                                            {"float64", LogicalTypeId::DOUBLE},
                                                            {"decimal", LogicalTypeId::DECIMAL},
                                                            {"time", LogicalTypeId::TIME},
                                                            {"time_ns", LogicalTypeId::TIME_NS},
                                                            {"date", LogicalTypeId::DATE},
                                                            {"timestamp", LogicalTypeId::TIMESTAMP},
                                                            {"timestamp_us", LogicalTypeId::TIMESTAMP},
                                                            {"timestamp_ms", LogicalTypeId::TIMESTAMP_MS},
                                                            {"timestamp_ns", LogicalTypeId::TIMESTAMP_NS},
                                                            {"timestamp_s", LogicalTypeId::TIMESTAMP_SEC},
                                                            {"timestamptz", LogicalTypeId::TIMESTAMP_TZ},
                                                            {"timestamptz_ns", LogicalTypeId::TIMESTAMP_TZ_NS},
                                                            {"timetz", LogicalTypeId::TIME_TZ},
                                                            {"interval", LogicalTypeId::INTERVAL},
                                                            {"varchar", LogicalTypeId::VARCHAR},
                                                            {"blob", LogicalTypeId::BLOB},
                                                            {"uuid", LogicalTypeId::UUID},
                                                            {"struct", LogicalTypeId::STRUCT},
                                                            {"map", LogicalTypeId::MAP},
                                                            {"list", LogicalTypeId::LIST},
                                                            {"unknown", LogicalTypeId::UNKNOWN}}};

static LogicalType ParseBaseType(const string &str) {
	for (auto &ducklake_type : DUCKLAKE_TYPES) {
		if (StringUtil::CIEquals(str, ducklake_type.name)) {
			return ducklake_type.id;
		}
	}

	if (StringUtil::CIEquals(str, "json")) {
		return LogicalType::JSON();
	}
	if (StringUtil::CIEquals(str, "variant")) {
		return LogicalType::VARIANT();
	}
	if (StringUtil::CIEquals(str, "geometry")) {
		return LogicalType::GEOMETRY();
	}

	throw InvalidInputException("Failed to parse DuckLake type - unsupported type '%s'", str);
}

static string ToStringBaseType(const LogicalType &type) {
	for (auto &ducklake_type : DUCKLAKE_TYPES) {
		if (type.id() == ducklake_type.id) {
			return ducklake_type.name;
		}
	}
	throw InvalidInputException("Failed to convert DuckDB type to DuckLake - unsupported type %s", type);
}

static LogicalType ParseEnumType(const string &type) {
	// enum('a', 'b', ...) — accept ENUM(...) as well
	idx_t start = type.find('(');
	idx_t end = type.rfind(')');
	if (start == string::npos || end == string::npos || end <= start + 1) {
		throw InvalidInputException("Invalid ENUM type '%s' - expected enum('v1', 'v2', ...)", type);
	}
	string members_str = type.substr(start + 1, end - start - 1);
	vector<string> members = StringUtil::SplitWithParentheses(members_str);
	if (members.empty()) {
		throw InvalidInputException("ENUM type must have at least one member");
	}
	Vector ordered_data(LogicalType::VARCHAR, members.size());
	auto data = FlatVector::GetDataMutable<string_t>(ordered_data);
	for (idx_t i = 0; i < members.size(); i++) {
		auto member = members[i];
		StringUtil::Trim(member);
		// Strip surrounding quotes if present
		if (member.size() >= 2 && ((member.front() == '\'' && member.back() == '\'') ||
		                           (member.front() == '"' && member.back() == '"'))) {
			member = member.substr(1, member.size() - 2);
			member = StringUtil::Replace(member, "''", "'");
		}
		data[i] = StringVector::AddString(ordered_data, member);
	}
	return LogicalType::ENUM(ordered_data, members.size());
}

static string EnumToString(const LogicalType &type) {
	string ret = "enum(";
	auto size = EnumType::GetSize(type);
	for (idx_t i = 0; i < size; i++) {
		if (i > 0) {
			ret += ", ";
		}
		ret += SQLString::ToString(EnumType::GetString(type, i).GetString());
	}
	ret += ")";
	return ret;
}

bool DuckLakeTypes::RequiresCast(const LogicalType &type) {
	// There are no types that requires casts as of DuckDB v1.5
	return false;
}

bool DuckLakeTypes::RequiresCast(const vector<LogicalType> &types) {
	for (auto &type : types) {
		if (RequiresCast(type)) {
			return true;
		}
	}
	return false;
}

LogicalType DuckLakeTypes::GetCastedType(const LogicalType &type) {
	// There are no types that requires casts as of DuckDB v1.5
	return type;
}

bool DuckLakeTypes::IsArrayType(const string &type) {
	return StringUtil::StartsWith(StringUtil::Lower(type), "array(") && StringUtil::EndsWith(type, ")");
}

idx_t DuckLakeTypes::ParseArraySize(const string &type) {
	if (!IsArrayType(type)) {
		throw InvalidInputException("Expected array(N) type, got '%s'", type);
	}
	string size_str = type.substr(6, type.size() - 7);
	try {
		StringUtil::Trim(size_str);
		auto size = std::stoull(size_str);
		if (size == 0) {
			throw InvalidInputException("ARRAY size must be greater than 0");
		}
		return size;
	} catch (...) {
		throw InvalidInputException("Invalid ARRAY size in type '%s'", type);
	}
}

LogicalType DuckLakeTypes::FromString(const string &type) {
	if (StringUtil::StartsWith(type, "decimal(") && StringUtil::EndsWith(type, ")")) {
		// decimal - parse width/scale
		string decimal_members_str = type.substr(8, type.size() - 9);
		vector<string> decimal_members_vect = StringUtil::SplitWithParentheses(decimal_members_str);
		if (decimal_members_vect.size() != 2) {
			throw NotImplementedException("Invalid DECIMAL type - expected width and scale");
		}
		auto width = std::stoull(decimal_members_vect[0]);
		auto scale = std::stoull(decimal_members_vect[1]);
		return LogicalType::DECIMAL(width, scale);
	}
	if (IsArrayType(type)) {
		// Parent placeholder — actual ARRAY type is reconstructed with child in TransformColumnType
		return LogicalTypeId::ARRAY;
	}
	auto lower = StringUtil::Lower(type);
	if (StringUtil::StartsWith(lower, "enum(") && StringUtil::EndsWith(type, ")")) {
		return ParseEnumType(type);
	}
	return ParseBaseType(type);
}

string DuckLakeTypes::ToString(const LogicalType &type) {
	// JSON is stored as an aliased VARCHAR
	if (type.IsJSONType()) {
		return "json";
	}
	// ENUM — serialize members; named ENUMs (CREATE TYPE) keep physical enum storage
	if (type.id() == LogicalTypeId::ENUM) {
		return EnumToString(type);
	}
	// STRUCT-alias UDTs expand to underlying struct physically
	if (type.HasAlias()) {
		if (type.id() == LogicalTypeId::STRUCT) {
			return "struct";
		}
		if (type.id() == LogicalTypeId::UNBOUND) {
			const auto type_name = type.GetAlias();
			if (StringUtil::Lower(type_name) == "json") {
				return "json";
			}
		}
		throw InvalidInputException("Unsupported user-defined type \"%s\"", type.GetAlias());
	}
	switch (type.id()) {
	case LogicalTypeId::STRUCT:
		return "struct";
	case LogicalTypeId::VARIANT:
		return "variant";
	case LogicalTypeId::GEOMETRY:
		return "geometry";
	case LogicalTypeId::LIST:
		return "list";
	case LogicalTypeId::ARRAY:
		return "array(" + to_string(ArrayType::GetSize(type)) + ")";
	case LogicalTypeId::MAP:
		return "map";
	case LogicalTypeId::DECIMAL:
		return "decimal(" + to_string(DecimalType::GetWidth(type)) + "," + to_string(DecimalType::GetScale(type)) + ")";
	case LogicalTypeId::VARCHAR:
		if (!StringType::GetCollation(type).empty()) {
			throw InvalidInputException("Collations are not supported in DuckLake storage");
		}
		return ToStringBaseType(type);
	default:
		return ToStringBaseType(type);
	}
}

void DuckLakeTypes::CheckSupportedType(const LogicalType &type) {
	TypeVisitor::VisitReplace(type, [](const LogicalType &type) {
		DuckLakeTypes::ToString(type);
		return type;
	});
}

} // namespace duckdb
