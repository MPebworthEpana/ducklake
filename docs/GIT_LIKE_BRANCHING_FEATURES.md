# Git-like Branching for DuckLake: Key Feature Catalog

## 1. Purpose & Sources

This document reviews the community discussion around adding git-like tracking and
modification semantics to DuckLake, and catalogs **all key features that would need to be
implemented to allow efficient git-like behavior within DuckLake**, at parity with what
[Project Nessie](https://projectnessie.org/) provides for Apache Iceberg.

Primary sources:

- [Discussion #720 — "[RFC] Git-style Branching for DuckLake"](https://github.com/duckdb/ducklake/discussions/720)
  (January 2026): a detailed RFC from a contributor (`nbalusu`) who has implemented
  zero-copy, metadata-layer branching on top of DuckLake, including a concrete schema
  design, plus follow-up debate about alternatives and use cases.
- [Discussion #194 — "Feature Request: Zero-Copy Clone and Git-like Branch/Merge for Duck Lake"](https://github.com/duckdb/ducklake/discussions/194)
  (June 2025): the original feature request, explicitly citing the Iceberg + Nessie
  experience, along with community workarounds and a lighter-weight alternative proposal.

All statements about DuckLake internals in this document are grounded in the current
source tree (metadata schema, transaction/conflict machinery, snapshot resolution, garbage
collection, and migration framework), with file references given inline. This document
catalogs *required features and design considerations*; it deliberately does not resolve
the open design questions raised in the RFC.

---

## 2. Background

### 2.1 What Nessie provides (the parity target)

Nessie gives Iceberg catalogs "Git for data" semantics:

| Capability | Description |
|---|---|
| Named references | Mutable **branches** and immutable **tags**, with a default branch (`main`) |
| Commits | Every catalog change is a commit with author/message metadata, forming a history per reference |
| Atomic multi-table transactions | A single commit can span many tables consistently |
| Merge | Merge one branch into another, with content-aware conflict detection |
| Cherry-pick / transplant | Apply selected commits from one branch onto another |
| Diff | Compare catalog state between any two references |
| Garbage collection | Data files are only deleted when unreachable from *every* reference |

### 2.2 What DuckLake already has

DuckLake's snapshot-based design already covers a surprising amount of this surface — for a
*single linear history*:

- **Snapshots as commits.** Every transaction commit creates a row in `ducklake_snapshot`
  (`snapshot_id`, `snapshot_time`, `schema_version`, `next_catalog_id`, `next_file_id`),
  and `ducklake_snapshot_changes` already records `changes_made`, `author`,
  `commit_message`, and `commit_extra_info` — i.e., git-like commit metadata exists today
  (see the schema DDL in `src/storage/ducklake_metadata_manager.cpp`, and
  `ducklake_set_commit_message()` in `src/functions/ducklake_set_commit_message.cpp`).
- **Time travel.** `SELECT ... FROM t AT (VERSION => N)` and `AT (TIMESTAMP => ...)` are
  supported; snapshot resolution for the `AT` clause is implemented in
  `DuckLakeMetadataManager::GetSnapshot(BoundAtClause&, SnapshotBound)`
  (`src/storage/ducklake_metadata_manager.cpp`). An `AT` clause can also be attached at the
  catalog level (`DuckLakeCatalog::CatalogSnapshot()` in `src/storage/ducklake_catalog.cpp`).
- **Atomic multi-table commits** within one catalog, via the metadata database transaction.
- **Optimistic concurrency control.** On commit, DuckLake compares its transaction's
  change set against changes committed concurrently by others and either retries or raises
  a transaction conflict (`DuckLakeTransactionState::CheckForConflicts` in
  `src/storage/ducklake_transaction_state.cpp`; retry loop in
  `src/storage/ducklake_transaction.cpp`).
- **History management.** `ducklake_snapshots()`, `ducklake_table_changes()`,
  `ducklake_expire_snapshots()`, and `ducklake_cleanup_old_files()` provide inspection,
  diffing between snapshots, and retention (all in `src/functions/`).
- **A versioned metadata spec with migrations.** The spec version is stored in
  `ducklake_metadata` under key `version` (0.1 → 0.2 → 0.3 → 0.4 → 1.0 → 1.1-dev…), with a
  `MigrateVxx` framework in `src/storage/ducklake_metadata_manager.cpp`.

### 2.3 The core architectural tension

Everything in DuckLake's metadata is versioned by a **validity interval on a single linear
snapshot chain**: nearly every metadata row (`ducklake_table`, `ducklake_column`,
`ducklake_data_file`, `ducklake_delete_file`, `ducklake_schema`, `ducklake_view`,
`ducklake_partition_info`, `ducklake_macro`, ...) carries `begin_snapshot` /
`end_snapshot`, and visibility at snapshot `S` means
`begin_snapshot <= S AND (end_snapshot IS NULL OR end_snapshot > S)`.

Branching turns the linear snapshot chain into a **DAG**, which breaks two assumptions:

1. **Row lifetimes are single-interval.** If branch `dev` drops a table inherited from
   `main`, setting `end_snapshot` on the shared row would incorrectly hide it on `main`
   too. Deletions must become *per-branch*.
2. **ID allocation is serialized on the chain.** `next_catalog_id` and `next_file_id` live
   on the snapshot row itself, so two branches committing concurrently would allocate
   colliding table/file IDs; a Postgres-level fork of the catalog makes these collisions
   *invisible* (same column, same value, different logical objects).

This tension is why discussion #720 concludes that branching must be implemented **inside
DuckLake's metadata model** rather than by forking the catalog database or the object
store underneath it (see §F15).

---

## 3. Key Features Required

Each feature below lists: what it is, why it is needed (with its source discussion), the
design proposed or debated in the discussions, and the DuckLake internals it touches.

### F1. Branch references (named refs)

**What:** A first-class notion of named, mutable branch references, each pointing at a
head snapshot, with `main` as the default branch.

**Design from #720:** A new `ducklake_branch` table:

```
ducklake_branch(
    branch_id BIGINT,          -- 0 reserved for main (backward compat)
    branch_name VARCHAR,
    parent_branch_id BIGINT,
    fork_snapshot_id BIGINT,   -- snapshot on the parent at which the branch forked
    head_snapshot_id BIGINT,   -- current head of this branch
    status VARCHAR,            -- e.g. active / merged / abandoned
    created_at TIMESTAMPTZ
)
```

**Touchpoints:** New metadata table; commit path must advance `head_snapshot_id`
atomically (a compare-and-swap on the branch head replaces today's implicit "max
snapshot_id" head); snapshot resolution (`GetSnapshot`) must become branch-scoped.

### F2. Zero-copy branch creation

**What:** Creating a branch must be instant and metadata-only — no Parquet data copied, no
metadata rows deep-copied.

**Why:** Both discussions identify full data copies as the expensive status quo. #194's
opening post asks for "zero-copy clones ... without duplicating data"; #720's problem
statement calls current workarounds "expensive and slow".

**Design from #720:** Branching `B` off `A` writes only ~3 rows into the lineage table
(§F3) plus copies of branch-local statistics (§F5). All schemas, tables, columns, and data
file entries stay in place and are *shared through the lineage join*. An early reviewer
asked whether the RFC was "essentially a deep copy of the metadata" — the author
clarified it is not: "All the actual metadata (schemas, tables, files) stays in place —
branches just see it through the lineage join."

**Touchpoints:** New `ducklake_create_branch(catalog, name [, from_ref/at_snapshot])`
function in `src/functions/`; must snapshot the parent's head as the fork point inside a
metadata transaction.

### F3. Branch visibility & lineage resolution

**What:** An efficient way to answer, for every metadata read: *"which rows are visible
from branch B at snapshot S?"* — including rows inherited from ancestor branches up to
each fork point.

**Why efficiency matters:** Metadata reads are DuckLake's hot path (every scan resolves
tables, columns, files, delete files, stats). A naive recursive walk of the branch DAG per
query would be prohibitive.

**Design from #720:** A pre-computed lineage/closure table:

```
ducklake_branch_lineage(
    branch_id BIGINT,
    ancestor_branch_id BIGINT,
    max_visible_snapshot BIGINT   -- visibility cap: fork point into that ancestor
)
```

Branching `B` from `A` (which itself forked from `main`) writes rows like:

```
(B, B,    MAX_INT)     -- B sees all of its own snapshots
(B, A,    fork_snap)   -- B sees A's rows up to the fork point
(B, main, cap)         -- inherited visibility, capped at A's fork from main
```

This makes visibility an O(1)-per-row check via a join: a row owned by branch `X` with
interval `[begin, end)` is visible from branch `B` at snapshot `S` iff a lineage row
`(B, X, cap)` exists and `begin <= min(S_effective, cap)` (and not end-dated before the
cap, and not hidden by a deletion record, §F4).

**Touchpoints:** Every metadata query in `DuckLakeMetadataManager` that today filters on
`begin_snapshot`/`end_snapshot` gains a lineage join — this is the single most pervasive
change in the proposal, and its performance overhead on unbranched catalogs is a key risk
(§4).

### F4. Branch-local deletion / hiding records

**What:** Per-branch records that hide inherited objects without mutating shared rows.

**Why:** As described in §2.3, a branch cannot set `end_snapshot` on a row it shares with
its ancestors. Dropping an inherited table, deleting an inherited data file (e.g. via
`DELETE`/compaction), or removing an inherited column on a branch needs a branch-local
"tombstone".

**Design from #720:** Eight deletion-tracking tables, one per object kind, all with the
same shape:

```
ducklake_branch_{files|tables|schemas|views|columns|macros|partitions|delete_files}_deletion(
    branch_id BIGINT,
    ancestor_branch_id BIGINT,
    object_id BIGINT,
    deleted_at_snapshot BIGINT
)
```

Additionally, `ducklake_delete_file` gains a `data_file_branch_id` column, because a
delete file created on branch `B` may target a data file *owned by an ancestor branch* —
the (branch, file-id) pair is needed to identify the target unambiguously.

**Touchpoints:** DROP/ALTER/DELETE code paths (`ducklake_delete.cpp`,
`ducklake_table_entry.cpp`, `ducklake_schema_entry.cpp`, catalog set logic) must write
tombstones instead of end-dating when the target row is owned by an ancestor; all
visibility queries (§F3) must anti-join against the deletion tables.

### F5. Schema changes to existing metadata tables

**What:** Making every versioned metadata table branch-aware.

**Design from #720:** All ~24 existing metadata tables gain
`branch_id BIGINT NOT NULL DEFAULT 0`, with primary keys becoming composite, e.g.:

| Table | Change |
|---|---|
| `ducklake_snapshot` | PK → `(branch_id, snapshot_id)` |
| `ducklake_data_file` | PK → `(branch_id, data_file_id)` |
| `ducklake_delete_file` | + `data_file_branch_id` (see §F4) |
| `ducklake_schema` / `ducklake_table` / `ducklake_column` / `ducklake_view` / ... | `branch_id` identifies the owning branch |

`DEFAULT 0` keeps existing single-history catalogs valid without rewriting rows
(branch 0 = `main`). The RFC also adds ~20 new indexes to keep the lineage-joined lookups
fast.

**Special case — latest-only tables:** `ducklake_table_stats` and
`ducklake_table_column_stats` have **no** `begin_snapshot`/`end_snapshot` columns (see the
DDL in `src/storage/ducklake_metadata_manager.cpp`) — they hold only the *current* state.
They therefore cannot be shared through lineage intervals and must be **copied per
branch** at fork time (the RFC's branch creation copies "lineage rows, deletion records,
and stats"). The same applies to `ducklake_inlined_data_tables` bookkeeping (§F8).

### F6. Cross-branch ID allocation

**What:** A strategy for allocating `table_id`, `column_id`, `data_file_id`, etc. that
remains collision-free across concurrently-writing branches.

**Why:** Today, `next_catalog_id` and `next_file_id` are carried on each
`ducklake_snapshot` row and advanced serially along the single chain. With branches, two
histories advance these counters independently. #720 gives the concrete failure mode:
"branch A creates a table → gets `table_id=4`. Meanwhile on a [catalog] fork, a
*different* table also auto-increments to `id=4`" — and a merge that only compares values
sees no conflict between two completely different logical objects.

**Options to evaluate:**
- a **global allocator** in the metadata database (single counter shared by all branches),
- **branch-scoped ID spaces** where `(branch_id, object_id)` is the true identity (this is
  effectively what the composite PKs in §F5 encode), or
- **remap-on-merge** (git-like, but invasive for stored file references).

The RFC's composite-PK design leans on the second option; merge (§F9) must still decide
how identities map when a branch's objects land on the target.

**Touchpoints:** Snapshot creation and ID handout in `DuckLakeTransaction` /
`DuckLakeMetadataManager`; the UUIDs already present on schemas/tables/views
(`schema_uuid`, `table_uuid`, `view_uuid`) can serve as stable cross-branch identity for
merge-time matching.

### F7. SQL surface / UX

**What:** User-facing functions and syntax for working with branches.

**Design from #720:**

```sql
-- Create an isolated workspace (instant)
SELECT * FROM ducklake_create_branch('catalog', 'dev');

-- Switch the session/catalog to a branch
CALL ducklake_use_branch('catalog', 'dev');

-- Cross-branch reads without switching
SELECT * FROM trades AT (BRANCH => 'main');
SELECT * FROM trades AT (BRANCH => 'dev');
```

**Required pieces:**
- `ducklake_create_branch`, `ducklake_use_branch`, plus (by symmetry with existing
  `ducklake_*` management functions) `ducklake_drop_branch` and a `ducklake_branches()`
  listing function — naming should follow the existing snake_case `ducklake_*` conventions
  (RFC open question #5).
- A new `BRANCH` unit for the `AT` clause. Today
  `DuckLakeMetadataManager::GetSnapshot(BoundAtClause&, ...)` accepts only `version` and
  `timestamp` units; `branch` (and later `tag`, §F10) must resolve to the ref's head
  snapshot. Combining units (`BRANCH => 'dev'` + `TIMESTAMP => ...`) should resolve
  "branch at time" like git's `ref@{time}`.
- An **attach-level default branch**, e.g.
  `ATTACH 'ducklake:...' (BRANCH 'dev')`, piggybacking on the existing catalog-level `AT`
  clause plumbing (`DuckLakeOptions::at_clause`, `DuckLakeCatalog::CatalogSnapshot()`),
  and parsed alongside existing options in `src/storage/ducklake_storage.cpp`.

### F8. Full write isolation on branches

**What:** All write paths must operate correctly and in isolation when the session is on a
branch — the RFC promises "Full isolation for DML, DDL, and compaction".

**Concretely, per subsystem:**

| Subsystem | Branch behavior required |
|---|---|
| INSERT / COPY (`ducklake_insert.cpp`) | New data files owned by the branch (`branch_id` on `ducklake_data_file`) |
| DELETE / UPDATE / MERGE INTO (`ducklake_delete.cpp`, `ducklake_update.cpp`, `ducklake_merge_into.cpp`) | Delete files / deletion vectors owned by the branch, possibly targeting ancestor-owned data files (`data_file_branch_id`, §F4) |
| DDL & schema evolution (`ducklake_table_entry.cpp`, `ducklake_schema_entry.cpp`) | Branch-local `ALTER`/`CREATE`/`DROP`; per-branch `schema_version` progression (`ducklake_schema_versions`) |
| Data inlining (`ducklake_inline_data.cpp`, `ducklake_flush_inlined_data.cpp`) | Inlined-data rows and their flush must be branch-scoped |
| Compaction (`ducklake_merge_adjacent_files`, `ducklake_rewrite_data_files` in `ducklake_compaction_functions.cpp`) | Compaction on a branch must rewrite only that branch's *view* of the files, never invalidating ancestor-owned files shared with other branches — rewritten outputs are branch-owned, originals are tombstoned per-branch (§F4) |
| Concurrent commits *within* a branch | Existing OCC (`CheckForConflicts` + retry) must be scoped per branch head |
| Server-side / staged commits (`ducklake_server_side_commit.cpp`, `ducklake_staged_commit.cpp`) | Both alternative commit paths must carry branch context |

**Motivating use case (#720 comment, `cbility`):** an HPC cluster stages DuckLake data
into cluster-local storage for heavy jobs (e.g. compaction). With branching, they can
"fork the data, run the heavy operation on that branch in the HPC environment, and merge
back once results are staged out — while the main ducklake continues operating as normal."

### F9. Merge support

**What:** Merging a branch back into its parent (or any ancestor). This is RFC open
question #2, with three candidate levels:

1. **Fast-forward only** — permitted when the target branch has not advanced since the
   fork point; the target's head simply moves to the source's head. Cheap, unambiguous,
   and already sufficient for "reduced but still useful" workflows (per the HPC comment:
   block writes on main during the job, then fast-forward).
2. **Three-way merge with conflict detection** — required for the full HPC use case and
   for any "merge while main keeps moving" workflow. The natural construction reuses
   DuckLake's existing OCC machinery: compute the source branch's cumulative change set
   since the fork snapshot (a `TransactionChangeInformation`-shaped summary) and check it
   against the target's changes since the same fork point, using the same conflict
   taxonomy `CheckForConflicts` already implements (drop-vs-drop, create-vs-create in same
   schema, insert-vs-alter, delete-vs-compaction, concurrent deletes touching the same
   files, ...). Non-conflicting changes are applied to the target as a single merge
   snapshot.
3. **No merge initially** — ship branching for isolated experimentation first; merge later.

**Why merge must be DuckLake-semantic:** #720's central argument against catalog-level
(e.g. Neon) branching is that a byte/row-level Postgres merge "wouldn't understand
DuckLake semantics": identical `table_id` values on two forks can denote different
objects (no conflict detected when there is one), and divergent `ALTER`s of the same
column are invisible as conflicts. "With the lineage approach, merge logic lives in
DuckLake where it belongs. We can do proper conflict resolution, and handle ID allocation
correctly."

**Nessie-parity extensions:** cherry-pick / transplant of individual snapshots between
branches. Snapshot-level change summaries (`ducklake_snapshot_changes.changes_made`, and
`ducklake_table_changes()`) provide the raw material, but replaying a snapshot onto a
different base needs the same conflict machinery as three-way merge — reasonable to defer.

**Merge bookkeeping:** merge commits need parentage metadata (source branch + snapshot)
recorded — e.g. in `ducklake_snapshot_changes.commit_extra_info` or a dedicated column —
so history remains auditable and repeat-merges can find the new common ancestor.

### F10. Tags (immutable references)

**What:** Named, immutable pointers to specific snapshots (e.g. `ducklake_create_tag`,
`AT (TAG => 'v2026-q2-close')`).

**Why:** Core Nessie parity (#194 explicitly targets the Nessie feature set); cheap once
F1/F7 exist (a tag is a branch ref that can never advance); high value for
reproducibility, auditing, and as GC retention roots (§F11). Today users can pin
`AT (VERSION => N)`, but raw snapshot IDs are not meaningful names and — critically — are
not visible to `ducklake_expire_snapshots()` as things to preserve.

### F11. Branch-aware garbage collection

**What:** Retention and file deletion must become **reachability-across-all-refs**
(RFC open question #3: "When can shared files be deleted? Only when no branch references
them? Need a GC protocol?").

**Why:** Branches share Parquet files by design (§F2). Today's cleanup pipeline —
`ducklake_expire_snapshots()` (drop old snapshots), `ducklake_cleanup_old_files()` /
`ducklake_files_scheduled_for_deletion` (physically delete files whose
`end_snapshot` precedes all live snapshots), and `ducklake_delete_orphaned_files()` —
assumes a single history. With branches, a file end-dated (or tombstoned) on `main` may
still be visible from `dev`; deleting it corrupts the branch.

**Required changes:**
- Expiry is per-branch, but *file deletion* requires a global check: a data/delete file is
  physically deletable only when no live snapshot **of any branch or tag** can reach it
  (own-branch intervals + lineage-inherited visibility − deletion records).
- Dropping a branch (`ducklake_drop_branch`) releases its references and feeds the same
  reachability check; branch-owned files unreachable from anywhere become collectible.
- The scheduled-deletion flow must record enough context to re-verify reachability at
  deletion time (branches may be created between scheduling and deletion).

### F12. Diff & audit across references

**What:** Inspection functions extended across branches:

- `ducklake_snapshots()` → per-branch commit log (filter/column for branch; a branch's
  history includes inherited ancestor snapshots up to the fork point).
- `ducklake_table_changes(catalog, schema, table, start, end)` → allow refs, enabling
  "what changed on `dev` vs `main`" — the data-level diff needed for pre-merge review.
- A catalog-level diff between two refs (tables/schemas added, dropped, altered), the
  Nessie `diff` equivalent — derivable from the same visibility queries as §F3.
- `ducklake_branches()` listing name, head, fork point, parent, status, created_at.

### F13. Migration & backward compatibility

**What:** A spec-version bump with a migration, following the existing pattern
(RFC open question #4: auto-migrate on attach vs explicit upgrade command).

**How it fits the codebase:** DuckLake already versions its metadata spec in
`ducklake_metadata` (key `version`) and ships idempotent `MigrateVxx` migrations
(`src/storage/ducklake_metadata_manager.cpp`), currently up to `1.1-dev`. Branching is a
larger change than prior migrations but structurally the same:

- `ALTER TABLE ... ADD COLUMN branch_id BIGINT DEFAULT 0` on the ~24 tables (no row
  rewrites needed — `DEFAULT 0` = `main`),
- `CREATE TABLE` for the 10 new tables, seeding `ducklake_branch` with
  `main` (branch 0, head = current max snapshot) and its self-lineage row,
- ~20 new indexes.

**Compatibility considerations:**
- Old readers/writers on a branched catalog are the dangerous direction (they would ignore
  `branch_id` and deletion records); the spec version gate must refuse or restrict them —
  consistent with how DuckLake already handles newer-spec catalogs.
- Backward compatibility for *unbranched* catalogs must be total: `DEFAULT 0`, and the
  lineage join must impose ~zero overhead when only `main` exists (§4).

### F14. Lighter-weight alternative: commit preconditions (optimistic locking)

**What:** From #194 (`dkrieger`): "short of branching semantics, some ability to attach
requirements to a commit, e.g. 'none of the target tables have been affected by a snapshot
newer than snapshot with ID 37', ideally enforced by the catalog (i.e. optimistic
locking) ... you could more confidently make local clones of the relevant datasets to
support development and verification prior to making a change to the production lake. ...
the optimistic locking approach ... might better fit ducklake and duckdb design goals."

**Why it belongs in this catalog:** It is a *user-facing* generalization of machinery
DuckLake already runs internally — `CheckForConflicts` already validates a transaction's
change set against concurrent snapshots, automatically. Exposing an explicit,
user-specified precondition (e.g. a commit/attach option asserting "tables X, Y untouched
since snapshot N", failing the commit otherwise) would enable clone → develop/verify
externally → promote workflows *without any of F1–F13*, and remains useful even after
full branching ships (cross-catalog promotion, external clones per #194's workaround).
It can be built and shipped independently of, and before, branching.

### F15. Considered-and-rejected external approaches (context)

For completeness, the alternatives discussed and why in-DuckLake branching was preferred:

| Approach | Source | Verdict in discussion |
|---|---|---|
| **Neon** Postgres branching (fork the entire metadata catalog, copy-on-write) | #720 (`YuweiXiao`) | POC done by RFC author; rejected: Databricks-owned, per-branch enterprise pricing, and — decisively — merge is semantically blind to DuckLake object identity (§F9) |
| **DoltgreSQL** (git-versioned Postgres) | #720 | ~5× slower than Postgres in author's testing; plain Dolt is MySQL-only, which DuckLake docs recommend against for metadata |
| **Database Lab Engine** | #720 | Too much infrastructure overhead outside that ecosystem |
| **LakeFS (storage branches) + NeonDB (catalog branches)**, attach with a new data path | #194 (`IS-Josh`) | Unanswered in-thread; inherits the same semantic-merge problem, plus coordination of two independent branching systems |
| **Manual metadata copy + `data_path` rewrite** (copy metadata DB locally, make `ducklake_data_file.path` absolute, repoint `data_path` so new writes land locally while reads span remote + local) | #194 (`Joffreybvn`) | Works as a one-way local "branch" today (related: the `OVERRIDE_DATA_PATH true` attach option, `src/storage/ducklake_initializer.cpp`); no isolation guarantees, no merge, manual SQL surgery on metadata |

The RFC author's conclusion: "No new infra, no vendor lock-in, just tables and refs. The
tradeoff is more code to maintain in DuckLake itself — but that feels right for something
this core to catalog semantics."

---

## 4. Cross-cutting Interactions & Risks

- **Hot-path performance.** The lineage join + deletion-record anti-joins touch every
  metadata read. The RFC mitigates with a pre-computed closure table and ~20 indexes, but
  the zero-branch case (only `main`) must be benchmarked to have ~zero regression —
  DuckLake metadata queries run on every table scan and are already carefully tuned per
  backend (DuckDB/PostgreSQL/SQLite metadata managers under `src/metadata_manager/`).
- **Branch-head concurrency.** Today "the head" is implicitly the max snapshot; with
  branches, commit must CAS `ducklake_branch.head_snapshot_id`, and the OCC retry loop
  must re-read the correct branch head. All three commit paths (client-side, server-side,
  staged) are affected.
- **Metadata growth.** Long-lived branches accumulate deletion records and per-branch
  stats; branch expiry (§F11) and possibly deletion-record compaction are needed to keep
  the metadata DB lean.
- **Latest-only state.** Any table without snapshot intervals (`ducklake_table_stats`,
  `ducklake_table_column_stats`, inlined-data bookkeeping) is a per-branch copy — an easy
  source of subtle bugs (stale stats on a branch after fork).
- **Encryption & paths.** Shared files mean shared `encryption_key` values and path
  semantics across branches; per-branch `data_path` overrides (the #194 local-branch
  pattern) interact with `path_is_relative` handling.
- **Ecosystem compatibility.** Non-DuckDB readers of DuckLake metadata (anything speaking
  the published spec) need the spec update; the spec-version gate (§F13) is the safety
  mechanism.

---

## 5. Suggested Phasing

Detailed per-phase implementation plans live in [`docs/branching/`](branching/README.md).

1. **Refs + read path** ([plan](branching/PHASE_1_NAMED_REFS.md)): F1 (ref table), F10
   (tags — a ref that never advances comes for free), the pointer form of F2, the read
   half of F7 (`AT (BRANCH/TAG => ...)`, attach option), expiry protection from F11, and
   F13 (migration). Refs are zero-copy snapshot pointers; branches are not yet writable.
2. **Writable branches** ([plan](branching/PHASE_2_WRITABLE_BRANCHES.md)): F3 (lineage
   visibility), F4 (deletion records), F5 (schema changes complete), F6 (ID allocation),
   F8 (DML/DDL/inlining/compaction isolation), full F2/F7.
3. **Fast-forward merge + branch-aware GC**
   ([plan](branching/PHASE_3_FAST_FORWARD_MERGE_AND_GC.md)): F9 (FF-only), F11 (full
   reachability GC, branch drop), F12 (per-branch history).
4. **Three-way merge** ([plan](branching/PHASE_4_THREE_WAY_MERGE.md)): F9 (conflict
   detection built on `CheckForConflicts`).
5. **Parity extras** ([plan](branching/PHASE_5_PARITY_EXTRAS.md)): cherry-pick/transplant,
   catalog-level diff, ref history (full F12).

Independent of the above: **F14 (commit preconditions)** is small, self-contained, and
valuable on its own — it could ship first as a stepping stone, per the suggestion in #194
([plan](branching/PHASE_0_COMMIT_PRECONDITIONS.md)).

---

## 6. Summary Table

| # | Feature | Nessie equivalent | DuckLake today | Source | Complexity |
|---|---|---|---|---|---|
| F1 | Branch references | Named branches | Single implicit `main` (max snapshot) | #720 | M |
| F2 | Zero-copy branch creation | Create reference | — (full copies only) | #720, #194 | S (given F3) |
| F3 | Lineage visibility resolution | Commit ancestry | Linear `begin`/`end` intervals | #720 | **L** |
| F4 | Branch-local deletion records | Per-branch content | `end_snapshot` (linear only) | #720 | L |
| F5 | `branch_id` on all metadata tables | — (Nessie stores content per-ref natively) | No branch column | #720 | M |
| F6 | Cross-branch ID allocation | Content-key identity | Serialized `next_*_id` on snapshot | #720 | M |
| F7 | SQL surface (`create/use/drop branch`, `AT (BRANCH ...)`, attach option) | Nessie CLI/API refs | `AT (VERSION/TIMESTAMP)` only | #720 | M |
| F8 | Branch write isolation (DML/DDL/compaction/inlining) | Per-branch commits | Single-history writes | #720 | **L** |
| F9 | Merge (FF; three-way; cherry-pick) | Merge/transplant | OCC conflict detection (reusable) | #720, #194 | **L** |
| F10 | Tags (immutable refs) | Tags | `AT (VERSION => N)` (unnamed, not GC-protected) | #194 (Nessie parity) | S |
| F11 | Branch-aware GC | Multi-ref GC | Single-history expiry/cleanup | #720 | L |
| F12 | Diff & audit across refs | Diff, reflog | `ducklake_snapshots()`, `ducklake_table_changes()` (single history) | #720, #194 | M |
| F13 | Migration & backward compat | — | `MigrateVxx` framework (reusable) | #720 | M |
| F14 | Commit preconditions (optimistic locking) | — (adjacent) | Internal OCC, not user-exposed | #194 | S |
| F15 | External approaches (Neon/LakeFS/manual) — documented, rejected | — | `OVERRIDE_DATA_PATH` partially serves the manual hack | #720, #194 | n/a |
