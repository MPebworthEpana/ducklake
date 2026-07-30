#!/usr/bin/env python3
"""Migrate a DuckDB catalog to DuckLake, preserving ARRAY / ENUM / generated columns.

Vendored from https://ducklake.select/docs/stable/duckdb/migrations/duckdb_to_ducklake
with fork-specific behavior (see docs/FOLLOWUP_FEATURES_PLAN.md R1):

- Fixed-size ARRAY (T[N]) and ENUM are preserved by default (use --legacy-casts
  to restore upstream ARRAY→list and ENUM→VARCHAR casts).
- UNION / VARINT / BIT still cast to DuckLake-supported types.
- User-defined ENUM / STRUCT types are created on the destination before tables.
- Prefer schema-aware CREATE TABLE + INSERT (generated / DEFAULT); fall back to CTAS.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
import warnings
from collections import deque
from typing import Any, Optional

import duckdb

# Cast suffixes applied in SELECT lists (and for destination type inference).
# DuckDB ≥1.6 may report VARINT as BIGNUM in duckdb_columns().
TYPE_MAPPING = {
    "VARINT": "::VARCHAR::INT",
    "BIGNUM": "::VARCHAR::INT",
    "UNION": "::VARCHAR",
    "BIT": "::VARCHAR",
    "ENUM": "::VARCHAR",  # only when --legacy-casts
}

FIXED_ARRAY_RE = re.compile(r"^(.+)\[(\d+)\]$")
ENUM_RE = re.compile(r"^ENUM\b", re.IGNORECASE)
UNION_RE = re.compile(r"^UNION\b", re.IGNORECASE)


def is_fixed_size_array(col_type: str) -> bool:
    """True for fixed-size arrays like INTEGER[3], VARCHAR[2], STRUCT(...)[4]."""
    return bool(FIXED_ARRAY_RE.match(col_type))


def is_enum_type(col_type: str) -> bool:
    return bool(ENUM_RE.match(col_type))


def is_union_type(col_type: str) -> bool:
    return bool(UNION_RE.match(col_type))


def cast_for_type(col_type: str, legacy_casts: bool = False) -> Optional[str]:
    """Return a SQL cast suffix (e.g. '::VARCHAR') or None to preserve the type."""
    if col_type in ("VARINT", "BIGNUM"):
        return TYPE_MAPPING[col_type]
    if col_type == "BIT":
        return TYPE_MAPPING["BIT"]
    if is_union_type(col_type):
        return TYPE_MAPPING["UNION"]
    if legacy_casts and is_enum_type(col_type):
        return TYPE_MAPPING["ENUM"]
    if legacy_casts and is_fixed_size_array(col_type):
        base = FIXED_ARRAY_RE.match(col_type).group(1)
        return f"::{base}[]"
    return None


def destination_type(col_type: str, cast_suffix: Optional[str]) -> str:
    """Logical destination column type after applying cast_suffix (if any)."""
    if cast_suffix is None:
        return col_type
    if cast_suffix == "::VARCHAR":
        return "VARCHAR"
    if cast_suffix == "::VARCHAR::INT":
        return "INTEGER"
    if cast_suffix.startswith("::") and cast_suffix.endswith("[]"):
        return cast_suffix[2:]
    return "VARCHAR"


def _resolve_data_types(
    table: str,
    schema: str,
    catalog: str,
    conn: duckdb.DuckDBPyConnection,
    legacy_casts: bool = False,
):
    """Collect EXCLUDE names and cast expressions for unsupported source types."""
    excepts: list[str] = []
    casts: list[str] = []
    for col_name, col_type in conn.execute(
        f"SELECT column_name, data_type FROM duckdb_columns() "
        f"WHERE table_name = '{table}' AND schema_name = '{schema}' "
        f"AND database_name = '{catalog}' ORDER BY column_index"
    ).fetchall():
        cast = cast_for_type(col_type, legacy_casts=legacy_casts)
        if cast:
            casts.append(f"{col_name}{cast} AS {col_name}")
            excepts.append(col_name)
    return excepts, casts


def get_postgres_secret() -> str:
    return f"""
        CREATE SECRET postgres_secret(
            TYPE postgres,
            HOST '{os.getenv("POSTGRES_HOST", "localhost")}',
            PORT {os.getenv("POSTGRES_PORT", "5432")},
            DATABASE {os.getenv("POSTGRES_DB", "migration_test")},
            USER '{os.getenv("POSTGRES_USER", "user")}',
            PASSWORD '{os.getenv("POSTGRES_PASSWORD", "simple")}'
        );"""


def _python_duckdb_identity() -> str:
    """Describe the imported duckdb package for ABI mismatch diagnostics."""
    parts = [
        f"version={getattr(duckdb, '__version__', '?')}",
        f"file={getattr(duckdb, '__file__', '?')}",
    ]
    git_rev = getattr(duckdb, "__git_revision__", None) or getattr(
        duckdb, "__git_hash__", None
    )
    if git_rev:
        parts.insert(1, f"source_id={git_rev}")
    return ", ".join(parts)


def _abi_mismatch_hint(ext_path: Optional[str], err: BaseException) -> str:
    """Actionable message when LOAD fails due to DuckDB source-id / ABI mismatch."""
    lines = [
        f"Failed to LOAD ducklake extension: {err}",
        f"  Python duckdb: {_python_duckdb_identity()}",
    ]
    if ext_path:
        lines.append(f"  Extension path: {ext_path}")
    lines.extend(
        [
            "  The extension must be built for the same DuckDB source-id as the",
            "  Python package. Rebuild Python duckdb against the pinned DuckDB",
            "  commit in .github/duckdb-version (bindings live in duckdb-python;",
            "  the duckdb/ submodule no longer vendors tools/pythonpkg), then:",
            "    export DUCKLAKE_EXTENSION_PATH="
            "$PWD/build/debug/extension/ducklake/ducklake.duckdb_extension",
            "  See scripts/tests/README.md for the ABI-matched recipe.",
            "  Fallback (no Python rebuild): bash scripts/tests/test_migrate_cli_smoke.sh",
        ]
    )
    return "\n".join(lines)


def load_ducklake(con: duckdb.DuckDBPyConnection) -> None:
    """LOAD the ducklake extension using env overrides or INSTALL/LOAD.

    Prefer ``DUCKLAKE_EXTENSION_PATH``. On ABI / source-id mismatch, raise with
    an actionable message naming both the Python duckdb identity and extension path.
    """
    ext_path = os.environ.get("DUCKLAKE_EXTENSION_PATH")
    if ext_path:
        try:
            con.execute(f"LOAD '{ext_path}'")
            return
        except Exception as e:
            msg = str(e)
            if "built specifically for DuckDB version" in msg or "can only be loaded" in msg:
                raise RuntimeError(_abi_mismatch_hint(ext_path, e)) from e
            raise RuntimeError(
                f"Failed to LOAD '{ext_path}': {e}\n"
                f"  Python duckdb: {_python_duckdb_identity()}"
            ) from e

    local_repo = os.environ.get("LOCAL_EXTENSION_REPO")
    if local_repo:
        con.execute(f"SET custom_extension_repository = '{local_repo}'")
        try:
            con.execute("SET allow_unsigned_extensions = true")
        except Exception:
            pass
        try:
            con.execute("LOAD ducklake")
            return
        except Exception:
            try:
                con.execute("INSTALL ducklake")
                con.execute("LOAD ducklake")
                return
            except Exception as e:
                msg = str(e)
                if "built specifically for DuckDB version" in msg or "can only be loaded" in msg:
                    raise RuntimeError(_abi_mismatch_hint(local_repo, e)) from e
                pass

    try:
        con.execute("INSTALL ducklake FROM 'community'")
    except Exception:
        pass
    try:
        con.execute("INSTALL ducklake")
    except Exception:
        pass
    try:
        con.execute("LOAD ducklake")
    except Exception as e:
        msg = str(e)
        if "built specifically for DuckDB version" in msg or "can only be loaded" in msg:
            raise RuntimeError(_abi_mismatch_hint(ext_path or "(community/default)", e)) from e
        raise


def _duckdb_columns_has_generated(con: duckdb.DuckDBPyConnection) -> bool:
    cols = {
        row[0]
        for row in con.execute(
            "SELECT column_name FROM (DESCRIBE SELECT * FROM duckdb_columns())"
        ).fetchall()
    }
    return "is_generated" in cols and "generation_expression" in cols


def fetch_table_columns(
    con: duckdb.DuckDBPyConnection, catalog: str, schema: str, table: str
) -> list[dict[str, Any]]:
    """Return ordered column metadata for schema-aware CREATE TABLE."""
    if _duckdb_columns_has_generated(con):
        rows = con.execute(
            f"SELECT column_name, data_type, is_generated, generation_expression, "
            f"column_default, is_nullable FROM duckdb_columns() "
            f"WHERE database_name = '{catalog}' AND schema_name = '{schema}' "
            f"AND table_name = '{table}' ORDER BY column_index"
        ).fetchall()
        return [
            {
                "column_name": r[0],
                "data_type": r[1],
                "is_generated": bool(r[2]),
                "generation_expression": r[3],
                "column_default": r[4],
                "is_nullable": bool(r[5]) if r[5] is not None else True,
            }
            for r in rows
        ]

    # Older DuckDB: no is_generated — infer from duckdb_tables().sql when possible.
    rows = con.execute(
        f"SELECT column_name, data_type, column_default, is_nullable FROM duckdb_columns() "
        f"WHERE database_name = '{catalog}' AND schema_name = '{schema}' "
        f"AND table_name = '{table}' ORDER BY column_index"
    ).fetchall()
    create_sql = con.execute(
        f"SELECT sql FROM duckdb_tables() WHERE database_name = '{catalog}' "
        f"AND schema_name = '{schema}' AND table_name = '{table}'"
    ).fetchone()
    sql_text = create_sql[0] if create_sql else ""
    generated: dict[str, str] = {}
    for match in re.finditer(
        r"(\w+)\s+\w[^,]*?\s+GENERATED\s+ALWAYS\s+AS\s*\((.+?)\)(?:\s+(?:VIRTUAL|STORED))?",
        sql_text or "",
        flags=re.IGNORECASE,
    ):
        generated[match.group(1)] = match.group(2)

    result = []
    for name, dtype, default, nullable in rows:
        is_gen = name in generated
        result.append(
            {
                "column_name": name,
                "data_type": dtype,
                "is_generated": is_gen,
                "generation_expression": generated.get(name) or (default if is_gen else None),
                "column_default": None if is_gen else default,
                "is_nullable": bool(nullable) if nullable is not None else True,
            }
        )
    return result


def fetch_user_types(
    con: duckdb.DuckDBPyConnection, catalog: str
) -> list[tuple[str, str, str, Any]]:
    """User-defined ENUM / STRUCT types in the source catalog (skip system types)."""
    return con.execute(
        f"SELECT schema_name, type_name, logical_type, labels "
        f"FROM duckdb_types() "
        f"WHERE database_name = '{catalog}' AND NOT internal "
        f"AND logical_type IN ('ENUM', 'STRUCT') "
        f"ORDER BY type_oid"
    ).fetchall()


def build_named_type_lookup(
    con: duckdb.DuckDBPyConnection, catalog: str
) -> dict[str, str]:
    """Map expanded type strings (ENUM(...)/STRUCT(...)) → qualified type name."""
    lookup: dict[str, str] = {}
    for schema_name, type_name, logical_type, labels in fetch_user_types(con, catalog):
        qualified = type_name if schema_name == "main" else f"{schema_name}.{type_name}"
        if logical_type == "ENUM" and labels is not None:
            label_list = list(labels)
            inner = ", ".join("'" + str(lab).replace("'", "''") + "'" for lab in label_list)
            lookup[f"ENUM({inner})"] = qualified
        elif logical_type == "STRUCT":
            typedef = con.execute(
                f"SELECT typeof(NULL::{catalog}.{schema_name}.{type_name})"
            ).fetchone()[0]
            lookup[typedef] = qualified
    return lookup


def migrate_user_types(
    con: duckdb.DuckDBPyConnection, duckdb_catalog: str
) -> None:
    """CREATE TYPE on the current (DuckLake) catalog for source user types."""
    for schema_name, type_name, logical_type, labels in fetch_user_types(
        con, duckdb_catalog
    ):
        con.execute(f"CREATE SCHEMA IF NOT EXISTS {schema_name}")
        qualified = (
            type_name if schema_name == "main" else f"{schema_name}.{type_name}"
        )
        try:
            if logical_type == "ENUM":
                label_list = list(labels) if labels is not None else []
                inner = ", ".join(
                    "'" + str(lab).replace("'", "''") + "'" for lab in label_list
                )
                con.execute(
                    f"CREATE TYPE IF NOT EXISTS {qualified} AS ENUM ({inner})"
                )
            elif logical_type == "STRUCT":
                typedef = con.execute(
                    f"SELECT typeof(NULL::{duckdb_catalog}.{schema_name}.{type_name})"
                ).fetchone()[0]
                con.execute(f"CREATE TYPE IF NOT EXISTS {qualified} AS {typedef}")
            print(
                f"Migrating Type: {duckdb_catalog}.{schema_name}.{type_name} ({logical_type})"
            )
        except Exception as e:
            print(f"WARNING: Could not create type {qualified}: {e}")


def _format_default_expr(default: str) -> str:
    """Normalize a column_default for use in DEFAULT <expr>."""
    return default


def migrate_table_schema_aware(
    con: duckdb.DuckDBPyConnection,
    catalog: str,
    schema: str,
    table: str,
    legacy_casts: bool,
    named_types: dict[str, str],
) -> None:
    """CREATE TABLE with generated/DEFAULT, then INSERT non-generated columns."""
    columns = fetch_table_columns(con, catalog, schema, table)
    col_defs: list[str] = []
    insert_cols: list[str] = []
    select_exprs: list[str] = []

    for col in columns:
        name = col["column_name"]
        dtype = col["data_type"]
        cast = cast_for_type(dtype, legacy_casts=legacy_casts)
        dest = named_types.get(dtype) or destination_type(dtype, cast)

        if col["is_generated"] and col["generation_expression"]:
            expr = col["generation_expression"]
            col_defs.append(f"{name} {dest} AS ({expr})")
            continue

        pieces = [name, dest]
        if col["column_default"] is not None:
            pieces.append(f"DEFAULT {_format_default_expr(col['column_default'])}")
        if not col["is_nullable"]:
            pieces.append("NOT NULL")
        col_defs.append(" ".join(pieces))
        insert_cols.append(name)
        if cast:
            select_exprs.append(f"{name}{cast} AS {name}")
        else:
            select_exprs.append(name)

    con.execute(
        f"CREATE TABLE IF NOT EXISTS {schema}.{table} ({', '.join(col_defs)})"
    )
    if insert_cols:
        con.execute(
            f"INSERT INTO {schema}.{table} ({', '.join(insert_cols)}) "
            f"SELECT {', '.join(select_exprs)} FROM {catalog}.{schema}.{table}"
        )


def migrate_table_ctas(
    con: duckdb.DuckDBPyConnection,
    catalog: str,
    schema: str,
    table: str,
    legacy_casts: bool,
) -> None:
    """Fallback: CREATE TABLE AS SELECT with unsupported-type casts baked in."""
    excepts, casts = _resolve_data_types(
        table, schema, catalog, con, legacy_casts=legacy_casts
    )
    if casts:
        select_clause = (
            "* EXCLUDE(" + ", ".join(excepts) + "),\n" + ",\n".join(casts)
        )
        con.execute(
            f"CREATE TABLE IF NOT EXISTS {schema}.{table} AS "
            f"SELECT {select_clause} FROM {catalog}.{schema}.{table}"
        )
    else:
        con.execute(
            f"CREATE TABLE IF NOT EXISTS {schema}.{table} AS "
            f"SELECT * FROM {catalog}.{schema}.{table}"
        )


def migrate_tables_and_views(
    duckdb_catalog: str,
    con: duckdb.DuckDBPyConnection,
    legacy_casts: bool = False,
):
    """
    Migrate tables and views from the DuckDB catalog to DuckLake using a queue.
    Failed view migrations are re-queued until dependencies exist (or permanently skipped).
    """
    named_types = build_named_type_lookup(con, duckdb_catalog)

    rows = con.execute(
        f"SELECT table_catalog, table_schema, table_name, table_type "
        f"FROM information_schema.tables WHERE table_catalog = '{duckdb_catalog}'"
    ).fetchall()

    queue = deque(rows)
    failed_last_round: set[tuple] = set()

    while queue:
        catalog, schema, table, table_type = queue.popleft()
        con.execute(f"CREATE SCHEMA IF NOT EXISTS {schema}")
        try:
            if table_type == "VIEW":
                view_definition = con.execute(
                    f"SELECT view_definition FROM information_schema.views "
                    f"WHERE table_name = '{table}' AND table_schema = '{schema}' "
                    f"AND table_catalog = '{catalog}'"
                ).fetchone()[0]
                con.execute(
                    f"CREATE VIEW IF NOT EXISTS {view_definition.removeprefix('CREATE VIEW ')}"
                )
                print(f"Migrating Catalog: {catalog}, Schema: {schema}, View: {table}")
            else:
                try:
                    migrate_table_schema_aware(
                        con,
                        catalog,
                        schema,
                        table,
                        legacy_casts=legacy_casts,
                        named_types=named_types,
                    )
                except Exception as schema_err:
                    warnings.warn(
                        f"Schema-aware migrate failed for {schema}.{table} "
                        f"({schema_err}); falling back to CTAS bake-in",
                        stacklevel=1,
                    )
                    print(
                        f"WARNING: Schema-aware migrate failed for {schema}.{table}: "
                        f"{schema_err}; falling back to CTAS"
                    )
                    try:
                        con.execute(f"DROP TABLE IF EXISTS {schema}.{table}")
                    except Exception:
                        pass
                    migrate_table_ctas(
                        con, catalog, schema, table, legacy_casts=legacy_casts
                    )
                print(f"Migrating Catalog: {catalog}, Schema: {schema}, Table: {table}")
        except Exception as e:
            print(f"WARNING: Requeuing {table_type} {table}")
            key = (catalog, schema, table, table_type)
            if key in failed_last_round:
                print(
                    f"Skipping {table_type} {table} permanently due to repeated failure. {e}"
                )
                continue
            queue.append(key)
            failed_last_round.add(key)
        else:
            failed_last_round.discard((catalog, schema, table, table_type))


def migrate_macros(con: duckdb.DuckDBPyConnection, duckdb_catalog: str):
    """Migrate macros from the DuckDB catalog to the DuckLake metadata database."""
    for row in con.execute(
        f"SELECT function_name, parameters, macro_definition FROM duckdb_functions() "
        f"WHERE database_name='{duckdb_catalog}'"
    ).fetchall():
        name, parameters, definition = row[0], row[1], row[2]
        if definition is None:
            continue
        print(f"Migrating Macro: {name}")
        params = parameters if parameters is not None else []
        con.execute(
            f"CREATE OR REPLACE MACRO {name}({','.join(params)}) AS {definition}"
        )


def build_ducklake_secret(args: argparse.Namespace) -> str:
    return (
        "CREATE SECRET ducklake_secret (TYPE ducklake"
        + (
            f"\n,METADATA_PATH '{args.ducklake_file if args.catalog_type == 'duckdb' else f'sqlite:{args.ducklake_file}'}'"
            if args.catalog_type in ("duckdb", "sqlite")
            else "\n,METADATA_PATH ''"
        )
        + f"\n,DATA_PATH '{args.ducklake_data_path}'"
        + (
            "\n,METADATA_PARAMETERS MAP {'TYPE': 'postgres', 'SECRET': 'postgres_secret'});"
            if args.catalog_type == "postgresql"
            else ");"
        )
    )


def parse_args(argv: Optional[list[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Migrate DuckDB catalog to DuckLake.")
    parser.add_argument("--duckdb-catalog", required=True, help="DuckDB catalog name")
    parser.add_argument("--duckdb-file", required=True, help="Path to DuckDB file")
    parser.add_argument(
        "--ducklake-catalog", required=True, help="DuckLake catalog name"
    )
    parser.add_argument(
        "--catalog-type",
        choices=["duckdb", "postgresql", "sqlite"],
        required=True,
        help="Choose one of: duckdb, postgresql, sqlite",
    )
    parser.add_argument("--ducklake-file", required=False, help="Path to DuckLake file")
    parser.add_argument(
        "--ducklake-data-path", required=True, help="Data path for DuckLake"
    )
    parser.add_argument(
        "--legacy-casts",
        action="store_true",
        help="Restore upstream ARRAY→list and ENUM→VARCHAR casts",
    )
    return parser.parse_args(argv)


def run_migration(args: argparse.Namespace) -> None:
    con = duckdb.connect(database=args.duckdb_file)
    try:
        try:
            con.execute("SET allow_unsigned_extensions = true")
        except Exception:
            pass
        load_ducklake(con)

        if args.catalog_type == "postgresql":
            con.execute(get_postgres_secret())

        con.execute(build_ducklake_secret(args))
        con.execute(
            f"ATTACH '{args.duckdb_file}' AS {args.duckdb_catalog};"
            f"ATTACH 'ducklake:ducklake_secret' AS {args.ducklake_catalog}; "
            f"USE {args.ducklake_catalog};"
        )

        migrate_user_types(con, duckdb_catalog=args.duckdb_catalog)
        migrate_tables_and_views(
            duckdb_catalog=args.duckdb_catalog,
            con=con,
            legacy_casts=args.legacy_casts,
        )

        if args.catalog_type == "duckdb":
            con.execute(f"USE {args.duckdb_catalog}; DETACH {args.ducklake_catalog};")
            con.execute(
                f"ATTACH '{args.ducklake_file}' AS ducklake_metadata; USE ducklake_metadata;"
            )
            migrate_macros(con=con, duckdb_catalog=args.duckdb_catalog)
    finally:
        con.close()


def main(argv: Optional[list[str]] = None) -> int:
    args = parse_args(argv)
    if args.catalog_type in ("duckdb", "sqlite") and not args.ducklake_file:
        print("--ducklake-file is required for catalog-type duckdb/sqlite", file=sys.stderr)
        return 2
    run_migration(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
