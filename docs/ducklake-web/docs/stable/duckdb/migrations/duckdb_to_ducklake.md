---
layout: docu
title: DuckDB to DuckLake
---

Migrating from DuckDB to DuckLake is straightforward with the DuckDB `ducklake`
extension. If you use features that were historically
[unsupported in DuckLake]({% link docs/stable/duckdb/unsupported_features.md %}),
this guide covers both the full-copy path and a Python migrator that preserves
what this tree supports.

> **Fork note:** This page targets DuckLake builds that support fixed-size
> `ARRAY`, `ENUM`, STORED generated columns, and expression defaults. Older
> DuckLake targets may still need casts — use `--legacy-casts` on the migrator
> when available, or fall back to the cast behavior below.

## First Scenario: Everything Is Supported

If you are not using unsupported features, migrate with:

```sql
ATTACH 'ducklake:my_ducklake.ducklake' AS my_ducklake;
ATTACH 'db.duckdb' AS my_duckdb;

COPY FROM DATABASE my_duckdb TO my_ducklake;
```

The metadata catalog backend (DuckDB, PostgreSQL, SQLite, …) does not matter for
this path.

## Second Scenario: Partial Support

If the source DuckDB database uses types or table features that need special
handling, prefer the migrator script over a blind `COPY FROM DATABASE`.

### What this fork preserves

| Source DuckDB feature | Migrator behavior |
|---|---|
| Fixed-size arrays (`INTEGER[N]`, `VARCHAR[N]`, `FLOAT[N]`, …) | **Preserve** as DuckLake `array(N)` |
| `ENUM(...)` / named `CREATE TYPE … AS ENUM` | **Preserve** (emit `CREATE TYPE` then table DDL when named) |
| STORED generated columns | Prefer `CREATE TABLE … AS (expr)` then `INSERT` omitting generated columns |
| Non-literal defaults (`DEFAULT now()`, …) | Prefer `CREATE TABLE` with `DEFAULT expr`, then `INSERT` |
| Macros | DuckDB metadata catalog: migrate macros (DuckLake macros when available) |

### Still cast / skipped

| Source | Behavior |
|---|---|
| `UNION` | Cast → `VARCHAR` (or compatible text) |
| `VARINT` | Cast → integer / `VARCHAR` as needed |
| `BIT` / `BITSTRING` | Cast → `VARCHAR` |
| Virtual (non-STORED) generated columns | Bake values via CTAS fallback, or skip VIRTUAL definition |
| Enforced PK / FK / UNIQUE | Not created on DuckLake; data still copies |

See [Data Types]({% link docs/stable/specification/data_types.md %}) for
`array(N)` / `enum('…')` encodings and Postgres/SQLite inlining notes.

### Migration Script

Vendored copy in this repository:

`scripts/duckdb_to_ducklake_migrate.py`

(Ported into the published docs site alongside this page; keep the script and
CLI flags in sync with R1.)

> Currently, only local migrations are supported by this script. Remote object
> storage (S3 / GCS) destinations may be added later.

#### Usage (CLI)

```text
python scripts/duckdb_to_ducklake_migrate.py \
    --duckdb-catalog DUCKDB_CATALOG \
    --duckdb-file DUCKDB_FILE \
    --ducklake-catalog DUCKLAKE_CATALOG \
    --catalog-type {duckdb,postgresql,sqlite} \
    [--ducklake-file DUCKLAKE_FILE] \
    --ducklake-data-path DUCKLAKE_DATA_PATH \
    [--legacy-casts]
```

Example (DuckDB metadata catalog):

```bash
python scripts/duckdb_to_ducklake_migrate.py \
  --duckdb-catalog src \
  --duckdb-file ./app.duckdb \
  --ducklake-catalog lake \
  --catalog-type duckdb \
  --ducklake-file ./lake.ducklake \
  --ducklake-data-path ./lake_files/
```

PostgreSQL metadata catalog — set:

- `POSTGRES_HOST`
- `POSTGRES_PORT`
- `POSTGRES_DB`
- `POSTGRES_USER`
- `POSTGRES_PASSWORD`

#### Behavior summary

1. Attach source DuckDB + target DuckLake (secret / `METADATA_PATH` per
   `--catalog-type`).
2. Queue tables and views; retry failed views when dependencies are missing.
3. For tables:
   - Resolve columns via `information_schema` / `duckdb_columns()`.
   - Cast only `UNION` / `VARINT` / `BIT` (unless `--legacy-casts` also casts
     ARRAY/ENUM for older targets).
   - Prefer DDL that keeps ARRAY, ENUM, STORED generated, and expression
     defaults; `INSERT` physical non-generated columns from the source.
   - Fall back to `CREATE TABLE AS SELECT` with casts if DuckLake rejects an
     expression.
4. For DuckDB-catalog DuckLake, migrate macros into the metadata database
   (or DuckLake macros when supported).

#### Minimal Python sketch (preserves ARRAY/ENUM)

The full script lives at `scripts/duckdb_to_ducklake_migrate.py`. Core type
resolution differs from older upstream docs like this:

```python
import re

# Still cast (unsupported / low ROI)
CAST_ALWAYS = {
    "VARINT": "::VARCHAR::INT",  # or dialect-appropriate
    "BIT": "::VARCHAR",
}

def needs_cast(col_type: str, legacy_casts: bool = False) -> str | None:
    if re.match(r"UNION\b", col_type):
        return "::VARCHAR"
    if col_type in CAST_ALWAYS:
        return CAST_ALWAYS[col_type]
    if re.match(r"BIT\b", col_type):
        return "::VARCHAR"
    # Historical path (older DuckLake only):
    if legacy_casts:
        if re.match(r"ENUM\b", col_type):
            return "::VARCHAR"
        if re.fullmatch(r"(INTEGER|VARCHAR|FLOAT)\[\d+\]", col_type):
            base = re.match(r"(INTEGER|VARCHAR|FLOAT)", col_type).group(1)
            return f"::{base}[]"
    # Modern path: leave ARRAY / ENUM alone
    return None
```

Run the script in any Python environment with DuckDB installed.

## Related

- [Unsupported Features]({% link docs/stable/duckdb/unsupported_features.md %})
- [Data Types]({% link docs/stable/specification/data_types.md %})
- [Constraints]({% link docs/stable/duckdb/advanced_features/constraints.md %})
