# DuckLake-web Port Package (F2)

Ready-to-apply documentation for publishing **Branching** on [ducklake.select](https://ducklake.select).

Source of truth for content: [`../guides/branching.md`](../guides/branching.md) in this repo. This directory holds the **Jekyll-adapted** copy and menu/cross-link patches for [`duckdb/ducklake-web`](https://github.com/duckdb/ducklake-web).

## Files to Add / Edit Upstream

| Path in `ducklake-web` | Action |
|---|---|
| `docs/stable/duckdb/guides/branching.md` | **Add** (copy from this package) |
| `_data/menu_docs_stable.json` | **Edit** Guides menu — insert Branching entry (see `MENU_PATCH.md`) |
| `docs/stable/duckdb/usage/snapshots.md` | **Optional** blurb (`CROSS_LINKS.md`) |
| `docs/stable/duckdb/usage/time_travel.md` | **Optional** blurb |
| `docs/stable/duckdb/advanced_features/data_change_feed.md` | **Optional** blurb |

## Apply Locally

```bash
git clone https://github.com/duckdb/ducklake-web.git
cd ducklake-web
cp /path/to/ducklake/docs/ducklake-web/docs/stable/duckdb/guides/branching.md \
   docs/stable/duckdb/guides/branching.md
# insert menu entry per _data/MENU_PATCH.md
# optional: add blurbs from CROSS_LINKS.md
./scripts/lint.sh
```

Expected live URL after publish:

`https://ducklake.select/docs/stable/duckdb/guides/branching`

## Tracking

Open / claim a `duckdb/ducklake-web` issue or PR titled along the lines of:

`[ducklake/#11] Branching guide needs documentation`
