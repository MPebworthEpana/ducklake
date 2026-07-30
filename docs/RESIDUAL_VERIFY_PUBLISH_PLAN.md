# Residual Verification & Publish Plan

Closes the three leftovers after R1–R3 in
[`FOLLOWUP_FEATURES_PLAN.md`](FOLLOWUP_FEATURES_PLAN.md):

| ID | Item | Nature |
|---|---|---|
| **V1** | Postgres / SQLite / Quack matrix runs | Engineering — rebuild + run + triage |
| **V2** | Migrator ↔ DuckLake Python integration | Engineering — ABI-matched load path + CI |
| **V3** | Live ducklake.select publish | Process — maintainer applies fork port package |

F1–F3e and R1–R3 **fork deliverables are already implemented**. This plan is only
for unfinished verification and the out-of-repo docs publish.

---

## Verdict

| ID | Priority | Owner shape | Blocker today |
|---|---|---|---|
| **V1** | P0 | Anyone with a full Catalogs-style build | Debug binary built without scanners |
| **V2** | P0 | Anyone who can align Python DuckDB with extension source-id | PyPI DuckDB ≠ built extension ABI |
| **V3** | P1 | Maintainer with write access to `duckdb/ducklake-web` (or a fork+PR) | This agent/environment cannot push upstream |

Ship order: **V1 ∥ V2 → V3** (publish after migrator/docs package is integration-proven).

---

## V1 — Postgres / SQLite / Quack matrix runs

### Problem

[`scripts/run_unsupported_catalog_matrix.sh`](../scripts/run_unsupported_catalog_matrix.sh)
and [`CATALOG_MATRIX_UNSUPPORTED.md`](CATALOG_MATRIX_UNSUPPORTED.md) exist. Only
the **DuckDB** catalog column is green. SQLite / Postgres / Quack were **SKIP**
because `build/debug/duckdb` was compiled without:

- `ENABLE_SQLITE_SCANNER=ON`
- `ENABLE_POSTGRES_SCANNER=ON`
- `ENABLE_QUACK=ON`

CI already builds that way in [`.github/workflows/Catalogs.yml`](../.github/workflows/Catalogs.yml).

### Goal

Fill the matrix table with real PASS/FAIL for SQLite + Postgres (+ Quack when
available), fix any suite-specific failures, and keep the doc current.

### Task breakdown

1. **Rebuild** (match Catalogs.yml):
   ```bash
   export CXX=g++ CC=gcc
   export ENABLE_SQLITE_SCANNER=ON
   export ENABLE_POSTGRES_SCANNER=ON
   export ENABLE_QUACK=ON
   export BUILD_EXTENSION_TEST_DEPS=full
   export VCPKG_TOOLCHAIN_PATH=$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake
   export VCPKG_ROOT=$HOME/vcpkg
   make debug -j$(nproc)   # or make release
   ```
2. **Confirm scanners**:
   ```bash
   ./build/debug/duckdb -unsigned -c \
     "SELECT extension_name, installed, loaded FROM duckdb_extensions()
      WHERE extension_name IN ('sqlite_scanner','postgres_scanner','quack','ducklake');"
   ```
3. **Postgres prep** (local):
   ```bash
   export PGHOST=127.0.0.1 PGUSER=postgres PGPASSWORD=postgres PGPORT=5432
   createdb ducklakedb 2>/dev/null || true
   ```
4. **Run matrix**:
   ```bash
   BUILD=build/debug bash scripts/run_unsupported_catalog_matrix.sh
   ```
5. **Quack path** (if extension present): either the helper’s quack config arm, or
   a focused filter via:
   ```bash
   python3 scripts/run_quack_tests.py --filter 'default/nested_defaults.test'
   # repeat for check_enforce, formal_metadata, array, enum, generated_columns
   ```
   Prefer documenting Quack as “PASS via `run_quack_tests.py --filter …`” if the
   helper’s in-process quack config is insufficient (sidecar lifecycle).
6. **Triage failures** (priority order):
   - `METADATA_CATALOG 'xx'` / search_path on Postgres (`formal_metadata`,
     `nested_defaults`)
   - ARRAY/ENUM inlining as VARCHAR on PG/SQLite (assert text fallback, don’t
     expect native types)
   - Identifier limits / locking → `skip_tests` in
     `test/configs/{postgres,sqlite}.json` **only** with a tracked reason
7. **Update** [`CATALOG_MATRIX_UNSUPPORTED.md`](CATALOG_MATRIX_UNSUPPORTED.md)
   with date, build flags, and PASS/FAIL/SKIP cells.
8. **CI evidence:** green `Catalogs.yml` on the PR that lands fixes, or a
   workflow step that runs the unsupported filter list first for faster signal.

### Exit criteria

- SQLite + Postgres columns filled for all core filters (not SKIP for “scanner
  missing”).
- Quack: PASS for core filters, or explicit SKIP with “extension/sidecar N/A”
  reason after attempting `ENABLE_QUACK=ON` build.
- Any new `skip_tests` entries have reasons linked to an issue/comment.
- Docs table matches the last successful run.

### Risks

- Full scanner rebuild is heavy (time/disk).
- Quack needs out-of-process sidecar (`run_quack_tests.py`); don’t treat a
  missing sidecar as a product bug.
- `formal_metadata.test` may need a small Postgres-friendly attach tweak.

---

## V2 — Migrator ↔ DuckLake Python integration

### Problem

[`scripts/tests/test_duckdb_to_ducklake_migrate.py`](../scripts/tests/test_duckdb_to_ducklake_migrate.py)
skips the full DuckLake integration when:

```text
extension built for DuckDB '0cd9e82950'
Python duckdb package is '5319116087' (or other PyPI build)
```

Unit + schema-aware tests already pass. What’s missing is an end-to-end run:

source DuckDB → `duckdb_to_ducklake_migrate.py` → DuckLake attach → assert
ARRAY / ENUM / generated preserved.

### Goal

One reliable integration path that loads **this repo’s** `ducklake` extension
into a Python DuckDB whose source-id matches, and runs in CI or a documented
local recipe.

### Options (pick one primary; keep others as fallback)

| Option | Approach | Pros | Cons |
|---|---|---|---|
| **A (preferred)** | Build Python duckdb from the pinned `duckdb/` submodule (`PIP_NO_BINARY`, same commit as `.github/duckdb-version`) and `LOAD` local extension | True ABI match | Slower setup |
| **B** | CLI-driven smoke: shell out to `./build/debug/duckdb` to create source + destination, invoke migrator only if py can LOAD, else duplicate asserts in SQL | Always runnable with current binary | Migrator script itself not executed under mismatch |
| **C** | `INSTALL ducklake FROM community` / nightly matching PyPI | Easy | May not include fork features (ARRAY/ENUM/formal tables) |

**Primary plan: A + thin B gate.**

### Task breakdown

1. **Document recipe** in `scripts/tests/README.md` (new):
   ```bash
   # From repo root, after make debug with ducklake built:
   python3 -m pip install -U pip
   python3 -m pip install --no-binary=duckdb \
     "duckdb @ git+file://$PWD/duckdb"   # or matching commit URL
   export DUCKLAKE_EXTENSION_PATH=$PWD/build/debug/extension/ducklake/ducklake.duckdb_extension
   export LOCAL_EXTENSION_REPO=$PWD/build/debug/repository
   python3 scripts/tests/test_duckdb_to_ducklake_migrate.py -v
   ```
2. **Harden loader** in `load_ducklake()`:
   - Prefer `DUCKLAKE_EXTENSION_PATH`
   - On ABI error, print actionable rebuild/pip instructions (not a generic skip)
   - Optional: detect `duckdb.__git_hash__` / source-id vs extension metadata and
     skip with `reason=` that names both hashes
3. **Unskip integration** when hashes match; assert:
   - `typeof(a)` → `INTEGER[3]` (or equivalent)
   - ENUM / named `mood` preserved
   - generated column recomputes / present as generated
   - UNION still VARCHAR under default (non-legacy) path
4. **CLI fallback smoke** `scripts/tests/test_migrate_cli_smoke.sh`:
   - Uses `./build/debug/duckdb -unsigned` only
   - Creates source tables with ARRAY/ENUM/generated/UNION
   - Manually applies the same SQL the migrator would emit (or calls Python if
     load works)
   - Guarantees a green signal even when pip-duckdb can’t be rebuilt
5. **CI (optional but recommended):**
   - New job or Catalogs.yml step: `pip install` submodule duckdb → run
     integration test with `DUCKLAKE_EXTENSION_PATH`
   - Or mark integration as `continue-on-error: false` only on that job

### Exit criteria

- Integration test **runs** (not skip) in at least one documented environment.
- Assertions cover ARRAY, ENUM, generated, UNION-cast.
- Failure mode on ABI mismatch is explicit and actionable.
- README documents the one supported recipe.

### Risks

- Building `duckdb` Python from source is slow and needs compilers.
- Fork-only features won’t appear on community extension (don’t use Option C for
  acceptance of fork behavior).

---

## V3 — Live ducklake.select publish (process)

### Problem

The fork port package under [`docs/ducklake-web/`](ducklake-web/README.md) is
ready (branching + types/migration/unsupported patches). Publishing to
[ducklake.select](https://ducklake.select) requires changing
[`duckdb/ducklake-web`](https://github.com/duckdb/ducklake-web), which this
cloud agent **cannot** push to.

This is not an extension coding gap — it’s an **upstream docs ownership** step.

### How we handle it

Treat V3 as a **checklist + handoff**, not as blocked product work.

```text
Fork (done)                    Maintainer (required)              Live site
─────────────────────────      ───────────────────────────       ─────────
docs/ducklake-web/ package  →  fork/PR to duckdb/ducklake-web →  ducklake.select
tracking issue text         →  review + lint + merge          →  URLs in README
```

### Task breakdown (maintainer / human)

1. **Open tracking issue** on `duckdb/ducklake-web` (or use Needs Documentation
   workflow):
   - Title: `[ducklake] Document array/enum types + updated DuckDB migrator`
   - Body: link this repo’s `docs/ducklake-web/README.md`, note fork catalog
     `1.1-dev6` formal tables may be fork-only — publish type-string encodings
     first if needed
2. **Apply package** (from README):
   - Edit `data_types.md` using `data_types_ARRAY_ENUM_PATCH.md`
   - Replace/update `migrations/duckdb_to_ducklake.md`
   - Patch `unsupported_features.md`
   - (Separately) branching guide + menu if not already live
3. **Run** `./scripts/lint.sh` in `ducklake-web`; open PR against
   `duckdb/ducklake-web`.
4. **Versioning honesty:**
   - Always publish: `array(N)`, `enum('…')`, migrator preserve behavior for
     engines that support them
   - Gate formal `ducklake_type` / `ducklake_table_constraint` / `generated_*`
     prose on upstream catalog version adoption — or mark “fork ≥ 1.1-dev6”
5. **After merge:** update this fork’s [`docs/README.md`](README.md) with live
   URLs; mark V3 Done in this plan with links.

### What the fork does without maintainer access

| Do | Don’t |
|---|---|
| Keep `docs/ducklake-web/` in sync with `SPEC_DATA_TYPES.md` + migrator | Claim ducklake.select already shows ARRAY/ENUM |
| File/link a tracking issue from the PR description | Block extension releases on upstream docs merge |
| Offer a ready-to-apply patch package | Require agents to push to `duckdb/ducklake-web` |

### Exit criteria

- Tracking issue or upstream PR exists and is linked from this plan / README.
- After merge: live data-types + migration pages match the package; this plan
  records the URLs.

### If upstream is slow

Ship the fork docs as canonical (`SPEC_DATA_TYPES.md`, in-repo migration page
under `docs/ducklake-web/…`) and keep a single “Pending upstream publish”
line in [`docs/README.md`](README.md). Product behavior does not wait on the
website.

---

## Suggested ship order

```text
V1  Rebuild with scanners → run matrix → fix → update CATALOG_MATRIX_UNSUPPORTED.md
V2  ABI-matched Python recipe + unskip integration (+ optional CLI smoke)
V3  Maintainer handoff: duckdb/ducklake-web issue/PR from docs/ducklake-web/
```

V1 and V2 are parallelizable. V3 should follow R1 docs accuracy (already true)
and ideally a green V2 so the published migrator script is integration-proven.

---

## Implementation status

| ID | Status | Notes |
|---|---|---|
| **R1–R3** | Done (fork) | Migrator, matrix helper, ducklake-web package — see follow-up plan / PR #17 |
| **V1** | Open | Need scanner-enabled rebuild + matrix fill-in |
| **V2** | Open | Need ABI-matched DuckLake py integration (or CLI smoke) |
| **V3** | Open (handoff) | Package ready; upstream publish is maintainer-owned |

---

## Recommendation

Do **V1 and V2** as one engineering PR on this fork (rebuild instructions + CI
hooks + integration test recipe). Handle **V3** as a short maintainer checklist
attached to that PR / a `ducklake-web` issue — do not block lake features on the
docs site going live.
