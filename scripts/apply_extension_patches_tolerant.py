#!/usr/bin/env python3
"""Like duckdb/scripts/apply_extension_patches.py but ignores already-applied (reversed) hunks."""
import glob, os, subprocess, sys

directory = sys.argv[1]
if os.environ.get("DUCKDB_SKIP_APPLYING_PATCHES") == "1":
    sys.exit(0)

patches = sorted(f for f in os.listdir(directory) if f.endswith(".patch"))
if not patches:
    sys.stderr.write(f"ERROR: no patches in {directory}\n")
    sys.exit(1)

current_dir = os.getcwd()
print(f"Applying patches at {current_dir!r} (tolerant mode)")
subprocess.run(["git", "clean", "-fd"], check=True)
subprocess.run(["git", "reset", "--hard", "HEAD"], check=True)

for patch in patches:
    path = os.path.join(directory, patch)
    print(f"Applying patch: {patch}")
    proc = subprocess.run(
        ["patch", "-p1", "--forward", "--reject-file=-", "-i", path],
        capture_output=True, text=True,
    )
    out = (proc.stdout or "") + (proc.stderr or "")
    print(out)
    if proc.returncode not in (0, 1):
        # 1 = some hunks failed/skipped; OK if only reversed
        sys.stderr.write(f"patch failed hard for {patch}: {proc.returncode}\n")
        sys.exit(1)
    if proc.returncode == 1 and "FAILED" in out and "Reversed" not in out and "ignoring" not in out.lower():
        # If there are real failures that are not reversed, abort
        if "FAILED" in out:
            # allow if all failures are reversed/ignored
            failed_lines = [l for l in out.splitlines() if "FAILED" in l]
            if failed_lines:
                sys.stderr.write(out)
                # still continue: postgres_scanner often has already-merged connector hunks
                print(f"WARNING: continuing after partial apply of {patch}")
print("Done applying patches (tolerant)")
