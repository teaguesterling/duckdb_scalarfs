# data+blob: Protocol

The `data+blob:` protocol allows inline content with escape sequences for control characters and binary bytes. It's ideal for mostly-text content that needs occasional special characters.

## Syntax

```
data+blob:<escaped_content>
```

Content after `data+blob:` is processed for escape sequences.

## Escape Sequences

| Sequence | Byte | Description |
|----------|------|-------------|
| `\\` | `0x5C` | Literal backslash |
| `\n` | `0x0A` | Newline (LF) |
| `\r` | `0x0D` | Carriage return (CR) |
| `\t` | `0x09` | Tab |
| `\0` | `0x00` | Null byte |
| `\xNN` | `0xNN` | Any byte in hexadecimal |

All other characters pass through unchanged.

## Basic Usage

```sql
-- Text with newlines
SELECT content FROM read_text('data+blob:line1\nline2\nline3');
```

Output:
```
line1
line2
line3
```

### Tab-Separated Content

```sql
SELECT content FROM read_text('data+blob:col1\tcol2\tcol3\nval1\tval2\tval3');
```

### Windows Line Endings (CRLF)

```sql
SELECT content FROM read_text('data+blob:line1\r\nline2\r\nline3');
```

## Hex Escapes

Use `\xNN` for any byte value:

```sql
-- ASCII characters via hex
SELECT content FROM read_text('data+blob:\x48\x65\x6C\x6C\x6F');
-- Returns: Hello

-- Control characters
SELECT content FROM read_text('data+blob:bell:\x07 backspace:\x08');

-- High bytes
SELECT content FROM read_text('data+blob:\xFF\xFE');
```

## Null Bytes

Unlike `data+varchar:`, `data+blob:` can include null bytes:

```sql
-- Content with embedded null
SELECT length(content) FROM read_blob('data+blob:before\0after');
-- Returns: 12 (before + null + after)

-- Multiple nulls
SELECT length(content) FROM read_blob('data+blob:\0\0\0');
-- Returns: 3
```

## Literal Backslashes

To include a literal backslash, escape it:

```sql
-- Windows path
SELECT content FROM read_text('data+blob:C:\\Users\\Alice\\file.txt');
-- Returns: C:\Users\Alice\file.txt

-- Regex pattern
SELECT content FROM read_text('data+blob:\\d+\\.\\d+');
-- Returns: \d+\.\d+
```

## Practical Examples

### CSV with Embedded Newlines

```sql
-- Note: the \n inside quotes is part of the data
SELECT * FROM read_csv('data+blob:name,bio
Alice,"Software engineer.\nLoves DuckDB."
Bob,"Data scientist.\nPython enthusiast."', quote='"');
```

### JSON with Special Characters

```sql
SELECT * FROM read_json('data+blob:{"message": "Hello\\nWorld", "tab": "col1\\tcol2"}');
```

### Log File Format

```sql
SELECT content FROM read_text('data+blob:2024-01-15 10:30:00\tINFO\tStarting service\n2024-01-15 10:30:01\tINFO\tReady');
```

## Overhead Analysis

`data+blob:` overhead depends on content:

| Content Type | Overhead |
|--------------|----------|
| Plain ASCII text | ~0% (just prefix) |
| Text with newlines | 1 extra byte per `\n` |
| Text with backslashes | 1 extra byte per `\` |
| Binary heavy | Consider base64 instead |

### Encoding Decision

```sql
-- Few escapes needed: data+blob is efficient
-- "hello\nworld" → "data+blob:hello\nworld" (21 bytes)

-- Many escapes needed: base64 might be better
-- Binary data with 50% special chars → use data:;base64,
```

## Comparison with Other Protocols

| Feature | `data+blob:` | `data+varchar:` | `data:;base64,` |
|---------|--------------|-----------------|-----------------|
| Null bytes | ✅ `\0` | ❌ | ✅ |
| Newlines | ✅ `\n` | ✅ literal | ✅ |
| Readability | Good | Excellent | Poor |
| Binary data | Partial | ❌ | ✅ |
| Overhead | Variable | None | 33% |

## Error Handling

### Invalid Hex Escape

```sql
SELECT * FROM read_text('data+blob:\xGG');
-- Error: Invalid escape sequence '\xGG' at position 0

SELECT * FROM read_text('data+blob:\x');
-- Error: Invalid escape sequence - incomplete \x at position 0
```

### Unknown Escape

```sql
SELECT * FROM read_text('data+blob:\q');
-- Error: Invalid escape sequence '\q' at position 0
```

## Generating data+blob: URIs

### In SQL

```sql
-- Manual encoding
SELECT to_blob_uri('line1' || chr(10) || 'line2');
-- Returns: data+blob:line1\nline2

-- With tabs
SELECT to_blob_uri('col1' || chr(9) || 'col2');
-- Returns: data+blob:col1\tcol2

-- Auto-selection uses blob when appropriate
SELECT to_scalarfs_uri('text with ' || chr(7) || ' bell');
-- Returns: data+blob:text with \x07 bell
```

### In Python

```python
def to_blob_uri(content: bytes) -> str:
    result = 'data+blob:'
    for b in content:
        if b == ord('\\'):
            result += '\\\\'
        elif b == ord('\n'):
            result += '\\n'
        elif b == ord('\r'):
            result += '\\r'
        elif b == ord('\t'):
            result += '\\t'
        elif b == 0:
            result += '\\0'
        elif b < 0x20 or b == 0x7F:
            result += f'\\x{b:02X}'
        else:
            result += chr(b)
    return result
```

## See Also

- [data+varchar: Protocol](data-varchar.md) — When you don't need escapes
- [data: Protocol](data-uri.md) — For binary-heavy content
- [Encoding Functions](../functions/encoding.md) — Generate URIs programmatically
