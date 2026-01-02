# Limitations

Understanding the constraints and boundaries of scalarfs.

## Content Size

### Memory Constraints

All content is fully buffered in memory before reading:

```sql
-- This loads the entire variable content into memory
SELECT * FROM read_json('variable:large_data');
```

**Recommendations:**

- Keep inline content under a few MB
- For larger data, use traditional file storage
- Monitor memory usage with large variables

### VARCHAR Limits

`data+varchar:` content is limited by DuckDB's VARCHAR size limit (currently ~256MB uncompressed).

```sql
-- This might fail for very large content
SELECT * FROM read_text('data+varchar:' || repeat('x', 300000000));
```

## Streaming

scalarfs does **not** support streaming reads:

```sql
-- The entire content is loaded before any rows are returned
SELECT * FROM read_csv('variable:large_csv') LIMIT 10;
-- Still loads the entire variable first, then returns 10 rows
```

**Implications:**

- Memory usage equals content size
- Cannot process files larger than available memory
- No partial/lazy loading

## Write Support

Only `variable:` and `pathvariable:` support writing:

| Protocol | Read | Write |
|----------|------|-------|
| `variable:` | ✅ | ✅ |
| `pathvariable:` | ✅ | ✅ |
| `data:` | ✅ | ❌ |
| `data+varchar:` | ✅ | ❌ |
| `data+blob:` | ✅ | ❌ |

```sql
-- Works
COPY my_table TO 'variable:output' (FORMAT csv);

-- Works (writes to the file at the path stored in the variable)
SET VARIABLE out_path = '/tmp/output.csv';
COPY my_table TO 'pathvariable:out_path' (FORMAT csv);

-- Does NOT work
COPY my_table TO 'data+varchar:output' (FORMAT csv);
-- Error: Cannot write to data URI
```

## Binary Content

### data+varchar: Limitations

Cannot contain null bytes:

```sql
-- Null byte terminates the string
SET VARIABLE test = 'before' || chr(0) || 'after';
SELECT length(getvariable('test'));
-- Returns 6 (just "before"), not 12
```

**Solution:** Use `data+blob:` or `data:;base64,` for binary content.

### data+blob: Limitations

While `data+blob:` can represent any byte sequence, it's optimized for mostly-text content. Heavy binary content results in many escape sequences:

```sql
-- Text with occasional binary: efficient
SELECT to_blob_uri('normal text with ' || chr(0) || ' null');
-- data+blob:normal text with \0 null

-- Heavy binary: inefficient (use base64 instead)
SELECT length(to_blob_uri(chr(0) || chr(1) || chr(2) || chr(3)));
-- Much longer than base64 equivalent
```

## Pattern Matching

### Variable and PathVariable Protocols Only

Glob patterns only work with `variable:` and `pathvariable:`:

```sql
-- Works: variable protocol
SELECT * FROM read_json('variable:config_*');

-- Works: pathvariable protocol (two-level globs)
SET VARIABLE input_2024 = '/data/2024/*.csv';
SELECT * FROM read_csv('pathvariable:input_*');

-- Does NOT work (asterisk is literal)
SELECT * FROM read_json('data+varchar:*');
```

### pathvariable: Type Restrictions

The variable must contain a VARCHAR or BLOB value to be used as a path:

```sql
SET VARIABLE int_val = 42;
SELECT * FROM read_text('pathvariable:int_val');
-- Error: Variable 'int_val' must be VARCHAR or BLOB type
```

### Case Sensitivity

Variable names are case-insensitive (following DuckDB conventions):

```sql
SET VARIABLE MyVar = 'test';

-- All of these work
SELECT * FROM read_text('variable:MyVar');
SELECT * FROM read_text('variable:myvar');
SELECT * FROM read_text('variable:MYVAR');

-- Glob matching is also case-insensitive
SELECT * FROM read_text('variable:my*');  -- Matches MyVar
```

### No Bracket Patterns

Standard glob bracket patterns (`[abc]`) are not currently supported:

```sql
-- NOT supported
SELECT * FROM read_text('variable:config_[123]');

-- Use multiple patterns or asterisk instead
SELECT * FROM read_text('variable:config_?');
```

## Variable Lifetime

Variables exist only within the current session:

```sql
-- Set a variable
SET VARIABLE my_data = '...';

-- Close and reopen DuckDB
-- my_data is now gone!

SELECT * FROM read_text('variable:my_data');
-- Error: Variable 'my_data' not found
```

**For persistence:**

- Export to files before closing
- Use actual file storage for persistent data

## Concurrent Access

Variables are not designed for concurrent write access:

```sql
-- Thread 1:
COPY (SELECT ...) TO 'variable:shared' (FORMAT json);

-- Thread 2 (simultaneously):
COPY (SELECT ...) TO 'variable:shared' (FORMAT json);

-- Result is undefined - last write wins with possible corruption
```

**Recommendations:**

- Use unique variable names per thread/query
- Implement your own locking if needed

## Format Detection

File readers may not auto-detect format from scalarfs URIs:

```sql
-- File extension hints don't work
SELECT * FROM read_csv('variable:data.csv');
-- The .csv is part of the variable name, not a format hint!

-- Explicitly specify format when needed
SELECT * FROM read_csv('variable:data', header=true);
```

## Error Handling

### No Partial Failure

If any part of a glob pattern fails, the entire query fails:

```sql
SET VARIABLE config_valid = '{"ok": true}';
SET VARIABLE config_invalid = 'not json';

SELECT * FROM read_json('variable:config_*');
-- Error: entire query fails due to config_invalid
```

### Variable State

Errors during write leave variables in undefined state:

```sql
-- If COPY fails partway through...
COPY (SELECT * FROM huge_table) TO 'variable:output' (FORMAT json);

-- ...the variable might be partially written
SELECT length(getvariable('output'));
-- Might be incomplete data
```

## Workarounds

### Large Data

For data too large for in-memory processing:

```sql
-- 1. Write to a file instead
COPY my_table TO '/tmp/large_data.json' (FORMAT json);

-- 2. Use external storage
COPY my_table TO 's3://bucket/large_data.json' (FORMAT json);

-- 3. Process in chunks
COPY (SELECT * FROM my_table LIMIT 10000 OFFSET 0) TO 'variable:chunk_1' (FORMAT json);
COPY (SELECT * FROM my_table LIMIT 10000 OFFSET 10000) TO 'variable:chunk_2' (FORMAT json);
```

### Binary Content

For binary data, always use base64:

```sql
-- Convert binary to base64 URI
SELECT to_data_uri(my_binary_column) FROM my_table;

-- Read binary content
SELECT * FROM read_blob('data:;base64,AAEC...');
```

### Persistence

For persistent storage:

```sql
-- Export before closing session
COPY (SELECT getvariable('my_data')) TO '/path/to/backup.txt';

-- Or use traditional file-based storage
COPY my_table TO '/path/to/data.json' (FORMAT json);
```

## See Also

- [Protocol Overview](../protocols/overview.md) — Choose the right protocol
- [API Reference](api.md) — Complete function reference
