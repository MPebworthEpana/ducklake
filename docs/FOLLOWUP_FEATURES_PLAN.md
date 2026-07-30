# Follow-up Features Plan

Closes the adjuncts after U0–U5 in
[`UNSUPPORTED_FEATURES_PLAN.md`](UNSUPPORTED_FEATURES_PLAN.md):

1. **F1–F3e** — Nested defaults, optional CHECK enforcement, catalog formalization
   (**implemented on `main`**)
2. **R1–R3** — Residual closeout: Python migrator, live catalog-matrix verification,
   upstream ducklake.select publish (**planned below; not yet done**)

This plan is for **this fork** (`MPebworthEpana/ducklake`). It prefers
**compat-preserving** schema evolution: keep reading today’s tag / type-string
encodings while new writes move to formal tables.

---

## Verdict

| ID | Feature | Spec change? | Priority | Status |
|---|---|---|---|---|
| **F1** | Nested column defaults | No | P0 | **Done** |
| **F2** | Optional CHECK enforcement | No (setting) | P1 | **Done** |
| **F3a** | Matrix-*ready* test attach paths + ENUM inline fix | No | P1 | **Done** (verification → **R2**) |
| **F3b** | Fork type encodings doc (`SPEC_DATA_TYPES.md`) | Docs | P1 | **Done** (upstream publish → **R3**) |
| **F3c** | `ducklake_type` (+ members) | Yes | P2 | **Done** |
| **F3d** | `ducklake_table_constraint` | Yes | P2 | **Done** |
| **F3e** | Formal generated-column columns | Yes | P3 | **Done** |
| **R1** | Python DuckDB→DuckLake migrator updates | Docs + script | P1 | **Done** |
| **R2** | Live Postgres/SQLite/Quack matrix verification | No | P1 | **Done (DuckDB; scanners TBD)** |
| **R3** | Upstream ducklake.select type/migration docs publish | Docs site | P1 | **Done (fork package)** |

Shipped: **F1 → F2 → F3a–F3e**. Remaining closeout: **R2 ∥ R1 → R3** (matrix can run in parallel with migrator; publish after fork docs + migrator script are ready).

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
| Breaking removal of tags in the same release as dual-write | Compat; sunset is optional later |

---

## Residual closeout (R1–R3)

F1–F3e shipped the extension features. These three leftovers still block
“migration + multi-catalog + public docs” completeness.

### R1 — Python DuckDB→DuckLake migrator (P1)

#### Problem

The public guide
[DuckDB to DuckLake](https://ducklake.select/docs/stable/duckdb/migrations/duckdb_to_ducklake)
embeds a Python script that still **casts away** features this fork now supports:

| Source DuckDB type / feature | Script today | Desired (this fork) |
|---|---|---|
| `INTEGER[N]` / `VARCHAR[N]` / `FLOAT[N]` | Cast → `T[]` (list) | Preserve as fixed-size `ARRAY` / `array(N)` |
| `ENUM(...)` / named ENUM | Cast → `VARCHAR` | Preserve ENUM (column type or `CREATE TYPE`) |
| `UNION` / `VARINT` / `BIT` | Cast → VARCHAR/INT | **Keep casting** (still skipped) |
| Generated columns / non-literal defaults | Values baked via `CREATE TABLE AS SELECT` | Prefer `CREATE TABLE` with `AS (expr)` / `DEFAULT expr`, then `INSERT` omitting generated/default cols where possible |
| Macros | Only when catalog_type=`duckdb` | Unchanged for PG/SQLite; for DuckDB catalog prefer DuckLake macros when available |

#### Source of truth

- **Upstream docs page:** `duckdb/ducklake-web` →
  `docs/stable/duckdb/migrations/duckdb_to_ducklake.md` (script is inline today).
- **Fork working copy (create):** `scripts/duckdb_to_ducklake_migrate.py` +
  `docs/ducklake-web/docs/stable/duckdb/migrations/duckdb_to_ducklake.md`
  (same port-package pattern as branching).

Vendoring the script in-repo lets us unit-test it before opening the
`ducklake-web` PR (R3).

#### Fix (task breakdown)

1. **Vendor** the current upstream script into
   `scripts/duckdb_to_ducklake_migrate.py` (byte-compatible CLI flags).
2. **ARRAY:** In `_resolve_data_types`, stop casting `T[N]` → `T[]`. Leave the
   column type alone so DuckLake receives fixed-size arrays.
3. **ENUM:** Stop matching `ENUM` into `::VARCHAR`. For anonymous
   `ENUM('a','b')` columns, create as-is. For named types
   (`CREATE TYPE mood AS ENUM ...`), emit `CREATE TYPE` on the DuckLake
   attachment before table create (query `duckdb_types` /
   `information_schema` for user types in the source catalog).
4. **Generated / expression defaults:** Detect via `duckdb_columns()`
   (`is_generated`, `generation_expression`, `column_default`). Prefer:
   ```sql
   CREATE TABLE dst (... , gen AS (expr), col DEFAULT expr, ...);
   INSERT INTO dst (physical_non_generated_cols) SELECT ... FROM src;
   ```
   Fall back to today’s CTAS bake-in only if expression bind fails on DuckLake.
5. **Keep** `UNION` / `VARINT` / `BIT` casts; keep macro path for DuckDB
   metadata catalogs (optionally try `CREATE MACRO` on DuckLake first).
6. **Tests:** `test/python/duckdb_to_ducklake_migrate.test` or a small
   `scripts/tests/test_duckdb_to_ducklake_migrate.py` that:
   - Builds a source DuckDB with ARRAY, ENUM, generated, UNION columns
   - Runs the script into a temp DuckLake (DuckDB catalog)
   - Asserts `typeof` / `SHOW CREATE` preserve ARRAY/ENUM/generated and still
     cast UNION
7. **Docs:** Update the fork `ducklake-web` migration page copy to say ARRAY /
   ENUM / STORED generated are preserved when the target DuckLake supports them
   (link [`SPEC_DATA_TYPES.md`](SPEC_DATA_TYPES.md)).

#### Exit criteria

- Script no longer casts `T[N]` or `ENUM` to list/VARCHAR.
- Generated columns survive as DuckLake generated (or documented fallback).
- Automated test covers the happy path on DuckDB-catalog DuckLake.
- Fork docs page matches script behavior (feeds R3).

#### Risks

- Named ENUM + STRUCT aliases need `CREATE TYPE` ordering (dependency queue
  already exists for views — extend for types).
- Older DuckLake targets without U1–U3 must keep the cast path behind a
  `--legacy-casts` flag or version probe (`ducklake_version` / try-create).

---

### R2 — Live catalog matrix verification (P1)

#### Problem

F3a made array/enum/generated/(+ F1/F2) tests **matrix-ready** via
`{DUCKLAKE_CONNECTION}`, but they were only proven on the **DuckDB** catalog.
CI workflow [`.github/workflows/Catalogs.yml`](../.github/workflows/Catalogs.yml)
already runs `test/sql/*` under Postgres and SQLite configs; confidence still
needs a recorded pass/fail + fixes for any new failures.

#### Scope

Target suites (minimum):

| Suite | Path |
|---|---|
| Nested defaults | `test/sql/default/nested_defaults.test` |
| CHECK enforce | `test/sql/constraints/check_enforce.test` |
| Formal metadata | `test/sql/catalog/formal_metadata.test` |
| ARRAY / ENUM / generated | `test/sql/types/array.test`, `enum.test`, `general/generated_columns.test` |
| Related smoke | `default_expressions`, `constraints/unsupported`, `drop_cascade` |

Backends: DuckDB (default), SQLite (`test/configs/sqlite.json`), Postgres
(`test/configs/postgres.json`), Quack (`scripts/run_quack_tests.py` /
`test/configs/quack.json` when scanner present).

#### Fix (task breakdown)

1. **Helper script** `scripts/run_unsupported_catalog_matrix.sh` (mirror
   [`scripts/run_branching_catalog_matrix.sh`](../scripts/run_branching_catalog_matrix.sh)):
   ```bash
   FILTERS=(
     test/sql/default/nested_defaults.test
     test/sql/constraints/check_enforce.test
     test/sql/catalog/formal_metadata.test
     test/sql/types/array.test
     test/sql/types/enum.test
     test/sql/general/generated_columns.test
   )
   # duckdb → sqlite.json → postgres.json (needs createdb ducklakedb)
   # optional quack.json
   ```
2. **Local baseline** with `BUILD=build/debug` (or release),
   `ENABLE_SQLITE_SCANNER=ON` / `ENABLE_POSTGRES_SCANNER=ON` as in Catalogs.yml.
3. **Triage failures** in priority order:
   - `METADATA_CATALOG 'xx'` assumptions on Postgres (attach/search_path)
   - Identifier / inlining VARCHAR fallbacks for ARRAY/ENUM (already coded;
     assert behavior, don’t expect native PG ENUM)
   - `formal_metadata.test` queries against `xx.ducklake_type` — ensure metadata
     catalog name resolves on each backend
4. **Skip policy:** add `skip_tests` entries in
   `test/configs/{postgres,sqlite,quack}.json` **only** with a tracked reason
   and issue/TODO; prefer fix.
5. **CI evidence:** either rely on green `Catalogs.yml` on the PR that closes
   R2, or add an optional job step that runs the helper filters first for a
   faster signal (same pattern discussed for branching in
   [`branching/NEAR_TERM_FOLLOWUPS.md`](branching/NEAR_TERM_FOLLOWUPS.md) F3).
6. **Record** a pass/fail table in this plan’s Implementation status (or a short
   `docs/CATALOG_MATRIX_UNSUPPORTED.md` note).

#### Exit criteria

- Helper script green locally for DuckDB + SQLite.
- Postgres green locally or in CI (`Catalogs.yml`) for the filter list.
- Quack: green, or explicitly skipped with reason if environment lacks scanner.
- No silent skips of the new suites without a logged reason.
- F3a status can be read as “verified”, not only “matrix-ready”.

#### Risks

- `formal_metadata.test` is DuckDB-metadata-catalog flavored (`METADATA_CATALOG
  'xx'`); may need a variant or config-specific expected relations.
- SQLite locking flakes — reuse existing `skip_error_messages` / skip lists.

---

### R3 — Upstream ducklake.select publish (P1)

#### Problem

Fork docs describe `array(N)` / `enum('…')` and formal tables in
[`SPEC_DATA_TYPES.md`](SPEC_DATA_TYPES.md), but the live site still omits them:

- [Data Types](https://ducklake.select/docs/stable/specification/data_types) —
  nested types list only `list` / `struct` / `map` (no `array`, no `enum`).
- [DuckDB → DuckLake migration](https://ducklake.select/docs/stable/duckdb/migrations/duckdb_to_ducklake)
  — still documents ENUM→VARCHAR and ARRAY→list casts; script outdated vs R1.
- Unsupported-features / constraints pages may still list shipped items as
  unsupported.

Publishing is an out-of-repo change to
[`duckdb/ducklake-web`](https://github.com/duckdb/ducklake-web) (same path as
branching’s [`docs/ducklake-web/`](ducklake-web/README.md) package).

#### Fix (task breakdown)

1. **Port package under** `docs/ducklake-web/` (extend the branching package):
   | Path in `ducklake-web` | Action |
   |---|---|
   | `docs/stable/specification/data_types.md` | **Edit** — add `array(N)` + `enum('…')` nested/UDT encodings from `SPEC_DATA_TYPES.md`; note PG/SQLite inlining VARCHAR fallback |
   | `docs/stable/duckdb/migrations/duckdb_to_ducklake.md` | **Edit** — replace script with R1 version; rewrite “casts” narrative |
   | `docs/stable/duckdb/unsupported_features.md` (or equivalent) | **Edit** — mark ARRAY/ENUM/generated/CHECK/CASCADE/defaults as supported where accurate; keep PK/FK/UNION skips |
   | `docs/stable/specification/` metadata pages | **Edit if needed** — document `ducklake_type`, `ducklake_table_constraint`, generated_* columns (`1.1-dev6` / note fork versioning) |
2. **Menu / cross-links:** only if new pages are added; type encodings likely
   edit in place.
3. **Tracking:** open/claim a `duckdb/ducklake-web` issue (Needs Documentation
   label workflow or manual), titled e.g.
   `[ducklake] Document array/enum types + updated DuckDB migrator`.
4. **Apply steps** (maintainer / fork with write access):
   ```bash
   git clone https://github.com/duckdb/ducklake-web.git
   # copy/edit files from docs/ducklake-web/ package
   ./scripts/lint.sh
   # open PR against duckdb/ducklake-web
   ```
5. After merge/publish, add live URL pointers in this repo’s
   [`docs/README.md`](README.md) and mark R3 Done here.
6. **Fork vs upstream versioning:** if `1.1-dev6` formal tables are
   fork-only, gate that section with “DuckLake ≥ 1.1-dev6 (fork)” or wait until
   upstream adopts the same migration — do **not** claim upstream catalog
   support the fork has not landed.

#### Exit criteria

- Live data-types page lists `array` / `enum` encodings.
- Live migration page + script preserve ARRAY/ENUM/generated (per R1).
- Unsupported-features page no longer lists shipped items as missing.
- In-repo port package matches what was submitted upstream.
- This plan marks R3 Done with the live URLs.

#### Risks

- Upstream may reject fork-only `1.1-dev6` tables — publish type-string
  encodings (`array(N)`, `enum`) first; formal tables in a follow-up PR.
- Dual sources of truth — keep `SPEC_DATA_TYPES.md` as fork canonical; web
  package is the publish snapshot (same as branching guide).

---

## Suggested residual ship order

```text
R2  Live matrix verification          (CI/local; parallelizable)
R1  Vendored migrator + tests + fork docs page
R3  ducklake-web port package + upstream PR
```

R2 does not depend on R1. R3 should include R1’s script/docs. Formal-table
spec paragraphs in R3 can ship after upstream agrees on `1.1-dev6` (or stay
fork-only).

---

## Recommendation

**F1–F3e and R1–R3 fork deliverables are implemented.** Remaining external steps:
rebuild with sqlite/postgres scanners and re-run the matrix; open/merge the
`duckdb/ducklake-web` PR from the port package. Never enforce PK/FK.
Optional later: sunset dual-write tags; virtual generated columns stay out of
scope.

---

## Implementation status

| ID | Status | Notes |
|---|---|---|
| **F1** | Done | Nested STRUCT/LIST/MAP/ARRAY defaults; `test/sql/default/nested_defaults.test` |
| **F2** | Done | `ducklake_enforce_checks`; INSERT/UPDATE/MERGE verification |
| **F3a** | Done (ready) | Tests use `{DUCKLAKE_CONNECTION}`; ENUM non-native on PG/SQLite — **live verify = R2** |
| **F3b** | Done (fork) | [`SPEC_DATA_TYPES.md`](SPEC_DATA_TYPES.md) — **upstream publish = R3** |
| **F3c** | Done | `ducklake_type` + members; dual-write `udt:*`; MigrateV15; `1.1-dev6` |
| **F3d** | Done | `ducklake_table_constraint`; dual-write `check_*` |
| **F3e** | Done | `ducklake_column.is_generated` / `generated_expression` / `generated_dialect` |
| **R1** | Done | `scripts/duckdb_to_ducklake_migrate.py` + `scripts/tests/`; ARRAY/ENUM/generated preserved; `--legacy-casts` opt-in; DuckLake py-integration skipped on ABI mismatch |
| **R2** | Done (DuckDB) | `scripts/run_unsupported_catalog_matrix.sh` + [`CATALOG_MATRIX_UNSUPPORTED.md`](CATALOG_MATRIX_UNSUPPORTED.md); DuckDB 9/9 PASS; PG/SQLite/Quack SKIP until scanners built |
| **R3** | Done (fork package) | `docs/ducklake-web/` types/migration/unsupported patches; live `duckdb/ducklake-web` PR still needs a maintainer |

Shipped tests: `nested_defaults`, `check_enforce`, `formal_metadata`, matrix-ready
`array` / `enum` / `generated_columns`; migrator unit tests via
`python3 scripts/tests/test_duckdb_to_ducklake_migrate.py -v`.
