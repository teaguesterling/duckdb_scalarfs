# API Reference

Complete reference for all scalarfs protocols and functions.

## Protocols

### variable:

Read from or write to DuckDB session variables.

| Property | Value |
|----------|-------|
| **Syntax** | `variable:<name>` |
| **Mode** | Read/Write |
| **Glob Support** | Yes (`*`, `?`) |

#### Reading

```sql
SET VARIABLE my_var = 'content';
SELECT * FROM read_text('variable:my_var');
```

#### Writing

```sql
COPY my_table TO 'variable:output' (FORMAT csv);
```

#### Glob Patterns

| Pattern | Matches |
|---------|---------|
| `*` | Any sequence of characters |
| `?` | Any single character |

```sql
SELECT * FROM read_json('variable:config_*');
```

---

### data:

RFC 2397 data URIs with base64 or URL encoding.

| Property | Value |
|----------|-------|
| **Syntax** | `data:[<mediatype>][;base64],<data>` |
| **Mode** | Read only |
| **Glob Support** | No |

#### Base64

```sql
SELECT * FROM read_text('data:;base64,aGVsbG8=');
```

#### URL-encoded

```sql
SELECT * FROM read_text('data:,hello%20world');
```

---

### data+varchar:

Raw inline content with no encoding.

| Property | Value |
|----------|-------|
| **Syntax** | `data+varchar:<content>` |
| **Mode** | Read only |
| **Glob Support** | No |
| **Restrictions** | No null bytes |

```sql
SELECT * FROM read_json('data+varchar:{"key": "value"}');
```

---

### data+blob:

Content with escape sequences for control characters.

| Property | Value |
|----------|-------|
| **Syntax** | `data+blob:<escaped_content>` |
| **Mode** | Read only |
| **Glob Support** | No |

#### Escape Sequences

| Sequence | Byte | Description |
|----------|------|-------------|
| `\\` | `0x5C` | Backslash |
| `\n` | `0x0A` | Newline |
| `\r` | `0x0D` | Carriage return |
| `\t` | `0x09` | Tab |
| `\0` | `0x00` | Null |
| `\xNN` | `0xNN` | Hex byte |

```sql
SELECT * FROM read_text('data+blob:line1\nline2');
```

---

## Encoding Functions

### to_data_uri

Encode content as base64 data URI.

```sql
to_data_uri(content VARCHAR) → VARCHAR
```

| Input | Output |
|-------|--------|
| `'hello'` | `'data:;base64,aGVsbG8='` |
| `''` | `'data:;base64,'` |

---

### to_varchar_uri

Create raw varchar URI (no encoding).

```sql
to_varchar_uri(content VARCHAR) → VARCHAR
```

| Input | Output |
|-------|--------|
| `'hello'` | `'data+varchar:hello'` |
| `'{"a":1}'` | `'data+varchar:{"a":1}'` |

---

### to_blob_uri

Create escaped blob URI.

```sql
to_blob_uri(content VARCHAR) → VARCHAR
```

| Input | Output |
|-------|--------|
| `'hello'` | `'data+blob:hello'` |
| `'a\nb'` (with newline) | `'data+blob:a\nb'` |
| `'a\tb'` (with tab) | `'data+blob:a\tb'` |
| `chr(0)` | `'data+blob:\0'` |

---

### to_scalarfs_uri

Auto-select optimal encoding.

```sql
to_scalarfs_uri(content VARCHAR) → VARCHAR
```

**Selection Logic:**

1. If safe for varchar → `data+varchar:`
2. If <10% needs escaping → `data+blob:`
3. Otherwise → `data:;base64,`

| Input | Output | Reason |
|-------|--------|--------|
| `'hello'` | `'data+varchar:hello'` | Safe text |
| `'a' \|\| chr(7) \|\| 'b'` | `'data+blob:a\x07b'` | Few escapes |
| `chr(0) \|\| chr(1)` | `'data:;base64,...'` | Binary |

---

## Decoding Functions

### from_data_uri

Decode base64 or URL-encoded data URI.

```sql
from_data_uri(uri VARCHAR) → VARCHAR
```

| Input | Output |
|-------|--------|
| `'data:;base64,aGVsbG8='` | `'hello'` |
| `'data:,hello%20world'` | `'hello world'` |

**Errors:**

- `Invalid data: URI - must start with 'data:'`
- `Invalid data: URI - missing comma separator`

---

### from_varchar_uri

Extract content from varchar URI.

```sql
from_varchar_uri(uri VARCHAR) → VARCHAR
```

| Input | Output |
|-------|--------|
| `'data+varchar:hello'` | `'hello'` |
| `'data+varchar:'` | `''` |

**Errors:**

- `Invalid data+varchar: URI - must start with 'data+varchar:'`

---

### from_blob_uri

Decode escape sequences in blob URI.

```sql
from_blob_uri(uri VARCHAR) → VARCHAR
```

| Input | Output |
|-------|--------|
| `'data+blob:hello'` | `'hello'` |
| `'data+blob:a\\b'` | `'a\b'` |
| `'data+blob:a\nb'` | `'a<newline>b'` |
| `'data+blob:\x41'` | `'A'` |

**Errors:**

- `Invalid data+blob: URI - must start with 'data+blob:'`
- `Invalid escape sequence '\xNN' at position N`
- `Invalid escape sequence - incomplete \x at position N`
- `Invalid escape sequence '\c' at position N`

---

### from_scalarfs_uri

Auto-detect and decode any scalarfs URI.

```sql
from_scalarfs_uri(uri VARCHAR) → VARCHAR
```

Handles: `data:`, `data+varchar:`, `data+blob:`

| Input | Output |
|-------|--------|
| `'data:;base64,aGVsbG8='` | `'hello'` |
| `'data+varchar:hello'` | `'hello'` |
| `'data+blob:hello'` | `'hello'` |

**Errors:**

- `Invalid scalarfs URI - must start with 'data:', 'data+varchar:', or 'data+blob:'`

---

## Error Messages

### Variable Protocol Errors

| Error | Cause |
|-------|-------|
| `Variable 'X' not found` | Variable doesn't exist |
| `Variable 'X' is NULL` | Variable is NULL |
| `No files found that match the pattern "variable:X"` | Glob matched nothing |

### Data URI Errors

| Error | Cause |
|-------|-------|
| `Invalid data: URI - must start with 'data:'` | Wrong prefix |
| `Invalid data: URI - missing comma separator` | No comma in URI |
| `Invalid base64 encoding` | Malformed base64 |

### Blob URI Errors

| Error | Cause |
|-------|-------|
| `Invalid escape sequence '\xNN'` | Invalid hex digits |
| `Invalid escape sequence - incomplete \x` | `\x` without 2 hex digits |
| `Invalid escape sequence '\c'` | Unknown escape character |
