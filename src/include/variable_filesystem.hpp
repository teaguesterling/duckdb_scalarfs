#pragma once

#include "duckdb.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/open_file_info.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

class VariableFileSystem : public FileSystem {
public:
	// FileSystem interface
	unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags,
	                                optional_ptr<FileOpener> opener) override;

	bool CanHandleFile(const string &fpath) override;
	string GetName() const override;

	vector<OpenFileInfo> Glob(const string &path, FileOpener *opener) override;

	// File operations
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
	string ExtractVariableName(const string &path);
};

// Read handle - holds variable content in memory
class VariableReadHandle : public FileHandle {
public:
	VariableReadHandle(FileSystem &fs, string path, string data);
	void Close() override;

	const string &GetData() const {
		return data;
	}
	idx_t GetPosition() const {
		return position;
	}
	void SetPosition(idx_t pos) {
		position = pos;
	}

private:
	string data;
	idx_t position = 0;
};

// Write handle - accumulates data and writes to variable on close
class VariableWriteHandle : public FileHandle {
public:
	VariableWriteHandle(FileSystem &fs, string path, string var_name, ClientContext &context);
	void Close() override;

	string &GetBuffer() {
		return buffer;
	}
	idx_t GetPosition() const {
		return position;
	}
	void SetPosition(idx_t pos) {
		position = pos;
	}
	ClientContext &GetContext() {
		return context;
	}
	const string &GetVariableName() const {
		return var_name;
	}

private:
	string var_name;
	string buffer;
	idx_t position = 0;
	ClientContext &context;
};

} // namespace duckdb
