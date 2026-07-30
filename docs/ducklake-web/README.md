# DuckLake-web Port Package

Ready-to-apply documentation for publishing on [ducklake.select](https://ducklake.select).

This package covers:

1. **Branching** (existing F2) — git-like branches/tags guide
2. **Types + migration** (R3) — `array(N)` / `enum('…')` encodings, updated DuckDB→DuckLake migrator narrative, unsupported-features patch

Source of truth in this fork:

- Branching: [`../guides/branching.md`](../guides/branching.md)
- Type encodings: [`../SPEC_DATA_TYPES.md`](../SPEC_DATA_TYPES.md)
- Migrator script: [`../../scripts/duckdb_to_ducklake_migrate.py`](../../scripts/duckdb_to_ducklake_migrate.py) (R1)
- Unsupported / migration plans: [`../UNSUPPORTED_FEATURES_PLAN.md`](../UNSUPPORTED_FEATURES_PLAN.md), [`../FOLLOWUP_FEATURES_PLAN.md`](../FOLLOWUP_FEATURES_PLAN.md)

Upstream target: [`duckdb/ducklake-web`](https://github.com/duckdb/ducklake-web).

---

## Files to Add / Edit Upstream

### Branching (existing)

| Path in `ducklake-web` | Action |
|---|---|
| `docs/stable/duckdb/guides/branching.md` | **Add** (copy from this package) |
| `_data/menu_docs_stable.json` | **Edit** Guides menu — insert Branching entry (see `_data/MENU_PATCH.md`) |
| `docs/stable/duckdb/usage/snapshots.md` | **Optional** blurb (`CROSS_LINKS.md`) |
| `docs/stable/duckdb/usage/time_travel.md` | **Optional** blurb |
| `docs/stable/duckdb/advanced_features/data_change_feed.md` | **Optional** blurb |

### Types + migration (R3)

| Path in `ducklake-web` | Action |
|---|---|
| `docs/stable/specification/data_types.md` | **Edit** — insert `array(N)` + `enum('…')` from `docs/stable/specification/data_types_ARRAY_ENUM_PATCH.md` |
| `docs/stable/duckdb/migrations/duckdb_to_ducklake.md` | **Edit/replace** with package copy (preserves ARRAY/ENUM/generated) |
| `docs/stable/duckdb/unsupported_features.md` | **Edit** — apply `docs/stable/duckdb/unsupported_features_PATCH.md` |

Menu: **no new menu entry** for type/migration edits (in-place). See
[`TYPES_MIGRATION_MENU_PATCH.md`](TYPES_MIGRATION_MENU_PATCH.md).

---

## Apply Locally

```bash
git clone https://github.com/duckdb/ducklake-web.git
cd ducklake-web

# --- Branching ---
cp /path/to/ducklake/docs/ducklake-web/docs/stable/duckdb/guides/branching.md \
   docs/stable/duckdb/guides/branching.md
# insert menu entry per _data/MENU_PATCH.md
# optional: add blurbs from CROSS_LINKS.md

# --- Types + migration ---
# Merge nested-types snippet from:
#   docs/stable/specification/data_types_ARRAY_ENUM_PATCH.md
# into docs/stable/specification/data_types.md
cp /path/to/ducklake/docs/ducklake-web/docs/stable/duckdb/migrations/duckdb_to_ducklake.md \
   docs/stable/duckdb/migrations/duckdb_to_ducklake.md
# Apply unsupported_features_PATCH.md into unsupported_features.md

./scripts/lint.sh
```

Expected live URLs after publish:

- `https://ducklake.select/docs/stable/duckdb/guides/branching`
- `https://ducklake.select/docs/stable/specification/data_types` (with array/enum)
- `https://ducklake.select/docs/stable/duckdb/migrations/duckdb_to_ducklake`
- `https://ducklake.select/docs/stable/duckdb/unsupported_features`

---

## Tracking

### Branching

Open / claim a `duckdb/ducklake-web` issue or PR titled along the lines of:

`[ducklake/#11] Branching guide needs documentation`

### Types + migration (R3)

Open / claim a `duckdb/ducklake-web` issue titled:

**`[ducklake] Document array/enum types + updated DuckDB migrator`**

Ready-to-paste body: [`UPSTREAM_ISSUE.md`](UPSTREAM_ISSUE.md).

**Filed:** [duckdb/ducklake-web#396](https://github.com/duckdb/ducklake-web/issues/396)

```bash
gh issue create -R duckdb/ducklake-web \
  --title "[ducklake] Document array/enum types + updated DuckDB migrator" \
  --body-file docs/ducklake-web/UPSTREAM_ISSUE.md
```

---

## Publish ownership (V3)

This directory is a **ready-to-apply port package**. Pushing to
[`duckdb/ducklake-web`](https://github.com/duckdb/ducklake-web) / updating
ducklake.select requires a maintainer (or a human-opened fork PR). See
[`../RESIDUAL_VERIFY_PUBLISH_PLAN.md`](../RESIDUAL_VERIFY_PUBLISH_PLAN.md) §V3 for
the handoff checklist. Fork docs remain canonical until the live site catches up.

