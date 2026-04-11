#pragma once

#include "duckdb/storage/storage_extension.hpp"

namespace duckdb {

// =============================================================================
// PathVariableStorageExtension
// =============================================================================
//
// Registers `pathvariable` as a DuckDB storage extension so `ATTACH` can use
// a variable to hold the database file path:
//
//   SET VARIABLE db_path = '/data/my.duckdb';
//   ATTACH 'pathvariable:db_path' AS db;
//
// The filesystem subsystem alone is not sufficient for ATTACH. DuckDB treats
// the `prefix:` in an ATTACH path as a database-type hint (see
// DBPathAndType::ExtractExtensionPrefix), and the actual database-file open
// later happens through a DatabaseFileOpener whose `TryGetClientContext()`
// returns nullptr — so the pathvariable filesystem could never resolve the
// variable from that code path. Registering a StorageExtension gives us an
// attach callback that receives a ClientContext directly, which we use to
// resolve the variable and rewrite `info.path` to the concrete file path
// before DuckDB constructs the SingleFileStorageManager.
//
struct PathVariableStorageExtension {
	static shared_ptr<StorageExtension> Create();
};

} // namespace duckdb
