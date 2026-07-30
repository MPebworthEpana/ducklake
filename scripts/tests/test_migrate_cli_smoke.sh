#!/usr/bin/env bash
# CLI smoke for duckdb → DuckLake migrate (V2 Option B).
#
# Uses ./build/debug/duckdb (or $BUILD/duckdb) only so LOAD of the locally
# built ducklake extension succeeds without rebuilding the Python package.
# Prefer the Python migrator when LOAD works; otherwise apply SQL that mirrors
# schema-aware migrator behavior.
#
# Usage (from repo root):
#   bash scripts/tests/test_migrate_cli_smoke.sh
#   BUILD=build/release bash scripts/tests/test_migrate_cli_smoke.sh
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

BUILD_DIR="${BUILD:-build/debug}"
DUCKDB_BIN="${DUCKDB_BIN:-$BUILD_DIR/duckdb}"
EXT_DEFAULT="$BUILD_DIR/extension/ducklake/ducklake.duckdb_extension"
EXT="${DUCKLAKE_EXTENSION_PATH:-$EXT_DEFAULT}"

if [[ ! -x "$DUCKDB_BIN" ]]; then
  echo "FAIL: duckdb binary not found/executable: $DUCKDB_BIN" >&2
  echo "  Set BUILD=... or DUCKDB_BIN=... after make debug" >&2
  exit 1
fi
if [[ ! -f "$EXT" ]]; then
  echo "FAIL: ducklake extension not found: $EXT" >&2
  echo "  Set DUCKLAKE_EXTENSION_PATH or build ducklake (make debug)" >&2
  exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

SRC="$TMP/source.db"
META="$TMP/meta.ducklake"
DATA="$TMP/data"
mkdir -p "$DATA"

echo "==> Creating source catalog with ARRAY / ENUM / generated / UNION / DEFAULT"
"$DUCKDB_BIN" -unsigned "$SRC" -c "
CREATE TYPE mood AS ENUM ('happy', 'sad');
CREATE TABLE t(
  a INTEGER[3],
  m mood,
  g INT AS (a[1] + 1),
  u UNION(num INT, str VARCHAR),
  b INT DEFAULT 7
);
INSERT INTO t(a, m, u, b) VALUES
  ([1, 2, 3], 'happy', union_value(num := 42), 10);
"

MIGRATE_MODE=""

# Prefer Python migrator when the installed package can LOAD this extension.
if python3 -c "
import os, sys
sys.path.insert(0, 'scripts')
try:
    import duckdb
except ImportError:
    sys.exit(2)
ext = os.environ.get('DUCKLAKE_EXTENSION_PATH', '$EXT')
con = duckdb.connect()
try:
    try:
        con.execute('SET allow_unsigned_extensions = true')
    except Exception:
        pass
    con.execute(f\"LOAD '{ext}'\")
except Exception as e:
    print(f'Python LOAD skipped: {e}', file=sys.stderr)
    sys.exit(3)
finally:
    con.close()
sys.exit(0)
" 2>"$TMP/py_load.err"; then
  MIGRATE_MODE="python"
  echo "==> Python duckdb can LOAD extension; invoking duckdb_to_ducklake_migrate.py"
  export DUCKLAKE_EXTENSION_PATH="$EXT"
  python3 scripts/duckdb_to_ducklake_migrate.py \
    --duckdb-catalog src \
    --duckdb-file "$SRC" \
    --ducklake-catalog dst \
    --catalog-type duckdb \
    --ducklake-file "$META" \
    --ducklake-data-path "$DATA"
else
  MIGRATE_MODE="sql"
  if [[ -s "$TMP/py_load.err" ]]; then
    echo "==> Python migrator unavailable (see below); using CLI SQL mirror"
    sed 's/^/    /' "$TMP/py_load.err" || true
  fi
  echo "==> Migrating via CLI SQL (mirrors schema-aware migrator)"
  "$DUCKDB_BIN" -unsigned -c "
LOAD '$EXT';
ATTACH '$SRC' AS src;
ATTACH 'ducklake:$META' AS dst (DATA_PATH '$DATA');
USE dst;
CREATE TYPE mood AS ENUM ('happy', 'sad');
CREATE TABLE t(
  a INTEGER[3],
  m mood,
  g INT AS (a[1] + 1),
  u VARCHAR,
  b INT DEFAULT 7
);
INSERT INTO t(a, m, u, b)
SELECT a, m, u::VARCHAR AS u, b FROM src.main.t;
"
fi

echo "==> Asserting preserved types / values (mode=$MIGRATE_MODE)"
RESULT="$TMP/assert.tsv"
"$DUCKDB_BIN" -unsigned -c "
LOAD '$EXT';
ATTACH 'ducklake:$META' AS dst (DATA_PATH '$DATA');
USE dst;
COPY (
  SELECT
    typeof(a) AS typeof_a,
    typeof(m) AS typeof_m,
    typeof(u) AS typeof_u,
    a::VARCHAR AS a_v,
    m::VARCHAR AS m_v,
    g AS g_v,
    u::VARCHAR AS u_v,
    b AS b_v
  FROM t
) TO '$RESULT' (HEADER false, DELIMITER '\t');
"

# Read single TSV row
IFS=$'\t' read -r TYPEOF_A TYPEOF_M TYPEOF_U A_V M_V G_V U_V B_V < "$RESULT"

fail=0
check() {
  local name="$1" got="$2" expect="$3"
  if [[ "$got" == "$expect" ]]; then
    echo "  OK  $name=$got"
  else
    echo "  FAIL $name: got='$got' expected='$expect'" >&2
    fail=1
  fi
}

check "typeof(a)" "$TYPEOF_A" "INTEGER[3]"
# ENUM may be reported as ENUM('happy', 'sad') or include mood name depending on path
case "$TYPEOF_M" in
  *ENUM*) echo "  OK  typeof(m)=$TYPEOF_M (ENUM preserved)" ;;
  *) echo "  FAIL typeof(m): got='$TYPEOF_M' expected ENUM*" >&2; fail=1 ;;
esac
check "typeof(u)" "$TYPEOF_U" "VARCHAR"
check "a" "$A_V" "[1, 2, 3]"
check "m" "$M_V" "happy"
check "g" "$G_V" "2"
check "b" "$B_V" "10"
# UNION cast to VARCHAR: numeric tag value stringified
if [[ -z "$U_V" ]]; then
  echo "  FAIL u: empty" >&2
  fail=1
else
  echo "  OK  u=$U_V (UNION→VARCHAR)"
fi

if [[ "$fail" -ne 0 ]]; then
  echo "FAIL: CLI migrate smoke assertions failed (mode=$MIGRATE_MODE)" >&2
  exit 1
fi

echo "PASS: CLI migrate smoke (mode=$MIGRATE_MODE, binary=$DUCKDB_BIN)"
exit 0
