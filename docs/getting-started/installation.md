# Installation

## From Source

### Prerequisites

- CMake 3.14+
- C++17 compiler (GCC 8+, Clang 7+, MSVC 2019+)
- Git

### Build Steps

```bash
# Clone with submodules
git clone --recurse-submodules https://github.com/your-repo/duckdb_scalarfs
cd duckdb_scalarfs

# Build
make

# Run tests (optional)
make test
```

### Build Outputs

After building, you'll have:

| File | Description |
|------|-------------|
| `build/release/duckdb` | DuckDB shell with extension loaded |
| `build/release/test/unittest` | Test runner |
| `build/release/extension/scalarfs/scalarfs.duckdb_extension` | Loadable extension file |

## Loading the Extension

### Using the Built-in Shell

The built `duckdb` binary already has scalarfs loaded:

```bash
./build/release/duckdb
```

```sql
-- Extension is already loaded, just use it
SELECT * FROM read_json('data+varchar:{"hello": "world"}');
```

### Loading into Standard DuckDB

Load the extension file into any DuckDB instance:

```sql
LOAD 'path/to/build/release/extension/scalarfs/scalarfs.duckdb_extension';

-- Now you can use scalarfs protocols
SELECT * FROM read_json('data+varchar:{"hello": "world"}');
```

### Unsigned Extensions

If you're loading an unsigned extension, you need to enable unsigned extensions:

=== "CLI"
    ```bash
    duckdb -unsigned
    ```

=== "Python"
    ```python
    import duckdb
    con = duckdb.connect(':memory:', config={'allow_unsigned_extensions': 'true'})
    con.execute("LOAD 'path/to/scalarfs.duckdb_extension'")
    ```

=== "Node.js"
    ```javascript
    const duckdb = require('duckdb');
    const db = new duckdb.Database(':memory:', {"allow_unsigned_extensions": "true"});
    ```

## Verifying Installation

After loading, verify the extension is working:

```sql
-- Test variable protocol
SET VARIABLE test = 'hello';
SELECT content FROM read_text('variable:test');
-- Should return: hello

-- Test data+varchar protocol
SELECT * FROM read_json('data+varchar:{"status": "ok"}');
-- Should return: status = "ok"
```

## Troubleshooting

### Extension Not Found

If you get "Extension not found" errors, check:

1. The path to the extension file is correct
2. The extension was built for your DuckDB version
3. You're using the correct architecture (x86_64, arm64, etc.)

### Unsigned Extension Error

If you get "unsigned extension" errors:

```sql
-- Enable unsigned extensions
SET allow_unsigned_extensions = true;
LOAD 'path/to/scalarfs.duckdb_extension';
```

Or start DuckDB with the `-unsigned` flag:

```bash
duckdb -unsigned
```
