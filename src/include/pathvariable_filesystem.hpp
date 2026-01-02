#pragma once

#include "duckdb.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/open_file_info.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

// =============================================================================
// PathVariableFileSystem
// =============================================================================
//
// A virtual filesystem that treats variable content as a file PATH, then
// delegates all operations to the underlying filesystem.
//
// Comparison with VariableFileSystem:
//   variable:X     - Variable content IS the file content
//   pathvariable:X - Variable content IS A PATH to a file
//
// Example:
//   SET VARIABLE my_path = '/data/input.csv';
//   SELECT * FROM read_csv('pathvariable:my_path');
//   -- Equivalent to: SELECT * FROM read_csv('/data/input.csv');
//
// Two-level glob support:
//   Level 1: Glob on variable names (pathvariable:data_* matches data_01, data_02)
//   Level 2: Glob within paths (if data_01 = '/data/*.csv', expands that glob too)
//
// Temp file handling (for COPY with USE_TMP_FILE):
//   tmp_pathvariable:X -> reads variable X, computes temp path, delegates to parent FS
//   MoveFile handles the temp -> final path transition
//

class PathVariableFileSystem : public FileSystem {
public:
	// FileSystem interface
	unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags,
	                                optional_ptr<FileOpener> opener) override;

	bool CanHandleFile(const string &fpath) override;
	string GetName() const override;

	vector<OpenFileInfo> Glob(const string &path, FileOpener *opener) override;

	// File operations - all delegate to parent filesystem via the handle
	void Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override;
	int64_t Read(FileHandle &handle, void *buffer, int64_t nr_bytes) override;
	void Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override;
	int64_t Write(FileHandle &handle, void *buffer, int64_t nr_bytes) override;
	int64_t GetFileSize(FileHandle &handle) override;
	bool FileExists(const string &filename, optional_ptr<FileOpener> opener) override;
	void Seek(FileHandle &handle, idx_t location) override;
	idx_t SeekPosition(FileHandle &handle) override;
	void Reset(FileHandle &handle) override;
	bool CanSeek() override;
	bool OnDiskFile(FileHandle &handle) override;
	timestamp_t GetLastModifiedTime(FileHandle &handle) override;
	void RemoveFile(const string &filename, optional_ptr<FileOpener> opener) override;
	bool TryRemoveFile(const string &filename, optional_ptr<FileOpener> opener) override;
	void MoveFile(const string &source, const string &target, optional_ptr<FileOpener> opener) override;

private:
	// Extract variable name from pathvariable: or tmp_pathvariable: path
	string ExtractVariableName(const string &path);

	// Check if this is a tmp_pathvariable: path
	bool IsTempPath(const string &path);

	// Resolve the actual file path from a variable
	// Returns the path stored in the variable
	// For tmp_pathvariable:, computes the temp path (prepends tmp_ to filename)
	string ResolvePath(const string &path, optional_ptr<FileOpener> opener);

	// Get the target path from a variable (without temp prefix computation)
	string GetPathFromVariable(const string &var_name, optional_ptr<FileOpener> opener);

	// Compute temp path for a given target path (prepend tmp_ to filename)
	static string ComputeTempPath(const string &target_path);

	// Get the parent filesystem for delegation
	FileSystem &GetParentFileSystem(optional_ptr<FileOpener> opener);
};

// =============================================================================
// PathVariableFileHandle
// =============================================================================
//
// A wrapper handle that holds:
// 1. The underlying file handle from the parent filesystem
// 2. Reference to the parent filesystem for operations
//
// All operations are delegated to the underlying handle's filesystem.
//

class PathVariableFileHandle : public FileHandle {
public:
	PathVariableFileHandle(FileSystem &pathvar_fs, string original_path, unique_ptr<FileHandle> underlying_handle,
	                       FileSystem &underlying_fs);
	void Close() override;

	FileHandle &GetUnderlyingHandle() {
		return *underlying_handle;
	}
	FileSystem &GetUnderlyingFileSystem() {
		return underlying_fs;
	}

private:
	unique_ptr<FileHandle> underlying_handle;
	FileSystem &underlying_fs;
};

} // namespace duckdb
