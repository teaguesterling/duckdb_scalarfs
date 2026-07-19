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

### pathmacro:

Resolve to real file paths by calling an allow-listed scalar macro.

| Property | Value |
|----------|-------|
| **Syntax** | `pathmacro:<macro>[?key=value&...]` |
| **Mode** | Read only |
| **Glob Support** | Yes (macro may return globs/other protocols, re-dispatched) |

The query string is passed to the macro as a `MAP(VARCHAR, VARCHAR)`; the macro must return `VARCHAR[]` (a list of paths). Requires opt-in via the `allowed_pathmacros` setting.

```sql
CREATE MACRO region_files(params) AS (
  SELECT list(file_path) FROM catalog WHERE region = params['region']
);
SET allowed_pathmacros = 'region_files';
SELECT * FROM read_csv('pathmacro:region_files?region=east');
```

See [pathmacro: Protocol](../protocols/pathmacro.md) for the full contract and security model.

---

## Settings

### allowed_pathmacros

| Property | Value |
|----------|-------|
| **Type** | VARCHAR |
| **Default** | `''` (empty — no macros allowed) |
| **Scope** | Session |

Comma-separated list of scalar-macro names the `pathmacro:` protocol may invoke. `pathmacro:` is inert until this is set.

```sql
SET allowed_pathmacros = 'region_files, sample_files';
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

## pathmacro: URL Functions

### to_pathmacro_url

Build a `pathmacro:` URL, URL-encoding each key and value.

```sql
to_pathmacro_url(macro VARCHAR) → VARCHAR
to_pathmacro_url(macro VARCHAR, params STRUCT|MAP) → VARCHAR
```

`params` may be a `STRUCT` (`{region: 'west', year: 2024}`) or a `MAP(VARCHAR, VARCHAR)`; non-text values are cast to text. The macro name is validated as a plain SQL identifier. NULL params (or none) yield the bare URL.

| Input | Output |
|-------|--------|
| `to_pathmacro_url('r', {region:'west', year:2024})` | `'pathmacro:r?region=west&year=2024'` |
| `to_pathmacro_url('m', {a:'x y', b:'1&2'})` | `'pathmacro:m?a=x%20y&b=1%262'` |
| `to_pathmacro_url('all')` | `'pathmacro:all'` |

**Errors:**

- `to_pathmacro_url: '<name>' is not a valid macro name (must be a plain SQL identifier)`
- `to_pathmacro_url: params must be a STRUCT (...) or MAP(VARCHAR, VARCHAR), got <type>`

### from_pathmacro_url

Parse a `pathmacro:` URL into its macro and decoded params.

```sql
from_pathmacro_url(url VARCHAR) → STRUCT(macro VARCHAR, params MAP(VARCHAR, VARCHAR))
```

| Input | Output |
|-------|--------|
| `from_pathmacro_url('pathmacro:r?region=west')` | `{'macro': r, 'params': {region=west}}` |

**Errors:**

- `from_pathmacro_url: '<url>' is not a pathmacro: URL (must start with 'pathmacro:')`

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
| `Invalid escape sequence: '\xNN' is not valid hex` | Invalid hex digits after `\x` |
| `Invalid escape sequence: incomplete \x` | `\x` without 2 hex digits |
| `Invalid escape sequence '\c'` | Unknown escape character |

### pathmacro: Errors

| Error | Cause |
|-------|-------|
| `macro 'X' is not in allowed_pathmacros` | Macro not opted in via the setting |
| `invalid macro name 'X'` | Macro name is not a plain SQL identifier |
| `macro 'X' must return VARCHAR[]` | Macro returned a non-list-of-varchar value |
| `catalog macro 'X' failed: ...` | The macro itself raised (e.g. missing table) |
