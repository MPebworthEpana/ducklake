# Phase 5: Nessie-Parity Extras — Cherry-Pick, Cross-Ref Diff, Ref History

Covers the cherry-pick portion of **F9** and the full form of **F12** from
[`GIT_LIKE_BRANCHING_FEATURES.md`](../GIT_LIKE_BRANCHING_FEATURES.md), completing
functional parity with the Nessie feature set targeted by
[discussion #194](https://github.com/duckdb/ducklake/discussions/194).

## Goal

Round out the git-like toolset: apply individual snapshots across branches
(cherry-pick/transplant), compare any two refs (catalog-level and data-level diff), and
inspect ref history (reflog-style audit).

## Prerequisites

Phase 4 (symmetric conflict detection and merge application — cherry-pick is a merge of a
single snapshot's change set; diff shares the change-set computation).

## Scope

### S1. Cherry-pick / transplant

- `ducklake_cherry_pick(catalog, source_ref, snapshot_id [, target_ref])`: apply exactly
  one snapshot's change set onto the target as a new snapshot; conflicts detected via
  P4's `DetectConflicts` with the single-snapshot Δ.
- Transplant (a contiguous snapshot range) as a loop with a single combined validation
  pass, mirroring Nessie's transplant.
- Bookkeeping: record provenance (source ref + snapshot) in
  `ducklake_snapshot_changes.commit_extra_info`.

### S2. Cross-ref diff

- **Catalog-level**: `ducklake_diff(catalog, ref_a, ref_b)` → result set of
  (object_type, schema, name, change: added/dropped/altered, detail) computed from the
  two refs' visible metadata (the P2 visibility fragment evaluated at both refs, joined on
  stable UUIDs). The Nessie `diff` equivalent; the pre-merge review tool.
- **Data-level**: extend `ducklake_table_changes(catalog, schema, table, start, end)`
  (`src/functions/ducklake_table_changes.cpp` — bounds are currently snapshot ids on one
  history) to accept refs for either bound, answering "which rows changed on `dev` vs
  `main`" via the existing insertions/deletions machinery
  (`ducklake_table_insertions.cpp` / deletions) evaluated per-branch from the common
  ancestor.

### S3. Ref history (reflog)

- `ducklake_ref_history(catalog, ref)`: movements of a ref's head (commits, merges,
  cherry-picks, FF), with author/message from `ducklake_snapshot_changes` and provenance
  from merge/cherry-pick bookkeeping. Requires persisting head movements — either version
  the `ducklake_ref` rows (begin/end snapshot pattern, consistent with the rest of the
  spec) or a compact `ducklake_ref_log` table; decide with maintainers (versioned rows
  recommended for consistency).
- Extend `ducklake_snapshots()` output with merge/cherry-pick provenance columns.

## Non-goals

- Rebase (rewriting a branch onto a new base) — expressible later as transplant + ref
  reset if demanded; not part of Nessie parity.
- Per-ref access control (Nessie has it; belongs to the metadata-database/permissions
  layer, not the DuckLake spec).
- Row-level merge/diff conflict resolution UI.

## Task breakdown

1. **T1 — Cherry-pick** on P4's machinery (single-snapshot Δ + apply + provenance).
2. **T2 — Transplant** (range validation + batched apply).
3. **T3 — Catalog-level diff** function + tests.
4. **T4 — Ref-aware `ducklake_table_changes`** + tests.
5. **T5 — Ref history**: head-movement persistence + `ducklake_ref_history` + provenance
   columns in `ducklake_snapshots()`.
6. **T6 — Docs**: a user-facing "Branching in DuckLake" guide consolidating P1–P5 usage
   (create/use/merge/tag/diff/expire), suitable for the ducklake.select docs site.
   **Shipped:** [`docs/guides/branching.md`](../guides/branching.md).

## Testing strategy

- Cherry-pick: pick from middle of a branch (not head) — asserts Δ isolation; pick a
  snapshot whose parent context differs on target (conflict path); provenance recorded.
- Transplant ranges incl. failure mid-range (atomicity: all-or-nothing).
- Diff: golden-result tests across DDL + DML divergence; `diff(a, a)` is empty;
  `diff(a, b)` inverse-consistent with `diff(b, a)`.
- Ref history: verify reflog across commit/FF/three-way/cherry-pick sequences.
- Backend portability as in earlier phases.

## Acceptance criteria

- Cherry-pick/transplant apply exactly the selected Δ with P4-grade conflict safety.
- `ducklake_diff` output is sufficient to review a branch before merging (used as such in
  the P4 dry-run tests).
- Ref history reconstructs every head movement with provenance.
- Feature set is at functional parity with the Nessie capabilities table in
  [`GIT_LIKE_BRANCHING_FEATURES.md` §2.1](../GIT_LIKE_BRANCHING_FEATURES.md) (except
  per-ref access control, explicitly out of scope).

## Risks / open questions

- Cherry-picking a snapshot that depends on earlier un-picked snapshots (e.g. inserts
  into a table created two snapshots earlier on the source) — dependency validation must
  fail closed with a clear message.
- Diff performance on very divergent refs → paginate/limit and push filters into the
  visibility queries.
- Reflog storage choice (versioned `ducklake_ref` vs log table) affects the P1 table's
  shape — flag early to keep P1's schema forward-compatible (P1 already reserves
  `parent_ref_id`; versioning columns can be added in this phase's migration).

## Follow-ups

Near-term product follow-ups after T1–T6 are planned in
[`NEAR_TERM_FOLLOWUPS.md`](NEAR_TERM_FOLLOWUPS.md).
