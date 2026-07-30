# Unsupported Features: What to Support Next

Prioritizes closing DuckDB → DuckLake gaps called out in:

- [Unsupported Features](https://ducklake.select/docs/stable/duckdb/unsupported_features)
- [DuckDB to DuckLake migration](https://ducklake.select/docs/stable/duckdb/migrations/duckdb_to_ducklake)
- [Constraints](https://ducklake.select/docs/stable/duckdb/advanced_features/constraints)
- [Data Types](https://ducklake.select/docs/stable/specification/data_types)

This plan is for **this fork** (`MPebworthEpana/ducklake`). It separates what the
**specification** must gain from what the **extension** can implement today, and
ranks work by migration impact vs. lakehouse-fit.

## Verdict

Support the **write-time / schema-portable** gaps first. Do **not** chase
enforced PK/FK/UNIQUE. Treat CHECK as optional metadata later.

| Priority | Feature | Spec change? | Why |
|---|---|---|---|
| **Done** | Non-literal column defaults (`now()`, etc.) | Already added (`default_value_type` / `default_value_dialect`) | Upstream [#571](https://github.com/duckdb/ducklake/pull/571); tests in `test/sql/default/default_expressions.test` |
| **Done** | `VARIANT`, macros in DuckLake catalog | Spec already has them | Older docs listed these; current tree supports both |
| **Done (U0)** | Expression-default completeness | Small | `ADD COLUMN … DEFAULT expr`, `UPDATE … SET DEFAULT` |
| **Done (U1)** | Fixed-size `ARRAY` | Yes (`array` nested type + size) | Stored as `array(N)` + child `element` |
| **Done (U2)** | `ENUM` (and STRUCT-alias UDTs) | Yes (type catalog / enum metadata) | Column ENUMs + persisted `CREATE TYPE` |
| **Done (U3)** | Stored generated columns | Tags / reuse expression-default fields | Constant + column-ref; evaluate on INSERT/UPDATE |
| **Done (U4)** | `DROP … CASCADE` for views/macros | No (catalog walk) | Drops dependent views; RESTRICT lists them |
| **Done (U5)** | Unenforced `CHECK` (optional) | Yes | Formal `ducklake_table_constraint` (+ `check_*` dual-write); optional `ducklake_enforce_checks` |
| **Skip** | Enforced PK / UNIQUE / FK | N/A | Prohibitive on lake data; use `MERGE INTO` |
| **Skip / cast** | `UNION`, `VARINT`, `BIT`, collations | N/A | Cast-on-migrate is fine; low ROI |

---

## Already Supported (Docs Lag)

Stable unsupported-features docs still list some items that this tree already handles:

1. **Non-literal defaults** — `CREATE TABLE t(ts TIMESTAMP DEFAULT now())` works.
   Metadata stores `default_value_type='expression'` + `default_value_dialect='duckdb'`.
2. **VARIANT** — `test/sql/types/variant.test`.
3. **Macros** — first-class DuckLake macros (`ducklake_macro*`), not only the old
   “create macro in `__ducklake_metadata_*`” workaround.

**U0** closed the ALTER / `SET DEFAULT` holes. **F1** added nested column
defaults for `STRUCT` / `LIST` / `MAP` / `ARRAY` (see
[`FOLLOWUP_FEATURES_PLAN.md`](FOLLOWUP_FEATURES_PLAN.md)).

---

## How to Fix (Cross-Cutting Approach)

Every feature below follows the same pattern:

```text
1. Spec      — add metadata columns / type names if engines besides DuckDB must interop
2. Persist   — write/read via DuckLakeMetadataManager (+ Postgres/SQLite/Quack paths)
3. Bind      — stop throwing NotImplemented in schema/table entry; bind expressions
4. Evaluate  — write-time only (insert/update/merge), never rewrite historical Parquet
5. Migrate   — update migration script to stop casting away newly supported features
6. Test      — flip test/sql/*/unsupported.test cases to positive tests; add catalog matrix
```

Hard rules:

- Prefer **write-time evaluation** over scan-time enforcement (lakehouse reality).
- Tag non-portable SQL with a **dialect** field (same design as views / expression defaults).
- Keep **NOT NULL** as the only enforced constraint unless a feature is explicitly
  unenforced metadata.
- Do not block branching/OCC work: new metadata must be branch-aware
  (`branch_id`) like other versioned tables.

---

## U0 — Expression-default completeness (P0)

### Problem

Core `DEFAULT now()` works, but migration and ALTER paths still hit gaps that force
application changes.

### Fix

| Gap | Approach |
|---|---|
| `ADD COLUMN … DEFAULT expr` | Allow only with explicit backfill literal / `NULL` for existing rows; store expression as ongoing default (same message already hints this — implement the two-step atomically) |
| `UPDATE … SET col = DEFAULT` | Resolve column default expression at bind/update time |
| Nested defaults | Start with constants only; expressions later |
| Typed literal casts | Track upstream [#1306](https://github.com/duckdb/ducklake/pull/1306) |

### Touch points

- `src/storage/ducklake_field_data.cpp` (`ExtractInitialValue`)
- `src/storage/ducklake_update.cpp` (`VALUE_DEFAULT`)
- `src/storage/ducklake_catalog.cpp` / `ducklake_table_entry.cpp` (default load/store)
- `test/sql/default/`

### Exit criteria

- `ADD COLUMN j INT DEFAULT random()` either works (existing rows NULL/literal) or has a
  single documented two-step path that migrations can automate.
- `UPDATE t SET ts = DEFAULT` works for expression defaults.

---

## U1 — Fixed-size `ARRAY` (P0/P1)

### Problem

`INT[3]` is rejected (`test/sql/types/unsupported.test`). Migration casts to `INT[]`,
losing length. Parquet and DuckDB both understand fixed-size arrays; DuckLakeTypes
simply has no `array` base type.

### Spec change

Add nested type:

| Type | Description |
|---|---|
| `array` | Fixed-length collection; child column `element`; size stored as column tag or `column_type` param e.g. `array(3)` |

Encoding options (pick one):

1. **`array(N)` in `column_type`** + child `element` (mirrors `decimal(P,S)`) — preferred.
2. Parent `list` + tag `array_size=N` — weaker typing, easier short-term hack.

### Extension fix

Much of the plumbing already exists:

- `DuckLakeFieldId::FieldIdFromType` has an `ARRAY` case.
- `DuckLakeTableEntry::ConvertColumn` has an `ARRAY` case.
- `DuckLakeUtil::ToSQLString` handles `ARRAY` values.

Missing pieces:

1. `DuckLakeTypes::ToString` / `FromString` — emit/parse `array(N)` (or equivalent).
2. Stats / promote-type / add_files type checks.
3. Inlining native support per catalog backend (fallback: VARCHAR text like other nested).
4. Flip `test/sql/types/unsupported.test`; add `test/sql/types/array.test`.

### Migration

Stop casting `INTEGER[N]` / `VARCHAR[N]` / `FLOAT[N]` when target DuckLake supports arrays.

### Exit criteria

`CREATE TABLE t(a INT[3]); INSERT …; SELECT` round-trips through Parquet + inlining
on DuckDB catalog; Postgres/SQLite catalogs at least via text fallback.

---

## U2 — ENUM + STRUCT-alias UDTs (P1)

### Problem

`CREATE TYPE` / `ENUM` / aliased STRUCTs fail:

- `DuckLakeSchemaEntry::CreateType` → not implemented
- `DuckLakeTypes::ToString` → “Unsupported user-defined type”
- Migration: `ENUM`/`UNION` → `VARCHAR`

### Reasonable scope

| Kind | Support? | Approach |
|---|---|---|
| `ENUM` | **Yes** | First-class: store as `varchar` physically **or** dictionary ints + metadata list of labels; expose as DuckDB ENUM on read |
| `CREATE TYPE … AS STRUCT(…)` | **Yes** | Catalog alias expanding to underlying STRUCT (no new physical type) |
| `CREATE TYPE … AS ENUM` | **Yes** | Same as ENUM |
| `UNION` | **No (cast)** | Keep migration → `VARCHAR` / variant; low demand |
| Arbitrary Domains | **No** | Out of scope |

### Spec change

New metadata (sketch):

```sql
-- ducklake_type: user-defined type registry (branch-scoped)
CREATE TABLE ducklake_type(
  type_id BIGINT,
  type_uuid UUID,
  begin_snapshot BIGINT,
  end_snapshot BIGINT,
  schema_id BIGINT,
  type_name VARCHAR,
  type_class VARCHAR,      -- 'enum' | 'struct_alias'
  physical_type VARCHAR,   -- e.g. 'varchar' or 'struct'
  dialect VARCHAR,
  branch_id BIGINT
);

CREATE TABLE ducklake_type_member(
  type_id BIGINT,
  member_index INTEGER,
  member_name VARCHAR,
  member_type VARCHAR      -- for struct fields; NULL for enum labels
);
```

### Extension fix

1. Implement `CreateType` / drop / list in schema entry.
2. Resolve aliases before `DuckLakeTypes::ToString`.
3. For ENUM: bind as DuckDB ENUM; persist labels; write Parquet as dictionary or VARCHAR.
4. Migration: preserve ENUMs when both sides are DuckLake-aware.

### Exit criteria

`CREATE TYPE mood AS ENUM ('happy','sad'); CREATE TABLE t(m mood);` survives detach/attach
and time travel.

---

## U3 — Stored generated columns (P1)

### Problem

`CREATE TABLE t(c0 INT AS (1), c1 INT)` fails. Migration guide says generated columns
must be materialized (no `VIRTUAL`).

### Reasonable scope

Support **STORED** only (DuckDB `AS (expr)` / VIRTUAL syntax is treated as materialize/store):

- Compute expression on INSERT (and recompute on UPDATE of base columns).
- Persist value in Parquet / inlined data like a normal column.
- Reject direct `UPDATE` of generated columns.
- Reject references to other generated columns (no cycles).

### Spec / metadata

- Constant generated: `default_value` expression + column tag `generated=<expr>` + table tag `generated:<col>=<expr>`
- Column-ref generated: column/table tags only (no default — ConstantBinder cannot bind column refs)

### Extension fix

1. `MaterializeGeneratedColumns` converts GENERATED → STANDARD physical columns with tags.
2. `PlanGeneratedColumnProjection` evaluates tagged expressions after defaults on INSERT/UPDATE.
3. `BindUpdateConstraints` rejects SET on generated columns; base-col UPDATE recomputes via projection.

### Exit criteria

`CREATE TABLE t(a INT, b INT AS (a+1)); INSERT INTO t(a) VALUES (1);` returns `b=2`
after checkpoint/reattach. `UPDATE t SET a = 10` recomputes `b`; `UPDATE t SET b = …` errors.

---

## U4 — `DROP … CASCADE` (P2)

### Problem

`DuckLakeSchemaEntry` throws `Cascade Drop not supported`. Docs note this is also a
DuckDB limitation in some paths, but DuckLake can still implement dependency drops for
views/macros it owns.

### Fix (no spec change)

1. Build dependents set (already partially present around macro/view enumeration).
2. On `CASCADE`, drop dependent views/macros in dependency order inside the same snapshot.
3. On `RESTRICT` (default), keep current error but list dependents.

### Exit criteria

Dropping a table with a dependent view succeeds under `CASCADE` and removes the view
from the catalog snapshot.

---

## U5 — Unenforced CHECK (P3, optional)

### Problem

Docs list CHECK as “likely,” while introduction/constraints pages say lakehouses don’t
support CHECK. Enforcing CHECK on distributed Parquet is expensive and racy.

### Reasonable approach

**Unenforced CHECK metadata only** (BigQuery-style):

- Store expression + dialect on the table.
- DuckDB extension **may** validate on its own writes (optional setting).
- Never validate foreign writers / existing files.
- Do **not** claim constraint integrity across engines.

### Spec sketch

```sql
CREATE TABLE ducklake_table_constraint(
  table_id BIGINT,
  constraint_id BIGINT,
  begin_snapshot BIGINT,
  end_snapshot BIGINT,
  constraint_type VARCHAR,  -- 'check'
  expression VARCHAR,
  dialect VARCHAR,
  enforced BOOLEAN,         -- always FALSE for v1
  branch_id BIGINT
);
```

### Exit criteria

`CHECK (i > 0)` survives dump/reload; inserts that violate it succeed unless
`ducklake_enforce_checks=true`.

---

## Explicit Non-Goals

| Feature | Why skip |
|---|---|
| Enforced PRIMARY KEY / UNIQUE / FOREIGN KEY | Full-table uniqueness checks don’t fit lake storage; docs already mark unlikely |
| `INSERT … ON CONFLICT` upserts | Use `MERGE INTO` |
| `UNION` type | Migration → `VARCHAR` is acceptable |
| `VARINT` / `BIT` | Cast to INT/VARCHAR |
| Collations / per-column compression | Storage/format concerns; keep rejected |
| Virtual generated columns | Would require scan-time compute + expression portability forever |

---

## Suggested Ship Order

```text
U0  expression-default holes          (small, unblocks migrations now)
U1  ARRAY type                        (spec + types.cpp + tests)
U2  ENUM + STRUCT-alias CREATE TYPE   (spec type catalog)
U3  STORED generated columns          (reuse expression defaults)
U4  DROP CASCADE                      (catalog only)
U5  unenforced CHECK                  (optional / later)
```

Parallelizable: **U4** anytime; **U1** independent of U2/U3; **U3** after U0.

---

## Migration Script Impact

Once U1–U3 land, update the Python migrator from the docs so it:

1. Preserves `T[N]` as ARRAY (no cast to list).
2. Preserves ENUMs (no `::VARCHAR`) when U2 exists.
3. Emits STORED generated columns instead of baking values only.
4. Keeps casting `UNION` / `VARINT` / `BIT`.
5. Continues two-step handling only for features still missing (e.g. virtual generated).

---

## Test Plan (per feature)

| Area | Tests |
|---|---|
| Types | Flip `test/sql/types/unsupported.test`; add positive array/enum/udt tests |
| Constraints | Flip CHECK cases only if U5 lands; keep PK/FK failing |
| Defaults | Extend `test/sql/default/default_expressions.test` |
| Generated | Replace `test/sql/general/generated_columns.test` error with STORED success |
| Cascade | New `test/sql/catalog/drop_cascade.test` |
| Catalogs | DuckDB + SQLite + Postgres matrix for metadata round-trip |
| Branching | New metadata tables must respect `branch_id` / lineage predicates |

---

## Implementation Notes in This Tree

| Location | Current behavior (post U0–U5 / F1–F3e) |
|---|---|
| `src/common/ducklake_types.cpp` | Serializes `array(N)` and `enum('…')`; see [`SPEC_DATA_TYPES.md`](SPEC_DATA_TYPES.md) |
| `src/storage/ducklake_schema_entry.cpp` | `CREATE TYPE` + `DROP … CASCADE` for dependent views |
| `src/storage/ducklake_table_entry.cpp` | Accepts CHECK + generated (materialized); still rejects PK/UNIQUE/FK |
| `src/storage/ducklake_field_data.cpp` | Nested + expression defaults supported; scan backfill uses constant `initial_default` only |
| `src/storage/ducklake_metadata_manager.cpp` | Expression defaults + formal `ducklake_type` / `ducklake_table_constraint` / generated_* (`1.1-dev6`) |
| `test/sql/types/unsupported.test` | Documents remaining rejects (`UNION`, collations, …) |
| `test/sql/constraints/unsupported.test` | Documents PK reject; CHECK create allowed (unenforced by default) |

---

## Recommendation

**U0–U5 and follow-ups F1–F3e are implemented on `main`.** Details:
[`FOLLOWUP_FEATURES_PLAN.md`](FOLLOWUP_FEATURES_PLAN.md) (status Done) and
[`SPEC_DATA_TYPES.md`](SPEC_DATA_TYPES.md). Never enforce PK/FK in DuckLake.
Still separate: virtual generated columns, Python DuckDB→DuckLake migrator
updates, and sunsetting legacy `udt:*` / `check_*` / `generated` dual-write tags.

---

## Implementation status (on `main`)

| ID | Status | Notes |
|---|---|---|
| **U0** | Done | `ADD COLUMN … DEFAULT expr` backfills NULL; `UPDATE … SET DEFAULT` resolves bound defaults |
| **U1** | Done | `array(N)` type + nested child `element`; postgres/sqlite inline as VARCHAR |
| **U2** | Done | Column ENUMs as `enum('…')`; `CREATE TYPE` via `ducklake_type` (+ `udt:*` dual-write) |
| **U3** | Done | Constant + column-ref generated columns; formal `generated_*` cols (+ tag dual-write) |
| **U4** | Done | `DROP TABLE/VIEW … CASCADE` drops dependent views; RESTRICT lists them |
| **U5** | Done | `ducklake_table_constraint` (+ `check_*` dual-write); `ducklake_enforce_checks` optional |
| **F1** | Done | Nested STRUCT/LIST/MAP/ARRAY column defaults |
| **F2** | Done | Write-time CHECK verification when `ducklake_enforce_checks=true` |
| **F3** | Done | Catalog matrix-ready tests + formal metadata tables (`1.1-dev6`) |

Tests: `default_expressions`, `nested_defaults`, `types/array`, `types/enum`,
`general/generated_columns`, `constraints/unsupported`, `constraints/check_enforce`,
`catalog/drop_cascade`, `catalog/formal_metadata`.
