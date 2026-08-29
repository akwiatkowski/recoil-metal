#!/usr/bin/env python3
"""Diff the vtables of every class that shares a base, slot by slot.

WHY THIS IS THE USEFUL PART. A single vtable tells you which functions a class dispatches
through. A *family* of vtables tells you something better: which slots each sibling
actually bothered to override. In a hierarchy like

    CTask -> CCommandTask -> {CUnitRepairTask, CUnitCaptureTask, CUnitReclaimTask, ...}

the slots where every sibling holds the same address are inherited plumbing and can be
ignored. The slots where the siblings all differ are the class's behaviour, and they are
the only addresses worth spending a disassembly session on. Reading the matrix this
produces replaces an afternoon of "which of these is TaskTick?".

It also detects the opposite case, which is just as informative: two siblings sharing a
non-base address in a slot means they share an implementation the base does not have.

Input is `vtables.tsv` and `rtti.bases.tsv` from `extract_vtables.py`. Read-only; touches
no binary.

    python3 tools/re/diff_sibling_vtables.py --base Moho::CCommandTask
"""

import argparse
import collections
import os
import sys

DEFAULT_EXPORTS = "build/re-fa/exports"


def load_vtables(path: str):
    """{(class, subobject offset): [(slot index, target va, name)]} in slot order."""
    tables = collections.defaultdict(list)
    with open(path, encoding="latin-1") as handle:
        header = handle.readline().rstrip("\n").split("\t")
        cols = {name: i for i, name in enumerate(header)}
        for line in handle:
            f = line.rstrip("\n").split("\t")
            key = (f[cols["class_name"]], int(f[cols["subobject_offset"]], 16))
            tables[key].append((int(f[cols["slot_index"]]),
                                int(f[cols["slot_va"]], 16),
                                f[cols["target_func_name"]]))
    for key in tables:
        tables[key].sort()
    return tables


def load_bases(path: str):
    """{class: [(base class, byte offset of the base subobject, bases it contains)]}."""
    bases = collections.defaultdict(list)
    with open(path, encoding="latin-1") as handle:
        header = handle.readline().rstrip("\n").split("\t")
        cols = {name: i for i, name in enumerate(header)}
        for line in handle:
            f = line.rstrip("\n").split("\t")
            bases[f[cols["class_name"]]].append((
                f[cols["base_name"]],
                int(f[cols["base_offset"]], 16),
                int(f[cols["base_contained"]]),
            ))
    return bases


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--base", required=True,
                        help="demangled base class, e.g. Moho::CCommandTask")
    parser.add_argument("--exports", default=DEFAULT_EXPORTS)
    parser.add_argument("--tsv", help="also write the matrix here")
    args = parser.parse_args()

    tables = load_vtables(os.path.join(args.exports, "vtables.tsv"))
    bases = load_bases(os.path.join(args.exports, "rtti.bases.tsv"))

    # Every class that has `--base` somewhere in its base list, together with the byte
    # offset at which that base's subobject — and therefore its vptr — lives.
    family = []
    for klass, records in bases.items():
        for base_name, offset, _contained in records:
            if base_name == args.base and (klass, offset) in tables:
                family.append((klass, offset))
                break
    family.sort()
    if not family:
        print(f"no class derives from {args.base} with a vtable at the base's offset",
              file=sys.stderr)
        return 1

    base_table = tables.get((args.base, 0)) or []
    base_targets = [va for _, va, _ in base_table]
    width = max(len(tables[key]) for key in family)

    # Column headers are short so the matrix stays readable in a terminal.
    labels = [k[0].rsplit("::", 1)[-1] for k in family]
    print(f"# base {args.base}: {len(base_targets)} slots"
          f"; {len(family)} derived vtables")
    for klass, offset in family:
        print(f"#   {klass} @ +0x{offset:x}  ({len(tables[(klass, offset)])} slots)")
    print()
    header = ["slot", "base"] + labels
    rows = []
    for index in range(width):
        base_va = base_targets[index] if index < len(base_targets) else None
        cells = []
        seen = collections.Counter()
        for key in family:
            entry = dict((i, (va, name)) for i, va, name in tables[key]).get(index)
            if entry is None:
                cells.append("-")
                continue
            va, _name = entry
            seen[va] += 1
            cells.append("=" if base_va is not None and va == base_va else f"{va:08x}")
        # Any name the oracle attached to this slot, from whichever sibling has one.
        label = ""
        for key in family:
            for i, _va, name in tables[key]:
                if i == index and name:
                    label = name
                    break
            if label:
                break
        if not label and index < len(base_table):
            label = base_table[index][2]
        rows.append(([f"+0x{4 * index:x}",
                      f"{base_va:08x}" if base_va is not None else "-"] + cells, label))

    widths = [max(len(header[c]), max(len(r[0][c]) for r in rows))
              for c in range(len(header))]
    print("  ".join(h.ljust(w) for h, w in zip(header, widths)))
    for cells, label in rows:
        line = "  ".join(c.ljust(w) for c, w in zip(cells, widths))
        print(f"{line}  {label}" if label else line)

    if args.tsv:
        with open(args.tsv, "w") as out:
            print("\t".join(header + ["name"]), file=out)
            for cells, label in rows:
                print("\t".join(cells + [label]), file=out)
        print(f"\n# wrote {args.tsv}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
