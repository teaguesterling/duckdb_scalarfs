# Protocol Overview

scalarfs provides five protocols for accessing in-memory content as files. Each protocol is suited for different use cases.

## Protocol Summary

| Protocol | Syntax | Mode | Best For |
|----------|--------|------|----------|
| `variable:` | `variable:name` | Read/Write | Persistent storage within a session |
| `pathvariable:` | `pathvariable:name` | Read/Write | Dynamic file paths stored in variables |
| `data:` | `data:[mediatype][;base64],content` | Read | Standards-compliant data URIs |
| `data+varchar:` | `data+varchar:content` | Read | Zero-overhead inline text |
| `data+blob:` | `data+blob:escaped_content` | Read | Text with control characters |

## Choosing a Protocol

### Use `variable:` when you need to:

- Store data across multiple queries
- Write output from COPY commands
- Use glob patterns to match multiple sources
- Keep data in a session variable

```sql
SET VARIABLE data = '...';
SELECT * FROM read_json('variable:data');
COPY results TO 'variable:output' (FORMAT json);
```

### Use `pathvariable:` when you need to:

- Access files at paths stored in variables
- Switch between different file locations dynamically
- Combine variable name globs with file path globs

```sql
SET VARIABLE input_path = '/data/reports/2024/*.csv';
SELECT * FROM read_csv('pathvariable:input_path');
COPY results TO 'pathvariable:output_path' (FORMAT json);
```

### Use `data+varchar:` when you need to:

- Embed small amounts of text inline
- Avoid any encoding overhead
- Work with simple content (no null bytes)

```sql
SELECT * FROM read_json('data+varchar:{"key": "value"}');
```

### Use `data:` when you need to:

- Use a standards-compliant format
- Handle binary content with base64
- Work with URL-encoded content

```sql
SELECT * FROM read_csv('data:;base64,bmFtZSxzY29yZQpBbGljZSw5NQ==');
```

### Use `data+blob:` when you need to:

- Include control characters (newlines, tabs) in a readable way
- Handle mostly-text content with occasional special bytes
- Keep the content human-readable

```sql
SELECT * FROM read_text('data+blob:line1\nline2\ttabbed');
```

## Protocol Comparison

### Encoding Overhead

| Protocol | Overhead | Example |
|----------|----------|---------|
| `variable:` | None (stored separately) | N/A |
| `pathvariable:` | None (path in variable) | N/A |
| `data+varchar:` | 13 bytes prefix | `data+varchar:` |
| `data+blob:` | 10 bytes + escapes | `data+blob:` + `\n` per newline |
| `data:;base64,` | 13 bytes + 33% | `data:;base64,` + base64 encoding |

### Content Restrictions

| Protocol | Null Bytes | Control Chars | Binary |
|----------|------------|---------------|--------|
| `variable:` | ✅ (as BLOB) | ✅ | ✅ |
| `pathvariable:` | Depends on target filesystem | Depends on target filesystem | Depends on target filesystem |
| `data+varchar:` | ❌ | ✅ | ❌ |
| `data+blob:` | ✅ (escaped) | ✅ (escaped) | Partial |
| `data:;base64,` | ✅ | ✅ | ✅ |

### Write Support

| Protocol | Can Write |
|----------|-----------|
| `variable:` | ✅ Yes |
| `pathvariable:` | ✅ Yes |
| `data+varchar:` | ❌ No |
| `data+blob:` | ❌ No |
| `data:` | ❌ No |

## Pattern Matching

The `variable:` and `pathvariable:` protocols support glob pattern matching:

```sql
-- Works: variable protocol with glob
SELECT * FROM read_json('variable:config_*');

-- Works: pathvariable with two-level globs
SET VARIABLE input_2024 = '/data/2024/*.csv';
SET VARIABLE input_2023 = '/data/2023/*.csv';
SELECT * FROM read_csv('pathvariable:input_*');  -- Matches both variables, expands both paths

-- Doesn't work: data protocols don't support globs
SELECT * FROM read_json('data+varchar:*');  -- Literal asterisk, not a glob
```

See [Variable Protocol](variable.md) and [PathVariable Protocol](pathvariable.md) for full glob documentation.

## Next Steps

- [variable: Protocol](variable.md) — Read/write variables with glob support
- [pathvariable: Protocol](pathvariable.md) — Dynamic file paths stored in variables
- [data: Protocol](data-uri.md) — RFC 2397 data URIs
- [data+varchar: Protocol](data-varchar.md) — Zero-overhead inline content
- [data+blob: Protocol](data-blob.md) — Escaped binary content
