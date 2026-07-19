# pathmacro: Protocol

The `pathmacro:` protocol resolves a URL to real file paths by calling a **scalar macro you register**. A globbing reader (`read_csv`, `read_json`, `read_parquet`, ...) then scans only the files the macro returns. This turns a catalog table into a file-selection / data-skipping layer: the macro decides *which* files to read; the reader does the rest.

Unlike the `variable:`/`data:` families, which materialize *content*, `pathmacro:` produces *paths* — it is a routing layer over your real filesystem(s).

## Syntax

```
pathmacro:<macro>[?key=value&key=value...]
```

- `<macro>` — the name of a registered scalar macro. Must be a plain SQL identifier and must be allow-listed (see [Security](#security)).
- The query string (after `?`) is parsed into a `MAP(VARCHAR, VARCHAR)` and passed to the macro as its single argument. Keys and values are URL-percent-decoded (`%20` → space, etc.).

## Macro Contract

The macro must:

1. Take a single argument — the query-string `MAP(VARCHAR, VARCHAR)`.
2. Return `VARCHAR[]` — a list of file paths to read.

```sql
CREATE MACRO region_files(params) AS (
  SELECT list(file_path) FROM catalog WHERE region = params['region']
);
```

If your selection logic is a query returning rows, wrap it with `list(...)`:

```sql
CREATE MACRO region_files(params) AS (
  SELECT list(DISTINCT file_path) FROM catalog WHERE region = params['region']
);
```

Notes:

- **Paramless calls** are valid: `pathmacro:my_macro` invokes `my_macro(map([], []))`. The macro still takes the map argument; it may ignore it.
- **Returned paths may be globs or other protocols.** A path like `/data/2024/*.csv`, `s3://bucket/shard-*.parquet`, or a directory glob is re-dispatched to the underlying filesystem and expanded. This lets one catalog row fan out to many files.
- A returned `NULL` path is skipped; a `NULL` list resolves to zero files.

## Example: per-shard catalog

```sql
-- 1. A catalog mapping a key to file paths (a plain table/view, or a parquet index).
CREATE VIEW catalog AS
  SELECT 'west' AS region, '/data/west.csv' AS file_path
  UNION ALL SELECT 'east', '/data/east.csv';

-- 2. A macro: MAP(VARCHAR, VARCHAR) -> VARCHAR[] of paths.
CREATE MACRO region_files(params) AS (
  SELECT list(file_path) FROM catalog WHERE region = params['region']
);

-- 3. Opt in (see Security).
SET allowed_pathmacros = 'region_files';

-- 4. Read ONLY the east shard, resolved through the catalog. No hard-coded path.
SELECT * FROM read_csv('pathmacro:region_files?region=east');
```

Because the macro returns only the east path, `region=west` and `region=east` each touch a single file — inter-file pruning driven entirely by the catalog.

### Inspecting what a URL resolves to

`glob()` runs the resolver without reading anything, so you can see exactly which files a URL selects:

```sql
SELECT file FROM glob('pathmacro:region_files?region=east');
-- /data/east.csv
```

### Composing with WHERE

Because `pathmacro:` only chooses *files*, it composes with an ordinary `WHERE` on the rows. The same value can drive both — file selection *and* a row filter — which is useful when a selected file still holds more than you want:

```sql
-- pathmacro picks the west file(s); WHERE narrows rows within them
SELECT * FROM read_csv('pathmacro:region_files?region=west') WHERE region = 'west';
```

`pathmacro:` handles the coarse, inter-file pruning (which files to open); the reader's own predicate pushdown handles the fine, intra-file filtering (which rows).

## Complex file selection

The macro body is ordinary SQL, so selection can use any expression that yields `VARCHAR[]`. These four patterns all work end-to-end through `pathmacro:`:

```sql
-- List comprehension: build paths directly from a URL param (years=2020,2021)
CREATE MACRO years_list(p) AS (
  ['/data/' || y || '.csv' FOR y IN string_split(p['years'], ',')]
);
-- pathmacro:years_list?years=2020%2C2021   (the comma is URL-encoded as %2C)

-- Arithmetic / BETWEEN over a catalog
CREATE MACRO year_between(p) AS (
  SELECT list(file_path) FROM catalog
  WHERE year BETWEEN CAST(p['from'] AS INT) AND CAST(p['to'] AS INT)
);
-- pathmacro:year_between?from=2020&to=2023

-- Subquery: only the latest year on record
CREATE MACRO latest_year(p) AS (
  SELECT list(file_path) FROM catalog WHERE year = (SELECT max(year) FROM catalog)
);
-- pathmacro:latest_year

-- Recursive ** glob, re-dispatched by pathmacro to the real filesystem
CREATE MACRO all_shards(p) AS (['/data/**/*.csv']);
-- pathmacro:all_shards
```

Because the macro can query an index table, the same mechanism scales from a handful of files to a large sharded dataset (e.g. partitioned by region + year): the catalog answers "which files", and the reader's own pushdown handles "which rows within a file".

## Building URLs: to_pathmacro_url / from_pathmacro_url

Rather than concatenating URLs by hand (and risking a stray `&` or space corrupting the query string), use the safe constructor. `to_pathmacro_url()` validates the macro name and URL-encodes every key and value.

```sql
-- STRUCT params read like named arguments; non-text values are cast to text
SELECT to_pathmacro_url('region_files', {region: 'west', year: 2024});
-- pathmacro:region_files?region=west&year=2024

-- MAP params work too
SELECT to_pathmacro_url('region_files', MAP {'region': 'west'});
-- pathmacro:region_files?region=west

-- Metacharacters in values survive as single params
SELECT to_pathmacro_url('m', {a: 'x y', b: '1&2', c: 'p=q'});
-- pathmacro:m?a=x%20y&b=1%262&c=p%3Dq

-- No params -> bare URL
SELECT to_pathmacro_url('all_shards');
-- pathmacro:all_shards
```

`from_pathmacro_url()` is the inverse — it parses a URL back into its macro name and a decoded params map:

```sql
SELECT from_pathmacro_url('pathmacro:region_files?region=west&year=2024');
-- {'macro': region_files, 'params': {region=west, year=2024}}
```

| Function | Signature | Returns |
|----------|-----------|---------|
| `to_pathmacro_url` | `to_pathmacro_url(macro VARCHAR[, params STRUCT│MAP])` | `VARCHAR` — a `pathmacro:` URL |
| `from_pathmacro_url` | `from_pathmacro_url(url VARCHAR)` | `STRUCT(macro VARCHAR, params MAP(VARCHAR, VARCHAR))` |

> Note: `to_pathmacro_url()` returns a runtime string, so it can't be passed directly to a table function that requires a constant path (e.g. `read_csv(<literal>)`). Use it to construct URLs in application code or dynamic SQL; use `from_pathmacro_url()` for inspection and round-tripping.

## Security

`pathmacro:` runs SQL (it calls your macro on a fresh connection during globbing), so it is locked down by default:

| Control | Behavior |
|---------|----------|
| **Opt-in allow-list** | Nothing is callable until `SET allowed_pathmacros = 'name1,name2'`. Default is empty. |
| **Identifier validation** | The macro name must be a plain SQL identifier (`[A-Za-z_][A-Za-z0-9_]*`). Rejected before any lookup. |
| **Injection-safe params** | Query-string values are passed as escaped string *data* (single quotes doubled; DuckDB string literals do not interpret backslashes), never interpolated as SQL. |

A hostile-looking value is treated as literal data, not executed:

```sql
-- The value `west' OR '1'='1` is matched as a literal string (matches no region),
-- NOT executed as SQL. It does not widen the result to all shards.
SELECT * FROM read_csv('pathmacro:region_files?region=west%27%20OR%20%271%27%3D%271');
-- Error: No files found that match the pattern ...
```

Only the macro *name* is treated as an identifier (and it is validated + allow-listed first); all `key`/`value` pairs are data.

## Error Cases

| Condition | Result |
|-----------|--------|
| Macro not in `allowed_pathmacros` | Error: `macro '<name>' is not in allowed_pathmacros` |
| Macro name is not a valid identifier | Error: `invalid macro name '<name>'` |
| Macro returns a non-`VARCHAR[]` value | Error: `macro '<name>' must return VARCHAR[]` |
| Macro raises / references a missing table | Error: `catalog macro '<name>' failed: <underlying error>` |
| Macro returns an empty list | The reader sees zero files (typically `No files found that match the pattern`) |

## Configuration

| Setting | Type | Default | Description |
|---------|------|---------|-------------|
| `allowed_pathmacros` | VARCHAR | `''` (empty) | Comma-separated list of macro names `pathmacro:` may invoke. |

```sql
SET allowed_pathmacros = 'region_files, sample_files';
```

## Next Steps

- [Protocol Overview](overview.md) — all scalarfs protocols
- [pathvariable: Protocol](pathvariable.md) — dynamic file paths stored in variables (the static-value analog of `pathmacro:`)
- [API Reference](../reference/api.md) — full protocol + configuration reference
