#!/usr/bin/env python3
"""Known-answer test for `mine_field_offsets.py`.

The sweep is only worth its output if it can re-derive the offsets this campaign already
won by hand, one function at a time.  Those hand-won offsets are recorded in
`docs/fa-exe-analysis-plan.md` under "Object layout"; the ones this sweep is *capable* of
reaching (i.e. reachable from a Lua accessor) are listed below with the claim that produced
them.

Anything the sweep is structurally unable to reach — `Moho::Sim` internals, `CRandomStream`,
`CEconomy`, which have no Lua accessors — is listed as `out_of_scope` and is not counted
against the hit rate, but is printed so the denominator stays honest.

Usage:
    python3 tools/re/verify_field_offsets.py build/re-fa/exports/field_offsets.tsv
"""

import csv
import sys

# (base class, offset, what the plan says it is, the claim id, expected type keyword)
KNOWN = [
    ("Moho::Entity", 0x68,  "entity id",                 "C-057", "int"),
    ("Moho::Entity", 0x90,  "health",                    "C-053", "float"),
    ("Moho::Entity", 0x94,  "max health",                "C-053", "float"),
    ("Moho::Entity", 0x148, "owning Sim*",               "C-060", "ptr"),
    ("Moho::Entity", 0x178, "collision shape",           "C-066", "ptr"),
    ("Moho::Unit",   0x98,  "health (= Entity+0x90)",    "C-053", "float"),
    ("Moho::Unit",   0x154, "owning army (CArmyImpl*)",  "C-068", "ptr"),
    ("Moho::Unit",   0x2AC, "silo build progress",       "C-081", "float"),
    ("Moho::Unit",   0x4A0, "64-bit unit state mask",    "C-081", "int"),
    ("Moho::Unit",   0x53C, "resource-consumed ratio",   "C-071", "float"),
    ("Moho::Unit",   0x544, "weapon container",          "C-081", "ptr"),
    ("Moho::Unit",   0x558, "IAiSiloBuild*",             "C-081", "ptr"),
]

# Recorded in the plan but not reachable from any Lua accessor, so the sweep cannot see them.
OUT_OF_SCOPE = [
    ("Moho::Entity", 0x4C, "spatial-grid record", "C-064"),
    ("Moho::Entity", 0x99, "dead flag", "C-060"),
    ("Moho::Entity", 0x1B9, "queued-for-destruction flag", "C-046"),
    ("Moho::Unit", 0x2E8, "produced this tick", "C-068"),
    ("Moho::Unit", 0x470, "consumption per second", "C-068"),
    ("Moho::Unit", 0x52C, "CEconStorage*", "C-068"),
    ("Moho::CEconomy", 0x18, "stored", "C-068"),
    ("Moho::Sim", 0x8F8, "beat number", "C-055"),
    ("CRandomStream", 0x9C0, "mti", "C-056"),
]


def main() -> int:
    path = sys.argv[1] if len(sys.argv) > 1 else "build/re-fa/exports/field_offsets.tsv"
    rows = list(csv.DictReader(open(path, encoding="utf-8"), delimiter="\t"))
    index: dict[tuple[str, int], list[dict]] = {}
    for row in rows:
        index.setdefault((row["base_class"], int(row["offset"], 16)), []).append(row)

    hits = 0
    print(f"{'result':7s} {'base+offset':24s} {'expected':28s} {'found as':22s} witness")
    for base, offset, meaning, claim, want_type in KNOWN:
        found = index.get((base, offset))
        if not found:
            print(f"{'MISS':7s} {base}+0x{offset:X}".ljust(32)
                  + f" {meaning:28s} {'-':22s} ({claim})")
            continue
        hits += 1
        best = sorted(found, key=lambda r: {"high": 0, "medium": 1, "low": 2}[r["confidence"]])[0]
        type_ok = "" if want_type in best["type"] or want_type == "int" and "int" in best["type"] \
            else "  [type differs]"
        print(f"{'HIT':7s} {base}+0x{offset:X}".ljust(32)
              + f" {meaning:28s} {best['field_meaning'][:21]:22s}"
              + f" {best['source_method_name']} ({best['confidence']}){type_ok}")

    print()
    print(f"KNOWN-ANSWER HIT RATE: {hits}/{len(KNOWN)} = {100.0 * hits / len(KNOWN):.0f}%")
    print()
    print("Recorded but structurally out of reach for a Lua-accessor sweep "
          "(no Lua getter exists):")
    for base, offset, meaning, claim in OUT_OF_SCOPE:
        seen = "seen anyway" if (base, offset) in index else "not seen (expected)"
        print(f"  {base + '+0x' + format(offset, 'X'):28s} {meaning:32s} {claim}  -> {seen}")
    return 0 if hits == len(KNOWN) else 1


if __name__ == "__main__":
    raise SystemExit(main())
