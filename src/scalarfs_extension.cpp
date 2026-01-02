#define DUCKDB_EXTENSION_MAIN

#include "scalarfs_extension.hpp"
#include "data_uri_filesystem.hpp"
#include "variable_filesystem.hpp"
#include "pathvariable_filesystem.hpp"
#include "scalarfs_functions.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/main/database.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	// Get the database instance and its filesystem
	auto &db = loader.GetDatabaseInstance();
	auto &fs = FileSystem::GetFileSystem(db);

	// Register the data URI filesystem (handles data:, data+varchar:, data+blob:)
	fs.RegisterSubSystem(make_uniq<DataURIFileSystem>());

	// Register the variable filesystem (handles variable:)
	fs.RegisterSubSystem(make_uniq<VariableFileSystem>());

	// Register the path variable filesystem (handles pathvariable:)
	fs.RegisterSubSystem(make_uniq<PathVariableFileSystem>());

	// Register scalar functions for encoding/decoding URIs
	ScalarfsFunctions::Register(loader);
}

void ScalarfsExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string ScalarfsExtension::Name() {
	return "scalarfs";
}

std::string ScalarfsExtension::Version() const {
#ifdef EXT_VERSION_SCALARFS
	return EXT_VERSION_SCALARFS;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(scalarfs, loader) {
	duckdb::LoadInternal(loader);
}
}
