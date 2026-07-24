#!/usr/bin/env bash
# Run branching + refs suites against DuckDB / SQLite / Postgres catalog backends.
# Mirrors Catalogs.yml filters for faster local signal on F3 work.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD="${BUILD:-build/debug}"
UNITTEST="${UNITTEST:-$BUILD/test/unittest}"
DUCKDB_BIN="${DUCKDB_BIN:-$BUILD/duckdb}"
FILTERS=('test/sql/branching/*' 'test/sql/refs/*')

if [[ ! -x "$UNITTEST" ]]; then
  echo "missing unittest at $UNITTEST (set BUILD= or UNITTEST=)" >&2
  exit 1
fi

run_filters() {
  local name="$1"
  local config="${2:-}"
  shift 2 || true
  local extra_env=("$@")
  echo "===== $name ====="
  for filter in "${FILTERS[@]}"; do
    echo "--- $filter ---"
    # shellcheck disable=SC2086
    env "${extra_env[@]}" "$UNITTEST" ${config:+--test-config "$config"} --test-dir ./ "$filter"
  done
}

has_ext() {
  local ext="$1"
  [[ -x "$DUCKDB_BIN" ]] || return 1
  "$DUCKDB_BIN" -unsigned -c "SELECT installed OR loaded FROM duckdb_extensions() WHERE extension_name='$ext';" 2>/dev/null \
    | grep -qi true
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
    unset PGSERVICE || true
    createdb ducklakedb 2>/dev/null || true
    run_filters "postgres-catalog" "test/configs/postgres.json"
  fi
else
  echo "SKIP postgres-catalog: postgres_scanner not built into $DUCKDB_BIN"
fi

echo "===== done ====="
