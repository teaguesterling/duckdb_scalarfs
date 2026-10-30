#pragma once

#include "duckdb.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/file_system.hpp"

namespace duckdb {

// Base for the scalarfs filesystems that take part in DuckDB's canonicalize
// phase (issue #26). A subclass implements ResolveCanonicalPath; this class
// owns the CanonicalizePath override and the recursion through the resolved
// path's own filesystem.
class ScalarfsCanonicalizingFileSystem : public FileSystem {
public:
	string CanonicalizePath(const string &path, optional_ptr<FileOpener> opener = nullptr) override {
		auto resolved = ResolveCanonicalPath(path, opener);
		auto context = FileOpener::TryGetClientContext(opener);
		if (resolved == path || !context) {
			return resolved;
		}
		// The resolved path is canonicalized by ITS owner too. A stored value is
		// often relative ('t.db'), and DuckDB compares canonical paths to refuse a
		// second ATTACH of a file that is already attached. Returning 't.db' while
		// a plain ATTACH 't.db' yields '/abs/t.db' let the same file be attached
		// twice (measured). The depth cap stops a chain of scalarfs paths that
		// loops back on itself; at the cap the path is returned as resolved so far.
		static thread_local idx_t depth = 0;
		if (depth >= MAX_CANONICALIZE_DEPTH) {
			return resolved;
		}
		struct DepthGuard {
			idx_t &d;
			explicit DepthGuard(idx_t &d_p) : d(d_p) {
				d++;
			}
			~DepthGuard() {
				d--;
			}
		} guard(depth);
		try {
			return FileSystem::GetFileSystem(*context).CanonicalizePath(resolved);
		} catch (...) {
			return resolved;
		}
	}

protected:
	//! Resolve a path this filesystem owns to the path it names -- losslessly,
	//! never throwing -- or return it unchanged.
	virtual string ResolveCanonicalPath(const string &path, optional_ptr<FileOpener> opener) = 0;

private:
	static constexpr idx_t MAX_CANONICALIZE_DEPTH = 8;
};

} // namespace duckdb
