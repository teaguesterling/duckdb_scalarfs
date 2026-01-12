# pathvariable: Protocol

The `pathvariable:` protocol allows you to store file paths in DuckDB variables and use those paths for file operations. This enables dynamic path selection and powerful two-level glob patterns.

## Syntax

```
pathvariable:[modifier:]...variable_name[!value]
```

Basic usage (no modifiers):
```
pathvariable:variable_name
```

With modifiers:
```
pathvariable:search:no-missing:append:roots!/blobs/data.bin
```

## Modifiers

Modifiers control how paths are resolved, filtered, and constructed. They can be combined.

| Modifier | Description |
|----------|-------------|
| `search` | Return only the first existing file (for multi-root fallback) |
| `no-missing` | Skip non-existent files instead of erroring |
| `no-glob` | Disable glob pattern expansion in paths |
| `no-scalarfs` | Don't modify scalarfs protocol paths with append/prepend |
| `no-protocols` | Don't modify paths with explicit protocols (://) with append/prepend |
| `no-cache` | Disable caching of path resolution |
| `append!/path` | Append literal path to each base path |
| `append!$var` | Append value of variable to each base path |
| `prepend!/path` | Prepend literal path to each base path |
| `prepend!$var` | Prepend value of variable to each base path |

### Multi-Root Search (file_search_path Pattern)

Create a custom search path that checks local cache before remote storage:

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

### Content-Addressed Storage (Blob Resolution)

Resolve blobs by hash across multiple storage tiers:

```sql
-- Set blob storage roots
SET VARIABLE blob_roots = [
    '/local/blobs',           -- Fast local storage
    's3://archive/blobs'      -- Cold storage fallback
];

-- Find blob by content hash (checks local first)
SELECT content FROM read_blob(
    format('pathvariable:append:search:blob_roots!/{}/{}.bin.gz', hash[:2], hash)
);
```

### Ignore Missing Files

Read all files that exist, skip missing ones:

```sql
SET VARIABLE config_paths = [
    '/etc/myapp/config.json',        -- System config
    '~/.config/myapp/config.json',   -- User config
    './config.json'                   -- Local override
];

-- Reads all configs that exist, ignores missing
SELECT * FROM read_json('pathvariable:no-missing:config_paths');
```

### Path Construction with Passthrough

When mixing protocols, use passthrough modifiers to prevent mangling:

```sql
SET VARIABLE sources = [
    'data+varchar:inline,data\n1,2',  -- Inline data
    's3://bucket/remote.csv',          -- Remote file
    '/local/file.csv'                  -- Local file
];

-- Append suffix only to local paths, leave protocols alone
SELECT * FROM read_csv('pathvariable:no-scalarfs:no-protocols:append:sources!/subdir');
-- Results in:
--   data+varchar:inline,data\n1,2     (unchanged - scalarfs protocol)
--   s3://bucket/remote.csv            (unchanged - explicit protocol)
--   /local/file.csv/subdir            (appended)
```

### Cartesian Product with Lists

When both base variable and append value are lists, creates all combinations:

```sql
SET VARIABLE roots = ['/data/primary', '/data/backup'];
SET VARIABLE tables = ['users.parquet', 'orders.parquet'];

-- All combinations: 4 paths total
SELECT * FROM read_parquet('pathvariable:no-missing:append:roots!$tables');
-- Tries: /data/primary/users.parquet, /data/primary/orders.parquet,
--        /data/backup/users.parquet, /data/backup/orders.parquet
-- Reads all that exist
```

## Key Difference from variable:

| Protocol | What the variable contains | What happens |
|----------|---------------------------|--------------|
| `variable:X` | File content | Content is used directly as file data |
| `pathvariable:X` | File path | Path is resolved, then file is opened |

```sql
-- variable: - content IS the file
SET VARIABLE my_data = 'a,b\n1,2';
SELECT * FROM read_csv('variable:my_data');

-- pathvariable: - content is a PATH to a file
SET VARIABLE my_path = '/data/input.csv';
SELECT * FROM read_csv('pathvariable:my_path');
```

## Reading via Path Variable

Store a file path in a variable, then read the file:

```sql
-- Store the path
SET VARIABLE data_path = '/data/reports/monthly.csv';

-- Read the file at that path
SELECT * FROM read_csv('pathvariable:data_path');
-- Equivalent to: SELECT * FROM read_csv('/data/reports/monthly.csv');
```

### Works with Any Protocol

The path stored in the variable can use any protocol that DuckDB supports:

```sql
-- Local files
SET VARIABLE local_path = '/home/user/data.csv';

-- S3 storage
SET VARIABLE s3_path = 's3://my-bucket/data.parquet';

-- HTTP URLs
SET VARIABLE http_path = 'https://example.com/data.json';

-- Other scalarfs protocols
SET VARIABLE inline_path = 'data+varchar:a,b\n1,2';

-- All work with pathvariable:
SELECT * FROM read_csv('pathvariable:local_path');
SELECT * FROM read_parquet('pathvariable:s3_path');
SELECT * FROM read_json('pathvariable:http_path');
SELECT * FROM read_csv('pathvariable:inline_path');
```

## Writing via Path Variable

Use COPY to write to a path stored in a variable:

```sql
-- Store the output path
SET VARIABLE output_path = '/data/exports/results.csv';

-- Write to that path
COPY my_table TO 'pathvariable:output_path' (FORMAT csv, HEADER true);

-- The file is written to /data/exports/results.csv
```

### Writing with Temp Files

DuckDB's default COPY behavior uses temporary files for safety. This works automatically with `pathvariable:`:

```sql
SET VARIABLE out = '/data/output.csv';

-- Uses temp file by default (writes to /data/tmp_output.csv first, then moves)
COPY my_table TO 'pathvariable:out' (FORMAT csv);

-- Disable temp file if needed
COPY my_table TO 'pathvariable:out' (FORMAT csv, USE_TMP_FILE false);
```

## List Variable Support

Store multiple paths in a `VARCHAR[]` list variable to read them all at once:

```sql
-- Store multiple paths in a list
SET VARIABLE data_files = ['/data/jan.csv', '/data/feb.csv', '/data/mar.csv'];

-- Read all files from the list
SELECT * FROM read_csv('pathvariable:data_files');
-- Reads all three CSV files
```

### Lists with Glob Patterns

List elements can contain glob patterns, which are expanded:

```sql
-- Each list element is a glob pattern
SET VARIABLE patterns = ['/data/2024/*.csv', '/archive/2023/*.csv'];

-- Expands both patterns and reads all matching files
SELECT * FROM read_csv('pathvariable:patterns');
```

### Storing Paths from Queries

Use `variable:` to store query results as a path list, then read with `pathvariable:`:

```sql
-- Store paths from a query into a variable
COPY (SELECT file_path FROM my_file_index WHERE category = 'reports')
TO 'variable:report_paths' (FORMAT csv, HEADER false);

-- The variable now contains paths, one per line
-- Read all those files
SELECT * FROM read_csv('pathvariable:report_paths');
```

!!! note "List writes not supported"
    List variables (`VARCHAR[]`) are only supported for reading. For writes, use a scalar VARCHAR variable.

## Multi-Level Glob Support

`pathvariable:` supports glob patterns at multiple levels:

### Level 1: Glob on Variable Names

Match multiple variables containing paths:

```sql
SET VARIABLE input_jan = '/data/2024/jan/sales.csv';
SET VARIABLE input_feb = '/data/2024/feb/sales.csv';
SET VARIABLE input_mar = '/data/2024/mar/sales.csv';

-- Glob matches variable names: input_jan, input_feb, input_mar
-- Then reads from each path
SELECT * FROM read_csv('pathvariable:input_*');
```

### Level 2: List Expansion

Variables containing `VARCHAR[]` lists expand to multiple paths:

```sql
SET VARIABLE all_inputs = ['/data/sales.csv', '/data/returns.csv'];

-- List expands to both paths
SELECT * FROM read_csv('pathvariable:all_inputs');
```

### Level 3: Glob in the Path

Paths stored in variables can contain glob patterns:

```sql
SET VARIABLE all_csvs = '/data/2024/*.csv';

-- Expands /data/2024/*.csv to all matching files
SELECT * FROM read_csv('pathvariable:all_csvs');
```

### All Levels Combined

The most powerful use case—glob on variable names, list expansion, AND glob in paths:

```sql
-- Each variable contains a glob path
SET VARIABLE logs_app1 = '/var/log/app1/*.log';
SET VARIABLE logs_app2 = '/var/log/app2/*.log';
SET VARIABLE logs_app3 = '/var/log/app3/*.log';

-- Level 1: matches logs_app1, logs_app2, logs_app3
-- Level 3: expands each /var/log/appN/*.log pattern
SELECT * FROM read_text('pathvariable:logs_*');
-- Reads ALL log files from ALL three app directories
```

## Dynamic Path Selection

Use SQL expressions to set paths dynamically:

```sql
-- Set path based on current date
SET VARIABLE daily_path = '/data/reports/' || strftime(current_date, '%Y/%m/%d') || '.csv';

SELECT * FROM read_csv('pathvariable:daily_path');
```

### Environment-Based Paths

```sql
-- Different paths for different environments
SET VARIABLE config_path = CASE
  WHEN current_setting('env') = 'production' THEN '/etc/app/prod.json'
  WHEN current_setting('env') = 'staging' THEN '/etc/app/staging.json'
  ELSE './config/dev.json'
END;

SELECT * FROM read_json('pathvariable:config_path');
```

## Error Handling

### Variable Not Found

```sql
SELECT * FROM read_text('pathvariable:nonexistent');
-- Error: Variable 'nonexistent' not found
```

### NULL Variable

```sql
SET VARIABLE nullable = NULL;
SELECT * FROM read_text('pathvariable:nullable');
-- Error: Variable 'nullable' is NULL
```

### Wrong Variable Type

The variable must be VARCHAR, BLOB, or a list of those types:

```sql
SET VARIABLE int_path = 42;
SELECT * FROM read_text('pathvariable:int_path');
-- Error: Variable 'int_path' must be VARCHAR or BLOB type to be used as a path, got INTEGER

-- Integer lists also fail
SET VARIABLE int_list = [1, 2, 3];
SELECT * FROM read_text('pathvariable:int_list');
-- Error: Variable 'int_list' is a list but child type must be VARCHAR or BLOB, got INTEGER[]
```

### List Variable for Writes

List variables cannot be used for write operations:

```sql
SET VARIABLE paths = ['/data/out1.csv', '/data/out2.csv'];
COPY my_table TO 'pathvariable:paths' (FORMAT csv);
-- Error: Variable 'paths' is a list type (VARCHAR[]). List variables are supported for reading,
--        but not for single-file write operations.
```

### File Not Found

If the path doesn't exist, you get the underlying filesystem's error:

```sql
SET VARIABLE bad_path = '/nonexistent/file.csv';
SELECT * FROM read_text('pathvariable:bad_path');
-- Error: Cannot open file "/nonexistent/file.csv": No such file or directory
```

### No Glob Matches

```sql
SET VARIABLE empty_glob = '/data/nonexistent_*.csv';
SELECT * FROM read_csv('pathvariable:empty_glob');
-- Error: No files found that match the pattern "/data/nonexistent_*.csv"
```

## Use Cases

### Configuration Management

```sql
-- Store config paths by environment
SET VARIABLE db_config = '/etc/app/database.json';
SET VARIABLE cache_config = '/etc/app/cache.json';
SET VARIABLE log_config = '/etc/app/logging.json';

-- Read all configs at once
SELECT * FROM read_json('pathvariable:*_config');
```

### Data Pipeline Paths

```sql
-- Define pipeline paths
SET VARIABLE raw_data = 's3://bucket/raw/2024/*.parquet';
SET VARIABLE processed_output = 's3://bucket/processed/2024/result.parquet';

-- Read raw, process, write processed
COPY (
  SELECT * FROM read_parquet('pathvariable:raw_data')
  WHERE valid = true
) TO 'pathvariable:processed_output' (FORMAT parquet);
```

### Multi-Tenant Data Access

```sql
-- Set tenant-specific path
SET VARIABLE tenant_data = '/data/tenants/' || current_setting('tenant_id') || '/data.csv';

-- All queries use tenant-specific data
SELECT * FROM read_csv('pathvariable:tenant_data');
```

## Best Practices

### Use Descriptive Variable Names

```sql
-- Good: clear what the path is for
SET VARIABLE daily_sales_report_path = '...';
SET VARIABLE customer_export_path = '...';

-- Avoid: unclear purpose
SET VARIABLE path1 = '...';
```

### Group Related Paths

```sql
-- Use consistent prefixes for glob access
SET VARIABLE input_customers = '/data/customers.csv';
SET VARIABLE input_orders = '/data/orders.csv';
SET VARIABLE input_products = '/data/products.csv';

-- Read all inputs at once
SELECT * FROM read_csv('pathvariable:input_*');
```

### Validate Paths Before Use

```sql
-- Set path
SET VARIABLE data_path = '/data/important.csv';

-- Use in query (will fail if file doesn't exist)
SELECT * FROM read_csv('pathvariable:data_path');
```

## See Also

- [variable: Protocol](variable.md) — Store file content in variables
- [Protocol Overview](overview.md) — Compare all protocols
