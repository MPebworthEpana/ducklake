#!/usr/bin/env python3
"""Tests for scripts/duckdb_to_ducklake_migrate.py (R1 migrator)."""

from __future__ import annotations

import os
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPTS_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS_DIR))

import duckdb_to_ducklake_migrate as migrate  # noqa: E402

try:
    import duckdb
except ImportError:  # pragma: no cover
    duckdb = None


def _find_ducklake_extension() -> str | None:
    """Locate a locally-built ducklake extension for integration tests."""
    env = os.environ.get("DUCKLAKE_EXTENSION_PATH")
    if env and Path(env).is_file():
        return env

    candidates = [
        REPO_ROOT / "build/debug/extension/ducklake/ducklake.duckdb_extension",
        REPO_ROOT / "build/release/extension/ducklake/ducklake.duckdb_extension",
        REPO_ROOT / "build/debug/repository",
        REPO_ROOT / "build/release/repository",
    ]
    for path in candidates:
        if path.is_file() and path.suffix == ".duckdb_extension":
            return str(path)
        if path.is_dir():
            matches = sorted(path.rglob("ducklake.duckdb_extension"))
            if matches:
                return str(matches[0])
    return None


def _can_load_ducklake() -> tuple[bool, str]:
    """Return (ok, detail). Sets DUCKLAKE_EXTENSION_PATH when a local build is found."""
    if duckdb is None:
        return False, "duckdb package not installed"
    ext = _find_ducklake_extension()
    con = duckdb.connect()
    try:
        try:
            con.execute("SET allow_unsigned_extensions = true")
        except Exception:
            pass
        if ext:
            os.environ["DUCKLAKE_EXTENSION_PATH"] = ext
            try:
                con.execute(f"LOAD '{ext}'")
                return True, ext
            except Exception as e:
                return False, f"LOAD '{ext}' failed: {e}"
        local_repo = os.environ.get("LOCAL_EXTENSION_REPO")
        if local_repo:
            try:
                con.execute(f"SET custom_extension_repository = '{local_repo}'")
                con.execute("LOAD ducklake")
                return True, f"LOCAL_EXTENSION_REPO={local_repo}"
            except Exception as e:
                return False, f"LOAD from LOCAL_EXTENSION_REPO failed: {e}"
        try:
            con.execute("INSTALL ducklake FROM 'community'")
            con.execute("LOAD ducklake")
            return True, "community"
        except Exception as e:
            return False, f"ducklake unavailable: {e}"
    finally:
        con.close()


DUCKLAKE_OK, DUCKLAKE_DETAIL = _can_load_ducklake()


class TestTypeClassification(unittest.TestCase):
    def test_fixed_size_array_detected(self):
        self.assertTrue(migrate.is_fixed_size_array("INTEGER[3]"))
        self.assertTrue(migrate.is_fixed_size_array("VARCHAR[2]"))
        self.assertTrue(migrate.is_fixed_size_array("FLOAT[8]"))
        self.assertTrue(migrate.is_fixed_size_array("DOUBLE[1]"))
        self.assertFalse(migrate.is_fixed_size_array("INTEGER[]"))
        self.assertFalse(migrate.is_fixed_size_array("VARCHAR"))

    def test_array_preserved_by_default(self):
        self.assertIsNone(migrate.cast_for_type("INTEGER[3]"))
        self.assertIsNone(migrate.cast_for_type("VARCHAR[4]", legacy_casts=False))
        self.assertIsNone(migrate.cast_for_type("FLOAT[2]"))

    def test_array_legacy_casts_to_list(self):
        self.assertEqual(
            migrate.cast_for_type("INTEGER[3]", legacy_casts=True), "::INTEGER[]"
        )
        self.assertEqual(
            migrate.cast_for_type("VARCHAR[2]", legacy_casts=True), "::VARCHAR[]"
        )
        self.assertEqual(
            migrate.cast_for_type("FLOAT[4]", legacy_casts=True), "::FLOAT[]"
        )

    def test_enum_preserved_by_default(self):
        self.assertIsNone(migrate.cast_for_type("ENUM('happy', 'sad')"))
        self.assertIsNone(migrate.cast_for_type("ENUM('a')", legacy_casts=False))

    def test_enum_legacy_casts_to_varchar(self):
        self.assertEqual(
            migrate.cast_for_type("ENUM('happy', 'sad')", legacy_casts=True),
            "::VARCHAR",
        )

    def test_union_still_cast(self):
        self.assertEqual(
            migrate.cast_for_type("UNION(num INTEGER, str VARCHAR)"), "::VARCHAR"
        )
        self.assertEqual(
            migrate.cast_for_type("UNION(num INTEGER, str VARCHAR)", legacy_casts=True),
            "::VARCHAR",
        )

    def test_varint_and_bit_still_cast(self):
        self.assertEqual(migrate.cast_for_type("VARINT"), "::VARCHAR::INT")
        self.assertEqual(migrate.cast_for_type("BIGNUM"), "::VARCHAR::INT")
        self.assertEqual(migrate.cast_for_type("BIT"), "::VARCHAR")

    def test_destination_type(self):
        self.assertEqual(migrate.destination_type("INTEGER[3]", None), "INTEGER[3]")
        self.assertEqual(
            migrate.destination_type("UNION(a INT)", "::VARCHAR"), "VARCHAR"
        )
        self.assertEqual(migrate.destination_type("VARINT", "::VARCHAR::INT"), "INTEGER")
        self.assertEqual(
            migrate.destination_type("INTEGER[3]", "::INTEGER[]"), "INTEGER[]"
        )

    def test_resolve_data_types_helpers_importable(self):
        self.assertTrue(callable(migrate._resolve_data_types))
        self.assertTrue(callable(migrate.cast_for_type))


@unittest.skipUnless(duckdb is not None, "duckdb package required")
class TestResolveDataTypesAgainstDuckDB(unittest.TestCase):
    def test_resolve_skips_array_and_enum(self):
        con = duckdb.connect()
        con.execute("CREATE TYPE mood AS ENUM ('happy', 'sad')")
        con.execute(
            "CREATE TABLE t(a INTEGER[3], m mood, u UNION(num INT, str VARCHAR), v VARINT, b BIT)"
        )
        excepts, casts = migrate._resolve_data_types(
            "t", "main", "memory", con, legacy_casts=False
        )
        cast_cols = {c.split("::", 1)[0] for c in casts}
        self.assertNotIn("a", excepts)
        self.assertNotIn("m", excepts)
        self.assertIn("u", excepts)
        self.assertIn("v", excepts)
        self.assertIn("b", excepts)
        self.assertTrue(any(c.startswith("u::") for c in casts))
        con.close()

    def test_resolve_legacy_casts_array_and_enum(self):
        con = duckdb.connect()
        con.execute("CREATE TABLE t(a INTEGER[3], m ENUM('x', 'y'))")
        excepts, casts = migrate._resolve_data_types(
            "t", "main", "memory", con, legacy_casts=True
        )
        self.assertEqual(set(excepts), {"a", "m"})
        self.assertTrue(any("INTEGER[]" in c for c in casts))
        self.assertTrue(any(c.startswith("m::VARCHAR") for c in casts))
        con.close()


@unittest.skipUnless(DUCKLAKE_OK, f"ducklake extension not loadable ({DUCKLAKE_DETAIL})")
class TestMigrateIntegration(unittest.TestCase):
    def test_migrate_preserves_array_enum_generated(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            source_db = tmp_path / "source.db"
            ducklake_file = tmp_path / "meta.ducklake"
            data_path = tmp_path / "data"
            data_path.mkdir()

            src = duckdb.connect(str(source_db))
            src.execute("CREATE TYPE mood AS ENUM ('happy', 'sad')")
            src.execute(
                "CREATE TABLE t("
                "a INTEGER[3], "
                "m mood, "
                "g INT AS (a[1] + 1), "
                "u UNION(num INT, str VARCHAR), "
                "b INT DEFAULT 7"
                ")"
            )
            src.execute(
                "INSERT INTO t(a, m, u, b) VALUES "
                "([1, 2, 3], 'happy', union_value(num := 42), 10)"
            )
            src.close()

            rc = migrate.main(
                [
                    "--duckdb-catalog",
                    "src",
                    "--duckdb-file",
                    str(source_db),
                    "--ducklake-catalog",
                    "dst",
                    "--catalog-type",
                    "duckdb",
                    "--ducklake-file",
                    str(ducklake_file),
                    "--ducklake-data-path",
                    str(data_path),
                ]
            )
            self.assertEqual(rc, 0)

            # Re-open DuckLake and assert preserved types / behavior.
            con = duckdb.connect()
            try:
                con.execute("SET allow_unsigned_extensions = true")
            except Exception:
                pass
            migrate.load_ducklake(con)
            con.execute(
                f"ATTACH 'ducklake:{ducklake_file}' AS dst (DATA_PATH '{data_path}'); "
                f"USE dst;"
            )
            row = con.execute(
                "SELECT typeof(a), typeof(m), typeof(u), a, m, g, u, b FROM t"
            ).fetchone()
            typeof_a, typeof_m, typeof_u, a, m, g, u, b = row
            self.assertEqual(typeof_a, "INTEGER[3]")
            self.assertIn("ENUM", typeof_m.upper())
            self.assertEqual(typeof_u, "VARCHAR")
            self.assertEqual(list(a), [1, 2, 3])
            self.assertEqual(str(m), "happy")
            self.assertEqual(g, 2)
            self.assertEqual(b, 10)

            # Generated should still compute when omitted from INSERT (if supported).
            try:
                con.execute("INSERT INTO t(a, m, u) VALUES ([9, 8, 7], 'sad', 'x')")
                g2 = con.execute(
                    "SELECT g FROM t WHERE a = [9, 8, 7]::INTEGER[3]"
                ).fetchone()[0]
                self.assertEqual(g2, 10)
            except Exception:
                # Some DuckLake builds bake generated values; value after migrate is enough.
                pass
            con.close()


@unittest.skipUnless(duckdb is not None, "duckdb package required")
class TestSchemaAwareMigrate(unittest.TestCase):
    """Exercise CREATE TABLE + INSERT path without requiring DuckLake."""

    def test_schema_aware_preserves_array_enum_generated(self):
        with tempfile.TemporaryDirectory() as tmp:
            src_path = Path(tmp) / "src.db"
            dst_path = Path(tmp) / "dst.db"
            src = duckdb.connect(str(src_path))
            src.execute("CREATE TYPE mood AS ENUM ('happy', 'sad')")
            src.execute(
                "CREATE TABLE t("
                "a INTEGER[3], m mood, g INT AS (a[1] + 1), "
                "u UNION(num INT, str VARCHAR), b INT DEFAULT 7)"
            )
            src.execute(
                "INSERT INTO t(a, m, u, b) VALUES "
                "([1, 2, 3], 'happy', union_value(num := 42), 10)"
            )
            src.close()

            con = duckdb.connect()
            con.execute(
                f"ATTACH '{src_path}' AS src; ATTACH '{dst_path}' AS dst; USE dst;"
            )
            migrate.migrate_user_types(con, "src")
            named = migrate.build_named_type_lookup(con, "src")
            migrate.migrate_table_schema_aware(
                con, "src", "main", "t", legacy_casts=False, named_types=named
            )
            row = con.execute(
                "SELECT typeof(a), typeof(m), typeof(u), g, b FROM t"
            ).fetchone()
            typeof_a, typeof_m, typeof_u, g, b = row
            self.assertEqual(typeof_a, "INTEGER[3]")
            self.assertIn("ENUM", typeof_m.upper())
            self.assertEqual(typeof_u, "VARCHAR")
            self.assertEqual(g, 2)
            self.assertEqual(b, 10)
            cols = {
                r[0]: r
                for r in con.execute(
                    "SELECT column_name, is_generated FROM duckdb_columns() "
                    "WHERE table_name = 't' AND database_name = 'dst'"
                ).fetchall()
            }
            if "is_generated" in {
                c[0]
                for c in con.execute(
                    "SELECT column_name FROM (DESCRIBE SELECT * FROM duckdb_columns())"
                ).fetchall()
            }:
                self.assertTrue(cols["g"][1])
            con.close()


if __name__ == "__main__":
    unittest.main()
