# Unsupported Catalog Matrix (R2 / V1)

Local verification of U1–U5 / F1–F3 suites across DuckLake catalog backends via
[`scripts/run_unsupported_catalog_matrix.sh`](../scripts/run_unsupported_catalog_matrix.sh).

## How to run

```bash
# Match Catalogs.yml (sqlite + postgres scanners). Quack optional.
export CXX=g++ CC=gcc
export ENABLE_SQLITE_SCANNER=ON
export ENABLE_POSTGRES_SCANNER=ON
# export ENABLE_QUACK=ON   # optional; uses duckdb-quack FetchContent
export BUILD_EXTENSION_TEST_DEPS=full
export VCPKG_TOOLCHAIN_PATH=$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake
export VCPKG_ROOT=$HOME/vcpkg
make release -j$(nproc)

# Postgres
export PGHOST=127.0.0.1 PGUSER=postgres PGPASSWORD=postgres PGPORT=5432 DUCKLAKE_CI=1
createdb ducklakedb 2>/dev/null || true
export LOCAL_EXTENSION_REPO=$PWD/build/release/repository

BUILD=build/release bash scripts/run_unsupported_catalog_matrix.sh
```

Optional: `SKIP_SMOKE=1` to omit related smoke filters.

If `postgres_scanner` is built as a loadable extension only, ensure it is present
under `$LOCAL_EXTENSION_REPO/<source_id>/linux_amd64/` (the `repository` CMake
target usually installs it). The matrix helper also detects
`build/release/extension/postgres_scanner/postgres_scanner.duckdb_extension`.

## Results

Recorded: **2026-07-30**, `BUILD=build/release` with
`ENABLE_SQLITE_SCANNER=ON` + `ENABLE_POSTGRES_SCANNER=ON` on branch
`cursor/residual-closeout-impl-97be`.

| Suite | DuckDB | SQLite | Postgres | Quack |
|---|---|---|---|---|
| `test/sql/default/nested_defaults.test` | PASS | PASS | PASS | SKIP¹ |
| `test/sql/constraints/check_enforce.test` | PASS | PASS | PASS | SKIP¹ |
| `test/sql/catalog/formal_metadata.test` | PASS | PASS² | PASS | SKIP¹ |
| `test/sql/types/array.test` | PASS | PASS | PASS | SKIP¹ |
| `test/sql/types/enum.test` | PASS | PASS | PASS | SKIP¹ |
| `test/sql/general/generated_columns.test` | PASS | PASS | PASS | SKIP¹ |
| `test/sql/default/default_expressions.test` (smoke) | PASS | PASS | PASS | SKIP¹ |
| `test/sql/constraints/unsupported.test` (smoke) | PASS | PASS | PASS | SKIP¹ |
| `test/sql/catalog/drop_cascade.test` (smoke) | PASS | PASS | PASS | SKIP¹ |

¹ **SKIP:** `ENABLE_QUACK` not enabled in this verification build. Re-run with
`ENABLE_QUACK=ON` and/or `python3 scripts/run_quack_tests.py --filter …` (sidecar)
to fill the Quack column.

² SQLite stores BOOLEAN as integer affinity; `formal_metadata.test` compares
`(is_generated IS TRUE)` so results are portable.

## Notes

- DuckDB + SQLite + Postgres core + smoke: **all green** (27/27 on those backends).
- Quack left for a dedicated `ENABLE_QUACK=ON` rebuild (FetchContent + sidecar).
- Postgres scanner may need `LOCAL_EXTENSION_REPO` pointing at
  `build/release/repository` when not statically linked into the CLI binary.
