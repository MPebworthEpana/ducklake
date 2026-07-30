# Unsupported Catalog Matrix (R2)

Local verification of U1–U5 / F1–F3 suites across DuckLake catalog backends via
[`scripts/run_unsupported_catalog_matrix.sh`](../scripts/run_unsupported_catalog_matrix.sh).

## How to run

```bash
BUILD=build/debug bash scripts/run_unsupported_catalog_matrix.sh
```

Optional: `SKIP_SMOKE=1` to omit related smoke filters
(`default_expressions`, `constraints/unsupported`, `drop_cascade`).

Backends: DuckDB (default), SQLite (`test/configs/sqlite.json`), Postgres
(`test/configs/postgres.json`), Quack (`test/configs/quack.json` when the
extension is present). Scanners require `ENABLE_SQLITE_SCANNER=ON` /
`ENABLE_POSTGRES_SCANNER=ON` (and Quack built) as in `.github/workflows/Catalogs.yml`.

## Results

Recorded: **2026-07-30**, `BUILD=build/debug` on branch
`cursor/residual-closeout-impl-97be`.

| Suite | DuckDB | SQLite | Postgres | Quack |
|---|---|---|---|---|
| `test/sql/default/nested_defaults.test` | PASS | SKIP¹ | SKIP¹ | SKIP² |
| `test/sql/constraints/check_enforce.test` | PASS | SKIP¹ | SKIP¹ | SKIP² |
| `test/sql/catalog/formal_metadata.test` | PASS | SKIP¹ | SKIP¹ | SKIP² |
| `test/sql/types/array.test` | PASS | SKIP¹ | SKIP¹ | SKIP² |
| `test/sql/types/enum.test` | PASS | SKIP¹ | SKIP¹ | SKIP² |
| `test/sql/general/generated_columns.test` | PASS | SKIP¹ | SKIP¹ | SKIP² |
| `test/sql/default/default_expressions.test` (smoke) | PASS | SKIP¹ | SKIP¹ | SKIP² |
| `test/sql/constraints/unsupported.test` (smoke) | PASS | SKIP¹ | SKIP¹ | SKIP² |
| `test/sql/catalog/drop_cascade.test` (smoke) | PASS | SKIP¹ | SKIP¹ | SKIP² |

¹ **SKIP:** `sqlite_scanner` / `postgres_scanner` not built into
`build/debug/duckdb` (`installed=false`). Rebuild with scanner flags to
verify; prefer fix over `skip_tests` if `formal_metadata` / `nested_defaults`
fail on SQLite when the scanner is available.

² **SKIP:** `test/configs/quack.json` exists, but the `quack` extension is not
built into this binary.

## Notes

- DuckDB-catalog core + smoke filters: **all green** (9/9).
- No test or config changes were required for this DuckDB-only baseline.
- Re-run with scanners enabled and update this table when Postgres/SQLite/Quack
  results are available.
