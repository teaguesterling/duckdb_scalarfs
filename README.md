# scalarfs — DuckDB Scalar Filesystem Extension

A DuckDB extension that enables reader/writer functions to work with in-memory content: variables and inline literals.

## Overview

DuckDB's file functions (`read_csv`, `read_json`, `COPY TO`, etc.) expect file paths. **scalarfs** bridges the gap when your content is already in memory, allowing the same functions to work with:

- **Variables** — Store data in DuckDB variables and read/write them as files
- **Inline literals** — Embed content directly in your queries without temporary files

| Protocol | Purpose | Mode |
|----------|---------|------|
| `variable:` | DuckDB variable as file | Read/Write |
| `pathvariable:` | File path stored in variable | Read/Write |
| `data:` | RFC 2397 data URI (base64/url-encoded) | Read |
| `data+varchar:` | Raw VARCHAR content as file | Read |
| `data+blob:` | Escaped BLOB content as file | Read |
| `decompress+gz:` | Gzip decompression wrapper | Read |
| `decompress+zstd:` | Zstd decompression wrapper | Read |
| `pathmacro:` | Paths resolved by an allow-listed scalar macro | Read/Write |

## Quick Start

```sql
INSTALL scalarfs FROM community;
LOAD scalarfs;

-- Read JSON from a variable
SET VARIABLE config = '{"debug": true, "port": 8080}';
SELECT * FROM read_json('variable:config');

-- Read CSV from inline content
SELECT * FROM read_csv('data+varchar:name,score
Alice,95
Bob,87');

-- Write query results to a variable
COPY (SELECT * FROM my_table WHERE active) TO 'variable:exported' (FORMAT json);
SELECT getvariable('exported');
```

## Installation

### From Community
```sql
INSTALL scalarfs FROM community;
LOAD scalarfs;
```

### From Source

```bash
git clone --recurse-submodules https://github.com/your-repo/duckdb_scalarfs
cd duckdb_scalarfs
make
```

Then in DuckDB:
```sql
LOAD 'build/release/extension/scalarfs/scalarfs.duckdb_extension';
```

## Protocols

### `variable:` — Variable as File

Read from or write to a DuckDB variable as if it were a file.

#### Reading Variables

```sql
-- Store CSV data in a variable
SET VARIABLE my_data = 'name,value
Alice,100
Bob,200
Carol,150';

-- Read it with any file reader
SELECT * FROM read_csv('variable:my_data');
-- ┌─────────┬───────┐
-- │  name   │ value │
-- │ varchar │ int64 │
-- ├─────────┼───────┤
-- │ Alice   │   100 │
-- │ Bob     │   200 │
-- │ Carol   │   150 │
-- └─────────┴───────┘

-- Works with JSON too
SET VARIABLE users = '[{"id": 1, "name": "Alice"}, {"id": 2, "name": "Bob"}]';
SELECT * FROM read_json('variable:users');
```

#### Writing to Variables

```sql
-- Export query results to a variable
CREATE TABLE sales(product VARCHAR, amount INT);
INSERT INTO sales VALUES ('Widget', 100), ('Gadget', 200);

COPY sales TO 'variable:sales_csv' (FORMAT csv, HEADER true);
SELECT getvariable('sales_csv');
-- product,amount
-- Widget,100
-- Gadget,200

-- Export as JSON
COPY (SELECT * FROM sales WHERE amount > 150) TO 'variable:big_sales' (FORMAT json);
SELECT getvariable('big_sales');
-- [{"product":"Gadget","amount":200}]
```

#### Writing Native Values with FORMAT variable

Store query results as native DuckDB values (not serialized text):

```sql
-- Store a list of paths for use with pathvariable:
COPY (SELECT path FROM file_index WHERE active) TO 'variable:paths' (FORMAT variable);
-- paths is now a VARCHAR[], not serialized text

-- Use with pathvariable: to read all files
SELECT * FROM read_csv('pathvariable:paths');

-- Store a struct
COPY (SELECT 1 AS id, 'Alice' AS name) TO 'variable:person' (FORMAT variable);
SELECT getvariable('person').name;  -- Alice
```

**LIST modes** control how results are converted:

| Mode | Behavior |
|------|----------|
| `auto` (default) | Smart: 1×1→scalar, N×1→list, 1×N→struct, N×N→list of structs |
| `rows` | Always list of structs, even for single row |
| `none` | Single value only (error if >1 row) |
| `scalar` | Single column only (error if >1 column) |

```sql
-- Force list of structs even for one row
COPY (SELECT 1 AS id) TO 'variable:x' (FORMAT variable, LIST rows);
-- x = [{'id': 1}]

-- Explicit single column mode
COPY (SELECT path FROM files) TO 'variable:paths' (FORMAT variable, LIST scalar);
-- Error if query has multiple columns
```

#### Glob Pattern Matching

Match multiple variables with glob patterns:

```sql
-- Set up multiple config variables
SET VARIABLE config_dev = '{"env": "development", "debug": true}';
SET VARIABLE config_prod = '{"env": "production", "debug": false}';
SET VARIABLE config_test = '{"env": "testing", "debug": true}';

-- Read all config_* variables at once
SELECT * FROM read_json('variable:config_*');
-- ┌─────────────┬─────────┐
-- │     env     │  debug  │
-- │   varchar   │ boolean │
-- ├─────────────┼─────────┤
-- │ development │ true    │
-- │ production  │ false   │
-- │ testing     │ true    │
-- └─────────────┴─────────┘

-- Use ? for single-character wildcards
SET VARIABLE data_01 = 'a,b\n1,2';
SET VARIABLE data_02 = 'a,b\n3,4';
SET VARIABLE data_100 = 'a,b\n5,6';

SELECT * FROM read_csv('variable:data_??');  -- Matches data_01, data_02 (not data_100)
```

### `pathvariable:` — Path Stored in Variable

Treat a variable's content as a file path, then read/write through the underlying filesystem.

**Key difference from `variable:`:**
- `variable:X` — Variable content **IS** the file content
- `pathvariable:X` — Variable content **IS A PATH** to a file

> **⚠️ Security — `pathvariable:` is a trust boundary.** The variable's content becomes the
> path handed to the underlying filesystem, including `s3://`, `http(s)://`, absolute paths and
> `../` traversal. **Never store untrusted or user-influenced input in a variable you then read
> through `pathvariable:`** — doing so lets the input's author choose the target (arbitrary local
> file read, or SSRF to internal/cloud-metadata endpoints), exactly as if you had passed that
> string straight to `read_csv(...)`. It grants no capability beyond `read_csv(userpath)`, and
> DuckDB's `enable_external_access` / `allowed_directories` still apply to the resolved path — so
> keep those gates enabled and treat `pathvariable:` targets as you would any raw user-supplied
> path.

#### Reading via Path Variable

```sql
-- Store a file path in a variable
SET VARIABLE data_path = '/data/reports/monthly.csv';

-- Read the file using the path from the variable
SELECT * FROM read_csv('pathvariable:data_path');
-- Equivalent to: SELECT * FROM read_csv('/data/reports/monthly.csv');

-- Works with any protocol the underlying filesystem supports
SET VARIABLE s3_path = 's3://my-bucket/data.parquet';
SELECT * FROM read_parquet('pathvariable:s3_path');
```

#### Writing via Path Variable

```sql
-- Store the output path in a variable
SET VARIABLE output_path = '/data/exports/results.csv';

-- Write query results to the path
COPY my_table TO 'pathvariable:output_path' (FORMAT csv);
-- Writes to /data/exports/results.csv
```

#### List Variable Support

Store multiple paths in a list variable to read them all at once:

```sql
-- Store multiple paths in a list
SET VARIABLE data_files = ['/data/jan.csv', '/data/feb.csv', '/data/mar.csv'];

-- Read all files from the list
SELECT * FROM read_csv('pathvariable:data_files');
-- Reads all three CSV files

-- Lists can contain glob patterns too
SET VARIABLE patterns = ['/data/2024/*.csv', '/archive/2023/*.csv'];
SELECT * FROM read_csv('pathvariable:patterns');
-- Expands both globs and reads all matching files
```

#### Multi-Level Glob Support

`pathvariable:` supports globs at multiple levels:

1. **Glob on variable names** — Match multiple path variables
2. **List expansion** — Variables containing `VARCHAR[]` expand to multiple paths
3. **Glob within paths** — Paths in variables can contain glob patterns

```sql
-- Level 1: Multiple variables with paths
SET VARIABLE input_jan = '/data/2024/jan/*.csv';
SET VARIABLE input_feb = '/data/2024/feb/*.csv';
SET VARIABLE input_mar = '/data/2024/mar/*.csv';

-- Glob matches variable names, then expands globs in each path
SELECT * FROM read_csv('pathvariable:input_*');
-- Reads all CSV files from jan/, feb/, and mar/ directories

-- Level 2: Single variable with glob path
SET VARIABLE all_logs = '/var/log/app/*.log';
SELECT * FROM read_text('pathvariable:all_logs');
-- Expands /var/log/app/*.log and reads all matching files
```

#### Dynamic Path Selection

Useful for switching between environments or data sources:

```sql
-- Set path based on environment
SET VARIABLE config_path = CASE
  WHEN current_setting('env') = 'prod' THEN '/etc/app/prod.json'
  ELSE './config/dev.json'
END;

SELECT * FROM read_json('pathvariable:config_path');
```

#### Modifiers

Modifiers control how paths are resolved, filtered, and constructed:

```
pathvariable:[modifier:]...variable_name[!value]
```

| Modifier | Description |
|----------|-------------|
| `search` | Return only the first existing file (for multi-root fallback) |
| `no-missing` | Skip non-existent files instead of erroring |
| `no-glob` | Disable glob pattern expansion in paths |
| `no-scalarfs` | Don't modify scalarfs protocol paths with append/prepend |
| `no-protocols` | Don't modify paths with explicit protocols (://) with append/prepend |
| `no-cache` | Reserved for future caching control (currently no-op) |
| `append!/path` | Append literal path to each base path |
| `append!$var` | Append value of variable to each base path |
| `prepend!/path` | Prepend literal path to each base path |
| `prepend!$var` | Prepend value of variable to each base path |

**Multi-Root Search** — Create a custom search path (like `file_search_path`) that checks local cache before remote:

```sql
-- Define search roots: local first, then remote
SET VARIABLE search_roots = [
    '/home/user/.cache/myapp/data',
    's3://company-bucket/shared/data'
];

-- Search returns FIRST existing match (local cache wins if present)
SELECT * FROM read_parquet(
    format('pathvariable:append:search:search_roots!/reports/{}.parquet', report_id)
);
```

**Cartesian Product** — When both base and append values are lists:

```sql
SET VARIABLE roots = ['/data/primary', '/data/backup'];
SET VARIABLE tables = ['users.parquet', 'orders.parquet'];

-- All combinations: 4 paths total
SELECT * FROM read_parquet('pathvariable:no-missing:append:roots!$tables');
-- Tries: /data/primary/users.parquet, /data/primary/orders.parquet,
--        /data/backup/users.parquet, /data/backup/orders.parquet
```

**Protocol Passthrough** — Prevent append/prepend from mangling protocol URLs:

```sql
SET VARIABLE sources = ['data+varchar:a,b\n1,2', 's3://bucket/data.csv', '/local/file.csv'];

-- Only append to local paths, leave protocols unchanged
SELECT * FROM read_csv('pathvariable:no-scalarfs:no-protocols:append:sources!/subdir');
-- Results: data+varchar:a,b\n1,2, s3://bucket/data.csv, /local/file.csv/subdir
```

See [pathvariable: Protocol Documentation](docs/protocols/pathvariable.md) for full details.

### `data+varchar:` — Raw Inline Content

Embed content directly in your query with zero encoding overhead.

```sql
-- Inline JSON
SELECT * FROM read_json('data+varchar:{"name": "Alice", "age": 30}');

-- Inline CSV (newlines work naturally in SQL strings)
SELECT * FROM read_csv('data+varchar:col1,col2,col3
1,2,3
4,5,6
7,8,9');

-- Quick test data
SELECT * FROM read_json('data+varchar:[
  {"id": 1, "status": "active"},
  {"id": 2, "status": "pending"},
  {"id": 3, "status": "complete"}
]');
```

### `data:` — RFC 2397 Data URIs

Standard data URI format with base64 or URL encoding.

```sql
-- Base64 encoded (good for binary or complex content)
SELECT * FROM read_csv('data:;base64,bmFtZSxzY29yZQpBbGljZSw5NQpCb2IsODc=');

-- URL encoded
SELECT * FROM read_csv('data:,name%2Cscore%0AAlice%2C95%0ABob%2C87');

-- With media type (ignored by readers, but valid syntax)
SELECT * FROM read_json('data:application/json;base64,eyJrZXkiOiJ2YWx1ZSJ9');
```

### `data+blob:` — Escaped Binary Content

For content with control characters, using simple escape sequences.

```sql
-- Content with escape sequences
SELECT * FROM read_text('data+blob:line1\nline2\nline3');

-- Supported escapes: \n \r \t \0 \\ \xNN
SELECT * FROM read_text('data+blob:tab\there\nnewline\nand hex: \x41\x42\x43');
```

### `decompress+gz:` / `decompress+zstd:` — Decompression Wrappers

Transparently decompress gzip or zstd content from any source. Wraps any path or protocol.

```sql
-- Decompress a local gzip file (useful when read_blob doesn't auto-decompress)
SELECT * FROM read_blob('decompress+gz:/path/to/data.bin.gz');

-- Decompress gzipped content stored in a variable
SET VARIABLE gzipped_data = from_base64('H4sIAAAAAAAAA/NIzcnJ11EIzy/KSQEAxoZbJgwAAAA=');
SELECT * FROM read_text('decompress+gz:variable:gzipped_data');

-- Decompress inline gzipped base64 data
SELECT * FROM read_text('decompress+gz:data:;base64,H4sIAAAAAAAAA/NIzcnJ11EIzy/KSQEAxoZbJgwAAAA=');

-- Chain with pathvariable for dynamic paths
SET VARIABLE gz_path = 's3://bucket/data.csv.gz';
SELECT * FROM read_csv('decompress+gz:pathvariable:gz_path');

-- Zstd decompression works the same way
SELECT * FROM read_blob('decompress+zstd:/path/to/data.bin.zst');
SELECT * FROM read_csv('decompress+zstd:variable:zstd_compressed_csv');
```

**Comparison with zipfs `archive:` protocol:**

| Feature | `decompress+gz:`/`decompress+zstd:` (scalarfs) | `archive:` (zipfs) |
|---------|-----------------------------------------------|-------------------|
| Format | gzip/zstd streams | zip archives only |
| Purpose | Decompress single stream | Access files within .zip |
| Use case | `.gz`/`.zst` files, compressed variables | Multi-file zip archives |
| Syntax | `decompress+gz:/path/file.gz` | `archive:///path/file.zip/entry.csv` |

The zipfs `archive:` protocol extracts files from zip archives (e.g., `archive:///data.zip/file.csv`). It does **not** handle gzip or zstd files - use `decompress+gz:` or `decompress+zstd:` for those.

#### Decompression output cap (bomb guard)

Decompressed output is materialized into memory as a raw buffer that is **not** tracked by
DuckDB's buffer manager, so it is **not** bounded by `SET memory_limit`. To stop a decompression
bomb (a tiny input that inflates to gigabytes), each `decompress+gz:` / `decompress+zstd:` read is
capped by the `scalarfs_max_decompressed_bytes` setting:

```sql
-- Default cap is 256 MiB. Lower it to be stricter, e.g. 16 MiB:
SET scalarfs_max_decompressed_bytes = 16777216;

-- Or raise it if you legitimately decompress larger payloads.
SET scalarfs_max_decompressed_bytes = 1073741824;  -- 1 GiB

-- Set to 0 to disable the cap entirely (not recommended for untrusted input).
SET scalarfs_max_decompressed_bytes = 0;
```

The cap is enforced at every inflate path — including the zstd frame-declared size (checked
*before* allocation) and the gzip / zstd-streaming accumulators — so a lying or absent size
header cannot route around it. When the limit is exceeded the read fails with a clean error
rather than exhausting memory.

### `pathmacro:` — Paths Resolved by a Macro

`pathmacro:<macro>?k=v&...` resolves to real file paths by calling a **scalar macro you register**, so a globbing reader (`read_csv`, `read_json`, `read_parquet`, ...) scans only the files the macro returns. This turns a catalog table into a data-skipping / file-selection layer: the macro decides *which* files to read, the reader does the rest.

```sql
-- A catalog mapping a key to file paths (a plain table/view or a parquet index)
CREATE VIEW catalog AS
  SELECT 'west' AS region, '/data/west.csv' AS file_path
  UNION ALL SELECT 'east', '/data/east.csv';

-- A scalar macro: MAP(VARCHAR, VARCHAR) -> VARCHAR[] of paths to read
CREATE MACRO region_files(params) AS (
  SELECT list(file_path) FROM catalog WHERE region = params['region']
);

-- Opt in: macros must be allow-listed before pathmacro: will call them
SET allowed_pathmacros = 'region_files';

-- Reads ONLY the files the macro returns (here: just the east shard)
SELECT * FROM read_csv('pathmacro:region_files?region=east');
```

**Contract:**

- The macro takes a single argument — the query string as a `MAP(VARCHAR, VARCHAR)` (`params['region']`) — and must return `VARCHAR[]` (a list of paths). Returning a table? Wrap it: `SELECT list(path) FROM ...`.
- Returned paths may themselves be globs or other protocols (e.g. `s3://…`, a directory glob); they're re-dispatched to the underlying filesystem.
- A paramless call is fine: `pathmacro:my_macro` invokes `my_macro(map([], []))`.

**The macro body is ordinary SQL** — selection logic can be as rich as you need:

```sql
-- List comprehension: build paths straight from a URL param (years=2020,2021)
CREATE MACRO years_list(p) AS (
  ['/data/' || y || '.csv' FOR y IN string_split(p['years'], ',')]
);

-- Arithmetic / range over a catalog
CREATE MACRO year_between(p) AS (
  SELECT list(file_path) FROM catalog
  WHERE year BETWEEN CAST(p['from'] AS INT) AND CAST(p['to'] AS INT)
);

-- Subquery: only the latest year on record
CREATE MACRO latest_year(p) AS (
  SELECT list(file_path) FROM catalog WHERE year = (SELECT max(year) FROM catalog)
);

-- Return a recursive glob; pathmacro re-dispatches it to the real filesystem
CREATE MACRO all_shards(p) AS (['/data/**/*.csv']);
```

**Build URLs safely** with `to_pathmacro_url()` instead of string concatenation — it URL-encodes keys and values so metacharacters survive:

```sql
SELECT to_pathmacro_url('region_files', {region: 'west', year: 2024});
-- pathmacro:region_files?region=west&year=2024

SELECT to_pathmacro_url('m', {a: 'x y', b: '1&2'});   -- encodes space and '&'
-- pathmacro:m?a=x%20y&b=1%262

SELECT from_pathmacro_url('pathmacro:region_files?region=west');  -- parse back
-- {'macro': region_files, 'params': {region=west}}
```

**Security** — this protocol runs SQL, so it is locked down by default:

- **Opt-in allow-list:** nothing is callable until you `SET allowed_pathmacros = 'name1,name2'`. The default is empty (no macros allowed).
- **Identifier validation:** the macro name must be a plain SQL identifier (rejected otherwise, before any lookup).
- **Injection-safe parameters:** query-string values are passed as escaped string *data* (single quotes doubled; DuckDB literals don't interpret backslashes), never as SQL. A value like `region=west' OR '1'='1` is matched as a literal string, not executed.

See [pathmacro: Protocol Documentation](docs/protocols/pathmacro.md) for the full contract, catalog patterns, and error cases.

## Helper Functions

Convert between content and URIs programmatically:

### Encoding Functions

```sql
-- Base64 data URI
SELECT to_data_uri('hello world');
-- data:;base64,aGVsbG8gd29ybGQ=

-- Raw varchar URI (zero overhead)
SELECT to_varchar_uri('{"key": "value"}');
-- data+varchar:{"key": "value"}

-- Escaped blob URI
SELECT to_blob_uri('line1\nline2');
-- data+blob:line1\nline2

-- Auto-select optimal encoding
SELECT to_scalarfs_uri('simple text');      -- Uses data+varchar:
SELECT to_scalarfs_uri(chr(0) || chr(1));   -- Uses data:;base64,
```

### Decoding Functions

```sql
-- Decode any scalarfs URI
SELECT from_data_uri('data:;base64,aGVsbG8=');           -- hello
SELECT from_varchar_uri('data+varchar:hello');           -- hello
SELECT from_blob_uri('data+blob:line1\nline2');          -- line1<newline>line2
SELECT from_scalarfs_uri('data+varchar:auto-detected');  -- auto-detected
```

### Auto-Selection Logic

`to_scalarfs_uri()` picks the optimal encoding automatically:

| Content Type | Chosen Protocol | Reason |
|--------------|-----------------|--------|
| Safe text (printable + whitespace) | `data+varchar:` | Zero overhead |
| Text with few control chars (<10%) | `data+blob:` | Minimal escaping |
| Binary or heavy escaping needed | `data:;base64,` | Predictable 33% overhead |

### pathmacro: URL Helpers

Build and parse `pathmacro:` URLs without manual string wrangling. `to_pathmacro_url()` validates the macro name and URL-encodes every key/value, so special characters (spaces, `&`, `=`) survive as data.

```sql
-- Build from a STRUCT (named-argument feel) or a MAP; non-text values cast to text
SELECT to_pathmacro_url('region_files', {region: 'west', year: 2024});
-- pathmacro:region_files?region=west&year=2024

SELECT to_pathmacro_url('m', {a: 'x y', b: '1&2', c: 'p=q'});
-- pathmacro:m?a=x%20y&b=1%262&c=p%3Dq

SELECT to_pathmacro_url('all_files');           -- no params -> bare URL
-- pathmacro:all_files

-- Parse back into macro + params MAP(VARCHAR, VARCHAR)
SELECT from_pathmacro_url('pathmacro:region_files?region=west&year=2024');
-- {'macro': region_files, 'params': {region=west, year=2024}}
```

| Function | Signature | Returns |
|----------|-----------|---------|
| `to_pathmacro_url` | `to_pathmacro_url(macro VARCHAR[, params STRUCT│MAP])` | `VARCHAR` (a `pathmacro:` URL) |
| `from_pathmacro_url` | `from_pathmacro_url(url VARCHAR)` | `STRUCT(macro VARCHAR, params MAP(VARCHAR, VARCHAR))` |

## Use Cases

### Inline Test Data

```sql
-- Quick tests without creating files
SELECT * FROM read_csv('data+varchar:id,name,email
1,Alice,alice@example.com
2,Bob,bob@example.com')
WHERE name LIKE 'A%';
```

### Configuration Management

```sql
-- Store configuration in variables
SET VARIABLE app_config = '{
  "database": {"host": "localhost", "port": 5432},
  "cache": {"enabled": true, "ttl": 3600}
}';

-- Access nested configuration
SELECT config.database.host, config.cache.ttl
FROM read_json('variable:app_config') AS config;
```

### Pipeline Intermediate Results

```sql
-- Process data through multiple steps using variables
SET VARIABLE raw_data = '...';

-- Step 1: Parse and filter
COPY (
  SELECT * FROM read_json('variable:raw_data')
  WHERE status = 'active'
) TO 'variable:filtered' (FORMAT json);

-- Step 2: Aggregate
COPY (
  SELECT category, count(*) as cnt
  FROM read_json('variable:filtered')
  GROUP BY category
) TO 'variable:summary' (FORMAT json);

-- Final result
SELECT * FROM read_json('variable:summary');
```

### Transparent Storage Facade

Store paths that could point to inline data, local files, or remote storage:

```sql
CREATE TABLE documents(
  id INT,
  content_path VARCHAR  -- Could be any of these:
);

-- Inline small content
INSERT INTO documents VALUES (1, 'data+varchar:{"type": "note", "text": "Hello"}');

-- Local file
INSERT INTO documents VALUES (2, '/data/documents/report.json');

-- S3
INSERT INTO documents VALUES (3, 's3://bucket/documents/large.json');

-- All work transparently with read_json()
SELECT id, doc.* FROM documents, read_json(content_path) AS doc;
```

## Building from Source

### Prerequisites

- CMake 3.14+
- C++17 compiler
- Git

### Build Steps

```bash
# Clone with submodules
git clone --recurse-submodules https://github.com/your-repo/duckdb_scalarfs
cd duckdb_scalarfs

# Build
make

# Run tests
make test
```

### Build Outputs

```
build/release/duckdb                                    # DuckDB shell with extension
build/release/test/unittest                             # Test runner
build/release/extension/scalarfs/scalarfs.duckdb_extension  # Loadable extension
```

## Running Tests

```bash
# Run all extension tests
make test

# Run specific test file
./build/release/test/unittest "test/sql/variable_glob.test"

# Run all SQL tests
./build/release/test/unittest "[sql]"
```

## Limitations

- **Content size**: Limited by DuckDB's VARCHAR/BLOB size limits and available memory
- **No streaming**: Entire content is buffered before reading
- **Write support**: Only `variable:` and `pathvariable:` protocols support writing
- **No null bytes in VARCHAR**: Use `data+blob:` or `data:;base64,` for binary content
- **pathvariable: type restriction**: Variable must be VARCHAR, BLOB, or a list of those types (VARCHAR[], BLOB[]). List variables are only supported for reading, not writing.

## License

MIT

## Related

- [DuckDB](https://duckdb.org/) — The database engine
- [RFC 2397](https://datatracker.ietf.org/doc/html/rfc2397) — The data: URL scheme specification
