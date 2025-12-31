# scalarfs Implementation Plan

A granular implementation plan for the scalarfs DuckDB extension.

## Phase Overview

| Phase | Feature | Effort |
|-------|---------|--------|
| 1a | `data:` (RFC 2397) | 1 day |
| 1b | `data+varchar:` | 0.5 day |
| 1c | `data+blob:` | 0.5 day |
| 2a | `variable:` read | 0.5 day |
| 2b | `variable:` write | 0.5 day |
| 3a | Encoding macros | 0.5 day |
| 3b | Decoding macros | 0.5 day |
| 3c | `to_scalarfs_uri()` auto-select | 0.5 day |

**Total: ~5 days**

---

## Prerequisites

### Project Setup

```bash
git clone https://github.com/duckdb/extension-template scalarfs
cd scalarfs
# Update extension name in CMakeLists.txt and extension_config.cmake
```

### Core Infrastructure

```cpp
// src/include/scalarfs_extension.hpp
#pragma once
#include "duckdb.hpp"

namespace duckdb {

class ScalarfsExtension : public Extension {
public:
    void Load(DuckDB &db) override;
    std::string Name() override { return "scalarfs"; }
};

} // namespace duckdb
```

```cpp
// src/include/memory_file_handle.hpp
#pragma once
#include "duckdb.hpp"

namespace duckdb {

class MemoryFileHandle : public FileHandle {
public:
    MemoryFileHandle(FileSystem &fs, string path, string data);
    
    void Close() override {}
    void Read(void *buffer, idx_t nr_bytes, idx_t location) override;
    idx_t GetFileSize() override;
    void Write(void *buffer, idx_t nr_bytes, idx_t location) override;
    void Truncate(idx_t new_size) override;
    
private:
    string data;
};

} // namespace duckdb
```

---

## Phase 1a: `data:` (RFC 2397)

**Goal:** Standard data URI with URL and base64 decoding.

### Files

- `src/data_uri_filesystem.cpp`
- `src/include/data_uri_filesystem.hpp`

### Implementation

```cpp
class DataURIFileSystem : public FileSystem {
public:
    bool CanHandleFile(const string &path) override {
        return StringUtil::StartsWith(path, "data:") &&
               !StringUtil::StartsWith(path, "data+");
    }
    
    unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags,
                                     optional_ptr<FileOpener> opener) override {
        if (flags.OpenForWriting()) {
            throw IOException("data: URIs are read-only");
        }
        
        auto content = ParseDataURI(path);
        return make_uniq<MemoryFileHandle>(*this, path, std::move(content));
    }
    
    vector<string> Glob(const string &path, FileOpener *opener) override {
        return {path};
    }
    
private:
    string ParseDataURI(const string &uri) {
        // Format: data:[<mediatype>][;base64],<data>
        auto comma_pos = uri.find(',');
        if (comma_pos == string::npos) {
            throw IOException("Invalid data: URI - missing comma");
        }
        
        string metadata = uri.substr(5, comma_pos - 5);
        string data = uri.substr(comma_pos + 1);
        
        bool is_base64 = metadata.find(";base64") != string::npos;
        
        return is_base64 ? DecodeBase64(data) : DecodeURLEncoded(data);
    }
    
    string DecodeURLEncoded(const string &input) {
        string result;
        for (size_t i = 0; i < input.size(); i++) {
            if (input[i] == '%' && i + 2 < input.size()) {
                int val;
                if (sscanf(input.substr(i + 1, 2).c_str(), "%x", &val) == 1) {
                    result += static_cast<char>(val);
                    i += 2;
                    continue;
                }
            }
            result += input[i];
        }
        return result;
    }
    
    string DecodeBase64(const string &input) {
        return Base64::Decode(input);
    }
};
```

### Tests

```sql
-- URL-encoded
SELECT * FROM read_csv('data:,a,b%0A1,2%0A3,4');

-- Base64
SELECT * FROM read_csv('data:;base64,YSxiCjEsMgo=');

-- With mediatype
SELECT * FROM read_json('data:application/json,{"a":1}');

-- Empty
SELECT length(content) FROM read_text('data:,');

-- Error: no comma
SELECT * FROM read_csv('data:invalid');
```

### Deliverables

- [ ] `MemoryFileHandle` implementation
- [ ] `DataURIFileSystem` class
- [ ] URL decoding
- [ ] Base64 decoding
- [ ] Unit tests

---

## Phase 1b: `data+varchar:`

**Goal:** Raw VARCHAR content, zero overhead.

### Implementation

Add to `DataURIFileSystem`:

```cpp
bool CanHandleFile(const string &path) override {
    return StringUtil::StartsWith(path, "data:") ||
           StringUtil::StartsWith(path, "data+varchar:") ||
           StringUtil::StartsWith(path, "data+blob:");
}

unique_ptr<FileHandle> OpenFile(const string &path, ...) override {
    if (flags.OpenForWriting()) {
        throw IOException("data: URIs are read-only");
    }
    
    string content;
    if (StringUtil::StartsWith(path, "data+varchar:")) {
        content = path.substr(13);  // len("data+varchar:")
    } else if (StringUtil::StartsWith(path, "data+blob:")) {
        content = DecodeBlobURI(path);
    } else {
        content = ParseDataURI(path);
    }
    
    return make_uniq<MemoryFileHandle>(*this, path, std::move(content));
}
```

### Tests

```sql
-- Simple content
SELECT * FROM read_json('data+varchar:{"a":1}');

-- With newlines
SELECT * FROM read_csv('data+varchar:a,b
1,2
3,4');

-- Empty
SELECT length(content) FROM read_text('data+varchar:');

-- In a table
CREATE TABLE t(path VARCHAR);
INSERT INTO t VALUES ('data+varchar:{"x":1}');
SELECT * FROM read_json((SELECT path FROM t LIMIT 1));
```

### Deliverables

- [ ] `data+varchar:` parsing
- [ ] Unit tests
- [ ] Verify zero overhead (content = path - prefix)

---

## Phase 1c: `data+blob:`

**Goal:** Escaped BLOB content with `\\` and `\xNN`.

### Implementation

```cpp
string DecodeBlobURI(const string &uri) {
    string content = uri.substr(10);  // len("data+blob:")
    string result;
    
    for (size_t i = 0; i < content.size(); i++) {
        if (content[i] == '\\' && i + 1 < content.size()) {
            char next = content[i + 1];
            if (next == '\\') {
                result += '\\';
                i++;
            } else if (next == 'x' && i + 3 < content.size()) {
                int val;
                if (sscanf(content.substr(i + 2, 2).c_str(), "%x", &val) == 1) {
                    result += static_cast<char>(val);
                    i += 3;
                } else {
                    result += content[i];
                }
            } else {
                result += content[i];
            }
        } else {
            result += content[i];
        }
    }
    
    return result;
}
```

### Tests

```sql
-- Backslash
SELECT * FROM read_text('data+blob:path\\to\\file');
-- Expected: path\to\file

-- Hex escapes
SELECT * FROM read_text('data+blob:line1\x0Aline2');
-- Expected: line1<newline>line2

-- Null byte
SELECT length(content) FROM read_blob('data+blob:a\x00b');
-- Expected: 3

-- Mixed
SELECT * FROM read_text('data+blob:test\\slash\x09tab\x0Anewline');

-- Invalid escape (pass through)
SELECT * FROM read_text('data+blob:test\qinvalid');
-- Expected: test\qinvalid
```

### Deliverables

- [ ] `DecodeBlobURI()` function
- [ ] `\\` escape handling
- [ ] `\xNN` hex escape handling
- [ ] Invalid escape fallback (pass through)
- [ ] Unit tests

---

## Phase 2a: `variable:` (Read)

**Goal:** Read DuckDB variables as files.

### Files

- `src/variable_filesystem.cpp`
- `src/include/variable_filesystem.hpp`

### Implementation

```cpp
class VariableFileSystem : public FileSystem {
public:
    bool CanHandleFile(const string &path) override {
        return StringUtil::StartsWith(path, "variable:");
    }
    
    unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags,
                                     optional_ptr<FileOpener> opener) override {
        string var_name = path.substr(9);
        auto &context = GetClientContext(opener);
        
        if (flags.OpenForWriting()) {
            throw IOException("variable: write not yet implemented");
        }
        
        string content = GetVariable(context, var_name);
        return make_uniq<MemoryFileHandle>(*this, path, std::move(content));
    }
    
private:
    string GetVariable(ClientContext &context, const string &name) {
        auto result = context.Query(
            "SELECT getvariable('" + EscapeSQL(name) + "')", false);
        
        if (result->HasError()) {
            throw IOException("Variable '" + name + "' not found");
        }
        if (result->RowCount() == 0) {
            throw IOException("Variable '" + name + "' not found");
        }
        
        auto value = result->GetValue(0, 0);
        if (value.IsNull()) {
            throw IOException("Variable '" + name + "' is NULL");
        }
        
        return value.ToString();
    }
};
```

### Tests

```sql
-- Basic read
SET VARIABLE csv_data = 'a,b
1,2';
SELECT * FROM read_csv('variable:csv_data');

-- JSON
SET VARIABLE config = '{"debug":true}';
SELECT * FROM read_json('variable:config');

-- Error: not found
SELECT * FROM read_text('variable:nonexistent');

-- Error: NULL
SET VARIABLE empty = NULL;
SELECT * FROM read_text('variable:empty');
```

### Deliverables

- [ ] `VariableFileSystem` class
- [ ] Variable lookup via `getvariable()`
- [ ] Error handling
- [ ] Unit tests

---

## Phase 2b: `variable:` (Write)

**Goal:** Enable COPY TO variable:name.

### Implementation

```cpp
class VariableWriteHandle : public FileHandle {
public:
    VariableWriteHandle(FileSystem &fs, string path, 
                        string var_name, ClientContext &context)
        : FileHandle(fs, path), var_name(var_name), context(context) {}
    
    void Write(void *buffer, idx_t nr_bytes, idx_t location) override {
        if (location + nr_bytes > data.size()) {
            data.resize(location + nr_bytes);
        }
        memcpy(data.data() + location, buffer, nr_bytes);
    }
    
    void Close() override {
        string escaped = StringUtil::Replace(data, "'", "''");
        context.Query("SET VARIABLE " + var_name + " = '" + escaped + "'", false);
    }
    
    idx_t GetFileSize() override { return data.size(); }
    void Read(void *buffer, idx_t nr_bytes, idx_t location) override {}
    
private:
    string var_name;
    string data;
    ClientContext &context;
};
```

Update `OpenFile`:

```cpp
if (flags.OpenForWriting()) {
    return make_uniq<VariableWriteHandle>(*this, path, var_name, context);
}
```

### Tests

```sql
-- Write CSV
CREATE TABLE t AS SELECT 1 as a, 2 as b;
COPY t TO 'variable:output' (FORMAT csv);
SELECT getvariable('output');

-- Write JSON
COPY t TO 'variable:json_out' (FORMAT json);
SELECT getvariable('json_out');

-- Round-trip
SET VARIABLE orig = 'x,y
1,2';
CREATE TABLE rt AS SELECT * FROM read_csv('variable:orig');
COPY rt TO 'variable:copy' (FORMAT csv);
SELECT * FROM read_csv('variable:copy');
```

### Deliverables

- [ ] `VariableWriteHandle` class
- [ ] Write accumulation
- [ ] Variable setting on close
- [ ] Round-trip tests

---

## Phase 3a: Encoding Macros

**Goal:** `to_data_uri()`, `to_varchar_uri()`, `to_blob_uri()`

### Implementation

```cpp
void RegisterEncodingMacros(DatabaseInstance &db) {
    // to_data_uri: base64 encoding
    auto to_data = ScalarFunction("to_data_uri", {LogicalType::VARCHAR}, 
                                   LogicalType::VARCHAR, ToDataURIFunction);
    ExtensionUtil::RegisterFunction(db, to_data);
    
    // to_varchar_uri: just prepend prefix
    auto to_varchar = ScalarFunction("to_varchar_uri", {LogicalType::VARCHAR},
                                      LogicalType::VARCHAR, ToVarcharURIFunction);
    ExtensionUtil::RegisterFunction(db, to_varchar);
    
    // to_blob_uri: escape special chars
    auto to_blob = ScalarFunction("to_blob_uri", {LogicalType::VARCHAR},
                                   LogicalType::VARCHAR, ToBlobURIFunction);
    ExtensionUtil::RegisterFunction(db, to_blob);
}

void ToDataURIFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    UnaryExecutor::Execute<string_t, string_t>(
        args.data[0], result, args.size(),
        [&](string_t input) {
            string encoded = Base64::Encode(input.GetString());
            return StringVector::AddString(result, "data:;base64," + encoded);
        });
}

void ToVarcharURIFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    UnaryExecutor::Execute<string_t, string_t>(
        args.data[0], result, args.size(),
        [&](string_t input) {
            return StringVector::AddString(result, "data+varchar:" + input.GetString());
        });
}

void ToBlobURIFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    UnaryExecutor::Execute<string_t, string_t>(
        args.data[0], result, args.size(),
        [&](string_t input) {
            string encoded = EncodeBlobContent(input.GetString());
            return StringVector::AddString(result, "data+blob:" + encoded);
        });
}

string EncodeBlobContent(const string &content) {
    string result;
    for (unsigned char c : content) {
        if (c == '\\') {
            result += "\\\\";
        } else if (c < 0x20 || c == 0x7F) {
            char hex[5];
            snprintf(hex, sizeof(hex), "\\x%02X", c);
            result += hex;
        } else {
            result += c;
        }
    }
    return result;
}
```

### Tests

```sql
-- to_data_uri
SELECT to_data_uri('hello');
-- Expected: data:;base64,aGVsbG8=

-- to_varchar_uri
SELECT to_varchar_uri('{"a":1}');
-- Expected: data+varchar:{"a":1}

-- to_blob_uri
SELECT to_blob_uri('line1
line2');
-- Expected: data+blob:line1\x0Aline2

-- to_blob_uri with backslash
SELECT to_blob_uri('path\to\file');
-- Expected: data+blob:path\\to\\file

-- Round-trip
SELECT * FROM read_json(to_varchar_uri('{"test":true}'));
```

### Deliverables

- [ ] `to_data_uri()` function
- [ ] `to_varchar_uri()` function
- [ ] `to_blob_uri()` function
- [ ] `EncodeBlobContent()` helper
- [ ] Unit tests

---

## Phase 3b: Decoding Macros

**Goal:** `from_data_uri()`, `from_varchar_uri()`, `from_blob_uri()`, `from_scalarfs_uri()`

### Implementation

```cpp
void RegisterDecodingMacros(DatabaseInstance &db) {
    // from_data_uri
    auto from_data = ScalarFunction("from_data_uri", {LogicalType::VARCHAR},
                                     LogicalType::VARCHAR, FromDataURIFunction);
    ExtensionUtil::RegisterFunction(db, from_data);
    
    // from_varchar_uri
    auto from_varchar = ScalarFunction("from_varchar_uri", {LogicalType::VARCHAR},
                                        LogicalType::VARCHAR, FromVarcharURIFunction);
    ExtensionUtil::RegisterFunction(db, from_varchar);
    
    // from_blob_uri
    auto from_blob = ScalarFunction("from_blob_uri", {LogicalType::VARCHAR},
                                     LogicalType::VARCHAR, FromBlobURIFunction);
    ExtensionUtil::RegisterFunction(db, from_blob);
    
    // from_scalarfs_uri (auto-detect)
    auto from_any = ScalarFunction("from_scalarfs_uri", {LogicalType::VARCHAR},
                                    LogicalType::VARCHAR, FromScalarfsURIFunction);
    ExtensionUtil::RegisterFunction(db, from_any);
}

void FromScalarfsURIFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    UnaryExecutor::Execute<string_t, string_t>(
        args.data[0], result, args.size(),
        [&](string_t input) {
            string uri = input.GetString();
            
            if (StringUtil::StartsWith(uri, "data+varchar:")) {
                return StringVector::AddString(result, uri.substr(13));
            } else if (StringUtil::StartsWith(uri, "data+blob:")) {
                return StringVector::AddString(result, DecodeBlobContent(uri.substr(10)));
            } else if (StringUtil::StartsWith(uri, "data:")) {
                return StringVector::AddString(result, DecodeDataURI(uri));
            } else {
                throw InvalidInputException("Unknown scalarfs URI format: " + uri);
            }
        });
}
```

### Tests

```sql
-- from_data_uri
SELECT from_data_uri('data:;base64,aGVsbG8=');
-- Expected: hello

-- from_varchar_uri
SELECT from_varchar_uri('data+varchar:test content');
-- Expected: test content

-- from_blob_uri
SELECT from_blob_uri('data+blob:line1\x0Aline2');
-- Expected: line1<newline>line2

-- from_scalarfs_uri (auto-detect)
SELECT from_scalarfs_uri('data+varchar:auto');
SELECT from_scalarfs_uri('data+blob:auto\x0A');
SELECT from_scalarfs_uri('data:;base64,YXV0bw==');

-- Round-trip
SELECT from_scalarfs_uri(to_scalarfs_uri('test content'));
```

### Deliverables

- [ ] `from_data_uri()` function
- [ ] `from_varchar_uri()` function
- [ ] `from_blob_uri()` function
- [ ] `from_scalarfs_uri()` auto-detect function
- [ ] Unit tests

---

## Phase 3c: `to_scalarfs_uri()` Auto-Select

**Goal:** Automatically choose optimal encoding.

### Implementation

```cpp
void ToScalarfsURIFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    UnaryExecutor::Execute<string_t, string_t>(
        args.data[0], result, args.size(),
        [&](string_t input) {
            string content = input.GetString();
            
            // Check if safe for raw varchar
            bool needs_escaping = false;
            int escape_count = 0;
            
            for (unsigned char c : content) {
                if (c == 0x00) {
                    // Null byte: must use base64
                    string encoded = Base64::Encode(content);
                    return StringVector::AddString(result, "data:;base64," + encoded);
                }
                if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') {
                    needs_escaping = true;
                    escape_count++;
                }
                if (c == 0x7F || c == '\\') {
                    needs_escaping = true;
                    escape_count++;
                }
            }
            
            if (!needs_escaping) {
                // Safe for raw varchar
                return StringVector::AddString(result, "data+varchar:" + content);
            }
            
            // Check if blob escaping is efficient (< 10% overhead)
            if (escape_count * 4 < content.size()) {
                string encoded = EncodeBlobContent(content);
                return StringVector::AddString(result, "data+blob:" + encoded);
            }
            
            // Fall back to base64
            string encoded = Base64::Encode(content);
            return StringVector::AddString(result, "data:;base64," + encoded);
        });
}
```

### Tests

```sql
-- Plain text → data+varchar:
SELECT to_scalarfs_uri('hello world');
-- Expected: data+varchar:hello world

-- Text with newlines → data+varchar: (allowed)
SELECT to_scalarfs_uri('line1
line2');
-- Expected: data+varchar:line1\nline2

-- Text with some control chars → data+blob:
SELECT to_scalarfs_uri('text' || chr(7) || 'bell');
-- Expected: data+blob:text\x07bell

-- Binary content → data:;base64,
SELECT to_scalarfs_uri(chr(0) || chr(1) || chr(2));
-- Expected: data:;base64,...

-- Full round-trip
SELECT from_scalarfs_uri(to_scalarfs_uri(content)) = content
FROM (VALUES ('plain'), ('with
newline'), (chr(0) || 'binary')) t(content);
-- Expected: all true
```

### Deliverables

- [ ] `to_scalarfs_uri()` auto-select function
- [ ] Content analysis logic
- [ ] Threshold tuning
- [ ] Round-trip tests for all content types

---

## Milestone Summary

| Phase | Feature | Est. |
|-------|---------|------|
| 1a | `data:` RFC 2397 | 1 day |
| 1b | `data+varchar:` | 0.5 day |
| 1c | `data+blob:` | 0.5 day |
| 2a | `variable:` read | 0.5 day |
| 2b | `variable:` write | 0.5 day |
| 3a | Encoding macros | 0.5 day |
| 3b | Decoding macros | 0.5 day |
| 3c | Auto-select | 0.5 day |

**Total: ~5 days**

---

## Future Considerations

- **query_table extension**: Separate extension for cell extraction
- **Compression**: `data+gzip:` or `data+zstd:` variants
- **Streaming**: For large content (low priority given use case)
- **BLOB support**: Native BLOB variable handling
