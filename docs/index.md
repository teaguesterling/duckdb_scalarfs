# scalarfs — DuckDB Scalar Filesystem Extension

A DuckDB extension that enables reader/writer functions to work with in-memory content: variables and inline literals.

## What is scalarfs?

DuckDB's file functions (`read_csv`, `read_json`, `COPY TO`, etc.) expect file paths. **scalarfs** bridges the gap when your content is already in memory, allowing the same functions to work with:

- **Variables** — Store data in DuckDB variables and read/write them as files
- **Inline literals** — Embed content directly in your queries without temporary files

## Supported Protocols

| Protocol | Purpose | Mode |
|----------|---------|------|
| `variable:` | DuckDB variable as file | Read/Write |
| `data:` | RFC 2397 data URI (base64/url-encoded) | Read |
| `data+varchar:` | Raw VARCHAR content as file | Read |
| `data+blob:` | Escaped BLOB content as file | Read |

## Quick Example

```sql
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
```

## Why scalarfs?

### Before scalarfs

```sql
-- Need to create temporary files for inline data
COPY (SELECT 'name,score' || chr(10) || 'Alice,95') TO '/tmp/data.csv';
SELECT * FROM read_csv('/tmp/data.csv');

-- Can't read variables as files
SET VARIABLE my_json = '{"key": "value"}';
-- ❌ SELECT * FROM read_json(getvariable('my_json'));  -- Doesn't work!
```

### With scalarfs

```sql
-- Inline data without temporary files
SELECT * FROM read_csv('data+varchar:name,score
Alice,95');

-- Variables work as files
SET VARIABLE my_json = '{"key": "value"}';
SELECT * FROM read_json('variable:my_json');  -- ✅ Works!

-- Glob patterns across variables
SET VARIABLE config_dev = '{"env": "dev"}';
SET VARIABLE config_prod = '{"env": "prod"}';
SELECT * FROM read_json('variable:config_*');  -- ✅ Reads both!
```

## Getting Started

Ready to use scalarfs? Check out the [Installation Guide](getting-started/installation.md) and [Quick Start Tutorial](getting-started/quickstart.md).

## Features

- **Variable Protocol** — Read and write DuckDB variables as files with `variable:name`
- **Glob Support** — Match multiple variables with patterns like `variable:config_*`
- **Data URIs** — Use RFC 2397 data URIs with `data:;base64,...`
- **Zero-Overhead Inline** — Embed content directly with `data+varchar:content`
- **Binary Support** — Handle binary content with `data+blob:...` escape sequences
- **Helper Functions** — Convert between content and URIs with `to_*_uri()` and `from_*_uri()`
