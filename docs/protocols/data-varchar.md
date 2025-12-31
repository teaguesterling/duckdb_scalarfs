# data+varchar: Protocol

The `data+varchar:` protocol provides zero-overhead inline content embedding. Everything after the prefix is used as-is with no encoding or escaping.

## Syntax

```
data+varchar:<content>
```

The content after `data+varchar:` is used literally with no transformation.

## Basic Usage

```sql
-- Inline JSON
SELECT * FROM read_json('data+varchar:{"name": "Alice", "age": 30}');

-- Inline CSV
SELECT * FROM read_csv('data+varchar:name,score
Alice,95
Bob,87
Carol,92');
```

Output:
```
┌─────────┬───────┐
│  name   │ score │
│ varchar │ int64 │
├─────────┼───────┤
│ Alice   │    95 │
│ Bob     │    87 │
│ Carol   │    92 │
└─────────┴───────┘
```

## Multiline Content

SQL string literals naturally support multiline content:

```sql
SELECT * FROM read_csv('data+varchar:id,name,department,salary
1,Alice,Engineering,95000
2,Bob,Marketing,75000
3,Carol,Engineering,105000
4,Dave,Sales,65000
5,Eve,Engineering,88000');
```

## JSON Examples

### Simple Object

```sql
SELECT * FROM read_json('data+varchar:{"key": "value", "count": 42}');
```

### Array of Objects

```sql
SELECT * FROM read_json('data+varchar:[
  {"id": 1, "status": "active"},
  {"id": 2, "status": "pending"},
  {"id": 3, "status": "complete"}
]');
```

### Nested Objects

```sql
SELECT config.database.host, config.cache.enabled
FROM read_json('data+varchar:{
  "database": {"host": "localhost", "port": 5432},
  "cache": {"enabled": true, "ttl": 3600}
}') AS config;
```

## CSV Examples

### With Header

```sql
SELECT * FROM read_csv('data+varchar:name,email,role
Alice,alice@example.com,admin
Bob,bob@example.com,user');
```

### Without Header

```sql
SELECT * FROM read_csv('data+varchar:Alice,95
Bob,87
Carol,92', header=false, columns={'name': 'VARCHAR', 'score': 'INT'});
```

### With Custom Delimiter

```sql
SELECT * FROM read_csv('data+varchar:name|score|grade
Alice|95|A
Bob|87|B', delimiter='|');
```

## Overhead Comparison

`data+varchar:` has the lowest overhead of any scalarfs protocol:

| Protocol | Prefix Length | Encoding Overhead |
|----------|---------------|-------------------|
| `data+varchar:` | 13 bytes | 0% |
| `data+blob:` | 10 bytes | Variable (escapes) |
| `data:;base64,` | 13 bytes | ~33% |

For a 1000-byte content:

| Protocol | Total Size |
|----------|------------|
| `data+varchar:` | 1013 bytes |
| `data:;base64,` | ~1346 bytes |

## Limitations

### No Null Bytes

`data+varchar:` cannot contain null bytes (`\0`). Use `data+blob:` or `data:;base64,` for binary content:

```sql
-- This won't work as expected (null byte terminates string)
SELECT * FROM read_text('data+varchar:before' || chr(0) || 'after');

-- Use data+blob: instead
SELECT * FROM read_text('data+blob:before\0after');
```

### Content Must Be Valid VARCHAR

The content must be valid UTF-8 text (or your database encoding).

### Read-Only

Cannot write to `data+varchar:` URIs:

```sql
-- This doesn't work
COPY my_table TO 'data+varchar:output';  -- Error!

-- Use variable: for writing
COPY my_table TO 'variable:output' (FORMAT csv);
```

## When to Use data+varchar:

### Best For

- Quick inline test data
- Small JSON configurations
- CSV snippets for testing
- Any text content without special encoding needs

### Avoid When

- Content contains null bytes → use `data+blob:` or `data:;base64,`
- Content is binary → use `data:;base64,`
- You need to write data → use `variable:`
- Content comes from untrusted sources → validate first

## Generating data+varchar: URIs

### In SQL

```sql
SELECT to_varchar_uri('{"key": "value"}');
-- Returns: data+varchar:{"key": "value"}

-- Or use auto-selection
SELECT to_scalarfs_uri('simple text');
-- Returns: data+varchar:simple text
```

### In Your Application

Simply prepend `data+varchar:` to your content:

```python
content = '{"key": "value"}'
uri = 'data+varchar:' + content
```

## See Also

- [data: Protocol](data-uri.md) — For binary content with base64
- [data+blob: Protocol](data-blob.md) — For content with control characters
- [Encoding Functions](../functions/encoding.md) — Generate URIs programmatically
