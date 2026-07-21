# Phase 0 (optional, independent): Commit Preconditions / Optimistic Locking

Covers **F14** from [`GIT_LIKE_BRANCHING_FEATURES.md`](../GIT_LIKE_BRANCHING_FEATURES.md).
Source: [discussion #194](https://github.com/duckdb/ducklake/discussions/194) (`dkrieger`):

> "some ability to attach requirements to a commit, e.g. 'none of the target tables have
> been affected by a snapshot newer than snapshot with ID 37', ideally enforced by the
> catalog (i.e. optimistic locking), would allow similar workflows ... the optimistic
> locking approach might better fit ducklake and duckdb design goals."

## Goal

Let users attach explicit preconditions to a transaction that the catalog enforces at
commit time, failing the commit if violated — e.g. *"fail unless tables X, Y have not been
changed by any snapshot after N"*. This enables **clone → develop/verify externally →
promote** workflows without any branching machinery, and stays useful after branching
ships (external clones, cross-catalog promotion).

## Prerequisites

None. Independent of all other phases.

## Scope

- A way to declare preconditions within a DuckLake transaction, e.g.:
  - `CALL ducklake_require_unchanged(catalog, tables := ['s.t1', 's.t2'], since_snapshot := 37)`
    (function name/shape to be settled during implementation review), and/or
  - a whole-catalog variant: `since_snapshot` with no table list = "no snapshot newer
    than N at commit time".
- Enforcement at commit, inside the existing conflict-check step, with a clear
  `TransactionException` naming the violated precondition and the offending snapshot(s).
- Preconditions are transaction-scoped state (cleared on commit/rollback).

## Non-goals

- No persistence of preconditions in metadata tables.
- No branching semantics of any kind.
- No cross-catalog enforcement (the precondition runs against the catalog being committed
  to; coordinating multiple catalogs remains the user's responsibility).

## Design

DuckLake already performs optimistic conflict detection at commit:
`DuckLakeTransactionState::CheckForConflicts` (`src/storage/ducklake_transaction_state.cpp`)
compares the transaction's `TransactionChangeInformation` against
`SnapshotChangeInformation` parsed from `ducklake_snapshot_changes` for snapshots committed
after the transaction started, and the commit loop in
`src/storage/ducklake_transaction.cpp` retries on conflict.

Phase 0 exposes a *user-declared* variant of the same check:

1. **State**: add a `vector<CommitPrecondition>` to `DuckLakeTransaction`
   (`{optional table list (resolved to TableIndex), snapshot_id floor}`); names resolved
   to IDs at declaration time so renames don't dodge the check.
2. **Declaration**: a table function (pattern: `src/functions/ducklake_set_commit_message.cpp`,
   which similarly stashes commit-scoped state on the transaction) registered in
   `src/ducklake_extension.cpp`.
3. **Enforcement**: during commit, after the current snapshot/changes are fetched
   (`GetSnapshotAndStatsAndChanges`), evaluate each precondition against
   `ducklake_snapshot_changes` in the range `(since_snapshot, committing snapshot)`:
   - table-scoped: fail if any change entry (created/altered/dropped/inserted/deleted,
     incl. inlined variants — the same categories `SnapshotChangeInformation` already
     parses) touches a listed table;
   - catalog-scoped: fail if any snapshot exists past the floor.
   Precondition failures are **not retried** (unlike ordinary OCC conflicts, retrying
   cannot satisfy them); they abort with a descriptive error.
4. **Read-only + no-op transactions**: preconditions with no writes still validate at
   `ducklake_commit()` time, so a pure "assert" workflow works.

## Task breakdown

| # | Task | Touchpoints |
|---|---|---|
| 1 | `CommitPrecondition` state + declaration function | `src/include/storage/ducklake_transaction.hpp`, new `src/functions/ducklake_require_unchanged.cpp`, `src/ducklake_extension.cpp`, `src/functions/CMakeLists.txt` |
| 2 | Enforcement in commit path (all three: client-side, server-side, staged) | `src/storage/ducklake_transaction.cpp`, `ducklake_transaction_state.cpp`, `ducklake_server_side_commit.cpp`, `ducklake_staged_commit.cpp` |
| 3 | Error taxonomy + no-retry classification | commit retry loop (`ducklake_transaction.cpp` ~L1339) |
| 4 | Tests | new `test/sql/concurrent/`-style tests: precondition satisfied/violated, table-scoped vs catalog-scoped, rename-after-declare, precondition-only transactions, interplay with automatic retry |

## Testing

sqllogictest with two connections (existing `test/sql/concurrent` patterns): declare a
precondition on connection A, commit an interfering snapshot on connection B, verify A's
commit fails with the precondition error (and succeeds when B's change targets an
unrelated table).

## Acceptance criteria

- Precondition violations abort commits deterministically with an error naming the
  precondition, table(s), and violating snapshot id(s).
- Non-violating concurrent activity does not affect commits (beyond existing OCC).
- No metadata schema change; no spec version bump; zero effect when the feature is unused.

## Risks / open questions

- **Naming/shape of the SQL surface** (function vs. attach/transaction option) should get
  maintainer input — this is new public API.
- **Semantics for `versions` gaps** (expired snapshots inside the checked range):
  `ducklake_snapshot_changes` rows for expired snapshots may be gone; decide whether the
  check fails closed (conservative: unknown history ⇒ error) — recommended — or open.
- Interaction with `ducklake_commit()`/autocommit boundaries needs explicit tests.
