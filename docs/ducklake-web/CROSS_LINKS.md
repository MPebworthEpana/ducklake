## Suggested Blurb for Snapshots

Add near the end of `docs/stable/duckdb/usage/snapshots.md`:

```markdown
## Branches and Tags

DuckLake can name snapshots with mutable **branches** and immutable **tags**. Writable branching, merge, diff, and cherry-pick are covered in the [Branching]({% link docs/stable/duckdb/guides/branching.md %}) guide.
```

## Suggested Blurb for Time Travel

Add to `docs/stable/duckdb/usage/time_travel.md` (near other `AT` clause examples):

```markdown
### Branch and Tag Pins

```sql
FROM events AT (BRANCH => 'dev');
FROM events AT (TAG => 'v1.0');
```

See [Branching]({% link docs/stable/duckdb/guides/branching.md %}).
```

## Suggested Blurb for Data Change Feed

Add to `docs/stable/duckdb/advanced_features/data_change_feed.md`:

```markdown
## Branch Bounds

`table_changes`, `table_insertions`, and `table_deletions` also accept branch or tag names as bounds. Cross-branch bounds start from the merge base. See [Branching]({% link docs/stable/duckdb/guides/branching.md %}).
```
