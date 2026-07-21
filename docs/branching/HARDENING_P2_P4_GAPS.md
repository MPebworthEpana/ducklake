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
| **H1** | G1 (+ column-drop + `data_file_branch_id`) + merge tombstone policy | Possibly `1.1-dev4` if merge policy is stored as catalog metadata | High — touches every DROP/ALTER/compaction/merge path |
| **H2** | G5 + G3 | No | Medium — merge correctness + OCC regression gate |
| **H3** | G2 + G4 | Possibly `1.1-dev4` for inlining layout catalog setting | Medium–High |

H1/H2/H3 may share a single `1.1-dev4` bump if catalog options are introduced for
merge-tombstone policy and inlining layout; otherwise store them in existing
`ducklake_metadata` key/value config (no bump). Prefer **config keys first**; bump only
if a migration must rewrite physical inlined tables.

**Deferred for later (out of this hardening pass):** persisting extra columns on
`ducklake_files_scheduled_for_deletion`. Unlink safety uses **re-verify by file id only**
(no schedule-table schema change).

---

## Decided policies

### P-A. Merge + tombstones: both strategies, admin-selectable

When merging a source branch that holds tombstones into a target (typically `main`), the
catalog supports **both**:

| Mode | Behavior on merge | When to use |
|---|---|---|
| **`convert_end_snapshot`** (default) | For each source tombstone whose target object is (or becomes) owned by the merge target: apply `end_snapshot` on the target-owned row at the merge snapshot, then **delete** the tombstone row. Target looks like a normal drop happened there. | Steady-state `main`; simplest GC and unbranched mental model |
| **`reown_tombstone`** | `UPDATE ducklake_deletion_* SET branch_id = target` (and related re-own). Tombstones remain tombstones, now owned by the target. | Admins who want merge to stay metadata-cheap / preserve tombstone audit shape |

**Default:** `convert_end_snapshot`.

**Configuration:**

- Catalog option, e.g. `merge_tombstone_mode` ∈ {`convert_end_snapshot`, `reown_tombstone`},
  stored in `ducklake_metadata` (or `ducklake_set_option`).
- Readable by everyone; **writable only by admin** (same gate as other dangerous catalog
  options — exact admin check follows existing DuckLake/DuckDB privilege patterns for
  `set_option` / catalog config).
- May be changed between merges; each merge reads the **current** setting at merge start.
- Optional per-merge override: `ducklake_merge_branch(..., merge_tombstone_mode => …)` for
  one-shot admin choice without changing the catalog default.

**Convert path details (default):**

1. Re-own non-tombstone source metadata as today (data files, tables created on source, …).
2. For source tombstones on objects that exist on the target after step 1:
   - `UPDATE … SET end_snapshot = <merge_snapshot> WHERE …` on the live target row
     (only if still open / not already end-dated).
   - `DELETE FROM ducklake_deletion_<kind> WHERE branch_id = source AND object_id = …`
3. Tombstones that still apply only to **sibling** visibility (should not exist after
   re-own of source-only objects) are dropped with the source branch cleanup rules.
4. If convert would end-date an object still required by another live branch’s lineage,
   **fail closed** (or leave the tombstone and error asking for `reown_tombstone`) —
   never silently break a sibling. Spell out the exact conflict rule in H1 tests.

### P-B. Inlining physical layout: both layouts, fixed at setup, admin conversion

A catalog is configured for exactly one inlining layout for its lifetime (until an admin
conversion runs):

| Mode | Layout | Default? |
|---|---|---|
| **`shared_table`** | One physical inlined table per `(table_id, schema_version)`; rows carry a **`branch_id`** column; all reads/writes filter by branch | **Yes** |
| **`per_branch_table`** | Separate physical table per `(table_id, schema_version, branch_id)`, e.g. `ducklake_inlined_data_{tid}_{sv}_b{bid}` | No |

**Fixed at the beginning:**

- Set when the catalog first enables writable branches / inlining on branches (ATTACH
  option or `ducklake_set_option` before any non-main inlined data exists).
- After any inlined data exists on a non-main branch, changing the mode without conversion
  is **rejected**.

**Admin-only conversion functions** (offline / maintenance style):

- `ducklake_convert_inlining_layout(catalog, target_mode => 'shared_table' | 'per_branch_table')`
  - Requires admin.
  - Requires no concurrent writers (document: exclusive maintenance window).
  - Copies/moves all inlined row data + rewrites `ducklake_inlined_data_tables`
    bookkeeping atomically in one metadata transaction (or fails and leaves the old
    layout intact).
  - Updates the catalog setting only after successful data migration.
- Optional `dry_run => true` returns planned table renames / row counts without applying.

**Default for new catalogs:** `shared_table`.

### P-C. Unlink re-verify schema bump — deferred

Do **not** extend `ducklake_files_scheduled_for_deletion` in this hardening pass.
Implement reachability **re-verify by `data_file_id` at unlink time** only. Revisit a
schedule-table schema bump later if operators need richer schedule context.

---

## H1 — Tombstones (G1)

### Goal

On branch `B`, mutating an object owned by ancestor `A` writes a tombstone instead of
end-dating `A`'s row. Removing the P2 refuse guard. Parent/siblings keep seeing the
object; `B` does not. Merges apply tombstones per **P-A** (default convert + drop on
target).

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
4. **Merge (FF / three-way):** apply **P-A**. Default path converts source tombstones into
   target `end_snapshot` and deletes those tombstone rows; `reown_tombstone` keeps today’s
   re-own SQL for `ducklake_deletion_*`. Implement both behind `merge_tombstone_mode`.

### Tasks

1. Implement `EndDateOrTombstone` + per-kind SQL builders.
2. Route all `FlushDrop` / `WriteDroppedColumns` / compaction input retirement through it.
3. Set `data_file_branch_id` on delete-file inserts.
4. Add catalog option `merge_tombstone_mode` (default `convert_end_snapshot`) + optional
   `ducklake_merge_branch` named parameter override; admin-only writes to the catalog
   default.
5. Implement convert and re-own merge paths; tests for both modes + sibling fail-closed
   case under convert.
6. Update `writable_branch_isolation.test`: DROP inherited table **succeeds** on branch and
   remains visible on `main`.
7. Add matrix tests under `test/sql/branching/tombstones/`:
   - DROP table/schema/view/column × {own, inherited} × {visible on parent? sibling?}
   - Compaction on branch hiding ancestor file
   - Full-file drop after delete-file reaches 100%
   - FF merge after tombstone under **both** merge modes

### Acceptance

- No remaining “Phase 2 M3” refuse strings in the hot path.
- Inherited DROP/ALTER on a branch never end-dates ancestor rows.
- Default merge converts tombstones to `end_snapshot` on the target and drops them;
  `reown_tombstone` leaves re-owned deletion rows instead.
- Lineage visibility + tombstones match P2 D2/D3 for the matrix above.
- Existing `[branching]` and `[refs]` suites stay green.

### Risks

- Silent no-op in `WriteDroppedColumns` today → must become an explicit tombstone write.
- Convert mode must not end-date objects still required by sibling branches.
- Tombstone growth on long-lived branches under `reown_tombstone` → rely on H3 branch-drop
  GC; consider later tombstone compaction (out of scope here).
- Three metadata backends must accept the same INSERT/UPDATE shapes.
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

Per **P-C**, do **not** add columns to `ducklake_files_scheduled_for_deletion` in this
pass — re-verify by file id at unlink time is sufficient for safety.

Branch drop today schedules all `branch_id = dropped` files. Keep that **after** confirming
ownership implies other refs cannot see those rows (true for branch-owned files; **false**
for mistaken scheduling of shared paths — reachability check is the safety net).

### Design — Inlining (G4)

Follow **P-B**.

1. Persist catalog setting `inlining_layout` ∈ {`shared_table`, `per_branch_table`},
   default **`shared_table`**. Set once at setup (before non-main inlined data exists).
2. Thread `branch_id` through registration, lookup, flush, and stats for **both** layouts:
   - **`shared_table`:** physical name stays `ducklake_inlined_data_{table}_{sv}`; add
     `branch_id` column to the inlined data table; every read/write filters
     `branch_id = current`. Bookkeeping in `ducklake_inlined_data_tables` also stores
     `branch_id`.
   - **`per_branch_table`:** physical name
     `ducklake_inlined_data_{table}_{sv}_b{branch}`; bookkeeping points at that name.
3. On `create_branch`, seed bookkeeping consistently with the chosen layout (copy
   registration rows with the new `branch_id`, or create empty per-branch tables — match
   stats-seeding semantics so the branch can read inherited inlined data via lineage
   until it writes its own).
4. Remove `DataInliningRowLimit → 0` for non-main once the active layout path is safe.
5. Inlined deletes: branch-local rows OK; inherited inlined rows require G1 tombstones or
   copy-on-write — after H1.
6. **Admin conversion:** implement
   `ducklake_convert_inlining_layout(catalog, target_mode => … [, dry_run => true])`
   as specified in P-B (exclusive maintenance, atomic, admin-only).

### Tasks

1. Implement `FileIsReachable(file_id) -> bool` using visibility fragments at every active
   ref head (+ tag pins as snapshot AT).
2. Call from schedule sites and cleanup execute path; add `dry_run` coverage.
3. Adversarial tests from P3 plan: file shared by {main only, main+branch, branch only,
   dropped branch}; expire/cleanup at each stage.
4. Implement `shared_table` layout end-to-end (default); gate `per_branch_table` behind
   the same setting with full read/write paths.
5. Admin conversion function + tests both directions (`shared_table` ↔ `per_branch_table`)
   on a small fixture catalog.
6. Update `writable_branch_inlining.test` to assert real inlining on a branch under the
   default layout; add a second test file for `per_branch_table` setup + conversion.

### Acceptance

- No reachable file is unlinked in the adversarial matrix.
- Inlining works on a writable branch for branch-local inserts under the default
  `shared_table` layout; main unchanged.
- `per_branch_table` can be selected at setup; admin conversion between layouts works and
  is rejected for non-admins / when writers are active (as documented).
- Single-branch catalogs: cleanup/expiry behavior unchanged.

### Risks

- Reachability query cost → batch by file id list; consider indexes on
  `(branch_id, begin_snapshot, end_snapshot)` if missing.
- Race: branch created between schedule and unlink → re-verify is mandatory.
- Conversion must be crash-safe (all-or-nothing); partial migration is unacceptable.
- `shared_table` leaks if any query omits the `branch_id` filter — treat filter
  discipline as a hard review checklist.

---

## Explicitly out of scope (leave for P5 or later)

- Cherry-pick / transplant / `ducklake_diff` / `ducklake_ref_history` (P5).
- Conflict *resolution* strategies (ours/theirs).
- Row/cell-level merge.
- Rebase.
- Tombstone compaction / vacuum of deletion tables.
- Per-ref access control.
- Extending `ducklake_files_scheduled_for_deletion` with schedule context columns (P-C).

## Testing strategy (cross-cutting)

| Suite | Covers |
|---|---|
| `test/sql/branching/tombstones/*` | H1 matrix |
| `test/sql/branching/merge_*` extensions | H2 file-level deletes, OCC unchanged |
| `test/sql/branching/gc_reachability.test` | H3 adversarial file lifetimes |
| `test/sql/branching/writable_branch_inlining.test` | H3 default `shared_table` enable path |
| `test/sql/branching/inlining_layout_convert.test` | H3 admin conversion both directions |
| `test/sql/concurrent/*` + existing OCC | H2 regression gate |
| `test/sql/refs/*`, `test/sql/cleanup/*` | No regressions |

Run backends: DuckDB metadata (required), Postgres/SQLite if CI allows.

## Suggested milestones and exit criteria

1. **H1 merged**: inherited DROP works; refuse string gone; isolation tests inverted to
   expect success + parent visibility; both merge tombstone modes tested; default is
   convert + drop tombstones on target.
2. **H2 merged**: disjoint delete merge green; OCC suite green; `DetectConflicts` shared.
3. **H3 merged**: reachability unlink safe (re-verify by file id); default shared-table
   inlining enabled on branches; per-branch layout + admin conversion available.
4. **Then** start [Phase 5](PHASE_5_PARITY_EXTRAS.md).

## Remaining open items

1. **H2 before vs after H1**: prefer **H1 → H2 → H3** so file-delete queries can see
   tombstones; H2 can start in parallel on the DetectConflicts refactor only.
2. **Admin gate**: exact privilege check for `merge_tombstone_mode` writes and
   `ducklake_convert_inlining_layout` (reuse `ducklake_set_option` admin patterns once
   confirmed in code).
3. **Sibling conflict under convert**: precise rule when convert would break another live
   branch — fail the merge vs auto-fall-back to re-own (plan recommends **fail closed**).
