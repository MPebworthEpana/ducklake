# Branching in DuckLake

DuckLake supports **git-like branches and tags** on top of its snapshot model.
Branches are mutable named pointers you can write on; tags are immutable pins.
Both are zero-copy: creating a branch or tag does not copy Parquet data.

This guide covers day-to-day usage from creating a branch through merge, diff,
cherry-pick, and retention. Examples use catalog macros on an attached DuckLake
named `my_ducklake`. The underlying table functions
(`ducklake_create_branch(...)`, etc.) are equivalent.

> **Catalog version.** Writable branching requires DuckLake metadata
> `1.1-dev3` or newer; ref history requires `1.1-dev4`. New catalogs use the
> latest version automatically. To upgrade an existing catalog:
>
> ```sql
> ATTACH 'ducklake:metadata.ducklake' AS my_ducklake (
>     DATA_PATH 'files/',
>     AUTOMATIC_MIGRATION TRUE
> );
> ```

---

## Concepts

| Concept | Meaning |
|---|---|
| **Snapshot** | One committed change set (a “commit”) with optional author/message |
| **Branch** | Mutable named pointer at a snapshot; writes advance the branch head |
| **Tag** | Immutable named pointer at a snapshot (release pin, audit pin) |
| **`main`** | Default branch created with the catalog |
| **Session branch** | The branch the current connection writes to (`use_branch`) |

Branches and tags share one namespace: you cannot have both a branch and a tag
named `dev`.

---

## Quick start

```sql
ATTACH 'ducklake:metadata.ducklake' AS my_ducklake (DATA_PATH 'files/');
USE my_ducklake;

CREATE TABLE events(id INTEGER, payload VARCHAR);
INSERT INTO events VALUES (1, 'hello');

-- Fork work onto a feature branch
CALL create_branch('feature');
CALL use_branch('feature');

INSERT INTO events VALUES (2, 'on feature');
CREATE TABLE feature_only(note VARCHAR);
INSERT INTO feature_only VALUES ('isolated');

-- Review, then merge back into main
CALL use_branch('main');
FROM diff('main', 'feature');
CALL merge_branch('feature');   -- fast-forward or three-way

SELECT * FROM events ORDER BY id;
SELECT * FROM feature_only;
```

---

## Creating and listing refs

```sql
-- Branch from the current head of the active branch (usually main)
CALL create_branch('dev');

-- Tag the current snapshot
CALL create_tag('v1.0');

-- Optional: pin an explicit snapshot
CALL create_tag('before_load', snapshot_version => 3);

FROM refs() ORDER BY ref_type, ref_name;
```

Drop when finished:

```sql
CALL drop_branch('dev');
CALL drop_tag('v1.0');
```

A branch that still owns exclusive snapshots (or has child branches) cannot be
dropped until those are merged or otherwise cleaned up.

---

## Switching the write target

```sql
CALL use_branch('dev');     -- subsequent commits go to dev
CALL use_branch('main');    -- switch back
```

`use_branch` changes the session’s active branch. It does **not** rewrite
history and does not appear in ref history.

### Attach on a branch (read-only pin)

Open a connection already positioned at a branch head:

```sql
ATTACH 'ducklake:metadata.ducklake' AS lake_dev (
    DATA_PATH 'files/',
    BRANCH 'dev'
);
```

`BRANCH` cannot be combined with `SNAPSHOT_VERSION` / `SNAPSHOT_TIME` on the
same `ATTACH`.

---

## Time travel by branch or tag

```sql
-- Table as of a branch or tag tip
FROM events AT (BRANCH => 'dev');
FROM events AT (TAG => 'v1.0');

-- Snapshot list for one branch
FROM snapshots(branch => 'dev');
```

`snapshots()` also returns provenance columns when branching is enabled, for
example `merge_source`, `merge_type`, `cherry_pick_source`, and
`transplant_source` (parsed from `commit_extra_info`).

---

## Writing on a branch

After `use_branch('dev')`, DDL and DML commit only on that branch:

- Tables created on `dev` are invisible on `main` until merged.
- Inserts/updates/deletes on shared tables are isolated until merge.
- Concurrent writers on the **same** branch still use DuckLake’s normal
  optimistic concurrency control.

```sql
CALL use_branch('dev');
INSERT INTO events VALUES (10, 'dev only');
ALTER TABLE events ADD COLUMN src VARCHAR;
```

---

## Merging

```sql
-- Preview (no metadata changes)
FROM merge_branch('dev', target := 'main', dry_run := true);

-- Apply
CALL merge_branch('dev', target := 'main');
```

| Merge type | When |
|---|---|
| **`fast_forward`** | Target has not moved since the branch was created (or last merged) |
| **`three_way`** | Both sides advanced with compose-clean changes |
| **`conflicts`** | Both sides changed the same object incompatibly (e.g. conflicting schema edits) |
| **`already_up_to_date`** | Nothing left to merge |

Conflicts fail closed with a clear message. Fix one side, or use
`dry_run := true` to inspect before applying.

Optional: `merge_tombstone_mode` selects how inherited deletes are recorded on
merge (`convert_end_snapshot` default, or `reown_tombstone`).

`convert_end_snapshot` end-dates the live object on the target and drops the
source tombstone. If another live **sibling** branch still needs that object,
convert **fails closed** (including under `dry_run := true`, which reports
`merge_type = conflicts` with a “break sibling” message). Use
`reown_tombstone`, or merge/drop the sibling first, then convert.

After a successful merge, the source branch typically owns no exclusive
snapshots and can be dropped:

```sql
CALL drop_branch('dev');
```

### Admin options

`merge_tombstone_mode` and `inlining_layout` / `convert_inlining_layout` are
**catalog-global operator controls**. DuckLake does not enforce a privilege
model for them in-engine: anyone who can run SQL against the catalog can set
them. Protect them via metadata database permissions (who can connect / run
`set_option` / `convert_inlining_layout`). Prefer documenting this policy for
now rather than inventing an in-engine ACL.
---

## Diff and change review

### Catalog-level diff

Compare schemas, tables, and views visible at two ref heads:

```sql
FROM diff('main', 'feature');
-- object_type | schema_name | object_name | change | detail
-- change is added / dropped / altered
```

`diff(a, a)` is empty. `diff(a, b)` is the inverse of `diff(b, a)` for
added/dropped pairs.

### Data-level changes

`table_changes` / `table_insertions` / `table_deletions` accept snapshot ids,
timestamps, **or branch/tag names**. Cross-branch bounds start from the
**merge base** and report rows that changed on the **end** ref:

```sql
-- Rows that changed on feature since it diverged from main
FROM table_changes('events', 'main', 'feature');

FROM table_insertions('events', 'main', 'feature');
FROM table_deletions('events', 'main', 'feature');

-- Mixed: absolute start snapshot + end branch
FROM table_insertions('events', 12, 'feature');
```

---

## Cherry-pick and transplant

Apply selected commits from one branch onto another without merging the whole
branch.

```sql
-- Single snapshot (DML inserts/deletes: data-file and inlined; CREATE TABLE)
FROM cherry_pick('feature', 42, target := 'main', dry_run := true);
CALL cherry_pick('feature', 42, target := 'main');

-- Contiguous range (all-or-nothing), including CREATE TABLE then DML
CALL transplant('feature', 40, 42, target := 'main');
```

Current limitations (fail closed):

- CREATE TABLE cherry-pick/transplant is supported (same `table_id` / UUID copied onto
  the target when the object is new there). Other DDL (ALTER/DROP, views, macros,
  schemas), flushed-inlined, and compaction snapshots are not supported yet.
- Inlined insert/delete cherry-pick and transplant are supported (shared_table default;
  per-branch layout is handled when present). Deletes of inherited inlined parent rows
  that were never remapped onto the target may not apply.
- Conflicts with the target are rejected using the same conflict detector as merge.

Provenance is recorded in `commit_extra_info` and exposed on `snapshots()` /
`ref_history()`.

---

## Ref history (reflog)

```sql
FROM ref_history('main');
-- ref_name, ref_type, from_snapshot_id, to_snapshot_id, operation,
-- author, commit_message, commit_extra_info, recorded_at, ...
```

Operations include `create`, `commit`, `merge`, `cherry_pick`, `transplant`,
and `drop`. Session switches via `use_branch` are **not** logged.

Requires catalog version `1.1-dev4`.

---

## Tags, expiry, and cleanup

Tags and live branch heads **pin** snapshots so retention cannot delete them:

```sql
CALL create_tag('release', snapshot_version => 20);

-- Fails while the tag (or a live branch head / fork point) still pins it
CALL ducklake_expire_snapshots('my_ducklake', versions => [20]);
-- Error: pinned by a named ref

CALL drop_tag('release');
CALL ducklake_expire_snapshots('my_ducklake', versions => [20]);
```

Branch-aware GC only removes files that are unreachable from **every** active
ref (and not otherwise pinned). After expiry, run the usual cleanup:

```sql
CALL ducklake_cleanup_old_files('my_ducklake');
```

See the [Expire Snapshots](https://ducklake.select/docs/stable/duckdb/maintenance/expire_snapshots)
and [Cleanup of Files](https://ducklake.select/docs/stable/duckdb/maintenance/cleanup_of_files)
docs for general retention workflow.

---

## Commit messages on a branch

```sql
CALL set_commit_message('alice', 'Add feature table',
                        extra_info => 'ticket=123');
-- next commit on this session’s branch records author / message / extra_info
```

---

## Function cheat sheet

Catalog macros (preferred):

| Macro | Purpose |
|---|---|
| `create_branch(name)` / `create_tag(name)` | Create a ref |
| `drop_branch(name)` / `drop_tag(name)` | Drop a ref |
| `refs()` | List live refs |
| `use_branch(name)` | Set session write branch |
| `merge_branch(source, …)` | Fast-forward or three-way merge |
| `diff(ref_a, ref_b)` | Catalog-level compare |
| `table_changes` / `table_insertions` / `table_deletions` | Data-level change feed (ids, timestamps, or refs) |
| `cherry_pick` / `transplant` | Replay selected commits |
| `ref_history(name)` | Reflog for one ref |
| `snapshots(branch => …)` | Per-branch commit log (+ provenance columns) |

SQL time travel: `AT (BRANCH => …)`, `AT (TAG => …)`, `AT (VERSION => …)`.

Attach options: `BRANCH 'name'`, `AUTOMATIC_MIGRATION TRUE`.

---

## Typical workflows

### Feature branch → merge

1. `create_branch('feature')` then `use_branch('feature')`
2. Develop (DDL/DML)
3. `diff('main', 'feature')` and/or `table_changes('t', 'main', 'feature')`
4. `merge_branch('feature', dry_run := true)` then apply
5. `drop_branch('feature')` when empty

### Hotfix onto main

1. Find the snapshot id on the source branch (`snapshots(branch => 'hotfix')`)
2. `cherry_pick('hotfix', snapshot_id, target := 'main', dry_run := true)`
3. Apply, verify with `ref_history('main')`

### Release pin

1. `create_tag('v1.2.0')` on the release snapshot
2. Readers use `AT (TAG => 'v1.2.0')` or attach with a known version
3. Keep the tag until the release no longer needs retention protection

---

## Limitations and non-goals

- **Rebase** (rewrite a branch onto a new base) is not a first-class command;
  transplant + careful ref management can approximate it later.
- **Cherry-pick/transplant** support DML inserts/deletes (data-file and inlined) and
  CREATE TABLE; other DDL, flushed-inlined, and compaction snapshots are not
  supported yet.
- **Per-ref access control** is left to the metadata database / permissions layer.
- Unbranched catalogs remain fully supported; branching features activate with
  the `1.1-dev*` metadata migrations above.

> **Live docs.** Publishing this guide on [ducklake.select](https://ducklake.select)
> is tracked via the **Needs Documentation** / `ducklake-web` workflow
> (`.github/workflows/NeedsDocumentation.yml`). The in-repo file remains the
> editable source until that port lands.

For design background and the Nessie parity map, see
[`GIT_LIKE_BRANCHING_FEATURES.md`](../GIT_LIKE_BRANCHING_FEATURES.md).
For phased implementation notes, see [`../branching/README.md`](../branching/README.md).