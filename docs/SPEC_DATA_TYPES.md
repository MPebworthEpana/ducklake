# DuckLake Type Encodings (Extension Spec)

Documents `column_type` encodings this fork already persists in
`ducklake_column.column_type`. These match the extension parse/emit paths in
`DuckLakeTypes::{ToString,FromString}` and nest via `parent_column`.

Published ducklake.select data-types docs may lag; this file is the fork source
of truth until upstream catches up.

---

## Fixed-size array — `array(N)`

| Field | Value |
|---|---|
| `column_type` | `array(N)` where `N` is the fixed length |
| Child row | `column_name = 'element'`, `parent_column = <array column_id>` |
| Child type | Element DuckLake type string (e.g. `integer`) |

Example:

```text
column_id=1  name=a  type=array(3)  parent=NULL
column_id=2  name=element  type=integer  parent=1
```

DuckDB SQL surface: `INTEGER[3]` / `a INTEGER[3]`.

**Inlining:** DuckDB metadata catalogs may use native `T[N]`. Postgres/SQLite
catalogs treat ARRAY as non-native and inline values as VARCHAR text.

---

## Enum — `enum('label1', 'label2', …)`

| Field | Value |
|---|---|
| `column_type` | `enum('a', 'b', …)` with ordered labels |
| Children | None (labels are encoded in the type string) |

Example:

```text
column_id=1  name=h  type=enum('world', 'hello')  parent=NULL
```

DuckDB SQL surface: `ENUM('world', 'hello')` or a named type from `CREATE TYPE`.

**Named ENUM / STRUCT aliases:** Prefer formal `ducklake_type` /
`ducklake_type_member` (catalog version `1.1-dev6+`). Older lakes may still
store `CREATE TYPE` as schema tags `udt:<name>` on `ducklake_tag`; both are
dual-read on attach.

**Inlining:** Postgres/SQLite catalogs treat ENUM as non-native and inline as
VARCHAR text.

---

## Related formal tables (`1.1-dev6+`)

| Table / columns | Purpose |
|---|---|
| `ducklake_type` + `ducklake_type_member` | User-defined ENUM / STRUCT aliases |
| `ducklake_table_constraint` | Unenforced CHECK metadata |
| `ducklake_column.is_generated` / `generated_expression` / `generated_dialect` | STORED generated columns |

See [`FOLLOWUP_FEATURES_PLAN.md`](FOLLOWUP_FEATURES_PLAN.md) for migration and
dual-write/dual-read rules.
