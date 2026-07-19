#pragma once

#include "duckdb.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/open_file_info.hpp"
#include "duckdb/main/client_context.hpp"
#include <utility>

namespace duckdb {

// =============================================================================
// PathMacroFileSystem  —  protocol: pathmacro:
// =============================================================================
//
// A RESOLVER-only virtual filesystem: it never serves bytes. Its Glob() runs a
// registered scalar macro that returns VARCHAR[] (a list of real paths), and
// DuckDB then re-dispatches OpenFile per returned path to whatever filesystem
// owns it (local, s3, ...). Same shape as PathVariableFileSystem, except the
// resolution source is a SQL macro instead of a variable value.
//
// URL scheme:
//   pathmacro:<macro>?<k>=<v>&<k>=<v>...
//
// Contract:
//   The macro is a SCALAR macro `name(p MAP(VARCHAR,VARCHAR)) -> LIST(VARCHAR)`.
//   Glob invokes it as `SELECT <macro>(map([keys],[vals]))` over a fresh
//   Connection (mirrors how plinking_duck loads parquet companions during bind).
//
// Example:
//   SET allowed_pathmacros = 'region_files';
//   CREATE MACRO region_files(p) AS (
//     SELECT list(DISTINCT file_path) FROM 'catalog.parquet'
//     WHERE region = p['region']
//       AND (NOT map_contains(p,'from') OR year >= p['from']::BIGINT)
//       AND (NOT map_contains(p,'to')   OR year <= p['to']::BIGINT));
//   SELECT * FROM read_parquet('pathmacro:region_files?region=west&from=2020&to=2023');
//
// Security:
//   - The macro NAME is an SQL identifier (can't be a bound param), so it is
//     gated by the `allowed_pathmacros` setting AND an identifier check.
//   - Param keys/values are passed as MAP *data* (escaped), never concatenated
//     into SQL, so values carry no injection surface.
//
class PathMacroFileSystem : public FileSystem {
public:
	bool CanHandleFile(const string &fpath) override;
	string GetName() const override {
		return "PathMacroFileSystem";
	}

	// Parsed form of a pathmacro: URL. Public so the to_/from_pathmacro_url scalar
	// functions build and parse URLs through the SAME code the resolver uses.
	struct ParsedPathMacro {
		string macro_name;
		vector<std::pair<string, string>> params; // ordered; keys/vals align
		bool is_temp = false;                     // path was tmp_pathmacro: (COPY's atomic-write temp)
	};

	static ParsedPathMacro Parse(const string &path); // strip prefix, split ?/&/=, url-decode
	static bool IsSafeIdentifier(const string &name); // [A-Za-z_][A-Za-z0-9_]*

	// The only method that matters: resolve params -> real paths.
	vector<OpenFileInfo> Glob(const string &path, FileOpener *opener) override;

	// Direct open (no glob) — resolve to a single path and delegate to the parent
	// FS; error if the macro yields != 1 path (use a globbing reader for many).
	unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags, optional_ptr<FileOpener> opener) override;
	bool FileExists(const string &filename, optional_ptr<FileOpener> opener) override;
	void RemoveFile(const string &filename, optional_ptr<FileOpener> opener) override;
	bool TryRemoveFile(const string &filename, optional_ptr<FileOpener> opener) override;
	void MoveFile(const string &source, const string &target, optional_ptr<FileOpener> opener) override;

	// Byte ops delegate to the wrapped underlying handle (see PathMacroFileHandle).
	void Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override;
	int64_t Read(FileHandle &handle, void *buffer, int64_t nr_bytes) override;
	void Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override;
	int64_t Write(FileHandle &handle, void *buffer, int64_t nr_bytes) override;
	int64_t GetFileSize(FileHandle &handle) override;
	void Seek(FileHandle &handle, idx_t location) override;
	idx_t SeekPosition(FileHandle &handle) override;
	void Reset(FileHandle &handle) override;
	bool CanSeek() override {
		return true;
	}
	bool OnDiskFile(FileHandle &handle) override;
	timestamp_t GetLastModifiedTime(FileHandle &handle) override;

private:
	static string EscapeSingle(const string &s); // ' -> ''
	static bool IsAllowedMacro(ClientContext &context, const string &name);

	// Run the macro and return its VARCHAR[] result as real paths (no Level-2 glob).
	vector<string> ResolvePaths(ClientContext &context, const ParsedPathMacro &parsed);
};

// Thin handle wrapper that forwards byte ops to the underlying (parent-FS) handle.
class PathMacroFileHandle : public FileHandle {
public:
	PathMacroFileHandle(FileSystem &fs, string path, unique_ptr<FileHandle> underlying, FileSystem &underlying_fs);
	void Close() override;
	FileHandle &Underlying() {
		return *underlying;
	}
	FileSystem &UnderlyingFS() {
		return underlying_fs;
	}

private:
	unique_ptr<FileHandle> underlying;
	FileSystem &underlying_fs;
};

} // namespace duckdb
