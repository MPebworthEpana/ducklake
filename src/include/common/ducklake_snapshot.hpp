//===----------------------------------------------------------------------===//
//                         DuckDB
//
// common/ducklake_snapshot.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"

namespace duckdb {

class Serializer;
class Deserializer;

struct DuckLakeSnapshot {
	DuckLakeSnapshot(idx_t snapshot_id, idx_t schema_version, idx_t next_catalog_id, idx_t next_file_id)
	    : snapshot_id(snapshot_id), schema_version(schema_version), next_catalog_id(next_catalog_id),
	      next_file_id(next_file_id), branch_id(0) {
	}

	DuckLakeSnapshot()
	    : snapshot_id(DConstants::INVALID_INDEX), schema_version(DConstants::INVALID_INDEX),
	      next_catalog_id(DConstants::INVALID_INDEX), next_file_id(DConstants::INVALID_INDEX), branch_id(0) {
	}

	idx_t snapshot_id;
	idx_t schema_version;
	idx_t next_catalog_id;
	idx_t next_file_id;
	//! Owning branch (0 = main). Populated when SupportsWritableBranches().
	idx_t branch_id;

	void Serialize(Serializer &serializer) const;
	static DuckLakeSnapshot Deserialize(Deserializer &deserializer);
};

} // namespace duckdb
