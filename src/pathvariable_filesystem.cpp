#include "pathvariable_filesystem.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/scalar/string_common.hpp"
#include "duckdb/main/client_config.hpp"
#include <algorithm>

namespace duckdb {

// =============================================================================
// PathVariableFileHandle Implementation
// =============================================================================

PathVariableFileHandle::PathVariableFileHandle(FileSystem &pathvar_fs, string original_path,
                                               unique_ptr<FileHandle> underlying_handle_p, FileSystem &underlying_fs_p)
    : FileHandle(pathvar_fs, std::move(original_path), underlying_handle_p->GetFlags()),
      underlying_handle(std::move(underlying_handle_p)), underlying_fs(underlying_fs_p) {
}

void PathVariableFileHandle::Close() {
	if (underlying_handle) {
		underlying_handle->Close();
	}
}

// =============================================================================
// PathVariableFileSystem Implementation
// =============================================================================

bool PathVariableFileSystem::CanHandleFile(const string &fpath) {
	return PathVariableParser::CanHandle(fpath);
}

string PathVariableFileSystem::GetName() const {
	return "PathVariableFileSystem";
}

bool PathVariableFileSystem::IsTempPath(const string &path) {
	return PathVariableParser::IsTempPath(path);
}

string PathVariableFileSystem::ExtractVariableName(const string &path) {
	// Use the parser to extract variable name (handles modifiers)
	auto parsed = PathVariableParser::Parse(path);
	return parsed.variable_name;
}

string PathVariableFileSystem::ComputeTempPath(const string &target_path) {
	// Given a target path like "/data/output.csv", compute temp path "/data/tmp_output.csv"
	// This mirrors how DuckDB creates temp files (prepending tmp_ to filename)

	auto sep_pos = target_path.find_last_of("/\\");
	if (sep_pos == string::npos) {
		// No directory component, just prepend tmp_ to filename
		return "tmp_" + target_path;
	}

	// Split into directory and filename, prepend tmp_ to filename
	string dir = target_path.substr(0, sep_pos + 1);
	string filename = target_path.substr(sep_pos + 1);
	return dir + "tmp_" + filename;
}

string PathVariableFileSystem::GetPathFromVariable(const string &var_name, optional_ptr<FileOpener> opener) {
	auto context = FileOpener::TryGetClientContext(opener);
	if (!context) {
		throw IOException("Cannot access variables without client context");
	}

	auto &config = ClientConfig::GetConfig(*context);
	Value result;
	if (!config.GetUserVariable(var_name, result)) {
		throw IOException("Variable '%s' not found", var_name);
	}
	if (result.IsNull()) {
		throw IOException("Variable '%s' is NULL", var_name);
	}

	// Type validation: must be VARCHAR or BLOB (or list of those for read operations)
	auto &type = result.type();
	if (type.id() == LogicalTypeId::LIST) {
		// Check if it's a valid list type (VARCHAR[] or BLOB[])
		auto &child_type = ListType::GetChildType(type);
		if (child_type.id() == LogicalTypeId::VARCHAR || child_type.id() == LogicalTypeId::BLOB) {
			// Valid list type - supported for reading via Glob, but not for single-file operations
			throw IOException("Variable '%s' is a list type (%s). List variables are supported for reading "
			                  "(e.g., read_csv, read_json), but not for single-file write operations. "
			                  "Use a scalar VARCHAR or BLOB variable for writes.",
			                  var_name, type.ToString());
		} else {
			// Invalid list child type
			throw IOException("Variable '%s' is a list but child type must be VARCHAR or BLOB, got %s", var_name,
			                  type.ToString());
		}
	}
	if (type.id() != LogicalTypeId::VARCHAR && type.id() != LogicalTypeId::BLOB) {
		throw IOException("Variable '%s' must be VARCHAR or BLOB type to be used as a path, got %s", var_name,
		                  type.ToString());
	}

	return result.ToString();
}

bool PathVariableFileSystem::IsListVariable(const string &var_name, optional_ptr<FileOpener> opener) {
	auto context = FileOpener::TryGetClientContext(opener);
	if (!context) {
		return false;
	}

	auto &config = ClientConfig::GetConfig(*context);
	Value result;
	if (!config.GetUserVariable(var_name, result)) {
		return false;
	}
	if (result.IsNull()) {
		return false;
	}

	return result.type().id() == LogicalTypeId::LIST;
}

vector<string> PathVariableFileSystem::GetPathsFromVariable(const string &var_name, optional_ptr<FileOpener> opener) {
	auto context = FileOpener::TryGetClientContext(opener);
	if (!context) {
		throw IOException("Cannot access variables without client context");
	}

	auto &config = ClientConfig::GetConfig(*context);
	Value result;
	if (!config.GetUserVariable(var_name, result)) {
		throw IOException("Variable '%s' not found", var_name);
	}
	if (result.IsNull()) {
		throw IOException("Variable '%s' is NULL", var_name);
	}

	auto &type = result.type();

	// Handle list types (VARCHAR[] or BLOB[])
	if (type.id() == LogicalTypeId::LIST) {
		auto &child_type = ListType::GetChildType(type);
		if (child_type.id() != LogicalTypeId::VARCHAR && child_type.id() != LogicalTypeId::BLOB) {
			throw IOException("Variable '%s' is a list but child type must be VARCHAR or BLOB, got %s[]", var_name,
			                  child_type.ToString());
		}

		vector<string> paths;
		auto &children = ListValue::GetChildren(result);
		for (const auto &child : children) {
			if (child.IsNull()) {
				throw IOException("Variable '%s' contains NULL element in list", var_name);
			}
			paths.push_back(child.ToString());
		}
		return paths;
	}

	// Handle scalar types (VARCHAR or BLOB)
	if (type.id() != LogicalTypeId::VARCHAR && type.id() != LogicalTypeId::BLOB) {
		throw IOException("Variable '%s' must be VARCHAR, BLOB, or a list of these types, got %s", var_name,
		                  type.ToString());
	}

	return {result.ToString()};
}

string PathVariableFileSystem::ResolvePath(const string &path, optional_ptr<FileOpener> opener) {
	string var_name = ExtractVariableName(path);
	string target_path = GetPathFromVariable(var_name, opener);

	if (IsTempPath(path)) {
		// For tmp_pathvariable:, compute the temp path
		return ComputeTempPath(target_path);
	}

	return target_path;
}

FileSystem &PathVariableFileSystem::GetParentFileSystem(optional_ptr<FileOpener> opener) {
	auto context = FileOpener::TryGetClientContext(opener);
	if (!context) {
		throw IOException("Cannot access filesystem without client context");
	}
	return FileSystem::GetFileSystem(*context);
}

unique_ptr<FileHandle> PathVariableFileSystem::OpenFile(const string &path, FileOpenFlags flags,
                                                        optional_ptr<FileOpener> opener) {
	// Resolve the actual file path from the variable
	string resolved_path = ResolvePath(path, opener);

	// Get the parent filesystem and delegate the open operation
	// Note: GetParentFileSystem returns an OpenerFileSystem which already has context,
	// so we pass nullptr for the opener to avoid "cannot take an opener" errors
	auto &parent_fs = GetParentFileSystem(opener);
	auto underlying_handle = parent_fs.OpenFile(resolved_path, flags, nullptr);

	// Wrap in our handle type
	return make_uniq<PathVariableFileHandle>(*this, path, std::move(underlying_handle), parent_fs);
}

vector<OpenFileInfo> PathVariableFileSystem::Glob(const string &path, FileOpener *opener) {
	// =============================================================================
	// Multi-level glob implementation with modifier support:
	//
	// Modifiers:
	//   no-glob        - Disable glob expansion in paths
	//   search         - Return only first existing match
	//   ignore-missing - Skip non-existent files
	//   reverse        - Reverse path order
	//   shuffle        - Randomize path order
	//   append!value   - Append value to each path
	//   prepend!value  - Prepend value to each path
	//
	// Levels:
	//   Level 1: Glob on variable names (pathvariable:data_* matches data_01, data_02)
	//   Level 1b: List expansion (if variable is VARCHAR[], expand to multiple paths)
	//   Level 2: Glob within paths (if data_01 = '/data/*.csv', expands that too)
	//            (disabled by no-glob modifier)
	// =============================================================================

	if (!CanHandleFile(path)) {
		return {};
	}

	// Parse the path to extract modifiers and variable name
	auto parsed = PathVariableParser::Parse(path);
	string pattern = parsed.variable_name;

	// Get client context for variable access
	auto context = FileOpener::TryGetClientContext(opener);
	if (!context) {
		// Without context, can't enumerate variables
		return {OpenFileInfo(path)};
	}

	auto &config = ClientConfig::GetConfig(*context);
	auto &parent_fs = FileSystem::GetFileSystem(*context);

	vector<string> resolved_paths;

	// Helper lambda to extract paths from a Value (handles scalar and list types)
	auto extract_paths_from_value = [](const Value &var_value, vector<string> &out_paths) -> bool {
		if (var_value.IsNull()) {
			return false;
		}

		auto &type = var_value.type();

		// Handle list types (VARCHAR[] or BLOB[])
		if (type.id() == LogicalTypeId::LIST) {
			auto &child_type = ListType::GetChildType(type);
			if (child_type.id() != LogicalTypeId::VARCHAR && child_type.id() != LogicalTypeId::BLOB) {
				return false; // Wrong child type
			}

			auto &children = ListValue::GetChildren(var_value);
			for (const auto &child : children) {
				if (!child.IsNull()) {
					out_paths.push_back(child.ToString());
				}
			}
			return true;
		}

		// Handle scalar types
		if (type.id() != LogicalTypeId::VARCHAR && type.id() != LogicalTypeId::BLOB) {
			return false; // Wrong type
		}

		out_paths.push_back(var_value.ToString());
		return true;
	};

	// Helper to resolve a PathVariableValue (literal or variable reference)
	auto resolve_value = [&config](const PathVariableValue &pv_value) -> vector<string> {
		if (pv_value.IsEmpty()) {
			return {};
		}
		if (!pv_value.is_variable) {
			// Literal value
			return {pv_value.value};
		}
		// Variable reference - look up the variable
		Value var_result;
		if (!config.GetUserVariable(pv_value.value, var_result)) {
			throw IOException("Variable '%s' (referenced in modifier) not found", pv_value.value);
		}
		if (var_result.IsNull()) {
			throw IOException("Variable '%s' (referenced in modifier) is NULL", pv_value.value);
		}
		vector<string> values;
		auto &type = var_result.type();
		if (type.id() == LogicalTypeId::LIST) {
			auto &child_type = ListType::GetChildType(type);
			if (child_type.id() != LogicalTypeId::VARCHAR && child_type.id() != LogicalTypeId::BLOB) {
				throw IOException("Variable '%s' list child type must be VARCHAR or BLOB", pv_value.value);
			}
			auto &children = ListValue::GetChildren(var_result);
			for (const auto &child : children) {
				if (!child.IsNull()) {
					values.push_back(child.ToString());
				}
			}
		} else if (type.id() == LogicalTypeId::VARCHAR || type.id() == LogicalTypeId::BLOB) {
			values.push_back(var_result.ToString());
		} else {
			throw IOException("Variable '%s' must be VARCHAR, BLOB, or list of these", pv_value.value);
		}
		return values;
	};

	// Helper to join two path components with proper delimiter handling
	auto join_paths = [](const string &base, const string &suffix) -> string {
		if (base.empty()) {
			return suffix;
		}
		if (suffix.empty()) {
			return base;
		}
		// Handle trailing slash on base and leading slash on suffix
		bool base_has_slash = (base.back() == '/' || base.back() == '\\');
		bool suffix_has_slash = (suffix.front() == '/' || suffix.front() == '\\');
		if (base_has_slash && suffix_has_slash) {
			return base + suffix.substr(1);
		}
		if (!base_has_slash && !suffix_has_slash) {
			return base + "/" + suffix;
		}
		return base + suffix;
	};

	// Helper to check if a path is a scalarfs protocol (should be passed through without modification)
	auto is_scalarfs_path = [](const string &p) -> bool {
		return StringUtil::StartsWith(p, "data:") || StringUtil::StartsWith(p, "data+varchar:") ||
		       StringUtil::StartsWith(p, "data+blob:") || StringUtil::StartsWith(p, "variable:") ||
		       StringUtil::StartsWith(p, "pathvariable:");
	};

	// Helper to check if a path has an explicit protocol (e.g., s3://, https://, file://)
	auto has_explicit_protocol = [](const string &p) -> bool {
		auto pos = p.find("://");
		// Must have :// and some protocol name before it
		return pos != string::npos && pos > 0 && pos < 20; // reasonable protocol name length
	};

	// Check which passthrough modes are enabled
	bool passthru_scalarfs = parsed.HasModifier(PathVariableModifierFlag::PASSTHRU_SCALARFS);
	bool passthru_explicit = parsed.HasModifier(PathVariableModifierFlag::PASSTHRU_EXPLICIT_FS);

	// Helper to check if a path should be passed through (not modified by append/prepend)
	auto should_passthru = [&](const string &p) -> bool {
		if (passthru_scalarfs && is_scalarfs_path(p)) {
			return true;
		}
		if (passthru_explicit && has_explicit_protocol(p)) {
			return true;
		}
		return false;
	};

	// Check if the pattern contains glob characters
	if (!FileSystem::HasGlob(pattern)) {
		// No glob in variable name - resolve this single variable
		Value result;
		if (!config.GetUserVariable(pattern, result)) {
			return {OpenFileInfo(path)};
		}
		if (result.IsNull()) {
			return {OpenFileInfo(path)};
		}
		if (!extract_paths_from_value(result, resolved_paths)) {
			return {OpenFileInfo(path)};
		}
	} else {
		// Level 1: Glob on variable names
		for (const auto &entry : config.user_variables) {
			const string &var_name = entry.first;
			const Value &var_value = entry.second;

			if (duckdb::Glob(var_name.c_str(), var_name.size(), pattern.c_str(), pattern.size())) {
				extract_paths_from_value(var_value, resolved_paths);
			}
		}
	}

	// Apply prepend modifier (before glob expansion)
	if (parsed.HasModifier(PathVariableModifierFlag::PREPEND)) {
		auto prepend_values = resolve_value(parsed.prepend_value);
		if (!prepend_values.empty()) {
			vector<string> new_paths;
			// Cartesian product: prepend_values × resolved_paths
			// But passthrough paths are kept unmodified
			for (const auto &prefix : prepend_values) {
				for (const auto &p : resolved_paths) {
					if (should_passthru(p)) {
						new_paths.push_back(p);
					} else {
						new_paths.push_back(join_paths(prefix, p));
					}
				}
			}
			resolved_paths = std::move(new_paths);
		}
	}

	// Apply append modifier (before glob expansion)
	if (parsed.HasModifier(PathVariableModifierFlag::APPEND)) {
		auto append_values = resolve_value(parsed.append_value);
		if (!append_values.empty()) {
			vector<string> new_paths;
			// Cartesian product: resolved_paths × append_values
			// But passthrough paths are kept unmodified
			for (const auto &p : resolved_paths) {
				if (should_passthru(p)) {
					new_paths.push_back(p);
				} else {
					for (const auto &suffix : append_values) {
						new_paths.push_back(join_paths(p, suffix));
					}
				}
			}
			resolved_paths = std::move(new_paths);
		}
	}

	// Level 2: Expand globs within paths (unless no-glob modifier is set)
	vector<OpenFileInfo> result;
	bool no_glob = parsed.HasModifier(PathVariableModifierFlag::NO_GLOB);

	for (const string &resolved_path : resolved_paths) {
		if (!no_glob && FileSystem::HasGlob(resolved_path)) {
			auto expanded = parent_fs.Glob(resolved_path, nullptr);
			for (auto &info : expanded) {
				result.push_back(std::move(info));
			}
		} else {
			result.push_back(OpenFileInfo(resolved_path));
		}
	}

	// Apply search modifier FIRST (before sorting) - returns first existing in original order
	// This is important for multi-root search where order matters (local before remote)
	if (parsed.HasModifier(PathVariableModifierFlag::SEARCH)) {
		for (auto &info : result) {
			if (parent_fs.FileExists(info.path, nullptr)) {
				return {std::move(info)};
			}
		}
		// No existing files found - return empty (will cause "no files found" error)
		return {};
	}

	// Apply ignore-missing modifier (filter out non-existent files)
	if (parsed.HasModifier(PathVariableModifierFlag::IGNORE_MISSING)) {
		vector<OpenFileInfo> existing;
		for (auto &info : result) {
			if (parent_fs.FileExists(info.path, nullptr)) {
				existing.push_back(std::move(info));
			}
		}
		result = std::move(existing);
	}

	// Sort for deterministic ordering
	std::sort(result.begin(), result.end(),
	          [](const OpenFileInfo &a, const OpenFileInfo &b) { return a.path < b.path; });

	return result;
}

void PathVariableFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	auto &pv_handle = handle.Cast<PathVariableFileHandle>();
	pv_handle.GetUnderlyingFileSystem().Read(pv_handle.GetUnderlyingHandle(), buffer, nr_bytes, location);
}

int64_t PathVariableFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	auto &pv_handle = handle.Cast<PathVariableFileHandle>();
	return pv_handle.GetUnderlyingFileSystem().Read(pv_handle.GetUnderlyingHandle(), buffer, nr_bytes);
}

void PathVariableFileSystem::Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	auto &pv_handle = handle.Cast<PathVariableFileHandle>();
	pv_handle.GetUnderlyingFileSystem().Write(pv_handle.GetUnderlyingHandle(), buffer, nr_bytes, location);
}

int64_t PathVariableFileSystem::Write(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	auto &pv_handle = handle.Cast<PathVariableFileHandle>();
	return pv_handle.GetUnderlyingFileSystem().Write(pv_handle.GetUnderlyingHandle(), buffer, nr_bytes);
}

int64_t PathVariableFileSystem::GetFileSize(FileHandle &handle) {
	auto &pv_handle = handle.Cast<PathVariableFileHandle>();
	return pv_handle.GetUnderlyingFileSystem().GetFileSize(pv_handle.GetUnderlyingHandle());
}

bool PathVariableFileSystem::FileExists(const string &filename, optional_ptr<FileOpener> opener) {
	if (!CanHandleFile(filename)) {
		return false;
	}

	try {
		string resolved_path = ResolvePath(filename, opener);
		auto &parent_fs = GetParentFileSystem(opener);
		return parent_fs.FileExists(resolved_path, nullptr);
	} catch (...) {
		return false;
	}
}

void PathVariableFileSystem::Seek(FileHandle &handle, idx_t location) {
	auto &pv_handle = handle.Cast<PathVariableFileHandle>();
	pv_handle.GetUnderlyingFileSystem().Seek(pv_handle.GetUnderlyingHandle(), location);
}

idx_t PathVariableFileSystem::SeekPosition(FileHandle &handle) {
	auto &pv_handle = handle.Cast<PathVariableFileHandle>();
	return pv_handle.GetUnderlyingFileSystem().SeekPosition(pv_handle.GetUnderlyingHandle());
}

void PathVariableFileSystem::Reset(FileHandle &handle) {
	auto &pv_handle = handle.Cast<PathVariableFileHandle>();
	pv_handle.GetUnderlyingFileSystem().Reset(pv_handle.GetUnderlyingHandle());
}

bool PathVariableFileSystem::CanSeek() {
	return true;
}

bool PathVariableFileSystem::OnDiskFile(FileHandle &handle) {
	auto &pv_handle = handle.Cast<PathVariableFileHandle>();
	return pv_handle.GetUnderlyingFileSystem().OnDiskFile(pv_handle.GetUnderlyingHandle());
}

timestamp_t PathVariableFileSystem::GetLastModifiedTime(FileHandle &handle) {
	auto &pv_handle = handle.Cast<PathVariableFileHandle>();
	return pv_handle.GetUnderlyingFileSystem().GetLastModifiedTime(pv_handle.GetUnderlyingHandle());
}

void PathVariableFileSystem::RemoveFile(const string &filename, optional_ptr<FileOpener> opener) {
	string resolved_path = ResolvePath(filename, opener);
	auto &parent_fs = GetParentFileSystem(opener);
	parent_fs.RemoveFile(resolved_path, nullptr);
}

bool PathVariableFileSystem::TryRemoveFile(const string &filename, optional_ptr<FileOpener> opener) {
	try {
		RemoveFile(filename, opener);
		return true;
	} catch (...) {
		return false;
	}
}

void PathVariableFileSystem::MoveFile(const string &source, const string &target, optional_ptr<FileOpener> opener) {
	// MoveFile is called by DuckDB's COPY to move temp file to final destination
	// Both source and target should be pathvariable: or tmp_pathvariable: paths

	if (!CanHandleFile(source) || !CanHandleFile(target)) {
		throw IOException("MoveFile: both source and target must be pathvariable: paths");
	}

	// Both paths reference the same variable, but source is tmp_pathvariable: and target is pathvariable:
	// Source resolves to temp path, target resolves to final path
	string source_path = ResolvePath(source, opener);
	string target_path = ResolvePath(target, opener);

	auto &parent_fs = GetParentFileSystem(opener);
	parent_fs.MoveFile(source_path, target_path, nullptr);
}

} // namespace duckdb
