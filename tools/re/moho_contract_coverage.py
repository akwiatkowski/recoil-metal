#!/usr/bin/env python3
"""Moho contract coverage: join the recovered Lua API against our AI surface.

Supply: build/re-fa/exports/moho.methods.annotated.tsv (C-032, 1,182 callables).
Demand: src/app/FafApi.inc (RM_FAF_GLOBAL/METHOD/SHIM names + corpus call sites).
Fidelity: FafAi.cpp's fidelityFor lambda (Known/Guessed, default Stub).

Prints markdown. Regenerate after touching any input:
  mise exec -- python3 tools/re/moho_contract_coverage.py
"""
import csv
import re
import sys
from collections import defaultdict
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]

INC = ROOT / "src/app/FafApi.inc"
FAFAI = ROOT / "src/app/FafAi.cpp"
METHODS = ROOT / "build/re-fa/exports/moho.methods.annotated.tsv"


def parse_inc():
    entries = []
    pat = re.compile(r'RM_FAF_(GLOBAL|METHOD|SHIM)\("([^"]+)", (\d+)\)')
    for line in INC.read_text().splitlines():
        m = pat.search(line)
        if m:
            entries.append({"kind": m.group(1), "name": m.group(2), "sites": int(m.group(3))})
    return entries


def parse_fidelity():
    """Mirror fidelityFor: each `if (name == ...)` block's return decides."""
    src = FAFAI.read_text()
    start = src.index("const auto fidelityFor")
    end = src.index("};", start)
    body = src[start:end]
    fidelity = {}
    # Split into `if (...) { return Fidelity::X; }` blocks; every condition name
    # in the block shares its return.
    for block in re.finditer(r"if \(([^)]*)\)\s*\{\s*return Fidelity::(\w+);", body):
        cond, level = block.group(1), block.group(2)
        names = re.findall(r'name == "([^"]+)"', cond)
        assert names, f"fidelity block without names: {cond}"
        for name in names:
            fidelity[name] = level
    return fidelity  # absent names are Stub, exactly like the lambda's default


def parse_supply():
    rows = []
    with METHODS.open(newline="") as f:
        reader = csv.DictReader(f, delimiter="\t")
        for row in reader:
            rows.append(
                {"subsystem": row["subsystem"], "scope": row["scope"], "name": row["name"]}
            )
    return rows


def main():
    demand = parse_inc()
    fidelity = parse_fidelity()
    supply = parse_supply()

    by_global = defaultdict(list)  # name -> scopes (class matches)
    global_names = set()
    for row in supply:
        if row["scope"] == "<global>":
            global_names.add(row["name"])
        else:
            by_global[row["name"]].append(row["scope"] + "::" + row["name"])

    # Demand-side bookkeeping for the subsystem table.
    sub = defaultdict(lambda: {"recovered": 0, "demanded": 0, "known": 0, "sites": 0})
    for row in supply:
        sub[row["subsystem"]]["recovered"] += 1

    matched = demanded_sites = known_sites = 0
    missing = []  # (sites, name, kind, scopes-or-note)
    unmatched_demand = []
    for entry in demand:
        name, kind, sites = entry["name"], entry["kind"], entry["sites"]
        level = fidelity.get(name, "Stub")
        if kind == "GLOBAL":
            scopes = ["<global>::" + name] if name in global_names else []
        else:
            scopes = sorted(set(by_global.get(name, [])))
        if scopes:
            matched += 1
            demanded_sites += sites
            if level == "Known":
                known_sites += sites
            # Attribute demand to every matching scope's subsystem (ambiguous
            # names count everywhere they could bind; ambiguity is reported).
            for scoped in scopes:
                scope = scoped.split("::")[0]
                for row in supply:
                    if row["scope"] == scope and row["name"] == name:
                        cell = sub[row["subsystem"]]
                        cell["demanded"] += 1
                        cell["sites"] += sites
                        if level == "Known":
                            cell["known"] += 1
                        break
        else:
            unmatched_demand.append((sites, name, kind))
        if level != "Known":
            missing.append((sites, name, kind, "; ".join(scopes) if scopes else "—"))

    total_sites = sum(e["sites"] for e in demand)
    known_names = sum(1 for e in demand if fidelity.get(e["name"], "Stub") == "Known")
    guessed_names = sum(1 for e in demand if fidelity.get(e["name"], "Stub") == "Guessed")

    print(f"Supply: {len(supply)} recovered callables (C-032).")
    kinds = defaultdict(int)
    for e in demand:
        kinds[e["kind"]] += 1
    print(f"Demand: {len(demand)} names ({kinds['GLOBAL']} globals, "
          f"{kinds['METHOD']} methods, {kinds['SHIM']} shims), {total_sites} corpus sites.")
    print(f"Name match: {matched}/{len(demand)} demanded names exist in the recovery.")
    print(f"Fidelity: {known_names} Known, {guessed_names} Guessed, "
          f"{len(demand) - known_names - guessed_names} Stub.")
    print(f"Call-weighted: {known_sites}/{demanded_sites} matched sites are Known "
          f"({100.0 * known_sites / max(demanded_sites, 1):.1f}% of matched demand).")
    print()
    print("| Subsystem | Recovered | Demanded names | Known | Corpus sites |")
    print("|---|---:|---:|---:|---:|")
    for name, cell in sorted(sub.items(), key=lambda kv: -kv[1]["sites"]):
        if cell["demanded"] == 0 and cell["recovered"] < 10:
            continue
        print(f"| {name} | {cell['recovered']} | {cell['demanded']} | {cell['known']} "
              f"| {cell['sites']} |")
    print()
    print("Top stubbed demand by corpus sites (name, kind, retail scopes):")
    for sites, name, kind, scopes in sorted(missing, reverse=True)[:20]:
        print(f"| {sites} | {name} | {kind} | {scopes} |")
    print()
    print("Demanded but absent from the recovery (Lua builtins and own shims, expected):")
    for sites, name, kind in sorted(unmatched_demand, reverse=True):
        print(f"| {sites} | {name} | {kind} |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
