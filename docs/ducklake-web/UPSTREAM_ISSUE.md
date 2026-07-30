<!--
Fork meta (not required when re-pasting): filed upstream as
https://github.com/duckdb/ducklake-web/issues/396
Title: [ducklake] Document array/enum types + updated DuckDB migrator
-->

### Summary

Document DuckLake `array(N)` / `enum('…')` type-string encodings and update the
DuckDB → DuckLake migration guide to match the current migrator (preserve
ARRAY / ENUM / STORED generated; keep UNION / VARINT / BIT casts).

Ready-to-apply port package:

- **Package README:** https://github.com/MPebworthEpana/ducklake/blob/cursor/residual-closeout-impl-97be/docs/ducklake-web/README.md
- **Type encodings (fork source of truth):** https://github.com/MPebworthEpana/ducklake/blob/cursor/residual-closeout-impl-97be/docs/SPEC_DATA_TYPES.md
- **Migrator script:** https://github.com/MPebworthEpana/ducklake/blob/cursor/residual-closeout-impl-97be/scripts/duckdb_to_ducklake_migrate.py

### Files to edit

| Path in `ducklake-web` | Action |
|---|---|
| `docs/stable/specification/data_types.md` | **Edit** — insert `array(N)` + `enum('…')` from package `docs/stable/specification/data_types_ARRAY_ENUM_PATCH.md` |
| `docs/stable/duckdb/migrations/duckdb_to_ducklake.md` | **Edit/replace** with package copy (preserves ARRAY/ENUM/generated) |
| `docs/stable/duckdb/unsupported_features.md` | **Edit** — apply package `docs/stable/duckdb/unsupported_features_PATCH.md` |

No new menu entry is required for these in-place edits (see package
`TYPES_MIGRATION_MENU_PATCH.md`). Branching guide + Guides menu remain a
separate apply path in the same package if not already live.

### Versioning note (fork `1.1-dev6` formal tables)

Formal catalog tables (`ducklake_type` / `ducklake_type_member`,
`ducklake_table_constraint`, `generated_*` columns) may still be **fork-only**
at catalog version `1.1-dev6`. Prefer publishing **type-string encodings**
(`array(N)`, `enum('…')`) and the updated migrator narrative **first**. Gate or
follow up formal-table prose until upstream adopts the same catalog migration —
or mark those sections clearly as fork ≥ `1.1-dev6`.

### Suggested apply steps

```bash
git clone https://github.com/duckdb/ducklake-web.git
cd ducklake-web
# Merge nested-types snippet from the fork package's data_types_ARRAY_ENUM_PATCH.md
# Replace migrations/duckdb_to_ducklake.md from the package
# Apply unsupported_features_PATCH.md
./scripts/lint.sh
# open PR against duckdb/ducklake-web
```

### Expected live URLs after merge

- https://ducklake.select/docs/stable/specification/data_types (with array/enum)
- https://ducklake.select/docs/stable/duckdb/migrations/duckdb_to_ducklake
- https://ducklake.select/docs/stable/duckdb/unsupported_features
