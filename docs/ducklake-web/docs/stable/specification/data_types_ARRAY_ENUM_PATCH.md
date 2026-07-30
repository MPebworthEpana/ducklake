<!--
  PATCH for duckdb/ducklake-web → docs/stable/specification/data_types.md

  Insert into the **Nested Types** section (after the existing list/struct/map
  table). Also add a short inlining note under Type Encoding for Data Inlining
  if desired. Canonical fork source: docs/SPEC_DATA_TYPES.md
-->

## Snippet — Nested Types table rows

Add these rows to the nested-types table (alongside `list` / `struct` / `map`):

| Type | Description |
| -------- | --------------------------------------------- |
| `array(N)` | Fixed-length collection of `N` values with a single child type |
| `enum('label1', 'label2', …)` | Ordered set of string labels (no child columns; labels live in the type string) |

## Snippet — Nested Types body (insert after the table)

### Fixed-size array — `array(N)`

| Field | Value |
|---|---|
| `column_type` | `array(N)` where `N` is the fixed length |
| Child row | `column_name = 'element'`, `parent_column = <array column_id>` |
| Child type | Element DuckLake type string (e.g. `integer` / `int32`) |

Example:

```text
column_id=1  name=a  type=array(3)  parent=NULL
column_id=2  name=element  type=integer  parent=1
```

DuckDB SQL surface: `INTEGER[3]` / `a INTEGER[3]`.

### Enum — `enum('label1', 'label2', …)`

| Field | Value |
|---|---|
| `column_type` | `enum('a', 'b', …)` with ordered labels |
| Children | None (labels are encoded in the type string) |

Example:

```text
column_id=1  name=h  type=enum('world', 'hello')  parent=NULL
```

DuckDB SQL surface: `ENUM('world', 'hello')` or a named type from `CREATE TYPE`.

Named ENUM / STRUCT aliases may also be registered in formal
`ducklake_type` / `ducklake_type_member` tables (catalog version `1.1-dev6+`).
Older lakes may still store `CREATE TYPE` as schema tags `udt:<name>` on
`ducklake_tag`; both are dual-read on attach when supported.

## Snippet — Inlining note (Postgres / SQLite)

Add under **Type Encoding for Data Inlining** (or as a callout near nested types):

> **ARRAY / ENUM inlining:** DuckDB metadata catalogs may use native `T[N]` and
> ENUM types. Postgres and SQLite catalogs treat fixed-size `ARRAY` and `ENUM`
> as non-native and inline values as `VARCHAR` text (same pattern as other
> nested types on those backends).

## Statistics note (optional)

Under Nested Types statistics, mention that `array` follows the same pattern as
`list` (parent has no min/max; child `element` carries stats). `enum` values are
string labels for stats purposes when collected.
