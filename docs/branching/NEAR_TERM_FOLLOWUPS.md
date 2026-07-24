# Near-Term Product Follow-Ups (Post Phase 5)

Plans the remaining **product** work after Phase 5 (T1–T6) shipped: broader
cherry-pick/transplant, publishing the user guide, catalog-backend CI confidence for
branching, and leftover hardening gates. Intentionally excludes the Phase 5
**non-goals** (rebase-as-command, conflict-resolution UI, row/cell merge, per-ref ACL).

Derived from:

- [`PHASE_5_PARITY_EXTRAS.md`](PHASE_5_PARITY_EXTRAS.md) risks / deferred scope
- [`HARDENING_P2_P4_GAPS.md`](HARDENING_P2_P4_GAPS.md) “Remaining open items”
- User guide limitations in [`docs/guides/branching.md`](../guides/branching.md)
- Current fail-closed gate in `CherryPickUnsupportedReason`
  (`src/storage/ducklake_metadata_manager.cpp`)

## Goal

Close the gap between “Nessie-parity surface exists” and “safe to use for the common
branch → review → merge / hot-fix workflows on production catalogs,” without opening
new architectural fronts.

## Prerequisites

- Phase 5 merged (cherry-pick, transplant, diff, ref-aware `table_changes`,
  `ref_history`, snapshots provenance, user guide).
- H1–H3 hardening merged (tombstones, unified `DetectConflicts`, shared inlining,
  reachability GC).

## Workstreams

| ID | Workstream | Priority | Risk | Depends on |
|---|---|---|---|---|
| **F1** | Broader cherry-pick / transplant | P0 | High | P5 apply helpers |
| **F2** | Publish Branching guide on ducklake.select | P1 | Low | Guide file already in-repo |
| **F3** | Branching suite confidence on Postgres/SQLite catalogs | P1 | Medium | Existing `Catalogs.yml` |
| **F4** | Hardening leftovers (admin gates, sibling convert, optional vacuum) | P2 | Medium | H1–H3 |

Suggested ship order: **F2** (docs, parallel) ∥ **F3** (CI triage) → **F1** (main code) →
**F4** (admin/ops polish; F4.3 optional/deferred).

---

## F1 — Broader cherry-pick / transplant

### Problem

`ducklake_cherry_pick` / `ducklake_transplant` only apply snapshots whose
`changes_made` is DML **data-file** inserts/deletes. Anything else fails closed via
`CherryPickUnsupportedReason`:

| Rejected today | Examples in `changes_made` |
|---|---|
| DDL | `created_table`, `altered_table`, `dropped_*`, schema/view/macro creates |
| Inlined data | `inlined_insert`, `inlined_delete`, `flushed_inlined` |
| Compaction | `compacted_table`, `merge_adjacent`, `rewrite_delete` |

Apply path today (`ApplyCherryPickSnapshot` + file-id remaps) copies
`ducklake_data_file` / `ducklake_delete_file` (+ related stats) onto the target branch.
Dependency validation (`ValidateCherryPickDependencies`) already enforces that deletes
target files visible on the target (or created earlier in the same transplant range).

### Scope

Extend selective apply **incrementally**, keeping fail-closed for anything not yet
supported. Prefer reusing merge/re-own SQL patterns over inventing a second rewriter.

#### F1.1 — Inlined DML (first code slice)

Highest user pain when `DATA_INLINING_ROW_LIMIT > 0`.

- Copy inlined insert/delete rows for the picked snapshot(s) onto the target branch,
  respecting `inlining_layout` (`shared_table` + `branch_id` vs `per_branch_table`).
- Remap / allocate inlined row identities as needed; keep tombstone / delete semantics
  consistent with branch visibility.
- Extend conflict + dependency checks for inlined tables touched by the Δ.
- Tests: pick/transplant with inlining enabled; dry_run; conflict with concurrent
  inlined delete on target.

**Status.** Shipped: `ApplyCherryPickInlinedData` +
`test/sql/branching/cherry_pick_inlined.test`.

#### F1.2 — Compose-clean DDL

Apply schema/table/view/macro creates, alters, and drops that the merge conflict
taxonomy already understands.

- Order matters: create schema → create table → DML → alter → drop (within and across
  a transplant range).
- UUID / `table_id` stability: cherry-pick must not invent colliding ids on the target;
  prefer copying source object ids when the object is new to the target, else fail closed
  with a clear dependency message (same spirit as missing parent table today).
- Cross-snapshot dependencies inside a transplant range must remain all-or-nothing.
- Tests: create table on source then insert (range transplant); alter column then DML;
  drop table only if not referenced on target; conflict when target altered the same
  table.

**Status.** Shipped: compose-clean DDL cherry-pick/transplant for CREATE/DROP
SCHEMA/TABLE/VIEW/MACRO and ALTER TABLE column ADD/DROP/RENAME, plus rename-as-create
rows for tables/views. Covered by `cherry_pick_create_table.test`,
`cherry_pick_inlined.test`, and `cherry_pick_ddl.test`. Still fail-closed for
flushed-inlined and compaction snapshots.

#### F1.3 — Compaction snapshots (optional / later within F1)

Compaction rewrites files without user-visible row identity changes but reshapes
file/delete metadata. Treat as **opt-in second phase** after F1.1–F1.2:

- Either replay compaction as “replace file set X with Y” on the target when inputs are
  visible, or continue rejecting compaction picks with a sharper error (“compact on the
  target instead”).
- Default recommendation: **keep reject** until there is a concrete user demand;
  document the workaround (merge the branch, or re-run compaction on target).

**Decision.** Keep reject. Workaround: merge the branch, or compact on the target.

### Non-goals for F1

- Rebase command.
- Automatic conflict resolution.
- Cherry-picking merge commits as opaque units (users should pick the underlying Δs or
  merge the branch).

### Task breakdown

1. **Inventory apply surface** — table of `SnapshotChangeInformation` fields → current
   apply / reject / needed SQL (living checklist in this doc or a short comment above
   `CherryPickUnsupportedReason`).
2. **F1.1 inlined apply** + tests under both inlining layouts.
3. **F1.2 DDL apply** ordered by dependency; extend
   `ValidateCherryPickDependencies` for schema objects.
4. **Error UX** — keep fail-closed messages specific (`contains DDL changes: altered_table`
   vs generic “unsupported”).
5. **Docs** — update [`guides/branching.md`](../guides/branching.md) limitations +
   examples once each slice lands.
6. **Decision gate for F1.3** — ship or explicitly defer compaction picks.

### Acceptance

- Snapshots that only contain supported change classes apply with P4-grade conflict
  safety and existing provenance (`cherry_pick_*` / `transplant_*`).
- Unsupported classes still fail closed with actionable errors.
- `test/sql/branching/cherry_pick*.test` and `transplant*.test` cover inlined + DDL
  happy paths and at least one conflict path each.
- Guide limitations section matches shipped capability.

### Risks

- DDL id/UUID collisions across branches; must fail closed rather than silently fork
  identity.
- Inlined apply differs by layout; shared-table row `branch_id` re-own vs per-branch
  physical tables.
- Transplant cumulative remaps grow more complex once DDL creates objects mid-range.

---

## F2 — Publish Branching guide on ducklake.select

### Problem

[`docs/guides/branching.md`](../guides/branching.md) lives in this repo but is not yet
on the [ducklake.select](https://ducklake.select) docs site (Guides / Advanced Features).

### Scope

- Port the guide into **`duckdb/ducklake-web`** (or the current docs source of truth)
  under Guides or Advanced Features, matching site tone and nav.
- Fix cross-links to existing pages (Expire Snapshots, Cleanup, Time Travel, Data
  Change Feed, Migrations).
- Optionally add a short “Branching” blurb on the DuckDB Extension Introduction /
  Snapshots pages pointing at the guide.
- Use the existing **Needs Documentation** label workflow
  (`.github/workflows/NeedsDocumentation.yml` → mirror issue on `ducklake-web`) if that
  is the team’s preferred entry path.

### Task breakdown

1. Open / claim the `ducklake-web` docs issue (Needs Documentation on the T6 PR, or a
   dedicated issue).
2. Port markdown; adjust headings/anchors to site conventions.
3. Add nav entry; preview build.
4. After publish, add a one-line pointer in this repo’s
   [`docs/README.md`](../README.md) to the live URL (keep the in-repo guide as the
   editable source or clearly mark which is canonical).

### Acceptance

- Guide reachable from ducklake.select docs nav.
- Examples match macros shipped on `main`.
- No stale “DML-only cherry-pick” wording once F1 slices land (update in lockstep).

**Prep note.** In-repo guide points operators at the Needs Documentation /
`ducklake-web` publish path; cherry-pick limitations match F1.1–F1.2 (inlined DML +
compose-clean DDL supported; flushed-inlined/compaction still not). Publishing the live
ducklake.select page remains an out-of-repo `ducklake-web` change.

### Risks

- Dual sources of truth (repo vs website) drifting — pick a canonical home and link the
  other.

---

## F3 — Branching suite on Postgres / SQLite catalogs

### Problem

`Catalogs.yml` already runs:

```text
unittest --test-config test/configs/{sqlite,postgres}.json --test-dir ./ "test/sql/*"
```

so branching tests are *in principle* covered. Near-term work is **confidence and
triage**, not inventing a new CI workflow from scratch.

### Scope

- Run and record `test/sql/branching/*` + `test/sql/refs/*` under Postgres and SQLite
  configs locally and in CI.
- Fix failures specific to identifier limits, locking, SQL dialect in metadata queries,
  or temp-table assumptions in cherry-pick apply maps.
- Add `skip_tests` entries only with a tracked reason (prefer fix over skip).
- Optional: add a CI job step that prints a dedicated branching summary, or a narrow
  filter job for faster signal on branching PRs (`test/sql/branching/*` +
  `test/sql/refs/*`) without waiting for the full `test/sql/*` matrix.

### Task breakdown

1. **Baseline** — capture pass/fail matrix (DuckDB / SQLite / Postgres) for branching +
   refs suites.
2. **Fix or skip** each failure with root cause notes.
3. **CI signal** — optional dedicated step/job for branching filters on PR paths under
   `src/**/ducklake_*branch*`, `src/**/ducklake_*ref*`, `test/sql/branching/**`.
4. Document known backend caveats in the user guide (only if user-visible).

### Acceptance

- Branching + refs suites green on DuckDB, SQLite, and Postgres metadata (or explicitly
  skipped with justified `skip_tests` entries).
- No silent bitrot: CI fails when a previously green branching test regresses on those
  backends.

### Baseline (this follow-ups pass)

| Backend | How covered | Local note |
|---|---|---|
| DuckDB catalog | `unittest "test/sql/branching/*"` (+ refs) | Primary day-to-day signal — **green** |
| SQLite catalog | `test/configs/sqlite.json` + branching/refs | **green** (587 + 50 assertions); needs `ENABLE_SQLITE_SCANNER=ON` |
| Postgres catalog | `test/configs/postgres.json` + branching/refs | **green** (597 + 50 assertions); needs `ENABLE_POSTGRES_SCANNER=ON` + Postgres |

Local helper: `scripts/run_branching_catalog_matrix.sh` (set `BUILD=build/debug` or release).

No branching/refs paths are listed under unjustified `skip_tests`. Catalog-specific smoke tests:

- `test/sql/branching/sqlite_catalog_main_schema.test` (skipped on Postgres config)
- `test/sql/branching/postgres_catalog_main_schema.test` (requires `DUCKLAKE_CI`; skipped on SQLite config)

**Fixes landed with this matrix**

1. **SQLite `branch_id` DEFAULT portability** — sqlite_scanner left `ADD COLUMN … DEFAULT 0` as NULL, so lineage visibility hid `main`. Fixed via explicit `branch_id=0` on init, MigrateV12 backfill, and catalog version **1.1-dev5** (`MigrateV14`) NULL→0 backfill; lineage predicates also `COALESCE(branch_id, 0)`.
2. **Postgres commit placeholders** — `PostgresMetadataManager::ExecuteQuery` now runs `SubstituteSnapshotPlaceholders` (was dropping `{BRANCH_ID*}`).
3. **Portable fail-closed raises** — DuckDB `error()` is not valid in native Postgres SQL; `{RAISE_ON_ROWS_*}` expands to `error()` for DuckDB execution and PL/pgSQL `RAISE` for `postgres_execute`.

CI `Catalogs.yml` remains the full `test/sql/*` gate.

### Risks

- Postgres locking / aborted-transaction behavior (already seen in other suites).
- Long identifiers / inlining interactions under Postgres’s identifier limit.
- Cherry-pick temp mapping tables (`__ducklake_cherry_pick_*`) portability across
  catalog engines.

---

## F4 — Hardening leftovers

From [`HARDENING_P2_P4_GAPS.md`](HARDENING_P2_P4_GAPS.md) remaining open items and
related ops polish.

### F4.1 — Admin gate for destructive / policy options

**Problem.** `merge_tombstone_mode` and `inlining_layout` / `convert_inlining_layout`
are powerful catalog-global settings. Today `ducklake_set_option` validates values but
does not enforce a dedicated admin privilege model.

**Plan.**

1. Inventory how other DuckLake options gate privileged operations (if any), and what
   the metadata database can provide (Postgres roles, etc.).
2. Choose one policy:
   - **Document-only** (“treat as admin SQL; protect at the catalog DB”), or
   - **Soft gate** (DuckLake option / attach flag `allow_admin_ops`), or
   - **Hard gate** tied to catalog DB privileges when available.
3. Apply the same gate to `ducklake_convert_inlining_layout` and global
   `merge_tombstone_mode` writes.
4. Tests for denied vs allowed paths where enforceable.

**Recommendation.** Start with **document-only + clear warnings in the guide**, then add
a soft attach/option gate if operators ask for in-engine enforcement. Avoid inventing a
full ACL system (explicit P5 non-goal).

**Status.** Document-only policy recorded in
[`guides/branching.md`](../guides/branching.md) (Merge → Admin options).
### F4.2 — Sibling conflict under `convert_end_snapshot`

**Problem.** Convert mode can break another live sibling branch’s visibility; merge SQL
already has fail-closed probes (`BuildConvertTombstonesSQL` error strings). Confirm the
rule is complete and tested.

**Plan.**

1. Audit convert path for all object kinds (data files, delete files, tables, …).
2. Golden tests: two siblings diverge with inherited deletes; merging one with
   `convert_end_snapshot` must **fail closed** when it would break the other; 
   `reown_tombstone` (or merging/dropping the sibling first) succeeds.
3. Ensure `dry_run` surfaces the same conflict without mutating metadata.
4. Document the operator choices in the user guide’s merge section.

**Status.** Covered by `test/sql/branching/tombstones/sibling_convert_fail_closed.test`
(dry_run → `conflicts` / “break sibling”; apply convert fails closed; `reown_tombstone`
and drop-sibling-then-convert succeed). Operator notes in
[`guides/branching.md`](../guides/branching.md) merge section.

### F4.3 — Optional: tombstone vacuum / schedule context (deferrable)

Low urgency; do not block F1–F3.

- Tombstone compaction / vacuum of `ducklake_deletion_*` tables.
- Extending `ducklake_files_scheduled_for_deletion` with schedule-context columns
  (hardening P-C).

Track as backlog unless GC storage growth becomes an operator issue.

### Acceptance (F4.1–F4.2)

- Written policy for admin ops (guide + optionally code gate).
- Sibling convert fail-closed covered by tests and dry_run.
- F4.3 explicitly deferred or ticketed, not half-implemented.

---

## Sequencing and milestones

```text
F2 (docs publish) ──────────────────────────────► live guide
F3 (backend triage) ──► green matrix ───────────► CI confidence
F1.1 inlined pick ────► F1.2 DDL pick ─ (F1.3?) ► fuller hot-fix
F4.2 sibling tests ─┐
F4.1 admin policy ──┴───────────────────────────► ops polish
```

| Milestone | Exit criteria |
|---|---|
| **M1** | F2 published (or ducklake-web PR open); F3 baseline matrix recorded |
| **M2** | F1.1 merged + guide updated; F3 branching green on PG/SQLite (or justified skips) |
| **M3** | F1.2 merged; F4.2 tests green; F4.1 policy documented |
| **M4 (optional)** | F1.3 decision; F4.3 ticketed or done |

## Testing strategy (cross-cutting)

| Suite | Covers |
|---|---|
| `test/sql/branching/cherry_pick*.test` | F1 slices + conflicts |
| `test/sql/branching/transplant*.test` | F1 range / atomicity |
| `test/sql/branching/writable_branch_inlining.test` + new pick tests | F1.1 layouts |
| `test/sql/branching/tombstones/*` | F4.2 sibling convert |
| `test/sql/refs/*` + branching under `postgres.json` / `sqlite.json` | F3 |
| Guide examples (manual or doctest later) | F2 / F1 docs drift |

## Explicitly out of scope

Unchanged from Phase 5 / hardening non-goals:

- Rebase as a first-class command
- Ours/theirs / manual conflict-resolution UI
- Row/cell-level merge
- Per-ref access control inside the DuckLake spec

## Open questions

1. **Canonical docs home** — in-repo `docs/guides/branching.md` vs ducklake-web as
   source of truth after F2?
2. **F1.3 compaction** — reject forever with “compact on target”, or schedule after DDL?
3. **Admin enforcement** — document-only vs soft attach flag vs catalog-DB privileges?
4. **CI shape** — rely on full `Catalogs.yml` `test/sql/*`, or add a fast branching
   filter job for PRs?
