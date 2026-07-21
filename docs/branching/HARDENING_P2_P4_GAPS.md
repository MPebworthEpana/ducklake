# Hardening Plan: Deferred Gaps from Phases 2–4

Closes correctness and completeness holes left after shipping P0–P4 on
`cursor/phase-0-2-branching-0a86` and `cursor/phase-3-4-merge-gc-0a86`. This is **not**
Phase 5 (cherry-pick / diff / reflog); it hardens the branching core before parity extras.

Feature IDs refer to [`docs/GIT_LIKE_BRANCHING_FEATURES.md`](../GIT_LIKE_BRANCHING_FEATURES.md).
Phase plans: [P2](PHASE_2_WRITABLE_BRANCHES.md), [P3](PHASE_3_FAST_FORWARD_MERGE_AND_GC.md),
[P4](PHASE_4_THREE_WAY_MERGE.md).

## Why this before P5

Cherry-pick and cross-ref diff (P5) reuse merge conflict detection, visibility, and
tombstone semantics. Shipping P5 on top of unfinished M3 / GC leaves false confidence:
diffs that ignore tombstones, merges that over-conflict on deletes, and GC that can
delete reachable files after a race. Harden first; then P5 is comparatively contained.

## Gap inventory (current vs planned)

| ID | Gap | Plan source | Schema ready? | Read path? | Write path? |
|---|---|---|---|---|---|
| G1 | Tombstones for inherited DROP/ALTER/full-file drop | P2 M3 / F4 | Yes (`ducklake_deletion_*`) | Yes (lineage anti-join) | **No** — `FlushDrop` refuses |
| G2 | Reachability file GC + re-verify-at-unlink | P3 D2 / F11 | Partial (schedule table) | Linear snapshot existence | **No** reachability query |
| G3 | OCC ↔ `DetectConflicts` single taxonomy | P4 T1 | N/A | Merge uses `DetectConflicts`; OCC separate | Drift risk |
| G4 | Branch-scoped data inlining | P2 M4 / D4 | Column exists | Not filtered | **Disabled** on non-main |
| G5 | File-level delete conflicts in merge | P4 D2 | N/A | Table-level only today | Over-conflicts |

Related small bugs to fold into G1:

- `WriteDroppedColumns` silently skips inherited columns (no tombstone, no error).
- `data_file_branch_id` on `ducklake_delete_file` is never written.

## Recommended sequencing

```
G1 Tombstones ──┬──► G4 Inlining (needs inherited inlined-delete story)
                ├──► G5 File-level merge deletes (cleaner once “file removed” is explicit)
                └──► G2 Reachability GC (tombstones are part of reachability)
G3 OCC refactor ── parallel with G1 after DetectConflicts API is stable
```

**Ship as three PRs** (each independently reviewable):

| PR | Gaps | Spec bump? | Risk |
|---|---|---|---|
| **H1** | G1 (+ column-drop + `data_file_branch_id`) | No (tables already in `1.1-dev3`) | High — touches every DROP/ALTER/compaction path |
| **H2** | G5 + G3 | No | Medium — merge correctness + OCC regression gate |
| **H3** | G2 + G4 | Optional `1.1-dev4` only if schedule-table schema changes | Medium–High |

Do **not** require a version bump for H1/H2: deletion tables and `branch_id` columns
already exist at `1.1-dev3`. H3 may add columns to
`ducklake_files_scheduled_for_deletion` (reachability context) → then bump.

---

## H1 — Tombstones (G1)

### Goal

On branch `B`, mutating an object owned by ancestor `A` writes a tombstone instead of
end-dating `A`'s row. Removing the P2 refuse guard. Parent/siblings keep seeing the
object; `B` does not.

### Design

Central helper (suggested location: `DuckLakeMetadataManager` next to `FlushDrop`):

```text
EndDateOrTombstone(table, object_id, branch_id):
  if row.branch_id == branch_id → UPDATE end_snapshot = {SNAPSHOT_ID}
  else → INSERT ducklake_deletion_<kind>(branch_id, ancestor_branch_id, object_id, deleted_at_snapshot)
```

Kinds and tables (already migrated):

| Kind | Tombstone table | Trigger paths |
|---|---|---|
| schema | `ducklake_deletion_schema` | `DropSchemas` |
| table | `ducklake_deletion_table` | `DropTables`, rename-as-drop |
| view | `ducklake_deletion_view` | `DropViews` |
| column | `ducklake_deletion_column` | `WriteDroppedColumns` / ALTER DROP COLUMN |
| data_file | `ducklake_deletion_data_file` | `DropDataFiles`, compaction rewrite inputs, full-file drop |
| delete_file | `ducklake_deletion_delete_file` | `DropDeleteFiles` |
| macro | `ducklake_deletion_macro` | `DropMacros` |
| partition | `ducklake_deletion_partition` | partition replace / drop |

Also:

1. **Remove** the `FlushDrop` `SELECT error('… Phase 2 M3 …')` refuse.
2. **Wire** `data_file_branch_id` when writing delete files that target ancestor-owned
   data files (`WriteNewDeleteFiles`).
3. **Compaction**: outputs remain branch-owned; ancestor inputs are **tombstoned**, never
   end-dated (P2 D6).
4. **Merge (FF / three-way)**: after re-own, convert source tombstones on objects that are
   now target-owned into real `end_snapshot` updates where appropriate (P3 D1.2 / P4 D3),
   or re-own tombstone rows onto the target (already done for `ducklake_deletion_*` in
   re-own SQL — verify semantics once writers exist).

### Tasks

1. Implement `EndDateOrTombstone` + per-kind SQL builders.
2. Route all `FlushDrop` / `WriteDroppedColumns` / compaction input retirement through it.
3. Set `data_file_branch_id` on delete-file inserts.
4. Update `writable_branch_isolation.test`: DROP inherited table **succeeds** on branch and
   remains visible on `main`.
5. Add matrix tests under `test/sql/branching/tombstones/`:
   - DROP table/schema/view/column × {own, inherited} × {visible on parent? sibling?}
   - Compaction on branch hiding ancestor file
   - Full-file drop after delete-file reaches 100%
   - FF merge after tombstone: main loses the object; sibling unaffected

### Acceptance

- No remaining “Phase 2 M3” refuse strings in the hot path.
- Inherited DROP/ALTER on a branch never end-dates ancestor rows.
- Lineage visibility + tombstones match P2 D2/D3 for the matrix above.
- Existing `[branching]` and `[refs]` suites stay green.

### Risks

- Silent no-op in `WriteDroppedColumns` today → must become an explicit tombstone write.
- Tombstone growth on long-lived branches → rely on H3 branch-drop GC; consider later
  tombstone compaction (out of scope here).
- Three metadata backends must accept the same INSERT shapes (DuckDB / Postgres / SQLite).

---

## H2 — Merge delete precision + OCC unification (G5, G3)

### Goal

1. **G5**: Merging two branches that delete **disjoint** files of the same table succeeds;
   overlapping file deletes conflict (Iceberg/Nessie file granularity).
2. **G3**: OCC and merge share one conflict taxonomy so rules cannot drift.

### Design

**File-level merge deletes (G5)**

At merge time, for each table in `source_delta.tables_deleted_from ∩ target_delta.tables_deleted_from`
(and compaction/delete overlaps), query file ids touched in each fork range:

- New `ducklake_delete_file` rows with `begin_snapshot ∈ (ancestor, head]` and
  `branch_id ∈ {source|target}`
- End-dated / tombstoned `ducklake_data_file` rows in the same window

Intersect the two file-id sets. Non-empty → conflict message naming table + file id.
Empty → allow (compose).

Reuse the SQL shape of
`DuckLakeTransactionState::GetFilesDeletedOrDroppedAfterSnapshot`, scoped by branch +
snapshot window (`GetBranchChangesSince` range).

Keep table-level delete-vs-alter / delete-vs-compact rules as they are.

**OCC refactor (G3)**

1. Add adapter `SnapshotChangeInformation FromTransactionChanges(const TransactionChangeInformation &)`
   (names/ids only; drop live `CatalogEntry` refs where possible).
2. Have `CheckForConflicts` call `DetectConflicts(local_as_snapshot, other)` for the
   shared taxonomy, then run OCC-only enrichments (file-level delete query, richer
   create-table messages).
3. **Regression gate**: existing concurrent / OCC tests must stay bit-identical in
   pass/fail behavior (exception text may be normalized carefully — prefer preserving
   strings where tests match on substrings).

Order inside H2: implement G5 first (merge-only, lower blast radius), then G3.

### Tasks

1. `GetFilesDeletedOrDroppedInRange(branch_id, after, through)` on metadata manager.
2. Teach `DetectConflicts` (or merge caller) to use file intersection when both sides
   deleted from a table.
3. Tests: disjoint file deletes → clean three-way; overlapping → `conflicts` / error.
4. Refactor OCC → shared `DetectConflicts` + enrichment; run `test/sql/concurrent` and
   commit-conflict suites.

### Acceptance

- Disjoint same-table deletes merge cleanly; overlapping files conflict.
- OCC behavior unchanged under the concurrent test suite.
- `DetectConflicts` is the single source of truth for the shared taxonomy.

### Risks

- Mapping `TransactionChangeInformation` → snapshot shape loses entry-type detail unless
  the adapter preserves it.
- Without G1, “file removed via tombstone” may be invisible to the range query — document
  that H2 file queries should also read `ducklake_deletion_data_file` once H1 lands
  (H2 can land after H1, or feature-detect).

---

## H3 — Reachability GC + branch inlining (G2, G4)

### Goal

1. **G2**: Never physically delete a file reachable from any live ref (heads, tags,
   lineage-visible snapshots, minus tombstones). Re-verify at unlink.
2. **G4**: Enable data inlining on non-`main` branches with correct branch-scoped
   bookkeeping (depends on G1 for inherited inlined deletes).

### Design — Reachability GC (G2)

Central predicate (pseudocode):

```sql
-- file F is reachable if there exists an active ref R such that
-- F is visible under R's head via {VISIBLE_DATA_FILE} / lineage + not tombstoned
```

Use cases:

1. **Scheduling** (`DeleteSnapshots`, post-compaction, branch drop): only schedule if
   unreachable from **all** active refs (not merely “no snapshot row in `[begin,end)`”).
2. **Unlink** (`ducklake_cleanup_old_files` / orphan cleanup): before `RemoveFiles`,
   re-run reachability by `data_file_id`; if now reachable, skip unlink and optionally
   delete the schedule row (or leave for retry).

Optional schema (triggers `1.1-dev4` if pursued):

```sql
ALTER TABLE ducklake_files_scheduled_for_deletion
  ADD COLUMN scheduled_from_snapshot BIGINT;  -- or JSON context
```

Minimum viable without bump: re-verify by file id alone at unlink time.

Branch drop today schedules all `branch_id = dropped` files. Keep that **after** confirming
ownership implies other refs cannot see those rows (true for branch-owned files; **false**
for mistaken scheduling of shared paths — reachability check is the safety net).

### Design — Inlining (G4)

1. Thread `branch_id` through `WriteNewInlinedTables`, `LatestInlinedTableQuery`,
   `GetInlinedTableNamesSql`, flush, and stats.
2. On `create_branch`, copy `ducklake_inlined_data_tables` rows like table stats (or
   leave empty and only inherit via lineage visibility of ancestor inlined tables —
   pick one; **copy bookkeeping + lineage-visible ancestor data** matches stats seeding).
3. Physical table naming: include `branch_id` in the name
   (`ducklake_inlined_data_{table}_{sv}_b{branch}`) **or** keep shared tables and
   filter rows by a `branch_id` column — prefer **separate physical tables per branch**
   to avoid cross-branch leakage.
4. Remove `DataInliningRowLimit → 0` for non-main once registration is branch-safe.
5. Inlined deletes: branch-local rows OK; inherited inlined rows require G1-style
   tombstones or “copy-on-write materialize then delete” — implement after H1.

### Tasks

1. Implement `FileIsReachable(file_id) -> bool` using visibility fragments at every active
   ref head (+ tag pins as snapshot AT).
2. Call from schedule sites and cleanup execute path; add `dry_run` coverage.
3. Adversarial tests from P3 plan: file shared by {main only, main+branch, branch only,
   dropped branch}; expire/cleanup at each stage.
4. Branch-scoped inlining registration + enable non-main; update
   `writable_branch_inlining.test` to assert real inlining on a branch.
5. Docs note: inlining on branches requires `1.1-dev3`+ and H1 for inherited deletes.

### Acceptance

- No reachable file is unlinked in the adversarial matrix.
- Inlining works on a writable branch for branch-local inserts; main unchanged.
- Single-branch catalogs: cleanup/expiry behavior unchanged.

### Risks

- Reachability query cost → batch by file id list; consider indexes on
  `(branch_id, begin_snapshot, end_snapshot)` if missing.
- Race: branch created between schedule and unlink → re-verify is mandatory.
- Inlined table name collisions across branches if naming omits `branch_id`.

---

## Explicitly out of scope (leave for P5 or later)

- Cherry-pick / transplant / `ducklake_diff` / `ducklake_ref_history` (P5).
- Conflict *resolution* strategies (ours/theirs).
- Row/cell-level merge.
- Rebase.
- Tombstone compaction / vacuum of deletion tables.
- Per-ref access control.

## Testing strategy (cross-cutting)

| Suite | Covers |
|---|---|
| `test/sql/branching/tombstones/*` | H1 matrix |
| `test/sql/branching/merge_*` extensions | H2 file-level deletes, OCC unchanged |
| `test/sql/branching/gc_reachability.test` | H3 adversarial file lifetimes |
| `test/sql/branching/writable_branch_inlining.test` | H3 enable path |
| `test/sql/concurrent/*` + existing OCC | H2 regression gate |
| `test/sql/refs/*`, `test/sql/cleanup/*` | No regressions |

Run backends: DuckDB metadata (required), Postgres/SQLite if CI allows.

## Suggested milestones and exit criteria

1. **H1 merged**: inherited DROP works; refuse string gone; isolation tests inverted to
   expect success + parent visibility.
2. **H2 merged**: disjoint delete merge green; OCC suite green; `DetectConflicts` shared.
3. **H3 merged**: reachability unlink safe; inlining enabled on branches; P3 acceptance
   line “no reachable file deleted” actually held by tests.
4. **Then** start [Phase 5](PHASE_5_PARITY_EXTRAS.md).

## Open decisions (resolve during H1 kickoff)

1. **Merge + tombstones**: re-own deletion rows vs convert to `end_snapshot` on target —
   confirm with maintainers (P3 recommended re-own; converting may be clearer for GC).
2. **Inlining physical layout**: per-branch tables vs shared table + `branch_id` column.
3. **Schedule-table schema bump**: re-verify-only (no bump) vs persist context (`1.1-dev4`).
4. **H2 before vs after H1**: prefer **H1 → H2 → H3** so file-delete queries can see
   tombstones; H2 can start in parallel on the DetectConflicts refactor only.
