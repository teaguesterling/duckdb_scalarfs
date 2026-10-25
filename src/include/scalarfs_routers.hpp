#pragma once

#include "data_uri_filesystem.hpp"
#include "decompress_filesystem.hpp"
#include "pathmacro_filesystem.hpp"
#include "pathvariable_filesystem.hpp"
#include "variable_filesystem.hpp"

namespace duckdb {

// The filesystems scalarfs registers (LoadInternal in scalarfs_extension.cpp).
// Code that asks "is this a scalarfs path?" asks their CanHandleFile rather than
// keep a prefix list of its own, so it cannot disagree with the routing that
// opens a file. None of these holds state that CanHandleFile reads, so a
// short-lived local set is cheap.
struct ScalarfsRouters {
	DataURIFileSystem data;
	VariableFileSystem variable;
	PathVariableFileSystem pathvariable;
	PathMacroFileSystem pathmacro;
	DecompressFileSystem decompress;

	bool Owns(const string &path) {
		return data.CanHandleFile(path) || variable.CanHandleFile(path) || pathvariable.CanHandleFile(path) ||
		       pathmacro.CanHandleFile(path) || decompress.CanHandleFile(path);
	}
};

} // namespace duckdb
