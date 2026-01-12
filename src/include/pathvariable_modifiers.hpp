#pragma once

#include "duckdb.hpp"
#include "duckdb/common/string_util.hpp"
#include <unordered_set>

namespace duckdb {

// =============================================================================
// PathVariable Modifiers
// =============================================================================
//
// Modifiers control how pathvariable: resolves and filters paths.
//
// Syntax: pathvariable:[modifier:]...varname[!value]
//
// Examples:
//   pathvariable:varname                    - No modifiers
//   pathvariable:no-glob:varname            - Disable glob expansion
//   pathvariable:search:varname             - Return first existing match
//   pathvariable:no-missing:varname         - Skip non-existent files
//   pathvariable:no-cache:varname           - Disable caching of glob results
//   pathvariable:append:varname!/path       - Append literal to paths
//   pathvariable:append:varname!$other_var  - Append variable value to paths
//

// Modifier flags (can be combined)
enum class PathVariableModifierFlag : uint8_t {
	NONE = 0,
	NO_GLOB = 1 << 0,           // Disable glob expansion in paths
	SEARCH = 1 << 1,            // Return only first existing match
	IGNORE_MISSING = 1 << 2,    // Skip non-existent files
	APPEND = 1 << 3,            // Append value to each path
	PREPEND = 1 << 4,           // Prepend value to each path
	PASSTHRU_SCALARFS = 1 << 5, // Don't modify scalarfs protocol paths (data:, variable:, etc.)
	PASSTHRU_EXPLICIT_FS = 1 << 6, // Don't modify paths with explicit protocols (://)
	NO_CACHE = 1 << 7           // Disable caching of path resolution
};

// Enable bitwise operations on modifier flags
inline PathVariableModifierFlag operator|(PathVariableModifierFlag a, PathVariableModifierFlag b) {
	return static_cast<PathVariableModifierFlag>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

inline PathVariableModifierFlag operator&(PathVariableModifierFlag a, PathVariableModifierFlag b) {
	return static_cast<PathVariableModifierFlag>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

inline PathVariableModifierFlag &operator|=(PathVariableModifierFlag &a, PathVariableModifierFlag b) {
	a = a | b;
	return a;
}

inline bool HasFlag(PathVariableModifierFlag flags, PathVariableModifierFlag flag) {
	return (static_cast<uint8_t>(flags) & static_cast<uint8_t>(flag)) != 0;
}

// =============================================================================
// ParsedPathVariablePath
// =============================================================================
//
// Result of parsing a pathvariable: path with modifiers
//

struct PathVariableValue {
	string value;
	bool is_variable; // true if value starts with $, meaning it's a variable reference

	PathVariableValue() : value(), is_variable(false) {
	}
	PathVariableValue(const string &val, bool is_var) : value(val), is_variable(is_var) {
	}

	bool IsEmpty() const {
		return value.empty();
	}
};

struct ParsedPathVariablePath {
	// The variable name (without modifiers)
	string variable_name;

	// Combined modifier flags
	PathVariableModifierFlag flags;

	// Value for append modifier (if APPEND flag is set)
	PathVariableValue append_value;

	// Value for prepend modifier (if PREPEND flag is set)
	PathVariableValue prepend_value;

	// Whether this is a tmp_pathvariable: path
	bool is_temp;

	ParsedPathVariablePath()
	    : variable_name(), flags(PathVariableModifierFlag::NONE), append_value(), prepend_value(), is_temp(false) {
	}

	bool HasModifier(PathVariableModifierFlag flag) const {
		return HasFlag(flags, flag);
	}
};

// =============================================================================
// PathVariableParser
// =============================================================================
//
// Parses pathvariable: paths into their components
//

class PathVariableParser {
public:
	// Parse a pathvariable: or tmp_pathvariable: path
	// Returns a ParsedPathVariablePath with all components extracted
	static ParsedPathVariablePath Parse(const string &path);

	// Check if a path is a pathvariable: or tmp_pathvariable: path
	static bool CanHandle(const string &path);

	// Check if a path is a tmp_pathvariable: path
	static bool IsTempPath(const string &path);

private:
	// Parse a modifier string and set flags/values
	// Returns true if the string was recognized as a modifier
	static bool ParseModifier(const string &modifier, ParsedPathVariablePath &result);

	// Parse a value (after !) which may be a literal or $variable reference
	static PathVariableValue ParseValue(const string &value_str);
};

// =============================================================================
// Implementation
// =============================================================================

inline bool PathVariableParser::CanHandle(const string &path) {
	return StringUtil::StartsWith(path, "pathvariable:") || StringUtil::StartsWith(path, "tmp_pathvariable:");
}

inline bool PathVariableParser::IsTempPath(const string &path) {
	return StringUtil::StartsWith(path, "tmp_pathvariable:");
}

inline PathVariableValue PathVariableParser::ParseValue(const string &value_str) {
	if (value_str.empty()) {
		return PathVariableValue();
	}
	if (value_str[0] == '$') {
		// Variable reference - strip the $
		return PathVariableValue(value_str.substr(1), true);
	}
	// Literal value
	return PathVariableValue(value_str, false);
}

inline bool PathVariableParser::ParseModifier(const string &modifier, ParsedPathVariablePath &result) {
	// Check for modifiers with values (contain !)
	auto bang_pos = modifier.find('!');
	string mod_name = (bang_pos != string::npos) ? modifier.substr(0, bang_pos) : modifier;
	string mod_value = (bang_pos != string::npos) ? modifier.substr(bang_pos + 1) : "";

	if (mod_name == "no-glob") {
		result.flags |= PathVariableModifierFlag::NO_GLOB;
		return true;
	}
	if (mod_name == "search") {
		result.flags |= PathVariableModifierFlag::SEARCH;
		return true;
	}
	if (mod_name == "no-missing") {
		result.flags |= PathVariableModifierFlag::IGNORE_MISSING;
		return true;
	}
	if (mod_name == "no-scalarfs") {
		result.flags |= PathVariableModifierFlag::PASSTHRU_SCALARFS;
		return true;
	}
	if (mod_name == "no-protocols") {
		result.flags |= PathVariableModifierFlag::PASSTHRU_EXPLICIT_FS;
		return true;
	}
	if (mod_name == "no-cache") {
		result.flags |= PathVariableModifierFlag::NO_CACHE;
		return true;
	}
	if (mod_name == "append") {
		result.flags |= PathVariableModifierFlag::APPEND;
		result.append_value = ParseValue(mod_value);
		return true;
	}
	if (mod_name == "prepend") {
		result.flags |= PathVariableModifierFlag::PREPEND;
		result.prepend_value = ParseValue(mod_value);
		return true;
	}

	// Not a recognized modifier
	return false;
}

inline ParsedPathVariablePath PathVariableParser::Parse(const string &path) {
	ParsedPathVariablePath result;

	// Determine prefix and starting position
	string remainder;
	if (StringUtil::StartsWith(path, "tmp_pathvariable:")) {
		result.is_temp = true;
		remainder = path.substr(17); // len("tmp_pathvariable:")
	} else if (StringUtil::StartsWith(path, "pathvariable:")) {
		result.is_temp = false;
		remainder = path.substr(13); // len("pathvariable:")
	} else {
		// Not a pathvariable path - return empty result
		return result;
	}

	// Split by colons to find modifiers
	// Format: [modifier:]...[modifier:]varname[!value]
	// The last segment (or first segment without a following colon) is the variable name
	//
	// We need to be careful: the variable name could contain a ! for append/prepend
	// that was specified at the end rather than with the modifier
	//
	// Examples:
	//   pathvariable:varname              -> varname
	//   pathvariable:no-glob:varname      -> no-glob modifier, varname
	//   pathvariable:search:varname       -> search modifier, varname
	//   pathvariable:append:varname!/path -> append modifier with value, varname
	//   pathvariable:append!/path:varname -> append modifier with value, varname (alt syntax)

	vector<string> segments;
	size_t start = 0;
	size_t pos = 0;

	while ((pos = remainder.find(':', start)) != string::npos) {
		segments.push_back(remainder.substr(start, pos - start));
		start = pos + 1;
	}
	// Add the last segment
	segments.push_back(remainder.substr(start));

	if (segments.empty()) {
		return result;
	}

	// Process segments from left to right
	// Each segment is either a modifier or the variable name
	// The variable name is the first segment that's NOT a recognized modifier
	for (size_t i = 0; i < segments.size(); i++) {
		const string &segment = segments[i];

		// Try to parse as a modifier
		if (ParseModifier(segment, result)) {
			continue;
		}

		// Not a modifier - this is the variable name
		// Everything from here to the end is part of the variable name (rejoin with :)
		string var_part;
		for (size_t j = i; j < segments.size(); j++) {
			if (j > i) {
				var_part += ":";
			}
			var_part += segments[j];
		}

		// Check for ! in the variable part (for append/prepend values specified at end)
		// But only if we don't already have an append value
		auto bang_pos = var_part.find('!');
		if (bang_pos != string::npos && result.append_value.IsEmpty() && result.prepend_value.IsEmpty()) {
			// This could be varname!value syntax
			// Only treat as value if we have an append or prepend modifier without a value
			if (HasFlag(result.flags, PathVariableModifierFlag::APPEND) ||
			    HasFlag(result.flags, PathVariableModifierFlag::PREPEND)) {
				result.variable_name = var_part.substr(0, bang_pos);
				auto value = ParseValue(var_part.substr(bang_pos + 1));
				if (HasFlag(result.flags, PathVariableModifierFlag::APPEND)) {
					result.append_value = value;
				} else {
					result.prepend_value = value;
				}
			} else {
				// No append/prepend modifier, so ! is part of the variable name
				// (unlikely but possible)
				result.variable_name = var_part;
			}
		} else {
			result.variable_name = var_part;
		}
		break;
	}

	return result;
}

} // namespace duckdb
