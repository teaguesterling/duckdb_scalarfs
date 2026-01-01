#include "scalarfs_functions.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/blob.hpp"

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

// Encode content to blob URI with escape sequences
static string EncodeBlobUri(const string_t &input) {
	string result = "data+blob:";
	const string &content = input.GetString();

	for (unsigned char c : content) {
		switch (c) {
		case '\\':
			result += "\\\\";
			break;
		case '\n':
			result += "\\n";
			break;
		case '\r':
			result += "\\r";
			break;
		case '\t':
			result += "\\t";
			break;
		case '\0':
			result += "\\0";
			break;
		default:
			if (c < 0x20 || c == 0x7F) {
				// Other control characters: use \xNN
				char hex[5];
				snprintf(hex, sizeof(hex), "\\x%02X", c);
				result += hex;
			} else {
				result += c;
			}
			break;
		}
	}

	return result;
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

	// URL decode
	string result;
	for (size_t i = 0; i < data.size(); i++) {
		if (data[i] == '%' && i + 2 < data.size()) {
			char hex[3] = {data[i + 1], data[i + 2], '\0'};
			char *end;
			long val = strtol(hex, &end, 16);
			if (end == hex + 2) {
				result += static_cast<char>(val);
				i += 2;
				continue;
			}
		}
		result += data[i];
	}
	return result;
}

// Decode raw varchar URI
static string DecodeVarcharUri(const string_t &input) {
	const string &uri = input.GetString();

	if (!StringUtil::StartsWith(uri, "data+varchar:")) {
		throw InvalidInputException("Invalid data+varchar: URI - must start with 'data+varchar:'");
	}

	return uri.substr(13); // len("data+varchar:")
}

// Decode blob URI with escape sequences
static string DecodeBlobUri(const string_t &input) {
	const string &uri = input.GetString();

	if (!StringUtil::StartsWith(uri, "data+blob:")) {
		throw InvalidInputException("Invalid data+blob: URI - must start with 'data+blob:'");
	}

	string content = uri.substr(10); // len("data+blob:")
	string result;

	for (size_t i = 0; i < content.size(); i++) {
		if (content[i] == '\\' && i + 1 < content.size()) {
			char next = content[i + 1];
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
			case 'x':
				if (i + 3 < content.size()) {
					char hex[3] = {content[i + 2], content[i + 3], '\0'};
					char *end;
					long val = strtol(hex, &end, 16);
					if (end == hex + 2) {
						result += static_cast<char>(val);
						i += 3;
					} else {
						throw InvalidInputException("Invalid escape sequence '\\x%c%c' at position %d", content[i + 2],
						                            content[i + 3], i);
					}
				} else {
					throw InvalidInputException("Invalid escape sequence - incomplete \\x at position %d", i);
				}
				break;
			default:
				throw InvalidInputException("Invalid escape sequence '\\%c' at position %d", next, i);
			}
		} else {
			result += content[i];
		}
	}

	return result;
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

void ScalarfsFunctions::Register(ExtensionLoader &loader) {
	loader.RegisterFunction(GetToDataUriFunction());
	loader.RegisterFunction(GetToVarcharUriFunction());
	loader.RegisterFunction(GetToBlobUriFunction());
	loader.RegisterFunction(GetToScalarfsUriFunction());
	loader.RegisterFunction(GetFromDataUriFunction());
	loader.RegisterFunction(GetFromVarcharUriFunction());
	loader.RegisterFunction(GetFromBlobUriFunction());
	loader.RegisterFunction(GetFromScalarfsUriFunction());
}

} // namespace duckdb
