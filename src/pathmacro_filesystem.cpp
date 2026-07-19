#include "pathmacro_filesystem.hpp"
#include "string_encodings.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include <cctype>

namespace duckdb {

static constexpr const char *PATHMACRO_PREFIX = "pathmacro:";

// ---------------------------------------------------------------------------
// Handle
// ---------------------------------------------------------------------------
PathMacroFileHandle::PathMacroFileHandle(FileSystem &fs, string path, unique_ptr<FileHandle> underlying_p,
                                       FileSystem &underlying_fs_p)
    : FileHandle(fs, std::move(path), underlying_p->GetFlags()), underlying(std::move(underlying_p)),
      underlying_fs(underlying_fs_p) {
}
void PathMacroFileHandle::Close() {
	if (underlying) {
		underlying->Close();
	}
}

// ---------------------------------------------------------------------------
// Parsing / helpers
// ---------------------------------------------------------------------------
bool PathMacroFileSystem::CanHandleFile(const string &fpath) {
	return StringUtil::StartsWith(fpath, PATHMACRO_PREFIX);
}

PathMacroFileSystem::ParsedPathMacro PathMacroFileSystem::Parse(const string &path) {
	ParsedPathMacro r;
	string rest = path.substr(strlen(PATHMACRO_PREFIX));
	auto qpos = rest.find('?');
	r.macro_name = (qpos == string::npos) ? rest : rest.substr(0, qpos);
	if (qpos == string::npos) {
		return r;
	}
	string qs = rest.substr(qpos + 1);
	size_t start = 0;
	while (start <= qs.size()) {
		auto amp = qs.find('&', start);
		string pair = qs.substr(start, amp == string::npos ? string::npos : amp - start);
		auto eq = pair.find('=');
		if (eq != string::npos && eq > 0) {
			r.params.emplace_back(DecodeURLEncoded(pair.substr(0, eq)), DecodeURLEncoded(pair.substr(eq + 1)));
		}
		if (amp == string::npos) {
			break;
		}
		start = amp + 1;
	}
	return r;
}

bool PathMacroFileSystem::IsSafeIdentifier(const string &name) {
	if (name.empty()) {
		return false;
	}
	if (!(isalpha((unsigned char)name[0]) || name[0] == '_')) {
		return false;
	}
	for (char c : name) {
		if (!(isalnum((unsigned char)c) || c == '_')) {
			return false;
		}
	}
	return true;
}

string PathMacroFileSystem::EscapeSingle(const string &s) {
	return StringUtil::Replace(s, "'", "''");
}

bool PathMacroFileSystem::IsAllowedMacro(ClientContext &context, const string &name) {
	Value setting;
	if (!context.TryGetCurrentSetting("allowed_pathmacros", setting) || setting.IsNull()) {
		return false; // opt-in: nothing allowed until explicitly set
	}
	auto allowed = StringUtil::Split(setting.ToString(), ',');
	for (auto &a : allowed) {
		StringUtil::Trim(a); // in-place; returns void
		if (a == name) {
			return true;
		}
	}
	return false;
}

// ---------------------------------------------------------------------------
// Core: run the macro, return VARCHAR[] as real paths
// ---------------------------------------------------------------------------
vector<string> PathMacroFileSystem::ResolvePaths(ClientContext &context, const ParsedPathMacro &parsed) {
	if (!IsSafeIdentifier(parsed.macro_name)) {
		throw IOException("pathmacro: invalid macro name '%s'", parsed.macro_name);
	}
	if (!IsAllowedMacro(context, parsed.macro_name)) {
		throw IOException("pathmacro: macro '%s' is not in allowed_pathmacros "
		                  "(SET allowed_pathmacros = '%s,...')",
		                  parsed.macro_name, parsed.macro_name);
	}

	// Build `SELECT <macro>(map([keys],[vals]))`. Keys/vals are DATA (escaped),
	// only the macro name is an identifier (already validated + allow-listed).
	string keys, vals;
	for (auto &kv : parsed.params) {
		if (!keys.empty()) {
			keys += ", ";
			vals += ", ";
		}
		keys += "'" + EscapeSingle(kv.first) + "'";
		vals += "'" + EscapeSingle(kv.second) + "'";
	}
	string sql = "SELECT " + parsed.macro_name + "(map([" + keys + "], [" + vals + "]))";

	// Nested query on a fresh connection — same pattern plinking_duck uses to read
	// parquet companions during bind.
	Connection conn(DatabaseInstance::GetDatabase(context));
	auto result = conn.Query(sql);
	if (result->HasError()) {
		throw IOException("pathmacro: catalog macro '%s' failed: %s", parsed.macro_name, result->GetError());
	}
	auto val = result->GetValue(0, 0);
	if (val.IsNull()) {
		return {};
	}
	if (val.type().id() != LogicalTypeId::LIST || ListType::GetChildType(val.type()).id() != LogicalTypeId::VARCHAR) {
		throw IOException("pathmacro: macro '%s' must return VARCHAR[] (a list of paths), got %s", parsed.macro_name,
		                  val.type().ToString());
	}
	vector<string> paths;
	for (auto &child : ListValue::GetChildren(val)) {
		if (!child.IsNull()) {
			paths.push_back(child.ToString());
		}
	}
	return paths;
}

vector<OpenFileInfo> PathMacroFileSystem::Glob(const string &path, FileOpener *opener) {
	if (!CanHandleFile(path)) {
		return {};
	}
	auto context = FileOpener::TryGetClientContext(opener);
	if (!context) {
		// No client context (e.g. db-level opener) — can't run the macro.
		return {OpenFileInfo(path)};
	}
	auto parsed = Parse(path);
	auto paths = ResolvePaths(*context, parsed);

	auto &parent_fs = FileSystem::GetFileSystem(*context);
	vector<OpenFileInfo> out;
	for (auto &p : paths) {
		// Level-2: allow the macro to return globs (e.g. a directory of shards).
		if (FileSystem::HasGlob(p)) {
			for (auto &info : parent_fs.Glob(p, nullptr)) {
				out.push_back(std::move(info));
			}
		} else {
			out.push_back(OpenFileInfo(p));
		}
	}
	return out;
}

// ---------------------------------------------------------------------------
// Direct open (rarely hit — globbing readers use Glob)
// ---------------------------------------------------------------------------
unique_ptr<FileHandle> PathMacroFileSystem::OpenFile(const string &path, FileOpenFlags flags,
                                                    optional_ptr<FileOpener> opener) {
	auto context = FileOpener::TryGetClientContext(opener);
	if (!context) {
		throw IOException("pathmacro: cannot resolve '%s' without a client context", path);
	}
	auto paths = ResolvePaths(*context, Parse(path));
	if (paths.size() != 1) {
		throw IOException("pathmacro: '%s' resolved to %llu paths; open a single path or use a globbing reader "
		                  "(read_parquet/read_csv/read_json)",
		                  path, (unsigned long long)paths.size());
	}
	auto &parent_fs = FileSystem::GetFileSystem(*context);
	auto underlying = parent_fs.OpenFile(paths[0], flags, nullptr);
	return make_uniq<PathMacroFileHandle>(*this, path, std::move(underlying), parent_fs);
}

bool PathMacroFileSystem::FileExists(const string &filename, optional_ptr<FileOpener> opener) {
	if (!CanHandleFile(filename)) {
		return false;
	}
	try {
		auto context = FileOpener::TryGetClientContext(opener);
		if (!context) {
			return false;
		}
		auto paths = ResolvePaths(*context, Parse(filename));
		auto &parent_fs = FileSystem::GetFileSystem(*context);
		return paths.size() == 1 && parent_fs.FileExists(paths[0], nullptr);
	} catch (...) {
		return false;
	}
}

// ---------------------------------------------------------------------------
// Byte ops -> underlying handle
// ---------------------------------------------------------------------------
void PathMacroFileSystem::Read(FileHandle &h, void *buffer, int64_t nr, idx_t loc) {
	auto &ph = h.Cast<PathMacroFileHandle>();
	ph.UnderlyingFS().Read(ph.Underlying(), buffer, nr, loc);
}
int64_t PathMacroFileSystem::Read(FileHandle &h, void *buffer, int64_t nr) {
	auto &ph = h.Cast<PathMacroFileHandle>();
	return ph.UnderlyingFS().Read(ph.Underlying(), buffer, nr);
}
void PathMacroFileSystem::Write(FileHandle &h, void *buffer, int64_t nr, idx_t loc) {
	auto &ph = h.Cast<PathMacroFileHandle>();
	ph.UnderlyingFS().Write(ph.Underlying(), buffer, nr, loc);
}
int64_t PathMacroFileSystem::Write(FileHandle &h, void *buffer, int64_t nr) {
	auto &ph = h.Cast<PathMacroFileHandle>();
	return ph.UnderlyingFS().Write(ph.Underlying(), buffer, nr);
}
int64_t PathMacroFileSystem::GetFileSize(FileHandle &h) {
	auto &ph = h.Cast<PathMacroFileHandle>();
	return ph.UnderlyingFS().GetFileSize(ph.Underlying());
}
void PathMacroFileSystem::Seek(FileHandle &h, idx_t loc) {
	auto &ph = h.Cast<PathMacroFileHandle>();
	ph.UnderlyingFS().Seek(ph.Underlying(), loc);
}
idx_t PathMacroFileSystem::SeekPosition(FileHandle &h) {
	auto &ph = h.Cast<PathMacroFileHandle>();
	return ph.UnderlyingFS().SeekPosition(ph.Underlying());
}
void PathMacroFileSystem::Reset(FileHandle &h) {
	auto &ph = h.Cast<PathMacroFileHandle>();
	ph.UnderlyingFS().Reset(ph.Underlying());
}
bool PathMacroFileSystem::OnDiskFile(FileHandle &h) {
	auto &ph = h.Cast<PathMacroFileHandle>();
	return ph.UnderlyingFS().OnDiskFile(ph.Underlying());
}
timestamp_t PathMacroFileSystem::GetLastModifiedTime(FileHandle &h) {
	auto &ph = h.Cast<PathMacroFileHandle>();
	return ph.UnderlyingFS().GetLastModifiedTime(ph.Underlying());
}

} // namespace duckdb
