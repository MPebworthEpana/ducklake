# Phase 3: Fast-Forward Merge + Branch-Aware Garbage Collection

Covers the fast-forward portion of **F9**, the full form of **F11**, and part of **F12**
from [`GIT_LIKE_BRANCHING_FEATURES.md`](../GIT_LIKE_BRANCHING_FEATURES.md). Together with
P2 this unlocks the reduced version of the HPC use case from
[discussion #720](https://github.com/duckdb/ducklake/discussions/720): pause writes to
`main`, fork, run the heavy job on the branch, fast-forward `main`, resume.

## Goal

1. **Fast-forward merge**: when the target branch has not advanced since the fork point,
   merging moves the target's head to the source's head — instant, metadata-only,
   conflict-free by construction.
2. **Branch-aware GC**: snapshot expiry, file cleanup, and branch drop become safe in
   branched catalogs (removing the conservative P2 guard rails).

## Prerequisites

Phase 2 (snapshot DAG, lineage, tombstones, branch ownership).

## Scope

- `ducklake_merge_branch(catalog, source [, target])` with fast-forward-only semantics
  (clear error when FF is impossible: "target has advanced; three-way merge is not yet
  supported").
- Merge bookkeeping: record the merge (source branch + snapshot) in
  `ducklake_snapshot_changes.commit_extra_info` (or a dedicated column) so history is
  auditable and later merges can find the new common ancestor.
- `ducklake_drop_branch` upgraded: dropping a branch releases its snapshots, tombstones,
  lineage rows, stats copies — and feeds file reclamation.
- Reachability-based file GC across **all refs**.
- Per-branch history: `ducklake_snapshots()` gains branch awareness (a branch's log =
  inherited ancestor snapshots up to fork + own snapshots).

## Non-goals (deferred)

Three-way merge and any conflict resolution (P4); cherry-pick and cross-ref diff (P5).

## Design

### D1. Fast-forward merge

FF from `dev` into `main` is legal iff `main.head == dev.fork_snapshot` (and `dev` forked
from `main` directly, or transitively with all intermediate branches also unadvanced —
initial version: direct parent only). The operation, in one metadata transaction:

1. CAS `main`'s head: `UPDATE ducklake_ref SET snapshot_id = dev.head WHERE ref_name='main' AND snapshot_id = dev.fork_snapshot` — concurrency-safe by construction; a concurrent commit to `main` makes the CAS fail and the merge errors cleanly.
2. **Re-own or re-cap**: rows owned by `dev` must become visible from `main`. Two options:
   - **(a) Re-own**: `UPDATE ... SET branch_id = main` for dev-owned rows in the merged
     range; converts dev tombstones on main-owned rows into real end-dates. Leaves `main`
     exactly as if the work happened there. More UPDATE volume, simplest steady state.
   - **(b) Re-cap lineage**: add/extend a lineage row so `main` sees `dev`'s rows
     (`(main, dev, dev.head)`); tombstones remain live. Cheapest merge, but permanently
     deepens `main`'s lineage (hot-path cost grows with merge count) and complicates GC.
   Recommendation: **(a)** — FF merges should leave no lineage residue on `main`; the
   UPDATEs are bounded by the branch's own change volume (which the HPC/compaction use
   case expects to be large but one-off).
3. Snapshot renumbering is unnecessary if ID allocation went global in P2-D5 (snapshot ids
   unique catalog-wide); `dev`'s snapshot rows are re-owned to `main` in option (a).
4. The source branch is left as a fully-merged ref (status field from P1 schema:
   `merged`), droppable at will.

### D2. Reachability-based GC

Replace the linear assumptions in the cleanup pipeline:

- **Expiry** (`ducklake_expire_snapshots.cpp`): per-branch expiry of snapshots that are
  (i) not a branch head, (ii) not tag/ref-pinned (P1), and (iii) not a **fork point** of
  any live branch (`ducklake_ref.parent_ref_id` + fork caps in `ducklake_branch_lineage`).
- **File deletion** (`ducklake_cleanup_old_files` / `ducklake_delete_orphaned_files` /
  `ducklake_files_scheduled_for_deletion`): a file is physically deletable iff **no live
  snapshot of any ref can reach it** — own-interval visibility ∪ lineage-inherited
  visibility − tombstones. Implement as one reachability query built from the P2 central
  visibility fragment, evaluated at deletion time (not just scheduling time — branches may
  be created between scheduling and deletion, so re-verify before unlink; extend the
  scheduled-deletion row with enough context, or simply re-run the check by file id).
- **Branch drop**: delete the ref, its lineage rows, tombstones, stats copies, and its
  snapshot rows; branch-owned files then fall out of reachability naturally and are
  scheduled per the standard flow.

### D3. Per-branch history

`ducklake_snapshots(catalog)` gains an optional `branch` parameter (default: current
branch) and a `branch_name`/`branch_id` column; implementation composes the existing
snapshot listing with the lineage caps. `ducklake_current_snapshot` /
`ducklake_last_committed_snapshot` become branch-scoped.

## Task breakdown

1. **T1 — FF merge function** (`src/functions/ducklake_merge_branch.cpp`): validation,
   CAS, re-own transaction, merge bookkeeping; registered in `ducklake_extension.cpp`.
2. **T2 — Expiry** rules (heads, pins, fork points) + tests.
3. **T3 — Reachability file GC**: central reachability query; re-verify-at-unlink;
   `dry_run` support matching existing cleanup functions.
4. **T4 — Branch drop with reclamation** + `status` transitions (`active`/`merged`).
5. **T5 — Per-branch history functions** (D3).
6. **T6 — Remove P2 guard rails** on GC functions in branched catalogs.

## Testing strategy

- FF happy path: fork → writes on branch (incl. compaction rewrites) → merge → verify
  `main` state identical to branch head; verify siblings unaffected; verify CAS failure
  path under a concurrent `main` commit (two-connection test).
- GC matrices: file shared by {main only, main+branch, branch only, dropped branch};
  expire/cleanup at each stage; verify nothing reachable is ever scheduled/deleted and
  everything unreachable eventually is. Reuse `test/sql/cleanup` patterns.
- Fork-point pinning: expiry must retain fork snapshots while a child branch lives, and
  release them when it is dropped/merged.
- Crash-safety reasoning for the merge transaction (single metadata transaction; no data
  files touched).

## Acceptance criteria

- FF merge is instant, atomic, and safe under concurrent target commits (fails cleanly).
- After merge + branch drop + expiry + cleanup, the catalog is byte-equivalent (metadata
  semantics and file set) to one where the work happened directly on `main`.
- No file reachable from any ref is ever deleted (verified by adversarial tests).
- The HPC workflow (fork → compact on branch → FF merge with main paused) works
  end-to-end.

## Risks / open questions

- **Re-own vs re-cap (D1.2)** is the key design decision; recommendation (a) needs
  maintainer sign-off since it moves rows between branch owners.
- **Deep lineage after repeated fork/merge cycles** if any (b)-style residue exists —
  re-own avoids it by construction.
- Reachability GC query cost on large catalogs → needs the P2 indexes; consider batching.
- Interaction with `ducklake_add_data_files` (externally registered files) and
  encryption keys of shared files during branch drop — files must be reference-counted by
  reachability, never by owning branch alone.
