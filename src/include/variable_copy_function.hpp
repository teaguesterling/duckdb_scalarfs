#pragma once

#include "duckdb.hpp"
#include "duckdb/function/copy_function.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

// =============================================================================
// Variable Copy Function
// =============================================================================
//
// A custom COPY function that stores query results directly as DuckDB Values
// in variables, without text serialization.
//
// Usage:
//   COPY (SELECT ...) TO 'variable:foo' (FORMAT variable);
//   COPY (SELECT ...) TO 'variable:foo' (FORMAT variable, LIST auto);
//
// LIST modes:
//   - auto (default): Smart detection based on row/column count
//       1 row, 1 col  → scalar value
//       N rows, 1 col → list of values
//       1 row, N cols → struct
//       N rows, N cols → list of structs
//   - rows: Always produce list of structs, even for single row
//   - none: Single value only, error if >1 row
//   - scalar: Single column only, error if >1 column
//       1 row → scalar, N rows → list
//

enum class VariableCopyListMode : uint8_t {
	AUTO = 0,    // Smart detection
	ROWS = 1,    // Always list of structs
	NONE = 2,    // Single value only (error if >1 row)
	SCALAR = 3   // Single column only (error if >1 column)
};

struct VariableCopyBindData : public FunctionData {
	string variable_name;
	VariableCopyListMode list_mode;
	vector<string> column_names;
	vector<LogicalType> column_types;

	VariableCopyBindData(string var_name, VariableCopyListMode mode, vector<string> names, vector<LogicalType> types)
	    : variable_name(std::move(var_name)), list_mode(mode), column_names(std::move(names)),
	      column_types(std::move(types)) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<VariableCopyBindData>(variable_name, list_mode, column_names, column_types);
	}

	bool Equals(const FunctionData &other) const override {
		auto &o = other.Cast<VariableCopyBindData>();
		return variable_name == o.variable_name && list_mode == o.list_mode;
	}
};

struct VariableCopyGlobalState : public GlobalFunctionData {
	unique_ptr<ColumnDataCollection> results;
	mutex lock;
};

struct VariableCopyLocalState : public LocalFunctionData {
	// Thread-local state (currently unused, but required by interface)
};

class VariableCopyFunction {
public:
	static void Register(ExtensionLoader &loader);

	static unique_ptr<FunctionData> Bind(ClientContext &context, CopyFunctionBindInput &input,
	                                     const vector<string> &names, const vector<LogicalType> &sql_types);

	static unique_ptr<GlobalFunctionData> InitializeGlobal(ClientContext &context, FunctionData &bind_data,
	                                                       const string &file_path);

	static unique_ptr<LocalFunctionData> InitializeLocal(ExecutionContext &context, FunctionData &bind_data);

	static void Sink(ExecutionContext &context, FunctionData &bind_data, GlobalFunctionData &gstate,
	                 LocalFunctionData &lstate, DataChunk &input);

	static void Combine(ExecutionContext &context, FunctionData &bind_data, GlobalFunctionData &gstate,
	                    LocalFunctionData &lstate);

	static void Finalize(ClientContext &context, FunctionData &bind_data, GlobalFunctionData &gstate);

private:
	static string ExtractVariableName(const string &path);
	static Value ConvertToValue(ColumnDataCollection &results, const VariableCopyBindData &bind_data);
};

} // namespace duckdb
