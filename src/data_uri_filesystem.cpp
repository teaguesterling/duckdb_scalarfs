#include "data_uri_filesystem.hpp"
#include "memory_file_handle.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/open_file_info.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/common/types/timestamp.hpp"

namespace duckdb {

// =============================================================================
// FileSystem Interface Implementation
// =============================================================================

bool DataURIFileSystem::CanHandleFile(const string &fpath) {
	return StringUtil::StartsWith(fpath, "data:") || StringUtil::StartsWith(fpath, "data+varchar:") ||
	       StringUtil::StartsWith(fpath, "data+blob:");
}

string DataURIFileSystem::GetName() const {
	return "DataURIFileSystem";
}

unique_ptr<FileHandle> DataURIFileSystem::OpenFile(const string &path, FileOpenFlags flags,
                                                   optional_ptr<FileOpener> opener) {
	if (flags.OpenForWriting()) {
		throw IOException("Data URIs are read-only");
	}

	string content;
	if (StringUtil::StartsWith(path, "data+varchar:")) {
		content = ParseVarcharURI(path);
	} else if (StringUtil::StartsWith(path, "data+blob:")) {
		content = ParseBlobURI(path);
	} else if (StringUtil::StartsWith(path, "data:")) {
		content = ParseDataURI(path);
	} else {
		throw IOException("Unknown data URI protocol: %s", path);
	}

	return make_uniq<MemoryFileHandle>(*this, path, std::move(content));
}

vector<OpenFileInfo> DataURIFileSystem::Glob(const string &path, FileOpener *opener) {
	// Data URIs don't glob - just return the path itself
	return {OpenFileInfo(path)};
}

void DataURIFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	auto &mem_handle = handle.Cast<MemoryFileHandle>();
	const auto &data = mem_handle.GetData();

	if (location >= data.size()) {
		return;
	}

	idx_t bytes_to_read = MinValue<idx_t>(nr_bytes, data.size() - location);
	memcpy(buffer, data.data() + location, bytes_to_read);
}

int64_t DataURIFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes) {
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

void DataURIFileSystem::Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	throw IOException("Data URIs are read-only");
}

int64_t DataURIFileSystem::Write(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	throw IOException("Data URIs are read-only");
}

int64_t DataURIFileSystem::GetFileSize(FileHandle &handle) {
	auto &mem_handle = handle.Cast<MemoryFileHandle>();
	return mem_handle.GetData().size();
}

bool DataURIFileSystem::FileExists(const string &filename, optional_ptr<FileOpener> opener) {
	// Data URIs always "exist" if they're well-formed
	return CanHandleFile(filename);
}

void DataURIFileSystem::Seek(FileHandle &handle, idx_t location) {
	auto &mem_handle = handle.Cast<MemoryFileHandle>();
	mem_handle.SetPosition(location);
}

idx_t DataURIFileSystem::SeekPosition(FileHandle &handle) {
	auto &mem_handle = handle.Cast<MemoryFileHandle>();
	return mem_handle.GetPosition();
}

void DataURIFileSystem::Reset(FileHandle &handle) {
	Seek(handle, 0);
}

bool DataURIFileSystem::CanSeek() {
	return true;
}

bool DataURIFileSystem::OnDiskFile(FileHandle &handle) {
	return false;
}

timestamp_t DataURIFileSystem::GetLastModifiedTime(FileHandle &handle) {
	// Data URIs have no modification time - return epoch
	return timestamp_t(0);
}

void DataURIFileSystem::RemoveFile(const string &filename, optional_ptr<FileOpener> opener) {
	throw IOException("Data URIs are read-only");
}

bool DataURIFileSystem::TryRemoveFile(const string &filename, optional_ptr<FileOpener> opener) {
	// Don't throw, just return false - data URIs can't be removed
	return false;
}

// =============================================================================
// Protocol Parsing
// =============================================================================

string DataURIFileSystem::ParseDataURI(const string &uri) {
	// Format: data:[<mediatype>][;base64],<data>
	auto comma_pos = uri.find(',');
	if (comma_pos == string::npos) {
		throw IOException("Invalid data: URI - missing comma separator");
	}

	// Extract metadata (everything between "data:" and the comma)
	string metadata = uri.substr(5, comma_pos - 5);
	string data = uri.substr(comma_pos + 1);

	// Check for base64 encoding marker
	bool is_base64 = metadata.find(";base64") != string::npos;

	if (is_base64) {
		return DecodeBase64(data);
	} else {
		return DecodeURLEncoded(data);
	}
}

string DataURIFileSystem::ParseVarcharURI(const string &uri) {
	// Format: data+varchar:<content>
	// Everything after the prefix is literal content
	return uri.substr(13); // len("data+varchar:")
}

string DataURIFileSystem::ParseBlobURI(const string &uri) {
	// Format: data+blob:<escaped_content>
	string content = uri.substr(10); // len("data+blob:")
	return DecodeBlobEscapes(content);
}

// =============================================================================
// Decoding Helpers
// =============================================================================

string DataURIFileSystem::DecodeURLEncoded(const string &input) {
	string result;
	result.reserve(input.size());

	for (size_t i = 0; i < input.size(); i++) {
		if (input[i] == '%') {
			if (i + 2 >= input.size()) {
				throw IOException("Invalid URL encoding - incomplete '%%' escape at position %llu",
				                  (unsigned long long)i);
			}
			char c1 = input[i + 1];
			char c2 = input[i + 2];
			// Validate hex characters
			bool valid_hex = ((c1 >= '0' && c1 <= '9') || (c1 >= 'A' && c1 <= 'F') || (c1 >= 'a' && c1 <= 'f')) &&
			                 ((c2 >= '0' && c2 <= '9') || (c2 >= 'A' && c2 <= 'F') || (c2 >= 'a' && c2 <= 'f'));
			if (!valid_hex) {
				throw IOException("Invalid URL encoding - '%%%c%c' is not valid hex at position %llu", c1, c2,
				                  (unsigned long long)i);
			}
			char hex[3] = {c1, c2, '\0'};
			long val = strtol(hex, nullptr, 16);
			result += static_cast<char>(val);
			i += 2;
		} else {
			result += input[i];
		}
	}

	return result;
}

string DataURIFileSystem::DecodeBase64(const string &input) {
	if (input.empty()) {
		return "";
	}
	// Use DuckDB's built-in base64 decoder
	return Blob::FromBase64(string_t(input));
}

string DataURIFileSystem::DecodeBlobEscapes(const string &input) {
	string result;
	result.reserve(input.size());

	for (size_t i = 0; i < input.size(); i++) {
		if (input[i] != '\\') {
			result += input[i];
			continue;
		}

		// Escape sequence starts here
		if (i + 1 >= input.size()) {
			throw IOException("Invalid escape sequence at end of data+blob: URI");
		}

		char next = input[i + 1];
		switch (next) {
		case '\\':
			result += '\\';
			i++;
			break;
		case 'n':
			result += '\n';
			i++;
			break;
		case 'r':
			result += '\r';
			i++;
			break;
		case 't':
			result += '\t';
			i++;
			break;
		case '0':
			result += '\0';
			i++;
			break;
		case 'x': {
			// Hex escape: \xNN
			if (i + 3 >= input.size()) {
				throw IOException("Invalid \\x escape at position %llu: expected 2 hex digits", (unsigned long long)i);
			}
			char hex[3] = {input[i + 2], input[i + 3], '\0'};
			char *end;
			long val = strtol(hex, &end, 16);
			if (end != hex + 2) {
				throw IOException("Invalid \\x escape at position %llu: '%s' is not valid hex", (unsigned long long)i,
				                  string(hex));
			}
			result += static_cast<char>(val);
			i += 3;
			break;
		}
		default:
			throw IOException("Invalid escape sequence '\\%c' at position %llu", next, (unsigned long long)i);
		}
	}

	return result;
}

} // namespace duckdb
