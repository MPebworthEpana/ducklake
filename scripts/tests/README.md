# Migrator tests (V2)

Unit and schema-aware tests in `test_duckdb_to_ducklake_migrate.py` run against
any installed `duckdb` Python package. The full DuckLake integration test needs
an **ABI-matched** pair: Python DuckDB source-id == built `ducklake` extension.

## CLI smoke (Option B — green V2 signal without Python rebuild)

Uses `./build/debug/duckdb` (or `$BUILD/duckdb`) only — same binary that built
the extension, so `LOAD` works without a Python rebuild:

```bash
# From repo root (requires build/debug/duckdb + ducklake.duckdb_extension)
bash scripts/tests/test_migrate_cli_smoke.sh
```

Creates a temp source with ARRAY / ENUM / generated / UNION / DEFAULT, migrates
into DuckLake (SQL mirroring the migrator, or the Python migrator if LOAD
works), and asserts:

- `typeof(a)` → `INTEGER[3]`
- ENUM / named `mood` preserved
- generated column value present
- UNION stored as `VARCHAR`

Exit `0` on success. Prefer this path when PyPI duckdb cannot LOAD the local
extension.

## ABI-matched Python recipe (Option A)

The pinned `duckdb/` submodule **no longer vendors** `tools/pythonpkg` (bindings
live in [duckdb/duckdb-python](https://github.com/duckdb/duckdb-python)). Building
a matching wheel is a full C++ compile and is usually too heavy for a quick
local check — use the CLI smoke above as the default green signal.

When you need the Python integration test itself to run (not skip):

```bash
# From repo root, after make debug with ducklake built.
# Pin duckdb-python's DuckDB submodule to the same commit as .github/duckdb-version
# (currently the first 10 hex chars must match the extension stamp, e.g. 0cd9e82950).
#
# Example (slow — full native build):
#   git clone --recurse-submodules https://github.com/duckdb/duckdb-python.git /tmp/duckdb-python
#   cd /tmp/duckdb-python && git submodule update --init
#   # checkout / point submodule at $(cat $REPO/.github/duckdb-version)
#   python3 -m pip install -U pip
#   python3 -m pip install --no-binary=duckdb .

export DUCKLAKE_EXTENSION_PATH=$PWD/build/debug/extension/ducklake/ducklake.duckdb_extension
export LOCAL_EXTENSION_REPO=$PWD/build/debug/repository

python3 -c "import duckdb; print(getattr(duckdb,'__git_revision__',None), duckdb.__version__)"
# Expect source_id prefix to match the extension (e.g. 0cd9e82950)

python3 scripts/tests/test_duckdb_to_ducklake_migrate.py -v
```

If `LOAD` still fails, the skip message and `load_ducklake()` error print both
the Python duckdb identity (`version`, `source_id`, `file`) and the extension
path — see those values when debugging.

### Why PyPI duckdb often skips integration

Local extensions are stamped with the repo DuckDB source-id (e.g. `0cd9e82950`).
A PyPI / prebuilt wheel often has a different id (e.g. `5319116087`), so
`LOAD '<extension>'` fails. That is expected; run
`bash scripts/tests/test_migrate_cli_smoke.sh` instead.
