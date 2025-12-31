# variable: Protocol

The `variable:` protocol allows you to read from and write to DuckDB session variables as if they were files.

## Syntax

```
variable:variable_name
```

## Reading Variables

Store content in a DuckDB variable, then read it with any file reader:

```sql
-- Store CSV data
SET VARIABLE employees = 'name,department,salary
Alice,Engineering,95000
Bob,Marketing,75000
Carol,Engineering,105000';

-- Read as CSV
SELECT * FROM read_csv('variable:employees');
```

Output:
```
┌─────────┬─────────────┬────────┐
│  name   │ department  │ salary │
│ varchar │   varchar   │ int64  │
├─────────┼─────────────┼────────┤
│ Alice   │ Engineering │  95000 │
│ Bob     │ Marketing   │  75000 │
│ Carol   │ Engineering │ 105000 │
└─────────┴─────────────┴────────┘
```

### JSON Variables

```sql
SET VARIABLE config = '{
  "database": {
    "host": "localhost",
    "port": 5432
  },
  "cache": {
    "enabled": true,
    "ttl": 3600
  }
}';

SELECT config.database.host, config.cache.ttl
FROM read_json('variable:config') AS config;
```

### Array Variables

```sql
SET VARIABLE users = '[
  {"id": 1, "name": "Alice", "active": true},
  {"id": 2, "name": "Bob", "active": false},
  {"id": 3, "name": "Carol", "active": true}
]';

SELECT * FROM read_json('variable:users')
WHERE active = true;
```

## Writing to Variables

Use `COPY ... TO` to write query results to a variable:

```sql
CREATE TABLE sales(product VARCHAR, amount INT, date DATE);
INSERT INTO sales VALUES
  ('Widget', 100, '2024-01-15'),
  ('Gadget', 200, '2024-01-16'),
  ('Widget', 150, '2024-01-17');

-- Export as CSV
COPY sales TO 'variable:sales_csv' (FORMAT csv, HEADER true);
SELECT getvariable('sales_csv');

-- Export as JSON
COPY sales TO 'variable:sales_json' (FORMAT json);
SELECT getvariable('sales_json');

-- Export filtered/aggregated data
COPY (
  SELECT product, SUM(amount) as total
  FROM sales
  GROUP BY product
) TO 'variable:summary' (FORMAT json);
```

### Write Formats

You can write in any format DuckDB's COPY supports:

| Format | Option | Example |
|--------|--------|---------|
| CSV | `FORMAT csv` | `COPY t TO 'variable:x' (FORMAT csv)` |
| CSV with header | `FORMAT csv, HEADER true` | `COPY t TO 'variable:x' (FORMAT csv, HEADER true)` |
| JSON | `FORMAT json` | `COPY t TO 'variable:x' (FORMAT json)` |
| Parquet | `FORMAT parquet` | `COPY t TO 'variable:x' (FORMAT parquet)` |

## Glob Pattern Matching

Match multiple variables using glob patterns:

### Asterisk (`*`) — Match Any Characters

```sql
SET VARIABLE config_dev = '{"env": "development"}';
SET VARIABLE config_staging = '{"env": "staging"}';
SET VARIABLE config_prod = '{"env": "production"}';

-- Match all config_* variables
SELECT * FROM read_json('variable:config_*');
```

### Question Mark (`?`) — Match Single Character

```sql
SET VARIABLE data_01 = 'a,b\n1,2';
SET VARIABLE data_02 = 'a,b\n3,4';
SET VARIABLE data_03 = 'a,b\n5,6';
SET VARIABLE data_100 = 'a,b\n7,8';

-- Match data_01, data_02, data_03 (NOT data_100)
SELECT * FROM read_csv('variable:data_??');
```

### Combined Patterns

```sql
SET VARIABLE log_2024_01 = '...';
SET VARIABLE log_2024_02 = '...';
SET VARIABLE log_2024_12 = '...';
SET VARIABLE log_2023_12 = '...';

-- Match all 2024 logs
SELECT * FROM read_text('variable:log_2024_*');

-- Match December logs from any year
SELECT * FROM read_text('variable:log_????_12');
```

### Results Are Sorted

Glob results are returned in alphabetical order for deterministic output:

```sql
SET VARIABLE z_first = '{"order": 1}';
SET VARIABLE a_second = '{"order": 2}';
SET VARIABLE m_third = '{"order": 3}';

SELECT * FROM read_json('variable:*_*');
-- Returns: a_second, m_third, z_first (alphabetically)
```

## Variable Types

### VARCHAR Variables

Standard string variables work directly:

```sql
SET VARIABLE text_data = 'hello world';
SELECT * FROM read_text('variable:text_data');
```

### BLOB Variables

Binary content is stored as BLOB type when written:

```sql
-- COPY automatically uses BLOB for binary formats
COPY my_table TO 'variable:parquet_data' (FORMAT parquet);

-- The variable is stored as BLOB internally
-- Reading works the same way
SELECT * FROM read_parquet('variable:parquet_data');
```

## Error Handling

### Variable Not Found

```sql
SELECT * FROM read_text('variable:nonexistent');
-- Error: Variable 'nonexistent' not found
```

### NULL Variable

```sql
SET VARIABLE nullable = NULL;
SELECT * FROM read_text('variable:nullable');
-- Error: Variable 'nullable' is NULL
```

### No Glob Matches

```sql
SELECT * FROM read_json('variable:xyz_*');
-- Error: No files found that match the pattern "variable:xyz_*"
```

## Best Practices

### Use Descriptive Names

```sql
-- Good: clear purpose
SET VARIABLE customer_report_2024_q1 = '...';

-- Avoid: unclear
SET VARIABLE data1 = '...';
```

### Use Prefixes for Related Variables

```sql
-- Group related variables with prefixes
SET VARIABLE config_database = '...';
SET VARIABLE config_cache = '...';
SET VARIABLE config_logging = '...';

-- Then use globs to access them
SELECT * FROM read_json('variable:config_*');
```

### Clean Up Temporary Variables

```sql
-- Use RESET to remove variables when done
RESET VARIABLE temp_data;
```
