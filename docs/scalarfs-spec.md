# scalarfs — DuckDB Scalar Filesystem Extension

A DuckDB extension for treating scalar values (variables, literals) as files.

## Overview

scalarfs enables DuckDB's reader functions (`read_csv`, `read_json`, `read_xml`, etc.) to work on content from scalar sources: variables and inline literals.

| Protocol | Purpose | Mode |
|----------|---------|------|
| `variable:` | DuckDB variable as file | R/W |
| `data:` | RFC 2397 data URI (base64/url-encoded) | R |
| `data+varchar:` | Raw VARCHAR content as file | R |
| `data+blob:` | Escaped BLOB content as file | R |

## Installation

```sql
LOAD scalarfs;
```

---

## Motivation

DuckDB's reader functions expect file paths. scalarfs bridges the gap when your content is already in memory:

```sql
-- File (already works)
SELECT * FROM read_json('/path/to/config.json');

-- Variable (now works)
SET VARIABLE config = '{"debug":true}';
SELECT * FROM read_json('variable:config');

-- Inline literal (now works)
SELECT * FROM read_json('data+varchar:{"debug":true}');
```

Same reader, multiple sources.

---

## Protocols

### `variable:name`

Read from or write to a DuckDB variable as if it were a file.

**Syntax:**
```
variable:variable_name
```

**Read:**
```sql
SET VARIABLE my_data = 'name,value
Alice,100
Bob,200';

SELECT * FROM read_csv('variable:my_data');
```

**Write:**
```sql
COPY my_table TO 'variable:exported' (FORMAT csv);
COPY (SELECT * FROM t WHERE active) TO 'variable:filtered' (FORMAT json);

-- Verify
SELECT getvariable('exported');
```

**Behavior:**
- Read: Returns variable value as raw bytes
- Write: Sets variable to output bytes
- Variable must exist for reads; created on write if nonexistent
- Supports VARCHAR and BLOB variable types

---

### `data:` (RFC 2397)

Standard data URI scheme for inline content with encoding.

**Syntax:**
```
data:[<mediatype>][;base64],<data>
```

**Examples:**
```sql
-- URL-encoded
SELECT * FROM read_csv('data:,a,b%0A1,2%0A3,4');

-- Base64 (preferred for complex content)
SELECT * FROM read_csv('data:;base64,YSxiCjEsMgo=');

-- With mediatype (ignored by reader)
SELECT * FROM read_json('data:application/json;base64,eyJhIjoxfQ==');
```

**URL Encoding Reference:**

| Character | Encoded |
|-----------|---------|
| newline | `%0A` |
| space | `%20` |
| `%` | `%25` |
| `,` | `%2C` |

**Behavior:**
- Read-only
- `;base64` triggers base64 decoding; otherwise URL decoding
- ~33% overhead for base64, variable for URL encoding
- Best for: binary content, content with many special characters

---

### `data+varchar:` (Raw VARCHAR)

Raw VARCHAR content with zero encoding overhead.

**Syntax:**
```
data+varchar:<content>
```

Everything after `data+varchar:` is the literal content. No escaping, no encoding.

**Examples:**
```sql
-- Simple JSON
SELECT * FROM read_json('data+varchar:{"debug":true,"port":8080}');

-- CSV with newlines (works because SQL strings handle this)
SELECT * FROM read_csv('data+varchar:a,b
1,2
3,4');

-- Stored in a column
INSERT INTO configs(path) VALUES ('data+varchar:{"env":"prod"}');
SELECT * FROM read_json(path) FROM configs;
```

**Behavior:**
- Read-only
- Zero overhead (just 13-byte prefix)
- Content must be valid VARCHAR (no null bytes)
- Best for: text content, command output, JSON, CSV, XML

---

### `data+blob:` (Escaped BLOB)

BLOB content with minimal escape sequences for special characters.

**Syntax:**
```
data+blob:<escaped_content>
```

**Escape Sequences:**

| Sequence | Byte | Description |
|----------|------|-------------|
| `\\` | `0x5C` | Literal backslash |
| `\xNN` | `0xNN` | Hex-encoded byte |

All other bytes pass through unchanged.

**Examples:**
```sql
-- Text with null byte
SELECT * FROM read_text('data+blob:hello\x00world');

-- Binary header + text
SELECT * FROM read_blob('data+blob:\x89PNG\x0D\x0A\x1A\x0Arest of png...');

-- Content with literal backslash
SELECT * FROM read_text('data+blob:path\\to\\file');

-- Control characters
SELECT * FROM read_text('data+blob:line1\x0Aline2\x0D\x0Aline3');
```

**Behavior:**
- Read-only
- Minimal overhead (only backslashes and special bytes need escaping)
- Handles null bytes and all control characters
- Best for: mostly-text content with occasional binary/control chars

---

## Helper Macros

scalarfs provides macros for converting between content and URIs:

### Encoding Macros

```sql
-- Standard data: URI (base64)
to_data_uri(content)          -- Returns 'data:;base64,<encoded>'

-- Raw varchar (zero overhead)
to_varchar_uri(content)       -- Returns 'data+varchar:<content>'

-- Escaped blob (minimal overhead)  
to_blob_uri(content)          -- Returns 'data+blob:<escaped>'

-- Auto-select optimal encoding
to_scalarfs_uri(content)      -- Picks best representation
```

### Decoding Macros

```sql
-- Decode any scalarfs URI
from_data_uri(uri)            -- Decodes data:;base64,... 
from_varchar_uri(uri)         -- Decodes data+varchar:...
from_blob_uri(uri)            -- Decodes data+blob:...
from_scalarfs_uri(uri)        -- Auto-detects and decodes any variant
```

### Auto-Selection Logic

`to_scalarfs_uri()` picks the optimal encoding:

| Content Type | Chosen Protocol | Why |
|--------------|-----------------|-----|
| Safe VARCHAR (no control chars except `\n`, `\r`, `\t`) | `data+varchar:` | Zero overhead |
| Text with some control chars | `data+blob:` | Minimal escaping |
| Binary / heavy escaping needed | `data:;base64,` | Predictable overhead |

```sql
-- Implementation sketch
CREATE MACRO to_scalarfs_uri(content) AS (
  CASE
    -- Safe for raw varchar: printable ASCII/UTF-8 + whitespace
    WHEN regexp_matches(content, '^[\x09\x0A\x0D\x20-\x7E\x80-\xFF]*$')
    THEN to_varchar_uri(content)
    
    -- Mostly safe: use blob escaping if <10% needs escaping
    WHEN (octet_length(content) - octet_length(
           regexp_replace(content, '[\x00-\x08\x0B\x0C\x0E-\x1F\x7F\\]', '', 'g')
         )) * 10 < octet_length(content)
    THEN to_blob_uri(content)
    
    -- Heavy escaping needed: use base64
    ELSE to_data_uri(content)
  END
);
```

---

## Usage Patterns

### Command Output Storage

Store small outputs inline, large outputs externally:

```sql
CREATE TABLE command_results(
  id INT PRIMARY KEY,
  command VARCHAR,
  exit_code INT,
  stdout_path VARCHAR,  -- 'data+varchar:...' OR '/path/...' OR 's3://...'
  stderr_path VARCHAR,
  created_at TIMESTAMP
);

-- Storage decision macro
CREATE MACRO store_output(content, external_path, threshold := 4096) AS (
  CASE 
    WHEN octet_length(content) <= threshold 
    THEN to_scalarfs_uri(content)
    ELSE external_path
  END
);

-- Insert with auto-storage
INSERT INTO command_results VALUES (
  1, 'ls -la', 0,
  store_output(stdout_content, '/outputs/1/stdout.txt'),
  store_output(stderr_content, '/outputs/1/stderr.txt'),
  now()
);
```

### Transparent Facade

The path column abstracts storage location:

```sql
-- All these "just work" with read_text():
-- stdout_path = 'data+varchar:file1\nfile2\nfile3'
-- stdout_path = 'data+blob:binary\x00content'
-- stdout_path = 'data:;base64,aGVsbG8gd29ybGQ='
-- stdout_path = '/mnt/outputs/1/stdout.txt'
-- stdout_path = 's3://bucket/outputs/1/stdout.txt'
-- stdout_path = 'tar:///archives/batch.tar/1/stdout.txt'

-- Facade view
CREATE VIEW command_outputs AS
SELECT 
  id,
  command,
  exit_code,
  read_text(stdout_path) AS stdout,
  read_text(stderr_path) AS stderr,
  created_at
FROM command_results;
```

### Migration is Just UPDATE

```sql
-- Move inline to S3 (after copying content)
UPDATE command_results 
SET stdout_path = 's3://archive/' || id || '/stdout.txt'
WHERE stdout_path LIKE 'data%'
  AND created_at < '2024-01-01';

-- Repack into tar archives
UPDATE command_results
SET stdout_path = 'tar:///archives/2024.tar/' || id || '/stdout.txt'
WHERE created_at < '2024-01-01';
```

### Inline Test Data

```sql
-- Quick tests without files
SELECT * FROM read_csv('data+varchar:name,score
Alice,95
Bob,87
Carol,92');

SELECT * FROM read_json('data+varchar:[{"id":1},{"id":2}]');
```

---

## Implementation Notes

### FileSystem Registration

```cpp
void ScalarfsExtension::Load(DuckDB &db) {
    auto &fs = db.GetFileSystem();
    fs.RegisterSubSystem(make_uniq<VariableFileSystem>());
    fs.RegisterSubSystem(make_uniq<DataURIFileSystem>());
    
    // Register macros
    RegisterMacros(db);
}
```

### Protocol Detection

| Protocol | Detection |
|----------|-----------|
| `variable:` | `StartsWith("variable:")` |
| `data:` | `StartsWith("data:")` and NOT `StartsWith("data+")` |
| `data+varchar:` | `StartsWith("data+varchar:")` |
| `data+blob:` | `StartsWith("data+blob:")` |

### `data+blob:` Decoding

```cpp
string DecodeBlobURI(const string &uri) {
    string content = uri.substr(10);  // len("data+blob:")
    string result;
    
    for (size_t i = 0; i < content.size(); i++) {
        if (content[i] == '\\' && i + 1 < content.size()) {
            if (content[i + 1] == '\\') {
                result += '\\';
                i++;
            } else if (content[i + 1] == 'x' && i + 3 < content.size()) {
                int hex_val;
                if (sscanf(content.substr(i + 2, 2).c_str(), "%x", &hex_val) == 1) {
                    result += static_cast<char>(hex_val);
                    i += 3;
                } else {
                    result += content[i];  // Invalid escape, keep as-is
                }
            } else {
                result += content[i];  // Unknown escape, keep as-is
            }
        } else {
            result += content[i];
        }
    }
    
    return result;
}
```

### `data+blob:` Encoding

```cpp
string EncodeBlobURI(const string &content) {
    string result = "data+blob:";
    
    for (unsigned char c : content) {
        if (c == '\\') {
            result += "\\\\";
        } else if (c < 0x20 || c == 0x7F) {
            // Control characters: \xNN
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

### Error Handling

| Condition | Error |
|-----------|-------|
| `variable:` — not found | "Variable 'X' not found" |
| `variable:` — is NULL | "Variable 'X' is NULL" |
| `data:` — missing comma | "Invalid data: URI - missing comma" |
| `data:` — invalid base64 | "Invalid base64 encoding" |
| `data+blob:` — invalid escape | "Invalid escape sequence at position N" |

---

## Limitations

- `data:` and `data+varchar:` content limited by practical string length limits
- `data+varchar:` cannot contain null bytes (use `data+blob:` or `data:`)
- Write support only for `variable:`
- No streaming; entire content buffered before read

---

## Related Extensions

For more complex scenarios, see:

- **query_table** (separate extension): Extract content from table cells
  ```sql
  SELECT * FROM read_json('query_table://configs.data/env=prod/one.json');
  ```

---

## License

MIT
