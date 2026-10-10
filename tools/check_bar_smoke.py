#!/usr/bin/env python3
"""Reject missing, stale, skipped or failed BAR gameplay smoke evidence."""

import argparse
import hashlib
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TEST = "recoil_metal/test_bar_runtime.lua"
# BAR's headless runner adds this synthetic test; the pinned upstream file
# explicitly disables it. That skip is not a substitute for our gameplay test.
UPSTREAM_DISABLED_TEST = "common/testing/infologtest.lua"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", type=Path, default=ROOT / "build/bar")
    args = parser.parse_args()
    try:
        receipt = json.loads((args.data_dir / "baseline.json").read_text())
        results = json.loads((args.data_dir / "testlog/results.json").read_text())
        if not receipt["smoke"] or receipt["baseline_lock_sha256"] != digest(ROOT / "tools/bar-runtime.lock.json"):
            raise ValueError("Result does not belong to the selected smoke baseline")
        if receipt["smoke_test_sha256"] != digest(ROOT / "src/games/bar/test_bar_runtime.lua"):
            raise ValueError("Result belongs to an older smoke scenario")
        rows = results["tests"]
        selected = [row for row in rows if row["file"].endswith(TEST)]
        pending = {row["fullTitle"] for row in results["pending"]}
        if len(selected) != 1 or selected[0]["err"] or selected[0]["fullTitle"] in pending:
            raise ValueError(f"Gameplay scenario is missing, failed or skipped: {results}")
        if results["stats"]["failures"] or results["stats"]["passes"] < 1:
            raise ValueError(f"BAR smoke did not pass: {results}")
        for row in rows:
            if row["err"] or (row["fullTitle"] in pending and row["file"] != UPSTREAM_DISABLED_TEST):
                raise ValueError(f"Unexpected failing/skipped scenario: {row}")
        infolog = (args.data_dir / "infolog.txt").read_text()
        for native_pass in ("terrain", "units"):
            if not re.search(rf"BAR-METAL {native_pass} triangles=[1-9][0-9]*", infolog):
                raise ValueError(f"No completed native Metal {native_pass} draw in this run")
        print(f"BAR smoke PASS: native Metal terrain/units, player MOVE/BUILD, SimpleAI construction, UI; evidence: {args.data_dir}")
        if pending:
            print("Upstream infolog-only check is disabled; no all-errors-clean claim.")
        return 0
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"BAR smoke FAIL: {error}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
