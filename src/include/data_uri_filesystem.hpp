#pragma once

#include "duckdb.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/open_file_info.hpp"

namespace duckdb {

class DataURIFileSystem : public FileSystem {
public:
	// FileSystem interface
	unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags, optional_ptr<FileOpener> opener) override;

	bool CanHandleFile(const string &fpath) override;
	string GetName() const override;

	// Glob just returns the path itself (data URIs don't expand)
	vector<OpenFileInfo> Glob(const string &path, FileOpener *opener) override;

	// File operations - data URIs are virtual, these don't apply
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

private:
	// Protocol parsing
	string ParseDataURI(const string &uri);
	string ParseVarcharURI(const string &uri);
	string ParseBlobURI(const string &uri);

	// Decoding helpers
	string DecodeURLEncoded(const string &input);
	string DecodeBase64(const string &input);
	string DecodeBlobEscapes(const string &input);
};

} // namespace duckdb
