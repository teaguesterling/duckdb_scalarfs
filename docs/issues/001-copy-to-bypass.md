# Issue #001: COPY TO Bypasses Custom FileSystem for Write Operations

## Summary

When using `COPY TO` with a data URI path (e.g., `COPY table TO 'data:,test'`), DuckDB creates a literal file on disk with that name instead of routing through our `DataURIFileSystem.OpenFile()`.

## Evidence

After running tests that included:
```sql
COPY (SELECT 1 as a) TO 'data:,test' (FORMAT csv);
COPY (SELECT 1 as a) TO 'data+varchar:test' (FORMAT csv);
```

Actual files were created on disk:
- `data:,test`
- `data+varchar:test`

## Expected Behavior

`DataURIFileSystem.OpenFile()` should be called with write flags, which would throw `IOException("Data URIs are read-only")`.

## Actual Behavior

- `OpenFile()` is not called for write operations
- `TryRemoveFile()` IS called (we implemented it to return `false`)
- A literal file is created on the local filesystem

## Investigation Notes

The COPY TO code path likely:
1. Calls `TryRemoveFile()` first (which we handle)
2. Then creates the file through a different mechanism that doesn't check `CanHandleFile()`

Possible causes:
- COPY may use `CreateFile()` or a direct path that bypasses `OpenFile()`
- The VirtualFileSystem dispatch may not cover all write operations
- There may be a separate write-path registration needed

## Workaround

For now, data URI protocols are documented as read-only. The `variable:` protocol (Phase 2) will properly support writes through a different mechanism.

## TODO

- [ ] Investigate DuckDB's COPY TO implementation
- [ ] Check if `CreateFile()` or similar needs to be overridden
- [ ] Determine if this is a DuckDB limitation or missing implementation
- [ ] Consider if write support for data URIs even makes sense (probably not)

## Priority

Low - data URIs are inherently read-only by design. This is more of a "defense in depth" issue to provide a better error message rather than creating spurious files.
