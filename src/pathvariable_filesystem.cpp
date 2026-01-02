#include "pathvariable_filesystem.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar/string_common.hpp"
#include "duckdb/main/client_config.hpp"

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
	return StringUtil::StartsWith(fpath, "pathvariable:") || StringUtil::StartsWith(fpath, "tmp_pathvariable:");
}

string PathVariableFileSystem::GetName() const {
	return "PathVariableFileSystem";
}

bool PathVariableFileSystem::IsTempPath(const string &path) {
	return StringUtil::StartsWith(path, "tmp_pathvariable:");
}

string PathVariableFileSystem::ExtractVariableName(const string &path) {
	// Extract variable name from path
	// Both "pathvariable:foo" and "tmp_pathvariable:foo" refer to variable "foo"
	if (StringUtil::StartsWith(path, "tmp_pathvariable:")) {
		return path.substr(17); // len("tmp_pathvariable:")
	}
	return path.substr(13); // len("pathvariable:")
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

	// Type validation: must be VARCHAR or BLOB
	auto &type = result.type();
	if (type.id() != LogicalTypeId::VARCHAR && type.id() != LogicalTypeId::BLOB) {
		throw IOException("Variable '%s' must be VARCHAR or BLOB type to be used as a path, got %s", var_name,
		                  type.ToString());
	}

	return result.ToString();
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
	// Two-level glob implementation:
	// Level 1: Glob on variable names (pathvariable:data_* matches data_01, data_02)
	// Level 2: Glob within paths (if data_01 = '/data/*.csv', expands that too)
	// =============================================================================

	if (!CanHandleFile(path)) {
		return {};
	}

	string pattern = ExtractVariableName(path);

	// Get client context for variable access
	auto context = FileOpener::TryGetClientContext(opener);
	if (!context) {
		// Without context, can't enumerate variables
		return {OpenFileInfo(path)};
	}

	auto &config = ClientConfig::GetConfig(*context);
	auto &parent_fs = FileSystem::GetFileSystem(*context);

	vector<string> resolved_paths;

	// Check if the pattern contains glob characters
	if (!FileSystem::HasGlob(pattern)) {
		// No glob in variable name - resolve this single variable
		// If variable doesn't exist or is invalid, we still try to get the path
		// and let OpenFile handle the error (consistent with variable: behavior)
		Value result;
		if (!config.GetUserVariable(pattern, result)) {
			// Variable not found - return the original path, let OpenFile throw the error
			return {OpenFileInfo(path)};
		}
		if (result.IsNull()) {
			// NULL variable - return the original path, let OpenFile throw the error
			return {OpenFileInfo(path)};
		}

		// Type check
		auto &type = result.type();
		if (type.id() != LogicalTypeId::VARCHAR && type.id() != LogicalTypeId::BLOB) {
			// Wrong type - return the original path, let OpenFile throw the error
			return {OpenFileInfo(path)};
		}

		resolved_paths.push_back(result.ToString());
	} else {
		// Level 1: Glob on variable names
		for (const auto &entry : config.user_variables) {
			const string &var_name = entry.first;
			const Value &var_value = entry.second;

			// Skip NULL or wrong type variables
			if (var_value.IsNull()) {
				continue;
			}
			auto &type = var_value.type();
			if (type.id() != LogicalTypeId::VARCHAR && type.id() != LogicalTypeId::BLOB) {
				continue;
			}

			// Match variable name against pattern
			if (duckdb::Glob(var_name.c_str(), var_name.size(), pattern.c_str(), pattern.size())) {
				resolved_paths.push_back(var_value.ToString());
			}
		}
	}

	// Level 2: Expand any globs within the resolved paths
	// Note: parent_fs is an OpenerFileSystem which already has context,
	// so we pass nullptr to avoid "cannot take an opener" errors
	vector<OpenFileInfo> result;
	for (const string &resolved_path : resolved_paths) {
		if (FileSystem::HasGlob(resolved_path)) {
			// Path contains glob - expand it using parent filesystem
			auto expanded = parent_fs.Glob(resolved_path, nullptr);
			for (auto &info : expanded) {
				result.push_back(std::move(info));
			}
		} else {
			// No glob in path - return as-is, let OpenFile handle errors
			result.push_back(OpenFileInfo(resolved_path));
		}
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
