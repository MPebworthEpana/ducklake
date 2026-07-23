# Git-like Branching for DuckLake — Phased Implementation Plans

> **Post–Phase 5 follow-ups:** see
> **[NEAR_TERM_FOLLOWUPS.md](NEAR_TERM_FOLLOWUPS.md)** (broader cherry-pick,
> docs site publish, catalog-backend CI, hardening leftovers).
>
> **Users:** for day-to-day usage, see the guide
> **[Branching in DuckLake](../guides/branching.md)** (Phase 5 T6; lands with or
> after that docs PR).

This directory contains per-phase implementation plans for adding git-like branching to
DuckLake, derived from the feature catalog in
[`docs/GIT_LIKE_BRANCHING_FEATURES.md`](../GIT_LIKE_BRANCHING_FEATURES.md) (which in turn
reviews [discussion #720](https://github.com/duckdb/ducklake/discussions/720) and
[discussion #194](https://github.com/duckdb/ducklake/discussions/194)).

Each phase is independently shippable, keeps unbranched catalogs fully backward
compatible, and gates its metadata changes behind a spec-version bump using the existing
`MigrateVxx` framework.

## Phase overview

| Phase | Plan | Scope | Features covered¹ | Risk | Depends on |
|---|---|---|---|---|---|
| P0 (optional) | [PHASE_0_COMMIT_PRECONDITIONS.md](PHASE_0_COMMIT_PRECONDITIONS.md) | User-exposed commit preconditions (optimistic locking) | F14 | Low | — (independent, can ship any time) |
| P1 | [PHASE_1_NAMED_REFS.md](PHASE_1_NAMED_REFS.md) | Named refs (branches + tags) as zero-copy snapshot pointers; read path; expiry protection | F1, F2 (pointer form), F7 (read), F10, F11 (partial), F13 | Low | — |
| P2 | [PHASE_2_WRITABLE_BRANCHES.md](PHASE_2_WRITABLE_BRANCHES.md) | Divergent, writable branches: snapshot DAG, lineage visibility, deletion records, write isolation | F2 (full), F3, F4, F5, F6, F7 (write), F8 | High | P1 |
| P3 | [PHASE_3_FAST_FORWARD_MERGE_AND_GC.md](PHASE_3_FAST_FORWARD_MERGE_AND_GC.md) | Fast-forward merge; fully branch-aware garbage collection; branch drop | F9 (FF), F11 (full), F12 (partial) | Medium | P2 |
| P4 | [PHASE_4_THREE_WAY_MERGE.md](PHASE_4_THREE_WAY_MERGE.md) | Three-way merge with DuckLake-semantic conflict detection | F9 (3-way) | High | P3 |
| P5 | [PHASE_5_PARITY_EXTRAS.md](PHASE_5_PARITY_EXTRAS.md) | Cherry-pick/transplant, catalog-level diff between refs, ref history | F9 (cherry-pick), F12 (full) | Medium | P4 |
| Post-P5 | [NEAR_TERM_FOLLOWUPS.md](NEAR_TERM_FOLLOWUPS.md) | Broader cherry-pick/transplant, docs site publish, PG/SQLite branching CI, hardening leftovers | — | Mixed | P5 + H1–H3 |

**Hardening (before P5):** [HARDENING_P2_P4_GAPS.md](HARDENING_P2_P4_GAPS.md) —
closes deferred gaps from P2–P4 (tombstones, reachability GC, OCC/`DetectConflicts`
unification, branch inlining, file-level merge deletes) in three PRs (H1→H2→H3).
Decided policies: admin-selectable merge tombstone mode (default convert-to-`end_snapshot`);
inlining layout fixed at setup (default shared table + `branch_id`) with admin conversion.

¹ Feature IDs (F1–F15) refer to the catalog in
[`GIT_LIKE_BRANCHING_FEATURES.md` §3](../GIT_LIKE_BRANCHING_FEATURES.md).

## Sequencing rationale

- **P1 first** because it is the largest slice that requires *no* changes to existing
  metadata tables, hot-path metadata queries, ID allocation, or the commit path — the four
  areas where all the hard problems live. It establishes the entire user-facing surface
  (`ducklake_create_branch`, `AT (BRANCH => ...)`, attach option, ref listing, pinned
  retention) that later phases make writable.
- **P2 is the watershed**: it turns the linear snapshot chain into a DAG and touches every
  metadata read. Everything after it is comparatively contained.
- **P3 before P4** because fast-forward merge needs no conflict handling and unlocks the
  reduced version of the fork → heavy-job → merge-back workflow (with writes to the target
  paused), while three-way merge (P4) unlocks the full concurrent version.
- **P0 is orthogonal**: commit preconditions generalize machinery DuckLake already runs
  internally (`CheckForConflicts`) and enable clone → verify → promote workflows without
  any branching at all. It can be built in parallel with, before, or after any phase.
