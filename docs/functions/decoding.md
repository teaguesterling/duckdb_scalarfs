# Decoding Functions

scalarfs provides functions to extract content from URI format back to raw data.

## Function Summary

| Function | Input Format | Description |
|----------|--------------|-------------|
| `from_data_uri(uri)` | `data:...` | Decode base64 or URL-encoded |
| `from_varchar_uri(uri)` | `data+varchar:...` | Extract raw content |
| `from_blob_uri(uri)` | `data+blob:...` | Decode escape sequences |
| `from_scalarfs_uri(uri)` | Any scalarfs URI | Auto-detect and decode |

## from_data_uri()

Decodes a base64 or URL-encoded data URI.

### Syntax

```sql
from_data_uri(uri VARCHAR) → VARCHAR
```

### Examples

```sql
-- Base64 decoding
SELECT from_data_uri('data:;base64,aGVsbG8=');
-- hello

-- URL decoding
SELECT from_data_uri('data:,hello%20world');
-- hello world

-- With media type (ignored)
SELECT from_data_uri('data:text/plain;base64,aGVsbG8=');
-- hello

-- Empty content
SELECT length(from_data_uri('data:;base64,'));
-- 0
```

### Error Handling

```sql
-- Missing data: prefix
SELECT from_data_uri('not a data uri');
-- Error: Invalid data: URI - must start with 'data:'

-- Missing comma
SELECT from_data_uri('data:no-comma');
-- Error: Invalid data: URI - missing comma separator
```

## from_varchar_uri()

Extracts content from a raw varchar URI (no decoding needed).

### Syntax

```sql
from_varchar_uri(uri VARCHAR) → VARCHAR
```

### Examples

```sql
SELECT from_varchar_uri('data+varchar:hello');
-- hello

SELECT from_varchar_uri('data+varchar:{"key": "value"}');
-- {"key": "value"}

SELECT from_varchar_uri('data+varchar:');
-- (empty string)

SELECT from_varchar_uri('data+varchar:line1
line2');
-- line1
-- line2
```

### Error Handling

```sql
SELECT from_varchar_uri('data:;base64,xyz');
-- Error: Invalid data+varchar: URI - must start with 'data+varchar:'

SELECT from_varchar_uri('hello');
-- Error: Invalid data+varchar: URI - must start with 'data+varchar:'
```

## from_blob_uri()

Decodes escape sequences in a blob URI.

### Syntax

```sql
from_blob_uri(uri VARCHAR) → VARCHAR
```

### Escape Sequences Decoded

| Escape | Result |
|--------|--------|
| `\\` | `\` |
| `\n` | Newline (0x0A) |
| `\r` | Carriage return (0x0D) |
| `\t` | Tab (0x09) |
| `\0` | Null byte (0x00) |
| `\xNN` | Byte with hex value NN |

### Examples

```sql
SELECT from_blob_uri('data+blob:hello');
-- hello

SELECT from_blob_uri('data+blob:path\\to\\file');
-- path\to\file

SELECT length(from_blob_uri('data+blob:line1\nline2'));
-- 11 (line1 + newline + line2)

SELECT from_blob_uri('data+blob:col1\tcol2');
-- col1	col2

SELECT length(from_blob_uri('data+blob:\0'));
-- 1 (single null byte)

SELECT from_blob_uri('data+blob:\x41\x42\x43');
-- ABC
```

### Error Handling

```sql
-- Wrong prefix
SELECT from_blob_uri('data+varchar:test');
-- Error: Invalid data+blob: URI - must start with 'data+blob:'

-- Incomplete hex escape
SELECT from_blob_uri('data+blob:\x');
-- Error: Invalid escape sequence - incomplete \x at position 0

SELECT from_blob_uri('data+blob:\x4');
-- Error: Invalid escape sequence - incomplete \x at position 0

-- Invalid hex digits
SELECT from_blob_uri('data+blob:\xGG');
-- Error: Invalid escape sequence '\xGG' at position 0

-- Unknown escape sequence
SELECT from_blob_uri('data+blob:\q');
-- Error: Invalid escape sequence '\q' at position 0
```

## from_scalarfs_uri()

Automatically detects the URI type and decodes appropriately.

### Syntax

```sql
from_scalarfs_uri(uri VARCHAR) → VARCHAR
```

### Examples

```sql
-- Detects data:;base64,
SELECT from_scalarfs_uri('data:;base64,aGVsbG8=');
-- hello

-- Detects data+varchar:
SELECT from_scalarfs_uri('data+varchar:hello');
-- hello

-- Detects data+blob:
SELECT from_scalarfs_uri('data+blob:hello');
-- hello

-- Handles all formats uniformly
SELECT from_scalarfs_uri(to_scalarfs_uri('test'));
-- test
```

### Error Handling

```sql
SELECT from_scalarfs_uri('invalid');
-- Error: Invalid scalarfs URI - must start with 'data:', 'data+varchar:', or 'data+blob:'
```

## Practical Examples

### Decode URIs from a Table

```sql
CREATE TABLE cached_data(key VARCHAR, uri VARCHAR);
INSERT INTO cached_data VALUES
  ('config', 'data+varchar:{"debug": true}'),
  ('users', 'data:;base64,W3siaWQiOjF9XQ==');

SELECT key, from_scalarfs_uri(uri) AS content
FROM cached_data;
```

### Validate Round-Trip

```sql
WITH test_data AS (
  SELECT 'simple text' AS content
  UNION ALL SELECT '{"json": true}'
  UNION ALL SELECT 'line1' || chr(10) || 'line2'
)
SELECT
  content,
  from_scalarfs_uri(to_scalarfs_uri(content)) = content AS round_trip_ok
FROM test_data;
```

### Extract and Parse

```sql
-- Decode URI and parse as JSON
SELECT parsed.*
FROM (SELECT 'data+varchar:{"a": 1, "b": 2}' AS uri) t,
     read_json(uri) AS parsed;
```

## See Also

- [Encoding Functions](encoding.md) — Convert content to URIs
- [Protocol Overview](../protocols/overview.md) — Understanding the different protocols
