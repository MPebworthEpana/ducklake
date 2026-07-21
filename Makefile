PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=ducklake
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Core extensions that we need for crucial testing
DEFAULT_TEST_EXTENSION_DEPS=
# For cloud testing we also need these extensions
FULL_TEST_EXTENSION_DEPS=httpfs

# Stabilize all tests in CI
ifdef CI
TEST_FLAGS:=--stabilize-tests
endif
# ~[.] excludes hidden/slow (.test_slow) tests
T ?= $(TEST_FLAGS) "~[.]test/*"

# Aws and Azure have vcpkg dependencies and therefore need vcpkg merging
ifeq (${BUILD_EXTENSION_TEST_DEPS}, full)
	USE_MERGED_VCPKG_MANIFEST:=1
endif

# Apply DuckDB grammar patch so AT (BRANCH/TAG => ...) parses. Idempotent.
.PHONY: apply_duckdb_at_patch
apply_duckdb_at_patch:
	@if [ -f patches/duckdb-at-branch-tag.patch ] && [ -d duckdb ]; then \
		if ! grep -q "'BRANCH' / 'TAG'" duckdb/src/parser/peg/grammar/statements/select.gram 2>/dev/null; then \
			echo "Applying patches/duckdb-at-branch-tag.patch"; \
			patch -p1 -d duckdb < patches/duckdb-at-branch-tag.patch; \
		fi; \
	fi

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# Ensure the AT BRANCH/TAG grammar patch is applied before builds that compile DuckDB.
debug release relassert reldebug: apply_duckdb_at_patch

unittest_relassert:
	python3 duckdb/scripts/ci/run_tests.py build/relassert/test/unittest $(T)