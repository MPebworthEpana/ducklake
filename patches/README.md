# DuckDB patches

## `duckdb-at-branch-tag.patch`

Extends DuckDB's `AT` clause grammar so `AT (BRANCH => …)` and `AT (TAG => …)` parse
(in addition to `VERSION` / `TIMESTAMP`). Required for Phase 1 named refs.

Applied automatically by `make apply_duckdb_at_patch` (a prerequisite of `debug` /
`release` / `relassert` / `reldebug`). Upstream this into DuckDB when possible; until
then keep the patch in sync with the pinned DuckDB submodule revision.
