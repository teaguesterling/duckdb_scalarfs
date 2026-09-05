#include "decompress_filesystem.hpp"
#include "memory_file_handle.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/gzip_file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "zstd.h"
#include "miniz.hpp"

namespace duckdb {

namespace {

// Size of the scratch buffer used while streaming gzip inflate. Bytes are appended
// to the growing output string one buffer at a time so we can enforce the output cap
// incrementally instead of materializing the whole (possibly bomb-sized) result first.
constexpr idx_t GZIP_DECOMPRESS_BUFFER_SIZE = 8192;

// Streaming gzip inflate with a hard output cap. Mirrors DuckDB's
// GZipFileSystem::UncompressGZIPString but aborts cleanly as soon as the running output
// would exceed max_output_bytes (0 == unlimited). The stock helper grows an unbounded
// std::string, so it cannot be used directly against untrusted input.
string UncompressGZIPStringCapped(const string &in, idx_t max_output_bytes) {
	auto data = in.data();
	auto size = in.size();
	auto body_ptr = data;

	duckdb_miniz::mz_stream mz_stream_val;
	memset(&mz_stream_val, 0, sizeof(mz_stream_val));
	auto mz_stream_ptr = &mz_stream_val;

	uint8_t gzip_hdr[GZIP_HEADER_MINSIZE];
	if (size < GZIP_HEADER_MINSIZE) {
		throw IOException("Input is not a GZIP stream");
	}
	memcpy(gzip_hdr, body_ptr, GZIP_HEADER_MINSIZE);
	body_ptr += GZIP_HEADER_MINSIZE;
	GZipFileSystem::VerifyGZIPHeader(gzip_hdr, GZIP_HEADER_MINSIZE, nullptr);

	if (gzip_hdr[3] & GZIP_FLAG_EXTRA) {
		throw IOException("Extra field in a GZIP stream unsupported");
	}
	if (gzip_hdr[3] & GZIP_FLAG_NAME) {
		char c;
		do {
			c = *body_ptr;
			body_ptr++;
		} while (c != '\0' && static_cast<idx_t>(body_ptr - data) < size);
	}

	auto status = duckdb_miniz::mz_inflateInit2(mz_stream_ptr, -MZ_DEFAULT_WINDOW_BITS);
	if (status != duckdb_miniz::MZ_OK) {
		throw InternalException("Failed to initialize miniz");
	}

	auto bytes_remaining = size - static_cast<idx_t>(body_ptr - data);
	mz_stream_ptr->next_in = reinterpret_cast<const unsigned char *>(body_ptr);
	mz_stream_ptr->avail_in = static_cast<unsigned int>(bytes_remaining);

	string decompressed;
	while (status == duckdb_miniz::MZ_OK) {
		unsigned char decompress_buffer[GZIP_DECOMPRESS_BUFFER_SIZE];
		mz_stream_ptr->next_out = decompress_buffer;
		mz_stream_ptr->avail_out = sizeof(decompress_buffer);
		status = duckdb_miniz::mz_inflate(mz_stream_ptr, duckdb_miniz::MZ_NO_FLUSH);
		if (status != duckdb_miniz::MZ_STREAM_END && status != duckdb_miniz::MZ_OK) {
			duckdb_miniz::mz_inflateEnd(mz_stream_ptr);
			throw IOException("Failed to uncompress");
		}
		// Enforce the cap on the cumulative output BEFORE appending this buffer, so we
		// never grow the result string past the limit even for a lying/absent header.
		if (max_output_bytes != 0 && mz_stream_ptr->total_out > max_output_bytes) {
			duckdb_miniz::mz_inflateEnd(mz_stream_ptr);
			throw IOException("Decompressed output exceeds scalarfs_max_decompressed_bytes limit "
			                  "(%llu bytes); refusing to materialize a potential decompression bomb. "
			                  "Raise the limit with SET scalarfs_max_decompressed_bytes if this is expected.",
			                  static_cast<unsigned long long>(max_output_bytes));
		}
		auto new_bytes = mz_stream_ptr->total_out - decompressed.size();
		decompressed.append(reinterpret_cast<char *>(decompress_buffer), new_bytes);
	}
	duckdb_miniz::mz_inflateEnd(mz_stream_ptr);

	if (decompressed.empty()) {
		throw IOException("Failed to uncompress");
	}
	return decompressed;
}

} // namespace

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

idx_t DecompressFileSystem::GetMaxOutputBytes(optional_ptr<FileOpener> opener) {
	auto context = FileOpener::TryGetClientContext(opener);
	if (context) {
		Value value;
		if (context->TryGetCurrentSetting(MAX_OUTPUT_BYTES_SETTING, value) && !value.IsNull()) {
			return value.GetValue<idx_t>();
		}
	}
	return DEFAULT_MAX_OUTPUT_BYTES;
}

string DecompressFileSystem::DecompressContent(const string &compressed, DecompressFormat format,
                                               idx_t max_output_bytes) {
	switch (format) {
	case DecompressFormat::GZIP: {
		if (compressed.empty()) {
			return "";
		}
		// Verify it's actually gzip format
		if (!GZipFileSystem::CheckIsZip(compressed.c_str(), compressed.size())) {
			throw IOException("Content is not in gzip format");
		}
		// Use the capped streaming inflate: gzip carries no trustworthy declared size, so
		// the only robust guard is to bound the accumulator as it grows.
		return UncompressGZIPStringCapped(compressed, max_output_bytes);
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
			// Never trust the attacker-controlled frame header to size the allocation.
			// Reject BEFORE resize() so a ~20-byte frame declaring multi-GB cannot force a
			// zero-filled multi-GB commit.
			if (max_output_bytes != 0 && decompressed_size > max_output_bytes) {
				throw IOException("Zstd frame declares %llu bytes, exceeding "
				                  "scalarfs_max_decompressed_bytes limit (%llu bytes); refusing to "
				                  "materialize a potential decompression bomb. Raise the limit with "
				                  "SET scalarfs_max_decompressed_bytes if this is expected.",
				                  static_cast<unsigned long long>(decompressed_size),
				                  static_cast<unsigned long long>(max_output_bytes));
			}
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

			// Bound the accumulator: an absent frame size (streaming path) must not let a
			// bomb grow the output string without limit.
			if (max_output_bytes != 0 && decompressed.size() + output.pos > max_output_bytes) {
				duckdb_zstd::ZSTD_freeDStream(dstream);
				throw IOException("Decompressed output exceeds scalarfs_max_decompressed_bytes limit "
				                  "(%llu bytes); refusing to materialize a potential decompression bomb. "
				                  "Raise the limit with SET scalarfs_max_decompressed_bytes if this is expected.",
				                  static_cast<unsigned long long>(max_output_bytes));
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

	// Decompress the content, capping the materialized output to guard against bombs.
	idx_t max_output_bytes = GetMaxOutputBytes(opener);
	string decompressed = DecompressContent(compressed_content, format, max_output_bytes);

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

// Flat namespace: a decompress+*: path names a decoded stream, not a path. Nothing here is ever a directory, so the
// answer is always false. It must still be answered rather than left to FileSystem's base implementation, which throws
// "not implemented" -- DuckDB v2.0's COPY TO probes the target with DirectoryExists() before writing.
bool DecompressFileSystem::DirectoryExists(const string &directory, optional_ptr<FileOpener> opener) {
	return false;
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
