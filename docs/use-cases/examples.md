# Use Cases & Examples

Real-world patterns for using scalarfs effectively.

## Inline Test Data

Quickly create test data without temporary files:

```sql
-- Test a query with sample data
SELECT department, AVG(salary) as avg_salary
FROM read_csv('data+varchar:name,department,salary
Alice,Engineering,95000
Bob,Marketing,75000
Carol,Engineering,105000
Dave,Sales,65000
Eve,Engineering,88000')
GROUP BY department
ORDER BY avg_salary DESC;
```

```
┌─────────────┬────────────────────┐
│ department  │     avg_salary     │
│   varchar   │       double       │
├─────────────┼────────────────────┤
│ Engineering │ 96000.0            │
│ Marketing   │ 75000.0            │
│ Sales       │ 65000.0            │
└─────────────┴────────────────────┘
```

## Configuration Management

Store and access configuration as JSON:

```sql
-- Define configuration
SET VARIABLE app_config = '{
  "database": {
    "host": "localhost",
    "port": 5432,
    "name": "production"
  },
  "cache": {
    "enabled": true,
    "ttl_seconds": 3600,
    "max_size_mb": 512
  },
  "features": {
    "dark_mode": true,
    "beta_features": false
  }
}';

-- Access nested configuration
SELECT
  config.database.host AS db_host,
  config.database.port AS db_port,
  config.cache.ttl_seconds AS cache_ttl
FROM read_json('variable:app_config') AS config;
```

### Environment-Specific Configs

```sql
-- Multiple environment configs
SET VARIABLE config_dev = '{"env": "development", "debug": true, "log_level": "DEBUG"}';
SET VARIABLE config_staging = '{"env": "staging", "debug": true, "log_level": "INFO"}';
SET VARIABLE config_prod = '{"env": "production", "debug": false, "log_level": "WARN"}';

-- Compare all configs
SELECT * FROM read_json('variable:config_*')
ORDER BY env;
```

## Data Pipeline Stages

Process data through multiple transformation stages:

```sql
-- Raw input data
SET VARIABLE raw_orders = '[
  {"id": 1, "customer": "Alice", "items": [{"sku": "A1", "qty": 2}], "status": "pending"},
  {"id": 2, "customer": "Bob", "items": [{"sku": "B2", "qty": 1}], "status": "shipped"},
  {"id": 3, "customer": "Carol", "items": [{"sku": "A1", "qty": 5}], "status": "pending"}
]';

-- Stage 1: Filter to pending orders
COPY (
  SELECT * FROM read_json('variable:raw_orders')
  WHERE status = 'pending'
) TO 'variable:pending_orders' (FORMAT json);

-- Stage 2: Expand items for analysis
COPY (
  SELECT
    o.id AS order_id,
    o.customer,
    unnest(o.items) AS item
  FROM read_json('variable:pending_orders') AS o
) TO 'variable:order_items' (FORMAT json);

-- Stage 3: Aggregate by SKU
COPY (
  SELECT
    item.sku,
    SUM(item.qty) AS total_qty,
    COUNT(*) AS order_count
  FROM read_json('variable:order_items')
  GROUP BY item.sku
) TO 'variable:sku_summary' (FORMAT json);

-- View final result
SELECT * FROM read_json('variable:sku_summary');
```

## Transparent Storage Facade

Store content paths that could point to any source:

```sql
CREATE TABLE documents(
  id INT,
  name VARCHAR,
  content_path VARCHAR,
  created_at TIMESTAMP
);

-- Small documents stored inline
INSERT INTO documents VALUES
  (1, 'config.json', 'data+varchar:{"version": "1.0"}', now()),
  (2, 'notes.txt', 'data+varchar:Meeting notes from Monday', now());

-- Large documents stored externally
INSERT INTO documents VALUES
  (3, 'report.json', '/data/reports/2024/q1.json', now()),
  (4, 'archive.json', 's3://bucket/archives/2023.json', now());

-- All work transparently!
SELECT
  name,
  length(content::VARCHAR) AS content_size
FROM documents,
     read_text(content_path) AS t(content);
```

### Storage Decision Macro

```sql
-- Auto-decide where to store based on size
CREATE MACRO store_content(content, external_path, threshold := 4096) AS (
  CASE
    WHEN octet_length(content) <= threshold
    THEN to_scalarfs_uri(content)
    ELSE external_path
  END
);

-- Use in inserts
INSERT INTO documents(id, name, content_path, created_at)
SELECT
  5,
  'auto.json',
  store_content(content, '/data/auto/5.json'),
  now()
FROM (SELECT '{"small": "data"}' AS content);
```

## API Response Caching

Cache API responses in variables:

```sql
-- Simulate API response storage
SET VARIABLE api_users = '[
  {"id": 1, "name": "Alice", "email": "alice@example.com"},
  {"id": 2, "name": "Bob", "email": "bob@example.com"}
]';

SET VARIABLE api_products = '[
  {"sku": "A1", "name": "Widget", "price": 9.99},
  {"sku": "B2", "name": "Gadget", "price": 19.99}
]';

-- Query cached data
SELECT u.name, p.name AS product, p.price
FROM read_json('variable:api_users') AS u
CROSS JOIN read_json('variable:api_products') AS p
WHERE u.id = 1;
```

## Report Generation

Generate and store reports:

```sql
-- Create sample data
CREATE TABLE sales(product VARCHAR, region VARCHAR, amount DECIMAL(10,2), date DATE);
INSERT INTO sales VALUES
  ('Widget', 'North', 1500.00, '2024-01-15'),
  ('Gadget', 'South', 2500.00, '2024-01-16'),
  ('Widget', 'North', 1800.00, '2024-01-17'),
  ('Gadget', 'North', 2200.00, '2024-01-18');

-- Generate regional summary
COPY (
  SELECT
    region,
    SUM(amount) AS total_sales,
    COUNT(*) AS transaction_count,
    AVG(amount) AS avg_transaction
  FROM sales
  GROUP BY region
) TO 'variable:regional_summary' (FORMAT json);

-- Generate product summary
COPY (
  SELECT
    product,
    SUM(amount) AS total_sales,
    MIN(date) AS first_sale,
    MAX(date) AS last_sale
  FROM sales
  GROUP BY product
) TO 'variable:product_summary' (FORMAT json);

-- Combine into final report
SELECT json_object(
  'generated_at', now()::VARCHAR,
  'by_region', (SELECT getvariable('regional_summary')),
  'by_product', (SELECT getvariable('product_summary'))
) AS full_report;
```

## Testing SQL Queries

Test queries with known data:

```sql
-- Test aggregation logic
WITH test_input AS (
  SELECT * FROM read_csv('data+varchar:category,value
A,10
A,20
B,30
B,40
B,50')
)
SELECT
  category,
  COUNT(*) AS cnt,
  SUM(value) AS total,
  AVG(value) AS avg_val
FROM test_input
GROUP BY category;

-- Verify result
-- A: cnt=2, total=30, avg=15
-- B: cnt=3, total=120, avg=40
```

## Data Validation

Validate data format before processing:

```sql
-- Test JSON structure
SELECT
  CASE
    WHEN json_valid(content) THEN 'Valid JSON'
    ELSE 'Invalid JSON'
  END AS status
FROM (SELECT from_varchar_uri('data+varchar:{"key": "value"}') AS content);

-- Validate CSV columns
SELECT column_count = 3 AS has_correct_columns
FROM (
  SELECT COUNT(*) AS column_count
  FROM read_csv('data+varchar:a,b,c
1,2,3', header=true)
  LIMIT 0
);
```

## Embedding in Applications

Generate scalarfs URIs in your application code:

### Python

```python
import duckdb

# Generate data in Python
data = [
    {"id": 1, "name": "Alice"},
    {"id": 2, "name": "Bob"}
]

# Convert to scalarfs URI
import json
uri = "data+varchar:" + json.dumps(data)

# Use in DuckDB
conn = duckdb.connect()
conn.execute("LOAD scalarfs")
result = conn.execute(f"SELECT * FROM read_json('{uri}')").fetchall()
```

### Node.js

```javascript
const duckdb = require('duckdb');

const data = JSON.stringify([
  {id: 1, name: "Alice"},
  {id: 2, name: "Bob"}
]);

const uri = `data+varchar:${data}`;

const db = new duckdb.Database(':memory:');
db.run("LOAD scalarfs", () => {
  db.all(`SELECT * FROM read_json('${uri}')`, (err, rows) => {
    console.log(rows);
  });
});
```

## See Also

- [Quick Start](../getting-started/quickstart.md) — Get started quickly
- [Protocol Overview](../protocols/overview.md) — Understand all protocols
- [Helper Functions](../functions/encoding.md) — Generate URIs programmatically
