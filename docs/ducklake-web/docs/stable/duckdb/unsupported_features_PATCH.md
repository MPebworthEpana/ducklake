<!--
  PATCH for duckdb/ducklake-web → docs/stable/duckdb/unsupported_features.md

  Apply by editing the "Likely to Be Supported in the Future" / supported
  narrative so shipped items are no longer listed as missing. Keep PK/FK,
  UNION, virtual generated, etc. as unsupported.
-->

## Replace / move out of “Likely to Be Supported in the Future”

The following are **now supported** in this DuckLake fork (and should be
documented as supported once upstreamed). Remove them from the “not supported”
list and optionally add a short “Recently supported” blurb.

### Now supported

| Feature | Notes |
|---|---|
| Fixed-size [`ARRAY`](https://duckdb.org/docs/current/sql/data_types/array) (`T[N]`) | Stored as `array(N)` + child `element`. See [Data Types]({% link docs/stable/specification/data_types.md %}). |
| [`ENUM`](https://duckdb.org/docs/current/sql/data_types/enum) (column + `CREATE TYPE … AS ENUM`) | Stored as `enum('…')`; named types via `ducklake_type` / tags. |
| Expression / non-literal defaults (`DEFAULT now()`, etc.) | Metadata: `default_value_type` / `default_value_dialect`. Nested foldable defaults also supported. |
| STORED [generated columns](https://duckdb.org/docs/current/sql/statements/create_table#generated-columns) | Write-time evaluation; persisted values. Formal `is_generated` / `generated_expression` / `generated_dialect` on `1.1-dev6+`. |
| `DROP … CASCADE` for dependent views / macros | Drops dependents; `RESTRICT` lists them. |
| Unenforced [`CHECK`](https://duckdb.org/docs/current/sql/constraints#check-constraint) constraints | Stored as metadata (`ducklake_table_constraint` / `check_*` tags). Optional write-time enforcement via `ducklake_enforce_checks`. |

### Suggested markdown for the live page

```markdown
## Supported (formerly listed as unsupported)

The following DuckDB features work with DuckLake:

- Fixed-size arrays (`INTEGER[3]`, etc.) — see [Data Types]({% link docs/stable/specification/data_types.md %}).
- `ENUM` types (anonymous and named via `CREATE TYPE`).
- Non-literal column defaults (`DEFAULT now()`, nested constant defaults, …).
- STORED generated columns (`col AS (expr)` / `GENERATED ALWAYS AS (…) STORED`).
- `DROP … CASCADE` for views and macros that depend on a dropped object.
- Unenforced `CHECK` constraints (metadata). Set `ducklake_enforce_checks` to
  verify CHECKs on INSERT / UPDATE / MERGE.

> Virtual generated columns (not STORED) remain unsupported.
```

## Keep under “Unsupported” / “Unlikely”

Do **not** mark these as supported:

| Feature | Status |
|---|---|
| Primary key / enforced UNIQUE / foreign keys | Unlikely (lakehouse cost); use [`MERGE INTO`]({% link docs/stable/duckdb/usage/upserting.md %}) |
| Indexes | Unsupported |
| Sequences | Unsupported |
| `UNION` type | Cast on migrate (typically → `VARCHAR`) |
| `VARINT` | Cast on migrate (typically → integer / `VARCHAR`) |
| `BIT` / `BITSTRING` | Cast on migrate (typically → `VARCHAR`) |
| Collations | Unsupported / cast |
| Virtual (non-STORED) generated columns | Unsupported |
| Enforced CHECK as a hard lake-wide invariant without the setting | Only optional via `ducklake_enforce_checks` |

## User-defined types wording

Replace a blanket “user defined types unsupported” with:

```markdown
- Arbitrary user-defined types beyond ENUM and STRUCT aliases remain limited.
  `CREATE TYPE … AS ENUM` and `CREATE TYPE … AS STRUCT(…)` are supported;
  other UDT shapes may still be rejected or require casting.
```
