#include "decompress_filesystem.hpp"
#include "memory_file_handle.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/gzip_file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "zstd.h"

namespace duckdb {

// =============================================================================
// FileSystem Interface Implementation
// =============================================================================

bool DecompressFileSystem::CanHandleFile(const string &fpath) {
	return StringUtil::StartsWith(fpath, GZIP_PREFIX) || StringUtil::StartsWith(fpath, ZSTD_PREFIX);
}

string DecompressFileSystem::GetName() const {
	return "DecompressFileSystem";
}

bool DecompressFileSystem::ParseProtocol(const string &path, DecompressFormat &format, string &underlying_path) {
	if (StringUtil::StartsWith(path, GZIP_PREFIX)) {
		format = DecompressFormat::GZIP;
		underlying_path = path.substr(strlen(GZIP_PREFIX));
		return true;
	}
	if (StringUtil::StartsWith(path, ZSTD_PREFIX)) {
		format = DecompressFormat::ZSTD;
		underlying_path = path.substr(strlen(ZSTD_PREFIX));
		return true;
	}
	return false;
}

FileSystem &DecompressFileSystem::GetParentFileSystem(optional_ptr<FileOpener> opener) {
	auto context = FileOpener::TryGetClientContext(opener);
	if (!context) {
		throw IOException("Cannot access filesystem without client context");
	}
	return FileSystem::GetFileSystem(*context);
}

string DecompressFileSystem::DecompressContent(const string &compressed, DecompressFormat format) {
	switch (format) {
	case DecompressFormat::GZIP: {
		if (compressed.empty()) {
			return "";
		}
		// Verify it's actually gzip format
		if (!GZipFileSystem::CheckIsZip(compressed.c_str(), compressed.size())) {
			throw IOException("Content is not in gzip format");
		}
		return GZipFileSystem::UncompressGZIPString(compressed);
	}
	case DecompressFormat::ZSTD: {
		if (compressed.empty()) {
			return "";
		}
		// Check zstd magic number (0xFD2FB528 little-endian)
		if (compressed.size() < 4) {
			throw IOException("Content is not in zstd format");
		}
		uint32_t magic;
		memcpy(&magic, compressed.data(), 4);
		if (magic != 0xFD2FB528) {
			throw IOException("Content is not in zstd format");
		}

		// Get decompressed size from frame header (if available)
		unsigned long long decompressed_size =
		    duckdb_zstd::ZSTD_getFrameContentSize(compressed.data(), compressed.size());

		if (decompressed_size == ZSTD_CONTENTSIZE_ERROR) {
			throw IOException("Invalid zstd frame header");
		}

		// If content size is known, use single-shot decompression
		if (decompressed_size != ZSTD_CONTENTSIZE_UNKNOWN) {
			string decompressed;
			decompressed.resize(decompressed_size);

			size_t result = duckdb_zstd::ZSTD_decompress((void *)decompressed.data(), decompressed.size(),
			                                             compressed.data(), compressed.size());

			if (duckdb_zstd::ZSTD_isError(result)) {
				throw IOException("Zstd decompression failed: %s", duckdb_zstd::ZSTD_getErrorName(result));
			}

			return decompressed;
		}

		// Content size unknown - use streaming decompression
		auto dstream = duckdb_zstd::ZSTD_createDStream();
		if (!dstream) {
			throw IOException("Failed to create zstd decompression stream");
		}

		size_t init_result = duckdb_zstd::ZSTD_initDStream(dstream);
		if (duckdb_zstd::ZSTD_isError(init_result)) {
			duckdb_zstd::ZSTD_freeDStream(dstream);
			throw IOException("Failed to initialize zstd stream: %s", duckdb_zstd::ZSTD_getErrorName(init_result));
		}

		string decompressed;
		size_t out_buf_size = duckdb_zstd::ZSTD_DStreamOutSize();
		auto out_buf = make_unsafe_uniq_array<char>(out_buf_size);

		duckdb_zstd::ZSTD_inBuffer input = {compressed.data(), compressed.size(), 0};

		while (input.pos < input.size) {
			duckdb_zstd::ZSTD_outBuffer output = {out_buf.get(), out_buf_size, 0};

			size_t ret = duckdb_zstd::ZSTD_decompressStream(dstream, &output, &input);
			if (duckdb_zstd::ZSTD_isError(ret)) {
				duckdb_zstd::ZSTD_freeDStream(dstream);
				throw IOException("Zstd streaming decompression failed: %s", duckdb_zstd::ZSTD_getErrorName(ret));
			}

			decompressed.append(out_buf.get(), output.pos);
		}

		duckdb_zstd::ZSTD_freeDStream(dstream);
		return decompressed;
	}
	default:
		throw IOException("Unknown decompression format");
	}
}

unique_ptr<FileHandle> DecompressFileSystem::OpenFile(const string &path, FileOpenFlags flags,
                                                      optional_ptr<FileOpener> opener) {
	if (flags.OpenForWriting()) {
		throw IOException("decompress protocols are read-only");
	}

	DecompressFormat format;
	string underlying_path;
	if (!ParseProtocol(path, format, underlying_path)) {
		throw IOException("Invalid decompress protocol path: %s", path);
	}

	// Get the parent filesystem and read the compressed content
	auto &parent_fs = GetParentFileSystem(opener);

	// Open and read the entire compressed file
	auto underlying_handle = parent_fs.OpenFile(underlying_path, FileOpenFlags::FILE_FLAGS_READ, nullptr);
	idx_t file_size = parent_fs.GetFileSize(*underlying_handle);

	string compressed_content;
	compressed_content.resize(file_size);
	if (file_size > 0) {
		parent_fs.Read(*underlying_handle, (void *)compressed_content.data(), file_size, 0);
	}
	underlying_handle->Close();

	// Decompress the content
	string decompressed = DecompressContent(compressed_content, format);

	return make_uniq<MemoryFileHandle>(*this, path, std::move(decompressed));
}

vector<OpenFileInfo> DecompressFileSystem::Glob(const string &path, FileOpener *opener) {
	// Decompress protocols don't glob - just return the path itself
	return {OpenFileInfo(path)};
}

void DecompressFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	auto &mem_handle = handle.Cast<MemoryFileHandle>();
	const auto &data = mem_handle.GetData();

	if (location >= data.size()) {
		return;
	}

	idx_t bytes_to_read = MinValue<idx_t>(nr_bytes, data.size() - location);
	memcpy(buffer, data.data() + location, bytes_to_read);
}

int64_t DecompressFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	auto &mem_handle = handle.Cast<MemoryFileHandle>();
	idx_t pos = mem_handle.GetPosition();
	idx_t file_size = GetFileSize(handle);

	if (pos >= file_size) {
		return 0;
	}

	idx_t bytes_to_read = MinValue<idx_t>(nr_bytes, file_size - pos);
	Read(handle, buffer, bytes_to_read, pos);
	mem_handle.SetPosition(pos + bytes_to_read);
	return bytes_to_read;
}

void DecompressFileSystem::Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	throw IOException("decompress protocols are read-only");
}

int64_t DecompressFileSystem::Write(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	throw IOException("decompress protocols are read-only");
}

int64_t DecompressFileSystem::GetFileSize(FileHandle &handle) {
	auto &mem_handle = handle.Cast<MemoryFileHandle>();
	return mem_handle.GetData().size();
}

bool DecompressFileSystem::FileExists(const string &filename, optional_ptr<FileOpener> opener) {
	DecompressFormat format;
	string underlying_path;
	if (!ParseProtocol(filename, format, underlying_path)) {
		return false;
	}

	try {
		auto &parent_fs = GetParentFileSystem(opener);
		return parent_fs.FileExists(underlying_path, nullptr);
	} catch (...) {
		return false;
	}
}

void DecompressFileSystem::Seek(FileHandle &handle, idx_t location) {
	auto &mem_handle = handle.Cast<MemoryFileHandle>();
	mem_handle.SetPosition(location);
}

idx_t DecompressFileSystem::SeekPosition(FileHandle &handle) {
	auto &mem_handle = handle.Cast<MemoryFileHandle>();
	return mem_handle.GetPosition();
}

void DecompressFileSystem::Reset(FileHandle &handle) {
	Seek(handle, 0);
}

bool DecompressFileSystem::CanSeek() {
	return true;
}

bool DecompressFileSystem::OnDiskFile(FileHandle &handle) {
	return false;
}

timestamp_t DecompressFileSystem::GetLastModifiedTime(FileHandle &handle) {
	// Decompressed content has no modification time - return epoch
	return timestamp_t(0);
}

void DecompressFileSystem::RemoveFile(const string &filename, optional_ptr<FileOpener> opener) {
	throw IOException("decompress protocols are read-only");
}

bool DecompressFileSystem::TryRemoveFile(const string &filename, optional_ptr<FileOpener> opener) {
	return false;
}

} // namespace duckdb
