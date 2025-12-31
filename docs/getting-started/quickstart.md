# Quick Start

This guide will get you up and running with scalarfs in just a few minutes.

## Your First Query

After [installing scalarfs](installation.md), try this simple example:

```sql
-- Read JSON from inline content
SELECT * FROM read_json('data+varchar:{"message": "Hello, scalarfs!"}');
```

Output:
```
┌─────────────────────┐
│       message       │
│       varchar       │
├─────────────────────┤
│ Hello, scalarfs!    │
└─────────────────────┘
```

## Reading from Variables

Store content in a DuckDB variable and read it as a file:

```sql
-- Store JSON in a variable
SET VARIABLE my_config = '{
  "database": "production",
  "debug": false,
  "max_connections": 100
}';

-- Read it as JSON
SELECT * FROM read_json('variable:my_config');
```

Output:
```
┌────────────┬─────────┬─────────────────┐
│  database  │  debug  │ max_connections │
│  varchar   │ boolean │      int64      │
├────────────┼─────────┼─────────────────┤
│ production │ false   │             100 │
└────────────┴─────────┴─────────────────┘
```

## Reading CSV Data

Inline CSV works great with `data+varchar:`:

```sql
SELECT * FROM read_csv('data+varchar:name,department,salary
Alice,Engineering,95000
Bob,Marketing,75000
Carol,Engineering,105000
Dave,Sales,65000');
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
│ Dave    │ Sales       │  65000 │
└─────────┴─────────────┴────────┘
```

## Writing to Variables

Export query results to a variable:

```sql
-- Create a table
CREATE TABLE employees(name VARCHAR, department VARCHAR, salary INT);
INSERT INTO employees VALUES
  ('Alice', 'Engineering', 95000),
  ('Bob', 'Marketing', 75000),
  ('Carol', 'Engineering', 105000);

-- Export to a variable as JSON
COPY (
  SELECT * FROM employees
  WHERE department = 'Engineering'
) TO 'variable:engineering_team' (FORMAT json);

-- Check the result
SELECT getvariable('engineering_team');
```

Output:
```
[{"name":"Alice","department":"Engineering","salary":95000},
 {"name":"Carol","department":"Engineering","salary":105000}]
```

## Using Glob Patterns

Match multiple variables with glob patterns:

```sql
-- Create several config variables
SET VARIABLE config_dev = '{"env": "development", "debug": true}';
SET VARIABLE config_staging = '{"env": "staging", "debug": true}';
SET VARIABLE config_prod = '{"env": "production", "debug": false}';

-- Read all at once
SELECT * FROM read_json('variable:config_*');
```

Output:
```
┌─────────────┬─────────┐
│     env     │  debug  │
│   varchar   │ boolean │
├─────────────┼─────────┤
│ development │ true    │
│ production  │ false   │
│ staging     │ true    │
└─────────────┴─────────┘
```

## Using Helper Functions

Convert content to URIs programmatically:

```sql
-- Encode content to different URI formats
SELECT to_varchar_uri('{"key": "value"}') AS varchar_uri;
-- data+varchar:{"key": "value"}

SELECT to_data_uri('{"key": "value"}') AS base64_uri;
-- data:;base64,eyJrZXkiOiAidmFsdWUifQ==

-- Decode URIs back to content
SELECT from_scalarfs_uri('data+varchar:hello world') AS decoded;
-- hello world
```

## Next Steps

Now that you've seen the basics, explore:

- [Protocol Overview](../protocols/overview.md) — Learn about all supported protocols
- [Variable Protocol](../protocols/variable.md) — Deep dive into variable: features
- [Use Cases](../use-cases/examples.md) — Real-world usage patterns
- [Helper Functions](../functions/encoding.md) — Programmatic URI handling
