# Phase 4: Three-Way Merge with Conflict Detection

Covers the three-way portion of **F9** from
[`GIT_LIKE_BRANCHING_FEATURES.md`](../GIT_LIKE_BRANCHING_FEATURES.md). This unlocks the
full concurrent workflow from [discussion #720](https://github.com/duckdb/ducklake/discussions/720):
merge a branch back **while the target keeps moving** — the complete HPC
fork → heavy-job → merge-back scenario, and general team workflows.

## Goal

`ducklake_merge_branch` handles the case where **both** source and target have advanced
since the fork point: compute both sides' change sets since the common ancestor, detect
conflicts using DuckLake semantics, and — when conflict-free — apply the source's changes
onto the target as a single merge snapshot. Conflicts abort with precise, actionable
errors (no auto-resolution in this phase).

## Why merge must be DuckLake-semantic

The core argument from #720: a byte/row-level merge of the metadata database cannot work —
identical `table_id` values on two forks can denote *different* objects (false
non-conflict), and divergent `ALTER`s of the same column are invisible as conflicts.
"Merge logic lives in DuckLake where it belongs."

## Prerequisites

Phase 3 (FF merge, merge bookkeeping/common-ancestor tracking, branch-aware GC).

## Scope

- Three-way merge: `ducklake_merge_branch(catalog, source [, target])` auto-selects FF
  when possible, otherwise three-way.
- Conflict detection across the full change taxonomy (below), reported as a result set
  (`dry_run => true`) or a failed merge with the same detail.
- Merge commit: one new snapshot on the target containing the source's net changes;
  parentage recorded for future common-ancestor computation (repeat merges of the same
  branch merge only the delta since the last merge).

## Non-goals (deferred)

Conflict *resolution* strategies (ours/theirs/manual), row-level (cell-level) data merge —
data conflicts are detected at file/table granularity per the taxonomy below; cherry-pick
(P5).

## Design

### D1. Change-set computation

For branch `B` since ancestor snapshot `A`, compute a cumulative
`SnapshotChangeInformation`-shaped summary by aggregating
`ducklake_snapshot_changes.changes_made` over `B`'s snapshots in `(A, head]` — exactly
what the commit path already parses for OCC
(`GetSnapshotAndStatsAndChanges` / `ParseSnapshotAndStatsAndChanges`,
`src/storage/ducklake_metadata_manager.*`), lifted from "since transaction start" to
"since fork point". Both sides are computed: `Δsource` and `Δtarget`.

### D2. Conflict detection

Reuse the taxonomy of `DuckLakeTransactionState::CheckForConflicts`
(`src/storage/ducklake_transaction_state.cpp`) — it already covers, between two change
sets: drop-vs-drop, create-vs-create (same name/schema), insert-vs-alter,
insert-vs-delete, delete-vs-delete on overlapping files, delete-vs-compaction,
drop-schema-vs-create-in-schema, inlined-data variants, etc. Refactor from
"transaction changes vs concurrent snapshots" to a symmetric
`DetectConflicts(Δsource, Δtarget)` used by both OCC (unchanged behavior) and merge.
Identity matching across branches uses stable UUIDs (`table_uuid`, `schema_uuid`,
`view_uuid`) plus global IDs from P2-D5.

Additional merge-only checks:
- **Schema-evolution divergence** on the same column (both sides altered column X of the
  same table) — conflict even when either side alone would pass OCC.
- **Partition/sort config divergence** on the same table.
- **Same-table data writes**: both sides inserted into table T → *not* a conflict (append
  semantics compose); both sides deleted/updated overlapping files → conflict (matches
  OCC's existing files-deleted check, `GetFilesDeletedOrDroppedAfterSnapshot`).

### D3. Applying the merge

In one metadata transaction on the target:
1. Create the merge snapshot (new snapshot id on target's chain; schema_version advanced
   if DDL is merged).
2. Re-emit `Δsource` against the target: new data/delete files re-owned (per P3's re-own
   policy) with `begin_snapshot` = merge snapshot; source tombstones on shared objects
   become target end-dates/tombstones; DDL re-applied as new column/table versions.
3. Rebuild affected latest-only state (global table stats) for merged tables — the
   stats-refresh SQL builders (`ReadFileColumnStatsForTableSql`, etc.) already exist for
   compaction rewrites and can be reused.
4. Record merge parentage (source branch, source head, common ancestor) in the merge
   snapshot's `ducklake_snapshot_changes`.
5. CAS the target head; on CAS failure (concurrent commit), retry the whole computation —
   same pattern as the existing OCC retry loop.

### D4. UX

- `dry_run => true` returns the would-be changes and any conflicts as a result set
  (mirroring `ducklake_expire_snapshots`' dry-run convention).
- Conflict errors name the object (schema.table/column), both branches' operations, and
  the snapshots involved.

## Task breakdown

1. **T1 — Refactor** `CheckForConflicts` into a symmetric, reusable
   `DetectConflicts(Δ, Δ)` with zero behavior change for OCC (existing concurrent tests
   must stay green).
2. **T2 — Fork-range change-set computation** (D1) incl. repeat-merge common-ancestor
   selection from P3 bookkeeping.
3. **T3 — Merge-only conflict rules** (D2 additions).
4. **T4 — Merge application** (D3) + retry loop.
5. **T5 — Dry-run + error reporting** (D4).
6. **T6 — Tests.**

## Testing strategy

- Conflict matrix tests: for each taxonomy entry, construct source/target divergence and
  assert conflict vs clean merge (table-driven sqllogictests under
  `test/sql/branching/merge/`).
- Compose-clean cases: disjoint tables; same-table double-append; branch compaction vs
  target reads; DDL on distinct columns.
- Repeat merges: merge, continue on branch, merge again — only the delta applies.
- Concurrency: merge racing a target commit (CAS retry) and racing a source commit
  (snapshot pinning during merge).
- Equivalence check: after a clean merge, target state equals a serial replay of both
  histories.
- Full HPC scenario end-to-end with `main` active throughout.

## Acceptance criteria

- Every conflict class in the documented taxonomy is detected; no false negatives in the
  matrix tests; compose-clean cases merge without spurious conflicts.
- Merge is atomic, retry-safe, and leaves stats/inlined bookkeeping correct.
- OCC behavior is bit-identical to pre-refactor (T1 regression gate).

## Risks / open questions

- **Row-level conflicts are out of scope** — two branches updating the same *row* via
  delete-file mechanics on the same ancestor file will conflict at file granularity; this
  is the honest, Iceberg/Nessie-like granularity, but must be documented prominently.
- **Expired history**: if `ducklake_snapshot_changes` rows in the fork range were expired
  (P3 expiry), change sets are incomputable → merge must fail closed with guidance
  (protect fork ranges of live branches from expiry, per P3-D2).
- **Semantics of merging compaction**: source compacted ancestor files that target also
  modified — resolve via the delete-vs-compaction conflict rule; needs careful tests.
- Long-running merges vs busy targets → CAS retry livelock risk; mitigate with bounded
  retries + clear error (same as existing commit retry policy).
