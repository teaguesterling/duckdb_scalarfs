# Encoding Functions

scalarfs provides functions to convert content into URI format programmatically.

## Function Summary

| Function | Output Format | Use Case |
|----------|---------------|----------|
| `to_data_uri(content)` | `data:;base64,...` | Binary content, standards compliance |
| `to_varchar_uri(content)` | `data+varchar:...` | Text content, zero overhead |
| `to_blob_uri(content)` | `data+blob:...` | Text with control characters |
| `to_scalarfs_uri(content)` | Auto-selected | Let the system choose |

## to_data_uri()

Encodes content as a base64 data URI.

### Syntax

```sql
to_data_uri(content VARCHAR) → VARCHAR
```

### Examples

```sql
SELECT to_data_uri('hello');
-- data:;base64,aGVsbG8=

SELECT to_data_uri('{"key": "value"}');
-- data:;base64,eyJrZXkiOiAidmFsdWUifQ==

SELECT to_data_uri('');
-- data:;base64,
```

### Use Cases

- Binary content that needs encoding
- Standards-compliant data URIs
- Content with many special characters

## to_varchar_uri()

Creates a raw varchar URI with no encoding.

### Syntax

```sql
to_varchar_uri(content VARCHAR) → VARCHAR
```

### Examples

```sql
SELECT to_varchar_uri('hello');
-- data+varchar:hello

SELECT to_varchar_uri('{"key": "value"}');
-- data+varchar:{"key": "value"}

SELECT to_varchar_uri('');
-- data+varchar:
```

### Use Cases

- Text content with no special encoding needs
- Maximum efficiency (zero overhead)
- Human-readable URIs

## to_blob_uri()

Encodes content with escape sequences for control characters.

### Syntax

```sql
to_blob_uri(content VARCHAR) → VARCHAR
```

### Escape Sequences Applied

| Character | Escaped As |
|-----------|------------|
| Backslash (`\`) | `\\` |
| Newline | `\n` |
| Carriage return | `\r` |
| Tab | `\t` |
| Null byte | `\0` |
| Other control chars | `\xNN` |

### Examples

```sql
SELECT to_blob_uri('hello');
-- data+blob:hello

SELECT to_blob_uri('path\to\file');
-- data+blob:path\\to\\file

SELECT to_blob_uri('line1
line2');
-- data+blob:line1\nline2

SELECT to_blob_uri('col1	col2');  -- tab character
-- data+blob:col1\tcol2

SELECT to_blob_uri(chr(0));
-- data+blob:\0

SELECT to_blob_uri(chr(7));  -- bell character
-- data+blob:\x07
```

### Use Cases

- Text with occasional control characters
- Content that should remain mostly readable
- Avoiding base64 overhead when possible

## to_scalarfs_uri()

Automatically selects the optimal encoding based on content analysis.

### Syntax

```sql
to_scalarfs_uri(content VARCHAR) → VARCHAR
```

### Selection Logic

1. **Safe for varchar?** (printable ASCII/UTF-8 + whitespace)
   → Returns `data+varchar:...`

2. **Few escapes needed?** (< 10% of content needs escaping)
   → Returns `data+blob:...`

3. **Otherwise**
   → Returns `data:;base64,...`

### Examples

```sql
-- Simple text → data+varchar:
SELECT to_scalarfs_uri('hello world');
-- data+varchar:hello world

-- Text with newlines → data+varchar: (newlines are safe)
SELECT to_scalarfs_uri('line1
line2');
-- data+varchar:line1\nline2

-- Text with one control char → data+blob:
SELECT to_scalarfs_uri('this is a longer string with one' || chr(7) || ' control char');
-- data+blob:this is a longer string with one\x07 control char

-- Binary heavy → data:;base64,
SELECT to_scalarfs_uri(chr(0) || chr(1) || chr(2) || chr(3) || chr(4));
-- data:;base64,AAECAwQ=
```

### Use Cases

- When you don't want to think about encoding
- Automated URI generation
- Optimal storage efficiency

## Practical Examples

### Store Query Results as URI

```sql
-- Get query result as a URI for later use
SELECT to_scalarfs_uri(
  (SELECT string_agg(name || ',' || score, chr(10))
   FROM (VALUES ('Alice', 95), ('Bob', 87)) AS t(name, score))
);
```

### Generate URIs for a Column

```sql
CREATE TABLE documents(id INT, content VARCHAR);
INSERT INTO documents VALUES
  (1, '{"type": "note"}'),
  (2, 'Plain text'),
  (3, 'Text with
newline');

SELECT id, to_scalarfs_uri(content) AS uri
FROM documents;
```

### Round-Trip Test

```sql
-- Encode and decode
SELECT from_scalarfs_uri(to_scalarfs_uri('test content')) = 'test content';
-- true
```

## See Also

- [Decoding Functions](decoding.md) — Convert URIs back to content
- [Protocol Overview](../protocols/overview.md) — Understanding the different protocols
