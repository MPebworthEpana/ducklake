# Phase 2: Writable, Divergent Branches

Covers **F3, F4, F5, F6, F8**, the full form of **F2**, and the write half of **F7** from
[`GIT_LIKE_BRANCHING_FEATURES.md`](../GIT_LIKE_BRANCHING_FEATURES.md). This is the
watershed phase: the linear snapshot chain becomes a DAG, and every metadata read becomes
branch-aware. It corresponds to the core of the
[discussion #720](https://github.com/duckdb/ducklake/discussions/720) RFC design.

## Goal

Branches created in Phase 1 become writable and can diverge from their parent: DML, DDL,
data inlining, and compaction all work on a branch in full isolation, sharing unchanged
metadata and all Parquet files with ancestors (zero-copy), while `main` continues
operating normally.

## Prerequisites

Phase 1 (ref table, `AT (BRANCH => ...)`, attach option, version gating).

## Scope

- Snapshot DAG: per-branch snapshot chains; branch heads that advance on commit.
- Lineage-based visibility for all metadata reads.
- Branch-local deletion (tombstone) records for hiding inherited objects.
- `branch_id` on the metadata tables that record ownership of versioned state.
- Branch-safe ID allocation.
- Write isolation across every write path.
- `ducklake_use_branch(catalog, name)` and writable `ATTACH ... (BRANCH 'x')`.
- Per-branch copies of latest-only state (global table stats, inlined-data bookkeeping).

## Non-goals (deferred)

Merge of any kind (P3/P4), branch-aware *file* GC and branch drop with reclamation (P3),
cross-ref diff (P5). Until P3 ships, documentation must state that
`ducklake_expire_snapshots` / `ducklake_cleanup_old_files` remain restricted in branched
catalogs (conservative behavior: refuse or skip when more than one live branch exists).

## Design (from the #720 RFC, adapted to this codebase)

### D1. Snapshot DAG and branch heads

- `ducklake_snapshot` gains `branch_id BIGINT NOT NULL DEFAULT 0`; PK →
  `(branch_id, snapshot_id)`. Branch 0 = `main` (all existing rows valid unchanged).
- `ducklake_ref.snapshot_id` becomes the **head** for branches and advances on every
  commit to that branch; commit performs a compare-and-swap on the head (today "the head"
  is implicitly `MAX(snapshot_id)` — see `GetSnapshot()` / `InsertSnapshotSql()` in
  `src/storage/ducklake_metadata_manager.*`). Tags never advance.
- The OCC retry loop (`src/storage/ducklake_transaction.cpp` ~L1339 and
  `DuckLakeTransactionState::CheckForConflicts`) re-reads *the current branch's* head and
  conflict-checks only against snapshots on the same branch since the transaction's start.
- All three commit paths (client-side, server-side `ducklake_server_side_commit.cpp`,
  staged `ducklake_staged_commit.cpp`) carry the branch context.

### D2. Lineage visibility (`ducklake_branch_lineage`)

New pre-computed closure table, exactly as in the RFC:

```sql
CREATE TABLE {METADATA_CATALOG}.ducklake_branch_lineage(
    branch_id BIGINT,
    ancestor_branch_id BIGINT,
    max_visible_snapshot BIGINT   -- fork-point cap into that ancestor
);
```

Branching `B` from `A` writes `(B, B, MAX)`, `(B, A, fork_snap)`, plus one capped row per
transitive ancestor. A row owned by branch `X` with interval `[begin, end)` is visible
from branch `B` at snapshot `S` iff lineage row `(B, X, cap)` exists and
`begin <= min(S, cap)` and the row is not end-dated at-or-before the effective snapshot
and not tombstoned (D3). Every metadata query in `DuckLakeMetadataManager` that currently
filters `begin_snapshot <= {SNAPSHOT_ID} AND (end_snapshot IS NULL OR end_snapshot > {SNAPSHOT_ID})`
gains this join — this is the single most pervasive change in the whole effort and should
be implemented once, centrally, as a shared SQL fragment/template analogous to the
existing `{METADATA_CATALOG}` / `{SNAPSHOT_ID}` placeholder expansion.

### D3. Branch-local deletion records

Per-object-kind tombstones (RFC: 8 tables), all shaped
`(branch_id, ancestor_branch_id, object_id, deleted_at_snapshot)`, for: data files,
tables, schemas, views, columns, macros, partitions, delete files. Rule: if the row being
dropped/end-dated is **owned by the current branch**, end-date it as today; if owned by an
**ancestor**, write a tombstone instead. `ducklake_delete_file` additionally gains
`data_file_branch_id` so a branch's delete file can target an ancestor-owned data file
unambiguously.

### D4. `branch_id` on existing tables

All versioned metadata tables gain `branch_id BIGINT NOT NULL DEFAULT 0` (owning branch),
with composite PKs/indexes per the RFC (~24 tables, ~20 new indexes). Latest-only tables
(`ducklake_table_stats`, `ducklake_table_column_stats`, `ducklake_inlined_data_tables`)
get `branch_id` **plus copy-on-branch-create semantics**, since they cannot be shared
through intervals. Note `UpdateGlobalTableStatsSql` and the stats-refresh SQL builders in
`ducklake_metadata_manager.hpp` must become branch-scoped.

### D5. ID allocation

`next_catalog_id` / `next_file_id` currently ride on `ducklake_snapshot` rows and are
advanced serially. Options (decision needed early in this phase, with maintainer input):

- **(a) Global allocator**: move counters to `ducklake_metadata` (or a dedicated
  one-row table) advanced transactionally at commit — IDs unique catalog-wide, merge in
  P3/P4 never sees collisions. Simple; small extra contention at commit.
- **(b) Branch-scoped identity**: keep per-chain counters; identity becomes
  `(branch_id, object_id)` everywhere (this is what composite PKs encode). No new
  contention, but merge must map identities, and every join in D2 must already carry
  `branch_id` anyway.

Recommendation: **(a)** for catalog objects and file IDs — it drastically simplifies
merge — while retaining `(branch_id, …)` PKs for ownership. The existing
`schema_uuid`/`table_uuid`/`view_uuid` columns provide stable identity for merge-time
matching regardless.

### D6. Write-path isolation matrix

| Path | Change |
|---|---|
| INSERT/COPY (`ducklake_insert.cpp`) | new `ducklake_data_file` rows owned by the branch |
| DELETE/UPDATE/MERGE INTO (`ducklake_delete.cpp`, `ducklake_update.cpp`, `ducklake_merge_into.cpp`) | branch-owned delete files/DVs; ancestor-owned data files referenced via `data_file_branch_id`; ancestor file "removal" = tombstone |
| DDL (`ducklake_table_entry.cpp`, `ducklake_schema_entry.cpp`, catalog set) | branch-owned new versions; tombstones for inherited objects; per-branch `schema_version` rows in `ducklake_schema_versions` |
| Inlining (`ducklake_inline_data.cpp`, flush) | inlined rows + bookkeeping branch-scoped |
| Compaction (`ducklake_merge_adjacent_files`, `ducklake_rewrite_data_files`) | outputs branch-owned; inputs tombstoned per-branch, never end-dated if ancestor-owned |
| Transaction-local catalog (`ducklake_catalog_set.cpp`, `ducklake_transaction.cpp`) | branch context part of transaction state |

## Task breakdown (suggested sub-milestones, each shippable behind the version gate)

1. **M1 — Snapshot DAG + branch commits**: D1; `ducklake_use_branch`; writable
   `ATTACH (BRANCH ...)`; per-branch OCC. Branches can advance but only with *new* tables
   created on the branch (no inherited-object mutation yet).
2. **M2 — Lineage read path**: D2 central visibility fragment; all metadata reads
   branch-aware; benchmark: no regression for single-branch catalogs (`benchmark/`
   directory exists for this).
3. **M3 — Tombstones**: D3 + DROP/ALTER/DELETE routing; delete files across branches.
4. **M4 — Full DML/DDL/inlining/compaction isolation** (D6) + latest-only-state copies
   (D4 remainder) + ID allocation switch (D5).
5. **M5 — Migration + hardening**: single `MigrateVxx` for the phase; concurrency tests;
   guard rails for GC functions until P3.

## Testing strategy

- Extensive sqllogictests per milestone under `test/sql/branching/` (isolation matrices:
  each DML/DDL op × {own object, inherited object} × {visible from parent? sibling?}).
- Concurrency tests (two connections, same branch → OCC; different branches → no
  interference), following `test/sql/concurrent` patterns.
- Time travel × branches: `AT (BRANCH => 'x', TIMESTAMP => ...)`-style combinations.
- Benchmark comparison (`benchmark/`) for the single-branch hot path before/after M2 —
  the lineage join must be ~free when only branch 0 exists.
- All three metadata backends.

## Acceptance criteria (phase-level)

- A branch can insert/update/delete/alter/compact in isolation; parent and siblings see
  no change; the branch sees inherited + own state correctly at every snapshot.
- Single-branch catalogs show no measurable metadata-query regression.
- Concurrent commits to different branches never conflict with each other; concurrent
  commits to one branch behave exactly like today's OCC on `main`.
- Older readers are excluded by the spec-version gate; migrated catalogs work unchanged
  until a second branch is created.

## Risks / open questions

- **Hot-path regression** is the headline risk → central visibility fragment + M2
  benchmark gate before proceeding.
- **Migration weight**: ~24 `ALTER TABLE ... ADD COLUMN ... DEFAULT 0` + new tables +
  indexes across three backends; `DEFAULT 0` avoids row rewrites but index builds on large
  catalogs may be slow → document; consider `automatic_migration` opt-in only.
- **ID allocation choice (D5)** shapes P3/P4 merge complexity — decide with maintainers
  before M1 completes.
- **Metadata growth** from tombstones/stats copies on long-lived branches → note for P3
  (branch drop + GC) and potential tombstone compaction.
- The RFC author reports a working implementation with 13 test files — if that code is
  published, reconcile this design with it rather than re-deriving.
