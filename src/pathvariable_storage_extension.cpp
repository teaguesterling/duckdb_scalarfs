#include "pathvariable_storage_extension.hpp"
#include "pathvariable_filesystem.hpp"
#include "pathvariable_modifiers.hpp"

#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"

namespace duckdb {

static unique_ptr<Catalog> PathVariableStorageAttach(optional_ptr<StorageExtensionInfo> storage_info,
                                                     ClientContext &context, AttachedDatabase &db, const string &name,
                                                     AttachInfo &info, AttachOptions &options) {
	// By the time this callback runs, DBPathAndType::ExtractExtensionPrefix has already
	// stripped the leading "pathvariable:" from info.path. Reconstruct the full form so
	// the shared parser can handle any modifier syntax uniformly.
	string full_path = "pathvariable:" + info.path;
	auto parsed = PathVariableParser::Parse(full_path);

	if (parsed.variable_name.empty()) {
		throw BinderException("ATTACH 'pathvariable:...' requires a variable name (got '%s')", info.path);
	}

	// ATTACH needs a single concrete file; modifiers like search/no-missing/append/prepend
	// are read-side glob-expansion features that do not make sense here. Reject them with
	// a clear error instead of silently ignoring them.
	if (parsed.flags != PathVariableModifierFlag::NONE || parsed.is_temp) {
		throw BinderException("ATTACH 'pathvariable:...' does not support modifiers or temp paths; got '%s'. "
		                      "Use a scalar VARCHAR variable holding the database file path.",
		                      info.path);
	}

	// Resolve the variable to a concrete path. Throws on missing / NULL / wrong type.
	string resolved_path = PathVariableFileSystem::GetPathFromVariable(context, parsed.variable_name);

	// Rewrite info.path so the SingleFileStorageManager (constructed immediately after this
	// callback returns, inside AttachedDatabase's storage-extension constructor) opens the
	// real file through the standard DuckDB code path.
	info.path = std::move(resolved_path);

	// Returning a DuckCatalog tells AttachedDatabase to create a normal SingleFileStorageManager
	// on info.path — identical to an ordinary `ATTACH '<file>'` from that point forward.
	return make_uniq<DuckCatalog>(db);
}

static unique_ptr<TransactionManager>
PathVariableStorageTransactionManager(optional_ptr<StorageExtensionInfo> storage_info, AttachedDatabase &db,
                                      Catalog &catalog) {
	return make_uniq<DuckTransactionManager>(db);
}

shared_ptr<StorageExtension> PathVariableStorageExtension::Create() {
	auto result = make_shared_ptr<StorageExtension>();
	result->attach = PathVariableStorageAttach;
	result->create_transaction_manager = PathVariableStorageTransactionManager;
	return result;
}

} // namespace duckdb
