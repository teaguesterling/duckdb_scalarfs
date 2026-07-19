#include "scalarfs_functions.hpp"
#include "string_encodings.hpp"
#include "pathmacro_filesystem.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/function_set.hpp"

namespace duckdb {

// =============================================================================
// Helper functions for encoding/decoding
// =============================================================================

// Encode content to base64 data URI
static string EncodeDataUri(const string_t &input) {
	string base64 = Blob::ToBase64(input.GetString());
	return "data:;base64," + base64;
}

// Encode content to raw varchar URI (just prepend prefix)
static string EncodeVarcharUri(const string_t &input) {
	return "data+varchar:" + input.GetString();
}

// Encode content to blob URI with escape sequences (shared implementation)
static string EncodeBlobUri(const string_t &input) {
	return "data+blob:" + EncodeBlobEscapes(input.GetString());
}

// Auto-select optimal encoding
static string EncodeScalarfsUri(const string_t &input) {
	const string &content = input.GetString();

	// Check if safe for raw varchar (printable + whitespace only)
	bool safe_for_varchar = true;
	int escape_count = 0;

	for (unsigned char c : content) {
		if (c == '\\') {
			escape_count++;
		} else if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') {
			// Control character that needs escaping
			safe_for_varchar = false;
			escape_count++;
		} else if (c == 0x7F) {
			safe_for_varchar = false;
			escape_count++;
		}
	}

	if (safe_for_varchar) {
		return EncodeVarcharUri(input);
	}

	// If less than 10% needs escaping, use blob encoding
	// Otherwise use base64
	if (escape_count * 10 < (int)content.size()) {
		return EncodeBlobUri(input);
	}

	return EncodeDataUri(input);
}

// Decode base64 data URI
static string DecodeDataUri(const string_t &input) {
	const string &uri = input.GetString();

	if (!StringUtil::StartsWith(uri, "data:")) {
		throw InvalidInputException("Invalid data: URI - must start with 'data:'");
	}

	auto comma_pos = uri.find(',');
	if (comma_pos == string::npos) {
		throw InvalidInputException("Invalid data: URI - missing comma separator");
	}

	string metadata = uri.substr(5, comma_pos - 5);
	string data = uri.substr(comma_pos + 1);

	bool is_base64 = metadata.find(";base64") != string::npos;

	if (is_base64) {
		return Blob::FromBase64(data);
	}

	// URL decode (shared implementation)
	return DecodeURLEncoded(data);
}

// Decode raw varchar URI
static string DecodeVarcharUri(const string_t &input) {
	const string &uri = input.GetString();

	if (!StringUtil::StartsWith(uri, "data+varchar:")) {
		throw InvalidInputException("Invalid data+varchar: URI - must start with 'data+varchar:'");
	}

	return uri.substr(13); // len("data+varchar:")
}

// Decode blob URI with escape sequences (shared implementation)
static string DecodeBlobUri(const string_t &input) {
	const string &uri = input.GetString();

	if (!StringUtil::StartsWith(uri, "data+blob:")) {
		throw InvalidInputException("Invalid data+blob: URI - must start with 'data+blob:'");
	}

	return DecodeBlobEscapes(uri.substr(10)); // len("data+blob:")
}

// Auto-detect and decode any scalarfs URI
static string DecodeScalarfsUri(const string_t &input) {
	const string &uri = input.GetString();

	if (StringUtil::StartsWith(uri, "data+varchar:")) {
		return DecodeVarcharUri(input);
	} else if (StringUtil::StartsWith(uri, "data+blob:")) {
		return DecodeBlobUri(input);
	} else if (StringUtil::StartsWith(uri, "data:")) {
		return DecodeDataUri(input);
	}

	throw InvalidInputException("Invalid scalarfs URI - must start with 'data:', 'data+varchar:', or 'data+blob:'");
}

// =============================================================================
// pathmacro: URL builder / parser
// =============================================================================
//
// to_pathmacro_url(macro[, params])  ->  'pathmacro:<macro>?k=v&...'
//   params is a STRUCT ({region:'west', year:2024}) or MAP(VARCHAR,VARCHAR).
//   Keys/values are URL-encoded, so a value like '1&2' or 'p=q' survives as a
//   single param rather than corrupting the query string. The macro name is
//   validated as a plain identifier (same rule the resolver enforces).
//
// from_pathmacro_url(url) -> STRUCT(macro VARCHAR, params MAP(VARCHAR,VARCHAR))
//   The inverse: parses a pathmacro: URL back into its macro + decoded params.

// A scalar Value's raw string content (no surrounding quotes for VARCHAR).
static string ValueToRawString(const Value &v) {
	if (v.type().id() == LogicalTypeId::VARCHAR) {
		return StringValue::Get(v);
	}
	return v.ToString();
}

static void AppendParamsToUrl(string &url, const Value &params) {
	if (params.IsNull()) {
		return;
	}
	const auto &type = params.type();
	vector<std::pair<string, Value>> kvs;
	if (type.id() == LogicalTypeId::STRUCT) {
		auto &child_types = StructType::GetChildTypes(type);
		auto &children = StructValue::GetChildren(params);
		for (idx_t i = 0; i < children.size(); i++) {
			kvs.emplace_back(child_types[i].first, children[i]);
		}
	} else if (type.id() == LogicalTypeId::MAP) {
		// A MAP value is physically a LIST of STRUCT(key, value).
		for (auto &entry : ListValue::GetChildren(params)) {
			auto &kv = StructValue::GetChildren(entry);
			kvs.emplace_back(ValueToRawString(kv[0]), kv[1]);
		}
	} else {
		throw InvalidInputException(
		    "to_pathmacro_url: params must be a STRUCT (e.g. {region:'west'}) or MAP(VARCHAR, VARCHAR), got %s",
		    type.ToString());
	}

	bool first = true;
	for (auto &kv : kvs) {
		if (kv.second.IsNull()) {
			continue; // omit params whose value is NULL
		}
		url += first ? "?" : "&";
		first = false;
		url += EncodeURLComponent(kv.first);
		url += "=";
		url += EncodeURLComponent(ValueToRawString(kv.second));
	}
}

static string BuildPathmacroUrl(const string &macro, const Value *params) {
	if (!PathMacroFileSystem::IsSafeIdentifier(macro)) {
		throw InvalidInputException("to_pathmacro_url: '%s' is not a valid macro name (must be a plain SQL identifier)",
		                            macro);
	}
	string url = "pathmacro:" + macro;
	if (params) {
		AppendParamsToUrl(url, *params);
	}
	return url;
}

static void ToPathmacroUrlNoParams(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();
	for (idx_t row = 0; row < count; row++) {
		Value mv = args.data[0].GetValue(row);
		if (mv.IsNull()) {
			result.SetValue(row, Value(LogicalType::VARCHAR));
			continue;
		}
		result.SetValue(row, Value(BuildPathmacroUrl(StringValue::Get(mv), nullptr)));
	}
	result.Verify(count);
}

static void ToPathmacroUrlWithParams(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();
	for (idx_t row = 0; row < count; row++) {
		Value mv = args.data[0].GetValue(row);
		if (mv.IsNull()) {
			result.SetValue(row, Value(LogicalType::VARCHAR));
			continue;
		}
		Value pv = args.data[1].GetValue(row);
		result.SetValue(row, Value(BuildPathmacroUrl(StringValue::Get(mv), &pv)));
	}
	result.Verify(count);
}

static Value ParsePathmacroUrl(const string &url) {
	if (!StringUtil::StartsWith(url, "pathmacro:")) {
		throw InvalidInputException("from_pathmacro_url: '%s' is not a pathmacro: URL (must start with 'pathmacro:')",
		                            url);
	}
	auto parsed = PathMacroFileSystem::Parse(url);
	vector<Value> keys, vals;
	for (auto &kv : parsed.params) {
		keys.emplace_back(Value(kv.first));
		vals.emplace_back(Value(kv.second));
	}
	child_list_t<Value> fields;
	fields.emplace_back("macro", Value(parsed.macro_name));
	fields.emplace_back("params", Value::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR, keys, vals));
	return Value::STRUCT(fields);
}

static void FromPathmacroUrlFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();
	for (idx_t row = 0; row < count; row++) {
		Value uv = args.data[0].GetValue(row);
		if (uv.IsNull()) {
			result.SetValue(row, Value(result.GetType()));
			continue;
		}
		result.SetValue(row, ParsePathmacroUrl(StringValue::Get(uv)));
	}
	result.Verify(count);
}

// =============================================================================
// Scalar function implementations
// =============================================================================

static void ToDataUriFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t input) {
		return StringVector::AddString(result, EncodeDataUri(input));
	});
}

static void ToVarcharUriFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t input) {
		return StringVector::AddString(result, EncodeVarcharUri(input));
	});
}

static void ToBlobUriFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t input) {
		return StringVector::AddString(result, EncodeBlobUri(input));
	});
}

static void ToScalarfsUriFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t input) {
		return StringVector::AddString(result, EncodeScalarfsUri(input));
	});
}

static void FromDataUriFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t input) {
		return StringVector::AddString(result, DecodeDataUri(input));
	});
}

static void FromVarcharUriFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t input) {
		return StringVector::AddString(result, DecodeVarcharUri(input));
	});
}

static void FromBlobUriFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t input) {
		return StringVector::AddString(result, DecodeBlobUri(input));
	});
}

static void FromScalarfsUriFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t input) {
		return StringVector::AddString(result, DecodeScalarfsUri(input));
	});
}

// =============================================================================
// Function definitions
// =============================================================================

ScalarFunction ScalarfsFunctions::GetToDataUriFunction() {
	return ScalarFunction("to_data_uri", {LogicalType::VARCHAR}, LogicalType::VARCHAR, ToDataUriFunction);
}

ScalarFunction ScalarfsFunctions::GetToVarcharUriFunction() {
	return ScalarFunction("to_varchar_uri", {LogicalType::VARCHAR}, LogicalType::VARCHAR, ToVarcharUriFunction);
}

ScalarFunction ScalarfsFunctions::GetToBlobUriFunction() {
	return ScalarFunction("to_blob_uri", {LogicalType::VARCHAR}, LogicalType::VARCHAR, ToBlobUriFunction);
}

ScalarFunction ScalarfsFunctions::GetToScalarfsUriFunction() {
	return ScalarFunction("to_scalarfs_uri", {LogicalType::VARCHAR}, LogicalType::VARCHAR, ToScalarfsUriFunction);
}

ScalarFunction ScalarfsFunctions::GetFromDataUriFunction() {
	return ScalarFunction("from_data_uri", {LogicalType::VARCHAR}, LogicalType::VARCHAR, FromDataUriFunction);
}

ScalarFunction ScalarfsFunctions::GetFromVarcharUriFunction() {
	return ScalarFunction("from_varchar_uri", {LogicalType::VARCHAR}, LogicalType::VARCHAR, FromVarcharUriFunction);
}

ScalarFunction ScalarfsFunctions::GetFromBlobUriFunction() {
	return ScalarFunction("from_blob_uri", {LogicalType::VARCHAR}, LogicalType::VARCHAR, FromBlobUriFunction);
}

ScalarFunction ScalarfsFunctions::GetFromScalarfsUriFunction() {
	return ScalarFunction("from_scalarfs_uri", {LogicalType::VARCHAR}, LogicalType::VARCHAR, FromScalarfsUriFunction);
}

ScalarFunctionSet ScalarfsFunctions::GetToPathmacroUrlFunctions() {
	ScalarFunctionSet set("to_pathmacro_url");
	// to_pathmacro_url(macro)
	ScalarFunction no_params("to_pathmacro_url", {LogicalType::VARCHAR}, LogicalType::VARCHAR, ToPathmacroUrlNoParams);
	no_params.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	set.AddFunction(no_params);
	// to_pathmacro_url(macro, params)  — params is a STRUCT or MAP (ANY dispatched at runtime)
	ScalarFunction with_params("to_pathmacro_url", {LogicalType::VARCHAR, LogicalType::ANY}, LogicalType::VARCHAR,
	                           ToPathmacroUrlWithParams);
	with_params.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	set.AddFunction(with_params);
	return set;
}

ScalarFunction ScalarfsFunctions::GetFromPathmacroUrlFunction() {
	auto ret = LogicalType::STRUCT(
	    {{"macro", LogicalType::VARCHAR}, {"params", LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR)}});
	ScalarFunction fn("from_pathmacro_url", {LogicalType::VARCHAR}, ret, FromPathmacroUrlFunction);
	fn.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	return fn;
}

void ScalarfsFunctions::Register(ExtensionLoader &loader) {
	loader.RegisterFunction(GetToDataUriFunction());
	loader.RegisterFunction(GetToVarcharUriFunction());
	loader.RegisterFunction(GetToBlobUriFunction());
	loader.RegisterFunction(GetToScalarfsUriFunction());
	loader.RegisterFunction(GetFromDataUriFunction());
	loader.RegisterFunction(GetFromVarcharUriFunction());
	loader.RegisterFunction(GetFromBlobUriFunction());
	loader.RegisterFunction(GetFromScalarfsUriFunction());
	loader.RegisterFunction(GetToPathmacroUrlFunctions());
	loader.RegisterFunction(GetFromPathmacroUrlFunction());
}

} // namespace duckdb
