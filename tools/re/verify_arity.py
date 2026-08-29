#!/usr/bin/env python3
"""Check every recovered Moho method's declared signature against its real arity check.

WHY. `extract_moho_methods.py` recovers each Lua callable's signature from a string the
engine ships. That string is *documentation*, and documentation can be wrong: the global
`Damage` is declared `Damage(instigator, target, amount, damageType)` — four parameters —
but its native implementation demands five, and the one retail script that calls it passes
five (`Damage(self, {0,0,0}, TargetEntity, self.Data, 'Normal')`). The declared signature had
silently lost the `location` argument.

The engine, however, checks arity at run time and says so:

    call  <lua_gettop-alike at 0x00977ce0>
    cmp   eax, <n>                          ; one cmp per accepted count
    ...   "%s\\n  expected %d args, but got %d"

So the accepted argument counts are recoverable from the code, and comparing them against the
parameter list in the doc string turns 797 unverified rows into machine-checked ones — and
surfaces exactly the rows where the shipped documentation lies.

Counting rules, which is where the care is needed:
  * a method's `self` IS an argument at this level; `GetHealth(self)` checks for 1
  * `[bracketed]` parameters are optional, so a range of counts is accepted
  * a `<global>` has no implicit receiver

Read-only.
"""

import argparse
import os
import re
import struct
import sys
from collections import Counter

from pe_reader import PE32

# The argument-count helper every binding calls before validating arity. Identified from
# Unit::GetHealth (0x006cb7a0) and confirmed at Damage, DamageArea and Entity::Kill.
ARGCOUNT_HELPER = 0x00977CE0

# How far past the helper call an arity comparison can sit. The compares follow almost
# immediately; this is slack, not a search.
COMPARE_WINDOW = 0x40

# How far into a method to look for the helper call in the first place.
PROLOGUE_WINDOW = 0x120

CALL = 0xE8
CMP_EAX_IMM8 = b"\x83\xf8"
CMP_EAX_IMM32 = b"\x3d"


def accepted_arities(pe: PE32, method_va: int):
    """The argument counts a method's prologue compares against, or None if not found."""
    body = pe.read(method_va, PROLOGUE_WINDOW)
    if body is None:
        return None

    call_end = None
    for i in range(len(body) - 5):
        if body[i] != CALL:
            continue
        rel = struct.unpack_from("<i", body, i + 1)[0]
        if method_va + i + 5 + rel == ARGCOUNT_HELPER:
            call_end = i + 5
            break
    if call_end is None:
        return None

    counts = set()
    window = body[call_end:call_end + COMPARE_WINDOW]
    for i in range(len(window) - 2):
        if window[i:i + 2] == CMP_EAX_IMM8:
            counts.add(window[i + 2])
        elif window[i:i + 1] == CMP_EAX_IMM32 and i + 5 <= len(window):
            value = struct.unpack_from("<I", window, i + 1)[0]
            if value < 64:  # an arity, not an unrelated constant
                counts.add(value)
    return counts or None


def declared_arity(scope: str, signature: str):
    """The argument counts the doc string can be read as declaring, or None.

    Returns a SET, not a single number, because the doc strings are inconsistent about the
    receiver: some spell it `self`, some name it after the class (`IsUnitState(unit,
    stateName)`, `brain:BuildPlatoon()`), and some omit it. Assuming one convention and
    adding a receiver produced 21 "disagreements" that were nothing of the sort — the parser
    was wrong, not the engine. Where the convention cannot be determined, both readings are
    admitted, so a reported mismatch is a real one.
    """
    match = re.search(r"\(([^)]*)\)", signature)
    if match is None:
        return None
    inside = match.group(1).strip()
    params = [p.strip() for p in inside.split(",") if p.strip()] if inside else []

    optional = sum(1 for p in params if "[" in p or "]" in p or "=" in p)
    stated = len(params)

    counts = set()
    for total in range(stated - optional, stated + 1):
        counts.add(total)
        # A class method may or may not have listed its own receiver; admit both readings.
        if scope != "<global>":
            counts.add(total + 1)
    return counts


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--exe",
        default=os.path.expanduser(
            "~/projects/llm/input/recoil-metal/retail-fa/bin/SupremeCommander.exe"))
    parser.add_argument("--tsv", default="build/re-fa/exports/moho.methods.tsv")
    parser.add_argument("--mismatches", action="store_true",
                        help="list rows where the doc string and the code disagree")
    args = parser.parse_args()

    pe = PE32(args.exe)
    rows = [l.rstrip("\n").split("\t") for l in open(args.tsv)][1:]

    stats = Counter()
    mismatches = []
    for row in rows:
        scope, name, signature, method_va = row[0], row[1], row[2], row[3]
        if method_va == "-":
            stats["no real method address"] += 1
            continue
        accepted = accepted_arities(pe, int(method_va, 16))
        if accepted is None:
            stats["no arity check found"] += 1
            continue
        declared = declared_arity(scope, signature)
        if declared is None:
            stats["signature not a parameter list"] += 1
            continue
        # Agreement means any admissible reading of the doc string matches any count the
        # code accepts. Deliberately generous: the point is to find doc strings that are
        # provably wrong, not to grade the engine's prose style.
        if accepted & declared:
            stats["agrees"] += 1
        else:
            stats["DISAGREES"] += 1
            mismatches.append((scope, name, signature, sorted(accepted), sorted(declared)))

    total = sum(stats.values())
    print(f"# rows: {total}", file=sys.stderr)
    for key, count in stats.most_common():
        print(f"#   {key:28s} {count:5d}  ({count / total:.1%})", file=sys.stderr)

    if args.mismatches:
        print("scope\tname\tdeclared_signature\tcode_accepts\tdoc_implies")
        for scope, name, signature, accepted, declared in mismatches:
            print(f"{scope}\t{name}\t{signature}\t{accepted}\t{declared}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
