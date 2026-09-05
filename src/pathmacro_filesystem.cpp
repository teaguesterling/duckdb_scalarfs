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
// DuckDB's atomic COPY writes to a temp sibling then MoveFile()s it onto the
// target; it forms the temp path by prepending "tmp_" to the last path
// component — here the whole "pathmacro:<macro>?..." URL. We recognize that
// prefix, resolve the same macro, and map to a temp sibling of the real path.
static constexpr const char *TMP_PATHMACRO_PREFIX = "tmp_pathmacro:";

// Prepend "tmp_" to the filename of a resolved real path (matches DuckDB).
static string ComputeTempPath(const string &target_path) {
	auto sep = target_path.find_last_of("/\\");
	if (sep == string::npos) {
		return "tmp_" + target_path;
	}
	return target_path.substr(0, sep + 1) + "tmp_" + target_path.substr(sep + 1);
}

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
	return StringUtil::StartsWith(fpath, PATHMACRO_PREFIX) || StringUtil::StartsWith(fpath, TMP_PATHMACRO_PREFIX);
}

PathMacroFileSystem::ParsedPathMacro PathMacroFileSystem::Parse(const string &path) {
	ParsedPathMacro r;
	string p = path;
	if (StringUtil::StartsWith(p, TMP_PATHMACRO_PREFIX)) {
		r.is_temp = true;
		p = p.substr(4); // strip "tmp_" -> "pathmacro:<macro>?..."
	}
	string rest = p.substr(strlen(PATHMACRO_PREFIX));
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
	vector<string> paths;
	auto tid = val.type().id();
	if (tid == LogicalTypeId::VARCHAR) {
		// Scalar VARCHAR — a single path. Natural for a write target
		// (`COPY ... TO 'pathmacro:m'` where m returns one path).
		paths.push_back(val.ToString());
	} else if (tid == LogicalTypeId::LIST && ListType::GetChildType(val.type()).id() == LogicalTypeId::VARCHAR) {
		for (auto &child : ListValue::GetChildren(val)) {
			if (!child.IsNull()) {
				paths.push_back(child.ToString());
			}
		}
	} else {
		throw IOException("pathmacro: macro '%s' must return VARCHAR or VARCHAR[] (a path or list of paths), got %s",
		                  parsed.macro_name, val.type().ToString());
	}
	if (parsed.is_temp) {
		// COPY's atomic write: this is the temp handle — write to a temp sibling
		// of each resolved real path; MoveFile() renames it onto the final path.
		for (auto &pth : paths) {
			pth = ComputeTempPath(pth);
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

// Mirrors FileExists: resolve the macro to a single path and ask the parent
// filesystem. DuckDB v2.0's COPY TO probes the target with DirectoryExists()
// before writing, and FileSystem's base implementation throws "not implemented"
// -- so without this every pathmacro: write fails on v2.0.
bool PathMacroFileSystem::DirectoryExists(const string &directory, optional_ptr<FileOpener> opener) {
	if (!CanHandleFile(directory)) {
		return false;
	}
	try {
		auto context = FileOpener::TryGetClientContext(opener);
		if (!context) {
			return false;
		}
		auto paths = ResolvePaths(*context, Parse(directory));
		auto &parent_fs = FileSystem::GetFileSystem(*context);
		return paths.size() == 1 && parent_fs.DirectoryExists(paths[0], nullptr);
	} catch (...) {
		return false;
	}
}

// Remove/TryRemove — resolve to a single path and delegate (used by COPY TO for
// overwrite). Writes are permitted: the macro is the caller's own allow-listed
// SQL, same trust posture as pathvariable: writes.
void PathMacroFileSystem::RemoveFile(const string &filename, optional_ptr<FileOpener> opener) {
	auto context = FileOpener::TryGetClientContext(opener);
	if (!context) {
		throw IOException("pathmacro: cannot resolve '%s' without a client context", filename);
	}
	auto paths = ResolvePaths(*context, Parse(filename));
	if (paths.size() != 1) {
		throw IOException("pathmacro: '%s' resolved to %llu paths; remove requires a single path", filename,
		                  (unsigned long long)paths.size());
	}
	auto &parent_fs = FileSystem::GetFileSystem(*context);
	parent_fs.RemoveFile(paths[0], nullptr);
}

bool PathMacroFileSystem::TryRemoveFile(const string &filename, optional_ptr<FileOpener> opener) {
	try {
		RemoveFile(filename, opener);
		return true;
	} catch (...) {
		return false;
	}
}

// MoveFile — DuckDB's atomic COPY calls this to rename the temp handle
// (tmp_pathmacro:...) onto the final target (pathmacro:...). Both resolve via
// the same macro; source maps to the temp sibling, target to the final path.
void PathMacroFileSystem::MoveFile(const string &source, const string &target, optional_ptr<FileOpener> opener) {
	auto context = FileOpener::TryGetClientContext(opener);
	if (!context) {
		throw IOException("pathmacro: cannot resolve move '%s' -> '%s' without a client context", source, target);
	}
	auto src = ResolvePaths(*context, Parse(source));
	auto tgt = ResolvePaths(*context, Parse(target));
	if (src.size() != 1 || tgt.size() != 1) {
		throw IOException("pathmacro: move requires single-path source and target ('%s' -> '%s')", source, target);
	}
	auto &parent_fs = FileSystem::GetFileSystem(*context);
	parent_fs.MoveFile(src[0], tgt[0], nullptr);
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
