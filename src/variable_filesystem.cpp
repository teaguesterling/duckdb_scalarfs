#include "variable_filesystem.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/client_config.hpp"

namespace duckdb {

// =============================================================================
// VariableReadHandle Implementation
// =============================================================================

VariableReadHandle::VariableReadHandle(FileSystem &fs, string path, string data_p)
    : FileHandle(fs, std::move(path), FileOpenFlags::FILE_FLAGS_READ), data(std::move(data_p)), position(0) {
}

void VariableReadHandle::Close() {
	// Nothing to clean up
}

// =============================================================================
// VariableWriteHandle Implementation
// =============================================================================

VariableWriteHandle::VariableWriteHandle(FileSystem &fs, string path, string var_name_p, ClientContext &ctx)
    : FileHandle(fs, std::move(path), FileOpenFlags::FILE_FLAGS_WRITE), var_name(std::move(var_name_p)), position(0),
      context(ctx) {
}

void VariableWriteHandle::Close() {
	// Write the accumulated buffer to the variable
	if (buffer.empty()) {
		// Nothing written, don't overwrite existing variable
		return;
	}

	auto &config = ClientConfig::GetConfig(context);

	// Check for null bytes to determine type (BLOB vs VARCHAR)
	bool has_null = memchr(buffer.data(), '\0', buffer.size()) != nullptr;

	if (has_null) {
		config.SetUserVariable(var_name, Value::BLOB(buffer));
	} else {
		config.SetUserVariable(var_name, Value(buffer));
	}
}

// =============================================================================
// VariableFileSystem Implementation
// =============================================================================

bool VariableFileSystem::CanHandleFile(const string &fpath) {
	// Handle both variable: and tmp_variable: prefixes
	//
	// Why tmp_variable:?
	// DuckDB's COPY command uses temp files by default. For a path like "variable:foo",
	// DuckDB creates temp path by splitting into directory + filename, then prepending "tmp_":
	//   - path = "" (empty, no directory component)
	//   - filename = "variable:foo" (entire path is the "filename")
	//   - temp_path = JoinPath("", "tmp_" + "variable:foo") = "tmp_variable:foo"
	//
	// Without handling tmp_variable:, the temp file would go to the local filesystem,
	// then the move operation would fail (cross-filesystem move from local to our virtual FS).
	// By handling tmp_variable: here, the entire temp file flow stays within our filesystem.
	return StringUtil::StartsWith(fpath, "variable:") ||
	       StringUtil::StartsWith(fpath, "tmp_variable:");
}

string VariableFileSystem::GetName() const {
	return "VariableFileSystem";
}

string VariableFileSystem::ExtractVariableName(const string &path) {
	// Extract the DuckDB variable name from a path
	//
	// Path mapping:
	//   "variable:foo"     -> variable name "foo"
	//   "tmp_variable:foo" -> variable name "tmp_foo"
	//
	// The tmp_ prefix is preserved in the variable name so that:
	//   1. Temp variables don't collide with user variables
	//   2. MoveFile(tmp_variable:foo, variable:foo) correctly moves tmp_foo -> foo
	//   3. After the move, tmp_foo is deleted and foo contains the data
	if (StringUtil::StartsWith(path, "tmp_variable:")) {
		return "tmp_" + path.substr(13); // len("tmp_variable:")
	}
	return path.substr(9); // len("variable:")
}

unique_ptr<FileHandle> VariableFileSystem::OpenFile(const string &path, FileOpenFlags flags,
                                                     optional_ptr<FileOpener> opener) {
	string var_name = ExtractVariableName(path);

	auto context = FileOpener::TryGetClientContext(opener);
	if (!context) {
		throw IOException("Cannot access variables without client context");
	}

	if (flags.OpenForWriting()) {
		// Write mode - create a write handle that accumulates data
		return make_uniq<VariableWriteHandle>(*this, path, var_name, *context);
	}

	// Read mode - get variable value and create read handle
	auto &config = ClientConfig::GetConfig(*context);
	Value result;
	if (!config.GetUserVariable(var_name, result)) {
		throw IOException("Variable '%s' not found", var_name);
	}
	if (result.IsNull()) {
		throw IOException("Variable '%s' is NULL", var_name);
	}

	string content = result.ToString();
	return make_uniq<VariableReadHandle>(*this, path, std::move(content));
}

vector<OpenFileInfo> VariableFileSystem::Glob(const string &path, FileOpener *opener) {
	return {OpenFileInfo(path)};
}

void VariableFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	auto &read_handle = handle.Cast<VariableReadHandle>();
	const auto &data = read_handle.GetData();

	if (location >= data.size()) {
		return;
	}

	idx_t bytes_to_read = MinValue<idx_t>(nr_bytes, data.size() - location);
	memcpy(buffer, data.data() + location, bytes_to_read);
}

int64_t VariableFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	auto &read_handle = handle.Cast<VariableReadHandle>();
	idx_t pos = read_handle.GetPosition();
	idx_t file_size = GetFileSize(handle);

	if (pos >= file_size) {
		return 0;
	}

	idx_t bytes_to_read = MinValue<idx_t>(nr_bytes, file_size - pos);
	Read(handle, buffer, bytes_to_read, pos);
	read_handle.SetPosition(pos + bytes_to_read);
	return bytes_to_read;
}

void VariableFileSystem::Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	auto &write_handle = handle.Cast<VariableWriteHandle>();
	auto &buf = write_handle.GetBuffer();
	const char *src = static_cast<const char *>(buffer);

	// Ensure buffer is large enough
	if (location + nr_bytes > buf.size()) {
		buf.resize(location + nr_bytes);
	}

	// Copy data using string's replace method
	buf.replace(location, nr_bytes, src, nr_bytes);
}

int64_t VariableFileSystem::Write(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	auto &write_handle = handle.Cast<VariableWriteHandle>();
	auto &buf = write_handle.GetBuffer();
	const char *src = static_cast<const char *>(buffer);

	// Append to buffer (sequential write)
	buf.append(src, nr_bytes);
	write_handle.SetPosition(buf.size());
	return nr_bytes;
}

int64_t VariableFileSystem::GetFileSize(FileHandle &handle) {
	// Check if it's a read or write handle
	if (handle.GetFlags().OpenForWriting()) {
		auto &write_handle = handle.Cast<VariableWriteHandle>();
		return write_handle.GetBuffer().size();
	} else {
		auto &read_handle = handle.Cast<VariableReadHandle>();
		return read_handle.GetData().size();
	}
}

bool VariableFileSystem::FileExists(const string &filename, optional_ptr<FileOpener> opener) {
	if (!CanHandleFile(filename)) {
		return false;
	}

	// If we can't get context, assume the variable exists and let OpenFile handle errors
	auto context = FileOpener::TryGetClientContext(opener);
	if (!context) {
		return true;
	}

	string var_name = ExtractVariableName(filename);
	auto &config = ClientConfig::GetConfig(*context);
	Value result;
	return config.GetUserVariable(var_name, result) && !result.IsNull();
}

void VariableFileSystem::Seek(FileHandle &handle, idx_t location) {
	if (handle.GetFlags().OpenForWriting()) {
		auto &write_handle = handle.Cast<VariableWriteHandle>();
		write_handle.SetPosition(location);
	} else {
		auto &read_handle = handle.Cast<VariableReadHandle>();
		read_handle.SetPosition(location);
	}
}

idx_t VariableFileSystem::SeekPosition(FileHandle &handle) {
	if (handle.GetFlags().OpenForWriting()) {
		auto &write_handle = handle.Cast<VariableWriteHandle>();
		return write_handle.GetPosition();
	} else {
		auto &read_handle = handle.Cast<VariableReadHandle>();
		return read_handle.GetPosition();
	}
}

void VariableFileSystem::Reset(FileHandle &handle) {
	Seek(handle, 0);
}

bool VariableFileSystem::CanSeek() {
	return true;
}

bool VariableFileSystem::OnDiskFile(FileHandle &handle) {
	return false;
}

timestamp_t VariableFileSystem::GetLastModifiedTime(FileHandle &handle) {
	return timestamp_t(0);
}

void VariableFileSystem::RemoveFile(const string &filename, optional_ptr<FileOpener> opener) {
	// Variables can be "removed" by resetting them
	auto context = FileOpener::TryGetClientContext(opener);
	if (context) {
		string var_name = ExtractVariableName(filename);
		auto &config = ClientConfig::GetConfig(*context);
		config.ResetUserVariable(var_name);
	}
}

bool VariableFileSystem::TryRemoveFile(const string &filename, optional_ptr<FileOpener> opener) {
	try {
		RemoveFile(filename, opener);
		return true;
	} catch (...) {
		return false;
	}
}

void VariableFileSystem::MoveFile(const string &source, const string &target, optional_ptr<FileOpener> opener) {
	// Move a variable from source path to target path
	//
	// This is called by DuckDB's COPY command after writing to a temp file:
	//   MoveFile("tmp_variable:foo", "variable:foo")
	//
	// The flow:
	//   1. Read source variable (tmp_foo)
	//   2. Write to target variable (foo)
	//   3. Delete source variable (tmp_foo)
	if (!CanHandleFile(source) || !CanHandleFile(target)) {
		throw IOException("MoveFile: both source and target must be variable: paths");
	}

	auto context = FileOpener::TryGetClientContext(opener);
	if (!context) {
		throw IOException("Cannot move variables without client context");
	}

	string src_var = ExtractVariableName(source);
	string tgt_var = ExtractVariableName(target);

	auto &config = ClientConfig::GetConfig(*context);

	// Read source variable
	Value src_value;
	if (!config.GetUserVariable(src_var, src_value)) {
		throw IOException("Source variable '%s' not found", src_var);
	}

	// Write to target variable
	config.SetUserVariable(tgt_var, src_value);

	// Remove source variable
	config.ResetUserVariable(src_var);
}

} // namespace duckdb
