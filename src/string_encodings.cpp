#include "string_encodings.hpp"
#include "duckdb/common/exception.hpp"
#include <cstdio>
#include <cstdlib>

namespace duckdb {

static bool IsHexDigit(char c) {
	return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

// ---------------------------------------------------------------------------
// URL / percent coding
// ---------------------------------------------------------------------------

string DecodeURLEncoded(const string &input) {
	string result;
	result.reserve(input.size());
	for (size_t i = 0; i < input.size(); i++) {
		if (input[i] != '%') {
			result += input[i];
			continue;
		}
		if (i + 2 >= input.size()) {
			throw InvalidInputException("Invalid URL encoding - incomplete '%%' escape at position %llu",
			                            (unsigned long long)i);
		}
		char c1 = input[i + 1];
		char c2 = input[i + 2];
		if (!IsHexDigit(c1) || !IsHexDigit(c2)) {
			throw InvalidInputException("Invalid URL encoding - '%%%c%c' is not valid hex at position %llu", c1, c2,
			                            (unsigned long long)i);
		}
		char hex[3] = {c1, c2, '\0'};
		result += static_cast<char>(strtol(hex, nullptr, 16));
		i += 2;
	}
	return result;
}

string EncodeURLComponent(const string &input) {
	static const char *HEX = "0123456789ABCDEF";
	string out;
	out.reserve(input.size());
	for (unsigned char c : input) {
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
		    c == '.' || c == '~') {
			out += static_cast<char>(c);
		} else {
			out += '%';
			out += HEX[c >> 4];
			out += HEX[c & 0x0F];
		}
	}
	return out;
}

// ---------------------------------------------------------------------------
// Backslash escape coding
// ---------------------------------------------------------------------------

string EncodeBlobEscapes(const string &input) {
	string result;
	result.reserve(input.size());
	for (unsigned char c : input) {
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
				char hex[5];
				snprintf(hex, sizeof(hex), "\\x%02X", c);
				result += hex;
			} else {
				result += static_cast<char>(c);
			}
			break;
		}
	}
	return result;
}

string DecodeBlobEscapes(const string &input) {
	string result;
	result.reserve(input.size());
	for (size_t i = 0; i < input.size(); i++) {
		if (input[i] != '\\') {
			result += input[i];
			continue;
		}
		if (i + 1 >= input.size()) {
			throw InvalidInputException("Invalid escape sequence at end of input");
		}
		char next = input[i + 1];
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
		case 'x': {
			if (i + 3 >= input.size()) {
				throw InvalidInputException("Invalid escape sequence: incomplete \\x at position %llu (expected 2 hex "
				                            "digits)",
				                            (unsigned long long)i);
			}
			char hex[3] = {input[i + 2], input[i + 3], '\0'};
			char *end;
			long val = strtol(hex, &end, 16);
			if (end != hex + 2) {
				throw InvalidInputException("Invalid escape sequence: '\\x%s' is not valid hex at position %llu",
				                            string(hex), (unsigned long long)i);
			}
			result += static_cast<char>(val);
			i += 3;
			break;
		}
		default:
			throw InvalidInputException("Invalid escape sequence '\\%c' at position %llu", next, (unsigned long long)i);
		}
	}
	return result;
}

} // namespace duckdb
