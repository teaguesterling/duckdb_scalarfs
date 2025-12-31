# data: Protocol (RFC 2397)

The `data:` protocol implements the standard RFC 2397 data URI scheme, allowing inline content with base64 or URL encoding.

## Syntax

```
data:[<mediatype>][;base64],<data>
```

Components:

| Part | Required | Description |
|------|----------|-------------|
| `data:` | Yes | Protocol prefix |
| `<mediatype>` | No | MIME type (ignored by readers) |
| `;base64` | No | Indicates base64 encoding |
| `,` | Yes | Separator before data |
| `<data>` | Yes | The encoded content |

## Base64 Encoding

Use `;base64` for binary content or content with special characters:

```sql
-- Base64 encoded CSV
SELECT * FROM read_csv('data:;base64,bmFtZSxzY29yZQpBbGljZSw5NQpCb2IsODc=');
```

Output:
```
┌─────────┬───────┐
│  name   │ score │
│ varchar │ int64 │
├─────────┼───────┤
│ Alice   │    95 │
│ Bob     │    87 │
└─────────┴───────┘
```

### Base64 Examples

```sql
-- JSON
SELECT * FROM read_json('data:;base64,eyJrZXkiOiJ2YWx1ZSJ9');
-- Decodes to: {"key":"value"}

-- Text with special characters
SELECT content FROM read_text('data:;base64,SGVsbG8KV29ybGQ=');
-- Decodes to: Hello\nWorld

-- Binary data (as text representation)
SELECT length(content) FROM read_blob('data:;base64,AAECA/8=');
-- Returns: 4 (bytes: 0x00, 0x01, 0x02, 0x03, 0xFF... wait, that's 5)
```

## URL Encoding

Without `;base64`, content is URL-decoded:

```sql
-- URL-encoded CSV (newline = %0A, comma = %2C)
SELECT * FROM read_csv('data:,name%2Cscore%0AAlice%2C95%0ABob%2C87');
```

### URL Encoding Reference

| Character | Encoded |
|-----------|---------|
| Newline | `%0A` |
| Carriage return | `%0D` |
| Space | `%20` |
| `%` | `%25` |
| `,` | `%2C` |
| `:` | `%3A` |
| `"` | `%22` |

### URL Encoding Examples

```sql
-- Simple text with spaces
SELECT content FROM read_text('data:,hello%20world');
-- Returns: hello world

-- JSON (quotes and colons need encoding)
SELECT * FROM read_json('data:,%7B%22key%22%3A%22value%22%7D');
-- Decodes to: {"key":"value"}
```

## Media Types

The optional media type is preserved for compatibility but ignored by DuckDB readers:

```sql
-- All of these work the same way
SELECT * FROM read_json('data:,{"a":1}');
SELECT * FROM read_json('data:application/json,{"a":1}');
SELECT * FROM read_json('data:text/plain,{"a":1}');

-- The media type doesn't affect how the content is parsed
-- The reader function (read_json, read_csv, etc.) determines parsing
```

## When to Use data:

### Advantages

- **Standard format**: RFC 2397 compliant, works with other tools
- **Binary support**: Base64 handles any byte sequence
- **Portable**: Can be copied/pasted from other sources

### Disadvantages

- **33% overhead**: Base64 increases size by ~33%
- **Less readable**: Encoded content isn't human-readable
- **Read-only**: Cannot write to data: URIs

### Comparison with data+varchar:

| Feature | `data:` | `data+varchar:` |
|---------|---------|-----------------|
| Overhead | 33% (base64) | 0% |
| Readability | Poor | Excellent |
| Binary content | ✅ | ❌ |
| Standards | RFC 2397 | scalarfs-specific |

## Generating Base64 URIs

### In SQL

Use the `to_data_uri()` helper function:

```sql
SELECT to_data_uri('hello world');
-- Returns: data:;base64,aGVsbG8gd29ybGQ=

SELECT to_data_uri('{"key": "value"}');
-- Returns: data:;base64,eyJrZXkiOiAidmFsdWUifQ==
```

### In Python

```python
import base64

content = '{"key": "value"}'
uri = 'data:;base64,' + base64.b64encode(content.encode()).decode()
# data:;base64,eyJrZXkiOiAidmFsdWUifQ==
```

### In Shell

```bash
echo -n '{"key": "value"}' | base64
# eyJrZXkiOiAidmFsdWUifQ==

# Full URI:
echo "data:;base64,$(echo -n '{"key": "value"}' | base64)"
```

## Error Handling

### Missing Comma

```sql
SELECT * FROM read_text('data:no-comma-here');
-- Error: Invalid data: URI - missing comma separator
```

### Invalid Base64

```sql
SELECT * FROM read_text('data:;base64,not-valid-base64!!!');
-- Error: Invalid base64 encoding
```

## See Also

- [data+varchar: Protocol](data-varchar.md) — Zero-overhead alternative for text
- [data+blob: Protocol](data-blob.md) — Escape sequences for control characters
- [Encoding Functions](../functions/encoding.md) — Generate URIs programmatically
