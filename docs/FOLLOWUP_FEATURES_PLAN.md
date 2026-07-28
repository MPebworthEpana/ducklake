# Follow-up Features Plan

Closes the three remaining adjuncts after U0–U5 in
[`UNSUPPORTED_FEATURES_PLAN.md`](UNSUPPORTED_FEATURES_PLAN.md):

1. **F1** — Nested expression defaults (`STRUCT` / `LIST` / `MAP` / `ARRAY`)
2. **F2** — Optional CHECK enforcement (`ducklake_enforce_checks`)
3. **F3** — Full catalog matrix + formal spec tables

This plan is for **this fork** (`MPebworthEpana/ducklake`). It prefers
**compat-preserving** schema evolution: keep reading today’s tag / type-string
encodings while new writes move to formal tables.

---

## Verdict

| ID | Feature | Spec change? | Priority | Why now |
|---|---|---|---|---|
| **F1** | Nested column defaults | No | P0 | Create-time reject + reload bug; blocks real nested schemas |
| **F2** | Optional CHECK enforcement | No (setting) | P1 | U5 exit criteria; tags already persist |
| **F3a** | Catalog matrix for U1–U5 tests | No | P1 | Prove Postgres/SQLite/Quack round-trip |
| **F3b** | Formalize `array` / `enum` in type strings | Docs + minor | P1 | Already encoded in `column_type`; spec lags |
| **F3c** | `ducklake_type` (+ members) | Yes | P2 | Replace `udt:*` schema tags |
| **F3d** | `ducklake_table_constraint` | Yes | P2 | Replace `check_*` table tags |
| **F3e** | Formal generated-column columns | Yes | P3 | Replace `generated` / `generated:*` tags |

Ship order: **F1 → F2 → F3a → F3b → F3c/F3d → F3e**.

Hard rules (same as U0–U5):

- Write-time evaluation only; never rewrite historical Parquet for expressions.
- Dialect-tag non-portable SQL (`duckdb` today).
- Branch-aware metadata (`branch_id`) for every new table/column.
- Dual-read old tags until a version bump sunsets them.

---

## Current State (baseline)

| Feature | Today | Gap |
|---|---|---|
| Scalar expression defaults | `ducklake_column.default_value` + `default_value_type` / `default_value_dialect` | Nested roots rejected / not reloaded |
| CHECK | Table tags `check_N` + `check_dialect_N` | No write-time enforcement setting |
| ARRAY | `column_type='array(N)'` + child `element` | Not in published type spec; tests not matrixed |
| Column ENUM | `column_type="enum('…')"` | Same |
| CREATE TYPE | Schema tags `udt:<name>` | No `ducklake_type` registry |
| Generated columns | Column/table tags `generated` / `generated:<col>` | No formal columns |
| Catalog matrix | `{DUCKLAKE_CONNECTION}` + `test/configs/{postgres,sqlite,quack}.json` | New U1–U3 tests hardcode DuckDB `.db` paths |

Backends: DuckDB (default), Postgres, SQLite, Quack — schema DDL in
`DuckLakeMetadataManager::GetCreateTableStatements()` / `MigrateV*`.

---

## F1 — Nested expression defaults (P0)

### Problem

Nested column defaults fail at create because DuckDB represents nested
“literals” as functions (`struct_pack`, `list_value`, `map`, …), not
`VALUE_CONSTANT`. Even foldable constants hit:

```text
NotImplementedException("Only constant default values are supported for STRUCT|LIST|ARRAY|MAP type")
```

in `DuckLakeFieldId::FieldIdFromType`
(`src/storage/ducklake_field_data.cpp`).

Separately, **persistence/reload is broken for nested roots**:

1. `ConvertColumn` / `GetColumnInfo` skip `ExtractDefaultValue` on nested roots.
2. `TransformColumnType` only loads defaults when `col.children.empty()` (leaves).
3. `ALTER … SET DEFAULT struct_pack(…)` can work in-txn, then disappears after commit/reattach.

### Scope

| Kind | Support? | Behavior |
|---|---|---|
| Foldable nested constant (`{'a':1}`, `[1,2]`, `MAP{…}`, `ARRAY[…]`) | **Yes** | Fold → store as `literal` + `initial_default` Value |
| Non-foldable nested expression (`list_value(random())`, …) | **Yes** | `expression` + `initial_default = NULL` (U0 backfill rule) |
| Column-ref defaults | **No** | Reject (generated-column territory) |
| Subquery / window defaults | **No** | Keep existing rejects |
| Struct *field* defaults (`ADD COLUMN col.k DEFAULT 42`) | Already works | Leave alone |

No metadata schema change — reuse `default_value` / `default_value_type` /
`default_value_dialect`.

### Fix

1. **`FieldIdFromType` (STRUCT/LIST/ARRAY/MAP arms)**  
   Remove the `VALUE_CONSTANT`-only guard. Accept any default that passes
   `ExtractDefaultExpression` (no subquery/window). Optionally try
   ConstantBinder + fold; if fold succeeds, treat as literal for
   `initial_default`.

2. **`ConvertColumn` / column info extraction**  
   Call `ExtractDefaultValue` on nested **roots** while still recursing
   children (children keep their own defaults).

3. **`TransformColumnType` (catalog load)**  
   Load `initial_default` / `default_value` / `default_value_type` for nested
   roots **before** building children — same as leaves.

4. **Scan / Parquet path**  
   Keep `CreateColumnFromFieldId` using only constant `initial_default`.
   Never attach write-time expressions as MultiFile scan defaults.

5. **Write path**  
   Existing `bound_defaults` / `VALUE_DEFAULT` / insert default projection
   should already evaluate nested roots once they survive bind+reload.

### Touch points

- `src/storage/ducklake_field_data.cpp` — accept + fold + extract
- `src/storage/ducklake_table_entry.cpp` — `ConvertColumn` / `GetColumnInfo`
- `src/storage/ducklake_catalog.cpp` — `TransformColumnType`
- `src/storage/ducklake_multi_file_reader.cpp` — confirm constant-only scan defaults
- `test/sql/default/` — new nested cases

### Exit criteria

- `CREATE TABLE t(s STRUCT(a INT) DEFAULT {'a': 7}); INSERT …; SELECT` works.
- Same for `LIST` / `MAP` / `ARRAY` foldable constants.
- Non-foldable nested expr: existing rows NULL on `ADD COLUMN`; new inserts evaluate.
- Detach / reattach / commit preserves nested defaults (fixes SET DEFAULT reload bug).
- Struct field defaults (`struct_field_default.test`) stay green.

### Tests

Add `test/sql/default/nested_defaults.test` (use `{DUCKLAKE_CONNECTION}`):

1. CREATE + INSERT omit-col for STRUCT/LIST/MAP/ARRAY constants.
2. Expression nested default + `ADD COLUMN` NULL backfill.
3. `ALTER … SET DEFAULT` + commit + reattach.
4. Reject column-ref nested default.

---

## F2 — Optional CHECK enforcement (P1)

### Problem

U5 stores unenforced CHECK as tags and allows violating inserts. The U5 exit
criteria also called for:

```sql
SET ducklake_enforce_checks = true;  -- violating writes fail
```

That setting does not exist.

### Current persistence (keep)

| Tag key | Value |
|---|---|
| `check_N` | Expression `ToString()` |
| `check_dialect_N` | `"duckdb"` |

Written in `CreateTableExtended`, flushed via `WriteNewTags`, reloaded in
`DuckLakeCatalog` into `CheckConstraint`. Formal
`ducklake_table_constraint` is **F3d**, not a blocker for F2.

### Fix

1. **Register setting** in `src/ducklake_extension.cpp`:

   ```cpp
   config.AddExtensionOption(
       "ducklake_enforce_checks",
       "Validate CHECK constraints on DuckLake writes from this engine",
       LogicalType::BOOLEAN, Value::BOOLEAN(false), nullptr, SetScope::GLOBAL);
   ```

2. **Evaluate on writes when true** (mirror DuckDB `VerifyCheckConstraint` /
   generated-column planning):

   | Path | Hook |
   |---|---|
   | INSERT | `DuckLakeCatalog::PlanInsert` and/or `DuckLakeInsert::Sink` |
   | UPDATE | `DuckLakeCatalog::PlanUpdate` |
   | MERGE | Shared insert/update sinks (`DuckLakeMergeInsert` / `MergeUpdate`) |

   Bind each CHECK expression over physical columns; on false/NULL-as-fail
   (match DuckDB CHECK semantics), throw `ConstraintException`.

3. **Do not validate** foreign writers, `add_files`, or existing Parquet
   (lakehouse rule).

4. **Dialect:** only enforce when `check_dialect_*` is empty/`duckdb` and
   the expression re-parses; skip unparseable tags (same as load).

### Touch points

- `src/ducklake_extension.cpp` — option registration
- `src/storage/ducklake_insert.cpp` / `ducklake_update.cpp` / merge ops — verify
- `src/storage/ducklake_catalog.cpp` — optional shared bind helper
- `test/sql/constraints/` — enforcement cases

### Exit criteria

- Default (`false`): violating INSERT/UPDATE/MERGE still succeeds; CHECK survives dump/reload.
- `SET ducklake_enforce_checks=true`: violating writes fail; valid writes succeed.
- Multi-CHECK tables; unparseable legacy tags ignored.
- PK/FK remain rejected at DDL.

### Tests

Extend `test/sql/constraints/unsupported.test` or add
`test/sql/constraints/check_enforce.test` with `{DUCKLAKE_CONNECTION}`.

---

## F3 — Catalog matrix + formal spec tables

Split into matrix work (no schema change) and formalization (schema + docs).

### F3a — Catalog matrix for existing features (P1)

#### Goal

U1–U5 features round-trip on **DuckDB + Postgres + SQLite** (+ Quack when
available) using the existing config-driven pattern.

#### Fix

1. Convert hardcoded DuckDB paths to env-based attach:

   | Test | Change |
   |---|---|
   | `test/sql/types/array.test` | `test-env DUCKLAKE_CONNECTION` + `ATTACH 'ducklake:{DUCKLAKE_CONNECTION}'` |
   | `test/sql/types/enum.test` | same |
   | `test/sql/general/generated_columns.test` | same |
   | New F1/F2 tests | start matrix-ready |

2. Fix inlining gaps on non-DuckDB catalogs:

   - ARRAY already falls back to VARCHAR text on Postgres/SQLite.
   - ENUM: mark non-native (`TypeIsNativelySupported` / `GetColumnType`) so
     inlining uses VARCHAR text, matching ARRAY.

3. Run under `test/configs/postgres.json`, `sqlite.json`, `quack.json`
   (skip Quack-only if environment lacks scanner).

#### Exit criteria

Array / enum / generated / CHECK / nested-default tests pass on at least
DuckDB + SQLite catalogs locally; Postgres in CI when available.

---

### F3b — Document `array(N)` and `enum(...)` in the type spec (P1)

#### Goal

Published data-types spec matches what the extension already writes into
`ducklake_column.column_type`.

#### Spec additions

| Encoding | Meaning |
|---|---|
| `array(N)` | Fixed-size array; child column `element` via `parent_column` |
| `enum('a','b',…)` | Ordered labels; physical storage as DuckDB ENUM / Parquet dict or VARCHAR |

No new tables required. Optionally note that Postgres/SQLite inlining may
store values as VARCHAR text.

#### Exit criteria

Spec PR / fork docs describe both encodings; extension tests remain the
source of truth for parse/emit in `DuckLakeTypes::{ToString,FromString}`.

---

### F3c — Formal `ducklake_type` registry (P2)

Replaces schema tags `udt:<name>` for `CREATE TYPE` ENUM/STRUCT.

#### Spec tables

```sql
CREATE TABLE ducklake_type(
  type_id BIGINT,
  type_uuid UUID,
  begin_snapshot BIGINT,
  end_snapshot BIGINT,
  schema_id BIGINT,
  type_name VARCHAR,
  type_class VARCHAR,      -- 'enum' | 'struct_alias'
  physical_type VARCHAR,   -- e.g. 'varchar' or 'struct(...)'
  dialect VARCHAR,
  branch_id BIGINT
);

CREATE TABLE ducklake_type_member(
  type_id BIGINT,
  member_index INTEGER,
  member_name VARCHAR,
  member_type VARCHAR      -- struct field type; NULL for enum labels
);
```

#### Compat strategy

1. **MigrateV15+** (or next free version): `CREATE TABLE` if not exists.
2. On attach / upgrade: copy live `udt:*` tags → type rows (idempotent).
3. New `CREATE TYPE` / `DROP TYPE`: write tables; optionally dual-write tags
   for one release.
4. Catalog load: prefer type table; fall back to `udt:*` tags.

#### Touch points

- `ducklake_metadata_manager.cpp` — DDL + migrate + CRUD
- Backend managers if SQL dialect differs (Postgres/SQLite)
- `ducklake_transaction*.cpp` / `ducklake_schema_entry.cpp` — create/drop
- `ducklake_catalog.cpp` — load order (table then tags)

#### Exit criteria

`CREATE TYPE` survives detach/reattach on DuckDB/SQLite/Postgres without
depending on `udt:*` for new catalogs; old lakes with only tags still load.

---

### F3d — Formal `ducklake_table_constraint` (P2)

Replaces `check_*` / `check_dialect_*` tags. Complements F2 (enforcement
reads constraints from catalog entry regardless of storage).

#### Spec table

```sql
CREATE TABLE ducklake_table_constraint(
  table_id BIGINT,
  constraint_id BIGINT,
  begin_snapshot BIGINT,
  end_snapshot BIGINT,
  constraint_type VARCHAR,  -- 'check'
  expression VARCHAR,
  dialect VARCHAR,
  enforced BOOLEAN,         -- metadata intent; DuckLake writes honor
                            -- ducklake_enforce_checks, not this alone
  branch_id BIGINT
);
```

#### Compat strategy

Same dual-read / migrate-from-tags pattern as F3c. New CREATE TABLE CHECK
writes formal rows; load merges tags + table.

#### Exit criteria

CHECK round-trips via the formal table on all backends; tag-only lakes still
reload; F2 enforcement still works.

---

### F3e — Formal generated-column metadata (P3)

Optional after F3c/F3d. Prefer adding columns on `ducklake_column`:

| Column | Purpose |
|---|---|
| `is_generated` BOOLEAN | Marks STORED generated column |
| `generated_expression` VARCHAR | Expression SQL |
| `generated_dialect` VARCHAR | e.g. `duckdb` |

Constants may continue to also use expression defaults. Migrate from column
tag `generated` + table tag `generated:<col>`.

#### Exit criteria

Generated columns reload without tags on new catalogs; U3 tests stay green
across the matrix.

---

## Suggested Ship Order

```text
F1   Nested defaults (accept + persist + reload)     extension-only
F2   ducklake_enforce_checks + write-time verify     extension-only
F3a  Matrix-enable array/enum/generated(+F1/F2) tests + ENUM inline fix
F3b  Spec docs for array(N) / enum('…')
F3c  ducklake_type (+ members) + udt:* migrate
F3d  ducklake_table_constraint + check_* migrate
F3e  generated columns on ducklake_column            optional polish
```

Parallelizable: **F2** anytime after U5; **F3a** anytime; **F3b** docs-only;
**F3c/F3d** after F3a so migrations are matrix-tested.

---

## Test Plan Summary

| Workstream | Tests |
|---|---|
| F1 | New `test/sql/default/nested_defaults.test` (matrix-ready) |
| F2 | `constraints/check_enforce.test` or extend `unsupported.test` |
| F3a | Convert `array` / `enum` / `generated_columns` to `{DUCKLAKE_CONNECTION}`; run postgres/sqlite/quack configs |
| F3c–e | Detach/reattach + tag-migration fixtures; query formal tables directly via `METADATA_CATALOG` |

---

## Non-Goals

| Item | Why |
|---|---|
| Enforced PK / UNIQUE / FK | Unchanged skip |
| Virtual generated columns | Scan-time forever; out of scope |
| Enforcing CHECK on foreign writers / `add_files` | Lakehouse reality |
| Breaking removal of tags in the same release as dual-write | Compat |
| Python DuckDB→DuckLake migrator updates | Separate follow-up once F3b+ land |

---

## Recommendation

Implement **F1 then F2** as small extension-only PRs (high user impact, no
spec version bump). Land **F3a** immediately after so nested defaults and
CHECK enforcement are proven on every catalog backend. Formal tables
(**F3c–F3e**) should ride a metadata version bump with dual-read of tags
and a clear sunset note — do not block F1/F2 on that work.
