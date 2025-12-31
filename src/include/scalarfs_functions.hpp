#pragma once

#include "duckdb.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

class ScalarfsFunctions {
public:
	// Encoding functions
	static ScalarFunction GetToDataUriFunction();
	static ScalarFunction GetToVarcharUriFunction();
	static ScalarFunction GetToBlobUriFunction();
	static ScalarFunction GetToScalarfsUriFunction();

	// Decoding functions
	static ScalarFunction GetFromDataUriFunction();
	static ScalarFunction GetFromVarcharUriFunction();
	static ScalarFunction GetFromBlobUriFunction();
	static ScalarFunction GetFromScalarfsUriFunction();

	// Register all functions via the extension loader
	static void Register(ExtensionLoader &loader);
};

} // namespace duckdb
