#!/usr/bin/env python3
"""Score ``serializer_members.tsv`` against offsets established by unrelated means.

The serializer field lists are only worth anything if they agree with facts recovered
from Lua accessors, vtable dispatch and hand disassembly.  This checks each recorded
offset against the recovered record and reports one of:

  EXACT       the offset is a member boundary in that class's own field list
  NESTED      the offset lands on a member boundary of an embedded sub-object whose
              own serializer is also in the table (e.g. ``Entity+0x90`` ==
              ``Entity+0x78`` [``SSTIEntityVariableData``] ``+0x18``)
  INSIDE      the offset falls strictly inside a serialised member whose sub-layout is
              not separately recovered (agreement, but weaker)
  ABSENT      the class is in the table and the offset is not covered at all -- either
              the field is not saved, or the sweep missed it.  Reported, never hidden.
  NO-CLASS    the class has no serializer at all

A NESTED resolution is only attempted for members whose type is a *value* sub-object
(mangled ``.?AU``/``.?AV`` with no trailing ``*``) that has its own serializer.

A member's span is its own type's width, NOT the distance to the next sibling: the gap
between two serialised members is a genuine gap (an unsaved field), and calling an
offset that lands in it "inside the previous member" would manufacture agreement.
Nested-class widths are lower bounds, so an ABSENT verdict is "no serialised member is
known to cover this", not a proof that nothing does.

Usage:  python3 tools/re/verify_serializer_members.py
"""

from __future__ import annotations

import sys
from dataclasses import dataclass
from pathlib import Path

TSV = "build/re-fa/exports/serializer_members.tsv"
RTTI_BASES = "build/re-fa/exports/rtti.bases.tsv"

# Byte width of each primitive the extractor can name. Used to bound a member's span,
# so that "offset 0x8f8 is inside the byte at 0x8e6" cannot be reported as agreement.
PRIMITIVE_WIDTH = {"u8/bool": 1, "u8": 1, "i8": 1, "u16": 2, "i16": 2,
                   "u32/i32": 4, "float": 4, "double": 8, "u64": 8, "ptr": 4}

# (base class, offset, what it is, evidence claim) -- every one established WITHOUT
# reading a serializer.  Sourced from docs/fa-exe-analysis-plan.md's offset table.
KNOWN: list[tuple[str, int, str, str]] = [
    ("Moho::Entity", 0x4C, "spatial-grid / CollisionDBRect", "C-064, C-137"),
    ("Moho::Entity", 0x68, "entity id", "C-057"),
    ("Moho::Entity", 0x6C, "blueprint pointer", "C-148"),
    ("Moho::Entity", 0x90, "health", "C-053"),
    ("Moho::Entity", 0x94, "max health", "C-053"),
    ("Moho::Entity", 0x99, "dead/destroyed flag", "C-060"),
    ("Moho::Entity", 0x9C, "orientation quaternion", "C-133"),
    ("Moho::Entity", 0xAC, "position float3", "C-057, C-060, C-133"),
    ("Moho::Entity", 0xD8, "fraction complete", "C-098, C-133"),
    ("Moho::Entity", 0x148, "owning Sim*", "C-060"),
    ("Moho::Entity", 0x14C, "owning army", "C-068, C-132"),
    ("Moho::Entity", 0x178, "collision shape ptr", "C-066"),
    ("Moho::Entity", 0x1B9, "queued-for-destruction flag", "C-046"),
    ("Moho::Entity", 0x1DC, "visibility: focus player", "C-133"),
    ("Moho::Entity", 0x1E0, "visibility: allies", "C-133"),
    ("Moho::Entity", 0x1E4, "visibility: enemies", "C-133"),
    ("Moho::Entity", 0x1E8, "visibility: neutrals", "C-133"),
    ("Moho::Unit", 0x08, "Entity subobject", "C-053"),
    ("Moho::Unit", 0x120, "current layer", "C-128"),
    ("Moho::Unit", 0x294, "fuel ratio", "C-131"),
    ("Moho::Unit", 0x298, "shield ratio", "C-131"),
    ("Moho::Unit", 0x2AC, "generic work progress", "C-081, C-132"),
    ("Moho::Unit", 0x2E8, "produced this tick E", "C-068"),
    ("Moho::Unit", 0x2F0, "consumed this tick E", "C-068"),
    ("Moho::Unit", 0x470, "consumption/sec E", "C-068"),
    ("Moho::Unit", 0x478, "production/sec E", "C-068"),
    ("Moho::Unit", 0x498, "script-bit mask", "C-128"),
    ("Moho::Unit", 0x4A0, "64-bit unit-state mask", "C-081, C-123, C-131"),
    ("Moho::Unit", 0x4B4, "command queue", "C-131"),
    ("Moho::Unit", 0x52C, "CEconStorage*", "C-068"),
    ("Moho::Unit", 0x534, "CEconRequest*", "C-068"),
    ("Moho::Unit", 0x538, "consumption active", "C-068"),
    ("Moho::Unit", 0x539, "production active", "C-068"),
    ("Moho::Unit", 0x53C, "resource-consumed ratio", "C-071"),
    ("Moho::Unit", 0x544, "IAiAttacker*", "C-081, C-132"),
    ("Moho::Unit", 0x558, "IAiSiloBuild*", "C-081"),
    ("Moho::Unit", 0x568, "armour multiplier map", "C-060"),
    ("Moho::CEconomy", 0x18, "stored [E,M]", "C-068"),
    ("Moho::CEconomy", 0x20, "income [E,M]", "C-068"),
    ("Moho::CEconomy", 0x30, "requested [E,M]", "C-068"),
    ("Moho::CEconomy", 0x38, "usage [E,M]", "C-068"),
    ("Moho::CEconomy", 0x40, "max storage (uint64)", "C-069"),
    ("Moho::CEconomy", 0x58, "CEconRequest list head", "C-068"),
    ("Moho::CEconRequest", 0x08, "demand[2]", "C-068"),
    ("Moho::CEconRequest", 0x10, "allocated[2]", "C-068"),
    ("Moho::Sim", 0x8F8, "beat", "C-055"),
    ("Moho::Sim", 0x900, "tick", "C-055"),
    ("Moho::Sim", 0x908, "COGrid*", "C-137"),
    ("Moho::Sim", 0xA3C, "world shield list", "C-144"),
    ("Moho::CRandomStream", 0x000, "stream state base", "C-056"),
    ("Moho::CRandomStream", 0x9C0, "stream cursor", "C-056"),
    ("Moho::Projectile", 0x2C8, "damage", "C-129"),
    ("Moho::Projectile", 0x330, "absolute expiry tick", "C-129"),
    ("Moho::UnitWeapon", 0x60, "min radius", "C-167"),
    ("Moho::UnitWeapon", 0x64, "max radius", "C-167"),
    ("Moho::UnitWeapon", 0x68, "min radius^2 (lazy cache)", "C-167"),
    ("Moho::UnitWeapon", 0x6C, "max radius^2 (lazy cache)", "C-167"),
    ("Moho::UnitWeapon", 0x70, "max height difference", "C-129, C-167"),
    ("Moho::UnitWeapon", 0x90, "damage radius", "C-129"),
    ("Moho::UnitWeapon", 0x94, "damage", "C-129"),
    ("Moho::UnitWeapon", 0xA0, "owning Unit*", "C-129"),
    ("Moho::UnitWeapon", 0xD0, "current target", "C-129"),
    ("Moho::UnitWeapon", 0x150, "target-priority vector", "C-157"),
]


@dataclass
class Member:
    index: int
    offset: int
    type_name: str
    access: str
    via: str


def load() -> dict[str, list[Member]]:
    table: dict[str, list[Member]] = {}
    with open(TSV, encoding="utf-8") as handle:
        header = handle.readline().rstrip("\n").split("\t")
        col = {name: i for i, name in enumerate(header)}
        for line in handle:
            parts = line.rstrip("\n").split("\t")
            table.setdefault(parts[col["class"]], []).append(Member(
                int(parts[col["member_index"]]), int(parts[col["offset"]], 16),
                parts[col["type"]], parts[col["access"]], parts[col["via"]]))
    for members in table.values():
        members.sort(key=lambda m: m.offset)
    return table


def demangle_value_type(mangled: str) -> str | None:
    """``.?AUSSTIEntityVariableData@Moho@@`` -> ``Moho::SSTIEntityVariableData``.

    Returns None for pointers and for anything not a plain class/struct, so nesting is
    never attempted through an indirection.
    """
    if not mangled.startswith((".?AU", ".?AV")) or mangled.endswith("*"):
        return None
    body = mangled[4:]
    if body.endswith("@@"):
        body = body[:-2]
    if "?$" in body or "@" not in body + "@":
        return None
    parts = [p for p in body.split("@") if p]
    return "::".join(reversed(parts)) if parts else None


def load_bases() -> dict[str, dict[int, list[str]]]:
    """RTTI base-class offsets per class -- an independent layout oracle.

    Used only to *explain* a miss: an offset that coincides with a base subobject the
    serializer never writes is an unserialised base, not a failure of the sweep.
    """
    out: dict[str, dict[int, list[str]]] = {}
    try:
        handle = open(RTTI_BASES, encoding="utf-8")
    except OSError:
        return out
    with handle:
        header = handle.readline().rstrip("\n").split("\t")
        col = {name: i for i, name in enumerate(header)}
        for line in handle:
            parts = line.rstrip("\n").split("\t")
            if len(parts) <= col["base_offset"]:
                continue
            out.setdefault(parts[col["class_name"]], {}).setdefault(
                int(parts[col["base_offset"]], 16), []).append(parts[col["base_name"]])
    return out


def width(table: dict[str, list[Member]], m: Member, seen: frozenset[str]) -> int:
    """Byte span of one member -- a LOWER bound when the type is a nested class."""
    if m.type_name in PRIMITIVE_WIDTH:
        return PRIMITIVE_WIDTH[m.type_name]
    if m.type_name.endswith("*"):
        return 4
    inner = demangle_value_type(m.type_name)
    if inner and inner in table and inner not in seen:
        return extent(table, inner, seen | {inner})
    return 4          # unknown aggregate: at least one dword


def extent(table: dict[str, list[Member]], cls: str, seen: frozenset[str] = frozenset()
           ) -> int:
    """Lower bound on ``sizeof(cls)`` from its serialised members alone."""
    return max((m.offset + width(table, m, seen | {cls}) for m in table.get(cls, [])),
               default=0)


def covering(table: dict[str, list[Member]], members: list[Member],
             offset: int) -> Member | None:
    """The member whose *span* contains ``offset``.

    The span is bounded by the member's own type width, never merely by the distance
    to the next sibling: a gap between two serialised members is a genuine gap, and
    reporting an offset that lands in it as "inside the previous member" would
    manufacture agreement that does not exist.
    """
    for m in members:
        if m.offset > offset:
            break
        if m.offset <= offset < m.offset + width(table, m, frozenset()):
            return m
    return None


def resolve(table: dict[str, list[Member]], bases: dict[str, dict[int, list[str]]],
            cls: str, offset: int, depth: int = 0) -> tuple[str, str]:
    if cls not in table:
        return "NO-CLASS", f"{cls} has no serializer"
    members = table[cls]
    for m in members:
        if m.offset == offset:
            kind = "EXACT" if depth == 0 else "NESTED"
            return kind, f"{cls}+{offset:#x} = member[{m.index}] {m.type_name or m.access}"
    m = covering(table, members, offset)
    if m is not None:
        inner = demangle_value_type(m.type_name)
        if inner and inner in table and depth < 6:
            kind, why = resolve(table, bases, inner, offset - m.offset, depth + 1)
            if kind in ("EXACT", "NESTED"):
                return "NESTED", (f"{cls}+{m.offset:#x} [{inner}] "
                                  f"+{offset - m.offset:#x} -> {why}")
            if kind in ("INSIDE", "ABSENT"):
                return kind, (f"{cls}+{m.offset:#x} [{inner}] "
                              f"+{offset - m.offset:#x} -> {why}")
        return "INSIDE", (f"inside {cls}+{m.offset:#x} [{m.type_name or m.access}], "
                          f"{offset - m.offset:#x} in")
    base_hit = bases.get(cls, {}).get(offset)
    if base_hit:
        return "ABSENT", (f"{cls}+{offset:#x} is an RTTI base subobject "
                          f"({', '.join(base_hit)}) that the serializer never writes")
    return "ABSENT", (f"{cls}+{offset:#x} covered by no serialised member "
                      f"(extents are lower bounds)")


def main() -> int:
    if not Path(TSV).exists():
        print(f"missing {TSV}; run extract_serializer_members.py first", file=sys.stderr)
        return 1
    table = load()
    bases = load_bases()
    tally: dict[str, int] = {}
    for cls, offset, what, evidence in KNOWN:
        kind, why = resolve(table, bases, cls, offset)
        tally[kind] = tally.get(kind, 0) + 1
        print(f"{kind:9s} {cls}+{offset:#05x} {what:34s} [{evidence}]\n"
              f"          {why}")
    total = len(KNOWN)
    agree = tally.get("EXACT", 0) + tally.get("NESTED", 0)
    print(f"\n--- {total} independently established offsets")
    for kind in ("EXACT", "NESTED", "INSIDE", "ABSENT", "NO-CLASS"):
        if kind in tally:
            print(f"  {kind:9s} {tally[kind]:3d}  ({100.0 * tally[kind] / total:.1f}%)")
    print(f"  member-boundary agreement (EXACT+NESTED): {agree}/{total} "
          f"= {100.0 * agree / total:.1f}%")
    covered = agree + tally.get("INSIDE", 0)
    print(f"  no contradiction (incl. INSIDE)         : {covered}/{total} "
          f"= {100.0 * covered / total:.1f}%")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
