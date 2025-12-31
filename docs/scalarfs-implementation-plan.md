# scalarfs Implementation Plan

A granular implementation plan for the scalarfs DuckDB extension.

## Phase Overview

| Phase | Feature | Description |
|-------|---------|-------------|
| 1a | `data:` (RFC 2397) | Base64 and URL-encoded data URIs |
| 1b | `data+varchar:` | Raw VARCHAR content, zero overhead |
| 1c | `data+blob:` | Escaped content with `\n`, `\t`, `\xNN` |
| 2a | `variable:` read | Read DuckDB variables as files |
| 2b | `variable:` write | Write to DuckDB variables via COPY |
| 3a | Encoding macros | `to_data_uri()`, `to_varchar_uri()`, `to_blob_uri()` |
| 3b | Decoding macros | `from_data_uri()`, `from_varchar_uri()`, `from_blob_uri()` |
| 3c | `to_scalarfs_uri()` | Auto-select optimal encoding |

---

## Key Design Decisions

### 1. Error Handling for Invalid Escapes

Invalid escape sequences in `data+blob:` URIs will **error immediately** with position information rather than pass through silently. Rationale: These URIs are typically generated programmatically, so corruption should be caught early.

### 2. Escape Sequences for `data+blob:`

Support common letter escapes in addition to hex escapes:

| Escape | Byte | Name |
|--------|------|------|
| `\n` | 0x0A | newline |
| `\r` | 0x0D | carriage return |
| `\t` | 0x09 | tab |
| `\0` | 0x00 | null |
| `\\` | 0x5C | backslash |
| `\xNN` | 0xNN | arbitrary hex byte |

Invalid escapes (e.g., `\q`, incomplete `\x4`) throw `IOException` with position info.

### 3. Variable API

Use DuckDB's internal `ClientConfig` API directly (no SQL string building):

```cpp
#include "duckdb/main/client_config.hpp"
#include "duckdb/common/file_opener.hpp"

// Get context from FileOpener
auto context = FileOpener::TryGetClientContext(opener);
auto &config = ClientConfig::GetConfig(*context);

// Read variable
Value result;
if (!config.GetUserVariable(var_name, result)) {
    throw IOException("Variable '%s' not found", var_name);
}

// Write variable
config.SetUserVariable(var_name, Value(content));
```

### 4. Base64 Utilities

Use DuckDB's built-in `Blob` class:

```cpp
#include "duckdb/common/types/blob.hpp"

// Encode
string encoded = Blob::ToBase64(input);

// Decode
string decoded = Blob::FromBase64(encoded_string);
```

### 5. Variable Write Type Selection

Determine VARCHAR vs BLOB based on content:
- If content contains null bytes → BLOB
- Otherwise → VARCHAR

This is simple and predictable. Writers that need BLOB can explicitly cast.

### 6. Test-First Development

Write `.test` files before implementation for each phase.

---

## DuckDB API Reference

### FileSystem Registration

```cpp
void ScalarfsExtension::Load(ExtensionLoader &loader) {
    auto &db = loader.GetDatabase();
    auto &fs = FileSystem::GetFileSystem(db);
    auto &vfs = fs.Cast<VirtualFileSystem>();

    vfs.RegisterSubSystem(make_uniq<DataURIFileSystem>());
    vfs.RegisterSubSystem(make_uniq<VariableFileSystem>());
}
```

### FileSystem Interface

Key methods to implement:

```cpp
class DataURIFileSystem : public FileSystem {
public:
    // Required
    bool CanHandleFile(const string &path) override;
    unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags,
                                     optional_ptr<FileOpener> opener) override;
    string GetName() const override { return "DataURIFileSystem"; }

    // Optional but recommended
    vector<string> Glob(const string &path, FileOpener *opener) override {
        return {path};  // Data URIs don't glob
    }
};
```

### FileHandle Interface

```cpp
class MemoryFileHandle : public FileHandle {
public:
    MemoryFileHandle(FileSystem &fs, string path, string data);

    void Close() override {}
    void Read(void *buffer, idx_t nr_bytes, idx_t location) override;
    idx_t GetFileSize() override { return data.size(); }

    // For read-only handles
    void Write(void *buffer, idx_t nr_bytes, idx_t location) override {
        throw IOException("Data URIs are read-only");
    }

private:
    string data;
};
```

---

## Phase 1a: `data:` (RFC 2397)

**Goal:** Standard data URI with URL and base64 decoding.

### Files to Create

- `src/include/data_uri_filesystem.hpp`
- `src/data_uri_filesystem.cpp`
- `src/include/memory_file_handle.hpp`
- `src/memory_file_handle.cpp`

### Protocol Detection

```cpp
bool CanHandleFile(const string &path) override {
    return StringUtil::StartsWith(path, "data:") ||
           StringUtil::StartsWith(path, "data+varchar:") ||
           StringUtil::StartsWith(path, "data+blob:");
}
```

Note: `data:` but NOT `data+` for RFC 2397 parsing.

### Parsing Logic

```cpp
string ParseDataURI(const string &uri) {
    // Format: data:[<mediatype>][;base64],<data>
    auto comma_pos = uri.find(',');
    if (comma_pos == string::npos) {
        throw IOException("Invalid data: URI - missing comma separator");
    }

    string metadata = uri.substr(5, comma_pos - 5);  // len("data:")
    string data = uri.substr(comma_pos + 1);

    bool is_base64 = metadata.find(";base64") != string::npos;

    return is_base64 ? Blob::FromBase64(data) : DecodeURLEncoded(data);
}
```

### URL Decoding

```cpp
string DecodeURLEncoded(const string &input) {
    string result;
    for (size_t i = 0; i < input.size(); i++) {
        if (input[i] == '%' && i + 2 < input.size()) {
            char hex[3] = {input[i + 1], input[i + 2], '\0'};
            char *end;
            long val = strtol(hex, &end, 16);
            if (end == hex + 2) {
                result += static_cast<char>(val);
                i += 2;
                continue;
            }
        }
        result += input[i];
    }
    return result;
}
```

### Tests (test/sql/data_uri.test)

```sql
# name: test/sql/data_uri.test
# group: [scalarfs]

require scalarfs

# Basic URL-encoded CSV
query II
SELECT * FROM read_csv('data:,a,b%0A1,2%0A3,4');
----
1	2
3	4

# Base64-encoded CSV (a,b\n1,2\n)
query II
SELECT * FROM read_csv('data:;base64,YSxiCjEsMgo=');
----
1	2

# With mediatype (ignored)
query I
SELECT a FROM read_json('data:application/json,{"a":42}');
----
42

# Empty content
query I
SELECT length(content) FROM read_text('data:,');
----
0

# Error: missing comma
statement error
SELECT * FROM read_csv('data:invalid');
----
Invalid data: URI

# URL encoding edge cases
query I
SELECT content FROM read_text('data:,%25%20%2C');
----
% ,
```

### Deliverables

- [ ] `MemoryFileHandle` class
- [ ] `DataURIFileSystem::CanHandleFile()`
- [ ] `DataURIFileSystem::OpenFile()`
- [ ] URL decoding
- [ ] Base64 decoding (via `Blob::FromBase64`)
- [ ] All tests passing

---

## Phase 1b: `data+varchar:`

**Goal:** Raw VARCHAR content with zero overhead.

### Implementation

Add to `DataURIFileSystem::OpenFile()`:

```cpp
if (StringUtil::StartsWith(path, "data+varchar:")) {
    string content = path.substr(13);  // len("data+varchar:")
    return make_uniq<MemoryFileHandle>(*this, path, std::move(content));
}
```

### Tests (test/sql/data_varchar.test)

```sql
# name: test/sql/data_varchar.test
# group: [scalarfs]

require scalarfs

# Simple JSON
query I
SELECT debug FROM read_json('data+varchar:{"debug":true}');
----
true

# CSV with embedded newlines
query II
SELECT * FROM read_csv('data+varchar:a,b
1,2
3,4');
----
1	2
3	4

# Empty content
query I
SELECT length(content) FROM read_text('data+varchar:');
----
0

# Special characters (no escaping needed)
query I
SELECT content FROM read_text('data+varchar:hello "world" & <stuff>');
----
hello "world" & <stuff>

# Colon in content
query I
SELECT content FROM read_text('data+varchar:key:value');
----
key:value

# Read-only check
statement error
COPY (SELECT 1 as a) TO 'data+varchar:test' (FORMAT csv);
----
read-only
```

### Deliverables

- [ ] `data+varchar:` prefix handling
- [ ] Zero-overhead content extraction
- [ ] All tests passing

---

## Phase 1c: `data+blob:`

**Goal:** Escaped content with letter escapes and hex escapes.

### Escape Sequence Decoding

```cpp
string DecodeBlobURI(const string &uri) {
    string content = uri.substr(10);  // len("data+blob:")
    string result;

    for (size_t i = 0; i < content.size(); i++) {
        if (content[i] != '\\') {
            result += content[i];
            continue;
        }

        // Escape sequence
        if (i + 1 >= content.size()) {
            throw IOException("Invalid escape sequence at end of data+blob: URI");
        }

        char next = content[i + 1];
        switch (next) {
            case '\\': result += '\\'; i++; break;
            case 'n':  result += '\n'; i++; break;
            case 'r':  result += '\r'; i++; break;
            case 't':  result += '\t'; i++; break;
            case '0':  result += '\0'; i++; break;
            case 'x': {
                if (i + 3 >= content.size()) {
                    throw IOException("Invalid \\x escape at position %d: expected 2 hex digits", i);
                }
                char hex[3] = {content[i + 2], content[i + 3], '\0'};
                char *end;
                long val = strtol(hex, &end, 16);
                if (end != hex + 2) {
                    throw IOException("Invalid \\x escape at position %d: '%s' is not valid hex",
                                      i, hex);
                }
                result += static_cast<char>(val);
                i += 3;
                break;
            }
            default:
                throw IOException("Invalid escape sequence '\\%c' at position %d", next, i);
        }
    }

    return result;
}
```

### Tests (test/sql/data_blob.test)

```sql
# name: test/sql/data_blob.test
# group: [scalarfs]

require scalarfs

# Letter escapes
query I
SELECT content FROM read_text('data+blob:line1\nline2');
----
line1
line2

query I
SELECT content FROM read_text('data+blob:col1\tcol2');
----
col1	col2

query I
SELECT content FROM read_text('data+blob:line1\r\nline2');
----
line1
line2

# Backslash escape
query I
SELECT content FROM read_text('data+blob:path\\to\\file');
----
path\to\file

# Hex escapes
query I
SELECT content FROM read_text('data+blob:bell\x07here');
----
bellhere

query I
SELECT octet_length(content) FROM read_blob('data+blob:a\x00b');
----
3

# Mixed escapes
query I
SELECT content FROM read_text('data+blob:start\ttab\nnewline\\slash\x1Besc');
----
start	tab
newline\slashesc

# Error: invalid escape
statement error
SELECT * FROM read_text('data+blob:test\qinvalid');
----
Invalid escape sequence

# Error: incomplete \x
statement error
SELECT * FROM read_text('data+blob:test\x4');
----
Invalid \\x escape

# Error: trailing backslash
statement error
SELECT * FROM read_text('data+blob:test\');
----
Invalid escape sequence at end

# Empty content
query I
SELECT length(content) FROM read_text('data+blob:');
----
0

# No escapes needed (passthrough)
query I
SELECT content FROM read_text('data+blob:plain text 123');
----
plain text 123
```

### Deliverables

- [ ] `DecodeBlobURI()` function
- [ ] Letter escape handling (`\n`, `\r`, `\t`, `\0`)
- [ ] Backslash escape (`\\`)
- [ ] Hex escape (`\xNN`)
- [ ] Error on invalid escapes with position info
- [ ] All tests passing

---

## Phase 2a: `variable:` (Read)

**Goal:** Read DuckDB variables as files.

### Files to Create

- `src/include/variable_filesystem.hpp`
- `src/variable_filesystem.cpp`

### Implementation

```cpp
class VariableFileSystem : public FileSystem {
public:
    bool CanHandleFile(const string &path) override {
        return StringUtil::StartsWith(path, "variable:");
    }

    unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags,
                                     optional_ptr<FileOpener> opener) override {
        string var_name = path.substr(9);  // len("variable:")

        auto context = FileOpener::TryGetClientContext(opener);
        if (!context) {
            throw IOException("Cannot access variables without client context");
        }

        if (flags.OpenForWriting()) {
            // Phase 2b
            throw IOException("variable: write not yet implemented");
        }

        auto &config = ClientConfig::GetConfig(*context);
        Value result;
        if (!config.GetUserVariable(var_name, result)) {
            throw IOException("Variable '%s' not found", var_name);
        }
        if (result.IsNull()) {
            throw IOException("Variable '%s' is NULL", var_name);
        }

        string content = result.ToString();
        return make_uniq<MemoryFileHandle>(*this, path, std::move(content));
    }

    string GetName() const override { return "VariableFileSystem"; }
};
```

### Tests (test/sql/variable_read.test)

```sql
# name: test/sql/variable_read.test
# group: [scalarfs]

require scalarfs

# Basic CSV read
statement ok
SET VARIABLE csv_data = 'a,b
1,2
3,4';

query II
SELECT * FROM read_csv('variable:csv_data');
----
1	2
3	4

# JSON read
statement ok
SET VARIABLE config = '{"debug":true,"port":8080}';

query II
SELECT debug, port FROM read_json('variable:config');
----
true	8080

# Text read
statement ok
SET VARIABLE message = 'Hello, World!';

query I
SELECT content FROM read_text('variable:message');
----
Hello, World!

# Error: variable not found
statement error
SELECT * FROM read_text('variable:nonexistent');
----
not found

# Error: NULL variable
statement ok
SET VARIABLE empty = NULL;

statement error
SELECT * FROM read_text('variable:empty');
----
is NULL

# Variable with special characters in content
statement ok
SET VARIABLE special = 'line1
line2	tabbed';

query I
SELECT content FROM read_text('variable:special');
----
line1
line2	tabbed
```

### Deliverables

- [ ] `VariableFileSystem` class
- [ ] Variable lookup via `ClientConfig::GetUserVariable()`
- [ ] Proper error messages for not found / NULL
- [ ] All tests passing

---

## Phase 2b: `variable:` (Write)

**Goal:** Enable `COPY TO 'variable:name'`.

### Implementation

```cpp
class VariableWriteHandle : public FileHandle {
public:
    VariableWriteHandle(FileSystem &fs, string path, string var_name,
                        ClientContext &context)
        : FileHandle(fs, path), var_name(var_name), context(context) {}

    void Write(void *buffer, idx_t nr_bytes, idx_t location) override {
        if (location + nr_bytes > data.size()) {
            data.resize(location + nr_bytes);
        }
        memcpy(data.data() + location, buffer, nr_bytes);
    }

    void Close() override {
        auto &config = ClientConfig::GetConfig(context);

        // Check for null bytes to determine type
        bool has_null = memchr(data.data(), '\0', data.size()) != nullptr;

        if (has_null) {
            config.SetUserVariable(var_name, Value::BLOB(data));
        } else {
            config.SetUserVariable(var_name, Value(data));
        }
    }

    idx_t GetFileSize() override { return data.size(); }
    void Read(void *buffer, idx_t nr_bytes, idx_t location) override {}
    void Truncate(int64_t new_size) override { data.resize(new_size); }

private:
    string var_name;
    string data;
    ClientContext &context;
};
```

Update `VariableFileSystem::OpenFile()`:

```cpp
if (flags.OpenForWriting()) {
    return make_uniq<VariableWriteHandle>(*this, path, var_name, *context);
}
```

### Tests (test/sql/variable_write.test)

```sql
# name: test/sql/variable_write.test
# group: [scalarfs]

require scalarfs

# Write CSV to variable
statement ok
CREATE TABLE t1 AS SELECT 1 as a, 2 as b UNION ALL SELECT 3, 4;

statement ok
COPY t1 TO 'variable:csv_out' (FORMAT csv, HEADER false);

query I
SELECT getvariable('csv_out');
----
1,2
3,4

# Write JSON to variable
statement ok
COPY t1 TO 'variable:json_out' (FORMAT json);

query I
SELECT getvariable('json_out') LIKE '[{"a":1%';
----
true

# Round-trip: CSV
statement ok
SET VARIABLE orig = 'x,y
10,20
30,40';

statement ok
CREATE TABLE rt AS SELECT * FROM read_csv('variable:orig');

statement ok
COPY rt TO 'variable:copy' (FORMAT csv, HEADER false);

query II
SELECT * FROM read_csv('variable:copy');
----
10	20
30	40

# Write creates new variable
statement ok
COPY (SELECT 'test' as msg) TO 'variable:new_var' (FORMAT csv, HEADER false);

query I
SELECT getvariable('new_var');
----
test

# Overwrite existing variable
statement ok
SET VARIABLE overwrite_me = 'old value';

statement ok
COPY (SELECT 'new value' as v) TO 'variable:overwrite_me' (FORMAT csv, HEADER false);

query I
SELECT getvariable('overwrite_me');
----
new value
```

### Deliverables

- [ ] `VariableWriteHandle` class
- [ ] Write accumulation in buffer
- [ ] Variable setting on `Close()`
- [ ] VARCHAR vs BLOB type selection
- [ ] Round-trip tests passing

---

## Phase 3a: Encoding Macros

**Goal:** `to_data_uri()`, `to_varchar_uri()`, `to_blob_uri()`

### Implementation

Register as scalar functions:

```cpp
void RegisterEncodingFunctions(DatabaseInstance &db) {
    // to_data_uri(content) -> 'data:;base64,<encoded>'
    ScalarFunction to_data("to_data_uri", {LogicalType::VARCHAR},
                           LogicalType::VARCHAR, ToDataURIFunction);
    ExtensionUtil::RegisterFunction(db, to_data);

    // to_varchar_uri(content) -> 'data+varchar:<content>'
    ScalarFunction to_varchar("to_varchar_uri", {LogicalType::VARCHAR},
                              LogicalType::VARCHAR, ToVarcharURIFunction);
    ExtensionUtil::RegisterFunction(db, to_varchar);

    // to_blob_uri(content) -> 'data+blob:<escaped>'
    ScalarFunction to_blob("to_blob_uri", {LogicalType::VARCHAR},
                           LogicalType::VARCHAR, ToBlobURIFunction);
    ExtensionUtil::RegisterFunction(db, to_blob);
}
```

### `to_blob_uri` Encoding Logic

```cpp
string EncodeBlobContent(const string &content) {
    string result;
    for (unsigned char c : content) {
        switch (c) {
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            case '\0': result += "\\0"; break;
            default:
                if (c < 0x20 || c == 0x7F) {
                    // Other control characters: use hex
                    char hex[5];
                    snprintf(hex, sizeof(hex), "\\x%02X", c);
                    result += hex;
                } else {
                    result += c;
                }
        }
    }
    return result;
}
```

### Tests (test/sql/encoding_macros.test)

```sql
# name: test/sql/encoding_macros.test
# group: [scalarfs]

require scalarfs

# to_data_uri
query I
SELECT to_data_uri('hello');
----
data:;base64,aGVsbG8=

# to_varchar_uri
query I
SELECT to_varchar_uri('{"a":1}');
----
data+varchar:{"a":1}

# to_blob_uri - letter escapes
query I
SELECT to_blob_uri('line1
line2');
----
data+blob:line1\nline2

query I
SELECT to_blob_uri('col1	col2');
----
data+blob:col1\tcol2

# to_blob_uri - backslash
query I
SELECT to_blob_uri('path\to\file');
----
data+blob:path\\to\\file

# to_blob_uri - control chars use hex
query I
SELECT to_blob_uri('bell' || chr(7) || 'here');
----
data+blob:bell\x07here

# Round-trip through filesystem
query I
SELECT content FROM read_text(to_varchar_uri('test content'));
----
test content

query I
SELECT content FROM read_text(to_blob_uri('line1
line2'));
----
line1
line2

query I
SELECT content FROM read_text(to_data_uri('binary test'));
----
binary test
```

### Deliverables

- [ ] `to_data_uri()` function
- [ ] `to_varchar_uri()` function
- [ ] `to_blob_uri()` function with letter escape preference
- [ ] Round-trip tests passing

---

## Phase 3b: Decoding Macros

**Goal:** `from_data_uri()`, `from_varchar_uri()`, `from_blob_uri()`, `from_scalarfs_uri()`

### Tests (test/sql/decoding_macros.test)

```sql
# name: test/sql/decoding_macros.test
# group: [scalarfs]

require scalarfs

# from_data_uri
query I
SELECT from_data_uri('data:;base64,aGVsbG8=');
----
hello

query I
SELECT from_data_uri('data:,hello%20world');
----
hello world

# from_varchar_uri
query I
SELECT from_varchar_uri('data+varchar:test content');
----
test content

# from_blob_uri
query I
SELECT from_blob_uri('data+blob:line1\nline2');
----
line1
line2

# from_scalarfs_uri (auto-detect)
query I
SELECT from_scalarfs_uri('data+varchar:auto');
----
auto

query I
SELECT from_scalarfs_uri('data+blob:with\ttab');
----
with	tab

query I
SELECT from_scalarfs_uri('data:;base64,YXV0bw==');
----
auto

# Error on unknown format
statement error
SELECT from_scalarfs_uri('unknown:format');
----
Unknown scalarfs URI format

# Round-trip encode/decode
query I
SELECT from_scalarfs_uri(to_data_uri('test'));
----
test

query I
SELECT from_scalarfs_uri(to_varchar_uri('test'));
----
test

query I
SELECT from_scalarfs_uri(to_blob_uri('test'));
----
test
```

### Deliverables

- [ ] `from_data_uri()` function
- [ ] `from_varchar_uri()` function
- [ ] `from_blob_uri()` function
- [ ] `from_scalarfs_uri()` auto-detect function
- [ ] All tests passing

---

## Phase 3c: `to_scalarfs_uri()` Auto-Select

**Goal:** Automatically choose optimal encoding.

### Selection Logic

```cpp
void ToScalarfsURIFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    UnaryExecutor::Execute<string_t, string_t>(
        args.data[0], result, args.size(),
        [&](string_t input) {
            string content = input.GetString();

            bool has_null = false;
            bool needs_escaping = false;
            int escape_count = 0;

            for (unsigned char c : content) {
                if (c == 0x00) {
                    has_null = true;
                    break;
                }
                if (c == '\\' || (c < 0x20 && c != '\t' && c != '\n' && c != '\r') || c == 0x7F) {
                    needs_escaping = true;
                    escape_count++;
                }
            }

            // Null bytes require base64
            if (has_null) {
                string encoded = Blob::ToBase64(content);
                return StringVector::AddString(result, "data:;base64," + encoded);
            }

            // No escaping needed: use varchar
            if (!needs_escaping) {
                return StringVector::AddString(result, "data+varchar:" + content);
            }

            // Light escaping: use blob
            if (escape_count * 4 < (int)content.size()) {
                string encoded = EncodeBlobContent(content);
                return StringVector::AddString(result, "data+blob:" + encoded);
            }

            // Heavy escaping: use base64
            string encoded = Blob::ToBase64(content);
            return StringVector::AddString(result, "data:;base64," + encoded);
        });
}
```

### Tests (test/sql/auto_select.test)

```sql
# name: test/sql/auto_select.test
# group: [scalarfs]

require scalarfs

# Plain text -> data+varchar:
query I
SELECT to_scalarfs_uri('hello world');
----
data+varchar:hello world

# Text with newlines -> data+varchar: (allowed)
query I
SELECT to_scalarfs_uri('line1
line2');
----
data+varchar:line1
line2

# Text with tabs -> data+varchar: (allowed)
query I
SELECT to_scalarfs_uri('col1	col2');
----
data+varchar:col1	col2

# Text with backslash -> data+blob:
query I
SELECT to_scalarfs_uri('path\to\file') LIKE 'data+blob:%';
----
true

# Text with control chars -> data+blob:
query I
SELECT to_scalarfs_uri('bell' || chr(7) || 'here') LIKE 'data+blob:%';
----
true

# Binary with null -> data:;base64,
query I
SELECT to_scalarfs_uri(chr(0) || 'test') LIKE 'data:;base64,%';
----
true

# Full round-trip for all content types
query I
SELECT from_scalarfs_uri(to_scalarfs_uri('plain text')) = 'plain text';
----
true

query I
SELECT from_scalarfs_uri(to_scalarfs_uri('with
newline')) = 'with
newline';
----
true

query I
SELECT from_scalarfs_uri(to_scalarfs_uri('path\to\file')) = 'path\to\file';
----
true

query I
SELECT from_scalarfs_uri(to_scalarfs_uri(chr(0) || 'binary')) = chr(0) || 'binary';
----
true
```

### Deliverables

- [ ] `to_scalarfs_uri()` auto-select function
- [ ] Content analysis logic
- [ ] All round-trip tests passing

---

## File Structure

```
src/
├── include/
│   ├── scalarfs_extension.hpp
│   ├── data_uri_filesystem.hpp
│   ├── variable_filesystem.hpp
│   └── memory_file_handle.hpp
├── scalarfs_extension.cpp
├── data_uri_filesystem.cpp
├── variable_filesystem.cpp
└── memory_file_handle.cpp

test/sql/
├── data_uri.test        (Phase 1a)
├── data_varchar.test    (Phase 1b)
├── data_blob.test       (Phase 1c)
├── variable_read.test   (Phase 2a)
├── variable_write.test  (Phase 2b)
├── encoding_macros.test (Phase 3a)
├── decoding_macros.test (Phase 3b)
└── auto_select.test     (Phase 3c)
```

---

## CMakeLists.txt Updates

```cmake
set(EXTENSION_SOURCES
    src/scalarfs_extension.cpp
    src/data_uri_filesystem.cpp
    src/variable_filesystem.cpp
    src/memory_file_handle.cpp
)
```

Remove OpenSSL dependency (not needed for this extension).

---

## Future Considerations

- **Compression variants**: `data+gzip:`, `data+zstd:`
- **Streaming**: For content larger than memory (low priority)
- **BLOB variable support**: Native BLOB reading/writing
- **query_table extension**: Separate extension for table cell extraction
