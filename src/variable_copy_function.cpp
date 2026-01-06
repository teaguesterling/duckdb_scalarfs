#include "variable_copy_function.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

// =============================================================================
// Helper Functions
// =============================================================================

string VariableCopyFunction::ExtractVariableName(const string &path) {
	// Extract variable name from "variable:foo" path
	if (!StringUtil::StartsWith(path, "variable:")) {
		throw BinderException("FORMAT variable requires 'variable:' path prefix, got '%s'", path);
	}
	return path.substr(9); // len("variable:")
}

// =============================================================================
// Bind
// =============================================================================

unique_ptr<FunctionData> VariableCopyFunction::Bind(ClientContext &context, CopyFunctionBindInput &input,
                                                    const vector<string> &names, const vector<LogicalType> &sql_types) {
	// Extract variable name from path
	string var_name = ExtractVariableName(input.info.file_path);

	if (var_name.empty()) {
		throw BinderException("Variable name cannot be empty");
	}

	// Parse LIST option
	VariableCopyListMode list_mode = VariableCopyListMode::AUTO;

	for (auto &option : input.info.options) {
		string loption = StringUtil::Lower(option.first);
		auto &values = option.second;

		if (loption == "list") {
			if (values.size() != 1) {
				throw BinderException("LIST option requires a single value");
			}
			string mode_str = StringUtil::Lower(values[0].ToString());

			if (mode_str == "auto") {
				list_mode = VariableCopyListMode::AUTO;
			} else if (mode_str == "rows") {
				list_mode = VariableCopyListMode::ROWS;
			} else if (mode_str == "none") {
				list_mode = VariableCopyListMode::NONE;
			} else if (mode_str == "scalar") {
				list_mode = VariableCopyListMode::SCALAR;
			} else {
				throw BinderException("Invalid LIST mode '%s'. Valid options: auto, rows, none, scalar", mode_str);
			}
		}
	}

	// Validate LIST scalar mode
	if (list_mode == VariableCopyListMode::SCALAR && sql_types.size() > 1) {
		throw BinderException("LIST scalar mode requires single-column result, got %d columns", sql_types.size());
	}

	return make_uniq<VariableCopyBindData>(var_name, list_mode, names, sql_types);
}

// =============================================================================
// Initialize Global State
// =============================================================================

unique_ptr<GlobalFunctionData> VariableCopyFunction::InitializeGlobal(ClientContext &context, FunctionData &bind_data,
                                                                      const string &file_path) {
	auto &bdata = bind_data.Cast<VariableCopyBindData>();

	auto state = make_uniq<VariableCopyGlobalState>();
	state->results = make_uniq<ColumnDataCollection>(context, bdata.column_types);

	return std::move(state);
}

// =============================================================================
// Initialize Local State
// =============================================================================

unique_ptr<LocalFunctionData> VariableCopyFunction::InitializeLocal(ExecutionContext &context,
                                                                    FunctionData &bind_data) {
	return make_uniq<VariableCopyLocalState>();
}

// =============================================================================
// Sink - Process incoming data chunks
// =============================================================================

void VariableCopyFunction::Sink(ExecutionContext &context, FunctionData &bind_data, GlobalFunctionData &gstate,
                                LocalFunctionData &lstate, DataChunk &input) {
	auto &state = gstate.Cast<VariableCopyGlobalState>();

	// Thread-safe append to results
	lock_guard<mutex> lock(state.lock);
	state.results->Append(input);
}

// =============================================================================
// Combine - Merge local state into global (no-op for us)
// =============================================================================

void VariableCopyFunction::Combine(ExecutionContext &context, FunctionData &bind_data, GlobalFunctionData &gstate,
                                   LocalFunctionData &lstate) {
	// No-op - we append directly to global state in Sink
}

// =============================================================================
// Convert Results to Value
// =============================================================================

Value VariableCopyFunction::ConvertToValue(ColumnDataCollection &results, const VariableCopyBindData &bind_data) {
	idx_t row_count = results.Count();
	idx_t col_count = bind_data.column_types.size();

	// Handle empty results
	if (row_count == 0) {
		// Return empty list with appropriate type
		if (col_count == 1) {
			return Value::LIST(bind_data.column_types[0], vector<Value>());
		} else {
			// Empty list of structs
			child_list_t<LogicalType> struct_children;
			for (idx_t i = 0; i < col_count; i++) {
				struct_children.push_back(make_pair(bind_data.column_names[i], bind_data.column_types[i]));
			}
			auto struct_type = LogicalType::STRUCT(std::move(struct_children));
			return Value::LIST(struct_type, vector<Value>());
		}
	}

	// Validate based on LIST mode
	switch (bind_data.list_mode) {
	case VariableCopyListMode::NONE:
		if (row_count > 1) {
			throw InvalidInputException("LIST none mode requires single row result, got %d rows", row_count);
		}
		break;
	case VariableCopyListMode::SCALAR:
		// Already validated in Bind
		break;
	default:
		break;
	}

	// Collect all values from results
	vector<vector<Value>> all_values(col_count);

	DataChunk chunk;
	chunk.Initialize(Allocator::DefaultAllocator(), bind_data.column_types);

	ColumnDataScanState scan_state;
	results.InitializeScan(scan_state);

	while (results.Scan(scan_state, chunk)) {
		for (idx_t col_idx = 0; col_idx < col_count; col_idx++) {
			for (idx_t row_idx = 0; row_idx < chunk.size(); row_idx++) {
				all_values[col_idx].push_back(chunk.data[col_idx].GetValue(row_idx));
			}
		}
		chunk.Reset();
	}

	// Convert based on LIST mode and dimensions
	bool single_row = (row_count == 1);
	bool single_col = (col_count == 1);

	switch (bind_data.list_mode) {
	case VariableCopyListMode::NONE:
		// Single value only
		if (single_col) {
			return all_values[0][0];
		} else {
			// Single row, multiple cols -> struct
			child_list_t<Value> struct_values;
			for (idx_t i = 0; i < col_count; i++) {
				struct_values.push_back(make_pair(bind_data.column_names[i], all_values[i][0]));
			}
			return Value::STRUCT(std::move(struct_values));
		}

	case VariableCopyListMode::SCALAR:
		// Single column, list of values (or scalar if 1 row)
		if (single_row) {
			return all_values[0][0];
		} else {
			return Value::LIST(bind_data.column_types[0], std::move(all_values[0]));
		}

	case VariableCopyListMode::ROWS:
		// Always list of structs
		{
			child_list_t<LogicalType> struct_children;
			for (idx_t i = 0; i < col_count; i++) {
				struct_children.push_back(make_pair(bind_data.column_names[i], bind_data.column_types[i]));
			}
			auto struct_type = LogicalType::STRUCT(struct_children);

			vector<Value> row_structs;
			for (idx_t row_idx = 0; row_idx < row_count; row_idx++) {
				child_list_t<Value> struct_values;
				for (idx_t col_idx = 0; col_idx < col_count; col_idx++) {
					struct_values.push_back(make_pair(bind_data.column_names[col_idx], all_values[col_idx][row_idx]));
				}
				row_structs.push_back(Value::STRUCT(std::move(struct_values)));
			}
			return Value::LIST(struct_type, std::move(row_structs));
		}

	case VariableCopyListMode::AUTO:
	default:
		// Smart detection
		if (single_row && single_col) {
			// 1 row, 1 col -> scalar
			return all_values[0][0];
		} else if (single_row && !single_col) {
			// 1 row, N cols -> struct
			child_list_t<Value> struct_values;
			for (idx_t i = 0; i < col_count; i++) {
				struct_values.push_back(make_pair(bind_data.column_names[i], all_values[i][0]));
			}
			return Value::STRUCT(std::move(struct_values));
		} else if (!single_row && single_col) {
			// N rows, 1 col -> list of values
			return Value::LIST(bind_data.column_types[0], std::move(all_values[0]));
		} else {
			// N rows, N cols -> list of structs
			child_list_t<LogicalType> struct_children;
			for (idx_t i = 0; i < col_count; i++) {
				struct_children.push_back(make_pair(bind_data.column_names[i], bind_data.column_types[i]));
			}
			auto struct_type = LogicalType::STRUCT(struct_children);

			vector<Value> row_structs;
			for (idx_t row_idx = 0; row_idx < row_count; row_idx++) {
				child_list_t<Value> struct_values;
				for (idx_t col_idx = 0; col_idx < col_count; col_idx++) {
					struct_values.push_back(make_pair(bind_data.column_names[col_idx], all_values[col_idx][row_idx]));
				}
				row_structs.push_back(Value::STRUCT(std::move(struct_values)));
			}
			return Value::LIST(struct_type, std::move(row_structs));
		}
	}
}

// =============================================================================
// Finalize - Store result in variable
// =============================================================================

void VariableCopyFunction::Finalize(ClientContext &context, FunctionData &bind_data, GlobalFunctionData &gstate) {
	auto &bdata = bind_data.Cast<VariableCopyBindData>();
	auto &state = gstate.Cast<VariableCopyGlobalState>();

	// Convert collected results to a Value
	Value result = ConvertToValue(*state.results, bdata);

	// Store in variable
	auto &config = ClientConfig::GetConfig(context);
	config.SetUserVariable(bdata.variable_name, result);
}

// =============================================================================
// Register the Copy Function
// =============================================================================

void VariableCopyFunction::Register(ExtensionLoader &loader) {
	CopyFunction info("variable");

	info.copy_to_bind = Bind;
	info.copy_to_initialize_global = InitializeGlobal;
	info.copy_to_initialize_local = InitializeLocal;
	info.copy_to_sink = Sink;
	info.copy_to_combine = Combine;
	info.copy_to_finalize = Finalize;

	info.extension = "scalarfs";

	loader.RegisterFunction(info);
}

} // namespace duckdb
