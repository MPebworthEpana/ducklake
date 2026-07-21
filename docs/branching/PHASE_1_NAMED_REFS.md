# Phase 1: Named References (Branches + Tags) — Read Path

Covers **F1, F10, F13**, plus the pointer form of **F2**, the read half of **F7**, and the
expiry-protection slice of **F11** from
[`GIT_LIKE_BRANCHING_FEATURES.md`](../GIT_LIKE_BRANCHING_FEATURES.md).

## Goal

Introduce named, zero-copy references — mutable-in-later-phases **branches** and immutable
**tags** — that pin snapshots on the existing linear history, queryable via
`AT (BRANCH => ...)` / `AT (TAG => ...)` and an attach-level `BRANCH` option, protected
from snapshot expiry. A Phase-1 branch is a named pointer to an existing snapshot; it
cannot advance yet (functionally a tag — exactly how Nessie models tags as non-advancing
refs). This establishes the entire user-facing ref surface that Phase 2 makes writable.

## Why this is the right first slice

Phase 1 requires **no changes to existing metadata tables, hot-path metadata queries, ID
allocation, or the commit path** — the four areas where all the hard branching problems
live (lineage joins, per-branch tombstones, ID collisions, branch-head CAS). It is purely
additive: one new metadata table.

## Prerequisites

None.

## Scope

- New `ducklake_ref` metadata table + spec-version bump + migration.
- `ducklake_create_branch`, `ducklake_create_tag`, `ducklake_drop_branch`,
  `ducklake_drop_tag`, `ducklake_refs` table functions.
- `AT (BRANCH => 'name')` / `AT (TAG => 'name')` resolution.
- `ATTACH ... (BRANCH 'name')` (read-only, like `SNAPSHOT_VERSION`).
- `ducklake_expire_snapshots()` refuses to expire ref-pinned snapshots.

## Non-goals (deferred to P2+)

Branch writes/advancement, `branch_id` columns on existing tables, lineage and
deletion-record tables, ID allocation changes, merge, `ducklake_use_branch` (session
default-branch switching only matters once branches are writable; `ATTACH (BRANCH ...)`
covers the read case), reachability-based GC beyond expiry protection.

## Design decisions

1. **One table for both ref types** (Nessie terminology), shaped so P2 extends rather than
   replaces it:

   ```sql
   CREATE TABLE {METADATA_CATALOG}.ducklake_ref(
       ref_id BIGINT PRIMARY KEY,
       ref_name VARCHAR,            -- unique among live refs (single namespace, git-like)
       ref_type VARCHAR,            -- 'branch' | 'tag'
       snapshot_id BIGINT,          -- pinned snapshot (P2: becomes the branch head)
       parent_ref_id BIGINT,        -- NULL in P1 (P2: fork parent)
       status VARCHAR,              -- 'active' in P1 (P3 adds 'merged'; per RFC ducklake_branch.status)
       created_at TIMESTAMPTZ
   );
   ```

2. **Ref DDL does not create a data snapshot.** Create/drop runs directly against the
   metadata catalog inside a metadata transaction (same pattern as
   `ducklake_expire_snapshots`), because refs point *at* history rather than being part of
   table history. `ref_name` uniqueness is verified inside that transaction.
3. **`AT` clause units `branch` and `tag`** (case-insensitive, like `version`/`timestamp`).
   DuckDB's parser passes arbitrary units through `BoundAtClause`; DuckLake alone validates
   them — the `else` branch in `DuckLakeMetadataManager::GetSnapshot(BoundAtClause&, ...)`
   (`src/storage/ducklake_metadata_manager.cpp`, ~L4374) currently throws
   `Unsupported AT clause unit`. Resolution = ref lookup → `snapshot_id` → the existing
   snapshot query.
4. **Attach option `BRANCH 'name'`** mirrors `SNAPSHOT_VERSION`/`SNAPSHOT_TIME`
   (`HandleDuckLakeOption`, `src/storage/ducklake_storage.cpp` L37–46): builds
   `BoundAtClause("branch", name)` and inherits the existing mandatory read-only
   enforcement (L116–121); error text updated to mention BRANCH.
5. **GC protection at expiry time.** Exclude ref-pinned snapshots from the candidate
   filter in `src/functions/ducklake_expire_snapshots.cpp` (~L81–96); raise a clear error
   when the user explicitly lists a pinned snapshot in `versions => [...]`. Because
   `ducklake_cleanup_old_files` / `ducklake_delete_orphaned_files` only remove files
   invisible from all *remaining* snapshots, protecting expiry is sufficient to protect
   shared files while history is still linear.
6. **Spec versioning.** New enum value in `src/include/common/ducklake_version.hpp`
   (extend the current dev line past `V1_1_DEV_1`; update `DUCKLAKE_LATEST_VERSION` and the
   string mappings), a new `MigrateVxx` step creating `ducklake_ref` with the
   `{IF_NOT_EXISTS}` idempotency pattern (precedent: `ducklake_view_column_tag` in
   `MigrateV10`), the table added to `InitializeDuckLake` for fresh catalogs, and a
   version-gated `SupportsRefs()` on `DuckLakeCatalog` (pattern: the existing
   `>= V1_1_DEV_1` checks in `src/include/storage/ducklake_catalog.hpp` L224–228). Ref
   functions raise a friendly error on older-spec catalogs.

## Task breakdown

### T1. Metadata schema, version gate, migration
- `ducklake_version.hpp`: new enum value; bump latest; string mappings.
- `ducklake_metadata_manager.cpp`: `ducklake_ref` DDL in `InitializeDuckLake`
  (~L229–294) + new `MigrateVxx`.
- `ducklake_catalog.hpp`: `SupportsRefs()`.
- **Acceptance:** fresh catalogs get the table; older catalogs migrate idempotently on
  DuckDB, SQLite, and Postgres metadata backends; pre-migration catalogs give a clear
  error from ref functions.

### T2. Metadata-manager ref API + `AT` resolution
- `DuckLakeMetadataManager`: `CreateRef`, `DropRef`, `GetRefs`, `ResolveRef` (small,
  portable single-table SQL using `{METADATA_CATALOG}` templating).
- Extend `GetSnapshot(BoundAtClause&, SnapshotBound)` with `branch`/`tag` units (join
  `ducklake_ref` → `ducklake_snapshot`, returning
  `snapshot_id, schema_version, next_catalog_id, next_file_id`).
- Verify per-transaction snapshot caching (`DuckLakeTransaction::GetSnapshot`,
  `src/storage/ducklake_transaction.cpp` L1621–1639, keyed by unit+value) is safe — refs
  are immutable in P1 except drop/recreate, and the cache is transaction-scoped; document
  with a comment.
- **Acceptance:** `AT (BRANCH => 'x')` / `AT (TAG => 'x')` resolve on all three backends;
  unknown ref → "No snapshot found at branch x"-style error; unknown units still throw.

### T3. SQL surface: table functions
- New `src/functions/ducklake_refs.cpp` (+ `CMakeLists.txt`):
  - `ducklake_create_branch(catalog, name)` / `ducklake_create_tag(catalog, name)` with
    optional `snapshot_version` / `snapshot_time` named parameters (default: current
    head), resolved via the existing `GetSnapshot` paths;
  - `ducklake_drop_branch(catalog, name)` / `ducklake_drop_tag(catalog, name)`;
  - `ducklake_refs(catalog)` listing (name, type, snapshot_id, created_at), modeled on
    `DuckLakeBaseMetadataFunction` (as `ducklake_snapshots` is).
  - Create/drop modeled on `ducklake_expire_snapshots.cpp` (VARCHAR catalog argument,
    execute-once global state, metadata transaction).
- Register in `src/ducklake_extension.cpp` (`loader.RegisterFunction`, ~L53–110).
- **Acceptance:** functions follow `ducklake_*` naming; create/drop error on read-only
  attach; visible in `duckdb_functions()`.

### T4. Attach option `BRANCH`
- `ducklake_storage.cpp` `HandleDuckLakeOption`: `branch` option → `BoundAtClause`,
  mutually exclusive with `snapshot_version`/`snapshot_time`; read-only enforcement error
  text updated.
- **Acceptance:** `ATTACH ... (BRANCH 'dev')` attaches read-only at the ref's snapshot;
  conflicts and unknown branches error clearly (existing attach-time `at_clause`
  validation, `src/storage/ducklake_initializer.cpp` L120).

### T5. Expiry protection
- `ducklake_expire_snapshots.cpp`: candidate filter excludes pinned snapshots; explicit
  `versions => [pinned]` raises an error naming the ref; `dry_run` consistent.
- **Acceptance:** `older_than => now()` retains pinned snapshots; after `ducklake_drop_tag`
  the same call expires them.

### T6. Tests (`test/sql/refs/`)
- `refs_basic.test` — create/list/drop both types; duplicate-name error; drop-missing
  error; persistence across re-attach.
- `refs_at_clause.test` — `AT (BRANCH/TAG => ...)` incl. schema evolution across the
  pinned snapshot; unknown-ref errors.
- `refs_attach.test` — read-only behavior; write attempts fail; option conflicts.
- `refs_expire.test` — T5 scenarios.
- `refs_migration.test` — attach a pre-refs catalog (existing migration-test pattern),
  verify auto-migration + usability.
- **Acceptance:** new tests green; existing `attach`, `cleanup`, `general`, `metadata`
  suites unaffected.

### T7. Docs
- Short usage note; keep `GIT_LIKE_BRANCHING_FEATURES.md` §5 in sync.

## Testing strategy

Build with `make` (vcpkg per repo instructions); run the new tests through the built
`unittest` runner filtered to `test/sql/refs/*`, plus the neighboring groups for
regression. Keep all new metadata SQL trivially portable (ANSI `CAST`, no DuckDB-only
syntax — the repo standardized on ANSI casts in PR #1139); follow however existing tests
exercise the SQLite/Postgres metadata backends.

## Acceptance criteria (phase-level)

- Creating a branch/tag is O(1) metadata work, instant regardless of catalog size.
- Cross-ref reads work: `SELECT * FROM t AT (BRANCH => 'x')` and `ATTACH (BRANCH 'x')`.
- Ref-pinned snapshots cannot be expired; dropping the ref releases them.
- Unbranched catalogs: zero behavior change, zero measurable overhead.
- Older DuckLake versions refuse (per existing spec-version gating) rather than
  misinterpret a refs-bearing catalog.

## Risks / open questions

- **v1.1 template layer**: confirm whether new-table creation belongs in the base
  metadata manager or the templated `DuckLakeMetadataManagerV1_1` layer
  (`src/metadata_manager/ducklake_metadata_manager_v1_1.cpp`); follow the
  `ducklake_view_column_tag` precedent.
- **Namespace**: recommendation is a single git-like namespace across branch/tag names;
  needs maintainer confirmation.
- **Naming** of `ducklake_refs()` vs `ducklake_branches()`/`ducklake_tags()` — RFC open
  question #5 (match existing DuckLake patterns).
