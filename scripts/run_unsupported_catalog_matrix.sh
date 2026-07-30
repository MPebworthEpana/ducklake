#!/usr/bin/env bash
# Run unsupported-features closeout suites against DuckDB / SQLite / Postgres
# catalog backends (optional Quack). Mirrors run_branching_catalog_matrix.sh
# for faster local signal on R2 / U1–U5 / F1–F3 work.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD="${BUILD:-build/debug}"
UNITTEST="${UNITTEST:-$BUILD/test/unittest}"
DUCKDB_BIN="${DUCKDB_BIN:-$BUILD/duckdb}"

# Core matrix filters (U1–U5 / F1–F3)
FILTERS=(
  'test/sql/default/nested_defaults.test'
  'test/sql/constraints/check_enforce.test'
  'test/sql/catalog/formal_metadata.test'
  'test/sql/types/array.test'
  'test/sql/types/enum.test'
  'test/sql/general/generated_columns.test'
)

# Optional related smoke (expression defaults, leftover unsupported, CASCADE)
FILTERS_SMOKE=(
  'test/sql/default/default_expressions.test'
  'test/sql/constraints/unsupported.test'
  'test/sql/catalog/drop_cascade.test'
)

# Set SKIP_SMOKE=1 to run only core FILTERS
SKIP_SMOKE="${SKIP_SMOKE:-0}"

if [[ ! -x "$UNITTEST" ]]; then
  echo "missing unittest at $UNITTEST (set BUILD= or UNITTEST=)" >&2
  exit 1
fi

all_filters() {
  printf '%s\n' "${FILTERS[@]}"
  if [[ "$SKIP_SMOKE" != "1" ]]; then
    printf '%s\n' "${FILTERS_SMOKE[@]}"
  fi
}

run_filters() {
  local name="$1"
  local config="${2:-}"
  shift
  # Drop optional config arg when present so leftover $@ are only env assignments
  if [[ $# -gt 0 && "$1" == "$config" && -n "$config" ]]; then
    shift
  fi
  local extra_env=("$@")
  echo "===== $name ====="
  while IFS= read -r filter; do
    [[ -z "$filter" ]] && continue
    echo "--- $filter ---"
    if [[ ${#extra_env[@]} -gt 0 ]]; then
      # shellcheck disable=SC2086
      env "${extra_env[@]}" "$UNITTEST" ${config:+--test-config "$config"} --test-dir ./ "$filter"
    else
      # shellcheck disable=SC2086
      "$UNITTEST" ${config:+--test-config "$config"} --test-dir ./ "$filter"
    fi
  done < <(all_filters)
}

has_ext() {
  local ext="$1"
  [[ -x "$DUCKDB_BIN" ]] || return 1
  if "$DUCKDB_BIN" -unsigned -c "SELECT installed OR loaded FROM duckdb_extensions() WHERE extension_name='$ext';" 2>/dev/null \
    | grep -qi true; then
    return 0
  fi
  # Loadable extension artifact present in this build (not always linked into the binary)
  if [[ -f "$BUILD/extension/${ext}/${ext}.duckdb_extension" ]]; then
    return 0
  fi
  if [[ -d "$BUILD/repository" ]] && find "$BUILD/repository" -name "${ext}*.duckdb_extension" 2>/dev/null | grep -q .; then
    return 0
  fi
  return 1
}

run_filters "duckdb-catalog"

if has_ext sqlite_scanner || has_ext sqlite; then
  run_filters "sqlite-catalog" "test/configs/sqlite.json"
else
  echo "SKIP sqlite-catalog: sqlite_scanner not built into $DUCKDB_BIN"
fi

if has_ext postgres_scanner || has_ext postgres; then
  if ! command -v psql >/dev/null; then
    echo "SKIP postgres-catalog: psql not available"
  else
    export PGHOST="${PGHOST:-127.0.0.1}"
    export PGUSER="${PGUSER:-postgres}"
    export PGPASSWORD="${PGPASSWORD:-postgres}"
    export PGPORT="${PGPORT:-5432}"
    export DUCKLAKE_CI="${DUCKLAKE_CI:-1}"
    # Unittest loads postgres_scanner from the local build repository
    export LOCAL_EXTENSION_REPO="${LOCAL_EXTENSION_REPO:-$ROOT/$BUILD/repository}"
    unset PGSERVICE || true
    createdb ducklakedb 2>/dev/null || true
    run_filters "postgres-catalog" "test/configs/postgres.json" \
      "LOCAL_EXTENSION_REPO=$LOCAL_EXTENSION_REPO" \
      "PGHOST=$PGHOST" "PGUSER=$PGUSER" "PGPASSWORD=$PGPASSWORD" "PGPORT=$PGPORT" "DUCKLAKE_CI=$DUCKLAKE_CI"
  fi
else
  echo "SKIP postgres-catalog: postgres_scanner not built into $DUCKDB_BIN"
fi

# Optional Quack: config present + extension loaded/installed
if [[ -f "test/configs/quack.json" ]] && { has_ext quack || has_ext quack_scanner; }; then
  run_filters "quack-catalog" "test/configs/quack.json"
elif [[ -f "test/configs/quack.json" ]]; then
  echo "SKIP quack-catalog: test/configs/quack.json present but quack extension not built into $DUCKDB_BIN"
else
  echo "SKIP quack-catalog: test/configs/quack.json not found"
fi

echo "===== done ====="
