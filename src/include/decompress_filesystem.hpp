#pragma once

#include "duckdb.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/open_file_info.hpp"

namespace duckdb {

// =============================================================================
// DecompressFileSystem
// =============================================================================
//
// A virtual filesystem that decompresses content from an underlying source.
//
// Protocols:
//   decompress+gz:<path>   - Decompress gzip content
//   decompress+zstd:<path> - Decompress zstd content (future)
//
// Examples:
//   decompress+gz:variable:compressed_data
//   decompress+gz:data:;base64,H4sIAAAA...
//   decompress+gz:/path/to/file.bin
//   decompress+gz:pathvariable:blob_path
//
// The protocol wraps any other path/protocol and decompresses the content
// on read. Write operations are not supported.
//

enum class DecompressFormat {
	GZIP,
	ZSTD
};

class DecompressFileSystem : public FileSystem {
public:
	// FileSystem interface
	unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags, optional_ptr<FileOpener> opener) override;

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

	// Protocol prefixes
	static constexpr const char *GZIP_PREFIX = "decompress+gz:";
	static constexpr const char *ZSTD_PREFIX = "decompress+zstd:";

private:
	// Parse the protocol and extract format + underlying path
	static bool ParseProtocol(const string &path, DecompressFormat &format, string &underlying_path);

	// Get the parent filesystem for delegation
	FileSystem &GetParentFileSystem(optional_ptr<FileOpener> opener);

	// Decompress content based on format
	static string DecompressContent(const string &compressed, DecompressFormat format);
};

} // namespace duckdb
