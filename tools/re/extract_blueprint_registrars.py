#!/usr/bin/env python3
"""Recover the blueprint reflection registrars from the retail executable.

Mechanism (read at `0x00527f30` and `0x00528160`, `C-198` / `C-183`): every blueprint
section has one function that registers its keys against a reflection/type-builder object
held in `ecx`.  Each key is one occurrence of

    push  $<offset within the section struct>     ; 6a imm8  or  68 imm32
    push  $<key name string VA>                   ; 68 imm32
    mov   %esi, %ecx                              ; optional, the builder object
    call  <per-type register function>            ; returns the descriptor in eax
    mov   %edi, 0xc(%eax)                         ; a small constant, same for every key
    mov   $<doc string VA>, 0x10(%eax)            ; the engine's own one-line description

The descriptor writes for key *N* are emitted after the call that created it and before the
call that creates key *N+1*, so the doc string binds backwards to the previous call.  The
call target is the only thing that encodes the member's TYPE, so the set of distinct targets
is the type table and is reported separately.

Two-phase by design.  Phase A is a raw byte scan with no notion of instruction boundaries,
so it cannot silently skip anything; it only proposes regions.  Phase B re-reads each region
through `objdump` and parses every line against a closed grammar, counting -- and printing --
every line it does not recognise, so a parser gap shows up as a number rather than as a
clean negative (evidence trap 3).

Read-only: opens the executable and nothing else.
"""

import argparse
import re
import subprocess
import sys
from collections import defaultdict
from dataclasses import dataclass, field

from pe_reader import PE32

KEY_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")

# objdump line: "  527f35: 6a 00 \t pushl $0x0"
LINE_RE = re.compile(r"^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*\t(.*)$")


@dataclass
class Key:
    name: str
    name_va: int
    offset: int | None
    call_target: int | None
    doc: str | None
    doc_va: int | None
    flag: int | None
    site_va: int


@dataclass
class Region:
    start: int
    end: int
    keys: list = field(default_factory=list)
    unparsed: list = field(default_factory=list)


def plausible_key(pe: PE32, va: int) -> str | None:
    """A key name is a short printable identifier living in .rdata."""
    section = pe.section_of(va)
    if section is None or section.name != ".rdata":
        return None
    text = pe.identifier(va, 64)
    if not text or not (2 <= len(text) <= 48):
        return None
    if not KEY_RE.match(text):
        return None
    return text


def scan_candidates(pe: PE32) -> list[int]:
    """Phase A: every `push $imm32` in .text whose operand is a plausible key name."""
    text = next(s for s in pe.sections if s.name == ".text")
    blob = pe.data[text.raw_offset:text.raw_offset + text.raw_size]
    base = text.virtual_address
    hits: list[int] = []
    start = 0
    while True:
        i = blob.find(b"\x68", start)
        if i < 0 or i + 5 > len(blob):
            break
        start = i + 1
        va = int.from_bytes(blob[i + 1:i + 5], "little")
        if plausible_key(pe, va):
            hits.append(base + i)
    return hits


def cluster(hits: list[int], gap: int = 96) -> list[tuple[int, int]]:
    """Consecutive push sites closer than `gap` bytes belong to one registrar body."""
    runs: list[tuple[int, int]] = []
    run_start = run_end = None
    for va in hits:
        if run_start is None:
            run_start = run_end = va
        elif va - run_end <= gap:
            run_end = va
        else:
            runs.append((run_start, run_end))
            run_start = run_end = va
    if run_start is not None:
        runs.append((run_start, run_end))
    return runs


def disassemble(exe: str, start: int, end: int) -> list[tuple[int, str]]:
    out = subprocess.run(
        ["objdump", "-d", f"--start-address={start:#x}", f"--stop-address={end:#x}", exe],
        capture_output=True, text=True, check=True).stdout
    rows: list[tuple[int, str]] = []
    for line in out.splitlines():
        m = LINE_RE.match(line)
        if m:
            rows.append((int(m.group(1), 16), m.group(3).split("#")[0].strip()))
    return rows


IMM = r"\$(?:0x)?(-?[0-9a-fx]+)"


def imm(token: str) -> int:
    token = token.lstrip("$")
    negative = token.startswith("-")
    token = token.lstrip("-")
    value = int(token, 16) if token.startswith("0x") else int(token, 16)
    return -value if negative else value


def parse_region(pe: PE32, rows: list[tuple[int, str]], region: Region) -> None:
    """Phase B: a closed grammar over the registrar body.

    State: `regs` holds constants materialised into registers (the compiler hoists a doc
    string or the flag constant into ebx/edi when several keys share it); `pending` is the
    (offset, name) pair pushed but not yet consumed by a call; `current` is the key whose
    descriptor is live in eax and therefore the target of the next `0xc`/`0x10` write.
    """
    regs: dict[str, int] = {}
    pushes: list[tuple[int, int]] = []      # (va, immediate)
    current: Key | None = None

    for va, text in rows:
        # --- pushes -------------------------------------------------------------
        m = re.match(r"^pushl\s+\$(0x[0-9a-f]+|-?\d+)$", text)
        if m:
            pushes.append((va, int(m.group(1), 0)))
            continue
        # --- constant into a register -------------------------------------------
        m = re.match(r"^movl\s+\$(0x[0-9a-f]+),\s+%(e[a-z]{2})$", text)
        if m:
            regs[m.group(2)] = int(m.group(1), 16)
            continue
        # --- register to register (the builder object into ecx) ------------------
        if re.match(r"^movl\s+%e[a-z]{2},\s+%e[a-z]{2}$", text):
            m = re.match(r"^movl\s+%(e[a-z]{2}),\s+%(e[a-z]{2})$", text)
            src, dst = m.group(1), m.group(2)
            if src in regs:
                regs[dst] = regs[src]
            else:
                regs.pop(dst, None)
            continue
        # --- descriptor field writes: mov <imm|reg>, 0xc|0x10(%eax) --------------
        m = re.match(r"^movl\s+\$(0x[0-9a-f]+),\s+(0x[0-9a-f]+)\(%eax\)$", text)
        if m:
            value, slot = int(m.group(1), 16), int(m.group(2), 16)
            if current is not None:
                if slot == 0x10:
                    current.doc_va = value
                    current.doc = pe.cstring(value, 400)
                elif slot == 0xc:
                    current.flag = value
            continue
        m = re.match(r"^movl\s+%(e[a-z]{2}),\s+(0x[0-9a-f]+)\(%eax\)$", text)
        if m:
            reg, slot = m.group(1), int(m.group(2), 16)
            value = regs.get(reg)
            if current is not None and value is not None:
                if slot == 0x10:
                    current.doc_va = value
                    current.doc = pe.cstring(value, 400)
                elif slot == 0xc:
                    current.flag = value
            continue
        # --- the registration call ----------------------------------------------
        m = re.match(r"^calll?\s+(0x[0-9a-f]+)", text)
        if m:
            target = int(m.group(1), 16)
            name_va = offset = None
            if pushes:
                name_va = pushes[-1][1]
                if len(pushes) >= 2:
                    offset = pushes[-2][1]
            name = plausible_key(pe, name_va) if name_va else None
            if name is not None:
                current = Key(name=name, name_va=name_va, offset=offset,
                              call_target=target, doc=None, doc_va=None,
                              flag=None, site_va=pushes[-1][0])
                region.keys.append(current)
            else:
                current = None
            pushes.clear()
            continue
        # --- benign / ignorable --------------------------------------------------
        if re.match(r"^(pushl|popl)\s+%e[a-z]{2}$", text):
            continue
        if text in ("retl", "ret", "nop", "leave") or text.startswith("int3"):
            continue
        if re.match(r"^(addl|subl)\s+\$0x[0-9a-f]+,\s+%esp$", text):
            continue
        if re.match(r"^(movl|leal)\s+.*%e(bp|sp)", text):
            continue
        if re.match(r"^xorl\s+%(e[a-z]{2}),\s+%\1$", text):
            regs[re.match(r"^xorl\s+%(e[a-z]{2}),", text).group(1)] = 0
            continue
        region.unparsed.append((va, text))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--min-keys", type=int, default=3,
                    help="a registrar body must register at least this many keys")
    args = ap.parse_args()

    pe = PE32(args.exe)
    hits = scan_candidates(pe)
    print(f"phase A: {len(hits)} `push $<identifier>` sites in .text", file=sys.stderr)

    runs = cluster(hits)
    print(f"phase A: {len(runs)} clusters", file=sys.stderr)

    regions: list[Region] = []
    unparsed_total = 0
    for start, end in runs:
        region = Region(start=start - 32, end=end + 48)
        rows = disassemble(args.exe, region.start, region.end)
        parse_region(pe, rows, region)
        unparsed_total += len(region.unparsed)
        if len(region.keys) >= args.min_keys:
            regions.append(region)

    print(f"phase B: {len(regions)} registrar bodies, "
          f"{sum(len(r.keys) for r in regions)} keys, "
          f"{unparsed_total} unrecognised instruction lines", file=sys.stderr)

    types: dict[int, int] = defaultdict(int)
    with open(args.out, "w") as handle:
        handle.write("registrar_va\tkey\tstruct_offset\tcall_target\tflag\t"
                     "key_va\tdoc_va\tdoc\tsite_va\n")
        for region in regions:
            reg_va = min(k.site_va for k in region.keys)
            for key in region.keys:
                types[key.call_target] += 1
                handle.write("\t".join([
                    f"{reg_va:08x}", key.name,
                    "" if key.offset is None else f"0x{key.offset:x}",
                    f"{key.call_target:08x}",
                    "" if key.flag is None else str(key.flag),
                    f"{key.name_va:08x}",
                    "" if key.doc_va is None else f"{key.doc_va:08x}",
                    (key.doc or "").replace("\t", " ").replace("\n", " "),
                    f"{key.site_va:08x}",
                ]) + "\n")

    print("call targets (candidate type table):", file=sys.stderr)
    for target, count in sorted(types.items(), key=lambda kv: -kv[1]):
        print(f"  {target:08x}  {count}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
